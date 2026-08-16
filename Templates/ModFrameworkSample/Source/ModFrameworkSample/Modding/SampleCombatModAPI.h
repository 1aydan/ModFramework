// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"
#include "API/GameCombatModAPI.h"
#include "SampleCombatModAPI.generated.h"

/**
 * The sample game's implementation of the combat modding API.
 *
 * UGameCombatModAPI lives in GameModSDK and declares *what* mods may do; it has no idea how this
 * game stores health or applies damage, and it must not - a mod author compiles against the SDK
 * without ever seeing this file. This subclass is the other half: it lives in the game module,
 * knows about ACombatCharacter / ACombatEnemy / ICombatDamageable, and is registered with
 * UModSubsystem at startup.
 *
 * That split is the whole point of the SDK boundary. If you find yourself wanting to move any of
 * this into the SDK, you are about to break the ability to ship an SDK without game source.
 */
UCLASS(NotBlueprintable)
class USampleCombatModAPI : public UGameCombatModAPI
{
	GENERATED_BODY()

public:
	//~ Begin UGameCombatModAPI
	virtual bool ApplyDamage(AActor* Target, float Amount, FName DamageType) override;
	virtual float GetHealth(AActor* Target) const override;
	virtual float GetMaxHealth(AActor* Target) const override;
	virtual bool IsAlive(AActor* Target) const override;
	virtual APawn* GetPlayerPawn(int32 PlayerIndex) const override;
	//~ End UGameCombatModAPI

private:
	/**
	 * Reads HP off whichever combat actor this is.
	 *
	 * ACombatCharacter and ACombatEnemy both carry CurrentHP/MaxHP but share no common base that
	 * exposes them, so this has to check each concretely. Returns false for anything else rather
	 * than guessing.
	 */
	static bool TryGetHitPoints(const AActor* Target, float& OutCurrent, float& OutMax);
};
