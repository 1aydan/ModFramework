// Copyright (c) 2026. Licensed for use in your own projects.

#include "Content/ModContentManager.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Set.h"
#include "Content/ModLooseContentMounter.h"
#include "Content/ModPakContentMounter.h"
#include "Core/ModFrameworkLog.h"
#include "HAL/FileManager.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CString.h"
#include "Misc/Char.h"
#include "Misc/MountPoint.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Templates/RefCounting.h"
#include "Templates/SharedPointer.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/TopLevelAssetPath.h"

namespace
{
	/** Highest pak read order a manifest may ask for. Keeps untrusted input out of the uint32 API. */
	constexpr int32 GMaxModMountOrder = 10000;

	/**
	 * Package roots the engine owns. A mod that mounts over one of these could replace base game or
	 * engine assets wholesale, so they are rejected before anything is touched even though
	 * FPackageName::MountPointExists would usually catch them too.
	 */
	const TCHAR* const GReservedPackageRoots[] =
	{
		TEXT("/Engine/"),
		TEXT("/Game/"),
		TEXT("/Script/"),
		TEXT("/Temp/"),
		TEXT("/Memory/"),
		TEXT("/None/"),
		TEXT("/Verse/"),
		TEXT("/Config/")
	};

	/** Builds a diagnostic that already carries the owning mod id. */
	FModDiagnostic MakeContentDiagnostic(EModDiagnosticSeverity Severity, const TCHAR* Code, FString Message,
		FString Context, const FModId& ModId)
	{
		FModDiagnostic Diagnostic(Severity, FName(Code), MoveTemp(Message), MoveTemp(Context));
		Diagnostic.ModId = ModId;
		return Diagnostic;
	}

	/** The asset registry, or nullptr when it does not exist yet (very early startup) or any more. */
	IAssetRegistry* TryGetAssetRegistry()
	{
		if (IAssetRegistry* Registry = IAssetRegistry::Get())
		{
			return Registry;
		}

		if (FAssetRegistryModule* Module =
			FModuleManager::LoadModulePtr<FAssetRegistryModule>(AssetRegistryConstants::ModuleName))
		{
			return Module->TryGet();
		}

		return nullptr;
	}

	/** "/MyMod/" -> "/MyMod". The asset registry addresses package paths without a trailing slash. */
	FString MountPointToPackagePath(const FString& InNormalizedMountPoint)
	{
		FString PackagePath = InNormalizedMountPoint;
		if (PackagePath.Len() > 1 && PackagePath.EndsWith(TEXT("/")))
		{
			PackagePath.LeftChopInline(1);
		}

		return PackagePath;
	}

	/** Absolute, forward-slashed, no trailing separator. Empty in, empty out. */
	FString MakeAbsoluteDirectory(const FString& In)
	{
		if (In.IsEmpty())
		{
			return FString();
		}

		FString Result = FPaths::ConvertRelativePathToFull(In);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}
}

//////////////////////////////////////////////////////////////////////////
// ModContentMounting

bool ModContentMounting::ValidateVirtualMountPoint(const FString& InNormalizedMountPoint, FString& OutError)
{
	OutError.Reset();

	if (InNormalizedMountPoint.IsEmpty())
	{
		OutError = TEXT("Mount point is empty.");
		return false;
	}

	if (!InNormalizedMountPoint.StartsWith(TEXT("/")) || !InNormalizedMountPoint.EndsWith(TEXT("/")))
	{
		OutError = FString::Printf(TEXT("Mount point '%s' is not of the form '/Name/'."), *InNormalizedMountPoint);
		return false;
	}

	// "/" and "//" both frame an empty name; guard before the substring maths.
	if (InNormalizedMountPoint.Len() < 3)
	{
		OutError = TEXT("Mount point has no name between its slashes.");
		return false;
	}

	// Strip the framing slashes; what is left has to be a single legal package-root segment.
	const FString Segment = InNormalizedMountPoint.Mid(1, InNormalizedMountPoint.Len() - 2);
	if (Segment.IsEmpty())
	{
		OutError = TEXT("Mount point has no name between its slashes.");
		return false;
	}

	if (Segment.Contains(TEXT("/")))
	{
		OutError = FString::Printf(
			TEXT("Mount point '%s' has more than one path segment; a mod mounts at a single package root."),
			*InNormalizedMountPoint);
		return false;
	}

	const TCHAR FirstChar = Segment[0];
	if (!FChar::IsAlpha(FirstChar) && !FChar::IsUnderscore(FirstChar))
	{
		OutError = FString::Printf(TEXT("Mount point '%s' must start with a letter or an underscore."),
			*InNormalizedMountPoint);
		return false;
	}

	for (int32 Index = 0; Index < Segment.Len(); ++Index)
	{
		const TCHAR Char = Segment[Index];
		if (!FChar::IsAlnum(Char) && !FChar::IsUnderscore(Char))
		{
			OutError = FString::Printf(
				TEXT("Mount point '%s' contains the character '%c' at index %d; only letters, digits and underscores are allowed."),
				*InNormalizedMountPoint, Char, Index);
			return false;
		}
	}

	for (const TCHAR* Reserved : GReservedPackageRoots)
	{
		if (InNormalizedMountPoint.Equals(Reserved, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("'%s' is a package root the engine reserves; a mod may not mount over it."),
				*InNormalizedMountPoint);
			return false;
		}
	}

	return true;
}

