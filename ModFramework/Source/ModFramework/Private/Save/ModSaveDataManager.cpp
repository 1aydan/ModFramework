// Copyright (c) 2026. Licensed for use in your own projects.

#include "Save/ModSaveDataManager.h"

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "Core/ModFrameworkVersion.h"
#include "CoreTypes.h"
#include "GameFramework/SaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "Permissions/ModPermissionRegistry.h"
#include "Permissions/ModPermissions.h"
#include "Registry/ModInfo.h"
#include "Registry/ModRegistry.h"
#include "Save/ModSaveGame.h"
#include "Save/ModSaveTypes.h"
#include "Settings/ModFrameworkSettings.h"
#include "Subsystem/ModSubsystem.h"
#include "Templates/Casts.h"
#include "Templates/SubclassOf.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ScriptInterface.h"
#include "UObject/WeakObjectPtr.h"

namespace ModSaveDataManagerPrivate
{
	/**
	 * Renders a mod id for a log line. An id that is somehow empty still has to print as something,
	 * because a log line that reads "mod '' was refused" is useless when diagnosing a broken mod.
	 */
	FString DescribeModId(const FModId& ModId)
	{
		return ModId.IsValid() ? ModId.ToString() : FString(TEXT("<invalid>"));
	}
}

void UModSaveDataManager::Initialize(UModSubsystem* InSubsystem)
{
	OwningSubsystem = InSubsystem;
	Migrations.Reset();
	ResetEnvelope();

	UE_LOG(LogModFramework, Verbose, TEXT("Save: data manager initialised (%s)."),
		InSubsystem ? TEXT("bound to the mod subsystem") : TEXT("standalone, permissions are not enforced"));
}

void UModSaveDataManager::Shutdown()
{
	UE_LOG(LogModFramework, Verbose, TEXT("Save: data manager shutting down, releasing %d record(s) and %d migration(s)."),
		Envelope.Records.Num(), Migrations.Num());

	Migrations.Reset();
	Envelope = FModSaveEnvelope();
	OwningSubsystem.Reset();
}

void UModSaveDataManager::ResetEnvelope()
{
	Envelope = FModSaveEnvelope();
	Envelope.EnvelopeVersion = FModSaveEnvelope::CurrentEnvelopeVersion;
	Envelope.FrameworkVersion = ModFrameworkVersion::Get();

	if (const UModFrameworkSettings* Settings = UModFrameworkSettings::Get())
	{
		Envelope.GameId = Settings->GameId;
	}
}

void UModSaveDataManager::SetEnvelope(const FModSaveEnvelope& In)
{
	Envelope = In;

	if (Envelope.EnvelopeVersion > FModSaveEnvelope::CurrentEnvelopeVersion)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.EnvelopeFromFuture: this envelope declares version %d but this framework understands %d. ")
			TEXT("Its records are kept untouched so a newer build can still read them."),
			Envelope.EnvelopeVersion, FModSaveEnvelope::CurrentEnvelopeVersion);
	}

	// Duplicate records are pathological - only a hand-edited or corrupted save produces them - but
	// they are still somebody's data, so they are reported and kept rather than silently collapsed.
	// Every lookup resolves to the first match, which leaves the later copies inert but intact.
	for (int32 Index = 0; Index < Envelope.Records.Num(); ++Index)
	{
		const FModSaveRecord& Record = Envelope.Records[Index];
		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			if (Envelope.Records[Earlier].ModId == Record.ModId)
			{
				UE_LOG(LogModFramework, Warning,
					TEXT("Save.DuplicateRecord: mod '%s' has more than one save record. The first is used; ")
					TEXT("the others are preserved but ignored."),
					*ModSaveDataManagerPrivate::DescribeModId(Record.ModId));
				break;
			}
		}
	}

	if (const UModFrameworkSettings* Settings = UModFrameworkSettings::Get())
	{
		if (!Envelope.GameId.IsEmpty() && !Settings->GameId.IsEmpty() && Envelope.GameId != Settings->GameId)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("Save.GameIdMismatch: the envelope was written by game '%s' but this game is '%s'. ")
				TEXT("Loading it anyway - the game decides whether that is acceptable."),
				*Envelope.GameId, *Settings->GameId);
		}
	}

	UE_LOG(LogModFramework, Verbose, TEXT("Save: envelope replaced (%d record(s), %d stamped mod(s))."),
		Envelope.Records.Num(), Envelope.RequiredMods.Num());
}

