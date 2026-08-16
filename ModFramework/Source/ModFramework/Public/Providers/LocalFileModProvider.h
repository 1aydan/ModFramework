// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "HAL/CriticalSection.h"
#include "Internationalization/Text.h"
#include "Providers/ModProvider.h"
#include "UObject/NameTypes.h"

/**
 * The default provider: mods that live in folders on this machine.
 *
 * It scans a list of search directories, in the order they were configured, and reports two kinds of
 * thing found directly inside each of them:
 *
 *   1. an immediate SUBDIRECTORY containing a `mod.json` - an unpacked mod, ready to mount. The
 *      result's RootPath is the subdirectory.
 *   2. a loose `*.mod` FILE - a packaged mod. Its manifest is read straight out of the container with
 *      FModPackageReader::PeekManifest, without extracting anything, so a mod browser can list it
 *      immediately. The result's RootPath is the `.mod` file itself and it carries an Info diagnostic
 *      with the code `Discovery.NotInstalled`: the package has to be installed before its content can
 *      be mounted. AcquireMod does that installation on demand.
 *
 * Determinism: search directories are visited in the configured order (they are never re-sorted), and
 * within one directory the unpacked mods come first in case-insensitive alphabetical order, then the
 * packages in the same order. Two scans of an unchanged filesystem therefore produce byte-identical
 * results, which is what makes the resolved load order reproducible.
 *
 * Duplicate ids: the first mod found wins. The loser is dropped and a Warning with the code
 * `Discovery.Duplicate` naming both paths is appended to the winner's diagnostics, so a player can be
 * told exactly which copy was ignored.
 *
 * Diagnostic codes this provider emits:
 *   Discovery.DirectoryMissing  a search directory does not exist and could not be created
 *   Discovery.Duplicate         two mods with the same id were found; the later one was dropped
 *   Discovery.ManifestInvalid   a `mod.json` (or a package manifest) could not be read or validated
 *   Discovery.InstallFailed     a package could not be verified, extracted or moved into place
 *   Discovery.NotInstalled      the mod exists only as a `.mod` package, or is not installed at all
 *   Discovery.Installed         (Info) a package was installed successfully
 *
 * Thread safety: the configured directories and the discovery cache are guarded by an internal lock,
 * so SetSearchDirectories may be called while a scan runs. The filesystem work itself is not
 * parallelised.
 */
class MODFRAMEWORK_API FLocalFileModProvider : public IModProvider
{
public:
	/**
	 * @param InSearchDirectories Directories to scan, already token-expanded - normally
	 *                            UModFrameworkSettings::GetResolvedSearchDirectories(). Relative
	 *                            paths are made absolute against the process working directory.
	 */
	explicit FLocalFileModProvider(TArray<FString> InSearchDirectories);

	virtual ~FLocalFileModProvider() override = default;

	/** "LocalFile" */
	static FName StaticProviderId();

	//~ Begin IModProvider interface
	virtual FName GetProviderId() const override;
	virtual FText GetDisplayName() const override;
	virtual void DiscoverMods(TArray<FModDiscoveryResult>& OutResults) override;
	virtual bool SupportsAsyncDiscovery() const override;
	virtual bool AcquireMod(const FModId& ModId, FString& OutLocalRootPath, FModDiagnostic& OutError) override;
	virtual bool ReleaseMod(const FModId& ModId) override;
	virtual bool SupportsInstall() const override;
	virtual bool InstallModPackage(const FString& PackagePath, FModId& OutInstalledId, TArray<FModDiagnostic>& OutDiagnostics) override;
	virtual bool UninstallMod(const FModId& ModId, TArray<FModDiagnostic>& OutDiagnostics) override;
	//~ End IModProvider interface

	/** Replaces the search directory list. Also clears the cached install directory. */
	void SetSearchDirectories(TArray<FString> In);

	/** The configured search directories, absolute and normalised, in scan order. */
	TArray<FString> GetSearchDirectories() const;

	/**
	 * Where extracted `.mod` packages are installed. Pass an empty string to go back to the default,
	 * which is the first search directory this process can actually write to (probed lazily, the
	 * first time an install, uninstall or package acquisition needs it).
	 */
	void SetInstallDirectory(const FString& In);

	/**
	 * The directory installs land in, resolving the default if it has not been resolved yet. Returns
	 * an empty string when no search directory is writable.
	 */
	FString GetInstallDirectory() const;

private:
	/** What DiscoverMods remembered about one mod, so AcquireMod can find it again. */
	struct FDiscoveredMod
	{
		/** Mod root directory, or the `.mod` file path when bPackaged is true. */
		FString RootPath;

		/** True when RootPath names a `.mod` container that has not been extracted yet. */
		bool bPackaged = false;
	};

	/** Scans one directory. Assumes Directory is already absolute and normalised. */
	void DiscoverInDirectory(const FString& Directory, TArray<FModDiscoveryResult>& OutResults,
		TMap<FModId, int32>& InOutFirstResultIndexById, TMap<FModId, FDiscoveredMod>& OutDiscovered) const;

	/** Builds a result from an unpacked mod root containing `mod.json`. */
	FModDiscoveryResult ReadUnpackedMod(const FString& ModRootPath) const;

	/** Builds a result from a `.mod` container, reading the manifest without extracting. */
	FModDiscoveryResult ReadPackagedMod(const FString& PackagePath) const;

	/** Resolves the install directory, probing search directories for writability. Takes the lock. */
	FString ResolveInstallDirectory() const;

	/** Absolute, normalised install path of a mod id, or empty when the install directory is unusable. */
	FString MakeInstallPathForId(const FModId& ModId) const;

	/** Directories to scan, in order. Absolute and normalised. */
	TArray<FString> SearchDirectories;

	/** Explicitly configured install directory. Empty means "use the first writable search directory". */
	FString InstallDirectory;

	/** Result of the writability probe. Empty until ResolveInstallDirectory has run. */
	mutable FString CachedInstallDirectory;

	/** Everything the last DiscoverMods call found, keyed by mod id. */
	mutable TMap<FModId, FDiscoveredMod> DiscoveredMods;

	/** Guards SearchDirectories, InstallDirectory, CachedInstallDirectory and DiscoveredMods. */
	mutable FCriticalSection StateLock;
};
