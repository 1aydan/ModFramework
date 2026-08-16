// Copyright (c) 2026. Licensed for use in your own projects.

#include "Providers/LocalFileModProvider.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "HAL/FileManager.h"
#include "Internationalization/Text.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModManifestParser.h"
#include "Manifest/ModVersion.h"
#include "Misc/CString.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Packaging/ModPackageFormat.h"
#include "Providers/ModProvider.h"
#include "Serialization/Archive.h"
#include "Templates/UniquePtr.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"

#define LOCTEXT_NAMESPACE "ModFramework"

/**
 * File-local helpers.
 *
 * Named rather than anonymous on purpose: this module is built with unity files, where every
 * anonymous namespace in the blob is the same namespace, so a generic helper name here would collide
 * with an identically named helper in a sibling .cpp. Member functions pull these in with a
 * function-scope using-directive, which cannot leak into the rest of the unity file.
 */
namespace LocalFileModProviderPrivate
{
	/** Stable diagnostic codes emitted by this provider. */
	const TCHAR* const CodeDirectoryMissing = TEXT("Discovery.DirectoryMissing");
	const TCHAR* const CodeDuplicate        = TEXT("Discovery.Duplicate");
	const TCHAR* const CodeManifestInvalid  = TEXT("Discovery.ManifestInvalid");
	const TCHAR* const CodeInstallFailed    = TEXT("Discovery.InstallFailed");
	const TCHAR* const CodeNotInstalled     = TEXT("Discovery.NotInstalled");

	/** Informational counterpart of Discovery.InstallFailed, so a UI can report the success too. */
	const TCHAR* const CodeInstalled        = TEXT("Discovery.Installed");

	FModDiagnostic MakeDiagnostic(EModDiagnosticSeverity Severity, const TCHAR* Code, FString Message,
		const FString& Context, const FModId& ModId)
	{
		FModDiagnostic Diagnostic(Severity, FName(Code), MoveTemp(Message), Context);
		Diagnostic.ModId = ModId;
		return Diagnostic;
	}

	void AddDiagnostic(TArray<FModDiagnostic>& OutDiagnostics, EModDiagnosticSeverity Severity, const TCHAR* Code,
		FString Message, const FString& Context, const FModId& ModId = FModId())
	{
		OutDiagnostics.Add(MakeDiagnostic(Severity, Code, MoveTemp(Message), Context, ModId));
	}

