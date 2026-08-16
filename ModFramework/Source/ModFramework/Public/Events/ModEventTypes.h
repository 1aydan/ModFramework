// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "StructUtils/InstancedStruct.h"
#include "Templates/TypeHash.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include <type_traits>

#include "ModEventTypes.generated.h"

/**
 * Base for typed event payloads. Games and SDKs derive their own USTRUCTs from this.
 *
 * The struct is deliberately polymorphic (it has a virtual destructor) so that a derived payload
 * with non-trivial members is destroyed correctly no matter which pointer type is holding it.
 * Nothing in the framework ever calls a virtual function on a payload - dispatch is by
 * UScriptStruct identity, not by vtable - so a payload never needs to override anything.
 *
 * Payloads travel inside FModEventContext::Payload as a TInstancedStruct<FModEventPayload>, which
 * stores the concrete UScriptStruct alongside the bytes. That is what makes
 * FModEventContext::GetPayload<T>() a *checked* cast rather than a reinterpret: a handler that asks
 * for the wrong type is told so with a null pointer instead of reading garbage. Treat every payload
 * that came from a mod as untrusted and always test the result of GetPayload<T>().
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEventPayload
{
	GENERATED_BODY()

	FModEventPayload() = default;
	virtual ~FModEventPayload() = default;

	// Declaring a destructor suppresses the implicit move operations and deprecates the implicit
	// copy operations, so spell all four out. FInstancedStruct copies payloads with the struct's
	// copy assignment operator, so copy assignment in particular must stay available.
	FModEventPayload(const FModEventPayload&) = default;
	FModEventPayload& operator=(const FModEventPayload&) = default;
	FModEventPayload(FModEventPayload&&) = default;
	FModEventPayload& operator=(FModEventPayload&&) = default;
};

/**
 * Everything a handler is told about one broadcast.
 *
 * A context is a value: UModEventBus copies it into the dispatch and hands every handler the same
 * const reference, so handlers must never assume they can mutate it or that it outlives the call.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEventContext
{
	GENERATED_BODY()

	/** Which event this is. Matches the FModEventDescriptor::EventId of a registered type, or any
	 *  id at all: broadcasting an id no one registered is legal (see UModEventBus::Broadcast). */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName EventId;

	/**
	 * Who raised the event. An invalid (empty) id means "the game or the framework itself", which
	 * is never permission gated. A valid id means a mod raised it, and the bus will refuse the
	 * broadcast unless that mod holds every permission the event descriptor requires.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId SourceModId;

	/** The typed payload, or an empty instanced struct for events that carry no data. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TInstancedStruct<FModEventPayload> Payload;

	/** Optional object the bus resolves a UWorld from, used for the server-authoritative check. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TObjectPtr<UObject> WorldContext = nullptr;

	/**
	 * Checked accessor for the payload.
	 *
	 * Returns a pointer only when the stored struct is exactly T::StaticStruct() or a struct
	 * derived from it; in every other case - no payload at all, or a payload of an unrelated type -
	 * it returns nullptr. Never dereference the result without testing it: the payload may have
	 * been supplied by a mod.
	 */
	template <typename T>
	const T* GetPayload() const
	{
		static_assert(std::is_base_of_v<FModEventPayload, T>,
			"FModEventContext::GetPayload<T> requires T to derive from FModEventPayload.");

		const UScriptStruct* StoredStruct = Payload.GetScriptStruct();
		const uint8* StoredMemory = Payload.GetMemory();
		if (StoredStruct == nullptr || StoredMemory == nullptr)
		{
			return nullptr;
		}

		const UScriptStruct* RequestedStruct = T::StaticStruct();
		if (RequestedStruct == nullptr)
		{
			return nullptr;
		}

		if (StoredStruct != RequestedStruct && !StoredStruct->IsChildOf(RequestedStruct))
		{
			return nullptr;
		}

		return reinterpret_cast<const T*>(StoredMemory);
	}

	/** True when a payload of any type is attached. */
	bool HasPayload() const
	{
		return Payload.IsValid();
	}

	/** The concrete payload struct, or nullptr when no payload is attached. */
	const UScriptStruct* GetPayloadType() const
	{
		return Payload.GetScriptStruct();
	}
};

