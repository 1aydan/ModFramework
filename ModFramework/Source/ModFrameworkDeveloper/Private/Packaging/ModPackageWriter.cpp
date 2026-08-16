// Copyright (c) 2026. Licensed for use in your own projects.

#include "Packaging/ModPackageWriter.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Manifest/ModManifestParser.h"
#include "Misc/Compression.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ModFrameworkDeveloperModule.h"
#include "Serialization/Archive.h"

namespace ModPackageWriterPrivate
{
	const TCHAR* const CodeOpenFailed    = TEXT("Package.OpenFailed");
	const TCHAR* const CodeWriteFailed   = TEXT("Package.WriteFailed");
	const TCHAR* const CodeUnsafePath    = TEXT("Package.UnsafePath");
	const TCHAR* const CodeDuplicate     = TEXT("Package.DuplicateEntry");
	const TCHAR* const CodeTooLarge      = TEXT("Package.TooLarge");
	const TCHAR* const CodeNoManifest    = TEXT("Package.NoManifest");
	const TCHAR* const CodeManifestBad   = TEXT("Package.ManifestInvalid");
	const TCHAR* const CodeSourceMissing = TEXT("Package.SourceMissing");

	// Deliberately not named MakeError: Templates/ValueOrError.h declares a global variadic
	// MakeError(ArgTypes&&...) that wins overload resolution at any call site with a using-directive.
	FModDiagnostic MakeWriterError(const TCHAR* Code, FString Message, const FString& Context = FString())
	{
		return FModDiagnostic::Error(FName(Code), MoveTemp(Message), Context);
	}

	/** Uppercase hex SHA-1, matching the reader's hash field format exactly. */
	FString HashBytes(TArrayView<const uint8> InBytes)
	{
		FSHAHash Hash;
		FSHA1::HashBuffer(InBytes.GetData(), static_cast<uint64>(InBytes.Num()), Hash.Hash);
		return Hash.ToString().ToUpper();
	}
}

FModPackageWriter::FModPackageWriter() = default;

FModPackageWriter::~FModPackageWriter()
{
	// Never leave a temp file behind, even if the caller forgot to Close or Abandon.
	Abandon();
}

bool FModPackageWriter::Open(const FString& InAbsoluteDestinationPath, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModPackageWriterPrivate;

	Abandon();

	if (InAbsoluteDestinationPath.IsEmpty())
	{
		OutDiagnostics.Add(MakeWriterError(CodeOpenFailed, TEXT("No destination path was supplied.")));
		return false;
	}

	DestinationPath = InAbsoluteDestinationPath;

	const FString DestinationDirectory = FPaths::GetPath(DestinationPath);
	if (!DestinationDirectory.IsEmpty() && !IFileManager::Get().MakeDirectory(*DestinationDirectory, /*Tree*/ true))
	{
		OutDiagnostics.Add(MakeWriterError(CodeOpenFailed,
			TEXT("Could not create the destination directory."), DestinationDirectory));
		return false;
	}

	// Sidecar temp file next to the destination so the final copy stays on one volume.
	TempPayloadPath = DestinationPath + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	TempPayloadArchive.Reset(IFileManager::Get().CreateFileWriter(*TempPayloadPath));
	if (!TempPayloadArchive.IsValid())
	{
		OutDiagnostics.Add(MakeWriterError(CodeOpenFailed,
			TEXT("Could not create the temporary payload file."), TempPayloadPath));
		TempPayloadPath.Reset();
		return false;
	}

	bOpen = true;
	return true;
}

bool FModPackageWriter::SetManifest(const FModManifest& InManifest, FModDiagnostic& OutError)
{
	using namespace ModPackageWriterPrivate;

	TArray<FModDiagnostic> ValidationDiagnostics;
	FModManifestParser::ValidateManifest(InManifest, DestinationPath, ValidationDiagnostics);
	if (ModDiagnostics::HasErrors(ValidationDiagnostics))
	{
		OutError = MakeWriterError(CodeManifestBad,
			FString::Printf(TEXT("The manifest is not valid: %s"),
				*ModDiagnostics::Join(ValidationDiagnostics, TEXT("; "))),
			InManifest.Id.ToString());
		return false;
	}

	FString Json;
	if (!FModManifestParser::SerializeToString(InManifest, Json, /*bPrettyPrint*/ true))
	{
		OutError = MakeWriterError(CodeManifestBad, TEXT("The manifest could not be serialised."),
			InManifest.Id.ToString());
		return false;
	}

	FTCHARToUTF8 Utf8(*Json);
	if (Utf8.Length() > ModPackage::MaxManifestBytes)
	{
		OutError = MakeWriterError(CodeTooLarge,
			FString::Printf(TEXT("The manifest is %d bytes, over the %lld byte limit."),
				Utf8.Length(), ModPackage::MaxManifestBytes));
		return false;
	}

	ManifestJson = MoveTemp(Json);
	bHasManifest = true;
	return true;
}

