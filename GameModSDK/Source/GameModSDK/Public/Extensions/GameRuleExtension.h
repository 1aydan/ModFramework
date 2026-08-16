// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Extensions/ModExtension.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "GameRuleExtension.generated.h"

/**
 * Base class for everything a mod contributes to the "game.gamerule" extension point.
 *
 * A rule extension is told when the session and each wave begin, and is asked whether an enemy may
 * spawn. That is the whole surface: it answers questions and observes, and the game acts on the
 * answers through UGameWorldModAPI. It cannot start a wave, spawn anything or end a session, and it
 * has no route to the game's own types - which is what keeps this class usable in an SDK a mod
 * author installs on its own.
 *
 * ExtensionPointId is filled in by the constructor. A Blueprint author never has to know the id.
 *
 * Ordering and conflicts: "game.gamerule" resolves with EModConflictPolicy::Merge, which means the
 * registry deliberately picks no single winner - rules compose. The game walks every active
 * extension in registry order (Priority descending, then load order, then id) and combines the
 * answers itself. For ShouldAllowSpawn the sane combination is unanimity: one veto is a veto, which
 * is the rule the game should implement in UGameWorldModAPI.
 *
 * The point is server authoritative. Wave and spawn decisions taken on a client are not decisions;
 * the game must only consult these hooks where it has authority.
 *
 * Every answer is untrusted mod-authored script. Nothing here asserts on one, and a mod that
 * implements nothing gets defaults that leave the game exactly as it was.
 */
UCLASS(Abstract, Blueprintable, BlueprintType, meta = (ModPublic, ModExtensionPoint = "game.gamerule"))
class GAMEMODSDK_API UGameRuleExtension : public UModExtension
{
	GENERATED_BODY()

public:
	/** Stamps ExtensionPointId with GameModExtensionPoints::GameRule so a Blueprint author never does. */
	UGameRuleExtension();

	// --- Blueprint hooks ---------------------------------------------------------------------------
	// Implement these in a Blueprint subclass. Game code calls the Native* counterparts below.

	/**
	 * A wave began.
	 *
	 * @param WaveNumber  The wave the game just started, as the game numbers them. It is passed
	 *                    through unvalidated, so a mod should not assume it starts at 1 or increases
	 *                    by one every time.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|Rule", meta = (DisplayName = "On Wave Started"))
	void OnWaveStarted(int32 WaveNumber);

	/** The session began. Raised once per session, before the first wave. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|Rule", meta = (DisplayName = "On Game Started"))
	void OnGameStarted();

	/**
	 * Vetoes a spawn.
	 *
	 * @param EnemyId  The id of the enemy the game is about to spawn. May be None when the game has
	 *                 no id for it, and a mod must answer sensibly for ids it has never heard of -
	 *                 returning false for everything unknown disables the game.
	 * @return false to veto this spawn, true to allow it. Returning true is always safe.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|Rule", meta = (DisplayName = "Should Allow Spawn"))
	bool ShouldAllowSpawn(FName EnemyId) const;

	// --- Native counterparts -----------------------------------------------------------------------
	// What the game calls. Each dispatches to the Blueprint hook only when a Blueprint subclass
	// actually implements it. BlueprintCallable rather than BlueprintPure on purpose: they run
	// mod-authored script, so they must never sit on a pure node that silently re-executes.

	/**
	 * Raises the Blueprint notification when a Blueprint subclass implements it.
	 *
	 * Does nothing - and never asserts - for a class default object, for an extension already
	 * collected during teardown, or for a mod that implemented nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|Rule", meta = (DisplayName = "Notify Wave Started"))
	virtual void NativeOnWaveStarted(int32 WaveNumber);

	/** As NativeOnWaveStarted, for the start of the session. */
	UFUNCTION(BlueprintCallable, Category = "Game|Rule", meta = (DisplayName = "Notify Game Started"))
	virtual void NativeOnGameStarted();

	/**
	 * The mod's verdict on one spawn, defaulting to "allowed".
	 *
	 * Returns true when the mod implemented nothing and when the extension cannot be dispatched into
	 * script (a class default object, or an object already collected during teardown). A rule that
	 * cannot be consulted must never silently block the game.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|Rule", meta = (DisplayName = "Resolve Should Allow Spawn"))
	virtual bool NativeShouldAllowSpawn(FName EnemyId) const;
};
