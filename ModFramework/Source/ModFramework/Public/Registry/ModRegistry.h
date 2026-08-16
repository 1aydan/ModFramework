// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/ContainerAllocationPolicies.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Registry/ModInfo.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ModRegistry.generated.h"

class UModAPIRegistry;
class UModExtensionRegistry;

/**
 * The UObjects one mod created, kept alive by the registry until the mod is released.
 *
 * This is a struct rather than a bare TArray because a UPROPERTY TMap cannot hold a container as its
 * value type.
 */
USTRUCT()
struct FModObjectSet
{
	GENERATED_BODY()

	/** Strong references, so a mod's objects survive garbage collection for as long as the mod does. */
	UPROPERTY()
	TArray<TObjectPtr<UObject>> Objects;
};

/**
 * The authoritative table of every mod the framework knows about in this session, plus the two
 * sub-registries whose contents are scoped to those mods.
 *
 * The registry is deliberately passive: it stores records, enforces the lifecycle state machine and
 * tells listeners when a state actually changed. It never discovers, mounts, loads or activates
 * anything - UModSubsystem drives all of that and funnels every state change back through
 * SetModState so there is exactly one place where a transition can be rejected.
 *
 * Threading: game thread only. Nothing here takes a lock.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModRegistry : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Creates the owned API and extension sub-registries. Idempotent: calling it twice keeps the
	 * existing sub-registries and their contents.
	 */
	void Initialize();

	/**
	 * Drops every mod record and tracked object, empties both sub-registries (including any APIs the
	 * game itself registered) and unbinds OnModStateChanged. Safe to call without a prior Initialize.
	 */
	void Shutdown();

	// --- Registration --------------------------------------------------------

	/**
	 * Files a newly discovered mod under its manifest id.
	 *
	 * Rejects, with OutError filled in and nothing mutated, a record whose manifest has no usable id
	 * ("Registry.InvalidModId"), an id that is already registered ("Registry.DuplicateModId") or a
	 * registration that would exceed UModFrameworkSettings::MaxMods ("Registry.LimitExceeded").
	 *
	 * A record arriving in EModState::Unknown is filed as EModState::Discovered, that being the only
	 * transition the state machine allows out of Unknown. Registration is not itself a transition, so
	 * OnModStateChanged does not fire here.
	 *
	 * OutError is written only on failure; on success it is left exactly as the caller passed it.
	 */
	bool RegisterMod(const FModInfo& InInfo, FModDiagnostic& OutError);

	/**
	 * Removes a mod record and releases everything it owned. Returns false when the id is unknown.
	 * Does not unmount content or unload code: the caller is expected to have done that already.
	 */
	bool UnregisterMod(const FModId& InId);

	/**
	 * Forgets every mod, releasing their tracked objects and unregistering their APIs and extensions.
	 * APIs and extension points the *game* registered survive, so a full mod reload does not require
	 * the game to re-announce its own surface.
	 */
	void Reset();

	// --- Lookup --------------------------------------------------------------

	/** Borrowed pointer into the table. Invalidated by any mutation; never store it. */
	const FModInfo* FindMod(const FModId& InId) const;

	/** Mutable counterpart of FindMod. Prefer the setters below so state stays validated. */
	FModInfo* FindModMutable(const FModId& InId);

	/** Copies a mod's record out. Returns false and leaves OutInfo untouched when the id is unknown. */
	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	bool GetModInfo(const FModId& ModId, FModInfo& OutInfo) const;

	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	bool IsModRegistered(const FModId& ModId) const;

	/** Every registered id, sorted lexically. */
	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	TArray<FModId> GetAllModIds() const;

	/**
	 * Every registered mod, sorted by load order ascending with unordered mods (LoadOrder
	 * INDEX_NONE) last, then by id ascending. Ids are unique, so the order is total and stable
	 * across runs.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	TArray<FModInfo> GetAllMods() const;

	/** The mods currently in one exact state, in GetAllMods order. */
	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	TArray<FModInfo> GetModsInState(EModState State) const;

	/** The mods FModInfo::IsLoaded accepts, in GetAllMods order. */
	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	TArray<FModInfo> GetLoadedMods() const;

	/** The mods in EModState::Activated, in GetAllMods order. */
	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	TArray<FModInfo> GetActiveMods() const;

	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	int32 GetModCount() const;

	// --- Mutation ------------------------------------------------------------

	/**
	 * Moves a mod to NewState if IsTransitionAllowed permits it.
	 *
	 * Returns false, logs a warning and changes nothing for an unknown id or an illegal transition.
	 * On success OnModStateChanged is broadcast once, after the new state has been committed, so a
	 * handler that reads the registry back sees the state it was told about.
	 */
	bool SetModState(const FModId& InId, EModState NewState);

	/**
	 * Records why a mod failed and moves it to EModState::Failed, which is reachable from every
	 * state. Also appends an Error diagnostic so the failure survives in FModInfo::Diagnostics.
	 */
	bool SetModFailed(const FModId& InId, EModLoadFailureReason Reason, const FString& Message);

	/**
	 * Assigns FModInfo::LoadOrder from the position of each id in InOrder and resets every mod not
	 * mentioned to INDEX_NONE.
	 *
	 * Returns false when InOrder names an unknown id or repeats one - the known, first occurrences
	 * are still applied so a partially stale order does not leave the table unordered.
	 */
	bool SetLoadOrder(const TArray<FModId>& InOrder);

	/**
	 * Switches a mod on or off.
	 *
	 * Disabling additionally moves the mod to EModState::Disabled when the state machine allows it
	 * from where the mod currently is; a mod that is already mounted or loaded keeps its state and
	 * only has the flag cleared, because tearing it down is UModSubsystem's job. Re-enabling a mod
	 * that sits in EModState::Disabled returns it to EModState::Discovered.
	 */
	bool SetModEnabled(const FModId& InId, bool bEnabled);

	/**
	 * Appends a diagnostic to a mod's record and mirrors it to LogModFramework. An entry that
	 * carries no mod id is stamped with InId. Silently ignores an unknown id.
	 */
	void AddDiagnostic(const FModId& InId, const FModDiagnostic& Diagnostic);

	/** Replaces the granted permission set, dropping duplicates and NAME_None entries. */
	void SetGrantedPermissions(const FModId& InId, const TArray<FName>& Permissions);

	/** Replaces the list of virtual mount points a mod's content is live under. */
	void SetMountedPaths(const FModId& InId, const TArray<FString>& Paths);

	// --- Mod-owned object tracking -------------------------------------------

	/**
	 * Remembers that Object belongs to a mod, so unloading the mod can release it.
	 *
	 * Ignores a null or already-garbage object and an invalid mod id. An object already owned by a
	 * *different* mod keeps its first owner and logs a warning: silently re-pointing ownership would
	 * make one mod's unload delete another mod's object.
	 */
	void TrackModObject(const FModId& InId, UObject* Object);

	/** Forgets an object without destroying it. No-op when it was not tracked for that mod. */
	void UntrackModObject(const FModId& InId, UObject* Object);

	/** The still-live objects tracked for a mod. Garbage and collected entries are skipped. */
	TArray<UObject*> GetModObjects(const FModId& InId) const;

	/**
	 * Marks every object tracked for a mod as garbage and forgets them.
	 *
	 * An object that is rooted is left alone (marking a rooted object garbage is a fatal error in
	 * the engine) but is still forgotten, and that is logged - a mod holding a root reference has to
	 * release it itself.
	 */
	void ReleaseModObjects(const FModId& InId);

	/**
	 * Which mod owns an object, or an invalid FModId when no mod does. Only objects passed to
	 * TrackModObject are found; the outer chain is not walked.
	 */
	FModId FindOwningMod(const UObject* Object) const;

	// --- Sub-registries ------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	UModAPIRegistry* GetAPIRegistry() const;

	UFUNCTION(BlueprintPure, Category = "Mod|Registry")
	UModExtensionRegistry* GetExtensionRegistry() const;

	// --- State machine -------------------------------------------------------

	/**
	 * The single definition of the mod lifecycle graph, exposed so tests and tooling can reason
	 * about it without mutating a registry.
	 *
	 *   Unknown              -> Discovered
	 *   Discovered           -> Validated | Failed | Disabled
	 *   Validated            -> DependenciesResolved | Failed | Disabled
	 *   DependenciesResolved -> Mounted | Failed
	 *   Mounted              -> Loading | Unmounted | Failed
	 *   Loading              -> Loaded | Failed
	 *   Loaded               -> Activated | Unmounted | Failed
	 *   Activated            -> Deactivated | Failed
	 *   Deactivated          -> Activated | Unmounted | Failed
	 *   Unmounted            -> Discovered | Mounted
	 *   Failed               -> Discovered
	 *   Disabled             -> Discovered
	 *
	 * Any state may move to Failed. Every other edge not listed above is rejected, including
	 * re-entering the state a mod is already in.
	 */
	static bool IsTransitionAllowed(EModState From, EModState To);

	/** Fired after a state change is committed. See SetModState. */
	FOnModStateChanged OnModStateChanged;