const FModSaveEnvelope& UModSaveDataManager::GetEnvelope() const
{
	return Envelope;
}

FModSaveEnvelope UModSaveDataManager::GetEnvelopeCopy() const
{
	return Envelope;
}

void UModSaveDataManager::StampRequiredMods()
{
	Envelope.EnvelopeVersion = FModSaveEnvelope::CurrentEnvelopeVersion;
	Envelope.FrameworkVersion = ModFrameworkVersion::Get();

	if (const UModFrameworkSettings* Settings = UModFrameworkSettings::Get())
	{
		if (!Settings->GameId.IsEmpty())
		{
			Envelope.GameId = Settings->GameId;
		}
	}

	Envelope.RequiredMods.Reset();

	UModRegistry* Registry = GetRegistry();
	if (!Registry)
	{
		UE_LOG(LogModFramework, Verbose,
			TEXT("Save: StampRequiredMods found no mod registry; the envelope lists no mods."));
		return;
	}

	const TArray<FModInfo> ActiveMods = Registry->GetActiveMods();
	Envelope.RequiredMods.Reserve(ActiveMods.Num());

	for (const FModInfo& Info : ActiveMods)
	{
		const FModId& ModId = Info.Manifest.Id;
		if (!ModId.IsValid())
		{
			// An active mod with no id cannot be matched back on load, so stamping it would only
			// produce an unresolvable "missing mod" entry later.
			UE_LOG(LogModFramework, Warning,
				TEXT("Save.UnstampableMod: an active mod has no id and was left out of the save stamp."));
			continue;
		}

		FModSaveDependency& Entry = Envelope.RequiredMods.AddDefaulted_GetRef();
		Entry.ModId = ModId;
		Entry.Version = Info.Manifest.Version;
		Entry.DisplayName = Info.Manifest.GetDisplayNameOrId();
		Entry.bWasRequired = Info.Manifest.bRequiredForNetworkPlay;
	}

	// Sorted by id so two saves written with the same mods produce byte-identical stamps.
	Envelope.RequiredMods.Sort([](const FModSaveDependency& A, const FModSaveDependency& B)
	{
		return A.ModId < B.ModId;
	});

	UE_LOG(LogModFramework, Verbose, TEXT("Save: stamped %d active mod(s) into the envelope."),
		Envelope.RequiredMods.Num());
}

bool UModSaveDataManager::WriteModJson(const FModId& ModId, const FString& Json, int32 DataVersion)
{
	if (!ValidateModId(ModId, TEXT("write JSON save data")))
	{
		return false;
	}

	if (!CheckWritePermission(ModId, TEXT("write JSON save data")))
	{
		return false;
	}

	if (DataVersion < 1)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.InvalidDataVersion: mod '%s' tried to write JSON at data version %d; versions start at 1."),
			*ModSaveDataManagerPrivate::DescribeModId(ModId), DataVersion);
		return false;
	}

	if (static_cast<int64>(Json.Len()) > MaxJsonPayloadChars)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.PayloadTooLarge: mod '%s' tried to write a %lld character JSON payload; the limit is %lld. ")
			TEXT("The write was refused and the previous payload is unchanged."),
			*ModSaveDataManagerPrivate::DescribeModId(ModId),
			static_cast<int64>(Json.Len()), MaxJsonPayloadChars);
		return false;
	}

	// Copied before the record array can grow: GetEnvelope hands out a reference to the live envelope,
	// so a caller is able - however unwisely - to pass a string that lives inside Records itself, and
	// FindOrAddRecord may reallocate that array out from under it.
	FString Payload = Json;

	FModSaveRecord& Record = Envelope.FindOrAddRecord(ModId);
	Record.Json = MoveTemp(Payload);
	Record.DataVersion = DataVersion;
	Record.bOrphaned = false;

	const FModVersion Resolved = ResolveModVersion(ModId);
	if (!Resolved.IsZero())
	{
		Record.ModVersion = Resolved;
	}

	UE_LOG(LogModFramework, VeryVerbose, TEXT("Save: mod '%s' wrote %d JSON character(s) at data version %d."),
		*ModId.ToString(), Record.Json.Len(), DataVersion);
	return true;
}

