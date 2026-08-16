// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Internationalization/Text.h"
#include "Manifest/ModManifest.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "ModProvider.generated.h"

/**
 * One thing a provider found while scanning for mods.
 *
 * A result is emitted for everything that *looks* like a mod, including the ones that turned out to
 * be broken: a game wants to tell a player "BetterCombat could not be read" rather than silently show
 * a shorter list. `bManifestValid` is the flag that separates the two, and Diagnostics always carries
 * the reason when it is false.
 *
 * A provider may also report a problem that is not about any particular mod - a search directory it
 * could not create, say. Those come back as a result with an empty ManifestPath, a default (invalid)
 * Manifest, bManifestValid false, and a diagnostic explaining what happened. Callers should treat any
 * result whose Manifest has no id as a report to surface rather than a mod to register.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModDiscoveryResult
{
	GENERATED_BODY()

	/** Which provider produced this result, e.g. FLocalFileModProvider::StaticProviderId(). */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName ProviderId;

	/**
	 * Absolute path of the mod root: the folder containing `mod.json` for an unpacked mod, and the
	 * `.mod` file itself for a mod that has been discovered but not yet installed. In the second
	 * case the result also carries an Info diagnostic with the code `Discovery.NotInstalled`, and
	 * the mod must be installed (IModProvider::InstallModPackage, or implicitly through
	 * IModProvider::AcquireMod) before its content can be mounted.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString RootPath;

	/**
	 * Absolute path of the manifest. For an unpacked mod that is `<RootPath>/mod.json`; for a `.mod`
	 * package it is the package path, because the manifest lives inside the container.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString ManifestPath;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModManifest Manifest;

	/** True only when the manifest parsed, validated and carries a usable id and version. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bManifestValid = false;

	/** Everything the provider has to say about this result, valid or not. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModDiagnostic> Diagnostics;
};

/** Completion callback for IModProvider::DiscoverModsAsync. */
DECLARE_DELEGATE_OneParam(FOnModDiscoveryComplete, const TArray<FModDiscoveryResult>&);

/**
 * A source of mods.
 *
 * The framework never knows where a mod came from. A provider turns "somewhere" - a folder on disk, a
 * workshop subscription, a download cache, a test fixture - into a list of FModDiscoveryResult and,
 * on request, into a directory on the local filesystem that the content layer can mount.
 *
 * Contract for implementers:
 *  - GetProviderId must be stable across runs; it is recorded in FModInfo::ProviderId and used to
 *    route AcquireMod back to the right provider.
 *  - DiscoverMods must be deterministic. Two runs over an unchanged filesystem must produce the same
 *    results in the same order, or load order stops being reproducible.
 *  - Nothing a provider reads is trusted. Never assert on file contents; report an FModDiagnostic.
 *  - DiscoverMods is called on the game thread and may block. Implement DiscoverModsAsync (and return
 *    true from SupportsAsyncDiscovery) when the source is slow or networked; the default
 *    implementation just runs the synchronous path and fires the delegate immediately.
 */
class MODFRAMEWORK_API IModProvider
{
public:
	virtual ~IModProvider() = default;

	/** Stable identifier of this provider. */
	virtual FName GetProviderId() const = 0;

	/** Localizable name for settings screens and mod browsers. */
	virtual FText GetDisplayName() const = 0;

	/** Appends everything this provider can see. Must not clear OutResults. */
	virtual void DiscoverMods(TArray<FModDiscoveryResult>& OutResults) = 0;

	/** True when DiscoverModsAsync does real asynchronous work rather than forwarding to DiscoverMods. */
	virtual bool SupportsAsyncDiscovery() const { return false; }

	/** Asynchronous discovery. The default runs the synchronous path and completes inline. */
	virtual void DiscoverModsAsync(FOnModDiscoveryComplete OnComplete)
	{
		TArray<FModDiscoveryResult> Results;
		DiscoverMods(Results);
		OnComplete.ExecuteIfBound(Results);
	}

	/**
	 * Makes the mod available on the local filesystem and reports where it ended up.
	 * A provider that already works against local files simply returns the discovered path.
	 */
	virtual bool AcquireMod(const FModId& ModId, FString& OutLocalRootPath, FModDiagnostic& OutError) = 0;

	/** Releases anything AcquireMod reserved. Providers that copy nothing can return true. */
	virtual bool ReleaseMod(const FModId& ModId) { return true; }

	/** True when this provider owns a directory it can install packages into. */
	virtual bool SupportsInstall() const { return false; }

	/** Installs a `.mod` package. Only providers that own a mod directory implement this. */
	virtual bool InstallModPackage(const FString& PackagePath, FModId& OutInstalledId, TArray<FModDiagnostic>& OutDiagnostics) { return false; }

	/** Removes a previously installed mod. Only providers that own a mod directory implement this. */
	virtual bool UninstallMod(const FModId& ModId, TArray<FModDiagnostic>& OutDiagnostics) { return false; }
};