bool ModContentMounting::RegisterPackageMountPoint(const FString& InNormalizedMountPoint,
	const FString& InAbsoluteContentPath, FString& OutError)
{
	OutError.Reset();

	FString LocalError;
	if (!ValidateVirtualMountPoint(InNormalizedMountPoint, LocalError))
	{
		OutError = MoveTemp(LocalError);
		return false;
	}

	if (InAbsoluteContentPath.IsEmpty())
	{
		OutError = TEXT("Content path is empty.");
		return false;
	}

	// 5.8 returns a refcounted handle to the mount point; a null handle means the engine rejected
	// the pair outright. The handle itself is deliberately dropped - the mount point stays alive
	// because it is registered, and it is released again by root path + content path.
	const TRefCountPtr<UE::PackageName::IMountPoint> MountPoint =
		FPackageName::RegisterMountPoint(InNormalizedMountPoint, InAbsoluteContentPath);

	if (!MountPoint.IsValid())
	{
		OutError = FString::Printf(TEXT("The engine refused to register '%s' -> '%s' as a package mount point."),
			*InNormalizedMountPoint, *InAbsoluteContentPath);
		return false;
	}

	UE_LOG(LogModFramework, Verbose, TEXT("Registered package mount point %s -> %s"),
		*InNormalizedMountPoint, *InAbsoluteContentPath);

	return true;
}

void ModContentMounting::UnregisterPackageMountPoint(const FString& InNormalizedMountPoint,
	const FString& InAbsoluteContentPath)
{
	if (InNormalizedMountPoint.IsEmpty() || InAbsoluteContentPath.IsEmpty())
	{
		return;
	}

	FPackageName::UnRegisterMountPoint(InNormalizedMountPoint, InAbsoluteContentPath);

	UE_LOG(LogModFramework, Verbose, TEXT("Unregistered package mount point %s -> %s"),
		*InNormalizedMountPoint, *InAbsoluteContentPath);
}

int32 ModContentMounting::ScanMountPointIntoAssetRegistry(const FString& InNormalizedMountPoint,
	TArray<FName>& OutDiscoveredPackages)
{
	OutDiscoveredPackages.Reset();

	IAssetRegistry* Registry = TryGetAssetRegistry();
	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Mounted %s but the asset registry is not available; its assets will only be discoverable once ")
			TEXT("something else scans that path."),
			*InNormalizedMountPoint);
		return INDEX_NONE;
	}

	const FString PackagePath = MountPointToPackagePath(InNormalizedMountPoint);

	// bForceRescan, because the same virtual root can be re-used by a different mod (or a different
	// version of the same mod) later in the session and stale entries would be worse than a rescan.
	Registry->ScanPathsSynchronous({ PackagePath }, /*bForceRescan*/ true, /*bIgnoreDenyListScanFilters*/ false);

	TArray<FAssetData> Assets;
	Registry->GetAssetsByPath(FName(*PackagePath), Assets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ false);

	TSet<FName> UniquePackages;
	UniquePackages.Reserve(Assets.Num());
	for (const FAssetData& Asset : Assets)
	{
		if (!Asset.PackageName.IsNone())
		{
			UniquePackages.Add(Asset.PackageName);
		}
	}

	OutDiscoveredPackages = UniquePackages.Array();
	OutDiscoveredPackages.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	return OutDiscoveredPackages.Num();
}

