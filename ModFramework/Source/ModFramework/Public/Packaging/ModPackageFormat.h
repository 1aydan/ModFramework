// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "Core/ModFrameworkVersion.h"
#include "CoreTypes.h"
#include "Manifest/ModManifest.h"
#include "Templates/UniquePtr.h"
#include "UObject/ObjectMacros.h"

#include "ModPackageFormat.generated.h"

class FArchive;

/**
 * =====================================================================================================
 *  THE `.mod` CONTAINER FORMAT - NORMATIVE ON-DISK LAYOUT (format version 1)
 * =====================================================================================================
 *
 * A `.mod` file is a plain file written with a single FArchive. It is deliberately trivial: no
 * third-party dependency, no engine package format, nothing that changes between engine versions.
 * The reader lives here (FModPackageReader); the writer lives in the ModFrameworkDeveloper module
 * (FModPackageWriter) and MUST produce exactly the bytes described below.
 *
 * ---------------------------------------------------------------------------------------------------
 *  FILE STRUCTURE
 * ---------------------------------------------------------------------------------------------------
 *
 *     +--------------------------------------------------+  offset 0
 *     |  header            (fixed, ModPackage::HeaderSize = 80 bytes)
 *     +--------------------------------------------------+  offset = Header.ManifestOffset
 *     |  manifest          (Header.ManifestSize bytes of UTF-8 `mod.json` text)
 *     +--------------------------------------------------+  offset = Header.TocOffset
 *     |  table of contents (Header.TocSize bytes)
 *     +--------------------------------------------------+  offset = Entry[i].Offset
 *     |  payload bytes     (one blob per TOC entry, in any order)
 *     +--------------------------------------------------+  end of file
 *
 * The canonical writing order is header, manifest, TOC, payloads - which is why the numeric header
 * fields cannot be known when the header is first written. The header is therefore FIXED SIZE: write
 * 80 zero-filled bytes (or a header with placeholder values) at offset 0, stream the rest, then
 * `Seek(0)` and re-serialise the completed header over the placeholder. Nothing else in the file
 * moves, because every field of the header has a size that does not depend on its value.
 *
 * ---------------------------------------------------------------------------------------------------
 *  ENDIANNESS AND PRIMITIVE ENCODING
 * ---------------------------------------------------------------------------------------------------
 *
 * All multi-byte integers are stored little-endian, exactly as FArchive's `operator<<` writes them on
 * a little-endian host with byte swapping disabled (the default for a file archive). `int32` is 4
 * bytes, `uint32` is 4 bytes, `int64` is 8 bytes. Booleans are stored as a single `uint8` holding 0
 * or 1.
 *
 * Strings are NEVER written with FArchive's built-in `operator<<(FString&)`. That encoding is
 * variable (ANSI vs UTF-16, signalled by the sign of the length) and its loader will happily allocate
 * two gigabytes when handed a hostile length. Instead:
 *
 *   - a "bounded UTF-8 string" is  `int32 NumBytes` followed by exactly NumBytes bytes of UTF-8 text
 *     with no BOM and no NUL terminator. The reader rejects NumBytes < 0 and NumBytes > the documented
 *     cap for that field before allocating anything.
 *   - a "hash field" is exactly ModPackage::HashHexLength (40) ASCII bytes holding the UPPERCASE
 *     hexadecimal SHA-1 digest. A hash that is not yet known is written as 40 '0' characters. The
 *     reader rejects any byte that is not [0-9A-Fa-f] and normalises what it reads to upper case.
 *
 * ---------------------------------------------------------------------------------------------------
 *  HEADER - exactly 80 bytes at offset 0
 * ---------------------------------------------------------------------------------------------------
 *
 *     offset  size  type      field
 *     ------  ----  --------  ---------------------------------------------------------------
 *          0     4  uint32    Magic           always ModPackage::Magic (0x444F4D55, 'U','M','O','D'
 *                                             when read as bytes in file order)
 *          4     4  uint32    FormatVersion   1 for this revision; a reader refuses 0 and anything
 *                                             greater than ModPackage::CurrentFormatVersion
 *          8     8  int64     ManifestOffset  absolute file offset of the manifest text
 *         16     8  int64     ManifestSize    length in bytes of the manifest text
 *         24     8  int64     TocOffset       absolute file offset of the table of contents
 *         32     8  int64     TocSize         length in bytes of the table of contents
 *         40    40  ASCII[40] ContentHash     FModPackageReader::ComputeContentHash of the final TOC
 *     ------  ----
 *         80        = ModPackage::HeaderSize
 *
 * ---------------------------------------------------------------------------------------------------
 *  MANIFEST REGION
 * ---------------------------------------------------------------------------------------------------
 *
 * `Header.ManifestSize` bytes of UTF-8 encoded `mod.json`, byte-identical to what
 * FModManifestParser::SerializeToString produced. A leading UTF-8 BOM is tolerated by the reader but
 * should not be written. The region must be non-empty and no larger than
 * ModPackage::MaxManifestBytes.
 *
 * ---------------------------------------------------------------------------------------------------
 *  TABLE OF CONTENTS REGION
 * ---------------------------------------------------------------------------------------------------
 *
 *     int32 EntryCount                      0 .. ModPackage::MaxEntries
 *     FModPackageEntry Entries[EntryCount]  serialised with operator<<(FArchive&, FModPackageEntry&)
 *
 * and one FModPackageEntry is:
 *
 *     int32     PathNumBytes    1 .. ModPackage::MaxEntryPathBytes
 *     UTF8[N]   RelativePath    forward slashes only, no leading slash, no "..", no drive letter
 *     int64     Offset          absolute file offset of this entry's payload
 *     int64     CompressedSize  bytes actually stored in the file
 *     int64     UncompressedSize bytes after decompression
 *     uint8     bCompressed     0 = stored verbatim, 1 = zlib (NAME_Zlib) raw block
 *     ASCII[40] Hash            SHA-1 of the UNCOMPRESSED bytes, uppercase hex
 *
 * so an entry occupies `69 + PathNumBytes` bytes. `Header.TocSize` must equal the exact number of
 * bytes the writer emitted for the count plus every entry.
 *
 * Invariants the reader enforces on every entry, rejecting the whole package when any of them fails:
 *   - RelativePath satisfies ModPackage::IsSafeRelativePath (see that function for the full rule set)
 *   - RelativePath is unique across the TOC, compared case-insensitively
 *   - 0 <= UncompressedSize <= ModPackage::MaxEntryUncompressedBytes
 *   - 0 <= CompressedSize   <= ModPackage::MaxEntryUncompressedBytes
 *   - when bCompressed is 0, CompressedSize == UncompressedSize (the format is unambiguous: a stored
 *     entry has exactly one size)
 *   - when bCompressed is 1 and UncompressedSize > 0, CompressedSize > 0
 *   - ModPackage::HeaderSize <= Offset and Offset + CompressedSize <= the real size of the file
 *   - the sum of every UncompressedSize is at most ModPackage::MaxTotalUncompressedBytes
 *
 * ---------------------------------------------------------------------------------------------------
 *  PAYLOADS
 * ---------------------------------------------------------------------------------------------------
 *
 * `Entry.CompressedSize` bytes at `Entry.Offset`. When `bCompressed` is set they are one zlib block
 * produced by `FCompression::CompressMemory(NAME_Zlib, ...)` and consumed by
 * `FCompression::UncompressMemory(NAME_Zlib, Dst, UncompressedSize, Src, CompressedSize)` - a single
 * block, never chunked. Payloads may appear in any order and may not overlap the header, the manifest
 * or the TOC; the reader only enforces that they lie inside the file.
 *
 * A zero-length entry (an empty file inside the package) is legal: UncompressedSize == 0,
 * CompressedSize == 0, bCompressed == 0, and Hash is the SHA-1 of the empty buffer.
 *
 * ---------------------------------------------------------------------------------------------------
 *  WRITING A PACKAGE (the recipe the writer in ModFrameworkDeveloper follows)
 * ---------------------------------------------------------------------------------------------------
 *
 *   1. Create the file archive. Serialise a default FModPackageHeader - 80 bytes of placeholder.
 *   2. Record `ManifestOffset = Ar->Tell()`, write the UTF-8 manifest bytes, record `ManifestSize`.
 *   3. Build the entry list in memory: for each source file compute the SHA-1 of its uncompressed
 *      bytes (FSHA1::HashBuffer -> FSHAHash::ToString), decide whether to compress it, and remember
 *      the payload blob. Offsets are not known yet.
 *   4. Reserve the TOC: `TocOffset = Ar->Tell()`, serialise `EntryCount` and every entry with
 *      placeholder offsets, `TocSize = Ar->Tell() - TocOffset`.
 *   5. Append every payload, filling in each `Entry.Offset` as it is written.
 *   6. `Seek(TocOffset)` and re-serialise the TOC now that the offsets are known. Because every entry
 *      field except the path is fixed size and the path did not change, the region is byte-for-byte
 *      the same length as the placeholder written in step 4.
 *   7. `Header.ContentHash = FModPackageReader::ComputeContentHash(Entries)`. Call that function -
 *      do not reimplement the digest, so the reader and the writer can never drift apart.
 *   8. `Seek(0)` and re-serialise the completed header.
 *
 * =====================================================================================================
 */