bool FModPackageWriter::AppendPayload(const FString& InRelativePath, TArrayView<const uint8> InBytes,
	FModDiagnostic& OutError)
{
	using namespace ModPackageWriterPrivate;

	if (!bOpen || !TempPayloadArchive.IsValid())
	{
		OutError = MakeWriterError(CodeWriteFailed, TEXT("The writer is not open."));
		return false;
	}

	// Normalise before validating so "Content\\Foo.pak" is judged as the "Content/Foo.pak" it becomes.
	FString RelativePath = InRelativePath;
	RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);

	FString PathError;
	if (!ModPackage::IsSafeRelativePath(RelativePath, PathError))
	{
		OutError = MakeWriterError(CodeUnsafePath, MoveTemp(PathError), RelativePath);
		return false;
	}

	// The reader compares paths case-insensitively, so reject a collision here rather than emit a
	// package it will refuse.
	const FString Key = ModPackage::MakeEntryKey(RelativePath);
	if (UsedEntryKeys.Contains(Key))
	{
		OutError = MakeWriterError(CodeDuplicate,
			TEXT("An entry with this path was already added (paths are compared case-insensitively)."),
			RelativePath);
		return false;
	}

	if (Entries.Num() >= ModPackage::MaxEntries)
	{
		OutError = MakeWriterError(CodeTooLarge,
			FString::Printf(TEXT("A package may hold at most %d entries."), ModPackage::MaxEntries));
		return false;
	}

	const int64 UncompressedSize = InBytes.Num();
	if (UncompressedSize > ModPackage::MaxEntryUncompressedBytes)
	{
		OutError = MakeWriterError(CodeTooLarge,
			FString::Printf(TEXT("'%s' is %lld bytes, over the %lld byte per-entry limit."),
				*RelativePath, UncompressedSize, ModPackage::MaxEntryUncompressedBytes),
			RelativePath);
		return false;
	}

	if (TotalUncompressedBytes + UncompressedSize > ModPackage::MaxTotalUncompressedBytes)
	{
		OutError = MakeWriterError(CodeTooLarge,
			TEXT("Adding this entry would exceed the total uncompressed size limit."), RelativePath);
		return false;
	}

	FModPackageEntry Entry;
	Entry.RelativePath = RelativePath;
	Entry.UncompressedSize = UncompressedSize;
	Entry.Hash = HashBytes(InBytes);   // always of the UNCOMPRESSED bytes, per the format

	TArray<uint8> Stored;
	bool bCompressed = false;

	if (bCompressionEnabled && UncompressedSize >= MinimumCompressionBytes && UncompressedSize > 0)
	{
		int32 CompressedBound = FCompression::CompressMemoryBound(NAME_Zlib, static_cast<int32>(UncompressedSize));
		Stored.SetNumUninitialized(CompressedBound);

		int32 CompressedSize = CompressedBound;
		if (FCompression::CompressMemory(NAME_Zlib, Stored.GetData(), CompressedSize,
			InBytes.GetData(), static_cast<int32>(UncompressedSize)))
		{
			// Only keep the compressed form when it actually helped. A zlib block larger than the
			// input costs space AND decompression time on every load.
			if (CompressedSize < UncompressedSize)
			{
				Stored.SetNum(CompressedSize, EAllowShrinking::No);
				bCompressed = true;
			}
		}

		if (!bCompressed)
		{
			Stored.Reset();
		}
	}

	if (!bCompressed)
	{
		Stored.Append(InBytes.GetData(), InBytes.Num());
	}

	Entry.bCompressed = bCompressed;
	Entry.CompressedSize = Stored.Num();

	// The format requires a stored entry to have exactly one size.
	if (!bCompressed && Entry.CompressedSize != Entry.UncompressedSize)
	{
		OutError = MakeWriterError(CodeWriteFailed,
			TEXT("Internal error: an uncompressed entry has mismatched sizes."), RelativePath);
		return false;
	}

	PayloadOffsetsInTemp.Add(TempPayloadArchive->Tell());

	if (Stored.Num() > 0)
	{
		TempPayloadArchive->Serialize(Stored.GetData(), Stored.Num());
	}

	if (TempPayloadArchive->IsError())
	{
		OutError = MakeWriterError(CodeWriteFailed, TEXT("Writing the payload failed."), RelativePath);
		return false;
	}

	Entries.Add(MoveTemp(Entry));
	UsedEntryKeys.Add(Key);
	TotalUncompressedBytes += UncompressedSize;
	return true;
}