void ModContentMounting::ForgetMountPointInAssetRegistry(const FString& InNormalizedMountPoint)
{
	IAssetRegistry* Registry = TryGetAssetRegistry();
	if (!Registry)
	{
		return;
	}

	const FString PackagePath = MountPointToPackagePath(InNormalizedMountPoint);

	// IAssetRegistry only lets a path be dropped once nothing is left under it, and it exposes no
	// way to purge the FAssetData of packages that came from an unmounted source. Failing here is
	// expected and harmless: the paths simply stay in the cached path list.
	if (!Registry->RemovePath(PackagePath))
	{
		UE_LOG(LogModFramework, Verbose,
			TEXT("Asset registry kept cached data for '%s' after unmounting; the engine offers no public API to ")
			TEXT("purge assets of an unmounted path."),
			*PackagePath);
	}
}

//////////////////////////////////////////////////////////////////////////
// FModContentManager

FModContentManager::FModContentManager() = default;

FModContentManager::~FModContentManager()
{
	Shutdown();
}

void FModContentManager::Initialize()
{
	if (bInitialized)
	{
		return;
	}

	bInitialized = true;

	// Order matters: the pak mounter is asked first so that a mod shipping both a pak and a loose
	// directory for the same root gets the packaged one, which is the one that was validated.
	RegisterMounter(MakeShared<FModPakContentMounter>());
	RegisterMounter(MakeShared<FModLooseContentMounter>());

	UE_LOG(LogModFramework, Verbose, TEXT("Mod content manager initialised with %d mounters."), Mounters.Num());
}

void FModContentManager::Shutdown()
{
	if (!bInitialized && ModMounts.Num() == 0 && Mounters.Num() == 0)
	{
		return;
	}

	TArray<FModId> MountedMods;
	ModMounts.GetKeys(MountedMods);
	MountedMods.Sort();

	for (const FModId& ModId : MountedMods)
	{
		TArray<FModDiagnostic> Diagnostics;
		if (!UnmountMod(ModId, Diagnostics))
		{
			ModDiagnostics::LogAll(Diagnostics);
		}
	}

	ModMounts.Reset();
	MountPointOwners.Reset();
	Mounters.Reset();
	bInitialized = false;
}

void FModContentManager::RegisterMounter(TSharedRef<IModContentMounter> InMounter)
{
	const FName MounterId = InMounter->GetMounterId();
	if (MounterId.IsNone())
	{
		UE_LOG(LogModFramework, Error, TEXT("Refusing to register a content mounter with no id."));
		return;
	}

	for (const TSharedRef<IModContentMounter>& Existing : Mounters)
	{
		if (Existing->GetMounterId() == MounterId)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("A content mounter with the id '%s' is already registered; ignoring the new one."),
				*MounterId.ToString());
			return;
		}
	}

	Mounters.Add(MoveTemp(InMounter));
}

void FModContentManager::UnregisterMounter(FName MounterId)
{
	const int32 RemovedCount = Mounters.RemoveAll(
		[MounterId](const TSharedRef<IModContentMounter>& Mounter)
		{
			return Mounter->GetMounterId() == MounterId;
		});

	if (RemovedCount > 0)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Unregistered content mounter '%s'."), *MounterId.ToString());
	}
}