namespace ModPackage
{
	/** File signature: 'U','M','O','D' in file order on a little-endian host. */
	inline constexpr uint32 Magic = 0x444F4D55;

	/** The layout revision this module writes and is able to read. */
	inline constexpr uint32 CurrentFormatVersion = static_cast<uint32>(MODFRAMEWORK_PACKAGE_FORMAT_VERSION);

	/** Serialised size of FModPackageHeader, in bytes. Fixed by design; see the layout table above. */
	inline constexpr int64 HeaderSize = 80;

	/** Number of ASCII characters in a serialised SHA-1 hash field. */
	inline constexpr int32 HashHexLength = 40;

	/** Hard cap on the number of TOC entries, checked before any per-entry allocation happens. */
	inline constexpr int32 MaxEntries = 200000;

	/** Hard cap on the UTF-8 byte length of one entry path, checked before the path is allocated. */
	inline constexpr int32 MaxEntryPathBytes = 1024;

	/** Hard cap on how many '/' separated segments one entry path may have. */
	inline constexpr int32 MaxEntryPathSegments = 64;

	/** Hard cap on the embedded manifest, so a hostile header cannot ask for a huge allocation. */
	inline constexpr int64 MaxManifestBytes = 4 * 1024 * 1024;

	/** Hard cap on the table of contents region. */
	inline constexpr int64 MaxTocBytes = 256 * 1024 * 1024;

