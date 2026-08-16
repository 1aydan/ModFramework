// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Manifest/ModVersion.h"
#include "UObject/Interface.h"
#include "UObject/ObjectMacros.h"

#include "ModSaveTypes.generated.h"

/**
 * One mod that was active when a save was written.
 *
 * This is a *stamp*, not a requirement the framework enforces: loading a save whose mods are gone is
 * always allowed, and the missing mods are reported through UModSaveDataManager::GetMissingMods so
 * the game can decide whether to warn, block or carry on.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSaveDependency
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModId ModId;

	/** The version of the mod that was running when the save was stamped. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModVersion Version;

	/** Human readable name captured at stamp time, so a mod-missing dialog can name the mod. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FString DisplayName;

	/** Copied from FModManifest::bRequiredForNetworkPlay at stamp time. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	bool bWasRequired = false;
};

/**
 * The isolated save payload of exactly one mod.
 *
 * Records are opaque to the framework. A mod owns its Json and its Binary bytes and nothing else can
 * read or write them through the public API. The only structural rule the framework enforces is
 * this one:
 *
 *   **A record whose mod is absent is never modified and never deleted.** It is flagged bOrphaned by
 *   UModSaveDataManager::MarkOrphanedRecords and then carried through every subsequent save/load
 *   round trip byte for byte, so uninstalling a mod, playing, saving and reinstalling the mod gets
 *   the player's data back. UModSaveDataManager::PurgeOrphanedRecord is the single, explicit,
 *   loudly-logged way to destroy one.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSaveRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModId ModId;

	/** Version of the mod that last wrote this record. Left at 0.0.0 when it could not be resolved. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModVersion ModVersion;

	/** Mod-authored schema version, used for migration. Always >= 1 for a record the framework wrote. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	int32 DataVersion = 1;

	/** Free-form JSON payload. Capped at UModSaveDataManager::MaxJsonPayloadChars. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FString Json;

	/** Free-form binary payload. Capped at UModSaveDataManager::MaxBinaryPayloadBytes. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	TArray<uint8> Binary;

	/** True when the owning mod was absent at load time - preserved verbatim, never dropped. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	bool bOrphaned = false;

	bool HasJson() const { return !Json.IsEmpty(); }
	bool HasBinary() const { return Binary.Num() > 0; }
	bool IsEmpty() const { return Json.IsEmpty() && Binary.Num() == 0; }
};

/**
 * Everything the mod framework contributes to a save file.
 *
 * A game embeds this in its own USaveGame (or uses UModSaveGame directly) and the framework never
 * needs to know anything about the game's own save data.
 *
 * Note on serialization: UGameplayStatics::SaveGameToSlot serializes a USaveGame with a plain
 * FObjectAndNameAsStringProxyArchive - it does *not* set FArchive::ArIsSaveGame - so every UPROPERTY
 * below round-trips without needing the `SaveGame` specifier. A game that writes its own archive
 * with ArIsSaveGame = true must serialize its FModSaveEnvelope member explicitly instead of relying
 * on property flags.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSaveEnvelope
{
	GENERATED_BODY()

	/** Schema version of this struct. Bumped only when the envelope layout itself changes. */
	static constexpr int32 CurrentEnvelopeVersion = 1;

	/** Kept as a literal rather than CurrentEnvelopeVersion so the initializer needs no reflection lookup. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	int32 EnvelopeVersion = 1;

	/** UModFrameworkSettings::GameId at the time the envelope was stamped. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FString GameId;

	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	FModVersion FrameworkVersion;

	/** The mods that were active when the save was written. See UModSaveDataManager::StampRequiredMods. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	TArray<FModSaveDependency> RequiredMods;

	/** One record per mod that has ever written data into this save, including orphaned ones. */
	UPROPERTY(BlueprintReadWrite, Category = "Mod")
	TArray<FModSaveRecord> Records;

	/** The first record for ModId, or nullptr. */
	const FModSaveRecord* FindRecord(const FModId& ModId) const
	{
		for (const FModSaveRecord& Record : Records)
		{
			if (Record.ModId == ModId)
			{
				return &Record;
			}
		}

		return nullptr;
	}

	/** Mutable counterpart of FindRecord. The pointer is invalidated by any change to Records. */
	FModSaveRecord* FindRecordMutable(const FModId& ModId)
	{
		for (FModSaveRecord& Record : Records)
		{
			if (Record.ModId == ModId)
			{
				return &Record;
			}
		}

		return nullptr;
	}

	/**
	 * The record for ModId, appending an empty one when there is none.
	 * The returned reference is invalidated by any later change to Records.
	 */
	FModSaveRecord& FindOrAddRecord(const FModId& ModId)
	{
		if (FModSaveRecord* Existing = FindRecordMutable(ModId))
		{
			return *Existing;
		}

		FModSaveRecord& Added = Records.AddDefaulted_GetRef();
		Added.ModId = ModId;
		return Added;
	}
};

/**
 * Implemented by a mod (or by the game on a mod's behalf) to upgrade a save record written by an
 * older schema version of that same mod.
 *
 * The framework calls this once per version step - 1 to 2, then 2 to 3, and so on - so a mod only
 * ever has to describe how to move forward by one version. Returning false stops the walk and leaves
 * the stored record untouched; the framework never migrates downwards and never guesses a missing
 * step.
 *
 * Record is passed by reference and is a working copy: mutate Json, Binary and any other field on
 * it. Do not call back into UModSaveDataManager from inside a migration - the working copy is
 * written back over the stored record when the walk finishes, so any write made from inside would be
 * overwritten.
 */
UINTERFACE(BlueprintType, MinimalAPI)
class UModSaveMigration : public UInterface
{
	GENERATED_BODY()
};

class MODFRAMEWORK_API IModSaveMigration
{
	GENERATED_BODY()

public:
	/**
	 * Migrates Record from FromVersion to ToVersion, where ToVersion is always FromVersion + 1.
	 * Return false when the step cannot be performed; the framework logs it and abandons the walk
	 * with the stored record unchanged.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Mod|Save")
	bool MigrateModSave(const FModId& ModId, int32 FromVersion, int32 ToVersion, UPARAM(ref) FModSaveRecord& Record);
};
