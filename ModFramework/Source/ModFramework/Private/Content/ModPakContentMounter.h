// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Content/ModContentTypes.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "UObject/NameTypes.h"

class FPakPlatformFile;
struct FModContentRoot;

/**
 * Mounts mod content shipped as Unreal pak files.
 *
 * Claims both EModContentRootType::Pak and EModContentRootType::IoStore so that an IoStore-typed
 * content root produces a precise "not supported in this build" diagnostic instead of the useless
 * "no mounter for this content root". See ModPakContentMounter.cpp for exactly why a standalone
 * .utoc/.ucas pair cannot be mounted at runtime in 5.8.
 *
 * A content root may name either a single .pak file or a directory, in which case every *.pak
 * directly inside it is mounted in sorted order under the same virtual root.
 *
 * Security: the pak's internal file tree is always mounted over a directory inside the mod's own
 * folder, overriding whatever mount point the pak recorded at cook time. Without that override a
 * mod cooked with the mount point "../../../TheGame/Content/" would silently replace base game
 * assets, which is a content-injection attack, not a mod.
 */
class FModPakContentMounter : public IModContentMounter
{
public:
	FModPakContentMounter();
	virtual ~FModPakContentMounter() override;

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
	/** Everything needed to reverse one mount exactly. */
	struct FPakMountRecord
	{
		/** Normalised "/MyMod/". */
		FString VirtualMountPoint;

		/** Absolute directory the paks were mounted over, with a trailing slash. */
		FString PhysicalRoot;

		/**
		 * The pak paths in the order they were mounted, byte-for-byte as handed to
		 * FPakPlatformFile::Mount - it matches mounted paks by exact string equality, so a
		 * differently normalised copy of the same path would not unmount anything.
		 */
		TArray<FString> PakFilenames;
	};

	/**
	 * Returns the pak layer, installing one over the current platform file when this build allows
	 * it. Fails with a developer-facing explanation in editor builds, where the pak layer is
	 * deliberately absent and loose content mounts are the supported path.
	 */
	FPakPlatformFile* AcquirePakPlatformFile(FString& OutError);

	/** Resolves a content root to the list of pak files it refers to, sorted for determinism. */
	static bool CollectPakFiles(const FString& AbsolutePath, TArray<FString>& OutPakFiles, FString& OutError);

	/** Unmounts the given paks, newest first. Returns the number that refused to unmount. */
	static int32 UnmountPakFiles(FPakPlatformFile& PakPlatformFile, const TArray<FString>& PakFilenames);

	/** Live mounts keyed by virtual mount point. */
	TMap<FString, FPakMountRecord> Records;

	/**
	 * Set only when this mounter had to create the pak layer itself because the build started
	 * without one. It is inserted into the platform file chain and is intentionally never deleted:
	 * tearing a platform file wrapper out of the chain while the engine is running is far more
	 * dangerous than leaking one small object for the lifetime of the process.
	 */
	FPakPlatformFile* OwnedPakPlatformFile = nullptr;
};
