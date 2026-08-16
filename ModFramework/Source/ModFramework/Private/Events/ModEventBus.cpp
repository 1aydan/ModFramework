// Copyright (c) 2026. Licensed for use in your own projects.

#include "Events/ModEventBus.h"

#include "API/ModAPIRegistry.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "Events/ModEventTypes.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeExit.h"
#include "Templates/Casts.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"

namespace ModEventBusPrivate
{
	/**
	 * How deeply broadcasts may nest before the bus stops playing along.
	 *
	 * Nesting is a legitimate pattern - a handler reacting to one event by raising another - but a
	 * mod handler that re-raises the event it is handling would recurse until the stack ran out.
	 * Refusing past this depth turns a hard crash into a log line naming the offending event.
	 */
	static constexpr int32 MaxBroadcastDepth = 32;

	/** One row of the framework's built-in event table. */
	struct FBuiltinEventType
	{
		FName EventId;
		const TCHAR* Description = nullptr;
		bool bLifecyclePayload = false;
	};

	/**
	 * The framework's own event declarations.
	 *
	 * Built on first use rather than at static initialisation time so it can never observe the
	 * ModFrameworkEvents FName constants before they exist, whatever order the linker picks.
	 */
	static const TArray<FBuiltinEventType>& GetBuiltinEventTypes()
	{
		static const TArray<FBuiltinEventType> Builtins =
		{
			{ ModFrameworkEvents::ModDiscovered,   TEXT("A provider found a mod root and parsed its manifest."),                  true },
			{ ModFrameworkEvents::ModValidated,    TEXT("A mod manifest passed semantic validation."),                            true },
			{ ModFrameworkEvents::ModMounted,      TEXT("Every content root of a mod is mounted."),                               true },
			{ ModFrameworkEvents::ModLoaded,       TEXT("A mod's entry point exists and has been told the mod loaded."),          true },
			{ ModFrameworkEvents::ModActivated,    TEXT("A mod's extensions and content bundles are live."),                      true },
			{ ModFrameworkEvents::ModDeactivated,  TEXT("A mod's extensions and content bundles are no longer live."),            true },
			{ ModFrameworkEvents::ModUnloaded,     TEXT("A mod's entry point has been released and its objects freed."),          true },
			{ ModFrameworkEvents::ModUnmounted,    TEXT("A mod's content roots are no longer mounted."),                          true },
			{ ModFrameworkEvents::ModFailed,       TEXT("A mod entered the Failed state; the payload carries the reason."),       true },
			{ ModFrameworkEvents::ModsRefreshed,   TEXT("A full discover/resolve/mount/load pass finished."),                     true },
			{ ModFrameworkEvents::WorldCreated,    TEXT("A world the framework tracks came up."),                                 false },
			{ ModFrameworkEvents::WorldDestroyed,  TEXT("A world the framework tracks went away."),                               false },
			{ ModFrameworkEvents::GameStarted,     TEXT("The game instance finished initialising the framework."),                false }
		};

		return Builtins;
	}
}

UModEventBus::UModEventBus()
{
	RegisterBuiltinEventTypes();
}

void UModEventBus::RegisterBuiltinEventTypes()
{
	UScriptStruct* const LifecyclePayloadType = FModLifecycleEventPayload::StaticStruct();

	for (const ModEventBusPrivate::FBuiltinEventType& Builtin : ModEventBusPrivate::GetBuiltinEventTypes())
	{
		if (Builtin.EventId.IsNone())
		{
			// Defensive: an id that is still NAME_None would poison the map with an unusable entry.
			continue;
		}

		FModEventDescriptor Descriptor;
		Descriptor.EventId = Builtin.EventId;
		Descriptor.Description = Builtin.Description;
		Descriptor.PayloadType = Builtin.bLifecyclePayload ? LifecyclePayloadType : nullptr;
		Descriptor.bServerAuthoritative = false;

		EventTypes.Add(Builtin.EventId, MoveTemp(Descriptor));
	}
}

