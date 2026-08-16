// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "CoreTypes.h"
#include "Events/ModEventTypes.h"
#include "Math/Vector.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "GameModEvents.generated.h"

class AActor;

/**
 * The game's own event ids.
 *
 * These are the ids a mod author types into a "Subscribe To Mod Event" node, so the string values
 * are part of the binding contract and must never change. They live in the SDK rather than in the
 * framework because the framework has no opinion about what a game's events mean - it only stores
 * declarations, orders subscribers and enforces permissions, authority and payload shape.
 *
 * Every id here is declared through BuildGameEventDescriptors(), which the game registers in one
 * call at startup:
 *
 *     for (const FModEventDescriptor& Descriptor : BuildGameEventDescriptors())
 *     {
 *         ModSubsystem->RegisterEventType(Descriptor);
 *     }
 *
 * Registering is what gives an event its payload-type check and its server-authority gate. An
 * unregistered id can still be broadcast - UModEventBus allows that deliberately - but it is then
 * ungated, so register before the first mod loads.
 */
namespace GameModEvents
{
	/** "Game.EnemyKilled" - an enemy died. Payload: FGameEnemyKilledEvent. */
	GAMEMODSDK_API extern const FName EnemyKilled;

	/** "Game.PlayerDamaged" - a player took damage. Payload: FGamePlayerDamagedEvent. */
	GAMEMODSDK_API extern const FName PlayerDamaged;

	/** "Game.WaveStarted" - a new wave began. Payload: FGameWaveStartedEvent. */
	GAMEMODSDK_API extern const FName WaveStarted;
}

/**
 * Payload of GameModEvents::EnemyKilled.
 *
 * ACTOR REFERENCES ARE WEAK ON PURPOSE. An FModEventContext is a stack value that UModEventBus hands
 * to every handler as a const reference and then throws away; it is not a reflected, garbage
 * collected reference, so a strong pointer stored here would dangle the moment a handler cached the
 * payload and let the actor be destroyed. Always test the weak pointer before dereferencing it -
 * with a mod handler in the chain, Enemy may already have been torn down by an earlier one.
 */
USTRUCT(BlueprintType, meta = (ModPublic))
struct GAMEMODSDK_API FGameEnemyKilledEvent : public FModEventPayload
{
	GENERATED_BODY()

	/** The enemy that died. Already dying or destroyed by the time some handlers see it. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	TWeakObjectPtr<AActor> Enemy;

	/** Whoever landed the killing blow, or null when the game could not attribute the kill. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	TWeakObjectPtr<AActor> Killer;

	/** Primary asset name of the UGameEnemyDefinition the enemy was spawned from, or NAME_None. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	FName EnemyId;

	/** Where it died. Survives the actor, which is what makes it the useful field for loot mods. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	FVector Location = FVector::ZeroVector;

	/** Wave the kill happened in, matching UGameWorldModAPI::GetCurrentWave. 0 when there is none. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	int32 WaveIndex = 0;
};

/**
 * Payload of GameModEvents::PlayerDamaged.
 *
 * The actor references are weak for the reason given on FGameEnemyKilledEvent: the context does not
 * keep anything alive.
 */
USTRUCT(BlueprintType, meta = (ModPublic))
struct GAMEMODSDK_API FGamePlayerDamagedEvent : public FModEventPayload
{
	GENERATED_BODY()

	/** The damaged player's pawn. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	TWeakObjectPtr<AActor> Player;

	/** What dealt the damage, or null for world damage with no attributable source. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	TWeakObjectPtr<AActor> DamageCauser;

	/** Damage actually applied, after the game's own mitigation. Never the requested amount. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	float Damage = 0.0f;

	/** Health left after the hit. Zero means the hit was lethal. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	float RemainingHealth = 0.0f;

	/** Game-defined damage category, matching UGameCombatModAPI::ApplyDamage. NAME_None when generic. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	FName DamageType;
};

/** Payload of GameModEvents::WaveStarted. */
USTRUCT(BlueprintType, meta = (ModPublic))
struct GAMEMODSDK_API FGameWaveStartedEvent : public FModEventPayload
{
	GENERATED_BODY()

	/** The wave that just began, matching UGameWorldModAPI::GetCurrentWave. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	int32 WaveIndex = 0;

	/** How many enemies the wave intends to field in total, not how many are alive right now. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	int32 EnemyCount = 0;

	/** True when no further wave follows this one. */
	UPROPERTY(BlueprintReadWrite, Category = "Game|Events", meta = (ModPublic))
	bool bFinalWave = false;
};

/**
 * Every event declaration the game exposes to mods, ready to hand to
 * UModSubsystem::RegisterEventType.
 *
 * Returned by value and rebuilt on each call - it is startup-time code, and returning a fresh array
 * means a caller can adjust a descriptor (tighten RequiredPermissions, say) before registering it
 * without editing anybody else's copy.
 *
 * Every descriptor is marked server-authoritative: all three events describe things that only ever
 * happen on the authority, so UModEventBus refuses to broadcast them from a world whose net mode is
 * NM_Client. None of them requires a permission to broadcast, because the game raises them itself
 * and the game is never permission gated. A game that also wants to stop a *mod* forging combat
 * events should add its own permission to RequiredPermissions before registering.
 */
GAMEMODSDK_API TArray<FModEventDescriptor> BuildGameEventDescriptors();
