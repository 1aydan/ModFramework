// Copyright (c) 2026. Licensed for use in your own projects.

#include "API/GameCombatModAPI.h"

#include "API/ModAPI.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "GameModSDKModule.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModVersion.h"
#include "UObject/NameTypes.h"

namespace
{
	/** The id this API registers under. Built once, on first use, so no static init order applies. */
	const TCHAR* const GameCombatApiIdString = TEXT("game.combat");

	/** The permission a mod must hold before UModAPIRegistry::RequestAPI hands this API over. */
	const TCHAR* const GameCombatPermissionString = TEXT("gameplay.modify");

	/**
	 * Announces that the SDK's own inert body ran instead of a game implementation.
	 *
	 * The first call for a given entry point warns: a game that forgot to register its own
	 * UGameCombatModAPI subclass has a real configuration bug, and the mod that called in is about to
	 * behave as though the game refused it. Every later call for the same entry point drops to
	 * Verbose, because a mod polling the API on tick must not be able to flood the log.
	 *
	 * bInOutReported is a latch owned by the call site. Game thread only, like the rest of the
	 * framework.
	 */
	void ReportNoGameImplementation(const UModAPI& Api, const TCHAR* EntryPoint, bool& bInOutReported)
	{
		if (!bInOutReported)
		{
			bInOutReported = true;

			UE_LOG(LogGameModSDK, Warning,
				TEXT("Mod API '%s' has no game implementation of %s. The SDK only declares this surface; the game must register a subclass that overrides it. The call was ignored."),
				*Api.GetApiId().ToString(), EntryPoint);

			return;
		}

		UE_LOG(LogGameModSDK, Verbose,
			TEXT("Mod API '%s': %s is still unimplemented; the call was ignored."),
			*Api.GetApiId().ToString(), EntryPoint);
	}
}

FName UGameCombatModAPI::GetStaticApiId()
{
	return FName(GameCombatApiIdString);
}

FModVersion UGameCombatModAPI::GetStaticApiVersion()
{
	return FModVersion(1, 0, 0);
}

FName UGameCombatModAPI::GetStaticRequiredPermission()
{
	return FName(GameCombatPermissionString);
}

//////////////////////////////////////////////////////////////////////////
// Damage

bool UGameCombatModAPI::ApplyDamage(AActor* Target, float Amount, FName DamageType)
{
	if (!VerifyServerAuthority(this, TEXT("UGameCombatModAPI::ApplyDamage")))
	{
		return false;
	}

	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("ApplyDamage"), bReported);

	return false;
}

//////////////////////////////////////////////////////////////////////////
// Health

float UGameCombatModAPI::GetHealth(AActor* Target) const
{
	if (!VerifyServerAuthority(this, TEXT("UGameCombatModAPI::GetHealth")))
	{
		return 0.0f;
	}

	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("GetHealth"), bReported);

	return 0.0f;
}

float UGameCombatModAPI::GetMaxHealth(AActor* Target) const
{
	if (!VerifyServerAuthority(this, TEXT("UGameCombatModAPI::GetMaxHealth")))
	{
		return 0.0f;
	}

	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("GetMaxHealth"), bReported);

	return 0.0f;
}

bool UGameCombatModAPI::IsAlive(AActor* Target) const
{
	if (!VerifyServerAuthority(this, TEXT("UGameCombatModAPI::IsAlive")))
	{
		return false;
	}

	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("IsAlive"), bReported);

	return false;
}

//////////////////////////////////////////////////////////////////////////
// Players

APawn* UGameCombatModAPI::GetPlayerPawn(int32 PlayerIndex) const
{
	if (!VerifyServerAuthority(this, TEXT("UGameCombatModAPI::GetPlayerPawn")))
	{
		return nullptr;
	}

	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("GetPlayerPawn"), bReported);

	return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// Identity

FName UGameCombatModAPI::NativeGetApiId() const
{
	return GetStaticApiId();
}

FModVersion UGameCombatModAPI::NativeGetApiVersion() const
{
	return GetStaticApiVersion();
}

FString UGameCombatModAPI::NativeGetApiDescription() const
{
	return TEXT("Reads and modifies combat state: applies damage, queries health and reaches the local players' pawns.");
}

bool UGameCombatModAPI::NativeGetServerAuthoritative(bool& bOutServerAuthoritative) const
{
	// Returning true means "this class has an opinion", which is what keeps the answer out of the
	// metadata fallback that a cooked build would strip.
	bOutServerAuthoritative = true;
	return true;
}

TArray<FName> UGameCombatModAPI::NativeGetRequiredPermissions() const
{
	return TArray<FName>{ GetStaticRequiredPermission() };
}
