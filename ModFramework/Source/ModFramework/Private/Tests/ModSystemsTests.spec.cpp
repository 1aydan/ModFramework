// Copyright (c) 2026. Licensed for use in your own projects.

#if WITH_DEV_AUTOMATION_TESTS

#include "Config/ModConfigManager.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "Core/ModFrameworkVersion.h"
#include "CoreTypes.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "Math/NumericLimits.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/CoreMiscDefines.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Optional.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Net/ModNetworkValidator.h"
#include "Net/ModSessionManifest.h"
#include "Packaging/ModPackageFormat.h"
#include "Permissions/ModPermissionRegistry.h"
#include "Permissions/ModPermissions.h"
#include "Save/ModSaveDataManager.h"
#include "Save/ModSaveTypes.h"
#include "Serialization/Archive.h"
#include "Serialization/MemoryWriter.h"
#include "Settings/ModFrameworkSettings.h"
#include "Templates/Tuple.h"
#include "Templates/UniquePtr.h"
#include "Templates/UnrealTemplate.h"
#include "Tests/ModSystemsTestDoubles.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ScriptInterface.h"
#include "UObject/UObjectGlobals.h"

/**
 * Builders shared by every test below.
 *
 * Nothing here needs a world or a game instance. The permission registry, the config manager and the
 * save data manager are all plain UObjects created into the transient package; the package reader and
 * the session manifest are not UObjects at all. Anything that touches disk writes under
 * FPaths::AutomationTransientDir() and is deleted again in AfterEach.
 *
 * These helpers live in a named namespace rather than an anonymous one because this module is built
 * with unity files: every anonymous namespace in a unity blob is the same namespace, so a generic
 * helper name here would collide with an identically named helper in a sibling .cpp.
 */
namespace ModSystemsTestsPrivate
{
	FModId MakeTestId(const TCHAR* InId)
	{
		return FModId(FName(InId));
	}

	/** "a, b, c" - so a failed ordering assertion prints the whole list rather than a count. */
	FString JoinNames(const TArray<FName>& Names)
	{
		FString Result;
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += TEXT(", ");
			}

			Result += Names[Index].ToString();
		}

		return Result;
	}

	/** A manifest that asks for exactly the listed permissions. Nothing else about it matters here. */
	FModManifest MakePermissionManifest(const TCHAR* InId, TArray<FName> Permissions)
	{
		FModManifest Manifest;
		Manifest.ManifestVersion = MODFRAMEWORK_MANIFEST_VERSION;
		Manifest.Id = MakeTestId(InId);
		Manifest.Version = FModVersion(1, 0, 0);
		Manifest.Game.GameId = TEXT("com.example.game");
		Manifest.RequestedPermissions = MoveTemp(Permissions);
		return Manifest;
	}

	/** Stable text for a mismatch type. FModNetworkValidator's own converter is file-local. */
	const TCHAR* MismatchTypeToString(EModNetworkMismatchType Type)
	{
		switch (Type)
		{
		case EModNetworkMismatchType::MissingOnClient:     return TEXT("MissingOnClient");
		case EModNetworkMismatchType::MissingOnServer:     return TEXT("MissingOnServer");
		case EModNetworkMismatchType::VersionMismatch:     return TEXT("VersionMismatch");
		case EModNetworkMismatchType::ContentHashMismatch: return TEXT("ContentHashMismatch");
		case EModNetworkMismatchType::ScopeViolation:      return TEXT("ScopeViolation");
		case EModNetworkMismatchType::GameMismatch:        return TEXT("GameMismatch");
		case EModNetworkMismatchType::FrameworkMismatch:   return TEXT("FrameworkMismatch");
		case EModNetworkMismatchType::SdkMismatch:         return TEXT("SdkMismatch");
		default:                                          return TEXT("Unknown");
		}
	}

	/** One dense line per mismatch, used by the "both directions agree" test. */
	FString FingerprintMismatches(const FModNetworkValidationResult& Result)
	{
		FString Out = Result.bCompatible ? TEXT("compatible\n") : TEXT("incompatible\n");

		for (const FModNetworkMismatch& Mismatch : Result.Mismatches)
		{
			Out += FString::Printf(TEXT("%s|%s|%s|%s|%s\n"),
				MismatchTypeToString(Mismatch.Type),
				*Mismatch.ModId.ToString(),
				*Mismatch.Expected.ToString(),
				*Mismatch.Actual.ToString(),
				*Mismatch.Message);
		}

		return Out;
	}

	FModNetworkEntry MakeNetEntry(const TCHAR* InId, const TCHAR* InVersion, bool bInRequired,
		EModNetworkScope InScope = EModNetworkScope::ClientAndServer, const TCHAR* InContentHash = TEXT(""),
		const TCHAR* InDisplayName = TEXT(""))
	{
		FModNetworkEntry Entry;
		Entry.ModId = MakeTestId(InId);
		Entry.Version = FModVersion::FromString(InVersion);
		Entry.bRequired = bInRequired;
		Entry.Scope = InScope;
		Entry.ContentHash = InContentHash;
		Entry.DisplayName = InDisplayName;
		return Entry;
	}

	/** The session identity both sides of every net test start from. */
	FModSessionManifest MakeSessionManifest(TArray<FModNetworkEntry> Entries = TArray<FModNetworkEntry>())
	{
		FModSessionManifest Manifest;
		Manifest.FormatVersion = ModSessionManifest::CurrentFormatVersion;
		Manifest.GameId = TEXT("com.example.game");
		Manifest.GameVersion = FModVersion(1, 5, 0);
		Manifest.FrameworkVersion = ModFrameworkVersion::Get();
		Manifest.SdkId = TEXT("com.example.game.sdk");
		Manifest.SdkVersion = FModVersion(1, 5, 0);
		Manifest.Entries = MoveTemp(Entries);
		return Manifest;
	}

	//~ -------------------------------------------------------------------------------------------
	//~ `.mod` package fixtures
	//~
	//~ FModPackageWriter lives in ModFrameworkDeveloper, and ModFramework cannot depend on its own
	//~ developer module, so this suite cannot use it. Every fixture below is therefore written by
	//~ hand with an FArchive against the normative byte layout documented at the top of
	//~ ModPackageFormat.h. That is a feature rather than a workaround: a writer-produced package can
	//~ only ever be well-formed, and what the reader has to survive is the opposite.
	//~ -------------------------------------------------------------------------------------------

	/** SHA-1 as uppercase hex, handling an empty buffer without dereferencing null. */
	FString HashBytesHex(const TArray<uint8>& Bytes)
	{
		static const uint8 EmptySentinel = 0;

		const bool bHasData = Bytes.Num() > 0;
		const void* Buffer = bHasData ? static_cast<const void*>(Bytes.GetData()) : static_cast<const void*>(&EmptySentinel);
		const uint64 Size = bHasData ? static_cast<uint64>(Bytes.Num()) : 0;

		return FSHA1::HashBuffer(Buffer, Size).ToString();
	}

	/** UTF-8 bytes of Text, which is what every fixture payload is made of. */
	TArray<uint8> MakePayload(const TCHAR* Text)
	{
		TArray<uint8> Bytes;

		const auto Converted = StringCast<UTF8CHAR>(Text);
		if (Converted.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
		}

		return Bytes;
	}

	/** A `mod.json` that parses cleanly, so a rejected fixture is never rejected for the wrong reason. */
	const TCHAR* GetFixtureManifestJson()
	{
		return TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.testpkg",
	"name": "Test Package",
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json");
	}

	/** One file to place inside a fixture package. Payloads are always stored uncompressed. */
	struct FPackageFixtureEntry
	{
		FString RelativePath;
		TArray<uint8> Bytes;

		/** When set, the TOC records a hash that the payload does not have. */
		bool bCorruptHash = false;
	};

	/**
	 * Every knob a hostile fixture needs. The defaults produce a package the reader must accept, which
	 * is what makes each individual override a controlled experiment.
	 *
	 * The overrides are TOptional rather than an INDEX_NONE sentinel on purpose: several of the most
	 * important fixtures below set a field to -1 precisely because a negative count or size is one of
	 * the things the reader has to refuse, and a -1 sentinel would silently turn those into
	 * "no override" - a test that passes without testing anything.
	 */
	struct FPackageFixtureOptions
	{
		uint32 Magic = ModPackage::Magic;
		uint32 FormatVersion = ModPackage::CurrentFormatVersion;

		/** Written into the TOC in place of the real entry count. */
		TOptional<int32> EntryCountOverride;

		TOptional<int64> ManifestOffsetOverride;
		TOptional<int64> ManifestSizeOverride;
		TOptional<int64> TocOffsetOverride;

		/** Applied to the first entry only. */
		TOptional<int64> FirstEntryOffsetOverride;
		TOptional<int64> FirstEntryCompressedSizeOverride;

		/** Replaces the header's ContentHash with 40 zeroes, which is how "not computed" is spelled. */
		bool bBlankContentHash = false;
	};

	/**
	 * Writes a `.mod` file following the recipe in ModPackageFormat.h: placeholder header, manifest,
	 * TOC with placeholder offsets, payloads, then seek back and stamp the TOC and the header.
	 */
	bool WritePackageFixture(const FString& Path, const FString& ManifestJson,
		const TArray<FPackageFixtureEntry>& SourceEntries, const FPackageFixtureOptions& Options)
	{
		TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileWriter(*Path));
		if (!Ar.IsValid())
		{
			return false;
		}

		FModPackageHeader Header;
		Header.Magic = Options.Magic;
		Header.FormatVersion = Options.FormatVersion;
		*Ar << Header;

		const int64 ManifestOffset = Ar->Tell();
		const auto ManifestUtf8 = StringCast<UTF8CHAR>(*ManifestJson, ManifestJson.Len());
		const int64 ManifestSize = ManifestUtf8.Length();
		if (ManifestSize > 0)
		{
			Ar->Serialize(const_cast<UTF8CHAR*>(ManifestUtf8.Get()), ManifestSize);
		}

		TArray<FModPackageEntry> Entries;
		Entries.Reserve(SourceEntries.Num());
		for (const FPackageFixtureEntry& Source : SourceEntries)
		{
			FModPackageEntry Entry;
			Entry.RelativePath = Source.RelativePath;
			Entry.Offset = 0;
			Entry.CompressedSize = Source.Bytes.Num();
			Entry.UncompressedSize = Source.Bytes.Num();
			Entry.bCompressed = false;
			Entry.Hash = Source.bCorruptHash
				? FString(TEXT("1111111111111111111111111111111111111111"))
				: HashBytesHex(Source.Bytes);
			Entries.Add(MoveTemp(Entry));
		}

		const int64 TocOffset = Ar->Tell();

		int32 EntryCount = Options.EntryCountOverride.Get(Entries.Num());
		*Ar << EntryCount;
		for (FModPackageEntry& Entry : Entries)
		{
			*Ar << Entry;
		}

		const int64 TocSize = Ar->Tell() - TocOffset;

		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			Entries[Index].Offset = Ar->Tell();

			const TArray<uint8>& Bytes = SourceEntries[Index].Bytes;
			if (Bytes.Num() > 0)
			{
				Ar->Serialize(const_cast<uint8*>(Bytes.GetData()), Bytes.Num());
			}
		}

		if (Entries.Num() > 0)
		{
			Entries[0].Offset = Options.FirstEntryOffsetOverride.Get(Entries[0].Offset);
			Entries[0].CompressedSize = Options.FirstEntryCompressedSizeOverride.Get(Entries[0].CompressedSize);
		}

		// Every entry field except the path is fixed size and no path changed, so re-serialising the
		// TOC here occupies exactly the bytes the placeholder pass reserved.
		Ar->Seek(TocOffset);
		*Ar << EntryCount;
		for (FModPackageEntry& Entry : Entries)
		{
			*Ar << Entry;
		}

		Header.ManifestOffset = Options.ManifestOffsetOverride.Get(ManifestOffset);
		Header.ManifestSize = Options.ManifestSizeOverride.Get(ManifestSize);
		Header.TocOffset = Options.TocOffsetOverride.Get(TocOffset);
		Header.TocSize = TocSize;
		Header.ContentHash = Options.bBlankContentHash
			? FString()
			: FModPackageReader::ComputeContentHash(Entries);

		Ar->Seek(0);
		*Ar << Header;

		return Ar->Close();
	}

	/** The three entries every well-formed fixture package carries. */
	TArray<FPackageFixtureEntry> MakeStandardEntries()
	{
		TArray<FPackageFixtureEntry> Entries;
		Entries.Add({ TEXT("mod.json"), MakePayload(GetFixtureManifestJson()), false });
		Entries.Add({ TEXT("Content/Data/Values.json"), MakePayload(TEXT("{\"damage\":42}")), false });
		Entries.Add({ TEXT("Readme.txt"), MakePayload(TEXT("Hello from a mod package.")), false });
		return Entries;
	}

	//~ -------------------------------------------------------------------------------------------
	//~ Session manifest wire fixtures
	//~
	//~ FModSessionManifest::ToBase64 always stamps the current format version and always writes a
	//~ consistent entry count, so it physically cannot produce the payloads a hostile client would
	//~ send. These builders emit the bytes by hand, following the format-version-1 layout documented
	//~ at the top of ModSessionManifest.cpp.
	//~ -------------------------------------------------------------------------------------------

	/** `int32 ByteCount` followed by ByteCount bytes of UTF-8, exactly as the reader expects. */
	void WireString(FArchive& Ar, const FString& Value)
	{
		const auto Converted = StringCast<UTF8CHAR>(*Value, Value.Len());

		int32 ByteCount = Converted.Length();
		Ar << ByteCount;

		if (ByteCount > 0)
		{
			Ar.Serialize(const_cast<UTF8CHAR*>(Converted.Get()), ByteCount);
		}
	}

	/** `int32 Major, int32 Minor, int32 Patch, String PreRelease, String BuildMetadata`. */
	void WireVersion(FArchive& Ar, const FModVersion& Version)
	{
		int32 Major = Version.Major;
		int32 Minor = Version.Minor;
		int32 Patch = Version.Patch;

		Ar << Major;
		Ar << Minor;
		Ar << Patch;

		WireString(Ar, Version.PreRelease);
		WireString(Ar, Version.BuildMetadata);
	}

	/** Everything a hand-built session manifest payload can lie about. */
	struct FRawSessionOptions
	{
		int32 FormatVersion = ModSessionManifest::CurrentFormatVersion;

		/** Written in place of the real entry count. */
		int32 DeclaredEntryCount = 1;

		/** How many well-formed entries actually follow the count. */
		int32 EntriesWritten = 1;

		/** Extra bytes appended after the last entry. */
		int32 TrailingBytes = 0;

		/**
		 * Written in place of GameId's real length prefix. TOptional rather than an INDEX_NONE
		 * sentinel because -1 is one of the lengths the reader has to refuse, and a -1 sentinel would
		 * quietly turn that fixture into a well-formed payload.
		 */
		TOptional<int32> GameIdLengthOverride;

		/** Written in place of the first entry's scope byte. */
		TOptional<uint8> ScopeByteOverride;
	};

	/** Builds a session manifest payload byte by byte and returns its base64url encoding. */
	FString EncodeRawSessionManifest(const FRawSessionOptions& Options)
	{
		TArray<uint8> Bytes;
		{
			FMemoryWriter Writer(Bytes, /*bIsPersistent=*/true);

			int32 FormatVersion = Options.FormatVersion;
			Writer << FormatVersion;

			const FString GameId = TEXT("com.example.game");
			if (Options.GameIdLengthOverride.IsSet())
			{
				int32 Length = Options.GameIdLengthOverride.GetValue();
				Writer << Length;

				const auto Converted = StringCast<UTF8CHAR>(*GameId, GameId.Len());
				Writer.Serialize(const_cast<UTF8CHAR*>(Converted.Get()), Converted.Length());
			}
			else
			{
				WireString(Writer, GameId);
			}

			WireVersion(Writer, FModVersion(1, 5, 0));
			WireVersion(Writer, FModVersion(0, 1, 0));
			WireString(Writer, TEXT("com.example.game.sdk"));
			WireVersion(Writer, FModVersion(1, 5, 0));

			int32 EntryCount = Options.DeclaredEntryCount;
			Writer << EntryCount;

			for (int32 Index = 0; Index < Options.EntriesWritten; ++Index)
			{
				WireString(Writer, FString::Printf(TEXT("com.example.mod%d"), Index));
				WireVersion(Writer, FModVersion(1, 0, 0));

				uint8 ScopeByte = Options.ScopeByteOverride.Get(static_cast<uint8>(EModNetworkScope::ClientAndServer));
				Writer << ScopeByte;

				uint8 RequiredByte = 1;
				Writer << RequiredByte;

				WireString(Writer, FString());
				WireString(Writer, FString());
			}

			for (int32 Index = 0; Index < Options.TrailingBytes; ++Index)
			{
				uint8 Filler = 0xAB;
				Writer << Filler;
			}
		}

		return FBase64::Encode(Bytes, EBase64Mode::UrlSafe);
	}
}

//////////////////////////////////////////////////////////////////////////

