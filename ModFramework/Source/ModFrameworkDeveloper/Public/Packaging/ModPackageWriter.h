// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Core/ModFrameworkTypes.h"
#include "CoreMinimal.h"
#include "Manifest/ModManifest.h"
#include "Packaging/ModPackageFormat.h"

class FArchive;

/**
 * Writes the `.mod` container described normatively at the top of ModPackageFormat.h.
 *
 * FModPackageReader is the specification, not this class. Both structs' `operator<<` live in the
 * runtime module and are reused here verbatim, so the bytes this emits are produced by exactly the
 * code that parses them - the one approach that cannot drift.
 *
 * WHY A TEMPORARY FILE
 * The canonical layout is header, manifest, TOC, payloads. Every TOC entry carries the absolute
 * offset of its payload, and the TOC's own size depends on how many entries there are and how long
 * their paths are - so no payload offset can be known until every entry has been added. Buffering
 * compressed payloads in memory would mean holding an entire mod's cooked content at once, which for
 * a real mod is hundreds of megabytes. Instead payloads stream into a sidecar temp file as they are
 * added, and Close() lays out the final file and copies them in. Peak memory is one entry.
 *
 * Nothing is written to the destination until Close() succeeds: a failed package leaves no
 * half-written file that a provider might later try to load.
 */
class MODFRAMEWORKDEVELOPER_API FModPackageWriter
{
public:
	FModPackageWriter();
	~FModPackageWriter();

	FModPackageWriter(const FModPackageWriter&) = delete;
	FModPackageWriter& operator=(const FModPackageWriter&) = delete;

	/** Begins a package. The destination is not created until Close(). */
	bool Open(const FString& InAbsoluteDestinationPath, TArray<FModDiagnostic>& OutDiagnostics);

	bool IsOpen() const { return bOpen; }

	/**
	 * The manifest to embed. Required before Close(); it is serialised with
	 * FModManifestParser::SerializeToString so a reader gets byte-identical text to a loose mod.json.
	 */
	bool SetManifest(const FModManifest& InManifest, FModDiagnostic& OutError);

	/** Adds an in-memory blob. RelativePath is validated with ModPackage::IsSafeRelativePath. */
	bool AddBytes(const FString& InRelativePath, TArrayView<const uint8> InBytes, FModDiagnostic& OutError);

	/** Adds a file from disk. */
	bool AddFile(const FString& InRelativePath, const FString& InAbsoluteSourcePath, FModDiagnostic& OutError);

	/** Lays out and writes the final package. On failure nothing is left at the destination. */
	bool Close(TArray<FModDiagnostic>& OutDiagnostics);

	/** Discards everything, removing the temp file. Safe to call at any time. */
	void Abandon();

	int32 GetEntryCount() const { return Entries.Num(); }
	int64 GetTotalUncompressedBytes() const { return TotalUncompressedBytes; }

	/** Compression is on by default. Turning it off stores every entry verbatim. */
	void SetCompressionEnabled(bool bInEnabled) { bCompressionEnabled = bInEnabled; }

	/**
	 * Entries smaller than this are stored verbatim. Compressing a 200-byte file usually makes it
	 * bigger and always costs CPU on load.
	 */
	void SetMinimumCompressionBytes(int64 InBytes) { MinimumCompressionBytes = InBytes; }

	/**
	 * Packages a whole mod folder: reads and validates its mod.json, then adds every file under it.
	 *
	 * Skips Intermediate/, Binaries/, Saved/, DerivedDataCache/ and any dot-prefixed file or folder -
	 * build artefacts and editor state are never part of a distributable mod, and shipping them leaks
	 * local paths at best.
	 */
	static bool PackageDirectory(const FString& InSourceDirectory, const FString& InDestinationFile,
		TArray<FModDiagnostic>& OutDiagnostics);

	/** True when this relative path is build output or editor state rather than mod content. */
	static bool ShouldExcludeFromPackage(const FString& InRelativePath);

private:
	bool AppendPayload(const FString& InRelativePath, TArrayView<const uint8> InBytes, FModDiagnostic& OutError);

	bool bOpen = false;
	bool bCompressionEnabled = true;
	int64 MinimumCompressionBytes = 256;

	FString DestinationPath;
	FString TempPayloadPath;
	TUniquePtr<FArchive> TempPayloadArchive;

	FString ManifestJson;
	bool bHasManifest = false;

	TArray<FModPackageEntry> Entries;
	/** Offsets recorded so far are relative to the start of the temp payload file, not the package. */
	TArray<int64> PayloadOffsetsInTemp;
	TSet<FString> UsedEntryKeys;
	int64 TotalUncompressedBytes = 0;
};
