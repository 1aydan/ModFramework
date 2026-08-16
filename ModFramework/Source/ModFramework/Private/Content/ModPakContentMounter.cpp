// Copyright (c) 2026. Licensed for use in your own projects.

#include "Content/ModPakContentMounter.h"

#include "Containers/UnrealString.h"
#include "Content/ModContentManager.h"
#include "Core/ModFrameworkLog.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IPlatformFilePak.h"
#include "Manifest/ModManifest.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CString.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Templates/UnrealTemplate.h"

namespace
{
	/**
	 * Why an IoStore-typed content root cannot be mounted on its own in 5.8.
	 *
	 * Verified against Engine/Source/Runtime:
	 *   - FIoStoreEnvironment is marked UE_DEPRECATED(5.8) in Core/Public/IO/IoDispatcher.h and is
	 *     no longer used to mount anything.
	 *   - FIoDispatcher::Mount takes a TSharedRef<IIoDispatcherBackend>, i.e. a whole storage
	 *     backend, not a container path. There is no public "mount this .utoc" entry point on it.
	 *   - The only code that opens a .utoc at runtime is UE::IoStore::IFileIoDispatcherBackend,
	 *     declared in Runtime/PakFile/Private/FileIoDispatcherBackend.h. Being private to the
	 *     PakFile module, it is unreachable from any other module.
	 *   - There is no IoStoreOnDemand plugin in this engine install.
	 *
	 * The supported route is the one FPakPlatformFile::Mount already implements: when a .pak is
	 * mounted, IPlatformFilePak.cpp looks for a sibling file with the .utoc extension and mounts the
	 * container and its package store entries alongside it. So an IoStore mod works perfectly - it
	 * just has to ship its .pak next to the .utoc/.ucas and declare the root as "Pak".
	 */
	const TCHAR* const GIoStoreUnsupportedMessage =
		TEXT("This build has no public runtime API for mounting a standalone IoStore container. Ship the .pak that ")
		TEXT("was cooked alongside the .utoc/.ucas and declare the content root as \"Pak\": mounting the pak mounts ")
		TEXT("the matching container automatically.");

	/** Ensures a directory path ends in exactly one separator, which is what pak mount points want. */
	FString MakeDirectoryPath(const FString& In)
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

FModPakContentMounter::FModPakContentMounter() = default;

FModPakContentMounter::~FModPakContentMounter()
{
	if (Records.Num() > 0)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("The pak content mounter is being destroyed with %d mount(s) still live; they are being released now."),
			Records.Num());

		FPakPlatformFile* PakPlatformFile =
			static_cast<FPakPlatformFile*>(FPlatformFileManager::Get().FindPlatformFile(FPakPlatformFile::GetTypeName()));

		for (const TPair<FString, FPakMountRecord>& Pair : Records)
		{
			ModContentMounting::ForgetMountPointInAssetRegistry(Pair.Value.VirtualMountPoint);
			ModContentMounting::UnregisterPackageMountPoint(Pair.Value.VirtualMountPoint, Pair.Value.PhysicalRoot);

			if (PakPlatformFile)
			{
				UnmountPakFiles(*PakPlatformFile, Pair.Value.PakFilenames);
			}
		}

		Records.Reset();
	}

	// OwnedPakPlatformFile is deliberately not deleted. If this mounter installed the pak layer it
	// is now part of the platform file chain and the engine may be holding handles through it;
	// removing it here would be far riskier than leaking one object at process teardown.
	OwnedPakPlatformFile = nullptr;
}

FName FModPakContentMounter::StaticMounterId()
{
	return FName(TEXT("Pak"));
}

FName FModPakContentMounter::GetMounterId() const
{
	return StaticMounterId();
}

bool FModPakContentMounter::CanMount(const FModContentRoot& Root, const FString& AbsolutePath) const
{
	// Claimed on type alone, deliberately. IoStore is claimed even though it cannot be mounted, and
	// a Pak root is claimed before checking that the path really is a pak: refusing either would
	// surface as Content.NoMounter, which tells a mod author nothing. Mount answers with a precise
	// Content.IoStoreUnsupported or Content.RootMissing instead.
	(void)AbsolutePath;
	return Root.Type == EModContentRootType::Pak || Root.Type == EModContentRootType::IoStore;
}

