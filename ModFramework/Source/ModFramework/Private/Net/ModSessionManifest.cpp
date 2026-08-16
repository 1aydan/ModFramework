// Copyright (c) 2026. Licensed for use in your own projects.

#include "Net/ModSessionManifest.h"

#include "Containers/Array.h"
#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModVersion.h"
#include "Misc/Base64.h"
#include "Misc/CString.h"
#include "Misc/SecureHash.h"
#include "Serialization/Archive.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Templates/UnrealTemplate.h"

/**
 * Wire format, version 1. Everything is little-endian, which is the only byte order the engine
 * supports (Core asserts on it when serializing strings).
 *
 *   Manifest := int32 FormatVersion
 *               String GameId
 *               Version GameVersion
 *               Version FrameworkVersion
 *               String SdkId
 *               Version SdkVersion
 *               int32 EntryCount
 *               Entry * EntryCount
 *
 *   Entry    := String ModId
 *               Version Version
 *               uint8 Scope
 *               uint8 bRequired
 *               String ContentHash
 *               String DisplayName
 *
 *   Version  := int32 Major, int32 Minor, int32 Patch, String PreRelease, String BuildMetadata
 *   String   := int32 ByteCount, ByteCount bytes of UTF-8 (no terminator)
 *
 * The engine's own FString archive operator is deliberately not used: it allocates the declared
 * character count before reading it, so a twenty byte payload claiming a two billion character
 * string would allocate gigabytes. The bounded reader below refuses anything longer than
 * ModSessionManifest::MaxStringBytes and anything that does not fit in the remaining stream.
 */
namespace ModSessionManifestPrivate
{
	/**
	 * Appends the UTF-8 encoding of In to Out. Never writes a terminator.
	 */
	static void AppendUtf8(const FString& In, TArray<uint8>& Out)
	{
		const auto Converted = StringCast<UTF8CHAR>(*In);
		if (Converted.Length() > 0)
		{
			Out.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
		}
	}

	/**
	 * Writes a length prefixed UTF-8 string.
	 *
	 * A string whose encoding exceeds the cap is clamped rather than dropped or allowed through: the
	 * only strings that can get there are author supplied display names, and losing the tail of one
	 * is far better than producing a payload the other side has to reject. The clamp happens on a
	 * TCHAR boundary (UTF-8 needs at most three bytes per UTF-16 code unit) so the result is never a
	 * truncated multi-byte sequence.
	 */
	static void SaveBoundedString(FArchive& Ar, const FString& Value)
	{
		TArray<uint8> Utf8;
		AppendUtf8(Value, Utf8);

		if (Utf8.Num() > ModSessionManifest::MaxStringBytes)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("Session manifest string of %d bytes exceeds the %d byte limit and was clamped."),
				Utf8.Num(), ModSessionManifest::MaxStringBytes);

			Utf8.Reset();
			AppendUtf8(Value.Left(ModSessionManifest::MaxStringBytes / 3), Utf8);
		}

		int32 ByteCount = Utf8.Num();
		Ar << ByteCount;

		if (ByteCount > 0)
		{
			Ar.Serialize(Utf8.GetData(), ByteCount);
		}
	}

	/**
	 * Reads a length prefixed UTF-8 string, bounding the allocation before it happens.
	 * On any problem the archive is marked as errored and Value is emptied.
	 */
	static void LoadBoundedString(FArchive& Ar, FString& Value)
	{
		Value.Reset();

		int32 ByteCount = 0;
		Ar << ByteCount;
		if (Ar.IsError())
		{
			return;
		}

		if (ByteCount < 0 || ByteCount > ModSessionManifest::MaxStringBytes)
		{
			Ar.SetError();
			return;
		}

		const int64 Remaining = Ar.TotalSize() - Ar.Tell();
		if (static_cast<int64>(ByteCount) > Remaining)
		{
			Ar.SetError();
			return;
		}

		if (ByteCount == 0)
		{
			return;
		}

		TArray<UTF8CHAR> Utf8;
		Utf8.SetNumUninitialized(ByteCount);
		Ar.Serialize(Utf8.GetData(), ByteCount);
		if (Ar.IsError())
		{
			return;
		}

		const auto Converted = StringCast<TCHAR>(Utf8.GetData(), ByteCount);
		Value = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
	}

	static void SerializeBoundedString(FArchive& Ar, FString& Value)
	{
		if (Ar.IsLoading())
		{
			LoadBoundedString(Ar, Value);
		}
		else
		{
			SaveBoundedString(Ar, Value);
		}
	}

	/**
	 * Serializes an FModVersion field by field. FModVersion has no archive operator of its own and
	 * must not get one here: this layout belongs to the session manifest format, not to the type.
	 */
	static void SerializeVersion(FArchive& Ar, FModVersion& Version)
	{
		Ar << Version.Major;
		Ar << Version.Minor;
		Ar << Version.Patch;
		SerializeBoundedString(Ar, Version.PreRelease);
		SerializeBoundedString(Ar, Version.BuildMetadata);

		if (Ar.IsLoading())
		{
			// A negative component is meaningless but harmless; normalise instead of rejecting, so a
			// merely sloppy sender is not treated as a hostile one.
			Version.Major = Version.Major < 0 ? 0 : Version.Major;
			Version.Minor = Version.Minor < 0 ? 0 : Version.Minor;
			Version.Patch = Version.Patch < 0 ? 0 : Version.Patch;
		}
	}

	/** Highest valid EModNetworkScope value, used to reject out of range bytes from the network. */
	static constexpr uint8 MaxNetworkScopeValue = static_cast<uint8>(EModNetworkScope::ServerOnly);
}