bool UModSaveDataManager::ReadModJson(const FModId& ModId, FString& OutJson, int32& OutDataVersion) const
{
	OutJson.Reset();
	OutDataVersion = 0;

	const FModSaveRecord* Record = Envelope.FindRecord(ModId);
	if (!Record || !Record->HasJson())
	{
		return false;
	}

	OutJson = Record->Json;
	OutDataVersion = Record->DataVersion;
	return true;
}

bool UModSaveDataManager::WriteModBytes(const FModId& ModId, const TArray<uint8>& Bytes, int32 DataVersion)
{
	if (!ValidateModId(ModId, TEXT("write binary save data")))
	{
		return false;
	}

	if (!CheckWritePermission(ModId, TEXT("write binary save data")))
	{
		return false;
	}

	if (DataVersion < 1)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.InvalidDataVersion: mod '%s' tried to write bytes at data version %d; versions start at 1."),
			*ModSaveDataManagerPrivate::DescribeModId(ModId), DataVersion);
		return false;
	}

	if (static_cast<int64>(Bytes.Num()) > MaxBinaryPayloadBytes)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.PayloadTooLarge: mod '%s' tried to write a %lld byte binary payload; the limit is %lld. ")
			TEXT("The write was refused and the previous payload is unchanged."),
			*ModSaveDataManagerPrivate::DescribeModId(ModId),
			static_cast<int64>(Bytes.Num()), MaxBinaryPayloadBytes);
		return false;
	}

	// Copied for the same reason as in WriteModJson: the source may alias into Envelope.Records, which
	// FindOrAddRecord is allowed to reallocate.
	TArray<uint8> Payload = Bytes;

	FModSaveRecord& Record = Envelope.FindOrAddRecord(ModId);
	Record.Binary = MoveTemp(Payload);
	Record.DataVersion = DataVersion;
	Record.bOrphaned = false;

	const FModVersion Resolved = ResolveModVersion(ModId);
	if (!Resolved.IsZero())
	{
		Record.ModVersion = Resolved;
	}

	UE_LOG(LogModFramework, VeryVerbose, TEXT("Save: mod '%s' wrote %d byte(s) at data version %d."),
		*ModId.ToString(), Record.Binary.Num(), DataVersion);
	return true;
}

bool UModSaveDataManager::ReadModBytes(const FModId& ModId, TArray<uint8>& OutBytes, int32& OutDataVersion) const
{
	OutBytes.Reset();
	OutDataVersion = 0;

	const FModSaveRecord* Record = Envelope.FindRecord(ModId);
	if (!Record || !Record->HasBinary())
	{
		return false;
	}

	OutBytes = Record->Binary;
	OutDataVersion = Record->DataVersion;
	return true;
}

bool UModSaveDataManager::ClearModData(const FModId& ModId)
{
	if (!ValidateModId(ModId, TEXT("clear save data")))
	{
		return false;
	}

	if (!CheckWritePermission(ModId, TEXT("clear save data")))
	{
		return false;
	}

	FModSaveRecord* Record = Envelope.FindRecordMutable(ModId);
	if (!Record)
	{
		// Nothing to clear is not a failure: the mod already has no data.
		return true;
	}

	Record->Json.Empty();
	Record->Binary.Empty();
	Record->DataVersion = 1;

	UE_LOG(LogModFramework, Log, TEXT("Save: cleared the save payload of mod '%s'. The record itself is kept."),
		*ModId.ToString());
	return true;
}

TArray<FModSaveDependency> UModSaveDataManager::GetMissingMods() const
{
	TArray<FModSaveDependency> Missing;

	if (!GetRegistry())
	{
		// With nothing to ask, "missing" cannot be distinguished from "unknown". Reporting every
		// stamped mod as missing would be a lie, so report none.
		return Missing;
	}

	for (const FModSaveDependency& Dependency : Envelope.RequiredMods)
	{
		if (!IsModPresent(Dependency.ModId))
		{
			Missing.Add(Dependency);
		}
	}

	return Missing;
}

