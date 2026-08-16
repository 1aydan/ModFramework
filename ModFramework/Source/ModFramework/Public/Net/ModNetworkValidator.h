// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Manifest/ModVersion.h"
#include "Net/ModSessionManifest.h"
#include "UObject/ObjectMacros.h"

#include "ModNetworkValidator.generated.h"

/** Why one mod (or one piece of session identity) stops a client from joining a server. */
UENUM(BlueprintType)
enum class EModNetworkMismatchType : uint8
{
	/** The server requires a mod the client does not have at all. */
	MissingOnClient,
	/** The client requires a mod the server does not run. */
	MissingOnServer,
	/** Both sides have the mod, at different versions. */
	VersionMismatch,
	/** Same id and version, but the content fingerprints disagree. */
	ContentHashMismatch,
	/** The client is running a mod that declares itself server-only. */
	ScopeViolation,
	/** Different game, or a different major game version. */
	GameMismatch,
	/** Different major version of the mod framework. */
	FrameworkMismatch,
	/** Different modding SDK, or a different major SDK version. */
	SdkMismatch
};

/**
 * One reason a join was refused.
 *
 * `Expected` is always what the *server* has and `Actual` always what the *client* has, whichever
 * direction the check ran in, so a message can be phrased the same way every time. For a mod-level
 * mismatch a zero version means "absent" and
 * FModNetworkValidationResult::BuildUserFacingMessage renders it as "(not installed)"; for the game,
 * framework and SDK mismatches the versions are always printed literally.
 *
 * `Message` is a technical English sentence for logs and bug reports. Player-facing text is built
 * from `Type` instead, so it can be localized.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModNetworkMismatch
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModNetworkMismatchType Type = EModNetworkMismatchType::VersionMismatch;

	/** The mod involved. Invalid for the game, framework and SDK identity mismatches. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId ModId;

	/** Human readable label. Falls back to ModId when empty. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString DisplayName;

	/** What the server has. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModVersion Expected;

	/** What the client has. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModVersion Actual;

	/** Technical detail for logs. Never shown to a player. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString Message;
};

/** The outcome of comparing two session manifests. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModNetworkValidationResult
{
	GENERATED_BODY()

	/** True only when Mismatches is empty. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bCompatible = false;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModNetworkMismatch> Mismatches;

	/**
	 * The message a rejected player should read, for example:
	 *
	 *   Cannot join this server.
	 *   Server requires: BetterCombat 2.1.0
	 *   You have: BetterCombat 1.8.0
	 *   Reason: Incompatible required mod version.
	 *
	 * Every mismatch contributes those three lines. At most eight are listed; anything beyond that
	 * is summarised as a final "(+N more)" line, because a player who is missing thirty mods is not
	 * helped by a thirty item list.
	 */
	FText BuildUserFacingMessage() const;

	/** One dense line per mismatch, for LogModFramework and bug reports. */
	FString ToDebugString() const;
};

/**
 * Compares the mods a server runs against the mods a client runs.
 *
 * The server is authoritative. Both entry points below run the identical rule set, so a client's
 * pre-flight check can never disagree with the server's decision:
 *
 *  - a different game id fails immediately; nothing else is worth reporting;
 *  - a different major game, framework or SDK version fails;
 *  - every server mod that is required and not server-only must be present on the client at exactly
 *    the same version, and - when UModFrameworkSettings::bVerifyContentHashes is set and both sides
 *    published a hash - with the same content hash;
 *  - a client must not run a mod that declares itself server-only;
 *  - a mod the client requires and the server does not run fails, unless it is client-only;
 *  - client-only mods on the client are always fine, and are what makes cosmetic modding possible.
 *
 * Both manifests are untrusted data. Nothing here asserts, allocates unboundedly, or is quadratic in
 * the number of entries.
 */
class MODFRAMEWORK_API FModNetworkValidator
{
public:
	/** Server-side check of a joining client. ServerOnly mods are ignored on the client side. */
	static FModNetworkValidationResult ValidateClient(const FModSessionManifest& Server, const FModSessionManifest& Client);

	/**
	 * Client-side pre-flight against an advertised server manifest.
	 *
	 * Note the reversed parameter order: this is the same relationship seen from the other end, so it
	 * is deliberately implemented as ValidateClient(Server, Client). The prediction and the verdict
	 * are therefore the same computation and cannot drift apart.
	 */
	static FModNetworkValidationResult ValidateServer(const FModSessionManifest& Client, const FModSessionManifest& Server);
};