bool FModContentManager::MountMod(const FModInfo& ModInfo, TArray<FModContentMount>& OutMounts,
	TArray<FModDiagnostic>& OutDiagnostics)
{
	OutMounts.Reset();

	if (!bInitialized)
	{
		Initialize();
	}

	const FModId ModId = ModInfo.GetId();
	if (!ModId.IsValid())
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.MountFailed"),
			TEXT("Cannot mount content for a mod with no id."), ModInfo.RootPath, ModId));
		return false;
	}

	if (const TArray<FModContentMount>* Existing = ModMounts.Find(ModId))
	{
		OutMounts = *Existing;
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Info, TEXT("Content.AlreadyMounted"),
			FString::Printf(TEXT("'%s' is already mounted with %d content root(s); nothing to do."),
				*ModId.ToString(), Existing->Num()),
			ModInfo.RootPath, ModId));
		return true;
	}

	const TArray<FModContentRoot>& Roots = ModInfo.Manifest.ContentRoots;
	if (Roots.Num() == 0)
	{
		// A mod that only registers extensions or APIs has no content of its own. That is normal.
		UE_LOG(LogModFramework, Verbose, TEXT("'%s' declares no content roots; nothing to mount."), *ModId.ToString());
		ModMounts.Add(ModId, TArray<FModContentMount>());
		OnMounted.Broadcast(ModId);
		return true;
	}

	const FString ModRootAbsolute = MakeAbsoluteDirectory(ModInfo.RootPath);
	if (ModRootAbsolute.IsEmpty() || !IFileManager::Get().DirectoryExists(*ModRootAbsolute))
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.RootMissing"),
			FString::Printf(TEXT("The mod root directory of '%s' does not exist."), *ModId.ToString()),
			ModInfo.RootPath, ModId));
		return false;
	}

	TArray<FModContentMount> Mounted;
	Mounted.Reserve(Roots.Num());

	bool bFailed = false;
	for (int32 RootIndex = 0; RootIndex < Roots.Num(); ++RootIndex)
	{
		FModContentMount Mount;
		if (!MountContentRoot(ModId, ModRootAbsolute, Roots[RootIndex], RootIndex, Mount, OutDiagnostics))
		{
			bFailed = true;
			break;
		}

		MountPointOwners.Add(Mount.VirtualMountPoint, ModId);
		Mounted.Add(MoveTemp(Mount));
	}

	if (bFailed)
	{
		// All-or-nothing: a mod is never left with some of its content present.
		RollbackMounts(Mounted, OutDiagnostics);
		UE_LOG(LogModFramework, Error, TEXT("Failed to mount '%s'; every content root already mounted was rolled back."),
			*ModId.ToString());
		return false;
	}

	OutMounts = Mounted;
	ModMounts.Add(ModId, MoveTemp(Mounted));

	UE_LOG(LogModFramework, Log, TEXT("Mounted %d content root(s) for '%s'."), OutMounts.Num(), *ModId.ToString());
	OnMounted.Broadcast(ModId);
	return true;
}

