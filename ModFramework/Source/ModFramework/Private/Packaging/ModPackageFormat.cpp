// Copyright (c) 2026. Licensed for use in your own projects.

#include "Packaging/ModPackageFormat.h"

#include "Containers/AllowShrinking.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "HAL/FileManager.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModManifestParser.h"
#include "Math/NumericLimits.h"
#include "Misc/CString.h"
#include "Misc/Compression.h"
#include "Misc/CompressionFlags.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/Archive.h"
#include "Serialization/MemoryReader.h"
#include "Templates/UniquePtr.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"
#include "UObject/UnrealNames.h"

/**
 * File-local helpers.
 *
 * These live in a named namespace rather than an anonymous one because this module is built with
 * unity files: every anonymous namespace in a unity blob is the *same* namespace, so a generic helper
 * name here would collide with an identically named helper in a sibling .cpp. Call sites qualify
 * explicitly; there is deliberately no `using namespace`.
 */
namespace ModPackageFormatPrivate
{
	/** Stable diagnostic codes. Spelled once so a typo cannot drift between call sites. */
	const TCHAR* const CodeOpenFailed         = TEXT("Package.OpenFailed");
	const TCHAR* const CodeBadMagic           = TEXT("Package.BadMagic");
	const TCHAR* const CodeUnsupportedVersion = TEXT("Package.UnsupportedVersion");
	const TCHAR* const CodeCorrupt            = TEXT("Package.Corrupt");
	const TCHAR* const CodeEntryNotFound      = TEXT("Package.EntryNotFound");
	const TCHAR* const CodeUnsafePath         = TEXT("Package.UnsafePath");
	const TCHAR* const CodeHashMismatch       = TEXT("Package.HashMismatch");
	const TCHAR* const CodeDecompressFailed   = TEXT("Package.DecompressFailed");
	const TCHAR* const CodeExtractFailed      = TEXT("Package.ExtractFailed");
	const TCHAR* const CodeTooLarge           = TEXT("Package.TooLarge");

	// Deliberately NOT named MakeError: Templates/ValueOrError.h declares a global variadic
	// MakeError(ArgTypes&&...) which is an exact match for any argument list. At call sites that pull
	// this namespace in with a using-directive, both land in one overload set and the engine template
	// wins over this overload (which needs a const TCHAR* -> FString conversion), producing a
	// baffling "cannot convert TValueOrError_ErrorProxy" error. Keep the name distinct.
	FModDiagnostic MakePackageError(const TCHAR* Code, FString Message, const FString& Context)
	{
		return FModDiagnostic::Error(FName(Code), MoveTemp(Message), Context);
	}

	void AddError(TArray<FModDiagnostic>& OutDiagnostics, const TCHAR* Code, FString Message, const FString& Context)
	{
		OutDiagnostics.Add(MakePackageError(Code, MoveTemp(Message), Context));
	}

	bool IsHexAnsiChar(ANSICHAR In)
	{
		return (In >= '0' && In <= '9') || (In >= 'a' && In <= 'f') || (In >= 'A' && In <= 'F');
	}

	ANSICHAR ToUpperHexAnsiChar(ANSICHAR In)
	{
		return (In >= 'a' && In <= 'f') ? static_cast<ANSICHAR>(In - 'a' + 'A') : In;
	}

	/**
	 * Renders a hash string into the fixed 40 byte wire form: uppercase hex when the value is a
	 * well-formed SHA-1, 40 '0' characters otherwise (which is how "not computed yet" is spelled).
	 */
	void WriteHashField(const FString& Value, ANSICHAR (&OutBuffer)[ModPackage::HashHexLength])
	{
		bool bUsable = (Value.Len() == ModPackage::HashHexLength);
		if (bUsable)
		{
			for (int32 Index = 0; Index < ModPackage::HashHexLength; ++Index)
			{
				const TCHAR Char = Value[Index];
				if (Char > 127 || !IsHexAnsiChar(static_cast<ANSICHAR>(Char)))
				{
					bUsable = false;
					break;
				}
			}
		}

		for (int32 Index = 0; Index < ModPackage::HashHexLength; ++Index)
		{
			OutBuffer[Index] = bUsable ? ToUpperHexAnsiChar(static_cast<ANSICHAR>(Value[Index])) : '0';
		}
	}

	/**
	 * Reads or writes a hash field: exactly ModPackage::HashHexLength ASCII bytes, always, so the
	 * enclosing record keeps a size that does not depend on its contents. On load a non-hex byte sets
	 * the archive's error flag only after the bytes have been consumed, so the stream stays aligned.
	 */
	void SerializeHashField(FArchive& Ar, FString& Value)
	{
		ANSICHAR Buffer[ModPackage::HashHexLength];

		if (Ar.IsLoading())
		{
			Ar.Serialize(Buffer, ModPackage::HashHexLength);
			if (Ar.IsError())
			{
				Value.Reset();
				return;
			}

			for (int32 Index = 0; Index < ModPackage::HashHexLength; ++Index)
			{
				if (!IsHexAnsiChar(Buffer[Index]))
				{
					Value.Reset();
					Ar.SetError();
					return;
				}

				Buffer[Index] = ToUpperHexAnsiChar(Buffer[Index]);
			}

			Value = FString::ConstructFromPtrSize(Buffer, ModPackage::HashHexLength);
		}
		else
		{
			WriteHashField(Value, Buffer);
			Ar.Serialize(Buffer, ModPackage::HashHexLength);
		}
	}

