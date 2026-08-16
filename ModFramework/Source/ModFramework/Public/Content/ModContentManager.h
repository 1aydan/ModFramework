// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Content/ModContentTypes.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Registry/ModInfo.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class UClass;

/**
 * Owns every mod content mount in the process.
 *
 * The manager knows nothing about pak files, IoStore containers or loose directories: it resolves
 * and security-checks paths, enforces that virtual mount points are unique across mods, picks a
 * registered IModContentMounter for each content root, and keeps the bookkeeping that lets the rest
 * of the framework answer "which mod owns this asset?".
 *
 * Mounting a mod is all-or-nothing. If any content root of a mod fails, everything already mounted
 * for that mod in the same call is unmounted again before MountMod returns false, so a mod is never
 * left half-present.
 *
 * Threading: this is game-thread only. It registers package mount points and drives the asset
 * registry, neither of which tolerates being called from a worker.
 *
 * A note on how mod paks must be built: the pak's internal file tree is mounted over a directory
 * inside the mod's own folder, never over the game's content directory, so a mod can never shadow
 * base-game assets. That means a mod pak has to be cooked with its content at the pak root (the
 * usual "cook the mod as a plugin, ship the plugin's chunk" layout) and the manifest's `mountPoint`
 * has to be the name the mod was cooked under. When those disagree the mount still succeeds but no
 * packages are discovered, and the framework says so in a diagnostic instead of failing silently.
 */
class MODFRAMEWORK_API FModContentManager
{
public:
	FModContentManager();
	~FModContentManager();

	FModContentManager(const FModContentManager&) = delete;
	FModContentManager& operator=(const FModContentManager&) = delete;

	/** Registers the built-in pak and loose mounters. Idempotent. */
	void Initialize();

	/** Unmounts everything still mounted, then drops the mounters. Idempotent. */
	void Shutdown();

	/** Mounters are consulted in registration order; the first one that claims a root wins. */
	void RegisterMounter(TSharedRef<IModContentMounter> InMounter);
	void UnregisterMounter(FName MounterId);

	/** Mounts every content root of a mod. All-or-nothing: rolls back on partial failure. */
	bool MountMod(const FModInfo& ModInfo, TArray<FModContentMount>& OutMounts, TArray<FModDiagnostic>& OutDiagnostics);

	/**
	 * Releases every mount of a mod. Returns true when nothing was left behind, including the case
	 * where the mod was not mounted at all.
	 */
	bool UnmountMod(const FModId& ModId, TArray<FModDiagnostic>& OutDiagnostics);

	bool IsModMounted(const FModId& ModId) const;

	/** Every live mount, ordered by owning mod id then mount point, so output is reproducible. */
	TArray<FModContentMount> GetMounts() const;
	TArray<FModContentMount> GetMountsForMod(const FModId& ModId) const;

	/**
	 * Which mod owns a given /VirtualRoot/... package name or object path. Invalid id when it is
	 * base game content. The longest registered mount point that prefixes the path wins, so nested
	 * roots resolve to the most specific mod.
	 */
	FModId FindOwningMod(const FString& LongPackageNameOrObjectPath) const;

	/** Asset queries scoped to a mod's mounts (asset registry backed). */
	TArray<FAssetData> GetModAssets(const FModId& ModId, UClass* ClassFilter = nullptr, bool bRecursiveClasses = true) const;
	TArray<FAssetData> GetAllModAssets(UClass* ClassFilter = nullptr, bool bRecursiveClasses = true) const;

	/**
	 * Normalises "/MyMod" / "MyMod" / "/MyMod/" / "\MyMod\" into "/MyMod/".
	 * Returns an empty string when there is nothing left after trimming separators; the caller is
	 * expected to reject that rather than mount it.
	 */
	static FString NormalizeMountPoint(const FString& In);

	/**
	 * Default mount point for a mod when the manifest does not specify one: "/" + the mod root
	 * folder name, sanitised into something that can legally be a package root. Falls back to the
	 * mod id, and then to "/Mod/", when the folder name has no usable characters.
	 */
	static FString MakeDefaultMountPoint(const FModId& ModId, const FString& ModRootPath);

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnModContentMountsChanged, const FModId&);

	/** Raised after every content root of a mod mounted successfully. */
	FOnModContentMountsChanged OnMounted;

	/** Raised after a mod's mounts were released, even if some of them failed to release cleanly. */
	FOnModContentMountsChanged OnUnmounted;

private:
	/**
	 * Resolves, security-checks and mounts one content root of one mod.
	 *
	 * Appends every diagnostic it produces - including the successful-but-empty case - and leaves
	 * nothing behind when it returns false.
	 */
	bool MountContentRoot(const FModId& ModId, const FString& ModRootAbsolute, const FModContentRoot& RawRoot,
		int32 RootIndex, FModContentMount& OutMount, TArray<FModDiagnostic>& OutDiagnostics);

	/** Finds the mounter that claims a root, or nullptr. */
	IModContentMounter* FindMounterFor(const FModContentRoot& Root, const FString& AbsolutePath) const;

	/** Finds a mounter by the id recorded in a live mount, or nullptr. */
	IModContentMounter* FindMounterById(FName MounterId) const;

	/** Releases mounts made during a failed MountMod, newest first. Appends failures to Diagnostics. */
	void RollbackMounts(TArray<FModContentMount>& InOutMounts, TArray<FModDiagnostic>& OutDiagnostics);

	/** Consulted in order; the first mounter whose CanMount returns true handles the root. */
	TArray<TSharedRef<IModContentMounter>> Mounters;

	/** Live mounts per mod, in manifest order. */
	TMap<FModId, TArray<FModContentMount>> ModMounts;

	/** Virtual mount point -> owning mod, so collisions are caught before anything is mounted. */
	TMap<FString, FModId> MountPointOwners;

	bool bInitialized = false;
};