bool FModPackageWriter::AddBytes(const FString& InRelativePath, TArrayView<const uint8> InBytes,
	FModDiagnostic& OutError)
{
	return AppendPayload(InRelativePath, InBytes, OutError);
}

bool FModPackageWriter::AddFile(const FString& InRelativePath, const FString& InAbsoluteSourcePath,
	FModDiagnostic& OutError)
{
	using namespace ModPackageWriterPrivate;

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *InAbsoluteSourcePath))
	{
		OutError = MakeWriterError(CodeSourceMissing, TEXT("The source file could not be read."),
			InAbsoluteSourcePath);
		return false;
	}

	return AppendPayload(InRelativePath, Bytes, OutError);
}

bool FModPackageWriter::Close(TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModPackageWriterPrivate;

	if (!bOpen)
	{
		OutDiagnostics.Add(MakeWriterError(CodeWriteFailed, TEXT("The writer is not open.")));
		return false;
	}

	if (!bHasManifest)
	{
		OutDiagnostics.Add(MakeWriterError(CodeNoManifest,
			TEXT("SetManifest must be called before Close: a package without a manifest cannot be discovered.")));
		Abandon();
		return false;
	}

	// Flush and close the temp payload file so it can be reopened for reading.
	TempPayloadArchive->Close();
	TempPayloadArchive.Reset();

	const FTCHARToUTF8 ManifestUtf8(*ManifestJson);
	const int64 ManifestSize = ManifestUtf8.Length();

	// TOC size is fully determined by the entries: int32 count, then 69 + PathNumBytes per entry.
	// This has to be exact - Header.TocSize must equal the bytes actually emitted.
	int64 TocSize = sizeof(int32);
	for (const FModPackageEntry& Entry : Entries)
	{
		const FTCHARToUTF8 PathUtf8(*Entry.RelativePath);
		TocSize += static_cast<int64>(sizeof(int32)) + PathUtf8.Length()   // path
			+ sizeof(int64) * 3                                            // offset, sizes
			+ sizeof(uint8)                                                // bCompressed
			+ ModPackage::HashHexLength;                                   // hash
	}

	if (TocSize > ModPackage::MaxTocBytes)
	{
		OutDiagnostics.Add(MakeWriterError(CodeTooLarge, TEXT("The table of contents is too large.")));
		Abandon();
		return false;
	}

	const int64 ManifestOffset = ModPackage::HeaderSize;
	const int64 TocOffset = ManifestOffset + ManifestSize;
	const int64 PayloadBase = TocOffset + TocSize;

	// Temp-file offsets become absolute package offsets now that the layout ahead of them is known.
	check(PayloadOffsetsInTemp.Num() == Entries.Num());
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		Entries[Index].Offset = PayloadBase + PayloadOffsetsInTemp[Index];
	}

	FModPackageHeader Header;
	Header.Magic = ModPackage::Magic;
	Header.FormatVersion = ModPackage::CurrentFormatVersion;
	Header.ManifestOffset = ManifestOffset;
	Header.ManifestSize = ManifestSize;
	Header.TocOffset = TocOffset;
	Header.TocSize = TocSize;
	Header.ContentHash = FModPackageReader::ComputeContentHash(Entries);

	// Write to a temp destination and move into place, so a failure never leaves a partial .mod file
	// somewhere a provider will try to load.
	const FString StagingPath = DestinationPath + TEXT(".writing");
	IFileManager::Get().Delete(*StagingPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);

	bool bSuccess = false;
	{
		TUniquePtr<FArchive> Out(IFileManager::Get().CreateFileWriter(*StagingPath));
		if (!Out.IsValid())
		{
			OutDiagnostics.Add(MakeWriterError(CodeOpenFailed,
				TEXT("Could not create the package file."), StagingPath));
			Abandon();
			return false;
		}

		// The header's operator<< comes from the runtime module - the same code the reader parses
		// with - so this is byte-identical by construction rather than by agreement.
		*Out << Header;

		if (Out->Tell() != ModPackage::HeaderSize)
		{
			OutDiagnostics.Add(MakeWriterError(CodeWriteFailed,
				FString::Printf(TEXT("Internal error: the header serialised to %lld bytes, expected %lld."),
					Out->Tell(), ModPackage::HeaderSize)));
			Out->Close();
			IFileManager::Get().Delete(*StagingPath, false, true);
			Abandon();
			return false;
		}

		Out->Serialize(const_cast<ANSICHAR*>(reinterpret_cast<const ANSICHAR*>(ManifestUtf8.Get())),
			ManifestSize);

		const int64 TocStart = Out->Tell();
		int32 EntryCount = Entries.Num();
		*Out << EntryCount;
		for (FModPackageEntry& Entry : Entries)
		{
			*Out << Entry;
		}

		const int64 ActualTocSize = Out->Tell() - TocStart;
		if (ActualTocSize != TocSize)
		{
			// Caught here rather than shipping a package whose header lies about its own TOC.
			OutDiagnostics.Add(MakeWriterError(CodeWriteFailed,
				FString::Printf(TEXT("Internal error: the table of contents serialised to %lld bytes but %lld were reserved."),
					ActualTocSize, TocSize)));
			Out->Close();
			IFileManager::Get().Delete(*StagingPath, false, true);
			Abandon();
			return false;
		}

		// Stream the payloads in, chunked so a large mod does not spike memory.
		{
			TUniquePtr<FArchive> TempIn(IFileManager::Get().CreateFileReader(*TempPayloadPath));
			if (!TempIn.IsValid())
			{
				OutDiagnostics.Add(MakeWriterError(CodeOpenFailed,
					TEXT("Could not reopen the temporary payload file."), TempPayloadPath));
				Out->Close();
				IFileManager::Get().Delete(*StagingPath, false, true);
				Abandon();
				return false;
			}

			constexpr int64 ChunkSize = 1024 * 1024;
			TArray<uint8> Chunk;
			Chunk.SetNumUninitialized(ChunkSize);

			int64 Remaining = TempIn->TotalSize();
			while (Remaining > 0)
			{
				const int64 ThisChunk = FMath::Min(Remaining, ChunkSize);
				TempIn->Serialize(Chunk.GetData(), ThisChunk);
				Out->Serialize(Chunk.GetData(), ThisChunk);
				Remaining -= ThisChunk;
			}
		}

		bSuccess = !Out->IsError();
		Out->Close();
	}

	if (!bSuccess)
	{
		OutDiagnostics.Add(MakeWriterError(CodeWriteFailed, TEXT("Writing the package failed."), StagingPath));
		IFileManager::Get().Delete(*StagingPath, false, true);
		Abandon();
		return false;
	}

	IFileManager::Get().Delete(*DestinationPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
	if (!IFileManager::Get().Move(*DestinationPath, *StagingPath, /*Replace*/ true))
	{
		OutDiagnostics.Add(MakeWriterError(CodeWriteFailed,
			TEXT("Could not move the finished package into place."), DestinationPath));
		IFileManager::Get().Delete(*StagingPath, false, true);
		Abandon();
		return false;
	}

	UE_LOG(LogModFrameworkDeveloper, Log,
		TEXT("Wrote '%s': %d entries, %lld bytes uncompressed."),
		*DestinationPath, Entries.Num(), TotalUncompressedBytes);

	Abandon();
	return true;
}