	/**
	 * Reads or writes `int32 NumBytes` followed by NumBytes of UTF-8 text.
	 *
	 * FArchive's own FString operator is deliberately not used: its loader allocates before it has
	 * anything to validate against, and a `.mod` file is hostile input. Here the length is bounded
	 * before a single byte is reserved.
	 */
	void SerializeBoundedUtf8String(FArchive& Ar, FString& Value, int32 MaxBytes)
	{
		if (Ar.IsLoading())
		{
			int32 NumBytes = 0;
			Ar << NumBytes;

			if (Ar.IsError() || NumBytes < 0 || NumBytes > MaxBytes)
			{
				Value.Reset();
				Ar.SetError();
				return;
			}

			if (NumBytes == 0)
			{
				Value.Reset();
				return;
			}

			TArray<UTF8CHAR> Buffer;
			Buffer.SetNumUninitialized(NumBytes);
			Ar.Serialize(Buffer.GetData(), NumBytes);
			if (Ar.IsError())
			{
				Value.Reset();
				return;
			}

			Value = FString::ConstructFromPtrSize(Buffer.GetData(), NumBytes);
		}
		else
		{
			const auto Converted = StringCast<UTF8CHAR>(*Value, Value.Len());
			int32 NumBytes = Converted.Length();
			Ar << NumBytes;
			if (NumBytes > 0)
			{
				Ar.Serialize(const_cast<UTF8CHAR*>(Converted.Get()), NumBytes);
			}

			// Flagged only after the bytes are written, so the record keeps the size the reader
			// expects. A writer that produced an over-long path has a bug the caller needs to hear.
			if (NumBytes > MaxBytes)
			{
				Ar.SetError();
			}
		}
	}

