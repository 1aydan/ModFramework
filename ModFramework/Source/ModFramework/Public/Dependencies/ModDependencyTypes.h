// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "UObject/ObjectMacros.h"

#include "ModDependencyTypes.generated.h"

/**
 * Everything the dependency resolver needs to know about the host game.
 *
 * This is the *other* half of every compatibility check: a manifest says what it needs, an
 * FModEnvironment says what is actually here. Keeping it in a plain struct rather than reading the
 * settings object directly means the resolver is a pure function of its inputs, so the editor
 * tooling and the automation tests can resolve against a hypothetical game version without touching
 * project settings.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEnvironment
{
	GENERATED_BODY()

	/** Reverse-DNS id of the running game, matched against every manifest's `game.id`. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString GameId;

	/** Version of the running game, matched against every manifest's `game.version` range. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModVersion GameVersion;

	/** Version of this framework build, matched against every manifest's `framework.version` range. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModVersion FrameworkVersion;

	/** Id of the modding SDK the game ships. Empty when the game ships none. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString SdkId;

	/** Version of the modding SDK the game ships. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModVersion SdkVersion;

	/**
	 * Builds the environment from UModFrameworkSettings and ModFrameworkVersion::Get().
	 *
	 * Misconfiguration is never fatal: when the settings object cannot be reached, or when GameVersion
	 * or SdkVersion do not parse as semver, this logs a warning to LogModFramework and leaves the
	 * corresponding field at its default. A field left at its default is treated as "unknown" by
	 * FModDependencyResolver::CheckEnvironment, which then skips that check and reports a warning
	 * diagnostic rather than rejecting every mod in the game.
	 */
	static FModEnvironment FromSettings();
};

/**
 * One mod the resolver refused to put in the load order, and why.
 *
 * Message is written for a player, not for a log grepper: it names the mod, what it wanted and what
 * is actually installed. RelatedMods carries the machine-readable version of the same story so a UI
 * can link to the other mods involved.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModRejection
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId ModId;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModLoadFailureReason Reason = EModLoadFailureReason::None;

	/** Complete, human-readable sentence. Safe to show in a mod browser without further formatting. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString Message;

	/**
	 * Mods implicated in the rejection.
	 *   MissingDependency / IncompatibleDependencyVersion : the dependency that could not be satisfied.
	 *   DependencyFailed                                  : the dependency chain from the immediate
	 *                                                       dependency down to the root cause, which is
	 *                                                       always the last element.
	 *   CircularDependency                                : the cycle in canonical rotation, starting at
	 *                                                       its lexicographically smallest member and not
	 *                                                       repeating that member at the end.
	 *   DuplicateModId                                    : the copy of the mod that was kept instead.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModId> RelatedMods;
};

/**
 * Input to one resolve pass.
 *
 * Candidates is the raw output of discovery: it may contain duplicate ids, mods that were never
 * meant for this game, and manifests whose dependencies point at nothing. The resolver is the layer
 * that makes sense of that, so nothing here has to be pre-filtered.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModResolveRequest
{
	GENERATED_BODY()

	/** Every discovered manifest, in discovery order. Duplicated ids are allowed and resolved. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModManifest> Candidates;

	/** Ids the player or the project turned off. Ids that match no candidate are ignored. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModId> DisabledMods;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModEnvironment Environment;

	/** When false, framework/game/sdk version checks are skipped (used by editor tooling). */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bCheckEnvironment = true;
};

/**
 * Output of one resolve pass.
 *
 * The load order is deterministic: the same Candidates in the same order, resolved against the same
 * environment, always produce byte-identical LoadOrder, Rejections and Diagnostics arrays.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModResolveResult
{
	GENERATED_BODY()

	/**
	 * True when the resolve produced a load order and nothing was rejected for a reason other than
	 * being explicitly disabled. A run in which the player merely switched some mods off is therefore
	 * still a success; a missing dependency, a version conflict or a cycle is not.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bSuccess = false;

	/** Deterministic load order. Only contains accepted mods. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModId> LoadOrder;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModRejection> Rejections;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModDiagnostic> Diagnostics;

	/**
	 * First rejection recorded for ModId, or nullptr.
	 *
	 * Note that a DuplicateModId rejection names the id of the copy that *lost*, which is the same id
	 * as the copy that was kept. An id can therefore appear both in LoadOrder and in a rejection;
	 * check LoadOrder first when you want to know whether a mod will load.
	 */
	const FModRejection* FindRejection(const FModId& ModId) const;

	/** Multi-line dump: summary, the load order with indices, every rejection, then every diagnostic. */
	FString ToDebugString() const;
};