TArray<FModSaveDependency> UModSaveDataManager::GetMissingRequiredMods() const
{
	TArray<FModSaveDependency> Required;

	for (const FModSaveDependency& Dependency : GetMissingMods())
	{
		if (Dependency.bWasRequired)
		{
			Required.Add(Dependency);
		}
	}

	return Required;
}

TArray<FModSaveRecord> UModSaveDataManager::GetOrphanedRecords() const
{
	TArray<FModSaveRecord> Orphans;

	for (const FModSaveRecord& Record : Envelope.Records)
	{
		if (Record.bOrphaned)
		{
			Orphans.Add(Record);
		}
	}

	return Orphans;
}

void UModSaveDataManager::MarkOrphanedRecords()
{
	if (!GetRegistry())
	{
		UE_LOG(LogModFramework, Verbose,
			TEXT("Save: MarkOrphanedRecords has no mod registry to ask; every orphan flag is left as it was."));
		return;
	}

	int32 OrphanCount = 0;

	for (FModSaveRecord& Record : Envelope.Records)
	{
		const bool bIsOrphanNow = !IsModPresent(Record.ModId);

		if (bIsOrphanNow != Record.bOrphaned)
		{
			Record.bOrphaned = bIsOrphanNow;

			if (bIsOrphanNow)
			{
				UE_LOG(LogModFramework, Warning,
					TEXT("Save.Orphaned: mod '%s' is not loaded but has save data. Its record is preserved ")
					TEXT("untouched and will survive every save from here on."),
					*ModSaveDataManagerPrivate::DescribeModId(Record.ModId));
			}
			else
			{
				UE_LOG(LogModFramework, Log,
					TEXT("Save: mod '%s' is back; its previously orphaned save record is live again."),
					*ModSaveDataManagerPrivate::DescribeModId(Record.ModId));
			}
		}

		if (bIsOrphanNow)
		{
			++OrphanCount;
		}
	}

	UE_LOG(LogModFramework, Verbose, TEXT("Save: %d of %d record(s) are orphaned."),
		OrphanCount, Envelope.Records.Num());
}

bool UModSaveDataManager::PurgeOrphanedRecord(const FModId& ModId)
{
	if (!ModId.IsValid())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Save.InvalidModId: cannot purge a record without a mod id."));
		return false;
	}

	int32 TotalForMod = 0;
	int32 OrphansForMod = 0;
	for (const FModSaveRecord& Record : Envelope.Records)
	{
		if (Record.ModId == ModId)
		{
			++TotalForMod;
			if (Record.bOrphaned)
			{
				++OrphansForMod;
			}
		}
	}

	if (TotalForMod == 0)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Save: nothing to purge, mod '%s' has no save record."),
			*ModId.ToString());
		return false;
	}

	if (OrphansForMod == 0)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.PurgeRefused: the record of mod '%s' is not orphaned. Only data belonging to an absent ")
			TEXT("mod can be purged, so live mod data is never destroyed by accident."),
			*ModId.ToString());
		return false;
	}

	const int32 Removed = Envelope.Records.RemoveAll([&ModId](const FModSaveRecord& Record)
	{
		return Record.ModId == ModId && Record.bOrphaned;
	});

	UE_LOG(LogModFramework, Warning,
		TEXT("Save: purged %d orphaned save record(s) for mod '%s'. This is irreversible - reinstalling the mod ")
		TEXT("will not bring the data back."),
		Removed, *ModId.ToString());

	// Once no record for the mod remains, its stamp entry is meaningless: keeping it would report the
	// mod as missing forever even though the player deliberately let go of its data.
	const bool bAnyRecordLeft = Envelope.Records.ContainsByPredicate([&ModId](const FModSaveRecord& Record)
	{
		return Record.ModId == ModId;
	});

	if (!bAnyRecordLeft)
	{
		const int32 RemovedStamps = Envelope.RequiredMods.RemoveAll([&ModId](const FModSaveDependency& Dependency)
		{
			return Dependency.ModId == ModId;
		});

		if (RemovedStamps > 0)
		{
			UE_LOG(LogModFramework, Log, TEXT("Save: also dropped the stamp entry for purged mod '%s'."),
				*ModId.ToString());
		}
	}

	return Removed > 0;
}