bool FModPakContentMounter::Mount(const FModId& ModId, const FModContentRoot& Root, const FString& AbsolutePath,
	FModContentMount& OutMount, FModDiagnostic& OutError)
{
	OutMount = FModContentMount();

	if (Root.Type == EModContentRootType::IoStore)
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.IoStoreUnsupported")), GIoStoreUnsupportedMessage,
			AbsolutePath);
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

	TArray<FString> PakFilenames;
	FString CollectError;
	if (!CollectPakFiles(AbsolutePath, PakFilenames, CollectError))
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.RootMissing")), MoveTemp(CollectError), AbsolutePath);
		return false;
	}

	FString AcquireError;
	FPakPlatformFile* PakPlatformFile = AcquirePakPlatformFile(AcquireError);
	if (!PakPlatformFile)
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.MountFailed")), MoveTemp(AcquireError), AbsolutePath);
		return false;
	}

	// The pak's file tree is mounted over the directory that holds it, inside the mod's own folder.
	// Overriding the mount point the pak recorded at cook time is what stops a mod from mounting
	// itself over the game's content directory and replacing base game assets.
	const FString PhysicalRoot = IFileManager::Get().DirectoryExists(*AbsolutePath)
		? MakeDirectoryPath(AbsolutePath)
		: MakeDirectoryPath(FPaths::GetPath(AbsolutePath));

	const uint32 PakOrder = static_cast<uint32>(FMath::Max(Root.MountOrder, 0));

	TArray<FString> MountedPaks;
	MountedPaks.Reserve(PakFilenames.Num());

	for (const FString& PakFilename : PakFilenames)
	{
		TArray<FString> AlreadyMounted;
		PakPlatformFile->GetMountedPakFilenames(AlreadyMounted);

		bool bDuplicate = false;
		for (const FString& Existing : AlreadyMounted)
		{
			if (FPaths::IsSamePath(Existing, PakFilename))
			{
				bDuplicate = true;
				break;
			}
		}

		if (bDuplicate)
		{
			UnmountPakFiles(*PakPlatformFile, MountedPaks);
			OutError = FModDiagnostic::Error(FName(TEXT("Content.MountFailed")),
				FString::Printf(TEXT("'%s' is already mounted; the same pak cannot be mounted twice."), *PakFilename),
				AbsolutePath);
			return false;
		}

		if (!PakPlatformFile->Mount(*PakFilename, PakOrder, *PhysicalRoot))
		{
			UnmountPakFiles(*PakPlatformFile, MountedPaks);
			OutError = FModDiagnostic::Error(FName(TEXT("Content.MountFailed")),
				FString::Printf(TEXT("The engine refused to mount '%s'. It is missing, corrupt, signed with a key ")
					TEXT("this build does not have, or encrypted."),
					*PakFilename),
				AbsolutePath);
			return false;
		}

		MountedPaks.Add(PakFilename);
		UE_LOG(LogModFramework, Verbose, TEXT("Mounted pak '%s' over '%s' at read order %u."),
			*PakFilename, *PhysicalRoot, PakOrder);
	}

	FString RegisterError;
	if (!ModContentMounting::RegisterPackageMountPoint(MountPoint, PhysicalRoot, RegisterError))
	{
		UnmountPakFiles(*PakPlatformFile, MountedPaks);
		OutError = FModDiagnostic::Error(FName(TEXT("Content.MountFailed")), MoveTemp(RegisterError), AbsolutePath);
		return false;
	}

	FPakMountRecord Record;
	Record.VirtualMountPoint = MountPoint;
	Record.PhysicalRoot = PhysicalRoot;
	Record.PakFilenames = MountedPaks;
	Records.Add(MountPoint, MoveTemp(Record));

	OutMount.OwnerModId = ModId;
	OutMount.VirtualMountPoint = MountPoint;
	OutMount.PhysicalPath = PhysicalRoot;
	OutMount.Type = EModContentRootType::Pak;
	OutMount.MountOrder = static_cast<int32>(PakOrder);
	OutMount.bMounted = true;
	OutMount.MounterId = StaticMounterId();
	ModContentMounting::ScanMountPointIntoAssetRegistry(MountPoint, OutMount.DiscoveredPackages);

	UE_LOG(LogModFramework, Log, TEXT("Mounted %d pak(s) for '%s' at %s (%d package(s) discovered)."),
		MountedPaks.Num(), *ModId.ToString(), *MountPoint, OutMount.DiscoveredPackages.Num());

	return true;
}

bool FModPakContentMounter::Unmount(const FModContentMount& Mount, FModDiagnostic& OutError)
{
	if (Mount.VirtualMountPoint.IsEmpty())
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.UnmountFailed")),
			TEXT("The mount record carries no virtual mount point, so there is nothing to release."),
			Mount.PhysicalPath);
		return false;
	}

	FPakMountRecord Record;
	if (!Records.RemoveAndCopyValue(Mount.VirtualMountPoint, Record))
	{
		// Nothing recorded: either it was never mounted through this mounter, or it already came
		// apart. Release whatever the mount claims anyway so no stale package root is left behind.
		ModContentMounting::ForgetMountPointInAssetRegistry(Mount.VirtualMountPoint);
		ModContentMounting::UnregisterPackageMountPoint(Mount.VirtualMountPoint, Mount.PhysicalPath);

		UE_LOG(LogModFramework, Warning,
			TEXT("No pak mount is recorded for '%s'; released its package root anyway."), *Mount.VirtualMountPoint);
		return true;
	}

	ModContentMounting::ForgetMountPointInAssetRegistry(Record.VirtualMountPoint);
	ModContentMounting::UnregisterPackageMountPoint(Record.VirtualMountPoint, Record.PhysicalRoot);

	FPakPlatformFile* PakPlatformFile =
		static_cast<FPakPlatformFile*>(FPlatformFileManager::Get().FindPlatformFile(FPakPlatformFile::GetTypeName()));

	if (!PakPlatformFile)
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.UnmountFailed")),
			FString::Printf(TEXT("The pak layer is gone, so the %d pak(s) behind '%s' could not be released."),
				Record.PakFilenames.Num(), *Record.VirtualMountPoint),
			Record.PhysicalRoot);
		return false;
	}

	const int32 FailureCount = UnmountPakFiles(*PakPlatformFile, Record.PakFilenames);
	if (FailureCount > 0)
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Content.UnmountFailed")),
			FString::Printf(TEXT("%d of the %d pak(s) behind '%s' refused to unmount."),
				FailureCount, Record.PakFilenames.Num(), *Record.VirtualMountPoint),
			Record.PhysicalRoot);
		return false;
	}

	UE_LOG(LogModFramework, Log, TEXT("Unmounted %d pak(s) at %s."),
		Record.PakFilenames.Num(), *Record.VirtualMountPoint);

	return true;
}