//////////////////////////////////////////////////////////////////////////
// FModNetworkEntry

FArchive& operator<<(FArchive& Ar, FModNetworkEntry& E)
{
	using namespace ModSessionManifestPrivate;

	// The id travels as text: FArchive's FName operator is a no-op on the base class and depends on
	// a name table that a raw memory archive does not have.
	FString IdText;
	if (!Ar.IsLoading() && E.ModId.IsValid())
	{
		// An invalid id travels as an empty string rather than as "None", so it round-trips back to an
		// invalid id instead of to a mod literally called "none".
		IdText = E.ModId.ToString();
	}
	SerializeBoundedString(Ar, IdText);
	if (Ar.IsLoading())
	{
		if (Ar.IsError())
		{
			return Ar;
		}
		// FromString lowercases without validating. The id is only ever used to match entries against
		// each other, so a malformed id simply fails to match rather than rejecting the whole payload.
		E.ModId = FModId::FromString(IdText);
	}

	SerializeVersion(Ar, E.Version);
	if (Ar.IsError())
	{
		return Ar;
	}

	uint8 ScopeByte = static_cast<uint8>(E.Scope);
	Ar << ScopeByte;
	if (Ar.IsLoading())
	{
		if (Ar.IsError())
		{
			return Ar;
		}
		if (ScopeByte > MaxNetworkScopeValue)
		{
			Ar.SetError();
			return Ar;
		}
		E.Scope = static_cast<EModNetworkScope>(ScopeByte);
	}

	uint8 RequiredByte = E.bRequired ? 1 : 0;
	Ar << RequiredByte;
	if (Ar.IsLoading())
	{
		if (Ar.IsError())
		{
			return Ar;
		}
		E.bRequired = RequiredByte != 0;
	}

	SerializeBoundedString(Ar, E.ContentHash);
	SerializeBoundedString(Ar, E.DisplayName);

	return Ar;
}

//////////////////////////////////////////////////////////////////////////
// FModSessionManifest

FArchive& operator<<(FArchive& Ar, FModSessionManifest& M)
{
	using namespace ModSessionManifestPrivate;

	// Saving always stamps the current format version, whatever the field happens to hold, so a
	// caller cannot accidentally produce a payload that FromBase64 will refuse.
	int32 Version = Ar.IsLoading() ? 0 : ModSessionManifest::CurrentFormatVersion;
	Ar << Version;
	if (Ar.IsLoading())
	{
		if (Ar.IsError() || Version != ModSessionManifest::CurrentFormatVersion)
		{
			Ar.SetError();
			return Ar;
		}
		M.FormatVersion = Version;
	}

	SerializeBoundedString(Ar, M.GameId);
	SerializeVersion(Ar, M.GameVersion);
	SerializeVersion(Ar, M.FrameworkVersion);
	SerializeBoundedString(Ar, M.SdkId);
	SerializeVersion(Ar, M.SdkVersion);
	if (Ar.IsError())
	{
		return Ar;
	}

	if (Ar.IsLoading())
	{
		M.Entries.Reset();

		int32 EntryCount = 0;
		Ar << EntryCount;
		if (Ar.IsError())
		{
			return Ar;
		}

		if (EntryCount < 0 || EntryCount > ModSessionManifest::MaxEntries)
		{
			Ar.SetError();
			return Ar;
		}

		// Refuse a count the remaining bytes cannot possibly satisfy, before reserving for it.
		const int64 Remaining = Ar.TotalSize() - Ar.Tell();
		if (static_cast<int64>(EntryCount) * ModSessionManifest::MinBytesPerEntry > Remaining)
		{
			Ar.SetError();
			return Ar;
		}

		M.Entries.Reserve(EntryCount);
		for (int32 Index = 0; Index < EntryCount; ++Index)
		{
			FModNetworkEntry& Entry = M.Entries.AddDefaulted_GetRef();
			Ar << Entry;
			if (Ar.IsError())
			{
				M.Entries.Reset();
				return Ar;
			}
		}
	}
	else
	{
		int32 EntryCount = M.Entries.Num();
		if (EntryCount > ModSessionManifest::MaxEntries)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("Session manifest holds %d entries; only the first %d are encoded."),
				EntryCount, ModSessionManifest::MaxEntries);
			EntryCount = ModSessionManifest::MaxEntries;
		}

		Ar << EntryCount;
		for (int32 Index = 0; Index < EntryCount; ++Index)
		{
			Ar << M.Entries[Index];
		}
	}

	return Ar;
}

