// Copyright (c) 2026. Licensed for use in your own projects.

#include "Net/ModNetworkStatics.h"

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "CoreTypes.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Text.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogMacros.h"
#include "Misc/CString.h"
#include "Net/ModNetworkValidator.h"
#include "Net/ModSessionManifest.h"
#include "Subsystem/ModSubsystem.h"
#include "UObject/Object.h"

#define LOCTEXT_NAMESPACE "ModFramework"

namespace ModNetworkStaticsPrivate
{
	/** "ModManifest=" - the option fragment prefix, built once per call site that needs it. */
	static FString MakeOptionPrefix()
	{
		return FString(UModNetworkStatics::GetLoginOptionKey()) + TEXT("=");
	}

	/**
	 * A client that sent nothing is treated as a matching install with no mods at all.
	 *
	 * Copying the server's identity fields is what makes that true: without it, the empty GameId
	 * would trip the game-identity check and a player missing one mod would be told they are running
	 * the wrong game. Every mod-level rule still applies, so a server that requires mods still turns
	 * such a client away - with a message that names the mods.
	 */
	static FModSessionManifest MakeEmptyClientManifest(const FModSessionManifest& Server)
	{
		FModSessionManifest Client;
		Client.GameId = Server.GameId;
		Client.GameVersion = Server.GameVersion;
		Client.FrameworkVersion = Server.FrameworkVersion;
		Client.SdkId = Server.SdkId;
		Client.SdkVersion = Server.SdkVersion;
		return Client;
	}
}

const TCHAR* UModNetworkStatics::GetLoginOptionKey()
{
	return TEXT("ModManifest");
}

FString UModNetworkStatics::BuildLocalSessionManifestOption(const UObject* WorldContextObject)
{
	UModSubsystem* Subsystem = UModSubsystem::Get(WorldContextObject);
	if (Subsystem == nullptr)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("BuildLocalSessionManifestOption: no mod subsystem for this world context; travelling without a mod manifest."));
		return FString();
	}

	const FModSessionManifest Manifest = Subsystem->BuildSessionManifest();
	const FString Encoded = Manifest.ToBase64();
	if (Encoded.IsEmpty())
	{
		UE_LOG(LogModFramework, Warning, TEXT("BuildLocalSessionManifestOption: the local session manifest could not be encoded."));
		return FString();
	}

	// No leading '?': AppendModManifestToTravelURL owns the separator.
	return ModNetworkStaticsPrivate::MakeOptionPrefix() + Encoded;
}

bool UModNetworkStatics::ParseSessionManifestFromOptions(const FString& Options, FModSessionManifest& OutManifest)
{
	OutManifest = FModSessionManifest();

	const FString Encoded = UGameplayStatics::ParseOption(Options, GetLoginOptionKey());
	if (Encoded.IsEmpty())
	{
		return false;
	}

	return FModSessionManifest::FromBase64(Encoded, OutManifest);
}

bool UModNetworkStatics::ValidateJoiningPlayer(const UObject* WorldContextObject, const FString& Options, FString& OutErrorMessage)
{
	OutErrorMessage.Reset();

	UModSubsystem* Subsystem = UModSubsystem::Get(WorldContextObject);
	if (Subsystem == nullptr)
	{
		// Nothing to enforce. Refusing every player because the framework is absent would be a far
		// worse failure than letting an unvalidated one in.
		UE_LOG(LogModFramework, Warning,
			TEXT("ValidateJoiningPlayer: no mod subsystem for this world context; the joining player is not validated."));
		return true;
	}

	const FModSessionManifest ServerManifest = Subsystem->BuildSessionManifest();

	FModSessionManifest ClientManifest;
	const FString Encoded = UGameplayStatics::ParseOption(Options, GetLoginOptionKey());
	if (Encoded.IsEmpty())
	{
		ClientManifest = ModNetworkStaticsPrivate::MakeEmptyClientManifest(ServerManifest);
	}
	else if (!FModSessionManifest::FromBase64(Encoded, ClientManifest))
	{
		const FText Message = FText::Join(FText::FromString(TEXT("\n")),
			LOCTEXT("ModNetwork.CannotJoin", "Cannot join this server."),
			LOCTEXT("ModNetwork.UnreadableManifest", "Reason: Your mod list could not be read."));

		OutErrorMessage = Message.ToString();

		UE_LOG(LogModFramework, Warning,
			TEXT("ValidateJoiningPlayer: rejecting a player whose mod manifest (%d encoded characters) could not be decoded."),
			Encoded.Len());
		return false;
	}

	const FModNetworkValidationResult Result = FModNetworkValidator::ValidateClient(ServerManifest, ClientManifest);
	if (Result.bCompatible)
	{
		return true;
	}

	OutErrorMessage = Result.BuildUserFacingMessage().ToString();

	UE_LOG(LogModFramework, Warning, TEXT("ValidateJoiningPlayer: %s"), *Result.ToDebugString());
	return false;
}

FString UModNetworkStatics::AppendModManifestToTravelURL(const FString& TravelURL, const FString& EncodedManifest)
{
	const FString OptionPrefix = ModNetworkStaticsPrivate::MakeOptionPrefix();

	FString Encoded = EncodedManifest.TrimStartAndEnd();

	// Accept both what BuildLocalSessionManifestOption returns and a bare payload, so composing the
	// two never produces "ModManifest=ModManifest=...".
	while (Encoded.StartsWith(TEXT("?"), ESearchCase::CaseSensitive))
	{
		Encoded = Encoded.RightChop(1);
	}
	if (Encoded.StartsWith(OptionPrefix, ESearchCase::IgnoreCase))
	{
		Encoded = Encoded.RightChop(OptionPrefix.Len());
	}

	if (Encoded.IsEmpty())
	{
		return TravelURL;
	}

	// FURL rejects an option containing either of these, so appending one would silently invalidate
	// the whole URL. Nothing this module produces can contain them; a caller-supplied payload can.
	if (Encoded.Contains(TEXT("?")) || Encoded.Contains(TEXT("#")))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("AppendModManifestToTravelURL: the encoded manifest contains '?' or '#' and cannot be a travel URL option; the URL was left unchanged."));
		return TravelURL;
	}

	// Everything from the first '#' is the portal fragment and must stay behind the options.
	FString Head = TravelURL;
	FString PortalTail;
	int32 HashIndex = 0;
	if (TravelURL.FindChar(TEXT('#'), HashIndex))
	{
		Head = TravelURL.Left(HashIndex);
		PortalTail = TravelURL.Mid(HashIndex);
	}

	// Unreal separates options with '?', including the second and later ones - there is no '&'.
	TArray<FString> Parts;
	Head.ParseIntoArray(Parts, TEXT("?"), /*bInCullEmpty=*/false);

	FString Rebuilt;
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		if (Index > 0)
		{
			// Drop a ModManifest option that is already there, so repeated calls stay idempotent
			// instead of stacking stale manifests that ParseOption would shadow.
			FString Key = Parts[Index];
			int32 EqualsIndex = 0;
			if (Key.FindChar(TEXT('='), EqualsIndex))
			{
				Key = Key.Left(EqualsIndex);
			}
			if (Key.Equals(GetLoginOptionKey(), ESearchCase::IgnoreCase))
			{
				continue;
			}

			Rebuilt += TEXT("?");
		}

		Rebuilt += Parts[Index];
	}

	Rebuilt += TEXT("?");
	Rebuilt += OptionPrefix;
	Rebuilt += Encoded;
	Rebuilt += PortalTail;

	return Rebuilt;
}

#undef LOCTEXT_NAMESPACE