	/** Absolute, forward-slashed, no trailing separator. Empty in, empty out. */
	FString NormalizeDirectory(const FString& In)
	{
		if (In.IsEmpty())
		{
			return FString();
		}

		FString Result = FPaths::ConvertRelativePathToFull(In);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	/** A total order, so sorting stays stable even between names that differ only in case. */
	bool NameLess(const FString& A, const FString& B)
	{
		const int32 Insensitive = A.Compare(B, ESearchCase::IgnoreCase);
		return Insensitive != 0 ? (Insensitive < 0) : (A.Compare(B, ESearchCase::CaseSensitive) < 0);
	}

	/** A unique, dot-prefixed name for the scratch and backup directories an install needs. */
	FString MakeScratchDirectoryName(const TCHAR* Prefix, const FString& IdString)
	{
		return FString(Prefix) + IdString + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	/** Creates the directory when it is missing, then proves this process can create a file in it. */
	bool IsDirectoryWritable(const FString& Directory)
	{
		if (Directory.IsEmpty())
		{
			return false;
		}

		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.DirectoryExists(*Directory))
		{
			FileManager.MakeDirectory(*Directory, /*Tree=*/true);
			if (!FileManager.DirectoryExists(*Directory))
			{
				return false;
			}
		}

		const FString ProbePath = Directory
			/ (FString(TEXT(".modframework-write-probe-")) + FGuid::NewGuid().ToString(EGuidFormats::Digits));

		bool bWritable = false;
		{
			TUniquePtr<FArchive> Writer(FileManager.CreateFileWriter(*ProbePath, FILEWRITE_Silent));
			if (Writer.IsValid())
			{
				Writer->Close();
				bWritable = !Writer->IsError();
			}
		}

		FileManager.Delete(*ProbePath, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
		return bWritable;
	}

	/** True when Candidate is a strict descendant of Root. Both must already be absolute. */
	bool IsStrictlyUnder(const FString& Candidate, const FString& Root)
	{
		return !Root.IsEmpty()
			&& Candidate.Len() > Root.Len()
			&& FPaths::IsUnderDirectory(Candidate, Root);
	}

	/** The first Error in a diagnostic list, or the supplied fallback when the list holds none. */
	FModDiagnostic FirstErrorOr(const TArray<FModDiagnostic>& In, FModDiagnostic Fallback)
	{
		for (const FModDiagnostic& Diagnostic : In)
		{
			if (Diagnostic.IsError())
			{
				return Diagnostic;
			}
		}

		return Fallback;
	}
}

FLocalFileModProvider::FLocalFileModProvider(TArray<FString> InSearchDirectories)
{
	SetSearchDirectories(MoveTemp(InSearchDirectories));
}

FName FLocalFileModProvider::StaticProviderId()
{
	return FName(TEXT("LocalFile"));
}

FName FLocalFileModProvider::GetProviderId() const
{
	return StaticProviderId();
}

FText FLocalFileModProvider::GetDisplayName() const
{
	return LOCTEXT("LocalFileModProviderName", "Local Files");
}

bool FLocalFileModProvider::SupportsAsyncDiscovery() const
{
	// Discovery is plain synchronous filesystem work, so IModProvider::DiscoverModsAsync forwarding
	// to DiscoverMods and completing inline is exactly the right behaviour here.
	return false;
}

bool FLocalFileModProvider::SupportsInstall() const
{
	return true;
}

void FLocalFileModProvider::SetSearchDirectories(TArray<FString> In)
{
	using namespace LocalFileModProviderPrivate;

	TArray<FString> Normalized;
	Normalized.Reserve(In.Num());

	for (const FString& Directory : In)
	{
		const FString Candidate = NormalizeDirectory(Directory);
		if (Candidate.IsEmpty())
		{
			continue;
		}

		// Keep the configured order and drop repeats: scan order is part of the contract, because
		// the first copy of a duplicated mod id is the one that wins.
		bool bAlreadyPresent = false;
		for (const FString& Existing : Normalized)
		{
			if (Existing.Equals(Candidate, ESearchCase::IgnoreCase))
			{
				bAlreadyPresent = true;
				break;
			}
		}

		if (!bAlreadyPresent)
		{
			Normalized.Add(Candidate);
		}
	}

	FScopeLock Lock(&StateLock);
	SearchDirectories = MoveTemp(Normalized);
	CachedInstallDirectory.Reset();
}

TArray<FString> FLocalFileModProvider::GetSearchDirectories() const
{
	FScopeLock Lock(&StateLock);
	return SearchDirectories;
}

void FLocalFileModProvider::SetInstallDirectory(const FString& In)
{
	const FString Normalized = LocalFileModProviderPrivate::NormalizeDirectory(In);

	FScopeLock Lock(&StateLock);
	InstallDirectory = Normalized;
	CachedInstallDirectory.Reset();
}

FString FLocalFileModProvider::GetInstallDirectory() const
{
	return ResolveInstallDirectory();
}

FString FLocalFileModProvider::ResolveInstallDirectory() const
{
	using namespace LocalFileModProviderPrivate;

	FString Explicit;
	FString Cached;
	TArray<FString> Candidates;
	{
		FScopeLock Lock(&StateLock);
		Explicit = InstallDirectory;
		Cached = CachedInstallDirectory;
		Candidates = SearchDirectories;
	}

	if (!Explicit.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*Explicit, /*Tree=*/true);
		return Explicit;
	}

	if (!Cached.IsEmpty())
	{
		return Cached;
	}

	// Probing touches the filesystem, so it happens outside the lock and the winner is published
	// afterwards. A concurrent probe would simply reach the same conclusion.
	for (const FString& Candidate : Candidates)
	{
		if (IsDirectoryWritable(Candidate))
		{
			FScopeLock Lock(&StateLock);
			CachedInstallDirectory = Candidate;
			return Candidate;
		}
	}

	return FString();
}

FString FLocalFileModProvider::MakeInstallPathForId(const FModId& ModId) const
{
	using namespace LocalFileModProviderPrivate;

	if (!ModId.IsValid())
	{
		return FString();
	}

	const FString IdString = ModId.ToString();

	// The id becomes a directory name verbatim, so it gets exactly the same containment treatment as
	// any other path fragment that came out of a mod: malformed, escaping or reserved names are out.
	FString IdError;
	if (!FModId::IsValidIdString(IdString, IdError))
	{
		return FString();
	}

	FString PathError;
	if (!ModPackage::IsSafeRelativePath(IdString, PathError))
	{
		return FString();
	}

	const FString Root = ResolveInstallDirectory();
	if (Root.IsEmpty())
	{
		return FString();
	}

	const FString Full = FPaths::ConvertRelativePathToFull(Root / IdString);
	return IsStrictlyUnder(Full, Root) ? Full : FString();
}

FModDiscoveryResult FLocalFileModProvider::ReadUnpackedMod(const FString& ModRootPath) const
{
	using namespace LocalFileModProviderPrivate;

	FModDiscoveryResult Result;
	Result.ProviderId = StaticProviderId();
	Result.RootPath = ModRootPath;
	Result.ManifestPath = ModRootPath / FModManifestParser::GetManifestFileName();

	const FModManifestParseResult Parsed = FModManifestParser::ParseFromFile(Result.ManifestPath);
	Result.Manifest = Parsed.Manifest;
	Result.Diagnostics = Parsed.Diagnostics;
	Result.bManifestValid = Parsed.bSuccess && Parsed.Manifest.IsValid();

	if (!Result.bManifestValid)
	{
		AddDiagnostic(Result.Diagnostics, EModDiagnosticSeverity::Error, CodeManifestInvalid,
			FString::Printf(TEXT("'%s' could not be read as a mod manifest, so this folder is not usable as a mod."),
				*Result.ManifestPath),
			Result.ManifestPath, Parsed.Manifest.Id);
	}

	return Result;
}

FModDiscoveryResult FLocalFileModProvider::ReadPackagedMod(const FString& PackagePath) const
{
	using namespace LocalFileModProviderPrivate;

	FModDiscoveryResult Result;
	Result.ProviderId = StaticProviderId();
	Result.RootPath = PackagePath;
	Result.ManifestPath = PackagePath;

	TArray<FModDiagnostic> PeekDiagnostics;
	FModManifest PackageManifest;
	const bool bPeeked = FModPackageReader::PeekManifest(PackagePath, PackageManifest, PeekDiagnostics);

	Result.Manifest = PackageManifest;
	Result.Diagnostics = MoveTemp(PeekDiagnostics);
	Result.bManifestValid = bPeeked && PackageManifest.IsValid();

	if (!Result.bManifestValid)
	{
		AddDiagnostic(Result.Diagnostics, EModDiagnosticSeverity::Error, CodeManifestInvalid,
			FString::Printf(TEXT("'%s' is not a readable mod package."), *PackagePath),
			PackagePath, PackageManifest.Id);
	}
	else
	{
		AddDiagnostic(Result.Diagnostics, EModDiagnosticSeverity::Info, CodeNotInstalled,
			FString::Printf(TEXT("'%s' is a packaged mod. It has to be installed before its content can be mounted."),
				*PackagePath),
			PackagePath, PackageManifest.Id);
	}

	return Result;
}

void FLocalFileModProvider::DiscoverInDirectory(const FString& Directory, TArray<FModDiscoveryResult>& OutResults,
	TMap<FModId, int32>& InOutFirstResultIndexById, TMap<FModId, FDiscoveredMod>& OutDiscovered) const
{
	using namespace LocalFileModProviderPrivate;

	IFileManager& FileManager = IFileManager::Get();

	// Records a result, dropping it when an earlier result already claimed the same id.
	auto Accept = [&OutResults, &InOutFirstResultIndexById, &OutDiscovered](FModDiscoveryResult&& Result, bool bPackaged)
	{
		if (Result.bManifestValid && Result.Manifest.Id.IsValid())
		{
			if (const int32* ExistingIndex = InOutFirstResultIndexById.Find(Result.Manifest.Id))
			{
				if (OutResults.IsValidIndex(*ExistingIndex))
				{
					FModDiscoveryResult& Winner = OutResults[*ExistingIndex];
					AddDiagnostic(Winner.Diagnostics, EModDiagnosticSeverity::Warning, CodeDuplicate,
						FString::Printf(TEXT("Two mods declare the id '%s'. Keeping '%s' and ignoring '%s'."),
							*Result.Manifest.Id.ToString(), *Winner.RootPath, *Result.RootPath),
						Result.RootPath, Result.Manifest.Id);
				}

				UE_LOG(LogModFramework, Warning, TEXT("Ignoring duplicate mod id '%s' found at '%s'."),
					*Result.Manifest.Id.ToString(), *Result.RootPath);
				return;
			}

			InOutFirstResultIndexById.Add(Result.Manifest.Id, OutResults.Num());

			FDiscoveredMod Discovered;
			Discovered.RootPath = Result.RootPath;
			Discovered.bPackaged = bPackaged;
			OutDiscovered.Add(Result.Manifest.Id, MoveTemp(Discovered));
		}

		OutResults.Add(MoveTemp(Result));
	};

	// 1. Immediate subdirectories that contain a mod.json.
	TArray<FString> SubDirectoryNames;
	FileManager.FindFiles(SubDirectoryNames, *(Directory / TEXT("*")), /*Files=*/false, /*Directories=*/true);
	SubDirectoryNames.Sort([](const FString& A, const FString& B) { return NameLess(A, B); });

	for (const FString& Name : SubDirectoryNames)
	{
		// A scratch or backup directory left behind by an interrupted install starts with a dot and
		// is deliberately not a mod, whatever it happens to contain.
		if (Name.IsEmpty() || Name.StartsWith(TEXT("."), ESearchCase::CaseSensitive))
		{
			continue;
		}

		const FString ModRootPath = Directory / Name;
		if (!FileManager.FileExists(*(ModRootPath / FModManifestParser::GetManifestFileName())))
		{
			continue;
		}

		Accept(ReadUnpackedMod(ModRootPath), /*bPackaged=*/false);
	}

	// 2. Loose *.mod packages sitting directly in the search directory.
	TArray<FString> PackageNames;
	FileManager.FindFiles(PackageNames, *Directory, TEXT("mod"));

	// Platform wildcard matching can be looser than it looks, so the extension is re-checked here.
	PackageNames.RemoveAll([](const FString& Name)
	{
		return Name.IsEmpty() || !FPaths::GetExtension(Name).Equals(TEXT("mod"), ESearchCase::IgnoreCase);
	});
	PackageNames.Sort([](const FString& A, const FString& B) { return NameLess(A, B); });

	for (const FString& Name : PackageNames)
	{
		Accept(ReadPackagedMod(Directory / Name), /*bPackaged=*/true);
	}
}

void FLocalFileModProvider::DiscoverMods(TArray<FModDiscoveryResult>& OutResults)
{
	using namespace LocalFileModProviderPrivate;

	TArray<FString> Directories;
	{
		FScopeLock Lock(&StateLock);
		Directories = SearchDirectories;
	}

	const int32 FirstResultIndex = OutResults.Num();

	TMap<FModId, int32> FirstResultIndexById;
	TMap<FModId, FDiscoveredMod> Discovered;

	IFileManager& FileManager = IFileManager::Get();

	for (const FString& Directory : Directories)
	{
		if (Directory.IsEmpty())
		{
			continue;
		}

		if (!FileManager.DirectoryExists(*Directory))
		{
			// Creating a missing search directory is a convenience, not a requirement: a read-only
			// install location is perfectly normal and must not turn into a hard error.
			FileManager.MakeDirectory(*Directory, /*Tree=*/true);
		}

		if (!FileManager.DirectoryExists(*Directory))
		{
			FModDiscoveryResult Missing;
			Missing.ProviderId = StaticProviderId();
			Missing.RootPath = Directory;
			Missing.bManifestValid = false;
			AddDiagnostic(Missing.Diagnostics, EModDiagnosticSeverity::Warning, CodeDirectoryMissing,
				FString::Printf(TEXT("The mod search directory '%s' does not exist and could not be created."), *Directory),
				Directory);
			OutResults.Add(MoveTemp(Missing));
			continue;
		}

		DiscoverInDirectory(Directory, OutResults, FirstResultIndexById, Discovered);
	}

	{
		FScopeLock Lock(&StateLock);
		DiscoveredMods = MoveTemp(Discovered);
	}

	UE_LOG(LogModFramework, Log, TEXT("LocalFile provider scanned %d director%s and produced %d result(s)."),
		Directories.Num(), Directories.Num() == 1 ? TEXT("y") : TEXT("ies"), OutResults.Num() - FirstResultIndex);
}

bool FLocalFileModProvider::AcquireMod(const FModId& ModId, FString& OutLocalRootPath, FModDiagnostic& OutError)
{
	using namespace LocalFileModProviderPrivate;

	OutLocalRootPath.Reset();

	if (!ModId.IsValid())
	{
		OutError = MakeDiagnostic(EModDiagnosticSeverity::Error, CodeNotInstalled,
			TEXT("An invalid mod id cannot be acquired."), FString(), ModId);
		return false;
	}

	FDiscoveredMod Entry;
	bool bFound = false;
	{
		FScopeLock Lock(&StateLock);
		if (const FDiscoveredMod* Existing = DiscoveredMods.Find(ModId))
		{
			Entry = *Existing;
			bFound = true;
		}
	}

	IFileManager& FileManager = IFileManager::Get();

	// Already unpacked on disk: nothing to do but hand the path back.
	if (bFound && !Entry.bPackaged)
	{
		if (FileManager.DirectoryExists(*Entry.RootPath))
		{
			OutLocalRootPath = Entry.RootPath;
			return true;
		}

		// The cache is stale: the folder went away since the last scan.
		bFound = false;
	}

	if (bFound && Entry.bPackaged)
	{
		if (!FileManager.FileExists(*Entry.RootPath))
		{
			OutError = MakeDiagnostic(EModDiagnosticSeverity::Error, CodeNotInstalled,
				FString::Printf(TEXT("The package '%s' is no longer present."), *Entry.RootPath),
				Entry.RootPath, ModId);
			return false;
		}

		TArray<FModDiagnostic> InstallDiagnostics;
		FModId InstalledId;
		if (!InstallModPackage(Entry.RootPath, InstalledId, InstallDiagnostics))
		{
			OutError = FirstErrorOr(InstallDiagnostics,
				MakeDiagnostic(EModDiagnosticSeverity::Error, CodeInstallFailed,
					FString::Printf(TEXT("The package '%s' could not be installed."), *Entry.RootPath),
					Entry.RootPath, ModId));
			return false;
		}

		if (InstalledId != ModId)
		{
			OutError = MakeDiagnostic(EModDiagnosticSeverity::Error, CodeInstallFailed,
				FString::Printf(TEXT("The package '%s' installed the mod '%s', not '%s'."),
					*Entry.RootPath, *InstalledId.ToString(), *ModId.ToString()),
				Entry.RootPath, ModId);
			return false;
		}
	}

	// Either it was already installed and the cache did not know, or it has just been installed.
	const FString InstalledPath = MakeInstallPathForId(ModId);
	if (!InstalledPath.IsEmpty()
		&& FileManager.DirectoryExists(*InstalledPath)
		&& FileManager.FileExists(*(InstalledPath / FModManifestParser::GetManifestFileName())))
	{
		{
			FScopeLock Lock(&StateLock);
			FDiscoveredMod& Cached = DiscoveredMods.FindOrAdd(ModId);
			Cached.RootPath = InstalledPath;
			Cached.bPackaged = false;
		}

		OutLocalRootPath = InstalledPath;
		return true;
	}

	OutError = MakeDiagnostic(EModDiagnosticSeverity::Error, CodeNotInstalled,
		FString::Printf(TEXT("The mod '%s' is not installed in any search directory."), *ModId.ToString()),
		InstalledPath, ModId);
	return false;
}

bool FLocalFileModProvider::ReleaseMod(const FModId& ModId)
{
	// Nothing is copied, locked or reference counted when a mod is acquired from the local
	// filesystem, so there is nothing to give back. Removal is the separate, explicit UninstallMod.
	return true;
}

bool FLocalFileModProvider::InstallModPackage(const FString& PackagePath, FModId& OutInstalledId, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace LocalFileModProviderPrivate;

	OutInstalledId.Reset();

	if (PackagePath.IsEmpty())
	{
		AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
			TEXT("No package path was supplied."), PackagePath);
		return false;
	}

