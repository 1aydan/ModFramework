// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Net/ModSessionManifest.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "ModNetworkStatics.generated.h"

/**
 * The glue between the session manifest and Unreal's travel/login plumbing.
 *
 * A game wires mod compatibility in with two calls:
 *
 *   // Client, just before travelling.
 *   const FString URL = UModNetworkStatics::AppendModManifestToTravelURL(
 *       BaseURL, UModNetworkStatics::BuildLocalSessionManifestOption(this));
 *
 *   // Server, in AGameModeBase::PreLogin.
 *   FString Error;
 *   if (!UModNetworkStatics::ValidateJoiningPlayer(this, Options, Error))
 *   {
 *       ErrorMessage = Error;
 *   }
 *
 * Travel URL note (verified against Engine/Private/URL.cpp and
 * Engine/Private/GameplayStatics.cpp in UE 5.8): Unreal option strings are `?key=value` pairs
 * separated by '?', not by '&' - `UGameplayStatics::GrabOption` requires a leading '?' on every
 * option and `FURL` splits on '?' and '#'. So the separator is '?' for the first option and for
 * every option after it. `FURL` also rejects any option containing '?' or '#' (`ValidNetChar`) and
 * treats an option starting with '-' as an option *removal*; the base64url alphabet contains none
 * of those characters and the fragment always starts with "ModManifest=", so an encoded manifest is
 * always a legal option.
 */
UCLASS()
class MODFRAMEWORK_API UModNetworkStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** The login option key the framework reads and writes: "ModManifest". */
	static const TCHAR* GetLoginOptionKey();

	/**
	 * The local session manifest as a bare `ModManifest=<base64url>` option fragment.
	 *
	 * Deliberately WITHOUT a leading '?': the separator belongs to whoever assembles the URL, which
	 * is AppendModManifestToTravelURL. Returns an empty string when there is no mod subsystem to ask.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|Network", meta = (WorldContext = "WorldContextObject"))
	static FString BuildLocalSessionManifestOption(const UObject* WorldContextObject);

	/**
	 * Reads the "ModManifest" option out of an Unreal options string and decodes it.
	 *
	 * OutManifest is reset first, so it is always well defined. Returns false when the option is
	 * absent, empty or malformed - the caller decides which of those is fatal.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Network")
	static bool ParseSessionManifestFromOptions(const FString& Options, FModSessionManifest& OutManifest);

	/**
	 * The server-side gate, meant to be called from AGameModeBase::PreLogin.
	 *
	 * Builds the local (server) session manifest, decodes the client's out of Options, runs
	 * FModNetworkValidator::ValidateClient and, when the answer is "no", fills OutErrorMessage with
	 * FModNetworkValidationResult::BuildUserFacingMessage().ToString().
	 *
	 * Three cases are worth knowing about:
	 *  - no mod subsystem on this server: nothing to enforce, the join is allowed and a warning is
	 *    logged, so dropping the framework out of a build never locks players out;
	 *  - no "ModManifest" option at all: the client is treated as a matching install running no mods,
	 *    so a vanilla client still joins a vanilla server and only ever sees mod-level reasons;
	 *  - a "ModManifest" option that will not decode: rejected outright, because a client that sends
	 *    a corrupt or hostile payload has already told you enough.
	 *
	 * @return true when the player may join.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Network", meta = (WorldContext = "WorldContextObject"))
	static bool ValidateJoiningPlayer(const UObject* WorldContextObject, const FString& Options, FString& OutErrorMessage);

	/**
	 * Adds (or replaces) the ModManifest option on an Unreal travel URL.
	 *
	 * Accepts either a bare base64 payload or the whole `ModManifest=<base64>` fragment that
	 * BuildLocalSessionManifestOption returns, so the two compose without a double prefix. An
	 * existing ModManifest option is dropped rather than duplicated, which makes repeated calls
	 * idempotent, and a '#Portal' suffix is preserved after the options. The URL is returned
	 * unchanged when the payload is empty or contains a character an Unreal option may not hold.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|Network")
	static FString AppendModManifestToTravelURL(const FString& TravelURL, const FString& EncodedManifest);
};
