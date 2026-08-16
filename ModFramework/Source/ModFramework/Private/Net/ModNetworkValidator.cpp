// Copyright (c) 2026. Licensed for use in your own projects.

#include "Net/ModNetworkValidator.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Text.h"
#include "Manifest/ModVersion.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CString.h"
#include "Net/ModSessionManifest.h"
#include "Settings/ModFrameworkSettings.h"

#define LOCTEXT_NAMESPACE "ModFramework"

namespace ModNetworkValidatorPrivate
{
	/** How many mismatches a player is shown before the rest are collapsed into "(+N more)". */
	static constexpr int32 MaxListedMismatches = 8;

	/**
	 * Content hash verification is opt-out through project settings. When the settings object is not
	 * available yet - which can happen very early in startup - the check stays on: a hash comparison
	 * only ever fires when both sides actually published a hash and they disagree, so failing closed
	 * costs nothing legitimate.
	 */
	static bool ShouldVerifyContentHashes()
	{
		if (const UModFrameworkSettings* Settings = UModFrameworkSettings::Get())
		{
			return Settings->bVerifyContentHashes;
		}
		return true;
	}

	/**
	 * Indexes a manifest's entries by id so validation stays linear in the number of entries.
	 *
	 * A hostile payload may repeat an id; the first occurrence wins, matching
	 * FModSessionManifest::FindEntry, so which entry gets checked never depends on the lookup path.
	 * The returned pointers alias Entries and are only valid while it is unmodified.
	 */
	static TMap<FModId, const FModNetworkEntry*> BuildEntryIndex(const TArray<FModNetworkEntry>& Entries)
	{
		TMap<FModId, const FModNetworkEntry*> Index;
		Index.Reserve(Entries.Num());

		for (const FModNetworkEntry& Entry : Entries)
		{
			if (!Index.Contains(Entry.ModId))
			{
				Index.Add(Entry.ModId, &Entry);
			}
		}

		return Index;
	}

	static FModNetworkMismatch& AddMismatch(FModNetworkValidationResult& Result, EModNetworkMismatchType Type)
	{
		FModNetworkMismatch& Mismatch = Result.Mismatches.AddDefaulted_GetRef();
		Mismatch.Type = Type;
		return Mismatch;
	}

	/** Stable identifier of a mismatch type, for logs. Never shown to a player. */
	static const TCHAR* TypeToString(EModNetworkMismatchType Type)
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
		default:                                           return TEXT("Unknown");
		}
	}

	/** The localizable sentence that follows "Reason: " for each mismatch type. */
	static FText TypeToReasonText(EModNetworkMismatchType Type)
	{
		switch (Type)
		{
		case EModNetworkMismatchType::MissingOnClient:
			return LOCTEXT("ModNetwork.Reason.MissingOnClient", "You do not have a mod this server requires.");
		case EModNetworkMismatchType::MissingOnServer:
			return LOCTEXT("ModNetwork.Reason.MissingOnServer", "This server does not run a mod you require.");
		case EModNetworkMismatchType::VersionMismatch:
			return LOCTEXT("ModNetwork.Reason.VersionMismatch", "Incompatible required mod version.");
		case EModNetworkMismatchType::ContentHashMismatch:
			return LOCTEXT("ModNetwork.Reason.ContentHashMismatch", "Your copy of this mod does not match the server's.");
		case EModNetworkMismatchType::ScopeViolation:
			return LOCTEXT("ModNetwork.Reason.ScopeViolation", "This mod is server-only and must not run on a client.");
		case EModNetworkMismatchType::GameMismatch:
			return LOCTEXT("ModNetwork.Reason.GameMismatch", "This server is running a different game or game version.");
		case EModNetworkMismatchType::FrameworkMismatch:
			return LOCTEXT("ModNetwork.Reason.FrameworkMismatch", "This server is running a different version of the mod framework.");
		case EModNetworkMismatchType::SdkMismatch:
			return LOCTEXT("ModNetwork.Reason.SdkMismatch", "This server is running a different modding SDK.");
		default:
			return LOCTEXT("ModNetwork.Reason.Unknown", "The server refused your mod list.");
		}
	}

	/** Player-facing name of whatever the mismatch is about. */
	static FText ResolveMismatchName(const FModNetworkMismatch& In)
	{
		if (!In.DisplayName.IsEmpty())
		{
			return FText::FromString(In.DisplayName);
		}
		if (In.ModId.IsValid())
		{
			return FText::FromString(In.ModId.ToString());
		}

		switch (In.Type)
		{
		case EModNetworkMismatchType::GameMismatch:
			return LOCTEXT("ModNetwork.Label.Game", "Game");
		case EModNetworkMismatchType::FrameworkMismatch:
			return LOCTEXT("ModNetwork.Label.Framework", "Mod Framework");
		case EModNetworkMismatchType::SdkMismatch:
			return LOCTEXT("ModNetwork.Label.Sdk", "Modding SDK");
		default:
			return LOCTEXT("ModNetwork.Label.UnknownMod", "Unknown mod");
		}
	}

	/** Log-facing name of whatever the mismatch is about. */
	static FString ResolveMismatchNameForLog(const FModNetworkMismatch& In)
	{
		if (!In.DisplayName.IsEmpty())
		{
			return In.DisplayName;
		}
		if (In.ModId.IsValid())
		{
			return In.ModId.ToString();
		}
		return TEXT("<none>");
	}

	/** True for the mismatch types that are about an individual mod rather than session identity. */
	static bool TypeDescribesAMod(EModNetworkMismatchType Type)
	{
		switch (Type)
		{
		case EModNetworkMismatchType::GameMismatch:
		case EModNetworkMismatchType::FrameworkMismatch:
		case EModNetworkMismatchType::SdkMismatch:
			return false;
		default:
			return true;
		}
	}

	/**
	 * Renders one side of a comparison.
	 *
	 * For a mod, a zero version means the side does not have it at all - that is how MissingOnClient,
	 * MissingOnServer and ScopeViolation encode absence. A mod genuinely installed at 0.0.0 would read
	 * the same way, but 0.0.0 is not a valid published mod version (FModManifest::IsValid rejects it)
	 * so that cannot happen in practice. Game, framework and SDK versions are never "absent", so they
	 * are always printed as-is.
	 */
	static FText FormatVersionSide(EModNetworkMismatchType Type, const FModVersion& Version)
	{
		if (TypeDescribesAMod(Type) && Version.IsZero())
		{
			return LOCTEXT("ModNetwork.NotInstalled", "(not installed)");
		}
		return FText::FromString(Version.ToString());
	}
}