	/** Hard cap on a single entry, compressed or not. Keeps every buffer inside TArray's int32 index. */
	inline constexpr int64 MaxEntryUncompressedBytes = 512 * 1024 * 1024;

	/** Hard cap on the sum of every entry's uncompressed size - the zip-bomb guard. 8 GiB. */
	inline constexpr int64 MaxTotalUncompressedBytes = 8LL * 1024 * 1024 * 1024;

	/** The extension a packaged mod uses, including the dot: ".mod". */
	MODFRAMEWORK_API const TCHAR* GetFileExtension();

	/**
	 * Decides whether a path read out of a package may be used, either to look an entry up or to
	 * write a file during extraction. Everything in a package is untrusted, so this is deliberately
	 * strict and rejects anything that is merely unusual.
	 *
	 * A path is safe when ALL of the following hold:
	 *   - it is not empty, and at most MaxEntryPathBytes characters long;
	 *   - it contains no NUL, no character below 0x20, and none of  \  :  *  ?  "  <  >  |
	 *     (this alone kills "C:/...", UNC paths, NTFS alternate data streams and shell wildcards);
	 *   - it does not begin with '/';
	 *   - splitting on '/' yields between 1 and MaxEntryPathSegments segments, none of them empty
	 *     (so "a//b" and a trailing '/' are rejected), none of them "." or "..";
	 *   - no segment ends in '.' or ' ' - Windows silently strips those, which would let two distinct
	 *     TOC entries resolve to the same file on disk;
	 *   - no segment names a reserved Windows device, with or without an extension: CON, PRN, AUX,
	 *     NUL, COM0..COM9, LPT0..LPT9 (compared case-insensitively).
	 *
	 * @param OutError  Filled with a human-readable reason when the path is rejected, emptied when it
	 *                  is accepted.
	 * @return true when the path is safe to use as a relative path under a destination directory.
	 */
	MODFRAMEWORK_API bool IsSafeRelativePath(const FString& In, FString& OutError);

	/**
	 * Folds a path into the form used as a TOC lookup key: backslashes become forward slashes, a
	 * leading "./" or '/' is removed, and the result is lower-cased. Used so that ReadEntry("A/B.txt")
	 * finds an entry stored as "a/b.txt" - packages are authored on case-insensitive filesystems more
	 * often than not. This normalises only; it does NOT make an unsafe path safe.
	 */
	MODFRAMEWORK_API FString MakeEntryKey(const FString& In);
}

/**
 * One file inside a `.mod` package.
 *
 * Offsets and sizes describe bytes in the container, not on disk after extraction. `Hash` is always
 * the SHA-1 of the UNCOMPRESSED contents, so it stays meaningful whether or not the entry happens to
 * be compressed, and it is what both FModPackageReader::ReadEntry and
 * FModPackageReader::VerifyIntegrity check against.
 */
