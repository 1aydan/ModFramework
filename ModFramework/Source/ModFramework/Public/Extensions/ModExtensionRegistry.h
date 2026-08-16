// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "API/ModAPIRegistry.h"
#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/DelegateCombinations.h"
#include "Extensions/ModExtension.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "ModExtensionRegistry.generated.h"

/**
 * The extensions contributed to one extension point, kept sorted.
 *
 * This exists only because a UPROPERTY TMap cannot hold a TArray value directly: the reflection
 * system has no container-of-container property type, so the array has to be wrapped in a USTRUCT.
 * Nothing outside UModExtensionRegistry should touch it.
 */
USTRUCT()
struct MODFRAMEWORK_API FModExtensionList
{
	GENERATED_BODY()

	/**
	 * Sorted by (Priority descending, owning mod's load order ascending, resolved ExtensionId
	 * ascending). The registry re-sorts on every mutation and whenever the load order changes, so
	 * readers can rely on the stored order without sorting again.
	 */
	UPROPERTY()
	TArray<TObjectPtr<UModExtension>> Extensions;
};

/**
 * Owns every extension point a game opened and every extension the loaded mods contributed to them.
 *
 * The registry is deliberately ignorant of what an extension *does*. It enforces identity
 * (a point must exist, an id must be unique within it), typing (the extension must derive from the
 * point's RequiredBaseClass), quota (bAllowMultiplePerMod), permissions (through an injected check
 * so the registry never owns permission state) and ordering. Everything else is the game's business.
 *
 * Ordering is a total order and therefore reproducible across runs:
 *   1. Priority, descending  - the mod author's own weight.
 *   2. The owning mod's load order, ascending - mods the resolver put first extend first.
 *      A mod absent from the last SetModLoadOrder call sorts after every known mod.
 *   3. The resolved extension id, lexically ascending - unique within a point, so no ties remain.
 *
 * Untrusted input: every RegisterExtension argument comes from a mod. Nothing here asserts; every
 * rejection produces an FModDiagnostic with one of these stable codes:
 *   Extension.Invalid              null extension, no ExtensionPointId, or an unusable class
 *   Extension.PointNotFound        ExtensionPointId names no registered point
 *   Extension.BaseClassMismatch    the class does not derive from the point's RequiredBaseClass
 *   Extension.DuplicateId          the resolved id is already registered against that point
 *   Extension.MultipleNotAllowed   the mod already contributed and the point forbids a second
 *   Extension.PermissionDenied     the owning mod lacks one of the point's RequiredPermissions
 *   ExtensionPoint.InvalidId       an extension point id of None
 *   ExtensionPoint.Duplicate       that extension point id is already registered
 *   ExtensionPoint.InUse           (logged) the point still has extensions registered against it
 *
 * Threading: game thread only, like the rest of the framework's UObject-owning registries.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModExtensionRegistry : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Opens an extension point. Fails when the id is None (ExtensionPoint.InvalidId) or already
	 * registered (ExtensionPoint.Duplicate); registering a point never replaces an existing one,
	 * because the extensions already filed under it were validated against the old descriptor.
	 */
	bool RegisterExtensionPoint(const FModExtensionPointDescriptor& InDescriptor, FModDiagnostic& OutError);

	/**
	 * Closes an extension point. Refuses (returning false and logging ExtensionPoint.InUse) while any
	 * extension is still registered against it; unregister those first, or call UnregisterAllForMod.
	 */
	bool UnregisterExtensionPoint(FName ExtensionPointId);

	UFUNCTION(BlueprintPure, Category = "Mod|Extensions")
	bool GetExtensionPoint(FName ExtensionPointId, FModExtensionPointDescriptor& OutDescriptor) const;

	/** Every registered point, sorted by id so the output is stable. */
	UFUNCTION(BlueprintPure, Category = "Mod|Extensions")
	TArray<FModExtensionPointDescriptor> GetExtensionPoints() const;

	/**
	 * Files a mod's extension under its declared point after running every validation above.
	 *
	 * On success InExtension->OwningModId is set, OnExtensionRegistered fires and OnExtensionsChanged
	 * broadcasts. The extension starts *inactive*: call SetModExtensionsActive once the owning mod
	 * reaches EModState::Activated. On failure OutError carries the reason and the extension is left
	 * exactly as it was found.
	 */
	bool RegisterExtension(UModExtension* InExtension, const FModId& OwningModId, FModDiagnostic& OutError);

	/** Removes one extension, matched on its resolved id. Fires OnExtensionUnregistered. */
	bool UnregisterExtension(FName ExtensionPointId, FName ExtensionId);

	/** Removes every extension a mod contributed, across all points. Safe when the mod has none. */
	void UnregisterAllForMod(const FModId& ModId);

	/**
	 * Flips the activation state of every extension a mod contributed, firing OnExtensionActivated /
	 * OnExtensionDeactivated only for the ones that actually changed.
	 */
	void SetModExtensionsActive(const FModId& ModId, bool bActive);

	/**
	 * Full teardown: unregisters every extension and closes every extension point, then forgets the
	 * load order. The injected permission check survives, so a registry that is reset and refilled
	 * keeps behaving. Call this on shutdown, not between refreshes - the game's extension points
	 * would go with it; UnregisterAllForMod is the per-mod tool.
	 *
	 * Both change delegates fire once afterwards with a None point id and an invalid mod id, meaning
	 * "everything you cached is stale".
	 */
	void Reset();

	/** Active extensions of a point, in registry order. */
	UFUNCTION(BlueprintPure, Category = "Mod|Extensions")
	TArray<UModExtension*> GetExtensions(FName ExtensionPointId) const;

	/** Every extension of a point including the ones whose mod is loaded but not activated. */
	UFUNCTION(BlueprintPure, Category = "Mod|Extensions")
	TArray<UModExtension*> GetAllExtensions(FName ExtensionPointId) const;

	/** Matches on the resolved id, so both an authored id and the "<modid>:<classname>" default work. */
	UFUNCTION(BlueprintPure, Category = "Mod|Extensions")
	UModExtension* FindExtension(FName ExtensionPointId, FName ExtensionId) const;

	/** Every extension one mod contributed, across all points, in registry order per point. */
	UFUNCTION(BlueprintPure, Category = "Mod|Extensions")
	TArray<UModExtension*> GetExtensionsForMod(const FModId& ModId) const;

	/**
	 * Applies the point's DefaultConflictPolicy to pick a single winner among the *active*
	 * extensions that claim ResourceId:
	 *   FirstWins - the earliest load order;
	 *   LastWins  - the latest load order;
	 *   Priority  - the highest Priority, ties broken by the latest load order;
	 *   Error and Merge - no single winner, so nullptr as soon as there is more than one contender.
	 * A single contender always wins, whatever the policy. Returns nullptr when the point is unknown
	 * or nothing claims the resource.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|Extensions")
	UModExtension* ResolveResource(FName ExtensionPointId, FName ResourceId) const;

	/**
	 * Every claim currently registered, for FModConflictDetector.
	 *
	 * One entry per (extension, claimed resource) pair, over *all* registered extensions rather than
	 * only the active ones, so conflicts surface before anything is activated. LoadOrder is
	 * INDEX_NONE for a mod the registry has no load order for; PreferredPolicy is the owning point's
	 * DefaultConflictPolicy, which is the only policy the registry knows about.
	 */
	TArray<FModResourceClaim> CollectResourceClaims() const;

	/**
	 * Ordering input from the resolver. Index 0 loads first. Mods missing from InOrder sort after
	 * every listed mod. Re-sorts every point immediately.
	 */
	void SetModLoadOrder(const TArray<FModId>& InOrder);

	/**
	 * Injected by UModSubsystem so the registry can ask about permissions without owning them.
	 * While no check is set every permission counts as held - a bare registry (tests, tooling) must
	 * not silently reject everything.
	 */
	void SetPermissionCheck(UModAPIRegistry::FModPermissionCheck InCheck);

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnModExtensionsChanged, FName /*PointId*/, const FModId& /*ModId*/);

	/** Raised whenever the extension set of a point changes. The mod id is invalid for bulk changes. */
	FOnModExtensionsChanged OnExtensionsChanged;

	/** Raised when a point is opened or closed. The mod id is always invalid. */
	FOnModExtensionsChanged OnExtensionPointsChanged;

private:
	/** The mod's index in the last SetModLoadOrder call, or MAX_int32 when it was not listed. */
	int32 GetSortLoadOrder(const FModId& ModId) const;

	/** The total order described in the class comment. */
	bool ExtensionLess(const UModExtension& A, const UModExtension& B) const;

	/** Re-applies the total order to one point's list. */
	void SortExtensionList(FModExtensionList& InOutList);

	/** Detaches an extension from the registry and runs its teardown callbacks. */
	void ReleaseExtension(UModExtension* InExtension);

	UPROPERTY()
	TMap<FName, FModExtensionPointDescriptor> ExtensionPoints;

	UPROPERTY()
	TMap<FName, FModExtensionList> Extensions;

	/** Not a UPROPERTY: FModId is a plain identifier, there is nothing here for the GC to keep alive. */
	TMap<FModId, int32> ModLoadOrder;

	UModAPIRegistry::FModPermissionCheck PermissionCheck;
};