	/** True for the Windows device names that resolve to a device however they are spelled. */
	bool IsReservedWindowsDeviceName(const FString& Segment)
	{
		// The device is matched on the part before the first '.', so "NUL.txt" is still the NUL device.
		FString Base = Segment;
		int32 DotIndex = INDEX_NONE;
		if (Base.FindChar(TEXT('.'), DotIndex))
		{
			Base.LeftInline(DotIndex, EAllowShrinking::No);
		}

		if (Base.Len() != 3 && Base.Len() != 4)
		{
			return false;
		}

		static const TCHAR* const ThreeLetterDevices[] = { TEXT("CON"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL") };
		for (const TCHAR* Device : ThreeLetterDevices)
		{
			if (Base.Equals(Device, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		if (Base.Len() == 4)
		{
			const FString Prefix = Base.Left(3);
			const TCHAR Digit = Base[3];
			if (Digit >= TEXT('0') && Digit <= TEXT('9')
				&& (Prefix.Equals(TEXT("COM"), ESearchCase::IgnoreCase) || Prefix.Equals(TEXT("LPT"), ESearchCase::IgnoreCase)))
			{
				return true;
			}
		}

		return false;
	}

	/**
	 * Reads Num bytes at Offset.
	 *
	 * Offset and Num must ALREADY have been bounds checked against Ar.TotalSize(): the engine's file
	 * reader hard-checks the argument of Seek, so an out-of-range offset would crash rather than fail.
	 */
	bool ReadRegion(FArchive& Ar, int64 Offset, int64 Num, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();

		if (Num < 0 || Num > MAX_int32)
		{
			return false;
		}

		Ar.Seek(Offset);
		if (Ar.IsError())
		{
			return false;
		}

		if (Num == 0)
		{
			return true;
		}

		OutBytes.SetNumUninitialized(static_cast<int32>(Num));
		Ar.Serialize(OutBytes.GetData(), Num);
		if (Ar.IsError())
		{
			OutBytes.Reset();
			return false;
		}

		return true;
	}

	/** SHA-1 of a byte range as uppercase hex. Handles an empty range without dereferencing null. */
	FString HashBytesToHex(const uint8* Data, int64 Num)
	{
		static const uint8 EmptySentinel = 0;

		const bool bHasData = (Num > 0 && Data != nullptr);
		const void* Buffer = bHasData ? static_cast<const void*>(Data) : static_cast<const void*>(&EmptySentinel);
		const uint64 Size = bHasData ? static_cast<uint64>(Num) : 0;

		return FSHA1::HashBuffer(Buffer, Size).ToString();
	}

	/**
	 * Validates a header that has just been deserialized.
	 *
	 * TotalFileSize is the authoritative size of the open file. Every offset is checked against it
	 * before anything seeks, and the checks are written as subtractions rather than
	 * "Offset + Size <= Total" so a hostile int64 cannot overflow its way past them.
	 */
	bool ValidateHeader(const FModPackageHeader& Header, int64 TotalFileSize, const FString& Context,
		TArray<FModDiagnostic>& OutDiagnostics)
	{
		if (Header.Magic != ModPackage::Magic)
		{
			AddError(OutDiagnostics, CodeBadMagic,
				FString::Printf(TEXT("Not a mod package: expected signature 0x%08X but the file starts with 0x%08X."),
					ModPackage::Magic, Header.Magic),
				Context);
			return false;
		}

		if (Header.FormatVersion == 0 || Header.FormatVersion > ModPackage::CurrentFormatVersion)
		{
			AddError(OutDiagnostics, CodeUnsupportedVersion,
				FString::Printf(TEXT("Package format version %u cannot be read by this build, which understands versions 1 to %u."),
					Header.FormatVersion, ModPackage::CurrentFormatVersion),
				Context);
			return false;
		}

		if (Header.ManifestSize <= 0 || Header.ManifestSize > ModPackage::MaxManifestBytes)
		{
			AddError(OutDiagnostics,
				Header.ManifestSize > ModPackage::MaxManifestBytes ? CodeTooLarge : CodeCorrupt,
				FString::Printf(TEXT("Embedded manifest size %" INT64_FMT " is outside the permitted range 1..%" INT64_FMT " bytes."),
					Header.ManifestSize, ModPackage::MaxManifestBytes),
				Context);
			return false;
		}

		if (Header.ManifestOffset < ModPackage::HeaderSize || Header.ManifestOffset > TotalFileSize - Header.ManifestSize)
		{
			AddError(OutDiagnostics, CodeCorrupt,
				FString::Printf(TEXT("The manifest region (offset %" INT64_FMT ", size %" INT64_FMT ") does not lie inside the %" INT64_FMT " byte file."),
					Header.ManifestOffset, Header.ManifestSize, TotalFileSize),
				Context);
			return false;
		}

		if (Header.TocSize < 4 || Header.TocSize > ModPackage::MaxTocBytes)
		{
			AddError(OutDiagnostics,
				Header.TocSize > ModPackage::MaxTocBytes ? CodeTooLarge : CodeCorrupt,
				FString::Printf(TEXT("Table of contents size %" INT64_FMT " is outside the permitted range 4..%" INT64_FMT " bytes."),
					Header.TocSize, ModPackage::MaxTocBytes),
				Context);
			return false;
		}

		if (Header.TocOffset < ModPackage::HeaderSize || Header.TocOffset > TotalFileSize - Header.TocSize)
		{
			AddError(OutDiagnostics, CodeCorrupt,
				FString::Printf(TEXT("The table of contents (offset %" INT64_FMT ", size %" INT64_FMT ") does not lie inside the %" INT64_FMT " byte file."),
					Header.TocOffset, Header.TocSize, TotalFileSize),
				Context);
			return false;
		}

		return true;
	}

	/** Reads and parses the manifest region. The header must already have been validated. */
	bool ReadManifestRegion(FArchive& Ar, const FModPackageHeader& Header, const FString& Context,
		FModManifest& OutManifest, TArray<FModDiagnostic>& OutDiagnostics)
	{
		TArray<uint8> ManifestBytes;
		if (!ReadRegion(Ar, Header.ManifestOffset, Header.ManifestSize, ManifestBytes))
		{
			AddError(OutDiagnostics, CodeCorrupt,
				FString::Printf(TEXT("Could not read the %" INT64_FMT " byte manifest region at offset %" INT64_FMT "."),
					Header.ManifestSize, Header.ManifestOffset),
				Context);
			return false;
		}

		FString ManifestText;
		FFileHelper::BufferToString(ManifestText, ManifestBytes.GetData(), ManifestBytes.Num());

		const FModManifestParseResult ParseResult = FModManifestParser::ParseFromString(ManifestText, Context);
		OutDiagnostics.Append(ParseResult.Diagnostics);
		OutManifest = ParseResult.Manifest;

		if (!ParseResult.bSuccess)
		{
			AddError(OutDiagnostics, CodeCorrupt,
				TEXT("The manifest embedded in this package is not a valid mod.json."), Context);
			return false;
		}

		return true;
	}

	/**
	 * Reads the table of contents and validates every entry against the real file size.
	 *
	 * The region is copied into a bounded buffer first, so per-entry deserialization physically
	 * cannot read past it however large the declared entry count is.
	 */
	bool ReadTableOfContents(FArchive& Ar, int64 TotalFileSize, const FModPackageHeader& Header, const FString& Context,
		TArray<FModPackageEntry>& OutEntries, TArray<FModDiagnostic>& OutDiagnostics)
	{
		OutEntries.Reset();

		TArray<uint8> TocBytes;
		if (!ReadRegion(Ar, Header.TocOffset, Header.TocSize, TocBytes))
		{
			AddError(OutDiagnostics, CodeCorrupt,
				FString::Printf(TEXT("Could not read the %" INT64_FMT " byte table of contents at offset %" INT64_FMT "."),
					Header.TocSize, Header.TocOffset),
				Context);
			return false;
		}

		FMemoryReader TocReader(TocBytes);

		int32 EntryCount = 0;
		TocReader << EntryCount;
		if (TocReader.IsError() || EntryCount < 0)
		{
			AddError(OutDiagnostics, CodeCorrupt,
				TEXT("The table of contents does not begin with a readable entry count."), Context);
			return false;
		}

		if (EntryCount > ModPackage::MaxEntries)
		{
			AddError(OutDiagnostics, CodeTooLarge,
				FString::Printf(TEXT("The package declares %d entries; this framework refuses more than %d."),
					EntryCount, ModPackage::MaxEntries),
				Context);
			return false;
		}

		// Every entry costs at least 70 bytes on the wire (69 fixed plus a one byte path), so a count
		// the region could not possibly hold is a lie - caught before anything is reserved.
		if (static_cast<int64>(EntryCount) * 70 > Header.TocSize)
		{
			AddError(OutDiagnostics, CodeCorrupt,
				FString::Printf(TEXT("The table of contents claims %d entries but is only %" INT64_FMT " bytes long."),
					EntryCount, Header.TocSize),
				Context);
			return false;
		}

		OutEntries.Reserve(EntryCount);

		TSet<FString> SeenKeys;
		SeenKeys.Reserve(EntryCount);

		int64 TotalUncompressed = 0;

		for (int32 Index = 0; Index < EntryCount; ++Index)
		{
			FModPackageEntry Entry;
			TocReader << Entry;

			if (TocReader.IsError())
			{
				AddError(OutDiagnostics, CodeCorrupt,
					FString::Printf(TEXT("Table of contents entry %d could not be read; the region is truncated or malformed."), Index),
					Context);
				return false;
			}

			FString PathError;
			if (!ModPackage::IsSafeRelativePath(Entry.RelativePath, PathError))
			{
				AddError(OutDiagnostics, CodeUnsafePath,
					FString::Printf(TEXT("Table of contents entry %d has an unusable path '%s': %s"),
						Index, *Entry.RelativePath, *PathError),
					Context);
				return false;
			}

			if (Entry.UncompressedSize < 0 || Entry.UncompressedSize > ModPackage::MaxEntryUncompressedBytes
				|| Entry.CompressedSize < 0 || Entry.CompressedSize > ModPackage::MaxEntryUncompressedBytes)
			{
				AddError(OutDiagnostics, CodeTooLarge,
					FString::Printf(TEXT("Entry '%s' declares sizes (%" INT64_FMT " stored, %" INT64_FMT " uncompressed) outside the permitted range 0..%" INT64_FMT "."),
						*Entry.RelativePath, Entry.CompressedSize, Entry.UncompressedSize, ModPackage::MaxEntryUncompressedBytes),
					Context);
				return false;
			}

			if (!Entry.bCompressed && Entry.CompressedSize != Entry.UncompressedSize)
			{
				AddError(OutDiagnostics, CodeCorrupt,
					FString::Printf(TEXT("Entry '%s' is stored uncompressed but declares %" INT64_FMT " stored bytes for %" INT64_FMT " uncompressed bytes."),
						*Entry.RelativePath, Entry.CompressedSize, Entry.UncompressedSize),
					Context);
				return false;
			}

			if (Entry.bCompressed && Entry.UncompressedSize > 0 && Entry.CompressedSize <= 0)
			{
				AddError(OutDiagnostics, CodeCorrupt,
					FString::Printf(TEXT("Entry '%s' is marked compressed but stores no bytes for %" INT64_FMT " uncompressed bytes."),
						*Entry.RelativePath, Entry.UncompressedSize),
					Context);
				return false;
			}

			if (Entry.Offset < ModPackage::HeaderSize || Entry.Offset > TotalFileSize - Entry.CompressedSize)
			{
				AddError(OutDiagnostics, CodeCorrupt,
					FString::Printf(TEXT("Entry '%s' points at offset %" INT64_FMT " with %" INT64_FMT " stored bytes, which does not lie inside the %" INT64_FMT " byte file."),
						*Entry.RelativePath, Entry.Offset, Entry.CompressedSize, TotalFileSize),
					Context);
				return false;
			}

			TotalUncompressed += Entry.UncompressedSize;
			if (TotalUncompressed > ModPackage::MaxTotalUncompressedBytes)
			{
				AddError(OutDiagnostics, CodeTooLarge,
					FString::Printf(TEXT("The package expands to more than the permitted %" INT64_FMT " bytes."),
						ModPackage::MaxTotalUncompressedBytes),
					Context);
				return false;
			}

			bool bAlreadySeen = false;
			SeenKeys.Add(ModPackage::MakeEntryKey(Entry.RelativePath), &bAlreadySeen);
			if (bAlreadySeen)
			{
				AddError(OutDiagnostics, CodeCorrupt,
					FString::Printf(TEXT("The table of contents lists '%s' more than once."), *Entry.RelativePath),
					Context);
				return false;
			}

			OutEntries.Add(MoveTemp(Entry));
		}

		return true;
	}
}

FArchive& operator<<(FArchive& Ar, FModPackageEntry& E)
{
	ModPackageFormatPrivate::SerializeBoundedUtf8String(Ar, E.RelativePath, ModPackage::MaxEntryPathBytes);
	Ar << E.Offset;
	Ar << E.CompressedSize;
	Ar << E.UncompressedSize;

	uint8 CompressedFlag = E.bCompressed ? 1 : 0;
	Ar << CompressedFlag;
	if (Ar.IsLoading())
	{
		E.bCompressed = (CompressedFlag != 0);
	}

	ModPackageFormatPrivate::SerializeHashField(Ar, E.Hash);
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FModPackageHeader& H)
{
	Ar << H.Magic;
	Ar << H.FormatVersion;
	Ar << H.ManifestOffset;
	Ar << H.ManifestSize;
	Ar << H.TocOffset;
	Ar << H.TocSize;
	ModPackageFormatPrivate::SerializeHashField(Ar, H.ContentHash);
	return Ar;
}

namespace ModPackage
{
	const TCHAR* GetFileExtension()
	{
		return TEXT(".mod");
	}

	bool IsSafeRelativePath(const FString& In, FString& OutError)
	{
		OutError.Reset();

		if (In.IsEmpty())
		{
			OutError = TEXT("the path is empty");
			return false;
		}

		if (In.Len() > MaxEntryPathBytes)
		{
			OutError = FString::Printf(TEXT("the path is %d characters long, more than the permitted %d"),
				In.Len(), MaxEntryPathBytes);
			return false;
		}

		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			const TCHAR Char = In[Index];

			if (Char == TEXT('\0'))
			{
				OutError = TEXT("the path contains a NUL character");
				return false;
			}

			if (Char < 0x20)
			{
				OutError = FString::Printf(TEXT("the path contains the control character 0x%02X at index %d"),
					static_cast<uint32>(Char), Index);
				return false;
			}

			// '\\' and ':' between them rule out drive letters, UNC paths and NTFS alternate data
			// streams; the rest are illegal on Windows and would be shell wildcards elsewhere.
			if (Char == TEXT('\\') || Char == TEXT(':') || Char == TEXT('*') || Char == TEXT('?')
				|| Char == TEXT('"') || Char == TEXT('<') || Char == TEXT('>') || Char == TEXT('|'))
			{
				OutError = FString::Printf(TEXT("the path contains the forbidden character '%c' at index %d"), Char, Index);
				return false;
			}
		}

		if (In[0] == TEXT('/'))
		{
			OutError = TEXT("the path is absolute; entry paths are always relative to the package root");
			return false;
		}

		TArray<FString> Segments;
		In.ParseIntoArray(Segments, TEXT("/"), /*bInCullEmpty=*/false);

		if (Segments.Num() == 0 || Segments.Num() > MaxEntryPathSegments)
		{
			OutError = FString::Printf(TEXT("the path has %d segments; between 1 and %d are permitted"),
				Segments.Num(), MaxEntryPathSegments);
			return false;
		}

		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty())
			{
				OutError = TEXT("the path has an empty segment (a doubled or trailing '/')");
				return false;
			}

			if (Segment.Equals(TEXT("."), ESearchCase::CaseSensitive) || Segment.Equals(TEXT(".."), ESearchCase::CaseSensitive))
			{
				OutError = FString::Printf(TEXT("the path contains the relative segment '%s'"), *Segment);
				return false;
			}

			const TCHAR LastChar = Segment[Segment.Len() - 1];
			if (LastChar == TEXT('.') || LastChar == TEXT(' '))
			{
				OutError = FString::Printf(TEXT("the segment '%s' ends in '%c', which some filesystems silently strip"),
					*Segment, LastChar);
				return false;
			}

			if (ModPackageFormatPrivate::IsReservedWindowsDeviceName(Segment))
			{
				OutError = FString::Printf(TEXT("the segment '%s' names a reserved device"), *Segment);
				return false;
			}
		}

		return true;
	}

	FString MakeEntryKey(const FString& In)
	{
		FString Key = In;
		Key.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);

		// Strip any mix of leading "./" and "/" so "./a/b", "/a/b" and "a/b" all fold together.
		for (;;)
		{
			if (Key.StartsWith(TEXT("./"), ESearchCase::CaseSensitive))
			{
				Key.RightChopInline(2, EAllowShrinking::No);
			}
			else if (Key.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
			{
				Key.RightChopInline(1, EAllowShrinking::No);
			}
			else
			{
				break;
			}
		}

		return Key.ToLower();
	}
}

FModPackageReader::FModPackageReader() = default;

FModPackageReader::~FModPackageReader()
{
	Close();
}

bool FModPackageReader::Open(const FString& AbsolutePath, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModPackageFormatPrivate;

	Close();

	if (AbsolutePath.IsEmpty())
	{
		AddError(OutDiagnostics, CodeOpenFailed, TEXT("No package path was supplied."), AbsolutePath);
		return false;
	}

	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*AbsolutePath, FILEREAD_Silent));
	if (!Reader.IsValid())
	{
		AddError(OutDiagnostics, CodeOpenFailed, TEXT("The package file could not be opened for reading."), AbsolutePath);
		return false;
	}