FPakPlatformFile* FModPakContentMounter::AcquirePakPlatformFile(FString& OutError)
{
	OutError.Reset();

	if (FPakPlatformFile* Existing =
		static_cast<FPakPlatformFile*>(FPlatformFileManager::Get().FindPlatformFile(FPakPlatformFile::GetTypeName())))
	{
		return Existing;
	}

	if (OwnedPakPlatformFile)
	{
		// Installed by us earlier but somehow no longer findable in the chain. Refuse rather than
		// install a second one on top.
		OutError = TEXT("The pak layer this process installed is no longer part of the platform file chain.");
		return nullptr;
	}

#if WITH_EDITOR
	OutError =
		TEXT("There is no pak layer in an editor build, and installing one under a running editor would change how ")
		TEXT("every file in the project is resolved. Mount this mod's content as a loose directory instead: set the ")
		TEXT("content root type to \"LooseDirectory\" and enable Project Settings > Plugins > Mod Framework > ")
		TEXT("Allow Loose Content Mounts.");
	return nullptr;
#else
	// A cooked build normally starts with the pak layer already installed. It is missing when the
	// game was staged without paks, which is exactly the case where mods still want to ship one.
	FPakPlatformFile* Created = new FPakPlatformFile();
	IPlatformFile& CurrentPlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!Created->Initialize(&CurrentPlatformFile, FCommandLine::Get()))
	{
		delete Created;
		OutError = TEXT("Failed to initialise a pak layer over the current platform file.");
		return nullptr;
	}

	Created->InitializeNewAsyncIO();
	FPlatformFileManager::Get().SetPlatformFile(*Created);
	OwnedPakPlatformFile = Created;

	UE_LOG(LogModFramework, Log,
		TEXT("This build started without a pak layer; the mod framework installed one so mod paks can be mounted."));

	return Created;
#endif
}

bool FModPakContentMounter::CollectPakFiles(const FString& AbsolutePath, TArray<FString>& OutPakFiles, FString& OutError)
{
	OutPakFiles.Reset();
	OutError.Reset();

	IFileManager& FileManager = IFileManager::Get();

	if (FileManager.DirectoryExists(*AbsolutePath))
	{
		TArray<FString> FoundNames;
		FileManager.FindFiles(FoundNames, *AbsolutePath, TEXT("pak"));

		if (FoundNames.Num() == 0)
		{
			OutError = FString::Printf(TEXT("The directory '%s' contains no .pak files."), *AbsolutePath);
			return false;
		}

		// Sorted so that two identical installs mount in the same order, which matters whenever
		// several paks share a read order.
		FoundNames.Sort();
		OutPakFiles.Reserve(FoundNames.Num());
		for (const FString& Name : FoundNames)
		{
			OutPakFiles.Add(AbsolutePath / Name);
		}

		return true;
	}

	if (!FileManager.FileExists(*AbsolutePath))
	{
		OutError = FString::Printf(TEXT("'%s' does not exist."), *AbsolutePath);
		return false;
	}

	if (!FPaths::GetExtension(AbsolutePath).Equals(TEXT("pak"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("'%s' is not a .pak file."), *AbsolutePath);
		return false;
	}

	OutPakFiles.Add(AbsolutePath);
	return true;
}

int32 FModPakContentMounter::UnmountPakFiles(FPakPlatformFile& PakPlatformFile, const TArray<FString>& PakFilenames)
{
	int32 FailureCount = 0;

	// Reverse order, mirroring how they were mounted.
	for (int32 Index = PakFilenames.Num() - 1; Index >= 0; --Index)
	{
		// FPakPlatformFile::Unmount matches mounted paks by exact string equality on the filename,
		// which is why the record keeps the strings that were handed to Mount verbatim.
		if (!PakPlatformFile.Unmount(*PakFilenames[Index]))
		{
			++FailureCount;
			UE_LOG(LogModFramework, Error, TEXT("Failed to unmount pak '%s'."), *PakFilenames[Index]);
		}
	}

	return FailureCount;
}