bool FModContentManager::MountContentRoot(const FModId& ModId, const FString& ModRootAbsolute,
	const FModContentRoot& RawRoot, int32 RootIndex, FModContentMount& OutMount, TArray<FModDiagnostic>& OutDiagnostics)
{
	const FString Context = FString::Printf(TEXT("%s content root #%d ('%s')"),
		*ModId.ToString(), RootIndex, *RawRoot.RelativePath);

	// --- 1. Shape of the declared path -------------------------------------------------------
	FString RelativePath = RawRoot.RelativePath;
	RelativePath.TrimStartAndEndInline();
	RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

	if (RelativePath.IsEmpty())
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.RootMissing"),
			TEXT("The content root declares no path."), Context, ModId));
		return false;
	}

	if (!FPaths::IsRelative(RelativePath))
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.PathEscape"),
			FString::Printf(TEXT("'%s' is an absolute path; a content root must be relative to the mod root."),
				*RelativePath),
			Context, ModId));
		return false;
	}

	// Reject "..", ":" and stray drive specifiers before any path maths happens, so that a hostile
	// manifest cannot rely on a normalisation quirk. The containment test below is the real guard;
	// this is the cheap, obvious one that also produces a much better error message.
	{
		TArray<FString> Segments;
		RelativePath.ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty*/ true);
		for (const FString& Segment : Segments)
		{
			if (Segment == TEXT("..") || Segment.Contains(TEXT(":")))
			{
				OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.PathEscape"),
					FString::Printf(TEXT("'%s' contains the segment '%s'; a content root may not leave the mod root."),
						*RelativePath, *Segment),
					Context, ModId));
				return false;
			}
		}
	}

	// --- 2. Resolve and prove containment ----------------------------------------------------
	const FString AbsolutePath = FPaths::ConvertRelativePathToFull(ModRootAbsolute / RelativePath);
	if (!FPaths::IsUnderDirectory(AbsolutePath, ModRootAbsolute))
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.PathEscape"),
			FString::Printf(TEXT("'%s' resolves to '%s', which is outside the mod root '%s'."),
				*RelativePath, *AbsolutePath, *ModRootAbsolute),
			Context, ModId));
		return false;
	}

	IFileManager& FileManager = IFileManager::Get();
	const bool bIsFile = FileManager.FileExists(*AbsolutePath);
	const bool bIsDirectory = FileManager.DirectoryExists(*AbsolutePath);
	if (!bIsFile && !bIsDirectory)
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.RootMissing"),
			FString::Printf(TEXT("'%s' does not exist."), *AbsolutePath), Context, ModId));
		return false;
	}

	// --- 3. Virtual mount point --------------------------------------------------------------
	FString MountPoint = NormalizeMountPoint(RawRoot.MountPoint);
	if (MountPoint.IsEmpty())
	{
		MountPoint = MakeDefaultMountPoint(ModId, ModRootAbsolute);
	}

	FString MountPointError;
	if (!ModContentMounting::ValidateVirtualMountPoint(MountPoint, MountPointError))
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.MountFailed"),
			MoveTemp(MountPointError), Context, ModId));
		return false;
	}

	if (const FModId* ExistingOwner = MountPointOwners.Find(MountPoint))
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.MountPointConflict"),
			FString::Printf(TEXT("'%s' is already mounted by '%s'. Mount points are unique across mods; give one of ")
				TEXT("them a different `mountPoint` in its manifest."),
				*MountPoint, *ExistingOwner->ToString()),
			Context, ModId));
		return false;
	}

	if (FPackageName::MountPointExists(MountPoint))
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.MountPointConflict"),
			FString::Printf(TEXT("'%s' is already a package root of the game, the engine or a plugin."), *MountPoint),
			Context, ModId));
		return false;
	}

	// --- 4. Hand the root to a mounter -------------------------------------------------------
	FModContentRoot ResolvedRoot = RawRoot;
	ResolvedRoot.RelativePath = RelativePath;
	ResolvedRoot.MountPoint = MountPoint;
	ResolvedRoot.MountOrder = FMath::Clamp(RawRoot.MountOrder, 0, GMaxModMountOrder);
	if (ResolvedRoot.MountOrder != RawRoot.MountOrder)
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Warning, TEXT("Content.MountOrderClamped"),
			FString::Printf(TEXT("Mount order %d is outside [0, %d] and was clamped to %d."),
				RawRoot.MountOrder, GMaxModMountOrder, ResolvedRoot.MountOrder),
			Context, ModId));
	}

	IModContentMounter* Mounter = FindMounterFor(ResolvedRoot, AbsolutePath);
	if (!Mounter)
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.NoMounter"),
			FString::Printf(TEXT("No registered mounter handles a '%s' content root at '%s'."),
				*ModFrameworkEnums::ToString(ResolvedRoot.Type), *AbsolutePath),
			Context, ModId));
		return false;
	}

	FModDiagnostic MountError;
	if (!Mounter->Mount(ModId, ResolvedRoot, AbsolutePath, OutMount, MountError))
	{
		MountError.ModId = ModId;
		if (MountError.Context.IsEmpty())
		{
			MountError.Context = Context;
		}
		if (MountError.Code.IsNone())
		{
			MountError.Code = FName(TEXT("Content.MountFailed"));
		}

		OutDiagnostics.Add(MoveTemp(MountError));
		return false;
	}

	// The mounter fills everything else; these two are the manager's to guarantee.
	OutMount.OwnerModId = ModId;
	OutMount.MounterId = Mounter->GetMounterId();

	if (OutMount.DiscoveredPackages.Num() == 0)
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Warning, TEXT("Content.NoPackagesFound"),
			FString::Printf(TEXT("'%s' mounted but no packages were found under it. For a pak, this usually means the ")
				TEXT("pak was cooked with its content nested below the pak root, or that `mountPoint` is not the name ")
				TEXT("the mod was cooked under."),
				*OutMount.VirtualMountPoint),
			Context, ModId));
	}

	return true;
}