void UModSaveDataManager::RegisterMigration(const FModId& ModId, TScriptInterface<IModSaveMigration> Migration)
{
	if (!ModId.IsValid())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.InvalidModId: a save migration was offered without a mod id and was ignored."));
		return;
	}

	UObject* MigrationObject = Migration.GetObject();
	if (!MigrationObject)
	{
		if (Migrations.Remove(ModId) > 0)
		{
			UE_LOG(LogModFramework, Verbose, TEXT("Save: cleared the registered save migration of mod '%s'."),
				*ModId.ToString());
		}
		return;
	}

	if (!MigrationObject->GetClass()->ImplementsInterface(UModSaveMigration::StaticClass()))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.MigrationInvalid: '%s' was offered as the save migration of mod '%s' but does not ")
			TEXT("implement IModSaveMigration. It was ignored."),
			*MigrationObject->GetName(), *ModId.ToString());
		return;
	}

	// Rebuilt from the object so the native interface pointer is filled in for native implementers and
	// left null for Blueprint ones, which is exactly what Execute_MigrateModSave expects.
	Migrations.Add(ModId, TScriptInterface<IModSaveMigration>(MigrationObject));

	UE_LOG(LogModFramework, Verbose, TEXT("Save: registered '%s' as the save migration of mod '%s'."),
		*MigrationObject->GetName(), *ModId.ToString());
}

bool UModSaveDataManager::UnregisterMigration(const FModId& ModId)
{
	return Migrations.Remove(ModId) > 0;
}

