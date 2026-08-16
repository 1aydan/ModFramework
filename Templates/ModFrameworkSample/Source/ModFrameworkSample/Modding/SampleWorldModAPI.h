// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"
#include "API/GameWorldModAPI.h"
#include "SampleWorldModAPI.generated.h"

class UGameEnemyDefinition;

/**
 * The sample game's implementation of the world modding API.
 *
 * Notable: SpawnEnemy resolves an enemy id by asking the framework's extension registry for the
 * registered "game.enemy" extensions and matching against the UGameEnemyDefinition each one
 * supplies. That means a mod can add a genuinely new enemy type - define it in a Data Asset,
 * register an extension, and it becomes spawnable by id - without this class knowing anything
 * about that mod.
 */
UCLASS(NotBlueprintable)
class USampleWorldModAPI : public UGameWorldModAPI
{
	GENERATED_BODY()

public:
	//~ Begin UGameWorldModAPI
	virtual AActor* SpawnEnemy(FName EnemyId, const FTransform& SpawnTransform) override;
	virtual bool DespawnEnemy(AActor* Enemy) override;
	virtual int32 GetActiveEnemyCount() const override;
	virtual int32 GetCurrentWave() const override;
	//~ End UGameWorldModAPI

private:
	/** Walks registered game.enemy extensions looking for a definition whose asset name matches. */
	UGameEnemyDefinition* FindEnemyDefinition(FName EnemyId) const;
};