	// The archive's own idea of the file size is what its Seek() asserts against, so that - and not a
	// separate stat call that could disagree - is what every bounds check below uses.
	const int64 TotalFileSize = Reader->TotalSize();
	if (TotalFileSize < ModPackage::HeaderSize)
	{
		AddError(OutDiagnostics, CodeCorrupt,
			FString::Printf(TEXT("The file is %" INT64_FMT " bytes long, too short to hold the %" INT64_FMT " byte package header."),
				TotalFileSize, ModPackage::HeaderSize),
			AbsolutePath);
		return false;
	}

	FModPackageHeader LocalHeader;
	Reader->Seek(0);
	*Reader << LocalHeader;
	if (Reader->IsError())
	{
		AddError(OutDiagnostics, CodeCorrupt, TEXT("The package header could not be read."), AbsolutePath);
		return false;
	}

	if (!ValidateHeader(LocalHeader, TotalFileSize, AbsolutePath, OutDiagnostics))
	{
		return false;
	}

	FModManifest LocalManifest;
	if (!ReadManifestRegion(*Reader, LocalHeader, AbsolutePath, LocalManifest, OutDiagnostics))
	{
		return false;
	}

	TArray<FModPackageEntry> LocalEntries;
	if (!ReadTableOfContents(*Reader, TotalFileSize, LocalHeader, AbsolutePath, LocalEntries, OutDiagnostics))
	{
		return false;
	}

