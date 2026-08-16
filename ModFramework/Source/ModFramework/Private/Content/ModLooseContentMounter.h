// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Content/ModContentTypes.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "UObject/NameTypes.h"

struct FModContentRoot;

/**
 * Mounts mod content that lives as loose, uncooked-or-cooked files in a directory.
 *
 * This is a development path and nothing else. It exists so a mod author can iterate in the editor
 * without repacking, and so automated tests can exercise the mounting pipeline without producing a
 * pak. It refuses to do anything unless UModFrameworkSettings::bAllowLooseContentMounts is set, and
 * it says loudly in the log that it is a development-only route every time it mounts something.
 *
 * Shipping games leave the setting off: loose content bypasses every integrity guarantee a packaged
 * mod has, since the files can be edited after the mod was validated.
 */
class FModLooseContentMounter : public IModContentMounter
{
public:
	FModLooseContentMounter();
	virtual ~FModLooseContentMounter() override;

	/** The value recorded in FModContentMount::MounterId. */
	static FName StaticMounterId();

	//~ Begin IModContentMounter interface
	virtual FName GetMounterId() const override;
	virtual bool CanMount(const FModContentRoot& Root, const FString& AbsolutePath) const override;
	virtual bool Mount(const FModId& ModId, const FModContentRoot& Root, const FString& AbsolutePath,
		FModContentMount& OutMount, FModDiagnostic& OutError) override;
	virtual bool Unmount(const FModContentMount& Mount, FModDiagnostic& OutError) override;
	//~ End IModContentMounter interface

private:
	/** Whether the project currently permits loose mounts. Missing settings mean "no". */
	static bool AreLooseMountsAllowed();

	/** Everything needed to reverse one mount exactly. */
	struct FLooseMountRecord
	{
		/** Normalised "/MyMod/". */
		FString VirtualMountPoint;

		/** Absolute directory the virtual root maps onto, with a trailing slash. */
		FString PhysicalRoot;
	};

	/** Live mounts keyed by virtual mount point. */
	TMap<FString, FLooseMountRecord> Records;
};