/**
 * The declaration of an event type: what it is called, what it carries and who may raise it.
 *
 * Registering a descriptor is optional - an unregistered id can still be broadcast - but it is the
 * only way to get a payload-type check, a permission gate or the server-authoritative guard, and it
 * is what lets tooling enumerate the events a game exposes to mods.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEventDescriptor
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName EventId;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString Description;

	/** Expected payload struct. Must derive from FModEventPayload. Null means "carries no data". */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TObjectPtr<UScriptStruct> PayloadType = nullptr;

	/** Server-authoritative events are refused when the broadcasting world is a network client. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bServerAuthoritative = false;

	/** A mod must hold every one of these to broadcast the event. The game itself never needs them. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FName> RequiredPermissions;

	bool IsValid() const
	{
		return !EventId.IsNone();
	}
};

/**
 * Framework event ids.
 *
 * These are the events the framework itself raises. Game-specific events belong in the game's SDK,
 * never here: the framework has no opinion about what a game's events mean.
 *
 * Every Mod.* event carries an FModLifecycleEventPayload. World.* and Game.* carry no payload.
 */
namespace ModFrameworkEvents
{
	/** "Mod.Discovered" - a provider found a mod root and parsed its manifest. */
	MODFRAMEWORK_API extern const FName ModDiscovered;
	/** "Mod.Validated" - the manifest passed semantic validation. */
	MODFRAMEWORK_API extern const FName ModValidated;
	/** "Mod.Mounted" - every content root of the mod is mounted. */
	MODFRAMEWORK_API extern const FName ModMounted;
	/** "Mod.Loaded" - the entry point exists and has been told the mod loaded. */
	MODFRAMEWORK_API extern const FName ModLoaded;
	/** "Mod.Activated" - the mod's extensions and content bundles are live. */
	MODFRAMEWORK_API extern const FName ModActivated;
	/** "Mod.Deactivated" - the mod's extensions and content bundles are no longer live. */
	MODFRAMEWORK_API extern const FName ModDeactivated;
	/** "Mod.Unloaded" - the entry point has been released and the mod's objects freed. */
	MODFRAMEWORK_API extern const FName ModUnloaded;
	/** "Mod.Unmounted" - the mod's content roots are no longer mounted. */
	MODFRAMEWORK_API extern const FName ModUnmounted;
	/** "Mod.Failed" - the mod entered EModState::Failed; the payload carries the reason. */
	MODFRAMEWORK_API extern const FName ModFailed;
	/** "Mod.Refreshed" - a full discover/resolve/mount/load pass finished. */
	MODFRAMEWORK_API extern const FName ModsRefreshed;
	/** "World.Created" - a world the framework tracks came up. */
	MODFRAMEWORK_API extern const FName WorldCreated;
	/** "World.Destroyed" - a world the framework tracks went away. */
	MODFRAMEWORK_API extern const FName WorldDestroyed;
	/** "Game.Started" - the game instance finished initialising the framework. */
	MODFRAMEWORK_API extern const FName GameStarted;
}

/** Payload for every ModFrameworkEvents::Mod* event. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModLifecycleEventPayload : public FModEventPayload
{
	GENERATED_BODY()

	/** The mod the event is about. This is *not* the broadcaster: the framework raises these. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId ModId;

	/** The state the mod is in now. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModState State = EModState::Unknown;

	/** Only meaningful for ModFrameworkEvents::ModFailed. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModLoadFailureReason FailureReason = EModLoadFailureReason::None;

	/** Free-form detail, typically the failure message. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString Message;
};

/**
 * Opaque identifier for one subscription.
 *
 * Ids come from a monotonically increasing counter that starts at 1 and is never rewound, not even
 * by UModEventBus::Reset(), so a handle kept past the lifetime of its subscription can never
 * accidentally name a different one. Zero is the invalid handle.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEventHandle
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	int64 Id = 0;

	bool IsValid() const { return Id != 0; }

	friend uint32 GetTypeHash(const FModEventHandle& In) { return GetTypeHash(In.Id); }

	bool operator==(const FModEventHandle& O) const { return Id == O.Id; }
	bool operator!=(const FModEventHandle& O) const { return Id != O.Id; }
};

/** Native handler signature. Bind with CreateUObject so the bus can drop the subscription when the
 *  owning object dies; raw and lambda bindings are the caller's responsibility to unsubscribe. */
DECLARE_DELEGATE_OneParam(FModEventDelegate, const FModEventContext&);

/** Blueprint handler signature. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FModEventDynamicDelegate, const FModEventContext&, EventContext);
