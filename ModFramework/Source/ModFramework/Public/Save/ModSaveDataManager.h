// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "JsonObjectConverter.h"
#include "Manifest/ModVersion.h"
#include "Save/ModSaveTypes.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptInterface.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ModSaveDataManager.generated.h"

class UModRegistry;
class UModSubsystem;

/**
 * Isolated per-mod save data.
 *
 * The manager owns a single FModSaveEnvelope. Every mod gets exactly one FModSaveRecord inside it,
 * keyed by mod id, and the public API never lets one mod read or write another's record. That
 * isolation is what makes the central promise of this class possible:
 *
 *   **Removing a mod never corrupts or drops unrelated data.** A record whose mod is absent is
 *   flagged bOrphaned by MarkOrphanedRecords and then carried through save/load untouched, byte for
 *   byte. Nothing in the normal flow deletes one. PurgeOrphanedRecord is the only destructive
 *   operation, it refuses to touch a record whose mod is present, and it logs a warning when it
 *   does delete.
 *
 * Everything a mod hands over is untrusted. Ids are validated, payloads are size capped
 * (MaxJsonPayloadChars / MaxBinaryPayloadBytes), data versions are range checked, migrations are
 * walked one step at a time with a hard step limit, and every refusal is a logged diagnostic rather
 * than an assert.
 *
 * Writing requires the calling mod to hold ModPermissions::SaveModify, which is asked of the owning
 * subsystem's permission registry. A manager created without a subsystem - which is how automation
 * tests use it - has nothing to ask, so it allows writes and reports no mods as missing or orphaned.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModSaveDataManager : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Upper bound on the character count of one record's Json payload (16 MiB).
	 * A mod that exceeds it gets Save.PayloadTooLarge and the write is refused; a mod is not allowed
	 * to make a save file unwritable for everyone else.
	 */
	static constexpr int64 MaxJsonPayloadChars = 16LL * 1024LL * 1024LL;

	/** Upper bound on the byte count of one record's Binary payload (64 MiB). */
	static constexpr int64 MaxBinaryPayloadBytes = 64LL * 1024LL * 1024LL;

	/**
	 * Upper bound on how many single-version migration steps one MigrateRecord call may run.
	 * Guards against a record or a target version that asks for an absurd walk through mod code.
	 */
	static constexpr int32 MaxMigrationSteps = 1024;

	/** Binds the manager to its owning subsystem. Passing nullptr is legal and means "standalone". */
	void Initialize(UModSubsystem* InSubsystem);

	/** Drops the envelope, the registered migrations and the subsystem link. */
	void Shutdown();

	/** Empties the envelope and re-stamps the current game id and framework version. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	void ResetEnvelope();

	/**
	 * Replaces the whole envelope, for a game that manages its own save file.
	 * Nothing is dropped or rewritten: duplicate records are reported but preserved.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	void SetEnvelope(const FModSaveEnvelope& In);

	/**
	 * The live envelope. Not a UFUNCTION: Blueprint cannot receive a const reference return.
	 * Blueprint uses GetEnvelopeCopy instead.
	 */
	const FModSaveEnvelope& GetEnvelope() const;

	/** Blueprint-facing copy of GetEnvelope. */
	UFUNCTION(BlueprintPure, Category = "Mod|Save")
	FModSaveEnvelope GetEnvelopeCopy() const;

	/**
	 * Stamps the envelope with the mods currently active, plus the game id and framework version.
	 * Call before writing a save. The previous stamp is replaced, the records are untouched.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	void StampRequiredMods();

	/**
	 * Stores Json as ModId's record payload. Requires ModPermissions::SaveModify.
	 * Returns false - and logs why - for an invalid id, a denied permission, a DataVersion below 1
	 * or a payload above MaxJsonPayloadChars. The record's Binary payload is left alone.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool WriteModJson(const FModId& ModId, const FString& Json, int32 DataVersion = 1);

	/** True when ModId has a record carrying a non-empty Json payload. Reading needs no permission. */
	UFUNCTION(BlueprintPure, Category = "Mod|Save")
	bool ReadModJson(const FModId& ModId, FString& OutJson, int32& OutDataVersion) const;

	/**
	 * Stores Bytes as ModId's record payload. Requires ModPermissions::SaveModify.
	 * Same refusals as WriteModJson, capped at MaxBinaryPayloadBytes. The Json payload is left alone.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool WriteModBytes(const FModId& ModId, const TArray<uint8>& Bytes, int32 DataVersion = 1);

	/** True when ModId has a record carrying a non-empty Binary payload. */
	UFUNCTION(BlueprintPure, Category = "Mod|Save")
	bool ReadModBytes(const FModId& ModId, TArray<uint8>& OutBytes, int32& OutDataVersion) const;

	/**
	 * Empties ModId's own payloads. Requires ModPermissions::SaveModify.
	 * The record itself survives - only PurgeOrphanedRecord removes a record. Returns false only when
	 * the call is refused, so clearing a mod that has no data at all still succeeds.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool ClearModData(const FModId& ModId);

	/**
	 * Typed helper for native code: serialises Value with FJsonObjectConverter and stores the result
	 * through WriteModJson, so every permission and size rule still applies. T must be a USTRUCT.
	 */
	template <typename T>
	bool WriteModStruct(const FModId& ModId, const T& Value, int32 DataVersion = 1);

	/**
	 * Typed counterpart of WriteModStruct. OutValue is only written when the record exists and its
	 * JSON parses; a malformed payload is a logged failure, never a crash.
	 */
	template <typename T>
	bool ReadModStruct(const FModId& ModId, T& OutValue, int32& OutDataVersion) const;

	/** Stamped mods that are not currently loaded. Empty when there is no subsystem to ask. */
	UFUNCTION(BlueprintPure, Category = "Mod|Save")
	TArray<FModSaveDependency> GetMissingMods() const;

	/** The subset of GetMissingMods whose bWasRequired is set. */
	UFUNCTION(BlueprintPure, Category = "Mod|Save")
	TArray<FModSaveDependency> GetMissingRequiredMods() const;

	/** Copies of every record currently flagged bOrphaned. */
	UFUNCTION(BlueprintPure, Category = "Mod|Save")
	TArray<FModSaveRecord> GetOrphanedRecords() const;

	/**
	 * Flags records whose mod is absent and clears the flag on records whose mod came back.
	 * Never deletes anything and never touches a record's payload. Called automatically by
	 * LoadFromSlot. Does nothing at all when there is no subsystem, because a manager with nothing to
	 * ask cannot tell "absent" from "unknown" and guessing would mislabel every record.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	void MarkOrphanedRecords();

	/**
	 * Permanently deletes ModId's orphaned record and its stamp entry. Explicit opt-in only:
	 * refuses - with a warning - when the mod is present, and logs a warning when it does delete.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool PurgeOrphanedRecord(const FModId& ModId);

	/**
	 * Registers the object that upgrades ModId's records between schema versions.
	 * A null Migration clears any previous registration. An object that does not implement
	 * IModSaveMigration is rejected with a warning rather than accepted and crashed on later.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	void RegisterMigration(const FModId& ModId, TScriptInterface<IModSaveMigration> Migration);

	/** Drops ModId's migration. True when one was registered. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool UnregisterMigration(const FModId& ModId);

	/**
	 * Walks ModId's record forward one version at a time until it reaches TargetVersion.
	 *
	 * Already at the target: succeeds and does nothing. Above the target: refused and logged - the
	 * framework never migrates downwards. No migration registered, a step that returns false, or a
	 * walk longer than MaxMigrationSteps: refused and logged, with the stored record left exactly as
	 * it was. The walk runs on a copy, so a failure halfway through cannot leave a half-migrated
	 * record behind.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool MigrateRecord(const FModId& ModId, int32 TargetVersion);

	/** Writes the envelope into a UModSaveGame in the named slot. Call StampRequiredMods first. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool SaveToSlot(const FString& SlotName, int32 UserIndex);

	/**
	 * Replaces the envelope with the one stored in the named slot, then runs MarkOrphanedRecords.
	 * A missing slot, an unreadable slot or a slot holding a different USaveGame class is a logged
	 * failure that leaves the current envelope untouched.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool LoadFromSlot(const FString& SlotName, int32 UserIndex);

private:
	/** Rejects an unusable mod id before it can reach the envelope. Logs Save.InvalidModId. */
	bool ValidateModId(const FModId& ModId, const TCHAR* Operation) const;

	/** Asks the owning subsystem whether ModId holds ModPermissions::SaveModify. */
	bool CheckWritePermission(const FModId& ModId, const TCHAR* Operation) const;

	/** Enforces MaxJsonPayloadChars / MaxBinaryPayloadBytes. Logs Save.PayloadTooLarge. */
	bool ValidatePayloadSizes(const FModId& ModId, const FModSaveRecord& Record, const TCHAR* Operation) const;

	/** The owning subsystem's mod registry, or nullptr when running standalone. */
	UModRegistry* GetRegistry() const;

	/** True when the registry knows ModId and reports it as loaded. False when there is no registry. */
	bool IsModPresent(const FModId& ModId) const;

	/** The registered version of ModId, or 0.0.0 when it cannot be resolved. */
	FModVersion ResolveModVersion(const FModId& ModId) const;

	/** Everything the framework contributes to the current save. */
	UPROPERTY()
	FModSaveEnvelope Envelope;

	/** Per-mod schema upgraders, registered through RegisterMigration. */
	UPROPERTY()
	TMap<FModId, TScriptInterface<IModSaveMigration>> Migrations;

	/**
	 * The subsystem that created this manager. Weak on purpose: the manager must stay usable while
	 * the subsystem tears down, and a manager built by a test has no subsystem at all.
	 */
	TWeakObjectPtr<UModSubsystem> OwningSubsystem;
};