bool UModEventBus::IsFrameworkEventId(FName EventId)
{
	if (EventId.IsNone())
	{
		return false;
	}

	for (const ModEventBusPrivate::FBuiltinEventType& Builtin : ModEventBusPrivate::GetBuiltinEventTypes())
	{
		if (Builtin.EventId == EventId)
		{
			return true;
		}
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////
// Event types

bool UModEventBus::RegisterEventType(const FModEventDescriptor& InDescriptor, FModDiagnostic& OutError)
{
	if (InDescriptor.EventId.IsNone())
	{
		OutError = FModDiagnostic::Error(
			FName(TEXT("Events.InvalidEventId")),
			TEXT("An event descriptor must declare a non-empty EventId."));
		return false;
	}

	const FString EventIdString = InDescriptor.EventId.ToString();

	if (InDescriptor.PayloadType != nullptr && !InDescriptor.PayloadType->IsChildOf(FModEventPayload::StaticStruct()))
	{
		// A declared payload type that is not an FModEventPayload can never travel in a context, so
		// accepting it would only produce a check that always fails at broadcast time.
		OutError = FModDiagnostic::Error(
			FName(TEXT("Events.InvalidPayloadType")),
			FString::Printf(
				TEXT("Payload type '%s' does not derive from FModEventPayload, so it can never be carried by an event context."),
				*InDescriptor.PayloadType->GetName()),
			EventIdString);
		return false;
	}

	if (EventTypes.Contains(InDescriptor.EventId))
	{
		if (!IsFrameworkEventId(InDescriptor.EventId))
		{
			OutError = FModDiagnostic::Error(
				FName(TEXT("Events.DuplicateEventType")),
				FString::Printf(TEXT("Event type '%s' is already registered."), *EventIdString),
				EventIdString);
			return false;
		}

		// Framework ids are pre-declared by the constructor, so a game replacing one is a
		// deliberate override rather than a collision.
		UE_LOG(LogModFramework, Verbose,
			TEXT("Event type '%s' replaces the framework's built-in declaration."), *EventIdString);
	}

	EventTypes.Add(InDescriptor.EventId, InDescriptor);

	UE_LOG(LogModFramework, Verbose,
		TEXT("Registered event type '%s' (payload '%s', server authoritative %s, %d required permission(s))."),
		*EventIdString,
		InDescriptor.PayloadType != nullptr ? *InDescriptor.PayloadType->GetName() : TEXT("none"),
		InDescriptor.bServerAuthoritative ? TEXT("yes") : TEXT("no"),
		InDescriptor.RequiredPermissions.Num());

	return true;
}

bool UModEventBus::UnregisterEventType(FName EventId)
{
	if (EventId.IsNone())
	{
		return false;
	}

	if (EventTypes.Remove(EventId) == 0)
	{
		return false;
	}

	UE_LOG(LogModFramework, Verbose,
		TEXT("Unregistered event type '%s'. %d subscription(s) remain and will still be dispatched."),
		*EventId.ToString(), GetSubscriberCount(EventId));

	return true;
}

TArray<FModEventDescriptor> UModEventBus::GetEventTypes() const
{
	TArray<FModEventDescriptor> Result;
	Result.Reserve(EventTypes.Num());

	for (const TPair<FName, FModEventDescriptor>& Pair : EventTypes)
	{
		Result.Add(Pair.Value);
	}

	Result.Sort([](const FModEventDescriptor& A, const FModEventDescriptor& B)
	{
		return A.EventId.LexicalLess(B.EventId);
	});

	return Result;
}

bool UModEventBus::GetEventType(FName EventId, FModEventDescriptor& OutDescriptor) const
{
	if (const FModEventDescriptor* Found = EventTypes.Find(EventId))
	{
		OutDescriptor = *Found;
		return true;
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////
// Subscription

FModEventHandle UModEventBus::AddSubscription(FName EventId, const FModId& Subscriber, int32 Priority,
	FModEventDelegate&& Native, FModEventDynamicDelegate&& Dynamic, const TCHAR* CallingFunction)
{
	FModEventHandle Handle;

	if (EventId.IsNone())
	{
		UE_LOG(LogModFramework, Warning, TEXT("%s refused: the event id is empty (subscriber: %s)."),
			CallingFunction, *DescribeSource(Subscriber));
		return Handle;
	}

	if (!Native.IsBound() && !Dynamic.IsBound())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("%s refused for event '%s': the handler delegate is not bound (subscriber: %s)."),
			CallingFunction, *EventId.ToString(), *DescribeSource(Subscriber));
		return Handle;
	}

	Handle.Id = NextHandleId++;

	// FindOrAdd may rehash the map. That is safe even mid-broadcast because dispatch never holds a
	// reference into these containers - it works from a snapshot.
	TArray<FModEventSubscription>& SubscriptionsForEvent = Subscriptions.FindOrAdd(EventId);

	FModEventSubscription& Subscription = SubscriptionsForEvent.AddDefaulted_GetRef();
	Subscription.Handle = Handle;
	Subscription.Subscriber = Subscriber;
	Subscription.Priority = Priority;
	Subscription.Native = MoveTemp(Native);
	Subscription.Dynamic = MoveTemp(Dynamic);
	Subscription.bPendingRemoval = false;

	HandleToEventId.Add(Handle.Id, EventId);

	UE_LOG(LogModFramework, Verbose,
		TEXT("%s: subscription %lld added to event '%s' for %s at priority %d."),
		CallingFunction, Handle.Id, *EventId.ToString(), *DescribeSource(Subscriber), Priority);

	return Handle;
}

FModEventHandle UModEventBus::Subscribe(FName EventId, const FModId& Subscriber, FModEventDelegate Delegate, int32 Priority)
{
	return AddSubscription(EventId, Subscriber, Priority, MoveTemp(Delegate), FModEventDynamicDelegate(),
		TEXT("UModEventBus::Subscribe"));
}

FModEventHandle UModEventBus::K2_Subscribe(FName EventId, FModId Subscriber, FModEventDynamicDelegate Delegate, int32 Priority)
{
	return AddSubscription(EventId, Subscriber, Priority, FModEventDelegate(), MoveTemp(Delegate),
		TEXT("Subscribe To Mod Event"));
}

void UModEventBus::MarkHandleForRemoval(int64 HandleId)
{
	const FName* EventIdPtr = HandleToEventId.Find(HandleId);
	if (EventIdPtr == nullptr)
	{
		return;
	}

	const FName EventId = *EventIdPtr;
	HandleToEventId.Remove(HandleId);

	TArray<FModEventSubscription>* SubscriptionsForEvent = Subscriptions.Find(EventId);
	if (SubscriptionsForEvent == nullptr)
	{
		return;
	}

	for (FModEventSubscription& Subscription : *SubscriptionsForEvent)
	{
		if (Subscription.Handle.Id == HandleId)
		{
			Subscription.bPendingRemoval = true;
			PendingRemovalHandles.Add(HandleId);
			break;
		}
	}
}

bool UModEventBus::Unsubscribe(FModEventHandle Handle)
{
	if (!Handle.IsValid() || !HandleToEventId.Contains(Handle.Id))
	{
		return false;
	}

	MarkHandleForRemoval(Handle.Id);

	if (BroadcastDepth == 0)
	{
		CompactSubscriptions();
	}

	return true;
}

void UModEventBus::UnsubscribeAllForMod(const FModId& ModId)
{
	if (!ModId.IsValid())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModEventBus::UnsubscribeAllForMod ignored: an invalid mod id means \"the game\", and removing the game's own handlers is never what a caller wants."));
		return;
	}

	int32 RemovedCount = 0;

	for (TPair<FName, TArray<FModEventSubscription>>& Pair : Subscriptions)
	{
		for (FModEventSubscription& Subscription : Pair.Value)
		{
			if (!Subscription.bPendingRemoval && Subscription.Subscriber == ModId)
			{
				Subscription.bPendingRemoval = true;
				PendingRemovalHandles.Add(Subscription.Handle.Id);
				HandleToEventId.Remove(Subscription.Handle.Id);
				++RemovedCount;
			}
		}
	}

	if (RemovedCount > 0)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Cancelled %d event subscription(s) owned by mod '%s'."),
			RemovedCount, *ModId.ToString());
	}

	if (BroadcastDepth == 0)
	{
		CompactSubscriptions();
	}
}