bool UModSaveDataManager::MigrateRecord(const FModId& ModId, int32 TargetVersion)
{
	if (!ValidateModId(ModId, TEXT("migrate save data")))
	{
		return false;
	}

	if (TargetVersion < 1)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.InvalidDataVersion: mod '%s' was asked to migrate to data version %d; versions start at 1."),
			*ModId.ToString(), TargetVersion);
		return false;
	}

	const FModSaveRecord* Existing = Envelope.FindRecord(ModId);
	if (!Existing)
	{
		UE_LOG(LogModFramework, Verbose,
			TEXT("Save: mod '%s' has no save record, so there is nothing to migrate to version %d."),
			*ModId.ToString(), TargetVersion);
		return false;
	}

	const int32 StartVersion = Existing->DataVersion;

	if (StartVersion == TargetVersion)
	{
		return true;
	}

	if (StartVersion > TargetVersion)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.MigrationDownward: the record of mod '%s' is at data version %d and version %d was asked ")
			TEXT("for. The framework never migrates downwards; the record is left untouched."),
			*ModId.ToString(), StartVersion, TargetVersion);
		return false;
	}

	if (TargetVersion - StartVersion > MaxMigrationSteps)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.MigrationTooManySteps: migrating mod '%s' from data version %d to %d would take %d steps ")
			TEXT("and the limit is %d. The record is left untouched."),
			*ModId.ToString(), StartVersion, TargetVersion, TargetVersion - StartVersion, MaxMigrationSteps);
		return false;
	}

	TScriptInterface<IModSaveMigration>* Registered = Migrations.Find(ModId);
	UObject* MigrationObject = Registered ? Registered->GetObject() : nullptr;

	if (!IsValid(MigrationObject))
	{
		if (Registered)
		{
			// A migration whose object has gone away is dead weight; drop it so the next attempt gives
			// the same clean "nothing registered" diagnostic.
			Migrations.Remove(ModId);
		}

		UE_LOG(LogModFramework, Warning,
			TEXT("Save.MigrationMissing: mod '%s' has save data at version %d but no usable migration is ")
			TEXT("registered, so it cannot be brought to version %d. The record is left untouched."),
			*ModId.ToString(), StartVersion, TargetVersion);
		return false;
	}

	if (!MigrationObject->GetClass()->ImplementsInterface(UModSaveMigration::StaticClass()))
	{
		Migrations.Remove(ModId);

		UE_LOG(LogModFramework, Warning,
			TEXT("Save.MigrationInvalid: the migration registered for mod '%s' ('%s') no longer implements ")
			TEXT("IModSaveMigration. The record is left untouched."),
			*ModId.ToString(), *MigrationObject->GetName());
		return false;
	}

	// The walk runs on a copy. Mod code is untrusted and may fail, may mutate the record in ways that
	// break the size caps, and may even re-enter the manager and reallocate the record array - none of
	// which is allowed to leave a half-migrated record in the envelope.
	FModSaveRecord Working = *Existing;
	Existing = nullptr;

	for (int32 FromVersion = StartVersion; FromVersion < TargetVersion; ++FromVersion)
	{
		const int32 ToVersion = FromVersion + 1;

		if (!IModSaveMigration::Execute_MigrateModSave(MigrationObject, ModId, FromVersion, ToVersion, Working))
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("Save.MigrationFailed: the migration of mod '%s' refused the step %d -> %d. The stored ")
				TEXT("record stays at version %d."),
				*ModId.ToString(), FromVersion, ToVersion, StartVersion);
			return false;
		}

		// The migration owns the payload, never the bookkeeping: identity and version are restored so a
		// careless implementation cannot rename the record or claim an arbitrary version.
		Working.ModId = ModId;
		Working.DataVersion = ToVersion;
	}

	if (!ValidatePayloadSizes(ModId, Working, TEXT("migration")))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.MigrationFailed: the migration of mod '%s' produced an oversized payload. The stored ")
			TEXT("record stays at version %d."),
			*ModId.ToString(), StartVersion);
		return false;
	}

	// Re-found rather than cached: a migration is allowed to have re-entered the manager, which can
	// have reallocated (or, in a pathological case, removed) the record array.
	FModSaveRecord* Target = Envelope.FindRecordMutable(ModId);
	if (!Target)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.RecordVanished: the save record of mod '%s' disappeared while its migration ran; ")
			TEXT("the migrated data was discarded rather than re-inserted."),
			*ModId.ToString());
		return false;
	}

	*Target = Working;

	UE_LOG(LogModFramework, Log, TEXT("Save: migrated the record of mod '%s' from data version %d to %d."),
		*ModId.ToString(), StartVersion, TargetVersion);
	return true;
}

bool UModSaveDataManager::SaveToSlot(const FString& SlotName, int32 UserIndex)
{
	if (SlotName.IsEmpty())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Save.InvalidSlot: cannot write a save to an empty slot name."));
		return false;
	}

	UModSaveGame* SaveGame = Cast<UModSaveGame>(UGameplayStatics::CreateSaveGameObject(UModSaveGame::StaticClass()));
	if (!SaveGame)
	{
		UE_LOG(LogModFramework, Error,
			TEXT("Save.SlotWriteFailed: could not create a UModSaveGame for slot '%s'."), *SlotName);
		return false;
	}

	SaveGame->Envelope = Envelope;

	if (!UGameplayStatics::SaveGameToSlot(SaveGame, SlotName, UserIndex))
	{
		UE_LOG(LogModFramework, Error,
			TEXT("Save.SlotWriteFailed: the platform save system rejected slot '%s' (user %d)."),
			*SlotName, UserIndex);
		return false;
	}

	UE_LOG(LogModFramework, Log, TEXT("Save: wrote %d mod record(s) to slot '%s' (user %d)."),
		Envelope.Records.Num(), *SlotName, UserIndex);
	return true;
}