	IFileManager& FileManager = IFileManager::Get();
	const FString AbsolutePackagePath = FPaths::ConvertRelativePathToFull(PackagePath);

	if (!FileManager.FileExists(*AbsolutePackagePath))
	{
		AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
			FString::Printf(TEXT("There is no package at '%s'."), *AbsolutePackagePath), AbsolutePackagePath);
		return false;
	}

	const FString InstallRoot = ResolveInstallDirectory();
	if (InstallRoot.IsEmpty())
	{
		AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
			TEXT("No writable mod install directory is configured or available."), AbsolutePackagePath);
		return false;
	}

	// The manifest is COPIED out of the reader rather than referenced: the reader is closed before
	// the directories are swapped, and a reference into it would dangle from that point on.
	FModManifest PackageManifest;
	FString TargetPath;
	{
		FModPackageReader Reader;
		if (!Reader.Open(AbsolutePackagePath, OutDiagnostics))
		{
			AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
				FString::Printf(TEXT("'%s' could not be opened as a mod package."), *AbsolutePackagePath),
				AbsolutePackagePath);
			return false;
		}

		if (!Reader.VerifyIntegrity(OutDiagnostics))
		{
			AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
				FString::Printf(TEXT("'%s' failed its integrity check and will not be installed."), *AbsolutePackagePath),
				AbsolutePackagePath);
			return false;
		}

		PackageManifest = Reader.GetManifest();
		if (!PackageManifest.IsValid())
		{
			AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
				FString::Printf(TEXT("'%s' does not carry a usable manifest."), *AbsolutePackagePath),
				AbsolutePackagePath);
			return false;
		}

		TargetPath = MakeInstallPathForId(PackageManifest.Id);
		if (TargetPath.IsEmpty())
		{
			AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
				FString::Printf(TEXT("The mod id '%s' cannot be used as a directory name under '%s'."),
					*PackageManifest.Id.ToString(), *InstallRoot),
				AbsolutePackagePath, PackageManifest.Id);
			return false;
		}

		const bool bTargetExists = FileManager.DirectoryExists(*TargetPath);
		if (bTargetExists)
		{
			const FString ExistingManifestPath = TargetPath / FModManifestParser::GetManifestFileName();
			const FModManifestParseResult ExistingParse = FModManifestParser::ParseFromFile(ExistingManifestPath);

			if (!ExistingParse.bSuccess || !ExistingParse.Manifest.IsValid())
			{
				// Something is already sitting there and it cannot be shown to be older. Overwriting
				// it would mean guessing with somebody else's files.
				AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
					FString::Printf(TEXT("'%s' already exists but its manifest could not be read; remove it before installing."),
						*TargetPath),
					TargetPath, PackageManifest.Id);
				return false;
			}

			if (!(ExistingParse.Manifest.Version < PackageManifest.Version))
			{
				AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
					FString::Printf(TEXT("Version %s of '%s' is already installed; the package supplies %s, which is not newer."),
						*ExistingParse.Manifest.Version.ToString(), *PackageManifest.Id.ToString(),
						*PackageManifest.Version.ToString()),
					TargetPath, PackageManifest.Id);
				return false;
			}
		}

		// Extract into a scratch directory first, so a failure halfway through never leaves a
		// half-written mod somewhere the discovery scan would pick it up.
		const FString ScratchPath = InstallRoot / MakeScratchDirectoryName(TEXT(".modinstall-"), PackageManifest.Id.ToString());
		FileManager.DeleteDirectory(*ScratchPath, /*RequireExists=*/false, /*Tree=*/true);

		if (!FileManager.MakeDirectory(*ScratchPath, /*Tree=*/true) && !FileManager.DirectoryExists(*ScratchPath))
		{
			AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
				FString::Printf(TEXT("The staging directory '%s' could not be created."), *ScratchPath),
				ScratchPath, PackageManifest.Id);
			return false;
		}

		if (!Reader.ExtractAll(ScratchPath, OutDiagnostics))
		{
			FileManager.DeleteDirectory(*ScratchPath, /*RequireExists=*/false, /*Tree=*/true);
			AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
				FString::Printf(TEXT("'%s' could not be extracted."), *AbsolutePackagePath),
				AbsolutePackagePath, PackageManifest.Id);
			return false;
		}

		Reader.Close();

		// A package need not carry mod.json as an entry, because the manifest already lives in the
		// container. The installed folder still needs one to be discoverable next time.
		const FString ScratchManifestPath = ScratchPath / FModManifestParser::GetManifestFileName();
		if (!FileManager.FileExists(*ScratchManifestPath)
			&& !FModManifestParser::SerializeToFile(PackageManifest, ScratchManifestPath))
		{
			FileManager.DeleteDirectory(*ScratchPath, /*RequireExists=*/false, /*Tree=*/true);
			AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
				FString::Printf(TEXT("The manifest could not be written to '%s'."), *ScratchManifestPath),
				ScratchManifestPath, PackageManifest.Id);
			return false;
		}

		// Swap the staging directory into place. The previous installation is moved aside rather than
		// deleted, so a failed rename can be undone instead of losing the player's mod.
		FString BackupPath;
		if (bTargetExists)
		{
			BackupPath = InstallRoot / MakeScratchDirectoryName(TEXT(".modbackup-"), PackageManifest.Id.ToString());
			if (!FileManager.Move(*BackupPath, *TargetPath, /*Replace=*/false, /*EvenIfReadOnly=*/false,
				/*Attributes=*/false, /*bDoNotRetryOrError=*/true))
			{
				FileManager.DeleteDirectory(*ScratchPath, /*RequireExists=*/false, /*Tree=*/true);
				AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
					FString::Printf(TEXT("The existing installation at '%s' could not be moved aside; it may be in use."),
						*TargetPath),
					TargetPath, PackageManifest.Id);
				return false;
			}
		}

		if (!FileManager.Move(*TargetPath, *ScratchPath, /*Replace=*/false, /*EvenIfReadOnly=*/false,
			/*Attributes=*/false, /*bDoNotRetryOrError=*/true))
		{
			if (!BackupPath.IsEmpty())
			{
				// Put the previous installation back before giving up.
				FileManager.Move(*TargetPath, *BackupPath, /*Replace=*/false, /*EvenIfReadOnly=*/false,
					/*Attributes=*/false, /*bDoNotRetryOrError=*/true);
			}

			FileManager.DeleteDirectory(*ScratchPath, /*RequireExists=*/false, /*Tree=*/true);
			AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
				FString::Printf(TEXT("'%s' could not be moved into place at '%s'."), *ScratchPath, *TargetPath),
				TargetPath, PackageManifest.Id);
			return false;
		}

		if (!BackupPath.IsEmpty() && !FileManager.DeleteDirectory(*BackupPath, /*RequireExists=*/false, /*Tree=*/true))
		{
			// The install itself succeeded; a leftover backup folder is untidy, not fatal.
			UE_LOG(LogModFramework, Warning, TEXT("Installed '%s' but could not remove the backup directory '%s'."),
				*PackageManifest.Id.ToString(), *BackupPath);
		}
	}

	{
		FScopeLock Lock(&StateLock);
		FDiscoveredMod& Cached = DiscoveredMods.FindOrAdd(PackageManifest.Id);
		Cached.RootPath = TargetPath;
		Cached.bPackaged = false;
	}

	AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Info, CodeInstalled,
		FString::Printf(TEXT("Installed '%s' version %s into '%s'."),
			*PackageManifest.Id.ToString(), *PackageManifest.Version.ToString(), *TargetPath),
		TargetPath, PackageManifest.Id);

	UE_LOG(LogModFramework, Log, TEXT("Installed mod '%s' version %s from '%s' into '%s'."),
		*PackageManifest.Id.ToString(), *PackageManifest.Version.ToString(), *AbsolutePackagePath, *TargetPath);

	OutInstalledId = PackageManifest.Id;
	return true;
}