void UModEventBus::Reset()
{
	if (BroadcastDepth > 0)
	{
		// A handler asked for a teardown mid-dispatch. Silence every handler that has not run yet
		// and do the real work when the outermost broadcast unwinds, so the in-flight snapshot
		// stays consistent.
		for (TPair<FName, TArray<FModEventSubscription>>& Pair : Subscriptions)
		{
			for (FModEventSubscription& Subscription : Pair.Value)
			{
				Subscription.bPendingRemoval = true;
				PendingRemovalHandles.Add(Subscription.Handle.Id);
			}
		}

		HandleToEventId.Reset();
		bResetPending = true;

		UE_LOG(LogModFramework, Verbose,
			TEXT("UModEventBus::Reset was called during a broadcast; the remaining handlers are silenced and the teardown is deferred."));
		return;
	}

	Subscriptions.Reset();
	HandleToEventId.Reset();
	PendingRemovalHandles.Reset();
	UnregisteredBroadcastCounts.Reset();
	EventTypes.Reset();
	bResetPending = false;

	// NextHandleId deliberately keeps counting: a handle captured before the reset must never be
	// able to name a subscription created after it. PermissionCheck is infrastructure injected by
	// the subsystem, not content, so it survives too.
	RegisterBuiltinEventTypes();
}