//////////////////////////////////////////////////////////////////////////
// FModNetworkValidationResult

FText FModNetworkValidationResult::BuildUserFacingMessage() const
{
	using namespace ModNetworkValidatorPrivate;

	if (bCompatible)
	{
		return LOCTEXT("ModNetwork.Compatible", "Your installed mods match this server.");
	}

	if (Mismatches.Num() == 0)
	{
		return LOCTEXT("ModNetwork.CannotJoin", "Cannot join this server.");
	}

	const int32 NumListed = FMath::Min(Mismatches.Num(), MaxListedMismatches);

	TArray<FText> Lines;
	Lines.Reserve(NumListed * 3 + 2);
	Lines.Add(LOCTEXT("ModNetwork.CannotJoin", "Cannot join this server."));
	for (int32 Index = 0; Index < NumListed; ++Index)
	{
		const FModNetworkMismatch& Mismatch = Mismatches[Index];
		const FText Name = ResolveMismatchName(Mismatch);

		{
			FFormatNamedArguments Args;
			Args.Add(TEXT("Name"), Name);
			Args.Add(TEXT("Version"), FormatVersionSide(Mismatch.Type, Mismatch.Expected));
			Lines.Add(FText::Format(LOCTEXT("ModNetwork.ServerRequires", "Server requires: {Name} {Version}"), Args));
		}

		{
			FFormatNamedArguments Args;
			Args.Add(TEXT("Name"), Name);
			Args.Add(TEXT("Version"), FormatVersionSide(Mismatch.Type, Mismatch.Actual));
			Lines.Add(FText::Format(LOCTEXT("ModNetwork.YouHave", "You have: {Name} {Version}"), Args));
		}

		{
			FFormatNamedArguments Args;
			Args.Add(TEXT("Reason"), TypeToReasonText(Mismatch.Type));
			Lines.Add(FText::Format(LOCTEXT("ModNetwork.ReasonLine", "Reason: {Reason}"), Args));
		}
	}

	if (Mismatches.Num() > NumListed)
	{
		Lines.Add(FText::Format(
			LOCTEXT("ModNetwork.MoreMismatches", "(+{0} more)"),
			FText::AsNumber(Mismatches.Num() - NumListed)));
	}

	return FText::Join(FText::FromString(TEXT("\n")), Lines);
}