private:
	/**
	 * Drops reverse-lookup entries whose object has died and re-arms the growth threshold. Runs
	 * amortised - once per doubling of the index - so tracking and lookup both stay O(1).
	 */
	void PruneStaleObjectOwners() const;

	/** Removes every reverse-lookup entry that points at one mod. */
	void RemoveObjectOwnersForMod(const FModId& InId);

	/** Mod records keyed by manifest id. FModId supplies GetTypeHash and operator==. */
	UPROPERTY()
	TMap<FModId, FModInfo> Mods;

	UPROPERTY()
	TObjectPtr<UModAPIRegistry> APIRegistry;

	UPROPERTY()
	TObjectPtr<UModExtensionRegistry> ExtensionRegistry;

	/** Strong, mod-scoped references to everything a mod created. */
	UPROPERTY()
	TMap<FModId, FModObjectSet> ModObjects;

	/**
	 * Reverse index for FindOwningMod. Deliberately not a UPROPERTY: it must not keep anything
	 * alive, and a dead key has to stay distinguishable from another dead key, which is what
	 * TWeakObjectPtrMapKeyFuncs guarantees.
	 */
	mutable TMap<
		TWeakObjectPtr<UObject>,
		FModId,
		FDefaultSetAllocator,
		TWeakObjectPtrMapKeyFuncs<TWeakObjectPtr<UObject>, FModId>> ObjectOwners;

	/** Size the reverse index has to reach before the next full stale sweep. Keeps tracking O(1). */
	mutable int32 ObjectOwnersSweepThreshold = 64;
};
