// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "API/ModAPIRegistry.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Events/ModEventTypes.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "ModEventBus.generated.h"

class UWorld;

/**
 * One live subscription.
 *
 * Internal to UModEventBus; it only lives in the public header because it is the value type of a
 * member container. Nothing outside the bus should construct or hold one of these.
 *
 * Exactly one of Native / Dynamic is bound. Both are weak with respect to their target UObject, so
 * a subscription never keeps a mod object alive and never needs to be a UPROPERTY.
 */
struct FModEventSubscription
{
	/** Identity handed back to the subscriber. */
	FModEventHandle Handle;

	/** Owning mod, or an invalid id when the game itself subscribed. */
	FModId Subscriber;

	/** Higher priorities are dispatched first; ties break by handle id, so registration order wins. */
	int32 Priority = 0;

	/** Native handler. */
	FModEventDelegate Native;

	/** Blueprint handler. */
	FModEventDynamicDelegate Dynamic;

	/**
	 * Set when the subscription has been cancelled but a broadcast is still in flight. Entries in
	 * this state are skipped by dispatch and removed by the next compaction at re-entrancy depth 0.
	 */
	bool bPendingRemoval = false;

	/** False once the bound object has been destroyed, which is how the bus prunes dead handlers. */
	bool IsBound() const
	{
		return Native.IsBound() || Dynamic.IsBound();
	}
};

/**
 * The framework's generic event bus.
 *
 * The bus knows nothing about what any event means. It stores event *declarations*, keeps ordered
 * subscriber lists, and enforces three rules on every broadcast:
 *
 *  1. Permissions - if the event's descriptor lists RequiredPermissions and the broadcast names a
 *     source mod, that mod must hold all of them. A broadcast with an invalid SourceModId comes
 *     from the game or the framework and always passes.
 *  2. Server authority - a descriptor marked bServerAuthoritative cannot be broadcast from a world
 *     whose net mode is NM_Client.
 *  3. Payload shape - a payload whose struct does not match the declared PayloadType is reported.
 *
 * Broadcasting an event id that nobody registered is deliberately legal: it logs at Verbose, counts
 * the occurrence, and delivers normally. That keeps a game free to raise events before its
 * descriptors are installed, and registering the type later works exactly as expected.
 *
 * Re-entrancy is fully supported. Dispatch runs over a snapshot, so a handler may subscribe,
 * unsubscribe, reset the bus or broadcast a nested event without corrupting the in-flight
 * iteration. Cancellations take effect immediately (a handler that has not run yet will not run)
 * but the underlying storage is only compacted when the outermost broadcast unwinds. Nesting is
 * capped at a fixed depth so a mod handler that re-raises its own event logs a warning instead of
 * overflowing the stack.
 *
 * The bus is not thread safe. Broadcast, subscribe and unsubscribe from the game thread.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModEventBus : public UObject
{
	GENERATED_BODY()

public:
	UModEventBus();

	// --- Event types ---------------------------------------------------------------------------

	/**
	 * Declares an event type.
	 *
	 * Fails when the id is empty, when PayloadType does not derive from FModEventPayload, or when a
	 * non-framework id is already registered. Re-registering one of the framework's own ids is
	 * allowed and replaces the built-in declaration, so a game can tighten a framework event.
	 *
	 * OutError is only written when this returns false.
	 */
	bool RegisterEventType(const FModEventDescriptor& InDescriptor, FModDiagnostic& OutError);

	/**
	 * Removes an event declaration. Existing subscriptions are left alone and keep receiving
	 * broadcasts - the event simply becomes an undeclared one again.
	 */
	bool UnregisterEventType(FName EventId);

	/** Every declared event, sorted by id so tooling output is stable. */
	UFUNCTION(BlueprintPure, Category = "Mod|Events")
	TArray<FModEventDescriptor> GetEventTypes() const;

	UFUNCTION(BlueprintPure, Category = "Mod|Events")
	bool GetEventType(FName EventId, FModEventDescriptor& OutDescriptor) const;

	// --- Subscription --------------------------------------------------------------------------

	/**
	 * Adds a native handler.
	 *
	 * Subscribing to an id that has no descriptor is legal. Pass an invalid Subscriber when the
	 * game itself is subscribing; a valid one lets UnsubscribeAllForMod tear the subscription down
	 * when the mod unloads.
	 *
	 * Returns an invalid handle when the id is empty or the delegate is unbound.
	 */
	FModEventHandle Subscribe(FName EventId, const FModId& Subscriber, FModEventDelegate Delegate, int32 Priority = 0);

	/** Blueprint spelling of Subscribe. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Events", meta = (DisplayName = "Subscribe To Mod Event"))
	FModEventHandle K2_Subscribe(FName EventId, FModId Subscriber, FModEventDynamicDelegate Delegate, int32 Priority = 0);

	/**
	 * Cancels one subscription. Safe to call from inside a handler, including on the subscription
	 * currently being dispatched. Returns false for an unknown or already-cancelled handle.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Events")
	bool Unsubscribe(FModEventHandle Handle);

	/**
	 * Cancels every subscription owned by one mod. Called by the subsystem when a mod unloads.
	 * An invalid mod id is ignored rather than treated as "the game", so this can never take out
	 * the host game's own handlers by accident.
	 */
	void UnsubscribeAllForMod(const FModId& ModId);

	/**
	 * Drops every subscription and every event declaration, then reinstates the framework's
	 * built-in declarations so the bus is exactly as it was when constructed. The injected
	 * permission check survives - it is infrastructure, not content - and handle ids keep counting
	 * up so stale handles can never alias a new subscription.
	 *
	 * Calling this from inside a handler is safe: the remaining handlers of the in-flight broadcast
	 * are silenced immediately and the teardown happens when the outermost broadcast unwinds.
	 */
	void Reset();

	// --- Broadcast -----------------------------------------------------------------------------

	/** Dispatches to every live subscriber of Context.EventId, highest priority first. */
	void Broadcast(const FModEventContext& Context);

	/** Blueprint spelling of Broadcast. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Events", meta = (DisplayName = "Broadcast Mod Event"))
	void K2_Broadcast(FName EventId, FModId SourceModId, const TInstancedStruct<FModEventPayload>& Payload, UObject* WorldContext);

	/**
	 * Convenience for the ModFrameworkEvents::Mod* events, used by UModSubsystem.
	 *
	 * The broadcast's SourceModId is deliberately left invalid: the framework raises lifecycle
	 * events *about* a mod, it does not raise them *as* that mod, so they are never subjected to
	 * the mod's permissions. The mod in question is in the payload's ModId field.
	 */
	void BroadcastLifecycle(FName EventId, const FModId& ModId, EModState State,
		EModLoadFailureReason Reason = EModLoadFailureReason::None, const FString& Message = FString());

	/** Injected by UModSubsystem so the bus can gate broadcasts without owning the permission model. */
	void SetPermissionCheck(UModAPIRegistry::FModPermissionCheck InCheck);

	/** Live subscribers for an event, excluding any cancelled during an in-flight broadcast. */
	UFUNCTION(BlueprintPure, Category = "Mod|Events")
	int32 GetSubscriberCount(FName EventId) const;

	// --- Diagnostics ---------------------------------------------------------------------------

	/** How often an undeclared event id has been broadcast. For console dumps and tests. */
	int32 GetUnregisteredBroadcastCount(FName EventId) const;

	/** True while a broadcast is in flight, including nested ones. */
	bool IsBroadcasting() const { return BroadcastDepth > 0; }

	/** True for the ids in the ModFrameworkEvents namespace. */
	static bool IsFrameworkEventId(FName EventId);