template <typename T>
bool UModSaveDataManager::WriteModStruct(const FModId& ModId, const T& Value, int32 DataVersion)
{
	FString JsonText;
	if (!FJsonObjectConverter::UStructToJsonObjectString(
			T::StaticStruct(), &Value, JsonText, /*CheckFlags*/ 0, /*SkipFlags*/ 0, /*Indent*/ 0,
			/*ExportCb*/ nullptr, /*bPrettyPrint*/ false))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.SerializeFailed: could not serialise a '%s' into JSON for mod '%s'."),
			*T::StaticStruct()->GetName(), *ModId.ToString());
		return false;
	}

	return WriteModJson(ModId, JsonText, DataVersion);
}

template <typename T>
bool UModSaveDataManager::ReadModStruct(const FModId& ModId, T& OutValue, int32& OutDataVersion) const
{
	OutDataVersion = 0;

	FString JsonText;
	int32 RecordDataVersion = 0;
	if (!ReadModJson(ModId, JsonText, RecordDataVersion))
	{
		return false;
	}

	// The templated overload resolves T::StaticStruct() internally and reports parse failures itself.
	if (!FJsonObjectConverter::JsonObjectStringToUStruct<T>(JsonText, &OutValue))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.DeserializeFailed: the save record of mod '%s' is not a valid '%s'."),
			*ModId.ToString(), *T::StaticStruct()->GetName());
		return false;
	}

	OutDataVersion = RecordDataVersion;
	return true;
}
