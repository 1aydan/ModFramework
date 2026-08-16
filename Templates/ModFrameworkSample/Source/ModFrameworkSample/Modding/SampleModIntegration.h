// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SampleModIntegration.generated.h"

class UGameCombatModAPI;
class UGameUIModAPI;
class UGameWorldModAPI;

/**
 * Registers this game's public modding surface with the framework.
 *
 * This is the entire game-developer integration: create the API instances, hand the framework the
 * extension point and event descriptors the SDK declares, and get out of the way. Everything a mod
 * can do flows through what is registered here, so this file is the honest answer to "what can mods
 * do to my game?"
 *
 * Ordering matters. UModSubsystem is also a UGameInstanceSubsystem, and subsystem initialisation
 * order is otherwise unspecified, so Initialize() declares the dependency explicitly.
 */
UCLASS()
class USampleModIntegrationSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem

	/** True once the extension points, events and APIs were all registered successfully. */
	UFUNCTION(BlueprintPure, Category = "Modding")
	bool IsModdingAvailable() const { return bRegistered; }

private:
	UPROPERTY(Transient)
	TObjectPtr<UGameCombatModAPI> CombatAPI;

	UPROPERTY(Transient)
	TObjectPtr<UGameWorldModAPI> WorldAPI;

	UPROPERTY(Transient)
	TObjectPtr<UGameUIModAPI> UIAPI;

	bool bRegistered = false;
};