bool UModSaveDataManager::LoadFromSlot(const FString& SlotName, int32 UserIndex)
{
	if (SlotName.IsEmpty())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Save.InvalidSlot: cannot read a save from an empty slot name."));
		return false;
	}

	if (!UGameplayStatics::DoesSaveGameExist(SlotName, UserIndex))
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Save: slot '%s' (user %d) does not exist."), *SlotName, UserIndex);
		return false;
	}

	USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(SlotName, UserIndex);
	if (!Loaded)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.SlotReadFailed: slot '%s' (user %d) exists but could not be read. The current envelope ")
			TEXT("is unchanged."),
			*SlotName, UserIndex);
		return false;
	}

	UModSaveGame* ModSaveGame = Cast<UModSaveGame>(Loaded);
	if (!ModSaveGame)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.SlotWrongType: slot '%s' holds a '%s', not a UModSaveGame. The current envelope is ")
			TEXT("unchanged. A game with its own save class should call SetEnvelope instead."),
			*SlotName, *Loaded->GetClass()->GetName());
		return false;
	}

	SetEnvelope(ModSaveGame->Envelope);
	MarkOrphanedRecords();

	UE_LOG(LogModFramework, Log, TEXT("Save: read %d mod record(s) from slot '%s' (user %d)."),
		Envelope.Records.Num(), *SlotName, UserIndex);
	return true;
}

bool UModSaveDataManager::ValidateModId(const FModId& ModId, const TCHAR* Operation) const
{
	if (ModId.IsValid())
	{
		return true;
	}

	UE_LOG(LogModFramework, Warning,
		TEXT("Save.InvalidModId: a mod with no id tried to %s. The call was refused."), Operation);
	return false;
}

bool UModSaveDataManager::CheckWritePermission(const FModId& ModId, const TCHAR* Operation) const
{
	UModSubsystem* Subsystem = OwningSubsystem.Get();
	if (!Subsystem)
	{
		// Standalone, which in practice means automation tests: there is no permission registry to ask,
		// so refusing everything would make the manager untestable. Allow and stay silent.
		return true;
	}

	UModPermissionRegistry* PermissionRegistry = Subsystem->GetPermissionRegistry();
	if (!PermissionRegistry)
	{
		return true;
	}

	if (PermissionRegistry->HasPermission(ModId, ModPermissions::SaveModify))
	{
		return true;
	}

	UE_LOG(LogModFramework, Warning,
		TEXT("Save.PermissionDenied: mod '%s' tried to %s without holding the '%s' permission."),
		*ModSaveDataManagerPrivate::DescribeModId(ModId), Operation, *ModPermissions::SaveModify.ToString());
	return false;
}

bool UModSaveDataManager::ValidatePayloadSizes(const FModId& ModId, const FModSaveRecord& Record, const TCHAR* Operation) const
{
	if (static_cast<int64>(Record.Json.Len()) > MaxJsonPayloadChars)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.PayloadTooLarge: %s of mod '%s' produced a %lld character JSON payload; the limit is %lld."),
			Operation, *ModSaveDataManagerPrivate::DescribeModId(ModId),
			static_cast<int64>(Record.Json.Len()), MaxJsonPayloadChars);
		return false;
	}

	if (static_cast<int64>(Record.Binary.Num()) > MaxBinaryPayloadBytes)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Save.PayloadTooLarge: %s of mod '%s' produced a %lld byte binary payload; the limit is %lld."),
			Operation, *ModSaveDataManagerPrivate::DescribeModId(ModId),
			static_cast<int64>(Record.Binary.Num()), MaxBinaryPayloadBytes);
		return false;
	}

	return true;
}

UModRegistry* UModSaveDataManager::GetRegistry() const
{
	UModSubsystem* Subsystem = OwningSubsystem.Get();
	return Subsystem ? Subsystem->GetRegistry() : nullptr;
}

bool UModSaveDataManager::IsModPresent(const FModId& ModId) const
{
	UModRegistry* Registry = GetRegistry();
	if (!Registry)
	{
		return false;
	}

	FModInfo Info;
	if (!Registry->GetModInfo(ModId, Info))
	{
		return false;
	}

	return Info.IsLoaded() || Info.IsActive();
}

FModVersion UModSaveDataManager::ResolveModVersion(const FModId& ModId) const
{
	UModRegistry* Registry = GetRegistry();
	if (!Registry)
	{
		return FModVersion();
	}

	FModInfo Info;
	if (!Registry->GetModInfo(ModId, Info))
	{
		return FModVersion();
	}

	return Info.Manifest.Version;
}
