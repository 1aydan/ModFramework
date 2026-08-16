// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

GAMEMODSDK_API DECLARE_LOG_CATEGORY_EXTERN(LogGameModSDK, Log, All);

/**
 * Runtime module for the Game Mod SDK.
 *
 * The SDK is the compatibility boundary between a game and third-party mods. It owns every
 * game-specific concept the framework deliberately refuses to know about: weapons, enemies,
 * abilities, game rules. The framework stays game-agnostic; this module is where a game's public
 * modding vocabulary lives.
 *
 * Nothing in this module may reference the game's own module. Game behaviour is reached only
 * through UModAPI implementations the game registers at runtime, so a mod author can compile
 * against the SDK without ever having the game's source.
 */
class GAMEMODSDK_API FGameModSDKModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	static FGameModSDKModule& Get();
	static bool IsAvailable();
	static const TCHAR* GetModuleName();
};