void UModEventBus::CompactSubscriptions()
{
	if (PendingRemovalHandles.Num() == 0)
	{
		return;
	}

	for (auto It = Subscriptions.CreateIterator(); It; ++It)
	{
		TArray<FModEventSubscription>& SubscriptionsForEvent = It.Value();

		SubscriptionsForEvent.RemoveAll([](const FModEventSubscription& Subscription)
		{
			return Subscription.bPendingRemoval;
		});

		if (SubscriptionsForEvent.Num() == 0)
		{
			It.RemoveCurrent();
		}
	}

	PendingRemovalHandles.Reset();
}

int32 UModEventBus::GetSubscriberCount(FName EventId) const
{
	const TArray<FModEventSubscription>* SubscriptionsForEvent = Subscriptions.Find(EventId);
	if (SubscriptionsForEvent == nullptr)
	{
		return 0;
	}

	int32 Count = 0;
	for (const FModEventSubscription& Subscription : *SubscriptionsForEvent)
	{
		if (!Subscription.bPendingRemoval)
		{
			++Count;
		}
	}

	return Count;
}

int32 UModEventBus::GetUnregisteredBroadcastCount(FName EventId) const
{
	const int32* Count = UnregisteredBroadcastCounts.Find(EventId);
	return Count != nullptr ? *Count : 0;
}

//////////////////////////////////////////////////////////////////////////
// Broadcast

FString UModEventBus::DescribeSource(const FModId& SourceModId)
{
	return SourceModId.IsValid()
		? FString::Printf(TEXT("mod '%s'"), *SourceModId.ToString())
		: FString(TEXT("the game"));
}

const UWorld* UModEventBus::ResolveWorld(const UObject* WorldContextObject)
{
	if (WorldContextObject == nullptr)
	{
		return nullptr;
	}

	if (const UWorld* AsWorld = Cast<UWorld>(WorldContextObject))
	{
		return AsWorld;
	}

	if (GEngine != nullptr)
	{
		// ReturnNull: a context object that cannot name a world is a normal situation here, not an
		// error - the framework broadcasts plenty of events with no world at all.
		return GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	}

	return WorldContextObject->GetWorld();
}

