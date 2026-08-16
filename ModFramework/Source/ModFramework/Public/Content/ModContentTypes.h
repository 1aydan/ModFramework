// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Manifest/ModManifest.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "ModContentTypes.generated.h"

/**
 * One live content mount belonging to one mod.
 *
 * A mount is the pairing of a *virtual* package root (`/MyMod/`, which is what package names and
 * object paths inside the mod are written against) with a *physical* location on disk (a directory,
 * or the directory a pak file was mounted over). Everything above this layer addresses mod content
 * exclusively through VirtualMountPoint; only IModContentMounter implementations ever care which
 * packaging technology is behind it.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModContentMount
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId OwnerModId;

	/** Virtual package root, always of the form "/Something/" (leading and trailing slash). */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString VirtualMountPoint;

	/**
	 * Absolute path the virtual root maps onto.
	 *
	 * For a loose mount this is the mod's content directory. For a pak mount this is the directory
	 * the pak's internal file tree was mounted over, which is always inside the mod root - a mod pak
	 * is never allowed to mount over the game's own content directory.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString PhysicalPath;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModContentRootType Type = EModContentRootType::Pak;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	int32 MountOrder = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bMounted = false;

	/** Which IModContentMounter owns this mount; used to route the matching Unmount call. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName MounterId;

	/** Long package names discovered under this mount. Empty when the asset registry was unavailable. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FName> DiscoveredPackages;

	/** Diagnostic one-liner, e.g. "/MyMod/ <- F:/Mods/MyMod/Content (Pak, order 0, 12 packages)". */
	FString ToString() const
	{
		return FString::Printf(TEXT("%s <- %s (%s, order %d, %d packages%s)"),
			*VirtualMountPoint,
			*PhysicalPath,
			*ModFrameworkEnums::ToString(Type),
			MountOrder,
			DiscoveredPackages.Num(),
			bMounted ? TEXT("") : TEXT(", NOT MOUNTED"));
	}
};

/**
 * Packaging-technology abstraction. Nothing above this layer may mention Pak or IoStore.
 *
 * A mounter is responsible for the *whole* mount: making the bytes reachable through the platform
 * file layer (if that is even needed), registering the virtual package root with FPackageName, and
 * populating the asset registry. Unmount must undo exactly what Mount did.
 *
 * Implementations are handed paths that have already been resolved to absolute form and verified to
 * live inside the mod root, but everything else about `Root` came out of a mod's manifest and is
 * therefore untrusted: validate it, never assert on it, and report problems through OutError.
 */
class MODFRAMEWORK_API IModContentMounter
{
public:
	virtual ~IModContentMounter() = default;

	/** Stable identifier recorded in FModContentMount::MounterId, e.g. "Pak" or "Loose". */
	virtual FName GetMounterId() const = 0;

	/**
	 * Whether this mounter handles the given content root. Called before Mount, in registration
	 * order; the first mounter that claims a root gets it.
	 *
	 * Answer on the EModContentRootType alone wherever you can. Everything else - the file being
	 * missing, the wrong shape, or a project policy forbidding this kind of mount - belongs in
	 * Mount, because refusing here collapses all of it into a single useless "no mounter handles
	 * this root" diagnostic. Claim the root, then explain precisely why it did not work.
	 */
	virtual bool CanMount(const FModContentRoot& Root, const FString& AbsolutePath) const = 0;

	/**
	 * Mounts one content root. Returns false and fills OutError on any failure, having already
	 * undone whatever it partially did.
	 *
	 * OutMount must be fully populated on success, including MounterId and bMounted, because it is
	 * the only thing handed back to Unmount later.
	 */
	virtual bool Mount(const FModId& ModId, const FModContentRoot& Root, const FString& AbsolutePath,
		FModContentMount& OutMount, FModDiagnostic& OutError) = 0;

	/**
	 * Reverses Mount. Returns false and fills OutError when something could not be released.
	 *
	 * Unmounting content whose objects are still referenced by the running game is unsafe: the
	 * framework never force-collects garbage to make it safe, it only warns.
	 */
	virtual bool Unmount(const FModContentMount& Mount, FModDiagnostic& OutError) = 0;
};

/**
 * Shared plumbing every IModContentMounter needs, so a third-party mounter (a network-backed one,
 * an encrypted-archive one, ...) does not have to re-derive the package-root and asset-registry
 * rules. These operate on *already normalised* mount points - run the string through
 * FModContentManager::NormalizeMountPoint first.
 */
namespace ModContentMounting
{
	/**
	 * Checks that a normalised "/Root/" string can legally become a package root: a single path
	 * segment starting with a letter or underscore, made only of letters, digits and underscores,
	 * and not one of the roots the engine reserves. Fills OutError with the reason on failure.
	 */
	MODFRAMEWORK_API bool ValidateVirtualMountPoint(const FString& InNormalizedMountPoint, FString& OutError);

	/**
	 * Maps the virtual root onto an absolute directory via FPackageName::RegisterMountPoint.
	 * Returns false with a reason when the engine refuses the pair.
	 */
	MODFRAMEWORK_API bool RegisterPackageMountPoint(const FString& InNormalizedMountPoint,
		const FString& InAbsoluteContentPath, FString& OutError);

	/** Reverses RegisterPackageMountPoint. Safe to call for a root that was never registered. */
	MODFRAMEWORK_API void UnregisterPackageMountPoint(const FString& InNormalizedMountPoint,
		const FString& InAbsoluteContentPath);

	/**
	 * Scans the virtual root into the asset registry and reports the long package names found.
	 * Returns the number of packages discovered, or INDEX_NONE when the asset registry is not
	 * available yet (which is not fatal: the mount itself still works, the registry just has not
	 * caught up).
	 */
	MODFRAMEWORK_API int32 ScanMountPointIntoAssetRegistry(const FString& InNormalizedMountPoint,
		TArray<FName>& OutDiscoveredPackages);

	/**
	 * Best-effort removal of a virtual root from the asset registry's cached path list.
	 *
	 * The engine offers no public "forget every asset under this path" call - IAssetRegistry only
	 * lets a path be removed once it holds no assets - so this frequently does nothing and never
	 * reports failure. Stale FAssetData for unmounted mod content is a known, logged limitation.
	 */
	MODFRAMEWORK_API void ForgetMountPointInAssetRegistry(const FString& InNormalizedMountPoint);
}
