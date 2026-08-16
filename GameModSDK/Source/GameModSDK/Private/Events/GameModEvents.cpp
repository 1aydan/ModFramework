// Copyright (c) 2026. Licensed for use in your own projects.

#include "Events/GameModEvents.h"

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Events/ModEventTypes.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"

/**
 * The game's own event ids.
 *
 * The string values are part of the binding contract: they are what a mod author types into a
 * Blueprint "Subscribe To Mod Event" node and what tooling prints, so they must never change.
 * FName construction during static initialisation is safe - the name table initialises lazily on
 * first use - and doing it here means the ids are interned exactly once per process.
 */
namespace GameModEvents
{
	const FName EnemyKilled(TEXT("Game.EnemyKilled"));
	const FName PlayerDamaged(TEXT("Game.PlayerDamaged"));
	const FName WaveStarted(TEXT("Game.WaveStarted"));
}

namespace
{
	/**
	 * Fills in one descriptor.
	 *
	 * Deliberately NOT named MakeError, MakeValue or anything else that collides with the global
	 * variadic templates in Templates/ValueOrError.h - overload resolution picks those up from the
	 * global namespace and the resulting diagnostic is unreadable.
	 */
	FModEventDescriptor MakeGameEventDescriptor(FName EventId, const TCHAR* Description, UScriptStruct* PayloadType)
	{
		FModEventDescriptor Descriptor;
		Descriptor.EventId = EventId;
		Descriptor.Description = Description;
		Descriptor.PayloadType = PayloadType;

		// All three describe something that only ever happens on the authority. The bus refuses to
		// broadcast them from a world whose net mode is NM_Client.
		Descriptor.bServerAuthoritative = true;

		// Left empty on purpose: the game raises these, and the game is never permission gated. A game
		// that wants to stop a mod forging them can add a permission here before registering.
		Descriptor.RequiredPermissions.Reset();

		return Descriptor;
	}
}

TArray<FModEventDescriptor> BuildGameEventDescriptors()
{
	TArray<FModEventDescriptor> Descriptors;
	Descriptors.Reserve(3);

	Descriptors.Add(MakeGameEventDescriptor(
		GameModEvents::EnemyKilled,
		TEXT("An enemy died. Carries the enemy, its killer, its definition id, where it fell and the wave it belonged to."),
		FGameEnemyKilledEvent::StaticStruct()));

	Descriptors.Add(MakeGameEventDescriptor(
		GameModEvents::PlayerDamaged,
		TEXT("A player took damage. Carries the pawn, the causer, the mitigated amount, the health left and the damage type."),
		FGamePlayerDamagedEvent::StaticStruct()));

	Descriptors.Add(MakeGameEventDescriptor(
		GameModEvents::WaveStarted,
		TEXT("A new wave began. Carries the wave index, how many enemies it will field and whether it is the last one."),
		FGameWaveStartedEvent::StaticStruct()));

	return Descriptors;
}