BEGIN_DEFINE_SPEC(FModSystemsSpec, "ModFramework.Systems",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	/** Per-test scratch directory under FPaths::AutomationTransientDir(). */
	FString TempDirectory;

	/**
	 * Save slot written by the round-trip test. The engine's save game system owns its own path
	 * (Saved/SaveGames) and cannot be redirected into the transient directory, so the slot name is
	 * made unique per run and deleted in AfterEach whether or not the test reached its own cleanup.
	 */
	FString SaveSlotName;

	//~ Project settings snapshot, so a test that has to change one cannot leak it into another suite.
	TArray<FName> SavedAutoGrantedPermissions;
	TArray<FName> SavedAlwaysDeniedPermissions;
	bool bSavedDenyUnknownPermissions = true;
	bool bSavedVerifyContentHashes = true;

	void ExpectContains(const TCHAR* What, const FString& Actual, const TCHAR* Needle);
	bool ExpectText(const TCHAR* What, const FString& Actual, const FString& Expected);
	void ExpectNames(const TCHAR* What, const TArray<FName>& Actual, const FString& Expected);
	void ExpectState(const TCHAR* What, EModPermissionState Actual, EModPermissionState Expected);
	void ExpectBytes(const TCHAR* What, const TArray<uint8>& Actual, const TArray<uint8>& Expected);

	const FModDiagnostic* ExpectDiagnosticCode(const TCHAR* What, const TArray<FModDiagnostic>& In, const TCHAR* Code);
	void ExpectNoDiagnostics(const TCHAR* What, const TArray<FModDiagnostic>& In);

	const FModNetworkMismatch* ExpectMismatch(const TCHAR* What, const FModNetworkValidationResult& Result,
		const TCHAR* ModId, EModNetworkMismatchType Expected);

	/** Opens PackagePath and asserts it was refused with exactly the given diagnostic code. */
	void ExpectPackageRejected(const TCHAR* What, const FString& PackagePath, const TCHAR* ExpectedCode);

	/** Builds a permission registry with the builtins registered. */
	UModPermissionRegistry* MakeRegistry();

	FString MakeFixturePath(const TCHAR* FileName) const;

	/**
	 * Writes <Root>/<ModFolder>/Config/<FileName> and returns <Root>/<ModFolder>, which is the mod root
	 * UModConfigManager::LoadConfig expects. Call it twice with different file names to build a mod
	 * that ships more than one default file.
	 */
	FString WriteConfigDefaults(const FString& Root, const TCHAR* ModFolder, const TCHAR* FileName, const TCHAR* Json);

	/** A standalone save manager: no subsystem, so no permission registry and no mod registry to ask. */
	UModSaveDataManager* MakeSaveManager();

	/**
	 * An envelope holding one live record and one already-orphaned record, both carrying JSON and
	 * binary payloads, plus a stamp entry for each. This is the fixture the orphan guarantee is proved
	 * against.
	 */
	FModSaveEnvelope MakeOrphanEnvelope() const;

	static TArray<FName> CollectRecordIds(const TArray<FModSaveRecord>& Records);
	static FString CollectRecordIdText(const TArray<FModSaveRecord>& Records);

	/** "1->2, 2->3" for a recorded migration walk. */
	static FString DescribeSteps(const TArray<TPair<int32, int32>>& Steps);

END_DEFINE_SPEC(FModSystemsSpec)

//////////////////////////////////////////////////////////////////////////
// Assertion helpers

void FModSystemsSpec::ExpectContains(const TCHAR* What, const FString& Actual, const TCHAR* Needle)
{
	const FString FailureMessage = FString::Printf(
		TEXT("%s: expected the text to contain \"%s\", but it was \"%s\""), What, Needle, *Actual);

	TestTrue(*FailureMessage, Actual.Contains(Needle));
}

/** Case-sensitive comparison. TestEqual's FString overload folds case, which several tests here care about. */
bool FModSystemsSpec::ExpectText(const TCHAR* What, const FString& Actual, const FString& Expected)
{
	if (!Actual.Equals(Expected, ESearchCase::CaseSensitive))
	{
		AddError(FString::Printf(TEXT("%s: expected '%s' but got '%s'."), What, *Expected, *Actual));
		return false;
	}

	return true;
}

void FModSystemsSpec::ExpectNames(const TCHAR* What, const TArray<FName>& Actual, const FString& Expected)
{
	ExpectText(What, ModSystemsTestsPrivate::JoinNames(Actual), Expected);
}

void FModSystemsSpec::ExpectState(const TCHAR* What, EModPermissionState Actual, EModPermissionState Expected)
{
	ExpectText(What, ModFrameworkEnums::ToString(Actual), ModFrameworkEnums::ToString(Expected));
}

void FModSystemsSpec::ExpectBytes(const TCHAR* What, const TArray<uint8>& Actual, const TArray<uint8>& Expected)
{
	if (!TestEqual(FString::Printf(TEXT("%s: byte count"), What), Actual.Num(), Expected.Num()))
	{
		return;
	}

	for (int32 Index = 0; Index < Expected.Num(); ++Index)
	{
		if (Actual[Index] != Expected[Index])
		{
			AddError(FString::Printf(TEXT("%s: byte %d is %u, expected %u."),
				What, Index, static_cast<uint32>(Actual[Index]), static_cast<uint32>(Expected[Index])));
			return;
		}
	}
}

const FModDiagnostic* FModSystemsSpec::ExpectDiagnosticCode(const TCHAR* What, const TArray<FModDiagnostic>& In, const TCHAR* Code)
{
	const FName Wanted(Code);

	for (const FModDiagnostic& Diagnostic : In)
	{
		if (Diagnostic.Code != Wanted)
		{
			continue;
		}

		if (Diagnostic.Severity != EModDiagnosticSeverity::Error)
		{
			AddError(FString::Printf(TEXT("%s: diagnostic '%s' has severity %s, expected Error."),
				What, Code, *ModFrameworkEnums::ToString(Diagnostic.Severity)));
			return nullptr;
		}

		if (Diagnostic.Message.IsEmpty())
		{
			AddError(FString::Printf(TEXT("%s: diagnostic '%s' carries no message; a mod author has to be able to read it."),
				What, Code));
			return nullptr;
		}

		return &Diagnostic;
	}

	AddError(FString::Printf(TEXT("%s: expected the diagnostic '%s'. Got:\n%s"), What, Code, *ModDiagnostics::Join(In)));
	return nullptr;
}

void FModSystemsSpec::ExpectNoDiagnostics(const TCHAR* What, const TArray<FModDiagnostic>& In)
{
	if (In.Num() > 0)
	{
		AddError(FString::Printf(TEXT("%s: expected no diagnostics but got %d:\n%s"),
			What, In.Num(), *ModDiagnostics::Join(In)));
	}
}

const FModNetworkMismatch* FModSystemsSpec::ExpectMismatch(const TCHAR* What, const FModNetworkValidationResult& Result,
	const TCHAR* ModId, EModNetworkMismatchType Expected)
{
	using namespace ModSystemsTestsPrivate;

	const FModId Wanted = MakeTestId(ModId);

	for (const FModNetworkMismatch& Mismatch : Result.Mismatches)
	{
		if (Mismatch.ModId == Wanted && Mismatch.Type == Expected)
		{
			return &Mismatch;
		}
	}

	AddError(FString::Printf(TEXT("%s: expected a %s mismatch for '%s'. Got:\n%s"),
		What, MismatchTypeToString(Expected), ModId, *Result.ToDebugString()));
	return nullptr;
}

void FModSystemsSpec::ExpectPackageRejected(const TCHAR* What, const FString& PackagePath, const TCHAR* ExpectedCode)
{
	TArray<FModDiagnostic> Diagnostics;
	FModPackageReader Reader;

	const bool bOpened = Reader.Open(PackagePath, Diagnostics);

	if (bOpened)
	{
		AddError(FString::Printf(TEXT("%s: the package was accepted, but it must be refused with '%s'."),
			What, ExpectedCode));
		Reader.Close();
		return;
	}

	TestFalse(FString::Printf(TEXT("%s: the reader is left closed after a refused open"), What), Reader.IsOpen());
	ExpectDiagnosticCode(What, Diagnostics, ExpectedCode);
}

UModPermissionRegistry* FModSystemsSpec::MakeRegistry()
{
	UModPermissionRegistry* Registry = NewObject<UModPermissionRegistry>(GetTransientPackage());
	Registry->Initialize();
	return Registry;
}

FString FModSystemsSpec::MakeFixturePath(const TCHAR* FileName) const
{
	return FPaths::Combine(TempDirectory, FileName);
}

FString FModSystemsSpec::WriteConfigDefaults(const FString& Root, const TCHAR* ModFolder, const TCHAR* FileName, const TCHAR* Json)
{
	const FString ModRoot = FPaths::Combine(Root, ModFolder);
	const FString FilePath = FPaths::Combine(ModRoot, TEXT("Config"), FileName);

	if (!FFileHelper::SaveStringToFile(FString(Json), *FilePath))
	{
		AddError(FString::Printf(TEXT("Could not write the config fixture '%s'."), *FilePath));
	}

	return ModRoot;
}

UModSaveDataManager* FModSystemsSpec::MakeSaveManager()
{
	UModSaveDataManager* Manager = NewObject<UModSaveDataManager>(GetTransientPackage());
	Manager->Initialize(nullptr);
	return Manager;
}

FModSaveEnvelope FModSystemsSpec::MakeOrphanEnvelope() const
{
	using namespace ModSystemsTestsPrivate;

	FModSaveEnvelope Envelope;
	Envelope.EnvelopeVersion = FModSaveEnvelope::CurrentEnvelopeVersion;
	Envelope.FrameworkVersion = ModFrameworkVersion::Get();

	// Matching the project's own game id keeps SetEnvelope from warning about a foreign save; the id
	// itself is not what this fixture is about.
	if (const UModFrameworkSettings* Settings = UModFrameworkSettings::Get())
	{
		Envelope.GameId = Settings->GameId;
	}

	FModSaveRecord Live;
	Live.ModId = MakeTestId(TEXT("save.live"));
	Live.ModVersion = FModVersion(1, 2, 3);
	Live.DataVersion = 2;
	Live.Json = TEXT("{\"live\":true}");
	Live.Binary = { 1, 2, 3 };
	Live.bOrphaned = false;
	Envelope.Records.Add(MoveTemp(Live));

	FModSaveRecord Orphan;
	Orphan.ModId = MakeTestId(TEXT("save.gone"));
	Orphan.ModVersion = FModVersion::FromString(TEXT("4.5.6-rc.1+build.7"));
	Orphan.DataVersion = 9;
	Orphan.Json = TEXT("{\"orphan\":\"do not touch\"}");
	Orphan.Binary = { 0xDE, 0xAD, 0xBE, 0xEF };
	Orphan.bOrphaned = true;
	Envelope.Records.Add(MoveTemp(Orphan));

	FModSaveDependency LiveStamp;
	LiveStamp.ModId = MakeTestId(TEXT("save.live"));
	LiveStamp.Version = FModVersion(1, 2, 3);
	LiveStamp.DisplayName = TEXT("Live Mod");
	LiveStamp.bWasRequired = true;
	Envelope.RequiredMods.Add(MoveTemp(LiveStamp));

	FModSaveDependency GoneStamp;
	GoneStamp.ModId = MakeTestId(TEXT("save.gone"));
	GoneStamp.Version = FModVersion(4, 5, 6);
	GoneStamp.DisplayName = TEXT("Uninstalled Mod");
	GoneStamp.bWasRequired = false;
	Envelope.RequiredMods.Add(MoveTemp(GoneStamp));

	return Envelope;
}

TArray<FName> FModSystemsSpec::CollectRecordIds(const TArray<FModSaveRecord>& Records)
{
	TArray<FName> Ids;
	Ids.Reserve(Records.Num());
	for (const FModSaveRecord& Record : Records)
	{
		Ids.Add(Record.ModId.Value);
	}

	return Ids;
}

FString FModSystemsSpec::CollectRecordIdText(const TArray<FModSaveRecord>& Records)
{
	return ModSystemsTestsPrivate::JoinNames(CollectRecordIds(Records));
}

FString FModSystemsSpec::DescribeSteps(const TArray<TPair<int32, int32>>& Steps)
{
	FString Result;
	for (int32 Index = 0; Index < Steps.Num(); ++Index)
	{
		if (Index > 0)
		{
			Result += TEXT(", ");
		}

		Result += FString::Printf(TEXT("%d->%d"), Steps[Index].Key, Steps[Index].Value);
	}

	return Result;
}

//////////////////////////////////////////////////////////////////////////

void FModSystemsSpec::Define()
{
	using namespace ModSystemsTestsPrivate;

	Describe(TEXT("Permissions"), [this]()
	{
		// Every permission test starts from a known project configuration: nothing auto-granted,
		// nothing always-denied, unknown permissions denied. The previous values are restored so this
		// suite cannot change the answer another suite gets.
		BeforeEach([this]()
		{
			UModFrameworkSettings* Settings = GetMutableDefault<UModFrameworkSettings>();

			SavedAutoGrantedPermissions = Settings->AutoGrantedPermissions;
			SavedAlwaysDeniedPermissions = Settings->AlwaysDeniedPermissions;
			bSavedDenyUnknownPermissions = Settings->bDenyUnknownPermissions;

			Settings->AutoGrantedPermissions.Reset();
			Settings->AlwaysDeniedPermissions.Reset();
			Settings->bDenyUnknownPermissions = true;
		});

		AfterEach([this]()
		{
			UModFrameworkSettings* Settings = GetMutableDefault<UModFrameworkSettings>();

			Settings->AutoGrantedPermissions = SavedAutoGrantedPermissions;
			Settings->AlwaysDeniedPermissions = SavedAlwaysDeniedPermissions;
			Settings->bDenyUnknownPermissions = bSavedDenyUnknownPermissions;
		});

		Describe(TEXT("Catalogue"), [this]()
		{
			It(TEXT("registers every builtin permission and returns them sorted by id"), [this]()
			{
				UModPermissionRegistry* Registry = MakeRegistry();

				const TArray<FModPermissionDescriptor> All = Registry->GetAllPermissions();

				TArray<FName> Ids;
				Ids.Reserve(All.Num());
				for (const FModPermissionDescriptor& Descriptor : All)
				{
					Ids.Add(Descriptor.PermissionId);
				}

				ExpectNames(TEXT("builtin permissions"), Ids,
					TEXT("assets.read, assets.write, console, filesystem.read, filesystem.write, ")
					TEXT("gameplay.modify, native_code, network, save.modify, ui.modify"));

				// Every descriptor has to be describable to a player, or a permission prompt cannot be
				// built from data alone.
				for (const FModPermissionDescriptor& Descriptor : All)
				{
					TestFalse(*FString::Printf(TEXT("'%s' has a display name"), *Descriptor.PermissionId.ToString()),
						Descriptor.DisplayName.IsEmpty());
					TestFalse(*FString::Printf(TEXT("'%s' has a description"), *Descriptor.PermissionId.ToString()),
						Descriptor.Description.IsEmpty());
				}
			});

			// The dangerous flags are the security policy of the whole framework. Pinning them here
			// means flipping one is a deliberate act with a failing test attached, not a quiet edit.
			It(TEXT("marks exactly the capabilities the framework cannot police afterwards as dangerous"), [this]()
			{
				UModPermissionRegistry* Registry = MakeRegistry();

				TArray<FName> Dangerous;
				TArray<FName> Safe;

				for (const FModPermissionDescriptor& Descriptor : Registry->GetAllPermissions())
				{
					(Descriptor.bDangerous ? Dangerous : Safe).Add(Descriptor.PermissionId);
				}

				ExpectNames(TEXT("dangerous permissions"), Dangerous,
					TEXT("assets.write, console, filesystem.read, filesystem.write, native_code, network"));
				ExpectNames(TEXT("safe permissions"), Safe,
					TEXT("assets.read, gameplay.modify, save.modify, ui.modify"));
			});

			It(TEXT("reports an unregistered permission as unregistered and leaves the output untouched"), [this]()
			{
				UModPermissionRegistry* Registry = MakeRegistry();

				TestTrue(TEXT("a builtin is registered"), Registry->IsPermissionRegistered(ModPermissions::GameplayModify));
				TestFalse(TEXT("an invented id is not registered"), Registry->IsPermissionRegistered(FName(TEXT("not.a.permission"))));

				FModPermissionDescriptor Out;
				Out.PermissionId = FName(TEXT("sentinel"));
				TestFalse(TEXT("GetPermission fails for an unregistered id"),
					Registry->GetPermission(FName(TEXT("not.a.permission")), Out));
				ExpectText(TEXT("the caller's struct is left alone"), Out.PermissionId.ToString(), TEXT("sentinel"));
			});
		});

		Describe(TEXT("Evaluation order"), [this]()
		{
			// DOCUMENTED GUARANTEE, in the order UModPermissionRegistry's header states it:
			//   1. the game's policy, when it has an opinion
			//   2. AlwaysDeniedPermissions
			//   3. an unregistered permission
			//   4. AutoGrantedPermissions, and only when the permission is not dangerous
			//   5. everything else stays Pending
			// Each rule below is asserted together with the rule it has to beat, because a chain is
			// only meaningful if the earlier links actually win.

			It(TEXT("rule 5: a requested permission nobody decided on stays pending"), [this]()
			{
				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.default"), { ModPermissions::GameplayModify }));

				const FModId ModId = MakeTestId(TEXT("perm.default"));
				ExpectState(TEXT("gameplay.modify"), Registry->GetPermissionState(ModId, ModPermissions::GameplayModify),
					EModPermissionState::Pending);
				ExpectNames(TEXT("pending list"), Registry->GetPendingPermissions(ModId), TEXT("gameplay.modify"));
				ExpectNames(TEXT("granted list"), Registry->GetGrantedPermissions(ModId), FString());
				ExpectNames(TEXT("denied list"), Registry->GetDeniedPermissions(ModId), FString());
			});

			It(TEXT("rule 4: a non-dangerous permission listed in AutoGrantedPermissions is granted"), [this]()
			{
				GetMutableDefault<UModFrameworkSettings>()->AutoGrantedPermissions =
					{ ModPermissions::GameplayModify, ModPermissions::UiModify };

				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.auto"),
					{ ModPermissions::GameplayModify, ModPermissions::UiModify, ModPermissions::SaveModify }));

				const FModId ModId = MakeTestId(TEXT("perm.auto"));
				ExpectNames(TEXT("granted"), Registry->GetGrantedPermissions(ModId), TEXT("gameplay.modify, ui.modify"));

				// save.modify was not listed, so it is not swept up by the same pass.
				ExpectNames(TEXT("pending"), Registry->GetPendingPermissions(ModId), TEXT("save.modify"));
			});

			// THE SECURITY RULE THIS SUITE EXISTS FOR. A dangerous permission is never auto-granted,
			// whatever DefaultGame.ini says. Do not relax this into "warn and grant".
			It(TEXT("rule 4: a dangerous permission is never auto-granted, even when it is listed"), [this]()
			{
				UModFrameworkSettings* Settings = GetMutableDefault<UModFrameworkSettings>();

				// Every builtin, safe and dangerous alike, is auto-granted by the project.
				Settings->AutoGrantedPermissions = ModPermissions::GetBuiltinPermissions();

				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.greedy"), ModPermissions::GetBuiltinPermissions()));

				const FModId ModId = MakeTestId(TEXT("perm.greedy"));

				// Exactly the four non-dangerous builtins came back granted.
				ExpectNames(TEXT("granted"), Registry->GetGrantedPermissions(ModId),
					TEXT("assets.read, gameplay.modify, save.modify, ui.modify"));

				// Every dangerous one is left waiting for something to grant it explicitly.
				ExpectNames(TEXT("pending"), Registry->GetPendingPermissions(ModId),
					TEXT("assets.write, console, filesystem.read, filesystem.write, native_code, network"));

				ExpectNames(TEXT("denied"), Registry->GetDeniedPermissions(ModId), FString());

				const FName DangerousIds[] =
				{
					ModPermissions::AssetsWrite,
					ModPermissions::Console,
					ModPermissions::FilesystemRead,
					ModPermissions::FilesystemWrite,
					ModPermissions::NativeCode,
					ModPermissions::Network
				};

				for (const FName Dangerous : DangerousIds)
				{
					TestFalse(*FString::Printf(TEXT("HasPermission('%s') is false"), *Dangerous.ToString()),
						Registry->HasPermission(ModId, Dangerous));
				}
			});

			It(TEXT("rule 3: an unregistered permission is denied when bDenyUnknownPermissions is set"), [this]()
			{
				GetMutableDefault<UModFrameworkSettings>()->bDenyUnknownPermissions = true;

				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.typo"),
					{ FName(TEXT("gameplay.modifi")), ModPermissions::GameplayModify }));

				const FModId ModId = MakeTestId(TEXT("perm.typo"));
				ExpectNames(TEXT("denied"), Registry->GetDeniedPermissions(ModId), TEXT("gameplay.modifi"));
				ExpectNames(TEXT("pending"), Registry->GetPendingPermissions(ModId), TEXT("gameplay.modify"));
			});

			It(TEXT("rule 3: an unregistered permission is left pending when bDenyUnknownPermissions is clear"), [this]()
			{
				GetMutableDefault<UModFrameworkSettings>()->bDenyUnknownPermissions = false;

				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.lenient"), { FName(TEXT("gameplay.modifi")) }));

				const FModId ModId = MakeTestId(TEXT("perm.lenient"));
				ExpectNames(TEXT("pending"), Registry->GetPendingPermissions(ModId), TEXT("gameplay.modifi"));
				ExpectNames(TEXT("denied"), Registry->GetDeniedPermissions(ModId), FString());

				// Lenient still is not permissive: pending is not permission to do anything.
				TestFalse(TEXT("an unknown pending permission is not held"),
					Registry->HasPermission(ModId, FName(TEXT("gameplay.modifi"))));
			});

			It(TEXT("rule 3 beats rule 4: an unregistered permission is not auto-granted"), [this]()
			{
				UModFrameworkSettings* Settings = GetMutableDefault<UModFrameworkSettings>();
				Settings->bDenyUnknownPermissions = false;
				Settings->AutoGrantedPermissions = { FName(TEXT("invented.capability")) };

				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.invented"), { FName(TEXT("invented.capability")) }));

				const FModId ModId = MakeTestId(TEXT("perm.invented"));
				ExpectState(TEXT("an unregistered, auto-granted permission"),
					Registry->GetPermissionState(ModId, FName(TEXT("invented.capability"))), EModPermissionState::Pending);
			});

			It(TEXT("rule 2 beats rule 4: AlwaysDeniedPermissions wins over AutoGrantedPermissions"), [this]()
			{
				UModFrameworkSettings* Settings = GetMutableDefault<UModFrameworkSettings>();
				Settings->AutoGrantedPermissions = { ModPermissions::GameplayModify, ModPermissions::UiModify };
				Settings->AlwaysDeniedPermissions = { ModPermissions::GameplayModify };

				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.blocked"),
					{ ModPermissions::GameplayModify, ModPermissions::UiModify }));

				const FModId ModId = MakeTestId(TEXT("perm.blocked"));
				ExpectNames(TEXT("denied"), Registry->GetDeniedPermissions(ModId), TEXT("gameplay.modify"));
				ExpectNames(TEXT("granted"), Registry->GetGrantedPermissions(ModId), TEXT("ui.modify"));
			});

			It(TEXT("rule 1 beats every later rule: the game's policy has the final say"), [this]()
			{
				UModFrameworkSettings* Settings = GetMutableDefault<UModFrameworkSettings>();
				Settings->AlwaysDeniedPermissions = { ModPermissions::Network };
				Settings->AutoGrantedPermissions = { ModPermissions::UiModify };

				UModSystemsTestPermissionPolicy* Policy = NewObject<UModSystemsTestPermissionPolicy>(GetTransientPackage());

				// The policy overrides all three later rules in both directions at once.
				Policy->Verdicts.Add(ModPermissions::Network, EModPermissionState::Granted);
				Policy->Verdicts.Add(ModPermissions::UiModify, EModPermissionState::Denied);
				Policy->Verdicts.Add(ModPermissions::FilesystemWrite, EModPermissionState::Granted);

				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->SetPolicy(TScriptInterface<IModPermissionPolicy>(Policy));

				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.policy"),
					{ ModPermissions::Network, ModPermissions::UiModify, ModPermissions::FilesystemWrite,
					  ModPermissions::GameplayModify }));

				const FModId ModId = MakeTestId(TEXT("perm.policy"));

				ExpectNames(TEXT("granted"), Registry->GetGrantedPermissions(ModId),
					TEXT("filesystem.write, network"));
				ExpectNames(TEXT("denied"), Registry->GetDeniedPermissions(ModId), TEXT("ui.modify"));

				// A permission the policy had no opinion on falls through to the default rules.
				ExpectNames(TEXT("pending"), Registry->GetPendingPermissions(ModId), TEXT("gameplay.modify"));

				// The policy was asked about every permission, and about this mod.
				TestEqual(TEXT("the policy was consulted once per permission"), Policy->Queries.Num(), 4);
				for (const TPair<FModId, FName>& Query : Policy->Queries)
				{
					ExpectText(TEXT("the policy was told which mod is asking"), Query.Key.ToString(), TEXT("perm.policy"));
				}
			});

			It(TEXT("refuses a policy object that does not implement the interface and keeps the previous one"), [this]()
			{
				UModSystemsTestPermissionPolicy* Policy = NewObject<UModSystemsTestPermissionPolicy>(GetTransientPackage());
				Policy->Verdicts.Add(ModPermissions::GameplayModify, EModPermissionState::Granted);

				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->SetPolicy(TScriptInterface<IModPermissionPolicy>(Policy));

				// An object that is not a policy at all. Installing it would silently weaken the gate,
				// so the registry keeps what it had. (The save-migration double is used here purely
				// because it is a concrete UObject that does not implement IModPermissionPolicy;
				// UObject itself is abstract and cannot be instantiated.)
				TScriptInterface<IModPermissionPolicy> Bogus;
				Bogus.SetObject(NewObject<UModSystemsTestSaveMigration>(GetTransientPackage()));
				Registry->SetPolicy(Bogus);

				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.keptpolicy"), { ModPermissions::GameplayModify }));

				ExpectState(TEXT("the original policy is still installed"),
					Registry->GetPermissionState(MakeTestId(TEXT("perm.keptpolicy")), ModPermissions::GameplayModify),
					EModPermissionState::Granted);
			});
		});

		Describe(TEXT("Recorded state"), [this]()
		{
			// DOCUMENTED GUARANTEE: Pending means "nobody has decided yet", and that is never a licence
			// to act. HasPermission and HasAllPermissions must be true for Granted and for nothing else.
			It(TEXT("treats pending as no permission at all"), [this]()
			{
				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("perm.pending"),
					{ ModPermissions::GameplayModify, ModPermissions::SaveModify }));

				const FModId ModId = MakeTestId(TEXT("perm.pending"));

				TestFalse(TEXT("a pending permission is not held"), Registry->HasPermission(ModId, ModPermissions::GameplayModify));
				TestFalse(TEXT("HasAllPermissions refuses a pending set"),
					Registry->HasAllPermissions(ModId, { ModPermissions::GameplayModify, ModPermissions::SaveModify }));

				Registry->GrantPermission(ModId, ModPermissions::GameplayModify);
				TestFalse(TEXT("HasAllPermissions still refuses while one is pending"),
					Registry->HasAllPermissions(ModId, { ModPermissions::GameplayModify, ModPermissions::SaveModify }));

				Registry->GrantPermission(ModId, ModPermissions::SaveModify);
				TestTrue(TEXT("HasAllPermissions accepts once both are granted"),
					Registry->HasAllPermissions(ModId, { ModPermissions::GameplayModify, ModPermissions::SaveModify }));

				// An empty list asks for nothing, so it is trivially satisfied; an empty id names no
				// capability, so there is nothing to withhold.
				TestTrue(TEXT("an empty requirement list is satisfied"), Registry->HasAllPermissions(ModId, TArray<FName>()));
				TestTrue(TEXT("an empty permission id is skipped"),
					Registry->HasAllPermissions(ModId, { ModPermissions::GameplayModify, NAME_None }));
			});

			It(TEXT("reports a mod that was never evaluated as NotRequested"), [this]()
			{
				UModPermissionRegistry* Registry = MakeRegistry();

				ExpectState(TEXT("an unseen mod"),
					Registry->GetPermissionState(MakeTestId(TEXT("perm.unseen")), ModPermissions::GameplayModify),
					EModPermissionState::NotRequested);
				ExpectNames(TEXT("granted list of an unseen mod"),
					Registry->GetGrantedPermissions(MakeTestId(TEXT("perm.unseen"))), FString());
			});

			// Documented: EvaluateManifest REPLACES a mod's record rather than merging into it, so
			// re-evaluating the same manifest always produces the same answer. Evaluate first, grant
			// after - not the other way round.
			It(TEXT("replaces rather than merges a mod's previous record when it is re-evaluated"), [this]()
			{
				UModPermissionRegistry* Registry = MakeRegistry();

				const FModManifest Manifest = MakePermissionManifest(TEXT("perm.reeval"), { ModPermissions::GameplayModify });
				const FModId ModId = MakeTestId(TEXT("perm.reeval"));

				Registry->EvaluateManifest(Manifest);
				Registry->GrantPermission(ModId, ModPermissions::GameplayModify);
				ExpectState(TEXT("after the explicit grant"),
					Registry->GetPermissionState(ModId, ModPermissions::GameplayModify), EModPermissionState::Granted);

				Registry->EvaluateManifest(Manifest);
				ExpectState(TEXT("after re-evaluating the same manifest"),
					Registry->GetPermissionState(ModId, ModPermissions::GameplayModify), EModPermissionState::Pending);
			});

			It(TEXT("refuses to grant a permission the project always denies and records the refusal"), [this]()
			{
				GetMutableDefault<UModFrameworkSettings>()->AlwaysDeniedPermissions = { ModPermissions::Network };

				UModPermissionRegistry* Registry = MakeRegistry();
				const FModId ModId = MakeTestId(TEXT("perm.forbidden"));

				Registry->GrantPermission(ModId, ModPermissions::Network);

				// "No mod ever receives this" has to hold for an explicit grant too, or the setting is
				// advice rather than policy.
				ExpectState(TEXT("an always-denied permission after GrantPermission"),
					Registry->GetPermissionState(ModId, ModPermissions::Network), EModPermissionState::Denied);
				TestFalse(TEXT("and it is not held"), Registry->HasPermission(ModId, ModPermissions::Network));
			});

			It(TEXT("ignores a grant or a denial with an empty mod id or permission id"), [this]()
			{
				UModPermissionRegistry* Registry = MakeRegistry();

				// Untrusted input reaches these through Blueprint; neither may assert.
				Registry->GrantPermission(FModId(), ModPermissions::GameplayModify);
				Registry->GrantPermission(MakeTestId(TEXT("perm.empty")), NAME_None);
				Registry->DenyPermission(FModId(), ModPermissions::GameplayModify);
				Registry->DenyPermission(MakeTestId(TEXT("perm.empty")), NAME_None);

				ExpectNames(TEXT("nothing was recorded"),
					Registry->GetGrantedPermissions(MakeTestId(TEXT("perm.empty"))), FString());
				ExpectState(TEXT("no state was invented for an empty id"),
					Registry->GetPermissionState(FModId(), ModPermissions::GameplayModify), EModPermissionState::NotRequested);
			});
		});

		Describe(TEXT("Isolation"), [this]()
		{
			// DOCUMENTED GUARANTEE: permission state is per mod. One mod's grant, denial or reset never
			// reaches another, however similar their ids.
			It(TEXT("keeps every mod's decisions to itself"), [this]()
			{
				GetMutableDefault<UModFrameworkSettings>()->AutoGrantedPermissions = { ModPermissions::UiModify };

				UModPermissionRegistry* Registry = MakeRegistry();

				const FModId First = MakeTestId(TEXT("iso.mod"));
				const FModId Second = MakeTestId(TEXT("iso.mod.extra"));

				Registry->EvaluateManifest(MakePermissionManifest(TEXT("iso.mod"),
					{ ModPermissions::GameplayModify, ModPermissions::UiModify }));
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("iso.mod.extra"),
					{ ModPermissions::SaveModify }));

				Registry->GrantPermission(First, ModPermissions::GameplayModify);
				Registry->DenyPermission(Second, ModPermissions::SaveModify);

				ExpectNames(TEXT("first mod granted"), Registry->GetGrantedPermissions(First),
					TEXT("gameplay.modify, ui.modify"));
				ExpectNames(TEXT("second mod granted"), Registry->GetGrantedPermissions(Second), FString());
				ExpectNames(TEXT("second mod denied"), Registry->GetDeniedPermissions(Second), TEXT("save.modify"));

				// Neither mod can see the other's capability.
				ExpectState(TEXT("the second mod never asked for gameplay.modify"),
					Registry->GetPermissionState(Second, ModPermissions::GameplayModify), EModPermissionState::NotRequested);
				ExpectState(TEXT("the first mod never asked for save.modify"),
					Registry->GetPermissionState(First, ModPermissions::SaveModify), EModPermissionState::NotRequested);
			});

			It(TEXT("clears one mod's record without touching another's"), [this]()
			{
				UModPermissionRegistry* Registry = MakeRegistry();

				const FModId First = MakeTestId(TEXT("reset.first"));
				const FModId Second = MakeTestId(TEXT("reset.second"));

				Registry->GrantPermission(First, ModPermissions::GameplayModify);
				Registry->GrantPermission(Second, ModPermissions::GameplayModify);

				int32 ChangeCount = 0;
				FModId LastChangedMod;
				Registry->OnPermissionChanged.AddLambda([&ChangeCount, &LastChangedMod](const FModId& ModId, FName)
				{
					++ChangeCount;
					LastChangedMod = ModId;
				});

				Registry->ResetForMod(First);

				ExpectState(TEXT("the reset mod"), Registry->GetPermissionState(First, ModPermissions::GameplayModify),
					EModPermissionState::NotRequested);
				ExpectState(TEXT("the untouched mod"), Registry->GetPermissionState(Second, ModPermissions::GameplayModify),
					EModPermissionState::Granted);

				TestEqual(TEXT("one change was broadcast"), ChangeCount, 1);
				ExpectText(TEXT("and it named the reset mod"), LastChangedMod.ToString(), TEXT("reset.first"));

				Registry->OnPermissionChanged.Clear();
			});

			It(TEXT("drops every mod's record on Reset but keeps the game's policy"), [this]()
			{
				UModSystemsTestPermissionPolicy* Policy = NewObject<UModSystemsTestPermissionPolicy>(GetTransientPackage());
				Policy->Verdicts.Add(ModPermissions::GameplayModify, EModPermissionState::Denied);

				UModPermissionRegistry* Registry = MakeRegistry();
				Registry->SetPolicy(TScriptInterface<IModPermissionPolicy>(Policy));
				Registry->GrantPermission(MakeTestId(TEXT("reset.all")), ModPermissions::GameplayModify);

				Registry->Reset();

				ExpectState(TEXT("the recorded grant is gone"),
					Registry->GetPermissionState(MakeTestId(TEXT("reset.all")), ModPermissions::GameplayModify),
					EModPermissionState::NotRequested);
				TestTrue(TEXT("the builtins are registered again"),
					Registry->IsPermissionRegistered(ModPermissions::GameplayModify));

				// Silently dropping the policy would weaken the gate at exactly the moment - a reload -
				// when a game is least likely to notice.
				Registry->EvaluateManifest(MakePermissionManifest(TEXT("reset.all"), { ModPermissions::GameplayModify }));
				ExpectState(TEXT("the policy survived Reset"),
					Registry->GetPermissionState(MakeTestId(TEXT("reset.all")), ModPermissions::GameplayModify),
					EModPermissionState::Denied);
			});
		});
	});

	//~ -----------------------------------------------------------------------------------------------
	//~ Config
	//~ -----------------------------------------------------------------------------------------------

	Describe(TEXT("Config"), [this]()
	{
		BeforeEach([this]()
		{
			TempDirectory = FPaths::Combine(FPaths::AutomationTransientDir(),
				TEXT("ModFrameworkSystemsSpec"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().MakeDirectory(*TempDirectory, /*Tree=*/true);
		});

		AfterEach([this]()
		{
			if (!TempDirectory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*TempDirectory, /*RequireExists=*/false, /*Tree=*/true);
				TempDirectory.Reset();
			}
		});

		// DOCUMENTED GUARANTEE - the entire reason config has two layers instead of one file.
		//
		//   defaults  <ModRoot>/Config/*.json      shipped by the author, never written back
		//   values    the user layer               edited by the player or the mod
		//
		// A read falls through to the default when the user layer has no entry, so a mod update can add
		// settings and change old defaults without clobbering a player's edits. Flattening these into
		// one store - the obvious simplification - breaks exactly that. Do not.
		It(TEXT("lets a user value shadow a default while other keys still fall through"), [this]()
		{
			UModConfigManager* Config = NewObject<UModConfigManager>(GetTransientPackage());
			Config->Initialize(nullptr);

			const FModId ModId = MakeTestId(TEXT("test.systems.config.alpha"));
			const FString ModRoot = WriteConfigDefaults(TempDirectory, TEXT("alpha"), TEXT("defaults.json"),
				TEXT("{\n\t\"damageMultiplier\": 1.5,\n\t\"enableDebug\": false,\n\t\"maxItems\": 10,\n\t\"playerName\": \"Default Name\"\n}"));

			TArray<FModDiagnostic> Diagnostics;
			TestTrue(TEXT("the config loads"), Config->LoadConfig(ModId, ModRoot, Diagnostics));
			ExpectNoDiagnostics(TEXT("loading a well-formed default file"), Diagnostics);
			TestTrue(TEXT("the mod now has a config"), Config->HasConfig(ModId));

			// Before any user edit every read comes from the shipped defaults.
			TestEqual(TEXT("default damageMultiplier"), Config->GetFloat(ModId, FName(TEXT("damageMultiplier")), 0.0f), 1.5f);
			TestEqual(TEXT("default maxItems"), Config->GetInt(ModId, FName(TEXT("maxItems")), 0), 10);
			TestFalse(TEXT("default enableDebug"), Config->GetBool(ModId, FName(TEXT("enableDebug")), true));
			ExpectText(TEXT("default playerName"),
				Config->GetString(ModId, FName(TEXT("playerName")), TEXT("")), TEXT("Default Name"));

			// One user edit shadows one default and leaves every other key alone.
			Config->SetFloat(ModId, FName(TEXT("damageMultiplier")), 2.5f);
			Config->SetBool(ModId, FName(TEXT("enableDebug")), true);

			TestEqual(TEXT("shadowed damageMultiplier"), Config->GetFloat(ModId, FName(TEXT("damageMultiplier")), 0.0f), 2.5f);
			TestTrue(TEXT("shadowed enableDebug"), Config->GetBool(ModId, FName(TEXT("enableDebug")), false));
			TestEqual(TEXT("maxItems still falls through"), Config->GetInt(ModId, FName(TEXT("maxItems")), 0), 10);
			ExpectText(TEXT("playerName still falls through"),
				Config->GetString(ModId, FName(TEXT("playerName")), TEXT("")), TEXT("Default Name"));

			Config->Shutdown();
		});

		It(TEXT("restores fall-through to the shipped defaults on ResetToDefaults"), [this]()
		{
			UModConfigManager* Config = NewObject<UModConfigManager>(GetTransientPackage());
			Config->Initialize(nullptr);

			const FModId ModId = MakeTestId(TEXT("test.systems.config.reset"));
			const FString ModRoot = WriteConfigDefaults(TempDirectory, TEXT("reset"), TEXT("defaults.json"),
				TEXT("{ \"damageMultiplier\": 1.5, \"maxItems\": 10 }"));

			TArray<FModDiagnostic> Diagnostics;
			Config->LoadConfig(ModId, ModRoot, Diagnostics);

			Config->SetFloat(ModId, FName(TEXT("damageMultiplier")), 9.0f);
			Config->SetString(ModId, FName(TEXT("userOnly")), TEXT("kept until reset"));
			TestEqual(TEXT("the user value is in force"), Config->GetFloat(ModId, FName(TEXT("damageMultiplier")), 0.0f), 9.0f);

			Config->ResetToDefaults(ModId);

			// Resetting drops the user layer only. The defaults were never written to, so they are still
			// there and reads fall through to them again.
			TestEqual(TEXT("damageMultiplier falls back to the default"),
				Config->GetFloat(ModId, FName(TEXT("damageMultiplier")), 0.0f), 1.5f);
			TestEqual(TEXT("maxItems is untouched"), Config->GetInt(ModId, FName(TEXT("maxItems")), 0), 10);
			TestFalse(TEXT("a key that only ever existed in the user layer is gone"),
				Config->HasKey(ModId, FName(TEXT("userOnly"))));
			TestTrue(TEXT("the mod still has a config"), Config->HasConfig(ModId));

			Config->Shutdown();
		});

		It(TEXT("returns the union of both layers from GetKeys, sorted"), [this]()
		{
			UModConfigManager* Config = NewObject<UModConfigManager>(GetTransientPackage());
			Config->Initialize(nullptr);

			const FModId ModId = MakeTestId(TEXT("test.systems.config.keys"));
			const FString ModRoot = WriteConfigDefaults(TempDirectory, TEXT("keys"), TEXT("defaults.json"),
				TEXT("{ \"playerName\": \"x\", \"damageMultiplier\": 1.0, \"maxItems\": 1 }"));

			TArray<FModDiagnostic> Diagnostics;
			Config->LoadConfig(ModId, ModRoot, Diagnostics);

			// One key that exists only in the user layer, and one that exists in both.
			Config->SetString(ModId, FName(TEXT("userOnly")), TEXT("u"));
			Config->SetInt(ModId, FName(TEXT("maxItems")), 5);

			// A key present in both layers appears exactly once, and the order is stable so a config
			// listing does not shuffle between runs.
			ExpectNames(TEXT("key union"), Config->GetKeys(ModId),
				TEXT("damageMultiplier, maxItems, playerName, userOnly"));

			TestTrue(TEXT("a defaults-only key is present"), Config->HasKey(ModId, FName(TEXT("playerName"))));
			TestTrue(TEXT("a user-only key is present"), Config->HasKey(ModId, FName(TEXT("userOnly"))));
			TestFalse(TEXT("an absent key is absent"), Config->HasKey(ModId, FName(TEXT("neverSet"))));

			Config->Shutdown();
		});

		It(TEXT("merges several default files in sorted order, letting the later file win"), [this]()
		{
			UModConfigManager* Config = NewObject<UModConfigManager>(GetTransientPackage());
			Config->Initialize(nullptr);

			const FModId ModId = MakeTestId(TEXT("test.systems.config.merge"));
			const FString ModRoot = WriteConfigDefaults(TempDirectory, TEXT("merge"), TEXT("a_base.json"),
				TEXT("{ \"shared\": 1, \"onlyInBase\": 2 }"));
			WriteConfigDefaults(TempDirectory, TEXT("merge"), TEXT("z_overrides.json"),
				TEXT("{ \"shared\": 3, \"onlyInOverride\": 4 }"));

			TArray<FModDiagnostic> Diagnostics;
			Config->LoadConfig(ModId, ModRoot, Diagnostics);

			TestEqual(TEXT("the later file wins on a collision"), Config->GetInt(ModId, FName(TEXT("shared")), 0), 3);
			TestEqual(TEXT("keys from the earlier file survive"), Config->GetInt(ModId, FName(TEXT("onlyInBase")), 0), 2);
			TestEqual(TEXT("keys from the later file survive"), Config->GetInt(ModId, FName(TEXT("onlyInOverride")), 0), 4);

			Config->Shutdown();
		});

		It(TEXT("warns about a malformed default file and falls back rather than failing the load"), [this]()
		{
			UModConfigManager* Config = NewObject<UModConfigManager>(GetTransientPackage());
			Config->Initialize(nullptr);

			const FModId ModId = MakeTestId(TEXT("test.systems.config.broken"));
			const FString ModRoot = WriteConfigDefaults(TempDirectory, TEXT("broken"), TEXT("defaults.json"),
				TEXT("{ this is not json"));

			TArray<FModDiagnostic> Diagnostics;

			// A player hand-edits these files, so a broken one is expected input, not an exception.
			TestTrue(TEXT("the load still succeeds"), Config->LoadConfig(ModId, ModRoot, Diagnostics));

			if (TestEqual(TEXT("one diagnostic was raised"), Diagnostics.Num(), 1))
			{
				ExpectText(TEXT("diagnostic code"), Diagnostics[0].Code.ToString(), TEXT("Config.InvalidJson"));
				ExpectText(TEXT("diagnostic severity"),
					ModFrameworkEnums::ToString(Diagnostics[0].Severity),
					ModFrameworkEnums::ToString(EModDiagnosticSeverity::Warning));
			}

			TestEqual(TEXT("nothing was salvaged from the broken file"), Config->GetKeys(ModId).Num(), 0);

			Config->Shutdown();
		});

		Describe(TEXT("SetConfigJson"), [this]()
		{
			It(TEXT("replaces the whole user layer while defaults keep falling through"), [this]()
			{
				UModConfigManager* Config = NewObject<UModConfigManager>(GetTransientPackage());
				Config->Initialize(nullptr);

				const FModId ModId = MakeTestId(TEXT("test.systems.config.setjson"));
				const FString ModRoot = WriteConfigDefaults(TempDirectory, TEXT("setjson"), TEXT("defaults.json"),
					TEXT("{ \"fromDefaults\": 7, \"shared\": 1 }"));

				TArray<FModDiagnostic> Diagnostics;
				Config->LoadConfig(ModId, ModRoot, Diagnostics);
				Config->SetInt(ModId, FName(TEXT("willBeReplaced")), 42);

				TestTrue(TEXT("valid JSON is accepted"),
					Config->SetConfigJson(ModId, TEXT("{ \"shared\": 99, \"brandNew\": 5 }")));

				TestEqual(TEXT("the new user layer is in force"), Config->GetInt(ModId, FName(TEXT("shared")), 0), 99);
				TestEqual(TEXT("a new user key is readable"), Config->GetInt(ModId, FName(TEXT("brandNew")), 0), 5);
				TestFalse(TEXT("the previous user layer is gone, not merged"),
					Config->HasKey(ModId, FName(TEXT("willBeReplaced"))));
				TestEqual(TEXT("the defaults are untouched and still fall through"),
					Config->GetInt(ModId, FName(TEXT("fromDefaults")), 0), 7);

				Config->Shutdown();
			});

			// A mod handing over broken JSON must not end up with a half-written config it then reads
			// back as truth. The store has to be exactly what it was before the call.
			It(TEXT("rejects malformed JSON without corrupting the existing store"), [this]()
			{
				UModConfigManager* Config = NewObject<UModConfigManager>(GetTransientPackage());
				Config->Initialize(nullptr);

				const FModId ModId = MakeTestId(TEXT("test.systems.config.badjson"));
				const FString ModRoot = WriteConfigDefaults(TempDirectory, TEXT("badjson"), TEXT("defaults.json"),
					TEXT("{ \"fromDefaults\": 7 }"));

				TArray<FModDiagnostic> Diagnostics;
				Config->LoadConfig(ModId, ModRoot, Diagnostics);
				Config->SetInt(ModId, FName(TEXT("userValue")), 42);
				Config->SetString(ModId, FName(TEXT("userText")), TEXT("intact"));

				FString Before;
				TestTrue(TEXT("the merged view is readable before the bad call"), Config->GetConfigJson(ModId, Before));

				const TCHAR* const Malformed[] =
				{
					TEXT("{ \"unterminated\": "),
					TEXT("not json at all"),
					TEXT("{ \"key\" 1 }"),
					TEXT("[1, 2, 3]"),
					TEXT("")
				};

				for (const TCHAR* Bad : Malformed)
				{
					TestFalse(*FString::Printf(TEXT("'%s' is rejected"), Bad), Config->SetConfigJson(ModId, Bad));
				}

				// A payload over the file size cap is refused before it is even parsed.
				TestFalse(TEXT("an oversized payload is rejected"),
					Config->SetConfigJson(ModId,
						FString::ChrN(static_cast<int32>(UModConfigManager::MaxConfigFileBytes) + 1, TEXT('x'))));

				TestEqual(TEXT("the user value survived every rejection"), Config->GetInt(ModId, FName(TEXT("userValue")), 0), 42);
				ExpectText(TEXT("the user string survived"),
					Config->GetString(ModId, FName(TEXT("userText")), TEXT("")), TEXT("intact"));
				TestEqual(TEXT("the defaults survived"), Config->GetInt(ModId, FName(TEXT("fromDefaults")), 0), 7);

				FString After;
				TestTrue(TEXT("the merged view is still readable"), Config->GetConfigJson(ModId, After));
				ExpectText(TEXT("the whole store is byte-identical to what it was"), After, Before);

				Config->Shutdown();
			});
		});

		// DOCUMENTED GUARANTEE: a mod's config is its own namespace. One mod can neither read nor write
		// another's, and the framework - not the mod - decides the path, so a mod cannot name its way
		// out of its own file.
		It(TEXT("keeps every mod's configuration to itself"), [this]()
		{
			UModConfigManager* Config = NewObject<UModConfigManager>(GetTransientPackage());
			Config->Initialize(nullptr);

			const FModId First = MakeTestId(TEXT("test.systems.config.first"));
			const FModId Second = MakeTestId(TEXT("test.systems.config.second"));

			const FString FirstRoot = WriteConfigDefaults(TempDirectory, TEXT("first"), TEXT("defaults.json"),
				TEXT("{ \"sharedKey\": 1, \"firstOnly\": true }"));
			const FString SecondRoot = WriteConfigDefaults(TempDirectory, TEXT("second"), TEXT("defaults.json"),
				TEXT("{ \"sharedKey\": 2 }"));

			TArray<FModDiagnostic> Diagnostics;
			Config->LoadConfig(First, FirstRoot, Diagnostics);
			Config->LoadConfig(Second, SecondRoot, Diagnostics);

			Config->SetInt(First, FName(TEXT("sharedKey")), 100);

			TestEqual(TEXT("the first mod sees its own edit"), Config->GetInt(First, FName(TEXT("sharedKey")), 0), 100);
			TestEqual(TEXT("the second mod still sees its own default"), Config->GetInt(Second, FName(TEXT("sharedKey")), 0), 2);
			TestFalse(TEXT("the second mod cannot see the first mod's key"), Config->HasKey(Second, FName(TEXT("firstOnly"))));

			ExpectNames(TEXT("second mod keys"), Config->GetKeys(Second), TEXT("sharedKey"));

			// Unloading one mod leaves the other alone.
			Config->UnloadConfig(First);
			TestFalse(TEXT("the unloaded mod has no config"), Config->HasConfig(First));
			TestTrue(TEXT("the other mod still does"), Config->HasConfig(Second));
			TestEqual(TEXT("and still reads correctly"), Config->GetInt(Second, FName(TEXT("sharedKey")), 0), 2);

			// Every mod's user file is a distinct path the mod never supplied any part of.
			const FString FirstPath = Config->GetUserConfigPath(First);
			const FString SecondPath = Config->GetUserConfigPath(Second);
			TestTrue(TEXT("the two user config paths differ"), FirstPath != SecondPath);
			ExpectContains(TEXT("first user config path"), FirstPath, TEXT("test.systems.config.first.json"));

			Config->Shutdown();
		});

		It(TEXT("reads nothing and reports nothing for a mod that was never loaded"), [this]()
		{
			UModConfigManager* Config = NewObject<UModConfigManager>(GetTransientPackage());
			Config->Initialize(nullptr);

			const FModId ModId = MakeTestId(TEXT("test.systems.config.absent"));

			TestFalse(TEXT("HasConfig"), Config->HasConfig(ModId));
			TestFalse(TEXT("HasKey"), Config->HasKey(ModId, FName(TEXT("anything"))));
			TestEqual(TEXT("GetKeys"), Config->GetKeys(ModId).Num(), 0);
			TestEqual(TEXT("GetInt returns the caller's fallback"), Config->GetInt(ModId, FName(TEXT("anything")), 77), 77);

			FString Json;
			TestFalse(TEXT("GetConfigJson"), Config->GetConfigJson(ModId, Json));

			// Resetting a mod that has no store must be a no-op rather than a crash.
			Config->ResetToDefaults(ModId);
			TestFalse(TEXT("and it still has no config"), Config->HasConfig(ModId));

			Config->Shutdown();
		});
	});

	//~ -----------------------------------------------------------------------------------------------
	//~ Save data
	//~ -----------------------------------------------------------------------------------------------

	Describe(TEXT("Save"), [this]()
	{
		AfterEach([this]()
		{
			if (!SaveSlotName.IsEmpty())
			{
				UGameplayStatics::DeleteGameInSlot(SaveSlotName, 0);
				SaveSlotName.Reset();
			}
		});

		Describe(TEXT("Records"), [this]()
		{
			It(TEXT("round-trips a JSON payload and its data version"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				const FModId ModId = MakeTestId(TEXT("save.json"));

				TestTrue(TEXT("the write is accepted"),
					Manager->WriteModJson(ModId, TEXT("{\"score\":7,\"name\":\"Ada\"}"), 3));

				FString Json;
				int32 DataVersion = 0;
				TestTrue(TEXT("the record reads back"), Manager->ReadModJson(ModId, Json, DataVersion));
				ExpectText(TEXT("payload"), Json, TEXT("{\"score\":7,\"name\":\"Ada\"}"));
				TestEqual(TEXT("data version"), DataVersion, 3);

				// A mod with no record at all reads nothing and leaves the outputs cleared, so a caller
				// that ignores the return value cannot mistake stale data for its own.
				FString Other = TEXT("stale");
				int32 OtherVersion = 99;
				TestFalse(TEXT("an unwritten mod reads nothing"),
					Manager->ReadModJson(MakeTestId(TEXT("save.nothing")), Other, OtherVersion));
				TestTrue(TEXT("and the output string is cleared"), Other.IsEmpty());
				TestEqual(TEXT("and the output version is cleared"), OtherVersion, 0);

				Manager->Shutdown();
			});

			It(TEXT("round-trips a binary payload byte for byte"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				const FModId ModId = MakeTestId(TEXT("save.bytes"));

				TArray<uint8> Written;
				Written.Reserve(256);
				for (int32 Index = 0; Index < 256; ++Index)
				{
					Written.Add(static_cast<uint8>(Index));
				}

				TestTrue(TEXT("the write is accepted"), Manager->WriteModBytes(ModId, Written, 2));

				TArray<uint8> Read;
				int32 DataVersion = 0;
				TestTrue(TEXT("the record reads back"), Manager->ReadModBytes(ModId, Read, DataVersion));
				ExpectBytes(TEXT("binary payload"), Read, Written);
				TestEqual(TEXT("data version"), DataVersion, 2);

				Manager->Shutdown();
			});

			It(TEXT("keeps the JSON and binary payloads of one record independent"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				const FModId ModId = MakeTestId(TEXT("save.both"));

				const TArray<uint8> Bytes = { 1, 2, 3, 4 };
				TestTrue(TEXT("JSON write"), Manager->WriteModJson(ModId, TEXT("{\"a\":1}"), 1));
				TestTrue(TEXT("binary write"), Manager->WriteModBytes(ModId, Bytes, 4));

				FString Json;
				int32 JsonVersion = 0;
				TestTrue(TEXT("the JSON survived the binary write"), Manager->ReadModJson(ModId, Json, JsonVersion));
				ExpectText(TEXT("JSON payload"), Json, TEXT("{\"a\":1}"));

				TArray<uint8> ReadBytes;
				int32 BinaryVersion = 0;
				TestTrue(TEXT("the bytes read back"), Manager->ReadModBytes(ModId, ReadBytes, BinaryVersion));
				ExpectBytes(TEXT("binary payload"), ReadBytes, Bytes);

				// Both payloads live in one record, so both report that record's data version.
				TestEqual(TEXT("the record carries a single data version"), JsonVersion, 4);
				TestEqual(TEXT("shared by both payloads"), BinaryVersion, 4);

				TestEqual(TEXT("one record, not two"), Manager->GetEnvelope().Records.Num(), 1);

				Manager->Shutdown();
			});

			It(TEXT("empties a mod's payloads on ClearModData but keeps the record"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				const FModId ModId = MakeTestId(TEXT("save.clear"));

				Manager->WriteModJson(ModId, TEXT("{\"a\":1}"), 5);
				Manager->WriteModBytes(ModId, { 9, 9 }, 5);

				TestTrue(TEXT("clearing succeeds"), Manager->ClearModData(ModId));

				FString Json;
				int32 DataVersion = 0;
				TestFalse(TEXT("no JSON is left"), Manager->ReadModJson(ModId, Json, DataVersion));

				TArray<uint8> Bytes;
				TestFalse(TEXT("no bytes are left"), Manager->ReadModBytes(ModId, Bytes, DataVersion));

				// Only PurgeOrphanedRecord removes a record.
				TestEqual(TEXT("the record itself is still there"), Manager->GetEnvelope().Records.Num(), 1);

				const FModSaveRecord* Record = Manager->GetEnvelope().FindRecord(ModId);
				if (TestTrue(TEXT("and is still findable"), Record != nullptr))
				{
					TestTrue(TEXT("and is empty"), Record->IsEmpty());
					TestEqual(TEXT("and is back at data version 1"), Record->DataVersion, 1);
				}

				// Clearing a mod that has no data at all is a success, not a failure.
				TestTrue(TEXT("clearing an unknown mod succeeds"), Manager->ClearModData(MakeTestId(TEXT("save.never"))));

				Manager->Shutdown();
			});

			It(TEXT("refuses an invalid mod id or a data version below one, leaving the record untouched"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				const FModId ModId = MakeTestId(TEXT("save.reject"));

				Manager->WriteModJson(ModId, TEXT("{\"good\":1}"), 2);

				// Untrusted input: refused and logged, never asserted on.
				TestFalse(TEXT("an empty mod id is refused"), Manager->WriteModJson(FModId(), TEXT("{}"), 1));
				TestFalse(TEXT("data version 0 is refused"), Manager->WriteModJson(ModId, TEXT("{\"bad\":1}"), 0));
				TestFalse(TEXT("a negative data version is refused"), Manager->WriteModJson(ModId, TEXT("{\"bad\":1}"), -5));
				TestFalse(TEXT("an empty mod id is refused for bytes"), Manager->WriteModBytes(FModId(), { 1 }, 1));
				TestFalse(TEXT("data version 0 is refused for bytes"), Manager->WriteModBytes(ModId, { 1 }, 0));

				FString Json;
				int32 DataVersion = 0;
				TestTrue(TEXT("the good record is still there"), Manager->ReadModJson(ModId, Json, DataVersion));
				ExpectText(TEXT("with its original payload"), Json, TEXT("{\"good\":1}"));
				TestEqual(TEXT("and its original version"), DataVersion, 2);
				TestEqual(TEXT("and no record was created for the empty id"), Manager->GetEnvelope().Records.Num(), 1);

				Manager->Shutdown();
			});

			// A mod is not allowed to make a save file unwritable for everyone else, so an oversized
			// payload is refused outright and the previous one is left in place.
			It(TEXT("caps a payload at the documented limit and keeps the previous one when it is exceeded"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				const FModId ModId = MakeTestId(TEXT("save.big"));

				Manager->WriteModJson(ModId, TEXT("{\"small\":1}"), 1);

				{
					// Exactly at the cap is legal - the check is "greater than", not "greater or equal".
					const FString AtCap = FString::ChrN(static_cast<int32>(UModSaveDataManager::MaxJsonPayloadChars), TEXT('j'));
					TestTrue(TEXT("a JSON payload exactly at the cap is accepted"), Manager->WriteModJson(ModId, AtCap, 1));
				}

				{
					const FString OverCap = FString::ChrN(static_cast<int32>(UModSaveDataManager::MaxJsonPayloadChars) + 1, TEXT('k'));
					TestFalse(TEXT("one character over the cap is refused"), Manager->WriteModJson(ModId, OverCap, 1));
				}

				FString Stored;
				int32 DataVersion = 0;
				TestTrue(TEXT("the accepted payload survived the refusal"), Manager->ReadModJson(ModId, Stored, DataVersion));
				TestEqual(TEXT("at exactly the cap"),
					Stored.Len(), static_cast<int32>(UModSaveDataManager::MaxJsonPayloadChars));
				TestTrue(TEXT("and is the payload that was accepted, not the one that was refused"),
					Stored.Len() > 0 && Stored[0] == TEXT('j'));

				Manager->ClearModData(ModId);

				{
					TArray<uint8> OverCap;
					OverCap.SetNumUninitialized(static_cast<int32>(UModSaveDataManager::MaxBinaryPayloadBytes) + 1);
					TestFalse(TEXT("one byte over the binary cap is refused"), Manager->WriteModBytes(ModId, OverCap, 1));
				}

				TArray<uint8> ReadBytes;
				TestFalse(TEXT("and nothing was stored"), Manager->ReadModBytes(ModId, ReadBytes, DataVersion));

				Manager->Shutdown();
			});

			// DOCUMENTED GUARANTEE: the public API never lets one mod read or write another's record.
			It(TEXT("keeps every mod's save record to itself"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();

				const FModId First = MakeTestId(TEXT("save.first"));
				const FModId Second = MakeTestId(TEXT("save.second"));

				Manager->WriteModJson(First, TEXT("{\"owner\":\"first\"}"), 1);
				Manager->WriteModBytes(Second, { 2, 2, 2 }, 1);

				FString Json;
				int32 DataVersion = 0;
				TestTrue(TEXT("the first mod reads its own record"), Manager->ReadModJson(First, Json, DataVersion));
				ExpectText(TEXT("first mod payload"), Json, TEXT("{\"owner\":\"first\"}"));

				TestFalse(TEXT("the second mod has no JSON of its own"), Manager->ReadModJson(Second, Json, DataVersion));

				TArray<uint8> Bytes;
				TestTrue(TEXT("the second mod reads its own bytes"), Manager->ReadModBytes(Second, Bytes, DataVersion));
				ExpectBytes(TEXT("second mod payload"), Bytes, { 2, 2, 2 });
				TestFalse(TEXT("the first mod has no bytes of its own"), Manager->ReadModBytes(First, Bytes, DataVersion));

				// Clearing one mod cannot reach the other.
				Manager->ClearModData(First);
				TestTrue(TEXT("the second mod's data survived the first mod's clear"),
					Manager->ReadModBytes(Second, Bytes, DataVersion));

				TestEqual(TEXT("two records, one per mod"), Manager->GetEnvelope().Records.Num(), 2);

				Manager->Shutdown();
			});
		});

		Describe(TEXT("Orphans"), [this]()
		{
			// Documented: MarkOrphanedRecords does nothing at all when there is no subsystem to ask,
			// because a manager with nothing to ask cannot tell "absent" from "unknown" and guessing
			// would mislabel every record in the save.
			It(TEXT("leaves every orphan flag alone when there is no registry to ask"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				Manager->SetEnvelope(MakeOrphanEnvelope());

				Manager->MarkOrphanedRecords();

				const FModSaveRecord* Live = Manager->GetEnvelope().FindRecord(MakeTestId(TEXT("save.live")));
				const FModSaveRecord* Orphan = Manager->GetEnvelope().FindRecord(MakeTestId(TEXT("save.gone")));

				if (TestTrue(TEXT("the live record is present"), Live != nullptr))
				{
					TestFalse(TEXT("and was not marked orphaned"), Live->bOrphaned);
				}

				if (TestTrue(TEXT("the orphaned record is present"), Orphan != nullptr))
				{
					TestTrue(TEXT("and kept its orphan flag"), Orphan->bOrphaned);
				}

				// With no registry there is nothing to compare the stamp against, so reporting mods as
				// missing would be a lie.
				TestEqual(TEXT("no mods are reported missing"), Manager->GetMissingMods().Num(), 0);
				TestEqual(TEXT("and none are reported required"), Manager->GetMissingRequiredMods().Num(), 0);

				ExpectNames(TEXT("orphan list"), CollectRecordIds(Manager->GetOrphanedRecords()), TEXT("save.gone"));

				Manager->Shutdown();
			});

			// ===========================================================================================
			// THE CORE GUARANTEE OF THIS WHOLE CLASS. Removing a mod must never corrupt a save.
			//
			// A record whose mod is absent is flagged bOrphaned and then carried through every save and
			// load BYTE FOR BYTE, so uninstalling a mod, playing, saving and reinstalling it gets the
			// player's data back. Nothing in the normal flow may drop, truncate or rewrite one.
			// If this test ever becomes inconvenient, the fix is in the code, not in the test.
			// ===========================================================================================
			It(TEXT("carries an orphaned record through a save and load round trip byte for byte"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();

				const FModSaveEnvelope Original = MakeOrphanEnvelope();
				Manager->SetEnvelope(Original);

				SaveSlotName = FString::Printf(TEXT("ModFrameworkSystemsSpec_%s"),
					*FGuid::NewGuid().ToString(EGuidFormats::Digits));

				if (!TestTrue(TEXT("the envelope writes to a slot"), Manager->SaveToSlot(SaveSlotName, 0)))
				{
					Manager->Shutdown();
					return;
				}

				// Wipe the in-memory envelope so the comparison below can only be satisfied by what came
				// off disk.
				Manager->ResetEnvelope();
				TestEqual(TEXT("the envelope really was emptied"), Manager->GetEnvelope().Records.Num(), 0);

				if (!TestTrue(TEXT("the slot reads back"), Manager->LoadFromSlot(SaveSlotName, 0)))
				{
					Manager->Shutdown();
					return;
				}

				const FModSaveEnvelope& Loaded = Manager->GetEnvelope();

				TestEqual(TEXT("record count"), Loaded.Records.Num(), Original.Records.Num());
				ExpectNames(TEXT("record order"), CollectRecordIds(Loaded.Records), CollectRecordIdText(Original.Records));

				for (const FModSaveRecord& Expected : Original.Records)
				{
					const FModSaveRecord* Actual = Loaded.FindRecord(Expected.ModId);
					const FString What = FString::Printf(TEXT("record '%s'"), *Expected.ModId.ToString());

					if (!TestTrue(*FString::Printf(TEXT("%s survived the round trip"), *What), Actual != nullptr))
					{
						continue;
					}

					ExpectText(*FString::Printf(TEXT("%s: JSON"), *What), Actual->Json, Expected.Json);
					ExpectBytes(*FString::Printf(TEXT("%s: binary"), *What), Actual->Binary, Expected.Binary);
					TestEqual(*FString::Printf(TEXT("%s: data version"), *What), Actual->DataVersion, Expected.DataVersion);
					ExpectText(*FString::Printf(TEXT("%s: mod version"), *What),
						Actual->ModVersion.ToString(), Expected.ModVersion.ToString());
					TestEqual(*FString::Printf(TEXT("%s: orphan flag"), *What),
						Actual->bOrphaned ? 1 : 0, Expected.bOrphaned ? 1 : 0);
				}

				// The stamp survives too, so a "you are missing these mods" dialog can still be built
				// after the mods themselves are long gone.
				TestEqual(TEXT("stamped mod count"), Loaded.RequiredMods.Num(), Original.RequiredMods.Num());
				if (Loaded.RequiredMods.Num() == Original.RequiredMods.Num() && Loaded.RequiredMods.Num() > 0)
				{
					for (int32 Index = 0; Index < Original.RequiredMods.Num(); ++Index)
					{
						ExpectText(*FString::Printf(TEXT("stamp[%d] id"), Index),
							Loaded.RequiredMods[Index].ModId.ToString(), Original.RequiredMods[Index].ModId.ToString());
						ExpectText(*FString::Printf(TEXT("stamp[%d] version"), Index),
							Loaded.RequiredMods[Index].Version.ToString(), Original.RequiredMods[Index].Version.ToString());
						ExpectText(*FString::Printf(TEXT("stamp[%d] display name"), Index),
							Loaded.RequiredMods[Index].DisplayName, Original.RequiredMods[Index].DisplayName);
						TestEqual(*FString::Printf(TEXT("stamp[%d] required flag"), Index),
							Loaded.RequiredMods[Index].bWasRequired ? 1 : 0,
							Original.RequiredMods[Index].bWasRequired ? 1 : 0);
					}
				}

				// And a second round trip changes nothing either: the guarantee is about every save from
				// here on, not just the first one.
				if (TestTrue(TEXT("the loaded envelope writes again"), Manager->SaveToSlot(SaveSlotName, 0))
					&& TestTrue(TEXT("and reads again"), Manager->LoadFromSlot(SaveSlotName, 0)))
				{
					const FModSaveRecord* Orphan = Manager->GetEnvelope().FindRecord(MakeTestId(TEXT("save.gone")));
					if (TestTrue(TEXT("the orphan is still there after a second round trip"), Orphan != nullptr))
					{
						TestTrue(TEXT("still flagged"), Orphan->bOrphaned);
						ExpectText(TEXT("still carrying its JSON"), Orphan->Json, TEXT("{\"orphan\":\"do not touch\"}"));
						ExpectBytes(TEXT("still carrying its bytes"), Orphan->Binary, { 0xDE, 0xAD, 0xBE, 0xEF });
					}
				}

				Manager->Shutdown();
			});

			// PurgeOrphanedRecord is the single, explicit, loudly-logged way to destroy save data.
			// It must refuse everything else.
			It(TEXT("purges only an orphaned record, and only when asked directly"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				Manager->SetEnvelope(MakeOrphanEnvelope());

				// A live mod's data is never destroyed by accident.
				TestFalse(TEXT("purging a live record is refused"),
					Manager->PurgeOrphanedRecord(MakeTestId(TEXT("save.live"))));
				TestTrue(TEXT("and the live record is still there"),
					Manager->GetEnvelope().FindRecord(MakeTestId(TEXT("save.live"))) != nullptr);

				// A mod with no record at all is nothing to purge.
				TestFalse(TEXT("purging an unknown mod is refused"),
					Manager->PurgeOrphanedRecord(MakeTestId(TEXT("save.unknown"))));
				TestFalse(TEXT("an empty mod id is refused"), Manager->PurgeOrphanedRecord(FModId()));

				TestEqual(TEXT("nothing was removed"), Manager->GetEnvelope().Records.Num(), 2);

				TestTrue(TEXT("purging the orphan succeeds"),
					Manager->PurgeOrphanedRecord(MakeTestId(TEXT("save.gone"))));

				ExpectNames(TEXT("records left"), CollectRecordIds(Manager->GetEnvelope().Records), TEXT("save.live"));

				// Once no record for the mod remains, its stamp entry would only report the mod as
				// missing forever, so it goes too.
				TestTrue(TEXT("the purged mod's stamp entry is gone"),
					Manager->GetEnvelope().RequiredMods.FindByPredicate([](const FModSaveDependency& In)
					{
						return In.ModId == FModId(FName(TEXT("save.gone")));
					}) == nullptr);

				// The surviving mod's stamp is untouched.
				TestTrue(TEXT("the live mod's stamp entry survives"),
					Manager->GetEnvelope().RequiredMods.FindByPredicate([](const FModSaveDependency& In)
					{
						return In.ModId == FModId(FName(TEXT("save.live")));
					}) != nullptr);

				Manager->Shutdown();
			});
		});

		Describe(TEXT("Migration"), [this]()
		{
			// DOCUMENTED GUARANTEE: the framework walks a record forward ONE VERSION AT A TIME so a mod
			// only ever describes how to move forward by one, and it never migrates downwards.
			It(TEXT("walks one version at a time, in ascending order"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				UModSystemsTestSaveMigration* Migration = NewObject<UModSystemsTestSaveMigration>(GetTransientPackage());

				const FModId ModId = MakeTestId(TEXT("save.migrate"));
				Manager->WriteModJson(ModId, TEXT("base"), 1);
				Manager->RegisterMigration(ModId, TScriptInterface<IModSaveMigration>(Migration));

				TestTrue(TEXT("the migration succeeds"), Manager->MigrateRecord(ModId, 4));

				// Three single steps, not one jump from 1 to 4.
				TestEqual(TEXT("step count"), Migration->Steps.Num(), 3);
				ExpectText(TEXT("the steps that ran"), DescribeSteps(Migration->Steps), TEXT("1->2, 2->3, 3->4"));

				FString Json;
				int32 DataVersion = 0;
				TestTrue(TEXT("the record reads back"), Manager->ReadModJson(ModId, Json, DataVersion));
				ExpectText(TEXT("the payload records every step in order"), Json, TEXT("base[1->2][2->3][3->4]"));
				TestEqual(TEXT("final data version"), DataVersion, 4);

				Manager->Shutdown();
			});

			It(TEXT("does nothing when the record is already at the target version"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				UModSystemsTestSaveMigration* Migration = NewObject<UModSystemsTestSaveMigration>(GetTransientPackage());

				const FModId ModId = MakeTestId(TEXT("save.samever"));
				Manager->WriteModJson(ModId, TEXT("base"), 3);
				Manager->RegisterMigration(ModId, TScriptInterface<IModSaveMigration>(Migration));

				TestTrue(TEXT("migrating to the current version succeeds"), Manager->MigrateRecord(ModId, 3));
				TestEqual(TEXT("no step ran"), Migration->Steps.Num(), 0);

				FString Json;
				int32 DataVersion = 0;
				Manager->ReadModJson(ModId, Json, DataVersion);
				ExpectText(TEXT("the payload is untouched"), Json, TEXT("base"));
				TestEqual(TEXT("the version is untouched"), DataVersion, 3);

				Manager->Shutdown();
			});

			// The framework never migrates downwards. Guessing how to undo a mod's own schema change is
			// not something the framework can do, and silently accepting the request would leave a
			// record labelled with a version whose shape it does not have.
			It(TEXT("refuses to migrate downwards and leaves the record exactly as it was"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				UModSystemsTestSaveMigration* Migration = NewObject<UModSystemsTestSaveMigration>(GetTransientPackage());

				const FModId ModId = MakeTestId(TEXT("save.downgrade"));
				Manager->WriteModJson(ModId, TEXT("v5 payload"), 5);
				Manager->RegisterMigration(ModId, TScriptInterface<IModSaveMigration>(Migration));

				TestFalse(TEXT("migrating from 5 to 4 is refused"), Manager->MigrateRecord(ModId, 4));
				TestFalse(TEXT("migrating from 5 to 1 is refused"), Manager->MigrateRecord(ModId, 1));
				TestEqual(TEXT("no step was ever offered to the mod"), Migration->Steps.Num(), 0);

				FString Json;
				int32 DataVersion = 0;
				Manager->ReadModJson(ModId, Json, DataVersion);
				ExpectText(TEXT("the payload is untouched"), Json, TEXT("v5 payload"));
				TestEqual(TEXT("the version is untouched"), DataVersion, 5);

				// Versions start at 1, so anything below that is refused before the walk is even planned.
				TestFalse(TEXT("target version 0 is refused"), Manager->MigrateRecord(ModId, 0));
				TestFalse(TEXT("a negative target version is refused"), Manager->MigrateRecord(ModId, -3));

				Manager->Shutdown();
			});

			It(TEXT("refuses a walk longer than the documented step limit"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				UModSystemsTestSaveMigration* Migration = NewObject<UModSystemsTestSaveMigration>(GetTransientPackage());

				const FModId ModId = MakeTestId(TEXT("save.toomany"));
				Manager->WriteModJson(ModId, TEXT("base"), 1);
				Manager->RegisterMigration(ModId, TScriptInterface<IModSaveMigration>(Migration));

				// A hand-edited or hostile record must not be able to ask for an unbounded walk through
				// mod code.
				TestFalse(TEXT("a walk over the limit is refused"),
					Manager->MigrateRecord(ModId, 1 + UModSaveDataManager::MaxMigrationSteps + 1));
				TestEqual(TEXT("and not a single step ran"), Migration->Steps.Num(), 0);

				FString Json;
				int32 DataVersion = 0;
				Manager->ReadModJson(ModId, Json, DataVersion);
				TestEqual(TEXT("the record is untouched"), DataVersion, 1);

				Manager->Shutdown();
			});

			// The walk runs on a copy, so a step that gives up halfway cannot leave a half-migrated
			// record behind - the one state a player can never recover from.
			It(TEXT("discards the whole walk when a step refuses"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				UModSystemsTestSaveMigration* Migration = NewObject<UModSystemsTestSaveMigration>(GetTransientPackage());
				Migration->FailAtFromVersion = 3;

				const FModId ModId = MakeTestId(TEXT("save.failstep"));
				Manager->WriteModJson(ModId, TEXT("base"), 1);
				Manager->RegisterMigration(ModId, TScriptInterface<IModSaveMigration>(Migration));

				TestFalse(TEXT("the migration fails"), Manager->MigrateRecord(ModId, 5));

				// It got as far as the failing step and stopped there.
				ExpectText(TEXT("the steps that were attempted"), DescribeSteps(Migration->Steps), TEXT("1->2, 2->3, 3->4"));

				FString Json;
				int32 DataVersion = 0;
				Manager->ReadModJson(ModId, Json, DataVersion);
				ExpectText(TEXT("the stored payload has none of the successful steps applied"), Json, TEXT("base"));
				TestEqual(TEXT("and stayed at its original version"), DataVersion, 1);

				Manager->Shutdown();
			});

			It(TEXT("refuses to migrate when nothing usable is registered"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();

				const FModId ModId = MakeTestId(TEXT("save.nomigration"));
				Manager->WriteModJson(ModId, TEXT("base"), 1);

				TestFalse(TEXT("with no migration registered"), Manager->MigrateRecord(ModId, 2));

				FString Json;
				int32 DataVersion = 0;
				Manager->ReadModJson(ModId, Json, DataVersion);
				TestEqual(TEXT("the record is untouched"), DataVersion, 1);

				// A record that does not exist has nothing to migrate.
				TestFalse(TEXT("with no record at all"), Manager->MigrateRecord(MakeTestId(TEXT("save.norecord")), 2));

				// An object that does not implement the interface is rejected at registration time rather
				// than accepted and crashed on later. (The permission-policy double is used here purely
				// because it is a concrete UObject that does not implement IModSaveMigration.)
				TScriptInterface<IModSaveMigration> Bogus;
				Bogus.SetObject(NewObject<UModSystemsTestPermissionPolicy>(GetTransientPackage()));
				Manager->RegisterMigration(ModId, Bogus);
				TestFalse(TEXT("a bogus migration is not usable"), Manager->MigrateRecord(ModId, 2));

				Manager->Shutdown();
			});

			// A migration owns the payload and nothing else. Letting it rename the record would let one
			// mod's upgrade path overwrite another mod's data.
			It(TEXT("restores the record's identity and version after every step"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				UModSystemsTestSaveMigration* Migration = NewObject<UModSystemsTestSaveMigration>(GetTransientPackage());
				Migration->bRewriteBookkeeping = true;

				const FModId ModId = MakeTestId(TEXT("save.misbehave"));
				Manager->WriteModJson(ModId, TEXT("base"), 1);
				Manager->RegisterMigration(ModId, TScriptInterface<IModSaveMigration>(Migration));

				TestTrue(TEXT("the migration still succeeds"), Manager->MigrateRecord(ModId, 3));

				FString Json;
				int32 DataVersion = 0;
				TestTrue(TEXT("the record is still owned by the original mod"), Manager->ReadModJson(ModId, Json, DataVersion));
				ExpectText(TEXT("with the migrated payload"), Json, TEXT("base[1->2][2->3]"));
				TestEqual(TEXT("and the version the framework decided, not the one the mod claimed"), DataVersion, 3);

				TestTrue(TEXT("no record was created for the id the migration tried to claim"),
					Manager->GetEnvelope().FindRecord(MakeTestId(TEXT("some.other.mod"))) == nullptr);
				TestEqual(TEXT("still exactly one record"), Manager->GetEnvelope().Records.Num(), 1);

				Manager->Shutdown();
			});

			It(TEXT("drops a migration on UnregisterMigration"), [this]()
			{
				UModSaveDataManager* Manager = MakeSaveManager();
				UModSystemsTestSaveMigration* Migration = NewObject<UModSystemsTestSaveMigration>(GetTransientPackage());

				const FModId ModId = MakeTestId(TEXT("save.unregister"));
				Manager->WriteModJson(ModId, TEXT("base"), 1);
				Manager->RegisterMigration(ModId, TScriptInterface<IModSaveMigration>(Migration));

				TestTrue(TEXT("the first unregister removes it"), Manager->UnregisterMigration(ModId));
				TestFalse(TEXT("the second reports there was nothing to remove"), Manager->UnregisterMigration(ModId));
				TestFalse(TEXT("and migration is refused afterwards"), Manager->MigrateRecord(ModId, 2));
				TestEqual(TEXT("no step ran"), Migration->Steps.Num(), 0);

				Manager->Shutdown();
			});
		});
	});

	//~ -----------------------------------------------------------------------------------------------
	//~ Packaging
	//~ -----------------------------------------------------------------------------------------------

	Describe(TEXT("Packaging"), [this]()
	{
		BeforeEach([this]()
		{
			TempDirectory = FPaths::Combine(FPaths::AutomationTransientDir(),
				TEXT("ModFrameworkSystemsSpec"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().MakeDirectory(*TempDirectory, /*Tree=*/true);
		});

		AfterEach([this]()
		{
			if (!TempDirectory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*TempDirectory, /*RequireExists=*/false, /*Tree=*/true);
				TempDirectory.Reset();
			}
		});

		Describe(TEXT("Path safety"), [this]()
		{
			// ==========================================================================================
			// SECURITY TEST. ModPackage::IsSafeRelativePath is the single gate between a path chosen by
			// whoever built a `.mod` file and a path the framework will write to on a player's disk.
			// Everything it rejects is rejected on purpose: absolute paths, drive letters, UNC paths,
			// "..", NTFS alternate data streams, Windows device names and the trailing dot/space that
			// Windows silently strips. Do not "simplify" any case below away because it looks unusual -
			// unusual is exactly what an attack looks like.
			// ==========================================================================================

			It(TEXT("accepts an ordinary relative path and reports no error for it"), [this]()
			{
				const TCHAR* const SafePaths[] =
				{
					TEXT("mod.json"),
					TEXT("Content/Data/Values.json"),
					TEXT("a/b/c/d.txt"),
					TEXT("file with spaces.txt"),
					TEXT("UPPER/Case.TXT"),
					TEXT("dots.in.name.txt"),
					TEXT("-leading-dash.txt"),
					TEXT(".hidden")
				};

				for (const TCHAR* Path : SafePaths)
				{
					FString PathError = TEXT("sentinel");
					if (!TestTrue(*FString::Printf(TEXT("'%s' is accepted"), Path),
						ModPackage::IsSafeRelativePath(Path, PathError)))
					{
						AddError(FString::Printf(TEXT("'%s' was rejected: %s"), Path, *PathError));
						continue;
					}

					TestTrue(*FString::Printf(TEXT("'%s' clears the error string"), Path), PathError.IsEmpty());
				}
			});

			It(TEXT("rejects every path that could escape the package root or name something dangerous"), [this]()
			{
				const TCHAR* const UnsafePaths[] =
				{
					// Nothing to write to.
					TEXT(""),
					// Directory traversal, plain and buried.
					TEXT("../escape.txt"),
					TEXT("Content/../../escape.txt"),
					TEXT("Content/./escape.txt"),
					// Absolute, drive-lettered and UNC.
					TEXT("/absolute.txt"),
					TEXT("C:/Windows/System32/evil.dll"),
					TEXT("//server/share/evil.dll"),
					TEXT("Content\\backslash.txt"),
					// An NTFS alternate data stream hides behind a colon.
					TEXT("Content/file.txt:hidden"),
					// Empty and trailing segments resolve to surprising places.
					TEXT("Content//double.txt"),
					TEXT("Content/trailing/"),
					// Windows silently strips these, so two TOC entries could land on one file.
					TEXT("Content/name./file.txt"),
					TEXT("Content/name /file.txt"),
					// Reserved device names, with and without an extension.
					TEXT("CON"),
					TEXT("Content/NUL.txt"),
					TEXT("Content/LPT1.dat"),
					TEXT("Content/com9"),
					// Shell wildcards and characters no filesystem here accepts.
					TEXT("Content/star*.txt"),
					TEXT("Content/question?.txt"),
					TEXT("Content/quote\".txt"),
					TEXT("Content/pipe|.txt"),
					TEXT("Content/less<.txt"),
					TEXT("Content/greater>.txt")
				};

				for (const TCHAR* Path : UnsafePaths)
				{
					FString PathError;
					if (ModPackage::IsSafeRelativePath(Path, PathError))
					{
						AddError(FString::Printf(TEXT("'%s' was accepted, but it must be refused."), Path));
						continue;
					}

					// A refusal a mod author cannot read is a refusal they cannot fix.
					TestFalse(*FString::Printf(TEXT("'%s' explains itself"), Path), PathError.IsEmpty());
				}

				// A control character and a NUL are rejected too, but they cannot be written as a
				// literal in the table above.
				FString ControlPath = TEXT("Content/bell.txt");
				ControlPath[7] = TCHAR(0x07);
				FString ControlError;
				TestFalse(TEXT("a path with a control character is refused"),
					ModPackage::IsSafeRelativePath(ControlPath, ControlError));

				// One character over the cap, checked before anything allocates for it.
				FString LongPath = FString::ChrN(ModPackage::MaxEntryPathBytes + 1, TEXT('a'));
				FString LongError;
				TestFalse(TEXT("an over-long path is refused"), ModPackage::IsSafeRelativePath(LongPath, LongError));
				ExpectContains(TEXT("over-long path"), LongError, TEXT("more than the permitted"));

				// More segments than the cap allows.
				FString DeepPath;
				for (int32 Index = 0; Index <= ModPackage::MaxEntryPathSegments; ++Index)
				{
					DeepPath += TEXT("d/");
				}
				DeepPath += TEXT("file.txt");

				FString DeepError;
				TestFalse(TEXT("an over-deep path is refused"), ModPackage::IsSafeRelativePath(DeepPath, DeepError));
			});

			It(TEXT("folds a lookup key without making an unsafe path safe"), [this]()
			{
				ExpectText(TEXT("backslashes and case"), ModPackage::MakeEntryKey(TEXT("A\\B.TXT")), TEXT("a/b.txt"));
				ExpectText(TEXT("a leading ./"), ModPackage::MakeEntryKey(TEXT("./a/b")), TEXT("a/b"));
				ExpectText(TEXT("a leading /"), ModPackage::MakeEntryKey(TEXT("/a/b")), TEXT("a/b"));
				ExpectText(TEXT("a mix of both"), ModPackage::MakeEntryKey(TEXT(".//./a")), TEXT("a"));
				ExpectText(TEXT("an already-folded key"), ModPackage::MakeEntryKey(TEXT("a/b.txt")), TEXT("a/b.txt"));

				// Normalising is not sanitising: ".." is still there afterwards, which is why the reader
				// validates the raw path rather than the folded key.
				FString PathError;
				TestFalse(TEXT("folding does not launder a traversal"),
					ModPackage::IsSafeRelativePath(ModPackage::MakeEntryKey(TEXT("../escape.txt")), PathError));
			});
		});

		Describe(TEXT("Reading a well-formed package"), [this]()
		{
			// The hostile fixtures below only prove anything if the fixture builder itself produces a
			// package the reader accepts. This test is that control.
			It(TEXT("opens a package and exposes its manifest, entries and content hash"), [this]()
			{
				const FString PackagePath = MakeFixturePath(TEXT("good.mod"));
				const TArray<FPackageFixtureEntry> Sources = MakeStandardEntries();

				TestTrue(TEXT("the fixture was written"),
					WritePackageFixture(PackagePath, GetFixtureManifestJson(), Sources, FPackageFixtureOptions()));

				TArray<FModDiagnostic> Diagnostics;
				FModPackageReader Reader;

				if (!TestTrue(TEXT("the package opens"), Reader.Open(PackagePath, Diagnostics)))
				{
					AddError(FString::Printf(TEXT("Opening a well-formed fixture failed:\n%s"),
						*ModDiagnostics::Join(Diagnostics)));
					return;
				}

				ExpectNoDiagnostics(TEXT("opening a well-formed package"), Diagnostics);
				TestTrue(TEXT("the reader reports itself open"), Reader.IsOpen());
				ExpectText(TEXT("the package path is remembered"), Reader.GetPackagePath(), PackagePath);

				ExpectText(TEXT("embedded manifest id"), Reader.GetManifest().Id.ToString(), TEXT("com.example.testpkg"));
				ExpectText(TEXT("embedded manifest name"), Reader.GetManifest().DisplayName, TEXT("Test Package"));
				ExpectText(TEXT("embedded manifest version"), Reader.GetManifest().Version.ToString(), TEXT("1.0.0"));

				const TArray<FModPackageEntry> Entries = Reader.GetEntries();
				if (TestEqual(TEXT("entry count"), Entries.Num(), 3))
				{
					// The table of contents comes back in file order, not sorted or hashed order.
					ExpectText(TEXT("entries[0]"), Entries[0].RelativePath, TEXT("mod.json"));
					ExpectText(TEXT("entries[1]"), Entries[1].RelativePath, TEXT("Content/Data/Values.json"));
					ExpectText(TEXT("entries[2]"), Entries[2].RelativePath, TEXT("Readme.txt"));

					TestEqual(TEXT("entries[2] uncompressed size"),
						static_cast<int32>(Entries[2].UncompressedSize), Sources[2].Bytes.Num());
					TestEqual(TEXT("a stored entry has one size"),
						static_cast<int32>(Entries[2].CompressedSize), static_cast<int32>(Entries[2].UncompressedSize));
					TestFalse(TEXT("and is not marked compressed"), Entries[2].bCompressed);
					TestEqual(TEXT("its hash field is 40 characters"), Entries[2].Hash.Len(), ModPackage::HashHexLength);
				}

				// Lookup folds case and accepts backslashes, because packages are authored on
				// case-insensitive filesystems more often than not.
				TestTrue(TEXT("HasEntry, exact"), Reader.HasEntry(TEXT("Content/Data/Values.json")));
				TestTrue(TEXT("HasEntry, folded case"), Reader.HasEntry(TEXT("content/data/values.json")));
				TestTrue(TEXT("HasEntry, backslashes"), Reader.HasEntry(TEXT("Content\\Data\\Values.json")));
				TestFalse(TEXT("HasEntry, absent"), Reader.HasEntry(TEXT("Content/Missing.json")));

				FModPackageEntry Found;
				if (TestTrue(TEXT("FindEntry"), Reader.FindEntry(TEXT("readme.txt"), Found)))
				{
					ExpectText(TEXT("FindEntry keeps the stored spelling"), Found.RelativePath, TEXT("Readme.txt"));
				}

				TArray<uint8> Bytes;
				FModDiagnostic ReadError;
				if (TestTrue(TEXT("ReadEntry"), Reader.ReadEntry(TEXT("Readme.txt"), Bytes, ReadError)))
				{
					ExpectBytes(TEXT("payload bytes"), Bytes, Sources[2].Bytes);
				}

				TestFalse(TEXT("reading an absent entry fails"),
					Reader.ReadEntry(TEXT("Content/Missing.json"), Bytes, ReadError));
				ExpectText(TEXT("and says why"), ReadError.Code.ToString(), TEXT("Package.EntryNotFound"));
				TestEqual(TEXT("and empties the output"), Bytes.Num(), 0);

				// The header's content hash is the reader's own digest over the final TOC, so the two
				// can never drift apart.
				ExpectText(TEXT("header content hash"), Reader.GetHeader().ContentHash,
					FModPackageReader::ComputeContentHash(Entries));

				TArray<FModDiagnostic> VerifyDiagnostics;
				TestTrue(TEXT("VerifyIntegrity"), Reader.VerifyIntegrity(VerifyDiagnostics));
				ExpectNoDiagnostics(TEXT("verifying a well-formed package"), VerifyDiagnostics);

				Reader.Close();
				TestFalse(TEXT("the reader is closed afterwards"), Reader.IsOpen());
				TestEqual(TEXT("and forgets its entries"), Reader.GetEntries().Num(), 0);
			});

			It(TEXT("peeks the manifest without validating the table of contents"), [this]()
			{
				const FString PackagePath = MakeFixturePath(TEXT("peek.mod"));
				WritePackageFixture(PackagePath, GetFixtureManifestJson(), MakeStandardEntries(), FPackageFixtureOptions());

				FModManifest Manifest;
				TArray<FModDiagnostic> Diagnostics;
				TestTrue(TEXT("PeekManifest succeeds"), FModPackageReader::PeekManifest(PackagePath, Manifest, Diagnostics));
				ExpectNoDiagnostics(TEXT("peeking a well-formed package"), Diagnostics);
				ExpectText(TEXT("peeked id"), Manifest.Id.ToString(), TEXT("com.example.testpkg"));

				// A failed peek must not leave a half-filled manifest behind.
				FPackageFixtureOptions BadMagic;
				BadMagic.Magic = 0xDEADBEEFu;
				const FString BadPath = MakeFixturePath(TEXT("peek-badmagic.mod"));
				WritePackageFixture(BadPath, GetFixtureManifestJson(), MakeStandardEntries(), BadMagic);

				TArray<FModDiagnostic> BadDiagnostics;
				TestFalse(TEXT("PeekManifest refuses a bad signature"),
					FModPackageReader::PeekManifest(BadPath, Manifest, BadDiagnostics));
				ExpectDiagnosticCode(TEXT("peek bad magic"), BadDiagnostics, TEXT("Package.BadMagic"));
				TestFalse(TEXT("and clears the caller's manifest"), Manifest.Id.IsValid());
			});

			// DOCUMENTED GUARANTEE: every extracted path is rebuilt from the entry's relative path,
			// resolved to an absolute path and re-checked to still be strictly under the destination.
			// Nothing a package names may land outside the directory the caller chose.
			It(TEXT("extracts every entry underneath the destination directory and nowhere else"), [this]()
			{
				const FString PackagePath = MakeFixturePath(TEXT("extract.mod"));
				const TArray<FPackageFixtureEntry> Sources = MakeStandardEntries();
				WritePackageFixture(PackagePath, GetFixtureManifestJson(), Sources, FPackageFixtureOptions());

				const FString Destination = FPaths::Combine(TempDirectory, TEXT("Extracted"));
				const FString Sibling = FPaths::Combine(TempDirectory, TEXT("Sibling"));
				IFileManager::Get().MakeDirectory(*Sibling, /*Tree=*/true);

				TArray<FModDiagnostic> Diagnostics;
				FModPackageReader Reader;
				if (!TestTrue(TEXT("the package opens"), Reader.Open(PackagePath, Diagnostics)))
				{
					return;
				}

				TestTrue(TEXT("extraction succeeds"), Reader.ExtractAll(Destination, Diagnostics));
				ExpectNoDiagnostics(TEXT("extracting a well-formed package"), Diagnostics);

				for (const FPackageFixtureEntry& Source : Sources)
				{
					const FString TargetPath = FPaths::Combine(Destination, Source.RelativePath);
					if (!TestTrue(*FString::Printf(TEXT("'%s' was written"), *Source.RelativePath),
						FPaths::FileExists(TargetPath)))
					{
						continue;
					}

					TArray<uint8> OnDisk;
					if (TestTrue(*FString::Printf(TEXT("'%s' is readable"), *Source.RelativePath),
						FFileHelper::LoadFileToArray(OnDisk, *TargetPath)))
					{
						ExpectBytes(*FString::Printf(TEXT("'%s' contents"), *Source.RelativePath), OnDisk, Source.Bytes);
					}
				}

				// Intermediate directories are created, and nothing appeared beside the destination.
				TestTrue(TEXT("the nested directory was created"),
					IFileManager::Get().DirectoryExists(*FPaths::Combine(Destination, TEXT("Content"), TEXT("Data"))));

				TArray<FString> SiblingFiles;
				IFileManager::Get().FindFilesRecursive(SiblingFiles, *Sibling, TEXT("*"), true, true);
				TestEqual(TEXT("nothing was written into a sibling directory"), SiblingFiles.Num(), 0);

				Reader.Close();
			});
		});

		Describe(TEXT("Rejecting a hostile package"), [this]()
		{
			// ==========================================================================================
			// SECURITY TESTS. FModPackageReader treats every byte of a `.mod` file as hostile until
			// proven otherwise, and each fixture below is a specific way a file could lie about itself.
			// They all assert the EXACT diagnostic code, because the codes are the stable contract the
			// rest of the framework and any mod-manager UI keys off. None of these may be reduced to
			// "the open failed".
			// ==========================================================================================

			It(TEXT("refuses a file that is not a mod package at all"), [this]()
			{
				FPackageFixtureOptions Options;
				Options.Magic = 0xDEADBEEFu;

				const FString PackagePath = MakeFixturePath(TEXT("badmagic.mod"));
				WritePackageFixture(PackagePath, GetFixtureManifestJson(), MakeStandardEntries(), Options);

				ExpectPackageRejected(TEXT("bad magic"), PackagePath, TEXT("Package.BadMagic"));
			});

			It(TEXT("refuses a format version this build does not understand"), [this]()
			{
				// Newer than anything this build can read: refusing beats guessing at a layout change.
				FPackageFixtureOptions FromTheFuture;
				FromTheFuture.FormatVersion = ModPackage::CurrentFormatVersion + 1;

				const FString FuturePath = MakeFixturePath(TEXT("future.mod"));
				WritePackageFixture(FuturePath, GetFixtureManifestJson(), MakeStandardEntries(), FromTheFuture);
				ExpectPackageRejected(TEXT("a future format version"), FuturePath, TEXT("Package.UnsupportedVersion"));

				// Version 0 is not a legal revision either, and is what a zero-filled header looks like.
				FPackageFixtureOptions Zero;
				Zero.FormatVersion = 0;

				const FString ZeroPath = MakeFixturePath(TEXT("version0.mod"));
				WritePackageFixture(ZeroPath, GetFixtureManifestJson(), MakeStandardEntries(), Zero);
				ExpectPackageRejected(TEXT("format version 0"), ZeroPath, TEXT("Package.UnsupportedVersion"));
			});

			It(TEXT("refuses a file too short to hold a header"), [this]()
			{
				const FString PackagePath = MakeFixturePath(TEXT("truncated.mod"));

				TArray<uint8> Stub;
				Stub.SetNumZeroed(static_cast<int32>(ModPackage::HeaderSize) - 1);
				TestTrue(TEXT("the stub was written"), FFileHelper::SaveArrayToFile(Stub, *PackagePath));

				ExpectPackageRejected(TEXT("a file shorter than the header"), PackagePath, TEXT("Package.Corrupt"));
			});

			It(TEXT("refuses a path that does not exist"), [this]()
			{
				ExpectPackageRejected(TEXT("a missing file"), MakeFixturePath(TEXT("does-not-exist.mod")),
					TEXT("Package.OpenFailed"));

				TArray<FModDiagnostic> Diagnostics;
				FModPackageReader Reader;
				TestFalse(TEXT("an empty path is refused"), Reader.Open(FString(), Diagnostics));
				ExpectDiagnosticCode(TEXT("empty path"), Diagnostics, TEXT("Package.OpenFailed"));
			});

			It(TEXT("refuses a manifest region that does not lie inside the file"), [this]()
			{
				const int64 FarPastEndOfFile = 1024LL * 1024LL * 1024LL;

				FPackageFixtureOptions PastEnd;
				PastEnd.ManifestOffsetOverride = FarPastEndOfFile;

				const FString PastEndPath = MakeFixturePath(TEXT("manifest-past-eof.mod"));
				WritePackageFixture(PastEndPath, GetFixtureManifestJson(), MakeStandardEntries(), PastEnd);
				ExpectPackageRejected(TEXT("a manifest offset past the end of the file"), PastEndPath, TEXT("Package.Corrupt"));

				// An offset inside the header is impossible too: the header is fixed size and comes first.
				FPackageFixtureOptions IntoHeader;
				IntoHeader.ManifestOffsetOverride = 8;

				const FString IntoHeaderPath = MakeFixturePath(TEXT("manifest-in-header.mod"));
				WritePackageFixture(IntoHeaderPath, GetFixtureManifestJson(), MakeStandardEntries(), IntoHeader);
				ExpectPackageRejected(TEXT("a manifest offset inside the header"), IntoHeaderPath, TEXT("Package.Corrupt"));

				// A size that overruns the file, even from a legal offset.
				FPackageFixtureOptions Overrun;
				Overrun.ManifestSizeOverride = 1024 * 1024;

				const FString OverrunPath = MakeFixturePath(TEXT("manifest-overrun.mod"));
				WritePackageFixture(OverrunPath, GetFixtureManifestJson(), MakeStandardEntries(), Overrun);
				ExpectPackageRejected(TEXT("a manifest size that overruns the file"), OverrunPath, TEXT("Package.Corrupt"));

				// A declared size beyond the documented cap is refused before anything allocates for it.
				FPackageFixtureOptions HugeManifest;
				HugeManifest.ManifestSizeOverride = ModPackage::MaxManifestBytes + 1;

				const FString HugePath = MakeFixturePath(TEXT("manifest-huge.mod"));
				WritePackageFixture(HugePath, GetFixtureManifestJson(), MakeStandardEntries(), HugeManifest);
				ExpectPackageRejected(TEXT("an absurd manifest size"), HugePath, TEXT("Package.TooLarge"));

				// A zero-length manifest is not a package either.
				FPackageFixtureOptions EmptyManifest;
				EmptyManifest.ManifestSizeOverride = 0;

				const FString EmptyPath = MakeFixturePath(TEXT("manifest-empty.mod"));
				WritePackageFixture(EmptyPath, GetFixtureManifestJson(), MakeStandardEntries(), EmptyManifest);
				ExpectPackageRejected(TEXT("an empty manifest region"), EmptyPath, TEXT("Package.Corrupt"));
			});

			It(TEXT("refuses a table of contents that does not lie inside the file"), [this]()
			{
				FPackageFixtureOptions PastEnd;
				PastEnd.TocOffsetOverride = 1024LL * 1024LL * 1024LL;

				const FString PackagePath = MakeFixturePath(TEXT("toc-past-eof.mod"));
				WritePackageFixture(PackagePath, GetFixtureManifestJson(), MakeStandardEntries(), PastEnd);
				ExpectPackageRejected(TEXT("a TOC offset past the end of the file"), PackagePath, TEXT("Package.Corrupt"));
			});

			It(TEXT("refuses an absurd entry count before it allocates for it"), [this]()
			{
				// Far beyond the documented cap: this is the allocation bomb the cap exists to stop.
				FPackageFixtureOptions Absurd;
				Absurd.EntryCountOverride = ModPackage::MaxEntries + 1;

				const FString AbsurdPath = MakeFixturePath(TEXT("entrycount-absurd.mod"));
				WritePackageFixture(AbsurdPath, GetFixtureManifestJson(), TArray<FPackageFixtureEntry>(), Absurd);
				ExpectPackageRejected(TEXT("an entry count over the cap"), AbsurdPath, TEXT("Package.TooLarge"));

				FPackageFixtureOptions Enormous;
				Enormous.EntryCountOverride = MAX_int32;

				const FString EnormousPath = MakeFixturePath(TEXT("entrycount-max.mod"));
				WritePackageFixture(EnormousPath, GetFixtureManifestJson(), TArray<FPackageFixtureEntry>(), Enormous);
				ExpectPackageRejected(TEXT("an entry count of MAX_int32"), EnormousPath, TEXT("Package.TooLarge"));

				// Under the cap but larger than the region could possibly hold: a lie the reader catches
				// from arithmetic alone, before it reserves a single entry.
				FPackageFixtureOptions Impossible;
				Impossible.EntryCountOverride = 100;

				const FString ImpossiblePath = MakeFixturePath(TEXT("entrycount-impossible.mod"));
				WritePackageFixture(ImpossiblePath, GetFixtureManifestJson(), MakeStandardEntries(), Impossible);
				ExpectPackageRejected(TEXT("an entry count the region cannot hold"), ImpossiblePath, TEXT("Package.Corrupt"));

				// A negative count is not a count.
				FPackageFixtureOptions Negative;
				Negative.EntryCountOverride = -1;

				const FString NegativePath = MakeFixturePath(TEXT("entrycount-negative.mod"));
				WritePackageFixture(NegativePath, GetFixtureManifestJson(), TArray<FPackageFixtureEntry>(), Negative);
				ExpectPackageRejected(TEXT("a negative entry count"), NegativePath, TEXT("Package.Corrupt"));
			});

			It(TEXT("refuses an entry whose payload does not lie inside the file"), [this]()
			{
				FPackageFixtureOptions PastEnd;
				PastEnd.FirstEntryOffsetOverride = 1024LL * 1024LL * 1024LL;

				const FString PastEndPath = MakeFixturePath(TEXT("entry-past-eof.mod"));
				WritePackageFixture(PastEndPath, GetFixtureManifestJson(), MakeStandardEntries(), PastEnd);
				ExpectPackageRejected(TEXT("an entry offset past the end of the file"), PastEndPath, TEXT("Package.Corrupt"));

				// An offset inside the header would let an entry read the header back at itself.
				FPackageFixtureOptions IntoHeader;
				IntoHeader.FirstEntryOffsetOverride = 0;

				const FString IntoHeaderPath = MakeFixturePath(TEXT("entry-in-header.mod"));
				WritePackageFixture(IntoHeaderPath, GetFixtureManifestJson(), MakeStandardEntries(), IntoHeader);
				ExpectPackageRejected(TEXT("an entry offset inside the header"), IntoHeaderPath, TEXT("Package.Corrupt"));

				// A stored (uncompressed) entry has exactly one size, so a mismatch is unambiguous. Here
				// the declared stored size is larger than the file itself.
				FPackageFixtureOptions Overrun;
				Overrun.FirstEntryCompressedSizeOverride = 1024 * 1024;

				const FString OverrunPath = MakeFixturePath(TEXT("entry-overrun.mod"));
				WritePackageFixture(OverrunPath, GetFixtureManifestJson(), MakeStandardEntries(), Overrun);
				ExpectPackageRejected(TEXT("an entry claiming more stored bytes than the file holds"),
					OverrunPath, TEXT("Package.Corrupt"));

				// The same rule catches a stored entry whose two sizes simply disagree.
				FPackageFixtureOptions Mismatched;
				Mismatched.FirstEntryCompressedSizeOverride = 1;

				const FString MismatchedPath = MakeFixturePath(TEXT("entry-size-mismatch.mod"));
				WritePackageFixture(MismatchedPath, GetFixtureManifestJson(), MakeStandardEntries(), Mismatched);
				ExpectPackageRejected(TEXT("a stored entry with two different sizes"), MismatchedPath, TEXT("Package.Corrupt"));

				// A size beyond the per-entry cap is refused before any buffer is sized from it.
				FPackageFixtureOptions Huge;
				Huge.FirstEntryCompressedSizeOverride = ModPackage::MaxEntryUncompressedBytes + 1;

				const FString HugePath = MakeFixturePath(TEXT("entry-huge.mod"));
				WritePackageFixture(HugePath, GetFixtureManifestJson(), MakeStandardEntries(), Huge);
				ExpectPackageRejected(TEXT("an entry size over the cap"), HugePath, TEXT("Package.TooLarge"));

				// And a negative one, which a naive bounds check would let through as "small".
				FPackageFixtureOptions NegativeSize;
				NegativeSize.FirstEntryCompressedSizeOverride = -1;

				const FString NegativePath = MakeFixturePath(TEXT("entry-negative-size.mod"));
				WritePackageFixture(NegativePath, GetFixtureManifestJson(), MakeStandardEntries(), NegativeSize);
				ExpectPackageRejected(TEXT("a negative entry size"), NegativePath, TEXT("Package.TooLarge"));
			});

			// SECURITY TEST: an entry path is the only part of a package that becomes a path on the
			// player's disk. Every one of these is refused at OPEN time, so nothing downstream ever sees
			// a package containing one.
			It(TEXT("refuses the whole package when any entry path is unsafe"), [this]()
			{
				const TCHAR* const UnsafePaths[] =
				{
					TEXT(""),
					TEXT("../escape.txt"),
					TEXT("Content/../../escape.txt"),
					TEXT("/absolute.txt"),
					TEXT("C:/Windows/System32/evil.dll"),
					TEXT("Content\\backslash.txt"),
					TEXT("Content/file.txt:stream"),
					TEXT("Content//double.txt"),
					TEXT("Content/trailing/"),
					TEXT("CON"),
					TEXT("Content/NUL.txt"),
					TEXT("Content/name./file.txt")
				};

				int32 FixtureIndex = 0;
				for (const TCHAR* Path : UnsafePaths)
				{
					TArray<FPackageFixtureEntry> Entries;
					Entries.Add({ TEXT("mod.json"), MakePayload(GetFixtureManifestJson()), false });
					Entries.Add({ Path, MakePayload(TEXT("payload")), false });

					const FString PackagePath = MakeFixturePath(*FString::Printf(TEXT("unsafe-%d.mod"), FixtureIndex++));
					WritePackageFixture(PackagePath, GetFixtureManifestJson(), Entries, FPackageFixtureOptions());

					ExpectPackageRejected(*FString::Printf(TEXT("an entry named '%s'"), Path),
						PackagePath, TEXT("Package.UnsafePath"));
				}
			});

			It(TEXT("refuses a table of contents that lists the same file twice"), [this]()
			{
				// Compared case-insensitively, because two entries that differ only in case would
				// resolve to one file on a Windows or macOS filesystem.
				TArray<FPackageFixtureEntry> Entries;
				Entries.Add({ TEXT("Content/Values.json"), MakePayload(TEXT("first")), false });
				Entries.Add({ TEXT("content/VALUES.json"), MakePayload(TEXT("second")), false });

				const FString PackagePath = MakeFixturePath(TEXT("duplicate.mod"));
				WritePackageFixture(PackagePath, GetFixtureManifestJson(), Entries, FPackageFixtureOptions());

				ExpectPackageRejected(TEXT("a duplicated entry path"), PackagePath, TEXT("Package.Corrupt"));
			});

			It(TEXT("refuses an unparseable embedded manifest"), [this]()
			{
				const FString PackagePath = MakeFixturePath(TEXT("badmanifest.mod"));
				WritePackageFixture(PackagePath, TEXT("{ this is not a manifest"),
					MakeStandardEntries(), FPackageFixtureOptions());

				ExpectPackageRejected(TEXT("a malformed embedded manifest"), PackagePath, TEXT("Package.Corrupt"));
			});

			It(TEXT("refuses an entry whose bytes do not match its recorded hash"), [this]()
			{
				TArray<FPackageFixtureEntry> Entries = MakeStandardEntries();
				Entries[2].bCorruptHash = true;

				const FString PackagePath = MakeFixturePath(TEXT("badhash.mod"));
				WritePackageFixture(PackagePath, GetFixtureManifestJson(), Entries, FPackageFixtureOptions());

				TArray<FModDiagnostic> Diagnostics;
				FModPackageReader Reader;

				// Open only validates structure; the payload hash is checked when the bytes are read.
				if (!TestTrue(TEXT("the package opens"), Reader.Open(PackagePath, Diagnostics)))
				{
					return;
				}

				TArray<uint8> Bytes;
				FModDiagnostic ReadError;
				TestFalse(TEXT("reading the tampered entry fails"), Reader.ReadEntry(TEXT("Readme.txt"), Bytes, ReadError));
				ExpectText(TEXT("read error code"), ReadError.Code.ToString(), TEXT("Package.HashMismatch"));
				TestEqual(TEXT("and no bytes are handed back"), Bytes.Num(), 0);

				// An untampered entry in the same package still reads.
				TestTrue(TEXT("an untouched entry still reads"),
					Reader.ReadEntry(TEXT("Content/Data/Values.json"), Bytes, ReadError));

				TArray<FModDiagnostic> VerifyDiagnostics;
				TestFalse(TEXT("VerifyIntegrity fails"), Reader.VerifyIntegrity(VerifyDiagnostics));
				ExpectDiagnosticCode(TEXT("verify"), VerifyDiagnostics, TEXT("Package.HashMismatch"));

				// Extraction stops at the first failure rather than writing bytes it could not verify.
				TArray<FModDiagnostic> ExtractDiagnostics;
				TestFalse(TEXT("extraction fails"),
					Reader.ExtractAll(FPaths::Combine(TempDirectory, TEXT("BadHashOut")), ExtractDiagnostics));
				ExpectDiagnosticCode(TEXT("extract"), ExtractDiagnostics, TEXT("Package.HashMismatch"));
				TestFalse(TEXT("and the tampered file was never written"),
					FPaths::FileExists(FPaths::Combine(TempDirectory, TEXT("BadHashOut"), TEXT("Readme.txt"))));

				Reader.Close();
			});

			It(TEXT("reports a header content hash that does not match the table of contents"), [this]()
			{
				FPackageFixtureOptions Options;
				Options.bBlankContentHash = true;

				const FString PackagePath = MakeFixturePath(TEXT("blankhash.mod"));
				WritePackageFixture(PackagePath, GetFixtureManifestJson(), MakeStandardEntries(), Options);

				TArray<FModDiagnostic> Diagnostics;
				FModPackageReader Reader;
				if (!TestTrue(TEXT("the package opens"), Reader.Open(PackagePath, Diagnostics)))
				{
					return;
				}

				// 40 zeroes mean "not computed", which is a mismatch rather than a free pass.
				ExpectText(TEXT("the header carries the not-computed sentinel"), Reader.GetHeader().ContentHash,
					TEXT("0000000000000000000000000000000000000000"));

				TArray<FModDiagnostic> VerifyDiagnostics;
				TestFalse(TEXT("VerifyIntegrity fails"), Reader.VerifyIntegrity(VerifyDiagnostics));
				ExpectDiagnosticCode(TEXT("verify"), VerifyDiagnostics, TEXT("Package.HashMismatch"));

				Reader.Close();
			});

			It(TEXT("refuses to read or extract from a reader that is not open"), [this]()
			{
				FModPackageReader Reader;

				TArray<uint8> Bytes;
				FModDiagnostic ReadError;
				TestFalse(TEXT("ReadEntry"), Reader.ReadEntry(TEXT("anything"), Bytes, ReadError));
				ExpectText(TEXT("read error code"), ReadError.Code.ToString(), TEXT("Package.OpenFailed"));

				TArray<FModDiagnostic> Diagnostics;
				TestFalse(TEXT("ExtractAll"), Reader.ExtractAll(FPaths::Combine(TempDirectory, TEXT("Nope")), Diagnostics));
				ExpectDiagnosticCode(TEXT("extract"), Diagnostics, TEXT("Package.OpenFailed"));

				TArray<FModDiagnostic> VerifyDiagnostics;
				TestFalse(TEXT("VerifyIntegrity"), Reader.VerifyIntegrity(VerifyDiagnostics));
				ExpectDiagnosticCode(TEXT("verify"), VerifyDiagnostics, TEXT("Package.OpenFailed"));
			});
		});

		Describe(TEXT("Content hash"), [this]()
		{
			// DOCUMENTED GUARANTEE: the digest covers each entry's PATH as well as its hash, so renaming
			// a file changes it - something hashing the entry hashes alone would not catch. Writers must
			// call ComputeContentHash rather than reimplement it, which is only safe while the digest
			// stays deterministic.
			It(TEXT("is deterministic, and changes when an entry is renamed or reordered"), [this]()
			{
				TArray<FModPackageEntry> First;
				FModPackageEntry A;
				A.RelativePath = TEXT("a.txt");
				A.Hash = ModSystemsTestsPrivate::HashBytesHex(MakePayload(TEXT("alpha")));
				First.Add(A);

				FModPackageEntry B;
				B.RelativePath = TEXT("b.txt");
				B.Hash = ModSystemsTestsPrivate::HashBytesHex(MakePayload(TEXT("beta")));
				First.Add(B);

				const FString Baseline = FModPackageReader::ComputeContentHash(First);
				TestEqual(TEXT("the digest is a 40 character hex string"), Baseline.Len(), ModPackage::HashHexLength);
				ExpectText(TEXT("computing it twice gives the same answer"),
					FModPackageReader::ComputeContentHash(First), Baseline);

				// Same files, different order: a different package, so a different digest.
				TArray<FModPackageEntry> Reordered;
				Reordered.Add(B);
				Reordered.Add(A);
				TestTrue(TEXT("reordering the entries changes the digest"),
					FModPackageReader::ComputeContentHash(Reordered) != Baseline);

				// Same bytes, different name.
				TArray<FModPackageEntry> Renamed = First;
				Renamed[1].RelativePath = TEXT("renamed.txt");
				TestTrue(TEXT("renaming an entry changes the digest"),
					FModPackageReader::ComputeContentHash(Renamed) != Baseline);

				// Same name, different bytes.
				TArray<FModPackageEntry> Retouched = First;
				Retouched[1].Hash = ModSystemsTestsPrivate::HashBytesHex(MakePayload(TEXT("gamma")));
				TestTrue(TEXT("changing an entry's contents changes the digest"),
					FModPackageReader::ComputeContentHash(Retouched) != Baseline);

				// An empty package still has a well-defined digest rather than an empty string.
				const FString EmptyDigest = FModPackageReader::ComputeContentHash(TArray<FModPackageEntry>());
				TestEqual(TEXT("an empty package digest is still 40 characters"), EmptyDigest.Len(), ModPackage::HashHexLength);
				TestTrue(TEXT("and differs from a package with entries"), EmptyDigest != Baseline);
			});

			It(TEXT("names the packaged file extension .mod"), [this]()
			{
				ExpectText(TEXT("package extension"), ModPackage::GetFileExtension(), TEXT(".mod"));
			});
		});
	});

	//~ -----------------------------------------------------------------------------------------------
	//~ Multiplayer
	//~ -----------------------------------------------------------------------------------------------

	Describe(TEXT("Net"), [this]()
	{
		BeforeEach([this]()
		{
			UModFrameworkSettings* Settings = GetMutableDefault<UModFrameworkSettings>();
			bSavedVerifyContentHashes = Settings->bVerifyContentHashes;
			Settings->bVerifyContentHashes = true;
		});

		AfterEach([this]()
		{
			GetMutableDefault<UModFrameworkSettings>()->bVerifyContentHashes = bSavedVerifyContentHashes;
		});

		Describe(TEXT("Encoding"), [this]()
		{
			It(TEXT("round-trips a session manifest through Base64 field for field"), [this]()
			{
				FModSessionManifest Source = MakeSessionManifest({
					MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true, EModNetworkScope::ClientAndServer,
						TEXT("ABCDEF0123456789ABCDEF0123456789ABCDEF01"), TEXT("Core Mod")),
					MakeNetEntry(TEXT("net.cosmetic"), TEXT("1.0.0-rc.1+build.7"), false, EModNetworkScope::ClientOnly,
						TEXT(""), TEXT("Cosmetic Pack")),
					MakeNetEntry(TEXT("net.admin"), TEXT("0.9.9"), true, EModNetworkScope::ServerOnly,
						TEXT(""), TEXT("Admin Tools")) });

				const FString Encoded = Source.ToBase64();
				TestFalse(TEXT("the encoding is not empty"), Encoded.IsEmpty());

				// base64url, so the result survives an Unreal travel URL and an HTTP query string.
				TestFalse(TEXT("no '+'"), Encoded.Contains(TEXT("+")));
				TestFalse(TEXT("no '/'"), Encoded.Contains(TEXT("/")));
				TestFalse(TEXT("no '?'"), Encoded.Contains(TEXT("?")));
				TestFalse(TEXT("no '#'"), Encoded.Contains(TEXT("#")));

				FModSessionManifest Decoded;
				if (!TestTrue(TEXT("the payload decodes"), FModSessionManifest::FromBase64(Encoded, Decoded)))
				{
					return;
				}

				TestEqual(TEXT("format version"), Decoded.FormatVersion, ModSessionManifest::CurrentFormatVersion);
				ExpectText(TEXT("game id"), Decoded.GameId, Source.GameId);
				ExpectText(TEXT("game version"), Decoded.GameVersion.ToString(), Source.GameVersion.ToString());
				ExpectText(TEXT("framework version"), Decoded.FrameworkVersion.ToString(), Source.FrameworkVersion.ToString());
				ExpectText(TEXT("sdk id"), Decoded.SdkId, Source.SdkId);
				ExpectText(TEXT("sdk version"), Decoded.SdkVersion.ToString(), Source.SdkVersion.ToString());

				if (TestEqual(TEXT("entry count"), Decoded.Entries.Num(), Source.Entries.Num()))
				{
					for (int32 Index = 0; Index < Source.Entries.Num(); ++Index)
					{
						const FModNetworkEntry& Expected = Source.Entries[Index];
						const FModNetworkEntry& Actual = Decoded.Entries[Index];
						const FString What = FString::Printf(TEXT("entries[%d]"), Index);

						ExpectText(*FString::Printf(TEXT("%s: id"), *What), Actual.ModId.ToString(), Expected.ModId.ToString());

						// ToString rather than operator==, because operator== ignores build metadata and
						// this has to prove the metadata survived the wire.
						ExpectText(*FString::Printf(TEXT("%s: version"), *What),
							Actual.Version.ToString(), Expected.Version.ToString());
						ExpectText(*FString::Printf(TEXT("%s: scope"), *What),
							ModFrameworkEnums::ToString(Actual.Scope), ModFrameworkEnums::ToString(Expected.Scope));
						TestEqual(*FString::Printf(TEXT("%s: required"), *What),
							Actual.bRequired ? 1 : 0, Expected.bRequired ? 1 : 0);
						ExpectText(*FString::Printf(TEXT("%s: content hash"), *What), Actual.ContentHash, Expected.ContentHash);
						ExpectText(*FString::Printf(TEXT("%s: display name"), *What), Actual.DisplayName, Expected.DisplayName);
					}
				}

				// FindEntry folds through FName, so case is irrelevant, and an absent id is a null.
				TestTrue(TEXT("FindEntry locates an entry"), Decoded.FindEntry(MakeTestId(TEXT("net.core"))) != nullptr);
				TestTrue(TEXT("FindEntry returns null for an absent id"),
					Decoded.FindEntry(MakeTestId(TEXT("net.absent"))) == nullptr);

				// An empty manifest still round-trips, which is what a vanilla server sends.
				FModSessionManifest Empty = MakeSessionManifest();
				FModSessionManifest DecodedEmpty;
				TestTrue(TEXT("an entry-less manifest round-trips"),
					FModSessionManifest::FromBase64(Empty.ToBase64(), DecodedEmpty));
				TestEqual(TEXT("and has no entries"), DecodedEmpty.Entries.Num(), 0);
				ExpectText(TEXT("and keeps its game id"), DecodedEmpty.GameId, TEXT("com.example.game"));
			});

			// The hand-built payloads below only prove anything if the builder itself produces something
			// the reader accepts. This is that control.
			It(TEXT("accepts a hand-built payload that follows the wire format"), [this]()
			{
				FModSessionManifest Decoded;
				TestTrue(TEXT("a well-formed hand-built payload decodes"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(FRawSessionOptions()), Decoded));

				TestEqual(TEXT("entry count"), Decoded.Entries.Num(), 1);
				ExpectText(TEXT("game id"), Decoded.GameId, TEXT("com.example.game"));
			});

			// ==========================================================================================
			// SECURITY TESTS. A session manifest arrives in a login option, which means it is written by
			// whoever is connecting. Every limit below is enforced BEFORE memory is allocated for the
			// data it describes. Do not relax any of them into a post-hoc check.
			// ==========================================================================================
			It(TEXT("refuses an empty, over-long or non-Base64 payload"), [this]()
			{
				FModSessionManifest Decoded;

				TestFalse(TEXT("an empty string"), FModSessionManifest::FromBase64(FString(), Decoded));
				TestFalse(TEXT("whitespace"), FModSessionManifest::FromBase64(TEXT("    "), Decoded));
				TestFalse(TEXT("characters outside both alphabets"),
					FModSessionManifest::FromBase64(TEXT("!!!!????"), Decoded));
				TestFalse(TEXT("plain prose"),
					FModSessionManifest::FromBase64(TEXT("this is not a session manifest"), Decoded));

				// Valid Base64 that decodes to bytes which are not a session manifest.
				const TArray<uint8> Junk = { 0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8 };
				TestFalse(TEXT("valid Base64 holding junk"),
					FModSessionManifest::FromBase64(FBase64::Encode(Junk, EBase64Mode::UrlSafe), Decoded));

				// Over the encoded-length cap, refused before the decode buffer is even sized.
				TestFalse(TEXT("a payload past the encoded length cap"),
					FModSessionManifest::FromBase64(
						FString::ChrN(ModSessionManifest::MaxEncodedLength + 1, TEXT('A')), Decoded));
			});

			It(TEXT("refuses a format version other than the current one"), [this]()
			{
				FModSessionManifest Decoded;

				FRawSessionOptions FromTheFuture;
				FromTheFuture.FormatVersion = ModSessionManifest::CurrentFormatVersion + 1;
				TestFalse(TEXT("a newer format version"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(FromTheFuture), Decoded));

				FRawSessionOptions Older;
				Older.FormatVersion = ModSessionManifest::CurrentFormatVersion - 1;
				TestFalse(TEXT("an older format version"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(Older), Decoded));

				FRawSessionOptions Nonsense;
				Nonsense.FormatVersion = MAX_int32;
				TestFalse(TEXT("a nonsensical format version"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(Nonsense), Decoded));
			});

			It(TEXT("refuses an entry count that is oversized, negative or larger than the payload"), [this]()
			{
				FModSessionManifest Decoded;

				// Past the documented cap. This is the allocation the cap exists to prevent.
				FRawSessionOptions Oversized;
				Oversized.DeclaredEntryCount = ModSessionManifest::MaxEntries + 1;
				Oversized.EntriesWritten = 0;
				TestFalse(TEXT("an entry count over the cap"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(Oversized), Decoded));

				FRawSessionOptions Enormous;
				Enormous.DeclaredEntryCount = MAX_int32;
				Enormous.EntriesWritten = 0;
				TestFalse(TEXT("an entry count of MAX_int32"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(Enormous), Decoded));

				FRawSessionOptions Negative;
				Negative.DeclaredEntryCount = -1;
				Negative.EntriesWritten = 0;
				TestFalse(TEXT("a negative entry count"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(Negative), Decoded));

				// Under the cap, but the remaining bytes cannot possibly hold that many entries - caught
				// from arithmetic before anything is reserved.
				FRawSessionOptions Impossible;
				Impossible.DeclaredEntryCount = 500;
				Impossible.EntriesWritten = 1;
				TestFalse(TEXT("an entry count the payload cannot hold"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(Impossible), Decoded));

				// Enough bytes left to satisfy the cheap arithmetic check, but they are not entries: the
				// per-entry read has to fail on its own rather than trusting the count.
				FRawSessionOptions Short;
				Short.DeclaredEntryCount = 3;
				Short.EntriesWritten = 1;
				Short.TrailingBytes = 128;
				TestFalse(TEXT("padding where the remaining entries should be"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(Short), Decoded));
			});

			It(TEXT("refuses trailing bytes, an over-long string and an out-of-range scope"), [this]()
			{
				FModSessionManifest Decoded;

				// Anything after the last entry means the sender and the reader disagree about the
				// layout, which is exactly when a lenient parser becomes a smuggling channel.
				FRawSessionOptions Trailing;
				Trailing.TrailingBytes = 8;
				TestFalse(TEXT("trailing bytes after the last entry"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(Trailing), Decoded));

				// A string length past the cap is refused before the buffer is sized from it.
				FRawSessionOptions LongString;
				LongString.GameIdLengthOverride = ModSessionManifest::MaxStringBytes + 1;
				TestFalse(TEXT("a string longer than the cap"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(LongString), Decoded));

				FRawSessionOptions NegativeString;
				NegativeString.GameIdLengthOverride = -1;
				TestFalse(TEXT("a negative string length"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(NegativeString), Decoded));

				// A length that fits the cap but not the remaining stream.
				FRawSessionOptions RunawayString;
				RunawayString.GameIdLengthOverride = ModSessionManifest::MaxStringBytes;
				TestFalse(TEXT("a string length past the end of the stream"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(RunawayString), Decoded));

				// EModNetworkScope has three values; anything else is not a scope.
				FRawSessionOptions BadScope;
				BadScope.ScopeByteOverride = static_cast<uint8>(200);
				TestFalse(TEXT("a scope byte outside the enum"),
					FModSessionManifest::FromBase64(EncodeRawSessionManifest(BadScope), Decoded));
			});

			// Documented: Out is reset before anything else happens, so a caller that ignores the return
			// value can never observe half-parsed data from a hostile payload.
			It(TEXT("resets the output before it parses, whatever the payload turns out to be"), [this]()
			{
				FModSessionManifest Decoded = MakeSessionManifest({
					MakeNetEntry(TEXT("net.stale"), TEXT("9.9.9"), true) });

				TestFalse(TEXT("the payload is refused"), FModSessionManifest::FromBase64(TEXT("!!!!"), Decoded));

				TestEqual(TEXT("the stale entries are gone"), Decoded.Entries.Num(), 0);
				TestTrue(TEXT("the stale game id is gone"), Decoded.GameId.IsEmpty());
				TestTrue(TEXT("the stale sdk id is gone"), Decoded.SdkId.IsEmpty());
				TestTrue(TEXT("the stale game version is gone"), Decoded.GameVersion.IsZero());
			});
		});

		Describe(TEXT("Digest"), [this]()
		{
			// DOCUMENTED GUARANTEE: two installs that would pass ValidateClient always produce the same
			// digest, which is what makes it usable as a server-browser filter. That means it must be
			// stable, independent of entry order, and must ignore exactly the entries validation
			// ignores - the optional ones and the client-only ones.
			It(TEXT("is stable and independent of entry order"), [this]()
			{
				const FModNetworkEntry Core = MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true);
				const FModNetworkEntry Combat = MakeNetEntry(TEXT("net.combat"), TEXT("1.0.0"), true);
				const FModNetworkEntry Ui = MakeNetEntry(TEXT("net.ui"), TEXT("3.0.0"), true);

				const FString Forwards = MakeSessionManifest({ Core, Combat, Ui }).ComputeDigest();
				const FString Backwards = MakeSessionManifest({ Ui, Combat, Core }).ComputeDigest();
				const FString Shuffled = MakeSessionManifest({ Combat, Ui, Core }).ComputeDigest();

				TestEqual(TEXT("the digest is a 40 character hex string"), Forwards.Len(), 40);
				ExpectText(TEXT("reversing the entry order"), Backwards, Forwards);
				ExpectText(TEXT("shuffling the entry order"), Shuffled, Forwards);

				// Computing it twice on the same manifest gives the same answer.
				ExpectText(TEXT("recomputing"), MakeSessionManifest({ Core, Combat, Ui }).ComputeDigest(), Forwards);

				// A different version is a different install.
				TestTrue(TEXT("a version change changes the digest"),
					MakeSessionManifest({ Core, Combat, MakeNetEntry(TEXT("net.ui"), TEXT("3.0.1"), true) }).ComputeDigest() != Forwards);

				// So is a mod that is not there.
				TestTrue(TEXT("a missing mod changes the digest"),
					MakeSessionManifest({ Core, Combat }).ComputeDigest() != Forwards);
			});

			It(TEXT("ignores exactly the entries that cannot cause a mismatch"), [this]()
			{
				const FModNetworkEntry Core = MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true);
				const FString Baseline = MakeSessionManifest({ Core }).ComputeDigest();

				// A client-only mod never affects whether a join succeeds, so it must never affect the
				// digest either - otherwise two compatible players could never match on it.
				ExpectText(TEXT("a required client-only mod is ignored"),
					MakeSessionManifest({ Core,
						MakeNetEntry(TEXT("net.cosmetic"), TEXT("1.0.0"), true, EModNetworkScope::ClientOnly) }).ComputeDigest(),
					Baseline);

				// An optional mod is the owner's own business.
				ExpectText(TEXT("an optional mod is ignored"),
					MakeSessionManifest({ Core,
						MakeNetEntry(TEXT("net.optional"), TEXT("1.0.0"), false) }).ComputeDigest(),
					Baseline);

				// A required server-only mod does take part: both sides have to agree it is there.
				TestTrue(TEXT("a required server-only mod is counted"),
					MakeSessionManifest({ Core,
						MakeNetEntry(TEXT("net.admin"), TEXT("1.0.0"), true, EModNetworkScope::ServerOnly) }).ComputeDigest() != Baseline);

				// The content hash and the display name are cosmetic to the digest.
				ExpectText(TEXT("the content hash and display name do not contribute"),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT("DEADBEEF"), TEXT("Renamed")) }).ComputeDigest(),
					Baseline);
			});
		});

		Describe(TEXT("Validation"), [this]()
		{
			It(TEXT("accepts a client whose mods match the server"), [this]()
			{
				const TArray<FModNetworkEntry> Entries = {
					MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true),
					MakeNetEntry(TEXT("net.optional"), TEXT("1.0.0"), false) };

				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(
					MakeSessionManifest(Entries), MakeSessionManifest(Entries));

				TestTrue(TEXT("compatible"), Result.bCompatible);
				TestEqual(TEXT("no mismatches"), Result.Mismatches.Num(), 0);
				ExpectText(TEXT("debug string"), Result.ToDebugString(), TEXT("Compatible."));
				TestFalse(TEXT("the player-facing message is not empty"),
					Result.BuildUserFacingMessage().IsEmpty());
			});

			It(TEXT("rejects a client missing a mod the server requires"), [this]()
			{
				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT(""), TEXT("Core Mod")) }),
					MakeSessionManifest());

				TestFalse(TEXT("incompatible"), Result.bCompatible);
				TestEqual(TEXT("exactly one mismatch"), Result.Mismatches.Num(), 1);

				const FModNetworkMismatch* Mismatch = ExpectMismatch(TEXT("missing on client"), Result,
					TEXT("net.core"), EModNetworkMismatchType::MissingOnClient);
				if (Mismatch != nullptr)
				{
					// Expected is always what the server has and Actual always what the client has, so a
					// message can be phrased the same way whichever direction the check ran in.
					ExpectText(TEXT("expected version"), Mismatch->Expected.ToString(), TEXT("2.1.0"));
					TestTrue(TEXT("actual version is zero, meaning absent"), Mismatch->Actual.IsZero());
					ExpectText(TEXT("display name"), Mismatch->DisplayName, TEXT("Core Mod"));
					ExpectContains(TEXT("message"), Mismatch->Message, TEXT("net.core"));
				}

				ExpectContains(TEXT("debug string"), Result.ToDebugString(), TEXT("[MissingOnClient]"));
			});

			It(TEXT("rejects a server missing a mod the client requires for network play"), [this]()
			{
				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(
					MakeSessionManifest(),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.extra"), TEXT("1.2.0"), true) }));

				TestFalse(TEXT("incompatible"), Result.bCompatible);
				TestEqual(TEXT("exactly one mismatch"), Result.Mismatches.Num(), 1);

				const FModNetworkMismatch* Mismatch = ExpectMismatch(TEXT("missing on server"), Result,
					TEXT("net.extra"), EModNetworkMismatchType::MissingOnServer);
				if (Mismatch != nullptr)
				{
					TestTrue(TEXT("expected version is zero, meaning the server does not have it"),
						Mismatch->Expected.IsZero());
					ExpectText(TEXT("actual version"), Mismatch->Actual.ToString(), TEXT("1.2.0"));
				}
			});

			It(TEXT("rejects a required mod present on both sides at different versions"), [this]()
			{
				// Exact equality, not a range: a required networked mod has to be the same build on both
				// sides or replicated state can diverge in ways no range expression can describe.
				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.1"), true) }));

				TestFalse(TEXT("incompatible"), Result.bCompatible);
				TestEqual(TEXT("exactly one mismatch"), Result.Mismatches.Num(), 1);

				const FModNetworkMismatch* Mismatch = ExpectMismatch(TEXT("version mismatch"), Result,
					TEXT("net.core"), EModNetworkMismatchType::VersionMismatch);
				if (Mismatch != nullptr)
				{
					ExpectText(TEXT("server version"), Mismatch->Expected.ToString(), TEXT("2.1.0"));
					ExpectText(TEXT("client version"), Mismatch->Actual.ToString(), TEXT("2.1.1"));

					// Both versions have to be in the sentence: a player cannot act on "incompatible".
					ExpectContains(TEXT("message"), Mismatch->Message, TEXT("2.1.0"));
					ExpectContains(TEXT("message"), Mismatch->Message, TEXT("2.1.1"));
				}

				// Build metadata is not part of a version's identity, so it cannot fail a join.
				const FModNetworkValidationResult Metadata = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0+alpha"), true) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0+beta"), true) }));
				TestTrue(TEXT("build metadata alone does not cause a mismatch"), Metadata.bCompatible);
			});

			It(TEXT("rejects matching versions whose content hashes disagree, and only when both are known"), [this]()
			{
				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT("AAAA1111")) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT("BBBB2222")) }));

				TestFalse(TEXT("incompatible"), Result.bCompatible);
				ExpectMismatch(TEXT("content hash"), Result, TEXT("net.core"), EModNetworkMismatchType::ContentHashMismatch);

				// An absent hash on either side means "unknown", never "different".
				const FModNetworkValidationResult ServerOnlyHash = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT("AAAA1111")) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true) }));
				TestTrue(TEXT("an unknown client hash is not a mismatch"), ServerOnlyHash.bCompatible);

				const FModNetworkValidationResult ClientOnlyHash = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT("BBBB2222")) }));
				TestTrue(TEXT("an unknown server hash is not a mismatch"), ClientOnlyHash.bCompatible);

				// Hashes are compared case-insensitively; the same digest in another case is the same mod.
				const FModNetworkValidationResult SameCaseFolded = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT("abcd1234")) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT("ABCD1234")) }));
				TestTrue(TEXT("case does not make two identical hashes differ"), SameCaseFolded.bCompatible);

				// The check is opt-out through project settings.
				GetMutableDefault<UModFrameworkSettings>()->bVerifyContentHashes = false;
				const FModNetworkValidationResult Disabled = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT("AAAA1111")) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true,
						EModNetworkScope::ClientAndServer, TEXT("BBBB2222")) }));
				TestTrue(TEXT("disabling hash verification silences the mismatch"), Disabled.bCompatible);
			});

			// A server-only mod running on a client is either a misconfigured install or an attempt to
			// run authoritative logic client side. It is refused regardless of anything else about it.
			It(TEXT("treats a server-only mod running on a client as a scope violation"), [this]()
			{
				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(
					MakeSessionManifest(),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.admin"), TEXT("1.0.0"), false,
						EModNetworkScope::ServerOnly, TEXT(""), TEXT("Admin Tools")) }));

				TestFalse(TEXT("incompatible"), Result.bCompatible);
				TestEqual(TEXT("exactly one mismatch"), Result.Mismatches.Num(), 1);

				const FModNetworkMismatch* Mismatch = ExpectMismatch(TEXT("scope violation"), Result,
					TEXT("net.admin"), EModNetworkMismatchType::ScopeViolation);
				if (Mismatch != nullptr)
				{
					// Optional or not, and whether or not the server runs it, it may not be on a client.
					ExpectText(TEXT("the client's version is reported"), Mismatch->Actual.ToString(), TEXT("1.0.0"));
					ExpectContains(TEXT("message"), Mismatch->Message, TEXT("server-only"));
				}

				// Even when the server runs the same mod, it is still a violation on the client.
				const FModNetworkValidationResult BothSides = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.admin"), TEXT("1.0.0"), true, EModNetworkScope::ServerOnly) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.admin"), TEXT("1.0.0"), true, EModNetworkScope::ServerOnly) }));
				TestFalse(TEXT("still incompatible when the server runs it too"), BothSides.bCompatible);
				ExpectMismatch(TEXT("scope violation with the server running it"), BothSides,
					TEXT("net.admin"), EModNetworkMismatchType::ScopeViolation);
			});

			// DOCUMENTED GUARANTEE: client-only mods are what makes cosmetic modding possible. The server
			// never needs to know about them, so they can never cause a mismatch - in either direction.
			It(TEXT("never lets a client-only mod cause a mismatch"), [this]()
			{
				const FModSessionManifest Server = MakeSessionManifest({
					MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true) });

				const FModSessionManifest Client = MakeSessionManifest({
					MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true),
					// Required, client-only, absent from the server, and with a content hash nobody can
					// match: still not the server's business.
					MakeNetEntry(TEXT("net.reshade"), TEXT("4.0.0"), true, EModNetworkScope::ClientOnly, TEXT("FFFF9999")),
					MakeNetEntry(TEXT("net.hud"), TEXT("1.0.0"), false, EModNetworkScope::ClientOnly) });

				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(Server, Client);

				TestTrue(TEXT("compatible"), Result.bCompatible);
				TestEqual(TEXT("no mismatches"), Result.Mismatches.Num(), 0);

				// The asymmetry here is deliberate and documented on FModNetworkValidator: the exemption
				// is for a CLIENT's client-only mods, which the server never needs to know about. It is
				// NOT an exemption for a client-only mod the SERVER lists as required - that is how a
				// server says "you must be running this client-side mod to play here", so it is still
				// demanded of the client.
				const FModNetworkValidationResult ServerRequiresClientMod = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.requiredhud"), TEXT("1.0.0"), true,
						EModNetworkScope::ClientOnly) }),
					MakeSessionManifest());
				TestFalse(TEXT("a required client-only server mod is still demanded of the client"),
					ServerRequiresClientMod.bCompatible);
				ExpectMismatch(TEXT("server-required client-only mod"), ServerRequiresClientMod,
					TEXT("net.requiredhud"), EModNetworkMismatchType::MissingOnClient);

				// An optional one, on the other hand, is nobody's problem.
				const FModNetworkValidationResult OptionalClientMod = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.optionalhud"), TEXT("1.0.0"), false,
						EModNetworkScope::ClientOnly) }),
					MakeSessionManifest());
				TestTrue(TEXT("an optional client-only server mod is ignored"), OptionalClientMod.bCompatible);
			});

			It(TEXT("does not demand the server's own server-only mods of a client"), [this]()
			{
				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({
						MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true),
						MakeNetEntry(TEXT("net.anticheat"), TEXT("1.0.0"), true, EModNetworkScope::ServerOnly) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true) }));

				TestTrue(TEXT("compatible"), Result.bCompatible);
				TestEqual(TEXT("no mismatches"), Result.Mismatches.Num(), 0);
			});

			It(TEXT("ignores an optional mod on either side"), [this]()
			{
				const FModNetworkValidationResult ServerSide = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.optional"), TEXT("1.0.0"), false) }),
					MakeSessionManifest());
				TestTrue(TEXT("an optional server mod is not required of the client"), ServerSide.bCompatible);

				const FModNetworkValidationResult ClientSide = FModNetworkValidator::ValidateClient(
					MakeSessionManifest(),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.optional"), TEXT("1.0.0"), false) }));
				TestTrue(TEXT("an optional client mod is not required of the server"), ClientSide.bCompatible);

				// An optional mod at a different version is still nobody's problem.
				const FModNetworkValidationResult Versions = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.optional"), TEXT("1.0.0"), false) }),
					MakeSessionManifest({ MakeNetEntry(TEXT("net.optional"), TEXT("2.0.0"), false) }));
				TestTrue(TEXT("an optional version difference is ignored"), Versions.bCompatible);
			});

			It(TEXT("stops at a different game id without listing mod differences on top of it"), [this]()
			{
				FModSessionManifest Client = MakeSessionManifest({ MakeNetEntry(TEXT("net.extra"), TEXT("1.0.0"), true) });
				Client.GameId = TEXT("com.example.othergame");
				Client.FrameworkVersion = FModVersion(99, 0, 0);
				Client.SdkId = TEXT("com.example.othersdk");

				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true) }), Client);

				TestFalse(TEXT("incompatible"), Result.bCompatible);

				// A different game is not a mod problem. Reporting the framework, SDK and mod differences
				// on top would only bury the one thing the player needs to read.
				TestEqual(TEXT("exactly one mismatch, despite four things differing"), Result.Mismatches.Num(), 1);
				if (Result.Mismatches.Num() == 1)
				{
					ExpectText(TEXT("mismatch type"), MismatchTypeToString(Result.Mismatches[0].Type), TEXT("GameMismatch"));
					TestFalse(TEXT("and it is not attributed to a mod"), Result.Mismatches[0].ModId.IsValid());
					ExpectContains(TEXT("message"), Result.Mismatches[0].Message, TEXT("com.example.othergame"));
				}

				// The game id is matched case-insensitively.
				FModSessionManifest CaseFolded = MakeSessionManifest();
				CaseFolded.GameId = TEXT("COM.EXAMPLE.GAME");
				TestTrue(TEXT("case does not make two identical game ids differ"),
					FModNetworkValidator::ValidateClient(MakeSessionManifest(), CaseFolded).bCompatible);
			});

			It(TEXT("rejects a different major game, framework or SDK version but tolerates a minor one"), [this]()
			{
				{
					FModSessionManifest Client = MakeSessionManifest();
					Client.GameVersion = FModVersion(2, 5, 0);

					const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(MakeSessionManifest(), Client);
					TestFalse(TEXT("a different major game version is refused"), Result.bCompatible);
					ExpectContains(TEXT("game mismatch"), Result.ToDebugString(), TEXT("[GameMismatch]"));
				}

				{
					FModSessionManifest Client = MakeSessionManifest();
					Client.GameVersion = FModVersion(1, 9, 3);
					TestTrue(TEXT("a different minor game version is tolerated"),
						FModNetworkValidator::ValidateClient(MakeSessionManifest(), Client).bCompatible);
				}

				{
					FModSessionManifest Client = MakeSessionManifest();
					Client.FrameworkVersion = FModVersion(99, 0, 0);

					const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(MakeSessionManifest(), Client);
					TestFalse(TEXT("a different major framework version is refused"), Result.bCompatible);
					ExpectContains(TEXT("framework mismatch"), Result.ToDebugString(), TEXT("[FrameworkMismatch]"));
				}

				{
					FModSessionManifest Client = MakeSessionManifest();
					Client.SdkVersion = FModVersion(2, 5, 0);

					const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(MakeSessionManifest(), Client);
					TestFalse(TEXT("a different major SDK version is refused"), Result.bCompatible);
					ExpectContains(TEXT("sdk mismatch"), Result.ToDebugString(), TEXT("[SdkMismatch]"));
				}

				{
					// A different SDK id makes the SDK versions incomparable, so only one of the two is
					// reported rather than both.
					FModSessionManifest Client = MakeSessionManifest();
					Client.SdkId = TEXT("com.example.othersdk");
					Client.SdkVersion = FModVersion(9, 0, 0);

					const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(MakeSessionManifest(), Client);
					TestFalse(TEXT("a different SDK id is refused"), Result.bCompatible);
					TestEqual(TEXT("and reported once, not twice"), Result.Mismatches.Num(), 1);
					ExpectContains(TEXT("sdk id mismatch"), Result.ToDebugString(), TEXT("com.example.othersdk"));
				}
			});

			It(TEXT("reports every mismatch when several are present at once"), [this]()
			{
				const FModSessionManifest Server = MakeSessionManifest({
					MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true),
					MakeNetEntry(TEXT("net.balance"), TEXT("1.0.0"), true) });

				const FModSessionManifest Client = MakeSessionManifest({
					MakeNetEntry(TEXT("net.core"), TEXT("2.0.0"), true),
					MakeNetEntry(TEXT("net.admin"), TEXT("1.0.0"), true, EModNetworkScope::ServerOnly),
					MakeNetEntry(TEXT("net.extra"), TEXT("1.0.0"), true) });

				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(Server, Client);

				TestFalse(TEXT("incompatible"), Result.bCompatible);
				TestEqual(TEXT("four mismatches"), Result.Mismatches.Num(), 4);

				// Server-side problems are reported before client-side ones, so the list reads in the
				// order the checks run.
				ExpectMismatch(TEXT("version"), Result, TEXT("net.core"), EModNetworkMismatchType::VersionMismatch);
				ExpectMismatch(TEXT("missing on client"), Result, TEXT("net.balance"), EModNetworkMismatchType::MissingOnClient);
				ExpectMismatch(TEXT("scope"), Result, TEXT("net.admin"), EModNetworkMismatchType::ScopeViolation);
				ExpectMismatch(TEXT("missing on server"), Result, TEXT("net.extra"), EModNetworkMismatchType::MissingOnServer);

				ExpectText(TEXT("mismatch order"),
					FString::Printf(TEXT("%s, %s, %s, %s"),
						MismatchTypeToString(Result.Mismatches[0].Type),
						MismatchTypeToString(Result.Mismatches[1].Type),
						MismatchTypeToString(Result.Mismatches[2].Type),
						MismatchTypeToString(Result.Mismatches[3].Type)),
					TEXT("VersionMismatch, MissingOnClient, ScopeViolation, MissingOnServer"));
			});

			// DOCUMENTED GUARANTEE: ValidateServer is implemented as ValidateClient with the arguments
			// swapped, so a client's pre-flight prediction and the server's actual verdict are the same
			// computation and cannot drift apart. If this ever fails, the two have been given separate
			// implementations - put them back together rather than adjusting the test.
			It(TEXT("gives a client's pre-flight check the identical answer the server will reach"), [this]()
			{
				const FModSessionManifest Server = MakeSessionManifest({
					MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true),
					MakeNetEntry(TEXT("net.balance"), TEXT("1.0.0"), true) });

				const FModSessionManifest Client = MakeSessionManifest({
					MakeNetEntry(TEXT("net.core"), TEXT("2.0.0"), true),
					MakeNetEntry(TEXT("net.admin"), TEXT("1.0.0"), true, EModNetworkScope::ServerOnly) });

				ExpectText(TEXT("the two directions agree"),
					FingerprintMismatches(FModNetworkValidator::ValidateServer(Client, Server)),
					FingerprintMismatches(FModNetworkValidator::ValidateClient(Server, Client)));

				// And they agree on the compatible case too.
				ExpectText(TEXT("the two directions agree when everything matches"),
					FingerprintMismatches(FModNetworkValidator::ValidateServer(Server, Server)),
					FingerprintMismatches(FModNetworkValidator::ValidateClient(Server, Server)));
			});

			It(TEXT("survives a manifest that repeats a mod id, using the first occurrence"), [this]()
			{
				// A hostile payload can repeat an id. The first occurrence wins, matching FindEntry, so
				// which entry gets checked never depends on the lookup path.
				const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(
					MakeSessionManifest({ MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true) }),
					MakeSessionManifest({
						MakeNetEntry(TEXT("net.core"), TEXT("2.1.0"), true),
						MakeNetEntry(TEXT("net.core"), TEXT("9.9.9"), true) }));

				TestTrue(TEXT("the first client entry is the one that counts"), Result.bCompatible);
			});
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