	EntryIndexByKey.Reserve(LocalEntries.Num());
	for (int32 Index = 0; Index < LocalEntries.Num(); ++Index)
	{
		EntryIndexByKey.Add(ModPackage::MakeEntryKey(LocalEntries[Index].RelativePath), Index);
	}

	Archive = MoveTemp(Reader);
	PackagePath = AbsolutePath;
	PackageFileSize = TotalFileSize;
	Header = MoveTemp(LocalHeader);
	Manifest = MoveTemp(LocalManifest);
	Entries = MoveTemp(LocalEntries);
	bIsOpen = true;

	UE_LOG(LogModFramework, Verbose, TEXT("Opened mod package '%s': %d entr%s, mod id '%s'."),
		*PackagePath, Entries.Num(), Entries.Num() == 1 ? TEXT("y") : TEXT("ies"), *Manifest.Id.ToString());

	return true;
}

void FModPackageReader::Close()
{
	if (Archive.IsValid())
	{
		Archive->Close();
		Archive.Reset();
	}

	PackagePath.Reset();
	PackageFileSize = 0;
	Header = FModPackageHeader();
	Manifest = FModManifest();
	Entries.Reset();
	EntryIndexByKey.Reset();
	bIsOpen = false;
}

bool FModPackageReader::IsOpen() const
{
	return bIsOpen && Archive.IsValid();
}

