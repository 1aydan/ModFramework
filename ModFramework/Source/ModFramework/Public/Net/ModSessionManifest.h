// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Manifest/ModVersion.h"
#include "UObject/ObjectMacros.h"

class FArchive;

#include "ModSessionManifest.generated.h"

/**
 * Hard limits of the session manifest wire format.
 *
 * Everything a client sends is hostile until proven otherwise, so every one of these is enforced
 * *before* memory is allocated for the corresponding data. They are public because tests and the
 * editor tooling need to reason about the same numbers.
 */
namespace ModSessionManifest
{
	/** The only FModSessionManifest::FormatVersion FromBase64 accepts. */
	inline constexpr int32 CurrentFormatVersion = 1;

	/** Maximum number of FModNetworkEntry records in one manifest. */
	inline constexpr int32 MaxEntries = 4096;

	/** Maximum UTF-8 byte length of any single string in the wire format. */
	inline constexpr int32 MaxStringBytes = 512;

	/** Maximum size of the decoded byte stream. 4096 entries of realistic size fit comfortably. */
	inline constexpr int32 MaxSerializedBytes = 1024 * 1024;

	/**
	 * Maximum length of the Base64 text FromBase64 will even look at. Base64 expands by 4/3, so this
	 * bounds the decode buffer at roughly MaxSerializedBytes and stops a hostile login option from
	 * turning into a large allocation.
	 */
	inline constexpr int32 MaxEncodedLength = 1400000;

	/**
	 * Smallest number of bytes one entry can occupy in format version 1: five length-prefixed strings
	 * at zero length (5 x 4), three int32 version components (12) and two enum/bool bytes. Used as a
	 * cheap sanity check on a declared entry count before the read loop runs.
	 */
	inline constexpr int32 MinBytesPerEntry = 34;
}

/**
 * One mod as it is advertised across the network.
 *
 * This is deliberately *not* an FModManifest: a session manifest travels in a login option, so it
 * carries only what a compatibility check needs - who the mod is, which version, whether it matters
 * for network play and an optional content fingerprint.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModNetworkEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModId ModId;

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModVersion Version;

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	EModNetworkScope Scope = EModNetworkScope::ClientAndServer;

	/** Mirrors FModManifest::bRequiredForNetworkPlay for the mod this entry describes. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	bool bRequired = false;

	/**
	 * Optional fingerprint of the mod's content, normally the SHA1 hex from FModPackageHeader.
	 * An empty hash means "not available"; it never causes a mismatch on its own.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FString ContentHash;

	/** Human readable name, used verbatim in the message a rejected player sees. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FString DisplayName;

	/**
	 * Wire serialization for format version 1.
	 *
	 * The mod id travels as UTF-8 text rather than as an FName because a raw memory archive has no
	 * name table. Loading validates every field and calls FArchive::SetError on anything malformed;
	 * it never asserts.
	 */
	friend MODFRAMEWORK_API FArchive& operator<<(FArchive& Ar, FModNetworkEntry& E);
};

/**
 * The complete mod-relevant description of one side of a network session.
 *
 * The server builds one from UModSubsystem::BuildSessionManifest(), encodes it into the travel URL
 * or the login options, and FModNetworkValidator compares the two. Nothing in here is trusted when
 * it arrives from the network: FromBase64 rejects malformed, oversized and wrong-version payloads
 * without allocating for them.
 *
 * Byte order: the format is little-endian, which is the only byte order Unreal Engine supports.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSessionManifest
{
	GENERATED_BODY()

	/**
	 * Schema version of the encoded form; always ModSessionManifest::CurrentFormatVersion.
	 * ToBase64 stamps that value whatever this field holds, and FromBase64 accepts nothing else.
	 * (Spelled as a literal rather than as the constant so the reflection parser sees a plain value.)
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	int32 FormatVersion = 1;

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FString GameId;

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModVersion GameVersion;

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModVersion FrameworkVersion;

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FString SdkId;

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModVersion SdkVersion;

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	TArray<FModNetworkEntry> Entries;

	/** First entry with this id, or nullptr. Ids are compared as FNames, so case does not matter. */
	const FModNetworkEntry* FindEntry(const FModId& ModId) const;

	/**
	 * Stable digest of the required, network-relevant entries.
	 *
	 * Only entries with bRequired set and a Scope other than ClientOnly take part, because those are
	 * exactly the mods both sides must agree on. Each contributing entry becomes "<id>@<version>|",
	 * the tokens are sorted, concatenated and hashed with SHA1; the result is the uppercase hex
	 * digest. Two installs that would pass ValidateClient always produce the same digest, which
	 * makes this usable as a cheap server-browser filter.
	 */
	FString ComputeDigest() const;

	/**
	 * Compact, URL-safe encoding for travel URLs and login options.
	 *
	 * Uses the RFC 4648 base64url alphabet (EBase64Mode::UrlSafe) so the result never contains '+'
	 * or '/', characters that intermediate web services like to mangle. It never contains '?' or
	 * '#' either, which is what makes it safe inside an Unreal travel URL. Returns an empty string
	 * only if serialization produced no bytes at all.
	 */
	FString ToBase64() const;

	/**
	 * Inverse of ToBase64. Treats In as hostile input.
	 *
	 * Rejects: an empty string, a string longer than ModSessionManifest::MaxEncodedLength, text that
	 * is not valid Base64 in either alphabet, a decoded stream larger than
	 * ModSessionManifest::MaxSerializedBytes, a FormatVersion other than the current one, an entry
	 * count outside [0, ModSessionManifest::MaxEntries], any field that runs off the end of the
	 * stream, and trailing bytes after the last entry.
	 *
	 * Out is reset to a default-constructed manifest before anything else happens, so a caller that
	 * ignores the return value can never observe half-parsed data.
	 */
	static bool FromBase64(const FString& In, FModSessionManifest& Out);

	/** Wire serialization for format version 1. See FModNetworkEntry's operator for the rules. */
	friend MODFRAMEWORK_API FArchive& operator<<(FArchive& Ar, FModSessionManifest& M);
};
