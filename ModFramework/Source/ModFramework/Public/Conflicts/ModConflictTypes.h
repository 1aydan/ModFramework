// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "ModConflictTypes.generated.h"

/**
 * One mod's declared intent to modify one named resource of one extension point.
 *
 * Claims come from two places and the framework does not distinguish them:
 *  - `FModManifest::Claims`, which a mod author writes by hand in `mod.json`, and
 *  - `UModExtension::ClaimedResourceIds`, which the extension registry turns into claims through
 *    `UModExtensionRegistry::CollectResourceClaims()`.
 *
 * Every field is untrusted. In particular `ExtensionPointId` and `ResourceId` may be `NAME_None`
 * and `ModId` may be invalid when a manifest was malformed; `FModConflictDetector::Detect` ignores
 * such claims rather than asserting on them.
 *
 * The framework never interprets what a resource *is*. A resource id is an opaque name that the
 * game (or its SDK) gives meaning to - "weapon.longsword", "table.loot.boss", "/Game/Maps/Arena" -
 * which is what keeps conflict detection generic across games.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModResourceClaim
{
	GENERATED_BODY()

	/** The mod making the claim. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId ModId;

	/** The extension point the resource belongs to. Claims never cross extension points. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName ExtensionPointId;

	/** The claimed resource, unique only within its extension point. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName ResourceId;

	/** Higher wins under EModConflictPolicy::Priority. Copied from the claiming extension. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	int32 Priority = 0;

	/**
	 * Position of the owning mod in the resolved load order, or INDEX_NONE when conflict detection
	 * runs before the dependency resolver has assigned one. Claims without a load order sort after
	 * every claim that has one.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	int32 LoadOrder = INDEX_NONE;

	/**
	 * The policy the claiming mod would like to see applied. It is only honoured when the policy
	 * table has no override for the resource or its extension point AND every claim on that
	 * resource asked for the same thing - one mod can never impose a policy on another.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModConflictPolicy PreferredPolicy = EModConflictPolicy::Error;

	/** Which extension raised the claim, for diagnostics. NAME_None for a manifest-declared claim. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName ExtensionId;
};

/**
 * One resource claimed by two or more *distinct* mods, together with the outcome of applying the
 * conflict policy to it.
 *
 * Two claims from the same mod on the same resource are never a conflict: a mod is allowed to
 * modify its own resource from several extensions.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModConflict
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName ExtensionPointId;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName ResourceId;

	/** Every distinct mod claiming the resource, in load order then id. Always 2 or more entries. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModId> Contenders;

	/** The policy that was actually applied. Explanation names the rule that selected it. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModConflictPolicy AppliedPolicy = EModConflictPolicy::Error;

	/** The mod whose claim takes effect. Invalid under Error and Merge, which pick no winner. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId Winner;

	/**
	 * The contenders the winner overrides, in the same order as Contenders.
	 * Empty whenever there is no winner - under Merge every contender is kept, and under Error
	 * every contender is blocked, so in neither case has anyone lost to anyone.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModId> Losers;

	/** True when the applied policy is Error: the conflict must be reported, not silently resolved. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bBlocking = false;

	/**
	 * A complete human sentence naming the resource, the contenders in load order, which rule chose
	 * the policy, and the winner (or why there is none). Safe to put straight in front of a player.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString Explanation;
};

/**
 * How the game wants contested resources reconciled.
 *
 * Lookups are most-specific-first: a per-resource override beats a per-extension-point override,
 * which beats DefaultPolicy. `FModConflictDetector::Detect` inserts one further rule between the
 * point override and the default - if every claim on a resource asked for the same PreferredPolicy,
 * that is used - which is why the detector does not simply call Resolve().
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModConflictPolicyTable
{
	GENERATED_BODY()

	/** Applied when nothing more specific matches. Mirrors UModFrameworkSettings::DefaultConflictPolicy. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModConflictPolicy DefaultPolicy = EModConflictPolicy::Error;

	/** Per extension point override, keyed by the extension point id. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TMap<FName, EModConflictPolicy> PointPolicies;

	/** Per resource override, keyed by MakeResourceKey(). Takes precedence over PointPolicies. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TMap<FName, EModConflictPolicy> ResourcePolicies;

	/**
	 * Builds the ResourcePolicies key for a resource: the extension point id, a single ':', then
	 * the resource id - for example "game.weapon:weapon.longsword".
	 *
	 * Always construct and look up ResourcePolicies keys through this function; it is the only
	 * definition of the format.
	 */
	static FName MakeResourceKey(FName ExtensionPointId, FName ResourceId);

	/** Resource override, else extension point override, else DefaultPolicy. */
	EModConflictPolicy Resolve(FName ExtensionPointId, FName ResourceId) const;
};
