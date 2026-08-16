// Copyright (c) 2026. Licensed for use in your own projects.

#include "Content/ModLooseContentMounter.h"

#include "Containers/UnrealString.h"
#include "Content/ModContentManager.h"
#include "Core/ModFrameworkLog.h"
#include "HAL/FileManager.h"
#include "Manifest/ModManifest.h"
#include "Misc/Paths.h"
#include "Settings/ModFrameworkSettings.h"
#include "Templates/UnrealTemplate.h"

namespace
{
	/** Ensures a directory path ends in exactly one separator, which is what mount points want. */
	FString MakeLooseDirectoryPath(const FString& In)
	{
		FString Result = In;
		FPaths::NormalizeDirectoryName(Result);

		// NormalizeDirectoryName keeps the separator after a drive letter ("F:/"), so test before
		// appending rather than assuming there is none.
		if (!Result.EndsWith(TEXT("/")))
		{
			Result.AppendChar(TEXT('/'));
		}

		return Result;
	}
}

FModLooseContentMounter::FModLooseContentMounter() = default;

FModLooseContentMounter::~FModLooseContentMounter()
{
	if (Records.Num() > 0)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("The loose content mounter is being destroyed with %d mount(s) still live; they are being released now."),
			Records.Num());

		for (const TPair<FString, FLooseMountRecord>& Pair : Records)
		{
			ModContentMounting::ForgetMountPointInAssetRegistry(Pair.Value.VirtualMountPoint);
			ModContentMounting::UnregisterPackageMountPoint(Pair.Value.VirtualMountPoint, Pair.Value.PhysicalRoot);
		}

		Records.Reset();
	}
}

FName FModLooseContentMounter::StaticMounterId()
{
	return FName(TEXT("Loose"));
}

FName FModLooseContentMounter::GetMounterId() const
{
	return StaticMounterId();
}

bool FModLooseContentMounter::AreLooseMountsAllowed()
{
	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();

	// No settings object means the class default object does not exist yet, which only happens
	// extremely early in startup. Refusing is the safe answer: a development convenience is never
	// worth guessing about.
	return Settings != nullptr && Settings->bAllowLooseContentMounts;
}

bool FModLooseContentMounter::CanMount(const FModContentRoot& Root, const FString& AbsolutePath) const
{
	// Claimed on type alone, deliberately. Neither the bAllowLooseContentMounts switch nor "is that
	// path actually a directory" is tested here: refusing would surface as Content.NoMounter, which
	// tells a mod author nothing. Mount answers both with a precise Content.LooseNotAllowed or
	// Content.RootMissing instead.
	(void)AbsolutePath;
	return Root.Type == EModContentRootType::LooseDirectory;
}

bool FModLooseContentMounter::Mount(const FModId& ModId, const FModContentRoot& Root, const FString& AbsolutePath,
	FModContentMount& OutMount, FModDiagnostic& OutError)
{
	OutMount = FModContentMount();

	if (!AreLooseMountsAllowed())
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.LooseNotAllowed")),
			FString::Printf(TEXT("'%s' asks for a loose content mount, but loose mounts are disabled. They are a ")
				TEXT("development-only path: loose files can be edited after the mod was validated, so a shipping ")
				TEXT("build should leave them off. Enable Project Settings > Plugins > Mod Framework > Allow Loose ")
				TEXT("Content Mounts to use them while iterating."),
				*ModId.ToString()),
			AbsolutePath);
		return false;
	}

	if (!IFileManager::Get().DirectoryExists(*AbsolutePath))
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.RootMissing")),
			FString::Printf(TEXT("'%s' is not a directory."), *AbsolutePath), AbsolutePath);
		return false;
	}

	const FString MountPoint = FModContentManager::NormalizeMountPoint(Root.MountPoint);
	FString MountPointError;
	if (!ModContentMounting::ValidateVirtualMountPoint(MountPoint, MountPointError))
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.MountFailed")), MoveTemp(MountPointError), AbsolutePath);
		return false;
	}

	if (Records.Contains(MountPoint))
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.MountPointConflict")),
			FString::Printf(TEXT("'%s' is already mounted by this mounter."), *MountPoint), AbsolutePath);
		return false;
	}

	const FString PhysicalRoot = MakeLooseDirectoryPath(AbsolutePath);

	FString RegisterError;
	if (!ModContentMounting::RegisterPackageMountPoint(MountPoint, PhysicalRoot, RegisterError))
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.MountFailed")), MoveTemp(RegisterError), AbsolutePath);
		return false;
	}

	FLooseMountRecord Record;
	Record.VirtualMountPoint = MountPoint;
	Record.PhysicalRoot = PhysicalRoot;
	Records.Add(MountPoint, MoveTemp(Record));

	OutMount.OwnerModId = ModId;
	OutMount.VirtualMountPoint = MountPoint;
	OutMount.PhysicalPath = PhysicalRoot;
	OutMount.Type = EModContentRootType::LooseDirectory;
	OutMount.MountOrder = Root.MountOrder;
	OutMount.bMounted = true;
	OutMount.MounterId = StaticMounterId();
	ModContentMounting::ScanMountPointIntoAssetRegistry(MountPoint, OutMount.DiscoveredPackages);

	// Said on every single loose mount on purpose. A project that ships with this switch on has a
	// mod loading path with no integrity guarantees at all, and the log is where that gets noticed.
	UE_LOG(LogModFramework, Warning,
		TEXT("Loose content mount (DEVELOPMENT ONLY): %s -> %s for '%s', %d package(s) discovered. Loose mod content ")
		TEXT("bypasses packaging and can be modified after validation; disable Allow Loose Content Mounts before ")
		TEXT("shipping."),
		*MountPoint, *PhysicalRoot, *ModId.ToString(), OutMount.DiscoveredPackages.Num());

	return true;
}

bool FModLooseContentMounter::Unmount(const FModContentMount& Mount, FModDiagnostic& OutError)
{
	if (Mount.VirtualMountPoint.IsEmpty())
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.UnmountFailed")),
			TEXT("The mount record carries no virtual mount point, so there is nothing to release."),
			Mount.PhysicalPath);
		return false;
	}

	FLooseMountRecord Record;
	if (!Records.RemoveAndCopyValue(Mount.VirtualMountPoint, Record))
	{
		// Release what the mount claims anyway, so a stale package root can never survive.
		ModContentMounting::ForgetMountPointInAssetRegistry(Mount.VirtualMountPoint);
		ModContentMounting::UnregisterPackageMountPoint(Mount.VirtualMountPoint, Mount.PhysicalPath);

		UE_LOG(LogModFramework, Warning,
			TEXT("No loose mount is recorded for '%s'; released its package root anyway."), *Mount.VirtualMountPoint);
		return true;
	}

	ModContentMounting::ForgetMountPointInAssetRegistry(Record.VirtualMountPoint);
	ModContentMounting::UnregisterPackageMountPoint(Record.VirtualMountPoint, Record.PhysicalRoot);

	UE_LOG(LogModFramework, Log, TEXT("Unmounted loose content at %s."), *Record.VirtualMountPoint);
	return true;
}
