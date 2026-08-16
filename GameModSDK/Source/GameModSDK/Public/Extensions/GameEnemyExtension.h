// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Extensions/ModExtension.h"
#include "UObject/ObjectMacros.h"

#include "GameEnemyExtension.generated.h"

class AActor;
class UGameEnemyDefinition;

/**
 * Base class for everything a mod contributes to the "game.enemy" extension point.
 *
 * An enemy extension names an enemy (by returning a UGameEnemyDefinition the mod authored), adjusts
 * the health the game is about to give one, and is told when one spawned. It never spawns, possesses
 * or destroys anything: the game does that through UGameWorldModAPI after reading what the extension
 * returned. Keeping it that way is what lets this class ship in an SDK that has no access to the
 * game's own module.
 *
 * A mod author subclasses this in Blueprint and implements only the hooks they care about. The rest
 * keep their native defaults, so an extension that merely watches spawns cannot accidentally zero
 * every enemy's health.
 *
 * ExtensionPointId is filled in by the constructor. A Blueprint author never has to know the id.
 *
 * Ordering and conflicts: "game.enemy" resolves with EModConflictPolicy::Priority, so the
 * highest-Priority extension claiming a resource id wins outright. Set Priority and
 * ClaimedResourceIds in the Blueprint's class defaults - both are inherited from UModExtension.
 *
 * The point is server authoritative: enemy health and spawning are the server's business. The flag
 * is a declaration; the game still has to apply the results only where it has authority.
 *
 * Everything a mod returns here is untrusted, and so is every actor pointer handed to it.
 */
UCLASS(Abstract, Blueprintable, BlueprintType, meta = (ModPublic, ModExtensionPoint = "game.enemy"))
class GAMEMODSDK_API UGameEnemyExtension : public UModExtension
{
	GENERATED_BODY()

public:
	/** Stamps ExtensionPointId with GameModExtensionPoints::Enemy so a Blueprint author never does. */
	UGameEnemyExtension();

	// --- Blueprint hooks ---------------------------------------------------------------------------
	// Implement these in a Blueprint subclass. Game code calls the Native* counterparts below, never
	// these: the counterparts validate what the mod returned and supply the default when it returned
	// nothing at all.

	/**
	 * The enemy this extension contributes, or nothing when it only adjusts or observes.
	 *
	 * Return an asset the mod ships. The game reads it and decides what to spawn from it.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|Enemy", meta = (DisplayName = "Get Enemy Definition"))
	UGameEnemyDefinition* GetEnemyDefinition() const;

	/**
	 * Adjusts the health an enemy is about to be given.
	 *
	 * @param Base  What the game would have used, already including every earlier extension in the
	 *              point's order - so returning Base unchanged is always correct.
	 * @return The health the game should use instead.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|Enemy", meta = (DisplayName = "Modify Enemy Health"))
	float ModifyEnemyHealth(float Base) const;

	/**
	 * Notification that an enemy spawned. Observation only.
	 *
	 * @param Enemy  The spawned actor, or null if the game could not supply one. It is passed so a
	 *               mod can identify what appeared and react through the game's APIs - not so the
	 *               extension can drive it directly.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|Enemy", meta = (DisplayName = "On Enemy Spawned"))
	void OnEnemySpawned(AActor* Enemy);

	// --- Native counterparts -----------------------------------------------------------------------
	// What the game calls. Each dispatches to the Blueprint hook only when a Blueprint subclass
	// actually implements it. BlueprintCallable rather than BlueprintPure on purpose: they run
	// mod-authored script, so they must never sit on a pure node that silently re-executes.

	/** The Blueprint hook's definition, or nullptr when the mod implemented nothing. Never asserts. */
	UFUNCTION(BlueprintCallable, Category = "Game|Enemy", meta = (DisplayName = "Resolve Enemy Definition"))
	virtual UGameEnemyDefinition* NativeGetEnemyDefinition() const;

	/**
	 * Base passed through the Blueprint hook, then sanitised.
	 *
	 * Returns Base unchanged when the mod implemented nothing, when the extension cannot be
	 * dispatched into script (a class default object, or an object already collected during
	 * teardown), or when the mod returned a NaN or an infinity. A finite result is clamped into
	 * [0, MaxEnemyHealth]; note that zero is allowed through, and the game decides what an enemy
	 * spawned with no health means.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|Enemy", meta = (DisplayName = "Resolve Enemy Health"))
	virtual float NativeModifyEnemyHealth(float Base) const;

	/**
	 * Raises the Blueprint notification when a Blueprint subclass implements it.
	 *
	 * Does nothing - and never asserts - for a class default object, for an extension already
	 * collected during teardown, or for a mod that implemented nothing. A null Enemy is forwarded as
	 * null rather than dropped, because "something spawned that the game could not name" is exactly
	 * the case a mod may want to notice.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|Enemy", meta = (DisplayName = "Notify Enemy Spawned"))
	virtual void NativeOnEnemySpawned(AActor* Enemy);

	/**
	 * Ceiling applied to whatever a mod returns from ModifyEnemyHealth.
	 *
	 * A sanity bound, not a balance decision: it stops a mod returning 1e30 from turning every health
	 * total that touches it into an infinity. Real balance caps belong in the game's own API layer.
	 */
	static constexpr float MaxEnemyHealth = 1.0e6f;
};