FString FModNetworkValidationResult::ToDebugString() const
{
	using namespace ModNetworkValidatorPrivate;

	if (Mismatches.Num() == 0)
	{
		return bCompatible ? TEXT("Compatible.") : TEXT("Incompatible, but no mismatch was recorded.");
	}

	FString Out = FString::Printf(TEXT("%s (%d mismatch(es)):"),
		bCompatible ? TEXT("Compatible") : TEXT("Incompatible"), Mismatches.Num());

	for (const FModNetworkMismatch& Mismatch : Mismatches)
	{
		Out += FString::Printf(TEXT("\n  [%s] %s: server=%s client=%s"),
			TypeToString(Mismatch.Type),
			*ResolveMismatchNameForLog(Mismatch),
			*Mismatch.Expected.ToString(),
			*Mismatch.Actual.ToString());

		if (!Mismatch.Message.IsEmpty())
		{
			Out += TEXT(" - ");
			Out += Mismatch.Message;
		}
	}

	return Out;
}

//////////////////////////////////////////////////////////////////////////
// FModNetworkValidator

FModNetworkValidationResult FModNetworkValidator::ValidateClient(const FModSessionManifest& Server, const FModSessionManifest& Client)
{
	using namespace ModNetworkValidatorPrivate;

	FModNetworkValidationResult Result;

	// --- Session identity ------------------------------------------------------------------------
	// A different game is not a mod problem. Reporting mod differences on top of it would only bury
	// the one thing the player needs to read, so stop here.
	if (!Server.GameId.Equals(Client.GameId, ESearchCase::IgnoreCase))
	{
		FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::GameMismatch);
		Mismatch.Expected = Server.GameVersion;
		Mismatch.Actual = Client.GameVersion;
		Mismatch.Message = FString::Printf(TEXT("Server game id '%s' does not match client game id '%s'."),
			*Server.GameId, *Client.GameId);

		Result.bCompatible = false;
		return Result;
	}

	if (Server.GameVersion.Major != Client.GameVersion.Major)
	{
		FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::GameMismatch);
		Mismatch.Expected = Server.GameVersion;
		Mismatch.Actual = Client.GameVersion;
		Mismatch.Message = FString::Printf(TEXT("Server game version %s and client game version %s differ in the major component."),
			*Server.GameVersion.ToString(), *Client.GameVersion.ToString());
	}

	if (Server.FrameworkVersion.Major != Client.FrameworkVersion.Major)
	{
		FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::FrameworkMismatch);
		Mismatch.Expected = Server.FrameworkVersion;
		Mismatch.Actual = Client.FrameworkVersion;
		Mismatch.Message = FString::Printf(TEXT("Server framework version %s and client framework version %s differ in the major component."),
			*Server.FrameworkVersion.ToString(), *Client.FrameworkVersion.ToString());
	}

	// A different SDK id makes the SDK versions incomparable, so only one of the two is reported.
	if (!Server.SdkId.Equals(Client.SdkId, ESearchCase::IgnoreCase))
	{
		FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::SdkMismatch);
		Mismatch.Expected = Server.SdkVersion;
		Mismatch.Actual = Client.SdkVersion;
		Mismatch.Message = FString::Printf(TEXT("Server SDK id '%s' does not match client SDK id '%s'."),
			*Server.SdkId, *Client.SdkId);
	}
	else if (Server.SdkVersion.Major != Client.SdkVersion.Major)
	{
		FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::SdkMismatch);
		Mismatch.Expected = Server.SdkVersion;
		Mismatch.Actual = Client.SdkVersion;
		Mismatch.Message = FString::Printf(TEXT("Server SDK version %s and client SDK version %s differ in the major component."),
			*Server.SdkVersion.ToString(), *Client.SdkVersion.ToString());
	}

	const TMap<FModId, const FModNetworkEntry*> ServerIndex = BuildEntryIndex(Server.Entries);
	const TMap<FModId, const FModNetworkEntry*> ClientIndex = BuildEntryIndex(Client.Entries);
	const bool bVerifyHashes = ShouldVerifyContentHashes();

	// --- Everything the server requires of a client ----------------------------------------------
	for (const FModNetworkEntry& ServerEntry : Server.Entries)
	{
		// Optional mods are the server's own business, and a server-only mod is by definition not
		// something a client is expected to have.
		if (!ServerEntry.bRequired || ServerEntry.Scope == EModNetworkScope::ServerOnly)
		{
			continue;
		}

		const FModNetworkEntry* const* Found = ClientIndex.Find(ServerEntry.ModId);
		if (Found == nullptr)
		{
			FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::MissingOnClient);
			Mismatch.ModId = ServerEntry.ModId;
			Mismatch.DisplayName = ServerEntry.DisplayName;
			Mismatch.Expected = ServerEntry.Version;
			Mismatch.Message = FString::Printf(TEXT("Client does not have required mod '%s' %s."),
				*ServerEntry.ModId.ToString(), *ServerEntry.Version.ToString());
			continue;
		}

		const FModNetworkEntry& ClientEntry = **Found;

		// Exact version equality, not a range: a required networked mod has to be the same build on
		// both sides or replicated state can diverge in ways no range expression can describe.
		if (ServerEntry.Version != ClientEntry.Version)
		{
			FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::VersionMismatch);
			Mismatch.ModId = ServerEntry.ModId;
			Mismatch.DisplayName = ServerEntry.DisplayName.IsEmpty() ? ClientEntry.DisplayName : ServerEntry.DisplayName;
			Mismatch.Expected = ServerEntry.Version;
			Mismatch.Actual = ClientEntry.Version;
			Mismatch.Message = FString::Printf(TEXT("Required mod '%s': server has %s, client has %s."),
				*ServerEntry.ModId.ToString(), *ServerEntry.Version.ToString(), *ClientEntry.Version.ToString());
			continue;
		}

		// An absent hash on either side means "unknown", never "different".
		if (bVerifyHashes
			&& !ServerEntry.ContentHash.IsEmpty()
			&& !ClientEntry.ContentHash.IsEmpty()
			&& !ServerEntry.ContentHash.Equals(ClientEntry.ContentHash, ESearchCase::IgnoreCase))
		{
			FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::ContentHashMismatch);
			Mismatch.ModId = ServerEntry.ModId;
			Mismatch.DisplayName = ServerEntry.DisplayName.IsEmpty() ? ClientEntry.DisplayName : ServerEntry.DisplayName;
			Mismatch.Expected = ServerEntry.Version;
			Mismatch.Actual = ClientEntry.Version;
			Mismatch.Message = FString::Printf(TEXT("Required mod '%s' %s: server content hash %s, client content hash %s."),
				*ServerEntry.ModId.ToString(), *ServerEntry.Version.ToString(),
				*ServerEntry.ContentHash, *ClientEntry.ContentHash);
		}
	}

	// --- Everything the client brings to the session ---------------------------------------------
	for (const FModNetworkEntry& ClientEntry : Client.Entries)
	{
		// A server-only mod running on a client is a red flag regardless of anything else: it is
		// either a misconfigured install or an attempt to run authoritative logic client side.
		if (ClientEntry.Scope == EModNetworkScope::ServerOnly)
		{
			FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::ScopeViolation);
			Mismatch.ModId = ClientEntry.ModId;
			Mismatch.DisplayName = ClientEntry.DisplayName;
			Mismatch.Actual = ClientEntry.Version;
			Mismatch.Message = FString::Printf(TEXT("Client is running '%s' %s, which declares itself server-only."),
				*ClientEntry.ModId.ToString(), *ClientEntry.Version.ToString());
			continue;
		}

		// Client-only mods are the whole point of cosmetic modding: the server never needs to know.
		if (ClientEntry.Scope == EModNetworkScope::ClientOnly)
		{
			continue;
		}

		if (!ClientEntry.bRequired)
		{
			continue;
		}

		if (ServerIndex.Find(ClientEntry.ModId) == nullptr)
		{
			FModNetworkMismatch& Mismatch = AddMismatch(Result, EModNetworkMismatchType::MissingOnServer);
			Mismatch.ModId = ClientEntry.ModId;
			Mismatch.DisplayName = ClientEntry.DisplayName;
			Mismatch.Actual = ClientEntry.Version;
			Mismatch.Message = FString::Printf(TEXT("Server does not run '%s' %s, which the client requires for network play."),
				*ClientEntry.ModId.ToString(), *ClientEntry.Version.ToString());
		}
	}

	Result.bCompatible = Result.Mismatches.Num() == 0;
	return Result;
}

FModNetworkValidationResult FModNetworkValidator::ValidateServer(const FModSessionManifest& Client, const FModSessionManifest& Server)
{
	// Same relationship, opposite viewpoint. Sharing the implementation is what guarantees that a
	// client's pre-flight answer and the server's actual verdict can never disagree.
	return ValidateClient(Server, Client);
}

#undef LOCTEXT_NAMESPACE
