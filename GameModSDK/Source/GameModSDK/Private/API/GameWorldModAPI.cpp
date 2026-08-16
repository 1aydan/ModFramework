// Copyright (c) 2026. Licensed for use in your own projects.

#include "API/GameWorldModAPI.h"

#include "API/ModAPI.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "GameModSDKModule.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModVersion.h"
#include "Math/Transform.h"
#include "UObject/NameTypes.h"

namespace
{
	/** The id this API registers under. Built once, on first use, so no static init order applies. */
	const TCHAR* const GameWorldApiIdString = TEXT("game.world");

	/** The permission a mod must hold before UModAPIRegistry::RequestAPI hands this API over. */
	const TCHAR* const GameWorldPermissionString = TEXT("gameplay.modify");

	/**
	 * Announces that the SDK's own inert body ran instead of a game implementation.
	 *
	 * The first call for a given entry point warns: a game that forgot to register its own
	 * UGameWorldModAPI subclass has a real configuration bug, and the mod that called in is about to
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

FName UGameWorldModAPI::GetStaticApiId()
{
	return FName(GameWorldApiIdString);
}

FModVersion UGameWorldModAPI::GetStaticApiVersion()
{
	return FModVersion(1, 0, 0);
}

FName UGameWorldModAPI::GetStaticRequiredPermission()
{
	return FName(GameWorldPermissionString);
}

//////////////////////////////////////////////////////////////////////////
// Spawning

AActor* UGameWorldModAPI::SpawnEnemy(FName EnemyId, const FTransform& SpawnTransform)
{
	if (!VerifyServerAuthority(this, TEXT("UGameWorldModAPI::SpawnEnemy")))
	{
		return nullptr;
	}

	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("SpawnEnemy"), bReported);

	return nullptr;
}

bool UGameWorldModAPI::DespawnEnemy(AActor* Enemy)
{
	if (!VerifyServerAuthority(this, TEXT("UGameWorldModAPI::DespawnEnemy")))
	{
		return false;
	}

	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("DespawnEnemy"), bReported);

	return false;
}

//////////////////////////////////////////////////////////////////////////
// Progress

int32 UGameWorldModAPI::GetActiveEnemyCount() const
{
	if (!VerifyServerAuthority(this, TEXT("UGameWorldModAPI::GetActiveEnemyCount")))
	{
		return 0;
	}

	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("GetActiveEnemyCount"), bReported);

	return 0;
}

int32 UGameWorldModAPI::GetCurrentWave() const
{
	if (!VerifyServerAuthority(this, TEXT("UGameWorldModAPI::GetCurrentWave")))
	{
		return 0;
	}

	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("GetCurrentWave"), bReported);

	return 0;
}

//////////////////////////////////////////////////////////////////////////
// Identity

FName UGameWorldModAPI::NativeGetApiId() const
{
	return GetStaticApiId();
}

FModVersion UGameWorldModAPI::NativeGetApiVersion() const
{
	return GetStaticApiVersion();
}

FString UGameWorldModAPI::NativeGetApiDescription() const
{
	return TEXT("Spawns and removes the game's enemies by definition id, and reports live enemy count and wave progress.");
}

bool UGameWorldModAPI::NativeGetServerAuthoritative(bool& bOutServerAuthoritative) const
{
	// Returning true means "this class has an opinion", which is what keeps the answer out of the
	// metadata fallback that a cooked build would strip.
	bOutServerAuthoritative = true;
	return true;
}

TArray<FName> UGameWorldModAPI::NativeGetRequiredPermissions() const
{
	return TArray<FName>{ GetStaticRequiredPermission() };
}