bool FModContentManager::UnmountMod(const FModId& ModId, TArray<FModDiagnostic>& OutDiagnostics)
{
	TArray<FModContentMount> Mounts;
	if (!ModMounts.RemoveAndCopyValue(ModId, Mounts))
	{
		OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Info, TEXT("Content.NotMounted"),
			FString::Printf(TEXT("'%s' has no mounted content."), *ModId.ToString()), FString(), ModId));
		return true;
	}

	if (Mounts.Num() > 0)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Unmounting %d content root(s) of '%s'. Any object still loaded from this mod keeps pointing at ")
			TEXT("storage that is going away; the framework does not force a garbage collection to make that safe, so ")
			TEXT("unload the mod's objects first."),
			Mounts.Num(), *ModId.ToString());
	}

	bool bAllReleased = true;

	// Newest first, so nested or dependent mounts come apart in the reverse of the order they went
	// together in.
	for (int32 Index = Mounts.Num() - 1; Index >= 0; --Index)
	{
		const FModContentMount& Mount = Mounts[Index];
		MountPointOwners.Remove(Mount.VirtualMountPoint);

		IModContentMounter* Mounter = FindMounterById(Mount.MounterId);
		if (!Mounter)
		{
			bAllReleased = false;
			OutDiagnostics.Add(MakeContentDiagnostic(EModDiagnosticSeverity::Error, TEXT("Content.UnmountFailed"),
				FString::Printf(TEXT("The mounter '%s' that mounted '%s' is no longer registered, so it could not be ")
					TEXT("released."),
					*Mount.MounterId.ToString(), *Mount.VirtualMountPoint),
				Mount.PhysicalPath, ModId));
			continue;
		}

		FModDiagnostic UnmountError;
		if (!Mounter->Unmount(Mount, UnmountError))
		{
			bAllReleased = false;
			UnmountError.ModId = ModId;
			if (UnmountError.Context.IsEmpty())
			{
				UnmountError.Context = Mount.PhysicalPath;
			}
			if (UnmountError.Code.IsNone())
			{
				UnmountError.Code = FName(TEXT("Content.UnmountFailed"));
			}

			OutDiagnostics.Add(MoveTemp(UnmountError));
		}
	}

	OnUnmounted.Broadcast(ModId);
	return bAllReleased;
}

void FModContentManager::RollbackMounts(TArray<FModContentMount>& InOutMounts, TArray<FModDiagnostic>& OutDiagnostics)
{
	for (int32 Index = InOutMounts.Num() - 1; Index >= 0; --Index)
	{
		const FModContentMount& Mount = InOutMounts[Index];
		MountPointOwners.Remove(Mount.VirtualMountPoint);

		IModContentMounter* Mounter = FindMounterById(Mount.MounterId);
		if (!Mounter)
		{
			continue;
		}

		FModDiagnostic UnmountError;
		if (!Mounter->Unmount(Mount, UnmountError))
		{
			UnmountError.ModId = Mount.OwnerModId;
			if (UnmountError.Code.IsNone())
			{
				UnmountError.Code = FName(TEXT("Content.UnmountFailed"));
			}

			OutDiagnostics.Add(MoveTemp(UnmountError));
		}
	}

	InOutMounts.Reset();
}

bool FModContentManager::IsModMounted(const FModId& ModId) const
{
	return ModMounts.Contains(ModId);
}

TArray<FModContentMount> FModContentManager::GetMounts() const
{
	TArray<FModId> ModIds;
	ModMounts.GetKeys(ModIds);
	ModIds.Sort();

	TArray<FModContentMount> Result;
	for (const FModId& ModId : ModIds)
	{
		Result.Append(ModMounts.FindChecked(ModId));
	}

	return Result;
}

TArray<FModContentMount> FModContentManager::GetMountsForMod(const FModId& ModId) const
{
	if (const TArray<FModContentMount>* Found = ModMounts.Find(ModId))
	{
		return *Found;
	}

	return TArray<FModContentMount>();
}

FModId FModContentManager::FindOwningMod(const FString& LongPackageNameOrObjectPath) const
{
	if (LongPackageNameOrObjectPath.IsEmpty())
	{
		return FModId();
	}

	// Appending a separator lets "/MyMod" match the mount point "/MyMod/" without a special case,
	// and is harmless for a path that already has more segments.
	FString Candidate = LongPackageNameOrObjectPath;
	Candidate.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (!Candidate.EndsWith(TEXT("/")))
	{
		Candidate.AppendChar(TEXT('/'));
	}

	FModId Best;
	int32 BestLength = 0;
	for (const TPair<FString, FModId>& Pair : MountPointOwners)
	{
		if (Pair.Key.Len() > BestLength && Candidate.StartsWith(Pair.Key, ESearchCase::IgnoreCase))
		{
			Best = Pair.Value;
			BestLength = Pair.Key.Len();
		}
	}

	return Best;
}