bool UModEventBus::CheckBroadcastPermissions(const FModEventDescriptor& Descriptor, const FModId& SourceModId, FString& OutReason) const
{
	if (Descriptor.RequiredPermissions.Num() == 0)
	{
		return true;
	}

	// An invalid source id means the game or the framework raised the event. The host is not
	// permission gated against itself.
	if (!SourceModId.IsValid())
	{
		return true;
	}

	if (!PermissionCheck.IsBound())
	{
		// Fail closed. A permission gate that cannot be evaluated is not a gate.
		OutReason = FString::Printf(
			TEXT("no permission check is installed on the event bus, so the %d required permission(s) cannot be verified"),
			Descriptor.RequiredPermissions.Num());
		return false;
	}

	for (const FName& Permission : Descriptor.RequiredPermissions)
	{
		if (Permission.IsNone())
		{
			continue;
		}

		if (!PermissionCheck.Execute(SourceModId, Permission))
		{
			OutReason = FString::Printf(TEXT("mod '%s' does not hold the required permission '%s'"),
				*SourceModId.ToString(), *Permission.ToString());
			return false;
		}
	}

	return true;
}

void UModEventBus::Broadcast(const FModEventContext& Context)
{
	if (Context.EventId.IsNone())
	{
		UE_LOG(LogModFramework, Warning, TEXT("UModEventBus::Broadcast refused: the event context carries no event id."));
		return;
	}

	if (BroadcastDepth >= ModEventBusPrivate::MaxBroadcastDepth)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Refused broadcast of event '%s' from %s: the bus is already %d broadcasts deep, which almost always means a handler re-raises an event it also listens for."),
			*Context.EventId.ToString(), *DescribeSource(Context.SourceModId), BroadcastDepth);
		return;
	}

	if (const FModEventDescriptor* Found = EventTypes.Find(Context.EventId))
	{
		// Copy rather than alias: the injected permission check is game code and could legally
		// register another event type, which would rehash the map out from under a pointer.
		const FModEventDescriptor Descriptor = *Found;

		FString RefusalReason;
		if (!CheckBroadcastPermissions(Descriptor, Context.SourceModId, RefusalReason))
		{
			UE_LOG(LogModFramework, Warning, TEXT("Refused broadcast of event '%s': %s."),
				*Context.EventId.ToString(), *RefusalReason);
			return;
		}

		if (Descriptor.bServerAuthoritative)
		{
			const UWorld* World = ResolveWorld(Context.WorldContext);
			if (World != nullptr)
			{
				if (World->GetNetMode() == NM_Client)
				{
					UE_LOG(LogModFramework, Warning,
						TEXT("Refused broadcast of server-authoritative event '%s' from %s: world '%s' is a network client."),
						*Context.EventId.ToString(), *DescribeSource(Context.SourceModId), *World->GetName());
					return;
				}
			}
			else
			{
				UE_LOG(LogModFramework, Verbose,
					TEXT("Server-authoritative event '%s' was broadcast without a resolvable world context, so the network-client check could not run."),
					*Context.EventId.ToString());
			}
		}

		if (Descriptor.PayloadType != nullptr && Context.HasPayload())
		{
			const UScriptStruct* ProvidedType = Context.GetPayloadType();
			if (ProvidedType == nullptr || !ProvidedType->IsChildOf(Descriptor.PayloadType.Get()))
			{
				// Delivered anyway: GetPayload<T> is a checked cast, so a handler expecting the
				// declared type simply sees no payload rather than misreading these bytes.
				UE_LOG(LogModFramework, Warning,
					TEXT("Event '%s' was broadcast by %s with payload type '%s' but declares '%s'. Handlers asking for the declared type will see no payload."),
					*Context.EventId.ToString(),
					*DescribeSource(Context.SourceModId),
					ProvidedType != nullptr ? *ProvidedType->GetName() : TEXT("none"),
					*Descriptor.PayloadType->GetName());
			}
		}
	}
	else
	{
		int32& Count = UnregisteredBroadcastCounts.FindOrAdd(Context.EventId);
		++Count;

		UE_LOG(LogModFramework, Verbose,
			TEXT("Broadcasting undeclared event id '%s' from %s (occurrence %d). Registering a descriptor for it later is supported."),
			*Context.EventId.ToString(), *DescribeSource(Context.SourceModId), Count);
	}

	const TArray<FModEventSubscription>* LiveSubscriptions = Subscriptions.Find(Context.EventId);
	if (LiveSubscriptions == nullptr || LiveSubscriptions->Num() == 0)
	{
		return;
	}

	// Snapshot before dispatching. Handlers are free to subscribe, unsubscribe or reset the bus,
	// any of which can reallocate or rehash the live containers, so nothing below may hold a
	// pointer into them.
	TArray<FModEventSubscription> Snapshot;
	Snapshot.Reserve(LiveSubscriptions->Num());
	for (const FModEventSubscription& Subscription : *LiveSubscriptions)
	{
		if (!Subscription.bPendingRemoval)
		{
			Snapshot.Add(Subscription);
		}
	}
	LiveSubscriptions = nullptr;

	if (Snapshot.Num() == 0)
	{
		return;
	}

	// Highest priority first; equal priorities keep registration order because handle ids only
	// ever increase. Handle ids are unique, so this is a total order and an unstable sort is fine.
	Snapshot.Sort([](const FModEventSubscription& A, const FModEventSubscription& B)
	{
		if (A.Priority != B.Priority)
		{
			return A.Priority > B.Priority;
		}
		return A.Handle.Id < B.Handle.Id;
	});

	++BroadcastDepth;
	ON_SCOPE_EXIT
	{
		--BroadcastDepth;
		if (BroadcastDepth == 0)
		{
			CompactSubscriptions();

			if (bResetPending)
			{
				Reset();
			}
		}
	};

	for (FModEventSubscription& Subscription : Snapshot)
	{
		// Re-check against the live cancellation set: an earlier handler in this very dispatch may
		// have unsubscribed this one, and a cancelled subscription must not fire.
		if (PendingRemovalHandles.Contains(Subscription.Handle.Id))
		{
			continue;
		}

		if (!Subscription.IsBound())
		{
			// The bound object has been destroyed. Retire the subscription instead of testing a
			// dead delegate on every future broadcast.
			UE_LOG(LogModFramework, Verbose,
				TEXT("Dropping subscription %lld to event '%s': its handler object no longer exists."),
				Subscription.Handle.Id, *Context.EventId.ToString());

			MarkHandleForRemoval(Subscription.Handle.Id);
			continue;
		}

		if (Subscription.Native.IsBound())
		{
			Subscription.Native.Execute(Context);
		}
		else
		{
			Subscription.Dynamic.Execute(Context);
		}
	}
}