bool FLocalFileModProvider::UninstallMod(const FModId& ModId, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace LocalFileModProviderPrivate;

	if (!ModId.IsValid())
	{
		AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeNotInstalled,
			TEXT("An invalid mod id cannot be uninstalled."), FString(), ModId);
		return false;
	}

	const FString InstallRoot = ResolveInstallDirectory();
	if (InstallRoot.IsEmpty())
	{
		AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
			TEXT("No writable mod install directory is configured or available."), FString(), ModId);
		return false;
	}

	FString TargetPath = MakeInstallPathForId(ModId);

	// Prefer the path discovery actually saw, but only when it really is inside the install
	// directory: a mod loaded from a read-only search directory must never be deleted from here.
	FString CachedRoot;
	{
		FScopeLock Lock(&StateLock);
		if (const FDiscoveredMod* Existing = DiscoveredMods.Find(ModId))
		{
			if (!Existing->bPackaged)
			{
				CachedRoot = Existing->RootPath;
			}
		}
	}

	if (!CachedRoot.IsEmpty())
	{
		const FString CachedFull = FPaths::ConvertRelativePathToFull(CachedRoot);
		if (IsStrictlyUnder(CachedFull, InstallRoot))
		{
			TargetPath = CachedFull;
		}
	}

	if (TargetPath.IsEmpty() || !IsStrictlyUnder(TargetPath, InstallRoot))
	{
		AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
			FString::Printf(TEXT("Refusing to uninstall '%s': it does not live under the install directory '%s'."),
				*ModId.ToString(), *InstallRoot),
			TargetPath, ModId);
		return false;
	}

	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*TargetPath))
	{
		AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeNotInstalled,
			FString::Printf(TEXT("'%s' is not installed at '%s'."), *ModId.ToString(), *TargetPath),
			TargetPath, ModId);
		return false;
	}

	FileManager.DeleteDirectory(*TargetPath, /*RequireExists=*/false, /*Tree=*/true);
	if (FileManager.DirectoryExists(*TargetPath))
	{
		AddDiagnostic(OutDiagnostics, EModDiagnosticSeverity::Error, CodeInstallFailed,
			FString::Printf(TEXT("'%s' could not be removed; some of its files may still be open."), *TargetPath),
			TargetPath, ModId);
		return false;
	}

	{
		FScopeLock Lock(&StateLock);
		DiscoveredMods.Remove(ModId);
	}

	UE_LOG(LogModFramework, Log, TEXT("Uninstalled mod '%s' from '%s'."), *ModId.ToString(), *TargetPath);
	return true;
}

#undef LOCTEXT_NAMESPACE