private:
	/** Installs the declarations for every ModFrameworkEvents id. Called from the constructor. */
	void RegisterBuiltinEventTypes();

	/** Shared body of Subscribe and K2_Subscribe. Exactly one delegate is expected to be bound. */
	FModEventHandle AddSubscription(FName EventId, const FModId& Subscriber, int32 Priority,
		FModEventDelegate&& Native, FModEventDynamicDelegate&& Dynamic, const TCHAR* CallingFunction);

	/** Flags one subscription as cancelled and retires its handle. No-op for unknown handles. */
	void MarkHandleForRemoval(int64 HandleId);

	/** Erases every cancelled subscription. Only ever called at BroadcastDepth == 0. */
	void CompactSubscriptions();

	/** True when SourceModId may raise Descriptor. Fills OutReason with a log-ready phrase if not. */
	bool CheckBroadcastPermissions(const FModEventDescriptor& Descriptor, const FModId& SourceModId, FString& OutReason) const;

	/** Best-effort world resolution for the server-authoritative guard. Null when unresolvable. */
	static const UWorld* ResolveWorld(const UObject* WorldContextObject);

	/** "the game" for an invalid id, "mod '<id>'" otherwise. For log messages. */
	static FString DescribeSource(const FModId& SourceModId);

	/** Declared event types, keyed by id. Reflected so a Blueprint payload struct stays rooted. */
	UPROPERTY()
	TMap<FName, FModEventDescriptor> EventTypes;

	/** Live subscriptions per event id, in registration order; dispatch sorts a copy. */
	TMap<FName, TArray<FModEventSubscription>> Subscriptions;

	/** Handle id -> event id, so Unsubscribe does not have to search every event. */
	TMap<int64, FName> HandleToEventId;

	/** Handles cancelled but not yet compacted. Consulted per handler so a cancellation during a
	 *  broadcast takes effect immediately even though dispatch runs over a snapshot. */
	TSet<int64> PendingRemovalHandles;

	/** How often each undeclared event id has been broadcast. */
	TMap<FName, int32> UnregisteredBroadcastCounts;

	/** Supplied by UModSubsystem. Unbound means "cannot verify", which fails permission checks closed. */
	UModAPIRegistry::FModPermissionCheck PermissionCheck;

	/** Monotonically increasing, starts at 1, never rewound. */
	int64 NextHandleId = 1;

	/** Broadcast re-entrancy depth. Compaction only happens on the way out of the outermost one. */
	int32 BroadcastDepth = 0;

	/** Set when Reset() was called from inside a handler; the teardown runs once dispatch unwinds. */
	bool bResetPending = false;
};