const FModManifest& FModPackageReader::GetManifest() const
{
	return Manifest;
}

const FModPackageHeader& FModPackageReader::GetHeader() const
{
	return Header;
}

TArray<FModPackageEntry> FModPackageReader::GetEntries() const
{
	return Entries;
}

const FString& FModPackageReader::GetPackagePath() const
{
	return PackagePath;
}

bool FModPackageReader::HasEntry(const FString& RelativePath) const
{
	return EntryIndexByKey.Contains(ModPackage::MakeEntryKey(RelativePath));
}

bool FModPackageReader::FindEntry(const FString& RelativePath, FModPackageEntry& OutEntry) const
{
	if (const int32* Index = EntryIndexByKey.Find(ModPackage::MakeEntryKey(RelativePath)))
	{
		if (Entries.IsValidIndex(*Index))
		{
			OutEntry = Entries[*Index];
			return true;
		}
	}

	return false;
}

bool FModPackageReader::ReadEntry(const FString& RelativePath, TArray<uint8>& OutBytes, FModDiagnostic& OutError)
{
	using namespace ModPackageFormatPrivate;

	OutBytes.Reset();

	if (!IsOpen())
	{
		OutError = MakePackageError(CodeOpenFailed, TEXT("The package is not open."), PackagePath);
		return false;
	}

	const int32* FoundIndex = EntryIndexByKey.Find(ModPackage::MakeEntryKey(RelativePath));
	if (FoundIndex == nullptr || !Entries.IsValidIndex(*FoundIndex))
	{
		OutError = MakePackageError(CodeEntryNotFound,
			FString::Printf(TEXT("The package does not contain an entry named '%s'."), *RelativePath),
			PackagePath);
		return false;
	}

	return ReadEntryAtIndex(*FoundIndex, OutBytes, OutError);
}