const FModNetworkEntry* FModSessionManifest::FindEntry(const FModId& ModId) const
{
	for (const FModNetworkEntry& Entry : Entries)
	{
		if (Entry.ModId == ModId)
		{
			return &Entry;
		}
	}
	return nullptr;
}

FString FModSessionManifest::ComputeDigest() const
{
	// Only the mods both sides have to agree on take part: a client-only mod never affects whether a
	// join succeeds, so it must never affect the digest either.
	TArray<FString> Tokens;
	Tokens.Reserve(Entries.Num());

	for (const FModNetworkEntry& Entry : Entries)
	{
		if (!Entry.bRequired || Entry.Scope == EModNetworkScope::ClientOnly)
		{
			continue;
		}
		Tokens.Add(FString::Printf(TEXT("%s@%s|"), *Entry.ModId.ToString(), *Entry.Version.ToString()));
	}

	// Sorting the rendered tokens orders by mod id (the '@' separator sorts below every character an
	// id may contain) and breaks ties on version, so duplicate ids cannot make the digest unstable.
	Tokens.Sort([](const FString& A, const FString& B)
	{
		return FCString::Strcmp(*A, *B) < 0;
	});

	int32 TotalLength = 0;
	for (const FString& Token : Tokens)
	{
		TotalLength += Token.Len();
	}

	FString Combined;
	Combined.Reserve(TotalLength);
	for (const FString& Token : Tokens)
	{
		Combined += Token;
	}

	// Hash the UTF-8 bytes so the digest does not depend on the platform's TCHAR width.
	TArray<uint8> Utf8;
	Utf8.Reserve(TotalLength);
	ModSessionManifestPrivate::AppendUtf8(Combined, Utf8);

	const FSHAHash Hash = FSHA1::HashBuffer(Utf8.GetData(), static_cast<uint64>(Utf8.Num()));
	return Hash.ToString();
}

FString FModSessionManifest::ToBase64() const
{
	TArray<uint8> Bytes;
	{
		FMemoryWriter Writer(Bytes, /*bIsPersistent=*/true);

		// The archive operator is bidirectional and therefore takes a mutable reference; the saving
		// path only ever reads from the struct, which is what makes the cast safe.
		FModSessionManifest& Mutable = const_cast<FModSessionManifest&>(*this);
		Writer << Mutable;
	}

	if (Bytes.Num() == 0)
	{
		return FString();
	}

	// base64url: no '+' and no '/', and never a '?' or '#', so the result is safe both in an Unreal
	// travel URL and in an HTTP query string a matchmaking service might route it through.
	return FBase64::Encode(Bytes, EBase64Mode::UrlSafe);
}

bool FModSessionManifest::FromBase64(const FString& In, FModSessionManifest& Out)
{
	// Reset first: a caller that ignores the return value must never see stale or partial data.
	Out = FModSessionManifest();

	if (In.IsEmpty())
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Session manifest decode failed: the encoded manifest is empty."));
		return false;
	}

	if (In.Len() > ModSessionManifest::MaxEncodedLength)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Session manifest decode failed: %d encoded characters exceeds the %d character limit."),
			In.Len(), ModSessionManifest::MaxEncodedLength);
		return false;
	}

	// ToBase64 emits base64url; the standard alphabet is accepted as well so a manifest produced by
	// other tooling still round-trips. The two alphabets only differ in two characters, so a payload
	// that decodes under one and not the other is unambiguous.
	TArray<uint8> Bytes;
	if (!FBase64::Decode(In, Bytes, EBase64Mode::UrlSafe) && !FBase64::Decode(In, Bytes, EBase64Mode::Standard))
	{
		UE_LOG(LogModFramework, Warning, TEXT("Session manifest decode failed: the payload is not valid Base64."));
		return false;
	}

	if (Bytes.Num() == 0)
	{
		UE_LOG(LogModFramework, Warning, TEXT("Session manifest decode failed: the payload decoded to zero bytes."));
		return false;
	}

	if (Bytes.Num() > ModSessionManifest::MaxSerializedBytes)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Session manifest decode failed: %d decoded bytes exceeds the %d byte limit."),
			Bytes.Num(), ModSessionManifest::MaxSerializedBytes);
		return false;
	}

	FModSessionManifest Parsed;
	FMemoryReader Reader(Bytes, /*bIsPersistent=*/true);
	Reader << Parsed;

	if (Reader.IsError() || Reader.IsCriticalError())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Session manifest decode failed: the payload is malformed."));
		return false;
	}

	if (Reader.Tell() != Reader.TotalSize())
	{
		// Bounded by MaxSerializedBytes above, so the narrowing cast cannot lose information.
		UE_LOG(LogModFramework, Warning,
			TEXT("Session manifest decode failed: %d trailing bytes after the last entry."),
			static_cast<int32>(Reader.TotalSize() - Reader.Tell()));
		return false;
	}

	Out = MoveTemp(Parsed);
	return true;
}