void FModPackageWriter::Abandon()
{
	if (TempPayloadArchive.IsValid())
	{
		TempPayloadArchive->Close();
		TempPayloadArchive.Reset();
	}

	if (!TempPayloadPath.IsEmpty())
	{
		IFileManager::Get().Delete(*TempPayloadPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		TempPayloadPath.Reset();
	}

	bOpen = false;
	bHasManifest = false;
	ManifestJson.Reset();
	Entries.Reset();
	PayloadOffsetsInTemp.Reset();
	UsedEntryKeys.Reset();
	TotalUncompressedBytes = 0;
	DestinationPath.Reset();
}

bool FModPackageWriter::ShouldExcludeFromPackage(const FString& InRelativePath)
{
	static const TCHAR* const ExcludedDirectories[] =
	{
		TEXT("Intermediate/"),
		TEXT("Binaries/"),
		TEXT("Saved/"),
		TEXT("DerivedDataCache/"),
		TEXT("Build/")
	};

	FString Normalised = InRelativePath;
	Normalised.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);

	for (const TCHAR* Excluded : ExcludedDirectories)
	{
		if (Normalised.StartsWith(Excluded, ESearchCase::IgnoreCase) ||
			Normalised.Contains(FString(TEXT("/")) + Excluded, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	// Dot-prefixed files and folders are editor/VCS state, never mod content.
	TArray<FString> Segments;
	Normalised.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty*/ true);
	for (const FString& Segment : Segments)
	{
		if (Segment.StartsWith(TEXT("."), ESearchCase::CaseSensitive))
		{
			return true;
		}
	}

	return false;
}

bool FModPackageWriter::PackageDirectory(const FString& InSourceDirectory, const FString& InDestinationFile,
	TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModPackageWriterPrivate;

	const FString SourceDirectory = FPaths::ConvertRelativePathToFull(InSourceDirectory);

	const FString ManifestPath = FPaths::Combine(SourceDirectory, FModManifestParser::GetManifestFileName());
	if (!FPaths::FileExists(ManifestPath))
	{
		OutDiagnostics.Add(MakeWriterError(CodeNoManifest,
			FString::Printf(TEXT("No %s at the root of the mod folder."),
				FModManifestParser::GetManifestFileName()),
			SourceDirectory));
		return false;
	}

	const FModManifestParseResult ParseResult = FModManifestParser::ParseFromFile(ManifestPath);
	OutDiagnostics.Append(ParseResult.Diagnostics);
	if (!ParseResult.bSuccess)
	{
		return false;
	}

	FModPackageWriter Writer;
	if (!Writer.Open(FPaths::ConvertRelativePathToFull(InDestinationFile), OutDiagnostics))
	{
		return false;
	}

	FModDiagnostic Error;
	if (!Writer.SetManifest(ParseResult.Manifest, Error))
	{
		OutDiagnostics.Add(Error);
		return false;
	}

	TArray<FString> FoundFiles;
	IFileManager::Get().FindFilesRecursive(FoundFiles, *SourceDirectory, TEXT("*"),
		/*Files*/ true, /*Directories*/ false);

	// Deterministic order: a package built twice from identical input should be identical, which
	// makes content hashes comparable and diffs meaningful.
	FoundFiles.Sort();

	int32 Excluded = 0;
	for (const FString& AbsoluteFile : FoundFiles)
	{
		FString RelativePath = AbsoluteFile;
		FPaths::MakePathRelativeTo(RelativePath, *(SourceDirectory / TEXT("")));
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);

		// mod.json is stored in the header's manifest region, not as an entry - the reader reads it
		// from there. Including it twice would just be dead weight.
		if (RelativePath.Equals(FModManifestParser::GetManifestFileName(), ESearchCase::IgnoreCase))
		{
			continue;
		}

		if (ShouldExcludeFromPackage(RelativePath))
		{
			++Excluded;
			continue;
		}

		if (!Writer.AddFile(RelativePath, AbsoluteFile, Error))
		{
			OutDiagnostics.Add(Error);
			return false;
		}
	}

	if (Excluded > 0)
	{
		OutDiagnostics.Add(FModDiagnostic::Info(FName(TEXT("Package.Excluded")),
			FString::Printf(TEXT("Skipped %d build-artefact or editor-state file(s)."), Excluded),
			SourceDirectory));
	}

	return Writer.Close(OutDiagnostics);
}