bool FModPackageReader::ReadEntryAtIndex(int32 Index, TArray<uint8>& OutBytes, FModDiagnostic& OutError)
{
	using namespace ModPackageFormatPrivate;

	OutBytes.Reset();

	if (!IsOpen() || !Entries.IsValidIndex(Index))
	{
		OutError = MakePackageError(CodeOpenFailed, TEXT("The package is not open."), PackagePath);
		return false;
	}

	const FModPackageEntry& Entry = Entries[Index];

	// Open() already checked this, but the seek below hard-asserts on an out-of-range position, so
	// the invariant is re-established here rather than trusted.
	if (Entry.Offset < ModPackage::HeaderSize
		|| Entry.CompressedSize < 0
		|| Entry.CompressedSize > ModPackage::MaxEntryUncompressedBytes
		|| Entry.UncompressedSize < 0
		|| Entry.UncompressedSize > ModPackage::MaxEntryUncompressedBytes
		|| Entry.Offset > PackageFileSize - Entry.CompressedSize)
	{
		OutError = MakePackageError(CodeCorrupt,
			FString::Printf(TEXT("Entry '%s' does not lie inside the package file."), *Entry.RelativePath),
			PackagePath);
		return false;
	}

	TArray<uint8> Stored;
	if (!ReadRegion(*Archive, Entry.Offset, Entry.CompressedSize, Stored))
	{
		OutError = MakePackageError(CodeCorrupt,
			FString::Printf(TEXT("Could not read the %" INT64_FMT " stored bytes of entry '%s'."),
				Entry.CompressedSize, *Entry.RelativePath),
			PackagePath);
		return false;
	}

	if (Entry.UncompressedSize == 0)
	{
		// Legal: an empty file inside the package. There is nothing to decompress.
		OutBytes.Reset();
	}
	else if (Entry.bCompressed)
	{
		TArray<uint8> Uncompressed;
		Uncompressed.SetNumUninitialized(static_cast<int32>(Entry.UncompressedSize));

		if (!FCompression::UncompressMemory(NAME_Zlib, Uncompressed.GetData(), Entry.UncompressedSize,
			Stored.GetData(), Entry.CompressedSize, COMPRESS_NoFlags))
		{
			OutError = MakePackageError(CodeDecompressFailed,
				FString::Printf(TEXT("Entry '%s' could not be decompressed; the payload is not a valid zlib block for %" INT64_FMT " bytes."),
					*Entry.RelativePath, Entry.UncompressedSize),
				PackagePath);
			return false;
		}

		OutBytes = MoveTemp(Uncompressed);
	}
	else
	{
		OutBytes = MoveTemp(Stored);
	}

	if (OutBytes.Num() != static_cast<int32>(Entry.UncompressedSize))
	{
		OutBytes.Reset();
		OutError = MakePackageError(CodeCorrupt,
			FString::Printf(TEXT("Entry '%s' produced a different number of bytes than the table of contents declares."),
				*Entry.RelativePath),
			PackagePath);
		return false;
	}

	const FString ActualHash = HashBytesToHex(OutBytes.GetData(), OutBytes.Num());
	if (!ActualHash.Equals(Entry.Hash, ESearchCase::IgnoreCase))
	{
		OutBytes.Reset();
		OutError = MakePackageError(CodeHashMismatch,
			FString::Printf(TEXT("Entry '%s' hashes to %s but the table of contents records %s."),
				*Entry.RelativePath, *ActualHash, *Entry.Hash),
			PackagePath);
		return false;
	}

	return true;
}

bool FModPackageReader::ExtractAll(const FString& DestinationDirectory, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModPackageFormatPrivate;

	if (!IsOpen())
	{
		AddError(OutDiagnostics, CodeOpenFailed, TEXT("The package is not open."), PackagePath);
		return false;
	}

	if (DestinationDirectory.IsEmpty())
	{
		AddError(OutDiagnostics, CodeExtractFailed, TEXT("No destination directory was supplied."), PackagePath);
		return false;
	}

	FString DestinationRoot = FPaths::ConvertRelativePathToFull(DestinationDirectory);
	FPaths::NormalizeDirectoryName(DestinationRoot);

	if (DestinationRoot.IsEmpty())
	{
		AddError(OutDiagnostics, CodeExtractFailed,
			FString::Printf(TEXT("The destination directory '%s' does not resolve to a usable path."), *DestinationDirectory),
			PackagePath);
		return false;
	}

	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.MakeDirectory(*DestinationRoot, /*Tree=*/true) && !FileManager.DirectoryExists(*DestinationRoot))
	{
		AddError(OutDiagnostics, CodeExtractFailed,
			FString::Printf(TEXT("The destination directory '%s' could not be created."), *DestinationRoot),
			PackagePath);
		return false;
	}

	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		const FString EntryPath = Entries[Index].RelativePath;

		// Re-validated at write time: the guarantee that matters is about the RESOLVED absolute path,
		// not about the text that was stored in the table of contents.
		FString PathError;
		if (!ModPackage::IsSafeRelativePath(EntryPath, PathError))
		{
			AddError(OutDiagnostics, CodeUnsafePath,
				FString::Printf(TEXT("Refusing to extract '%s': %s"), *EntryPath, *PathError),
				PackagePath);
			return false;
		}

		const FString TargetPath = FPaths::ConvertRelativePathToFull(DestinationRoot / EntryPath);
		if (TargetPath.Len() <= DestinationRoot.Len() || !FPaths::IsUnderDirectory(TargetPath, DestinationRoot))
		{
			AddError(OutDiagnostics, CodeUnsafePath,
				FString::Printf(TEXT("Refusing to extract '%s': it resolves to '%s', which is outside '%s'."),
					*EntryPath, *TargetPath, *DestinationRoot),
				PackagePath);
			return false;
		}

		TArray<uint8> Bytes;
		FModDiagnostic ReadError;
		if (!ReadEntryAtIndex(Index, Bytes, ReadError))
		{
			OutDiagnostics.Add(MoveTemp(ReadError));
			return false;
		}

		const FString TargetDirectory = FPaths::GetPath(TargetPath);
		if (!TargetDirectory.IsEmpty()
			&& !FileManager.MakeDirectory(*TargetDirectory, /*Tree=*/true)
			&& !FileManager.DirectoryExists(*TargetDirectory))
		{
			AddError(OutDiagnostics, CodeExtractFailed,
				FString::Printf(TEXT("The directory '%s' could not be created."), *TargetDirectory),
				PackagePath);
			return false;
		}

		if (!FFileHelper::SaveArrayToFile(Bytes, *TargetPath))
		{
			AddError(OutDiagnostics, CodeExtractFailed,
				FString::Printf(TEXT("The file '%s' could not be written."), *TargetPath),
				PackagePath);
			return false;
		}
	}

	UE_LOG(LogModFramework, Verbose, TEXT("Extracted %d entries from '%s' into '%s'."),
		Entries.Num(), *PackagePath, *DestinationRoot);

	return true;
}

