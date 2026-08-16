// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "GameFramework/SaveGame.h"
#include "Save/ModSaveTypes.h"
#include "UObject/ObjectMacros.h"

#include "ModSaveGame.generated.h"

/**
 * The save game object the framework's own slot helpers read and write.
 *
 * A game that already has its own USaveGame subclass does not need this class at all: it can embed
 * an FModSaveEnvelope member, feed it to UModSaveDataManager::SetEnvelope after loading and read
 * UModSaveDataManager::GetEnvelope back before saving. UModSaveGame exists so that a game with no
 * save system of its own still gets isolated per-mod save data for free through
 * UModSaveDataManager::SaveToSlot / LoadFromSlot.
 *
 * Nothing here is mod-writable. Mods reach their own record through UModContext, which routes to
 * UModSaveDataManager and enforces the ModPermissions::SaveModify gate.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModSaveEnvelope Envelope;

	/** How many mod records this save carries, orphaned ones included. */
	UFUNCTION(BlueprintPure, Category = "Mod|Save")
	int32 GetRecordCount() const;

	/** How many of those records belong to a mod that was absent the last time the envelope was checked. */
	UFUNCTION(BlueprintPure, Category = "Mod|Save")
	int32 GetOrphanedRecordCount() const;

	/**
	 * Multi-line human readable dump of the envelope: game id, framework version, stamped mods and
	 * one line per record with its payload sizes. Used by the Mod.* console commands and by tests;
	 * never parse it.
	 */
	FString ToDebugString() const;
};