TArray<FAssetData> FModContentManager::GetModAssets(const FModId& ModId, UClass* ClassFilter,
	bool bRecursiveClasses) const
{
	const TArray<FModContentMount>* Mounts = ModMounts.Find(ModId);
	if (!Mounts || Mounts->Num() == 0)
	{
		return TArray<FAssetData>();
	}

	FARFilter Filter;
	Filter.PackagePaths.Reserve(Mounts->Num());
	for (const FModContentMount& Mount : *Mounts)
	{
		Filter.PackagePaths.Add(FName(*MountPointToPackagePath(Mount.VirtualMountPoint)));
	}

	Filter.bRecursivePaths = true;
	if (ClassFilter)
	{
		Filter.ClassPaths.Add(ClassFilter->GetClassPathName());
		Filter.bRecursiveClasses = bRecursiveClasses;
	}

	IAssetRegistry* Registry = TryGetAssetRegistry();
	if (!Registry)
	{
		return TArray<FAssetData>();
	}

	TArray<FAssetData> Result;
	Registry->GetAssets(Filter, Result);
	return Result;
}

TArray<FAssetData> FModContentManager::GetAllModAssets(UClass* ClassFilter, bool bRecursiveClasses) const
{
	FARFilter Filter;
	for (const TPair<FModId, TArray<FModContentMount>>& Pair : ModMounts)
	{
		for (const FModContentMount& Mount : Pair.Value)
		{
			Filter.PackagePaths.Add(FName(*MountPointToPackagePath(Mount.VirtualMountPoint)));
		}
	}

	if (Filter.PackagePaths.Num() == 0)
	{
		return TArray<FAssetData>();
	}

	Filter.bRecursivePaths = true;
	if (ClassFilter)
	{
		Filter.ClassPaths.Add(ClassFilter->GetClassPathName());
		Filter.bRecursiveClasses = bRecursiveClasses;
	}

	IAssetRegistry* Registry = TryGetAssetRegistry();
	if (!Registry)
	{
		return TArray<FAssetData>();
	}

	TArray<FAssetData> Result;
	Registry->GetAssets(Filter, Result);
	return Result;
}

FString FModContentManager::NormalizeMountPoint(const FString& In)
{
	FString Working = In;
	Working.TrimStartAndEndInline();
	Working.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Collapse runs of separators so "//MyMod//" behaves like "/MyMod/".
	while (Working.ReplaceInline(TEXT("//"), TEXT("/")) > 0)
	{
	}

	int32 Start = 0;
	int32 End = Working.Len();
	while (Start < End && Working[Start] == TEXT('/'))
	{
		++Start;
	}
	while (End > Start && Working[End - 1] == TEXT('/'))
	{
		--End;
	}

	if (End <= Start)
	{
		return FString();
	}

	return FString::Printf(TEXT("/%s/"), *Working.Mid(Start, End - Start));
}

FString FModContentManager::MakeDefaultMountPoint(const FModId& ModId, const FString& ModRootPath)
{
	auto Sanitize = [](const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			const TCHAR Char = In[Index];
			if (FChar::IsAlnum(Char) || FChar::IsUnderscore(Char))
			{
				Out.AppendChar(Char);
			}
			else
			{
				// Keep the name recognisable rather than dropping separators silently.
				Out.AppendChar(TEXT('_'));
			}
		}

		// A package root may not start with a digit.
		if (Out.Len() > 0 && FChar::IsDigit(Out[0]))
		{
			Out.InsertAt(0, TEXT('_'));
		}

		return Out;
	};

	FString Directory = ModRootPath;
	FPaths::NormalizeDirectoryName(Directory);
	FString Candidate = Sanitize(FPaths::GetPathLeaf(Directory));

	if (Candidate.IsEmpty())
	{
		Candidate = Sanitize(ModId.ToString());
	}

	if (Candidate.IsEmpty())
	{
		Candidate = TEXT("Mod");
	}

	return FString::Printf(TEXT("/%s/"), *Candidate);
}

IModContentMounter* FModContentManager::FindMounterFor(const FModContentRoot& Root, const FString& AbsolutePath) const
{
	for (const TSharedRef<IModContentMounter>& Mounter : Mounters)
	{
		if (Mounter->CanMount(Root, AbsolutePath))
		{
			return &Mounter.Get();
		}
	}

	return nullptr;
}

IModContentMounter* FModContentManager::FindMounterById(FName MounterId) const
{
	for (const TSharedRef<IModContentMounter>& Mounter : Mounters)
	{
		if (Mounter->GetMounterId() == MounterId)
		{
			return &Mounter.Get();
		}
	}

	return nullptr;
}