bool FModPackageReader::VerifyIntegrity(TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModPackageFormatPrivate;

	if (!IsOpen())
	{
		AddError(OutDiagnostics, CodeOpenFailed, TEXT("The package is not open."), PackagePath);
		return false;
	}

	bool bOk = true;

	// Deliberately does not stop at the first failure: a mod author fixing a broken package wants the
	// whole list, not one entry per attempt.
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		TArray<uint8> Bytes;
		FModDiagnostic ReadError;
		if (!ReadEntryAtIndex(Index, Bytes, ReadError))
		{
			OutDiagnostics.Add(MoveTemp(ReadError));
			bOk = false;
		}
	}

	const FString ExpectedContentHash = ComputeContentHash(Entries);
	if (!ExpectedContentHash.Equals(Header.ContentHash, ESearchCase::IgnoreCase))
	{
		AddError(OutDiagnostics, CodeHashMismatch,
			FString::Printf(TEXT("The table of contents hashes to %s but the package header records %s."),
				*ExpectedContentHash, *Header.ContentHash),
			PackagePath);
		bOk = false;
	}

	return bOk;
}

bool FModPackageReader::PeekManifest(const FString& AbsolutePath, FModManifest& OutManifest, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModPackageFormatPrivate;

	OutManifest = FModManifest();

	if (AbsolutePath.IsEmpty())
	{
		AddError(OutDiagnostics, CodeOpenFailed, TEXT("No package path was supplied."), AbsolutePath);
		return false;
	}

	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*AbsolutePath, FILEREAD_Silent));
	if (!Reader.IsValid())
	{
		AddError(OutDiagnostics, CodeOpenFailed, TEXT("The package file could not be opened for reading."), AbsolutePath);
		return false;
	}

	const int64 TotalFileSize = Reader->TotalSize();
	if (TotalFileSize < ModPackage::HeaderSize)
	{
		AddError(OutDiagnostics, CodeCorrupt,
			FString::Printf(TEXT("The file is %" INT64_FMT " bytes long, too short to hold the %" INT64_FMT " byte package header."),
				TotalFileSize, ModPackage::HeaderSize),
			AbsolutePath);
		return false;
	}

	FModPackageHeader LocalHeader;
	Reader->Seek(0);
	*Reader << LocalHeader;
	if (Reader->IsError())
	{
		AddError(OutDiagnostics, CodeCorrupt, TEXT("The package header could not be read."), AbsolutePath);
		return false;
	}

	if (!ValidateHeader(LocalHeader, TotalFileSize, AbsolutePath, OutDiagnostics))
	{
		return false;
	}

	const bool bParsed = ReadManifestRegion(*Reader, LocalHeader, AbsolutePath, OutManifest, OutDiagnostics);
	Reader->Close();
	return bParsed;
}

FString FModPackageReader::ComputeContentHash(const TArray<FModPackageEntry>& Entries)
{
	const uint8 Separator = 0x0A;

	FSHA1 Sha;
	for (const FModPackageEntry& Entry : Entries)
	{
		const auto PathUtf8 = StringCast<UTF8CHAR>(*Entry.RelativePath, Entry.RelativePath.Len());
		if (PathUtf8.Length() > 0)
		{
			Sha.Update(reinterpret_cast<const uint8*>(PathUtf8.Get()), static_cast<uint64>(PathUtf8.Length()));
		}
		Sha.Update(&Separator, 1);

		ANSICHAR HashField[ModPackage::HashHexLength];
		ModPackageFormatPrivate::WriteHashField(Entry.Hash, HashField);
		Sha.Update(reinterpret_cast<const uint8*>(HashField), static_cast<uint64>(ModPackage::HashHexLength));
		Sha.Update(&Separator, 1);
	}

	return Sha.Finalize().ToString();
}