USTRUCT()
struct MODFRAMEWORK_API FModPackageEntry
{
	GENERATED_BODY()

	/** Forward slashes, no leading slash, no "..". See ModPackage::IsSafeRelativePath. */
	UPROPERTY()
	FString RelativePath;

	/** Absolute offset of the payload inside the package file. */
	UPROPERTY()
	int64 Offset = 0;

	/** Bytes actually stored in the file. Equal to UncompressedSize when bCompressed is false. */
	UPROPERTY()
	int64 CompressedSize = 0;

	/** Bytes after decompression. */
	UPROPERTY()
	int64 UncompressedSize = 0;

	/** True when the payload is a single zlib (NAME_Zlib) block rather than raw bytes. */
	UPROPERTY()
	bool bCompressed = false;

	/** Uppercase hex SHA-1 of the uncompressed bytes; exactly ModPackage::HashHexLength characters. */
	UPROPERTY()
	FString Hash;

	/** See the layout block at the top of this file. Sets the archive's error flag on bad input. */
	friend MODFRAMEWORK_API FArchive& operator<<(FArchive& Ar, FModPackageEntry& E);
};

/**
 * The fixed 80 byte prologue of a `.mod` package.
 *
 * Every field has a size independent of its value, which is what lets a writer emit a placeholder,
 * stream the body, and then seek back to offset 0 to stamp the real offsets and content hash without
 * moving a single byte of the rest of the file.
 */
USTRUCT()
struct MODFRAMEWORK_API FModPackageHeader
{
	GENERATED_BODY()

	/** Must equal ModPackage::Magic. Checked before anything else is read. */
	UPROPERTY()
	uint32 Magic = ModPackage::Magic;

	/** Layout revision. A reader refuses 0 and anything above ModPackage::CurrentFormatVersion. */
	UPROPERTY()
	uint32 FormatVersion = ModPackage::CurrentFormatVersion;

	/** Absolute offset of the UTF-8 manifest text. */
	UPROPERTY()
	int64 ManifestOffset = 0;

	/** Length of the UTF-8 manifest text in bytes. */
	UPROPERTY()
	int64 ManifestSize = 0;

	/** Absolute offset of the table of contents. */
	UPROPERTY()
	int64 TocOffset = 0;

	/** Length of the table of contents in bytes. */
	UPROPERTY()
	int64 TocSize = 0;

	/**
	 * FModPackageReader::ComputeContentHash over the final TOC: uppercase hex SHA-1, exactly
	 * ModPackage::HashHexLength characters. 40 '0' characters mean "not computed", which
	 * VerifyIntegrity reports as a mismatch.
	 */
	UPROPERTY()
	FString ContentHash;

	/** See the layout block at the top of this file. Always reads or writes ModPackage::HeaderSize bytes. */
	friend MODFRAMEWORK_API FArchive& operator<<(FArchive& Ar, FModPackageHeader& H);
};

/**
 * Reads a `.mod` package.
 *
 * EVERY byte this class touches is hostile until proven otherwise. It never asserts on file content;
 * a malformed, truncated, oversized or malicious package comes back as `false` plus FModDiagnostic
 * entries carrying one of these stable codes:
 *
 *   Package.OpenFailed        the file could not be opened, or the reader is not open
 *   Package.BadMagic          the first four bytes are not ModPackage::Magic
 *   Package.UnsupportedVersion the format version is 0 or newer than this build understands
 *   Package.Corrupt           a structural rule was violated: truncated file, out-of-range offset,
 *                             overlapping or impossible sizes, unreadable TOC, unparseable manifest
 *   Package.EntryNotFound     the requested relative path is not in the table of contents
 *   Package.UnsafePath        an entry path escapes the package root or names something the framework
 *                             refuses to write (see ModPackage::IsSafeRelativePath)
 *   Package.HashMismatch      an entry's bytes do not hash to its recorded SHA-1, or the header's
 *                             ContentHash does not match the table of contents
 *   Package.DecompressFailed  zlib rejected an entry's payload
 *   Package.ExtractFailed     a destination directory or file could not be created or written
 *   Package.TooLarge          a declared size exceeds one of the ModPackage:: caps
 *
 * Threading: an instance owns an open file handle and is NOT thread safe. Use one reader per thread,
 * or serialise access externally. The static helpers PeekManifest and ComputeContentHash are
 * re-entrant and hold no state.
 */
class MODFRAMEWORK_API FModPackageReader
{
public:
	FModPackageReader();
	~FModPackageReader();

	FModPackageReader(const FModPackageReader&) = delete;
	FModPackageReader& operator=(const FModPackageReader&) = delete;

