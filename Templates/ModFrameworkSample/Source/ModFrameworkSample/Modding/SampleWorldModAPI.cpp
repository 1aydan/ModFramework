// Copyright (c) 2026. Licensed for use in your own projects.

#include "SampleWorldModAPI.h"

#include "CombatEnemy.h"
#include "Data/GameEnemyDefinition.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Extensions/GameEnemyExtension.h"
#include "Extensions/GameModSDKExtensionPoints.h"
#include "Extensions/ModExtensionRegistry.h"
#include "Logging/LogMacros.h"
#include "Subsystem/ModSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSampleWorldModding, Log, All);

UGameEnemyDefinition* USampleWorldModAPI::FindEnemyDefinition(FName EnemyId) const
{
	UModSubsystem* Subsystem = UModSubsystem::Get(this);
	if (Subsystem == nullptr)
	{
		return nullptr;
	}

	UModExtensionRegistry* Registry = Subsystem->GetExtensionRegistry();
	if (Registry == nullptr)
	{
		return nullptr;
	}

	// GetExtensions returns only active extensions, already ordered by (priority, load order, id).
	// Taking the first match therefore honours the same precedence the framework uses everywhere
	// else, which is what a mod author will expect when two mods define the same enemy id.
	for (UModExtension* Extension : Registry->GetExtensions(GameModExtensionPoints::Enemy))
	{
		const UGameEnemyExtension* EnemyExtension = Cast<UGameEnemyExtension>(Extension);
		if (EnemyExtension == nullptr)
		{
			continue;
		}

		UGameEnemyDefinition* Definition = EnemyExtension->NativeGetEnemyDefinition();
		if (Definition == nullptr)
		{
			continue;
		}

		// The definition asset's own name is its id. Data assets have stable names and a mod author
		// already thinks of "DA_Grunt" as the thing they made, so this needs no extra id field to
		// keep in sync.
		if (Definition->GetFName() == EnemyId)
		{
			return Definition;
		}
	}

	return nullptr;
}

AActor* USampleWorldModAPI::SpawnEnemy(FName EnemyId, const FTransform& SpawnTransform)
{
	if (!VerifyServerAuthority(this, TEXT("SpawnEnemy")))
	{
		return nullptr;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	UGameEnemyDefinition* Definition = FindEnemyDefinition(EnemyId);
	if (Definition == nullptr)
	{
		UE_LOG(LogSampleWorldModding, Warning,
			TEXT("SpawnEnemy: no registered game.enemy extension supplies a definition named '%s'."),
			*EnemyId.ToString());
		return nullptr;
	}

	// TSoftClassPtr, so the class may not be in memory yet. This is a deliberate synchronous load:
	// spawning is a discrete gameplay action a mod asked for right now, and returning null while an
	// async load completes would be a worse API. The class is small; the content it references is
	// what is heavy, and that stays soft.
	UClass* EnemyClass = Definition->EnemyClass.LoadSynchronous();
	if (EnemyClass == nullptr)
	{
		UE_LOG(LogSampleWorldModding, Warning,
			TEXT("SpawnEnemy: definition '%s' has no loadable EnemyClass."), *EnemyId.ToString());
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	AActor* Spawned = World->SpawnActor<AActor>(EnemyClass, SpawnTransform, Params);
	if (Spawned == nullptr)
	{
		return nullptr;
	}

	// Let every registered enemy extension react - this is how a mod applies its own modifiers to
	// an enemy another mod spawned.
	if (UModSubsystem* Subsystem = UModSubsystem::Get(this))
	{
		if (UModExtensionRegistry* Registry = Subsystem->GetExtensionRegistry())
		{
			for (UModExtension* Extension : Registry->GetExtensions(GameModExtensionPoints::Enemy))
			{
				if (UGameEnemyExtension* EnemyExtension = Cast<UGameEnemyExtension>(Extension))
				{
					EnemyExtension->NativeOnEnemySpawned(Spawned);
				}
			}
		}
	}

	return Spawned;
}

bool USampleWorldModAPI::DespawnEnemy(AActor* Enemy)
{
	if (!VerifyServerAuthority(Enemy, TEXT("DespawnEnemy")))
	{
		return false;
	}

	// Scoped to enemies on purpose. A world API that despawns *any* actor lets a mod delete the
	// player, the game mode's helpers, or level geometry.
	if (!IsValid(Enemy) || !Enemy->IsA<ACombatEnemy>())
	{
		return false;
	}

	Enemy->Destroy();
	return true;
}

int32 USampleWorldModAPI::GetActiveEnemyCount() const
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0;
	}

	int32 Count = 0;
	for (TActorIterator<ACombatEnemy> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			++Count;
		}
	}

	return Count;
}

int32 USampleWorldModAPI::GetCurrentWave() const
{
	// This sample game has no wave system - ACombatEnemySpawner spawns continuously rather than in
	// waves. Returning 0 is a documented "not modelled" answer rather than invented state; the
	// Game.WaveStarted event exists in the SDK for games that do have one.
	return 0;
}
