// Copyright (c) 2026. Licensed for use in your own projects.

#include "SampleCombatModAPI.h"

#include "CombatCharacter.h"
#include "CombatDamageable.h"
#include "CombatEnemy.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogSampleModding, Log, All);

bool USampleCombatModAPI::TryGetHitPoints(const AActor* Target, float& OutCurrent, float& OutMax)
{
	if (const ACombatCharacter* Character = Cast<ACombatCharacter>(Target))
	{
		OutCurrent = Character->GetCurrentHP();
		OutMax = Character->GetMaxHP();
		return true;
	}

	if (const ACombatEnemy* Enemy = Cast<ACombatEnemy>(Target))
	{
		OutCurrent = Enemy->CurrentHP;
		OutMax = Enemy->GetMaxHP();
		return true;
	}

	return false;
}

bool USampleCombatModAPI::ApplyDamage(AActor* Target, float Amount, FName DamageType)
{
	// Declared server-authoritative in the SDK, so refuse on clients. A mod driving damage from a
	// client is the single most common way a modded game desyncs.
	if (!VerifyServerAuthority(Target, TEXT("ApplyDamage")))
	{
		return false;
	}

	if (!IsValid(Target))
	{
		return false;
	}

	// Negative damage would be a backdoor heal that bypasses whatever rules ApplyHealing enforces.
	if (!(Amount > 0.0f) || !FMath::IsFinite(Amount))
	{
		UE_LOG(LogSampleModding, Warning,
			TEXT("A mod called ApplyDamage with an invalid amount (%f) on '%s'; ignoring."),
			Amount, *Target->GetName());
		return false;
	}

	ICombatDamageable* Damageable = Cast<ICombatDamageable>(Target);
	if (Damageable == nullptr)
	{
		UE_LOG(LogSampleModding, Verbose,
			TEXT("ApplyDamage: '%s' does not implement ICombatDamageable."), *Target->GetName());
		return false;
	}

	// The game's interface has no damage-type concept. Rather than silently dropping the argument,
	// say so once at verbose level so a mod author can see why their damage type did nothing.
	if (!DamageType.IsNone())
	{
		UE_LOG(LogSampleModding, Verbose,
			TEXT("ApplyDamage: damage type '%s' is not modelled by this game and was ignored."),
			*DamageType.ToString());
	}

	Damageable->ApplyDamage(Amount, /*DamageCauser*/ nullptr, Target->GetActorLocation(), FVector::ZeroVector);
	return true;
}

float USampleCombatModAPI::GetHealth(AActor* Target) const
{
	float Current = 0.0f;
	float Max = 0.0f;
	return TryGetHitPoints(Target, Current, Max) ? Current : 0.0f;
}

float USampleCombatModAPI::GetMaxHealth(AActor* Target) const
{
	float Current = 0.0f;
	float Max = 0.0f;
	return TryGetHitPoints(Target, Current, Max) ? Max : 0.0f;
}

bool USampleCombatModAPI::IsAlive(AActor* Target) const
{
	float Current = 0.0f;
	float Max = 0.0f;
	return TryGetHitPoints(Target, Current, Max) && Current > 0.0f;
}

APawn* USampleCombatModAPI::GetPlayerPawn(int32 PlayerIndex) const
{
	if (PlayerIndex < 0)
	{
		return nullptr;
	}

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	return UGameplayStatics::GetPlayerPawn(World, PlayerIndex);
}
