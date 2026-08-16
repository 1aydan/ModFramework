// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "Core/ModFrameworkVersion.h"
#include "CoreTypes.h"
#include "Manifest/ModVersion.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPath.h"

#include "ModManifest.generated.h"

/**
 * One entry of a mod's `dependencies` array.
 *
 * A dependency is *required* unless bOptional is set. The resolver rejects a mod whose required
 * dependencies are missing or version-incompatible, and only emits an Info diagnostic for an
 * unsatisfied optional one.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModDependency
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModId Id;

	/** Defaults to FModVersionRange::Any() when the manifest omits `version`. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModVersionRange VersionRange;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	bool bOptional = false;

	/** Human explanation shown when the dependency cannot be satisfied. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString Reason;
};

/** The game a mod was authored against. `GameId` is the only manifest-required part. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModGameRequirement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString GameId;

	/** Defaults to FModVersionRange::Any() when the manifest omits `game.version`. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModVersionRange VersionRange;
};

/**
 * The game-specific modding SDK a mod was authored against.
 *
 * An empty SdkId means "this mod does not care which SDK is present"; the dependency resolver
 * skips the SDK check entirely in that case.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSdkRequirement
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString SdkId;

	/** Defaults to FModVersionRange::Any() when the manifest omits `sdk.version`. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModVersionRange VersionRange;
};

/**
 * One mountable chunk of a mod's content.
 *
 * RelativePath is always interpreted relative to the mod root directory. The manifest validator
 * rejects absolute paths and any path containing a ".." segment, because a mod must never be able
 * to name a file outside of its own directory.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModContentRoot
{
	GENERATED_BODY()

	/** Path relative to the mod root, e.g. "Content/MyMod.pak" or "Content". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString RelativePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	EModContentRootType Type = EModContentRootType::Pak;

	/**
	 * Virtual root the content mounts under. Defaults to "/" + <mod root folder name>.
	 * When authored it must have the form "/Word/" - a single package-root segment starting with a
	 * letter or an underscore and made only of letters, digits and underscores.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString MountPoint;

	/** Pak mount order; higher wins for identical paths. Clamped by the framework. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	int32 MountOrder = 0;
};

/** How a mod hooks itself into the running game. Every part is optional. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEntryPoint
{
	GENERATED_BODY()

	/** Optional native module name (advanced; not loaded by the MVP). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString NativeModule;

	/** Blueprint or native class deriving from UModEntryPointBase. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FSoftClassPath EntryClass;

	/** Content bundles (UModContentBundle) loaded when the mod activates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TArray<FSoftObjectPath> ContentBundles;

	/**
	 * Which registered IModScriptRuntime runs this mod's scripts, e.g. "lua". Empty means the mod
	 * ships no scripting, which is the normal case.
	 *
	 * The manifest layer deliberately does NOT check this against the runtime registry, and does not
	 * check a script's extension against it either: a manifest has to be parseable on a machine with
	 * no runtimes registered at all, which is exactly the situation the packaging tools run in.
	 * Matching a runtime to this name happens at load time, where the registry exists.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FName ScriptRuntime;

	/**
	 * Script files, relative to the mod root, LOADED IN THE ORDER LISTED.
	 *
	 * The order is part of the contract, not an implementation detail: a later script may rely on
	 * what an earlier one set up, so nothing is allowed to sort or deduplicate this array.
	 * FModManifestParser::ValidateManifest applies the same containment rules a content root gets -
	 * no absolute path, no drive letter, no ".." segment - because a mod must never be able to name a
	 * file outside of its own directory.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TArray<FString> Scripts;
};

/** What this mod declares it will modify, for conflict detection. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModResourceClaimDeclaration
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FName ExtensionPointId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FName ResourceId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	EModConflictPolicy PreferredPolicy = EModConflictPolicy::Error;

	/**
	 * Whether the manifest actually wrote a `policy` for this claim.
	 *
	 * Carried through to FModResourceClaim::bHasPreferredPolicy, where the unanimity rule needs to
	 * tell "every mod asked for Error" apart from "no mod asked for anything". Without it, two mods
	 * that omitted `policy` would silently override the game's configured default. Not serialised -
	 * it is implied by the presence of the field.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bHasPreferredPolicy = false;
};

/**
 * The parsed form of a mod's `mod.json`.
 *
 * This struct is pure data: it holds no opinion about whether the mod can actually run. Every
 * semantic rule lives in FModManifestParser::ValidateManifest (shape, security and identity) and in
 * FModDependencyResolver (environment and graph). Treat a manifest that came from disk as untrusted
 * until it has been through the validator.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModManifest
{
	GENERATED_BODY()

	/** Schema version of the file this was read from. Never greater than MODFRAMEWORK_MANIFEST_VERSION. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	int32 ManifestVersion = MODFRAMEWORK_MANIFEST_VERSION;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModId Id;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString Description;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString Author;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString Homepage;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString License;

	/**
	 * Optional path, relative to the mod root, of a plain image file used as the mod's icon in a mod
	 * browser: "Icon.png", or "Art/Icon.jpg".
	 *
	 * It is deliberately a loose image rather than a `.uasset`, because the icon has to be loadable
	 * before the mod is mounted - a mod list shows icons for mods that were merely discovered, that
	 * are switched off, or that were rejected, which is exactly when a player most needs to tell them
	 * apart. FModManifestParser::ValidateManifest restricts it to a contained relative path ending in
	 * .png, .jpg or .jpeg; empty means the mod ships no icon, which is not a problem.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString IconPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModVersion Version;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModGameRequirement Game;

	/** Which framework versions this mod runs on. Defaults to FModVersionRange::Any(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModVersionRange FrameworkVersionRange;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModSdkRequirement Sdk;

	/** Required and optional dependencies; FModDependency::bOptional distinguishes them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TArray<FModDependency> Dependencies;

	/** Soft ordering edges: this mod loads before every listed mod that is also present. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TArray<FModId> LoadBefore;

	/** Soft ordering edges: this mod loads after every listed mod that is also present. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TArray<FModId> LoadAfter;

	/** Higher loads earlier among mods the dependency graph leaves unordered. Range [-1000, 1000]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	int32 Priority = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	EModNetworkScope NetworkScope = EModNetworkScope::ClientAndServer;

	/** If true a server running this mod requires connecting clients to run it too. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	bool bRequiredForNetworkPlay = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TArray<FName> RequestedPermissions;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModEntryPoint EntryPoint;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TArray<FModContentRoot> ContentRoots;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TArray<FModResourceClaimDeclaration> Claims;

	/** Free-form author metadata. The framework never interprets these keys. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TMap<FString, FString> Metadata;

	/** Minimal identity check: a usable id and a version other than 0.0.0. */
	bool IsValid() const;

	/** DisplayName when the author supplied one, otherwise the raw id. Never empty for a valid manifest. */
	FString GetDisplayNameOrId() const;

	/** Finds a required or optional dependency by id. Returns nullptr when there is none. */
	const FModDependency* FindDependency(const FModId& InId) const;
};