	/**
	 * Opens a package: validates the header, loads and parses the embedded manifest, and loads and
	 * validates the whole table of contents. Payloads are NOT read - use ReadEntry, ExtractAll or
	 * VerifyIntegrity for that.
	 *
	 * A previously opened package is closed first, whether or not this call succeeds.
	 *
	 * @param AbsolutePath   Path of the `.mod` file.
	 * @param OutDiagnostics Appended to, never cleared.
	 * @return true when the package is open and every structural invariant held.
	 */
	bool Open(const FString& AbsolutePath, TArray<FModDiagnostic>& OutDiagnostics);

	/** Releases the file handle and forgets the manifest and the table of contents. Idempotent. */
	void Close();

	bool IsOpen() const;

	/** The parsed embedded manifest. Default constructed when the reader is not open. */
	const FModManifest& GetManifest() const;

	/** The validated header. Default constructed when the reader is not open. */
	const FModPackageHeader& GetHeader() const;

	/** The table of contents, in the order it appears in the file. Empty when the reader is not open. */
	TArray<FModPackageEntry> GetEntries() const;

	/** True when the package contains an entry whose path folds to the same ModPackage::MakeEntryKey. */
	bool HasEntry(const FString& RelativePath) const;

	/** Copies out one TOC entry. Returns false when there is no such entry. */
	bool FindEntry(const FString& RelativePath, FModPackageEntry& OutEntry) const;

	/**
	 * Reads one entry, decompressing it when needed and verifying its SHA-1 before returning.
	 * OutBytes is emptied on failure. Lookup is case-insensitive and accepts backslashes.
	 */
	bool ReadEntry(const FString& RelativePath, TArray<uint8>& OutBytes, FModDiagnostic& OutError);

	/**
	 * Extracts every entry under DestinationDirectory, creating intermediate directories as needed.
	 *
	 * Each destination path is rebuilt from the entry's relative path, converted to an absolute path
	 * (which collapses any relative segment that survived validation) and then re-checked to still be
	 * strictly under DestinationDirectory. Anything that escapes aborts the extraction with
	 * Package.UnsafePath. Every entry's hash is verified as it is written.
	 *
	 * Extraction stops at the first failure and returns false. Files written before the failure are
	 * left in place: extract into a scratch directory you own and delete it when this returns false.
	 */
	bool ExtractAll(const FString& DestinationDirectory, TArray<FModDiagnostic>& OutDiagnostics);

	/**
	 * Reads and hashes every entry, then checks the header's ContentHash against the table of
	 * contents. Unlike ExtractAll this does not stop at the first problem: every failing entry
	 * produces its own diagnostic so a mod author gets the complete picture.
	 */
	bool VerifyIntegrity(TArray<FModDiagnostic>& OutDiagnostics);

	/** Absolute path this reader was opened with. Empty when it is not open. */
	const FString& GetPackagePath() const;

	/**
	 * Reads only the header and the embedded manifest, then closes the file again. Used by discovery
	 * to list `*.mod` files without paying for TOC validation or leaving handles open.
	 */
	static bool PeekManifest(const FString& AbsolutePath, FModManifest& OutManifest, TArray<FModDiagnostic>& OutDiagnostics);

	/**
	 * The package's content hash: SHA-1, uppercase hex, over the concatenation - in table-of-contents
	 * order - of, for each entry, the UTF-8 bytes of RelativePath, one '\n' (0x0A), the 40 uppercase
	 * hex characters of Hash, and one more '\n'.
	 *
	 * Including the path means renaming an entry changes the digest, which hashing the entry hashes
	 * alone would not catch. Writers MUST call this function rather than reimplement it.
	 */
	static FString ComputeContentHash(const TArray<FModPackageEntry>& Entries);

private:
	/** Reads Entries[Index], decompressing and hash-checking it. Index is assumed already in range. */
	bool ReadEntryAtIndex(int32 Index, TArray<uint8>& OutBytes, FModDiagnostic& OutError);

	/** Open file handle. Null when the reader is closed. */
	TUniquePtr<FArchive> Archive;

	/** Absolute path of the open package. */
	FString PackagePath;

	/** Size of the open package in bytes, captured once at Open time and used for every bounds check. */
	int64 PackageFileSize = 0;

	FModPackageHeader Header;

	FModManifest Manifest;

	TArray<FModPackageEntry> Entries;

	/** ModPackage::MakeEntryKey(RelativePath) -> index into Entries. */
	TMap<FString, int32> EntryIndexByKey;

	bool bIsOpen = false;
};