void UModEventBus::K2_Broadcast(FName EventId, FModId SourceModId, const TInstancedStruct<FModEventPayload>& Payload, UObject* WorldContext)
{
	FModEventContext Context;
	Context.EventId = EventId;
	Context.SourceModId = SourceModId;
	Context.Payload = Payload;
	Context.WorldContext = WorldContext;

	Broadcast(Context);
}

void UModEventBus::BroadcastLifecycle(FName EventId, const FModId& ModId, EModState State,
	EModLoadFailureReason Reason, const FString& Message)
{
	FModEventContext Context;
	Context.EventId = EventId;

	// SourceModId stays invalid on purpose: the framework raises lifecycle events *about* a mod,
	// never *as* one, so they are not subject to that mod's permissions. The mod is in the payload.
	Context.WorldContext = this;

	FModLifecycleEventPayload& Lifecycle = Context.Payload.InitializeAs<FModLifecycleEventPayload>();
	Lifecycle.ModId = ModId;
	Lifecycle.State = State;
	Lifecycle.FailureReason = Reason;
	Lifecycle.Message = Message;

	Broadcast(Context);
}

void UModEventBus::SetPermissionCheck(UModAPIRegistry::FModPermissionCheck InCheck)
{
	PermissionCheck = MoveTemp(InCheck);
}
