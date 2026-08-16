// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Extensions/ModExtension.h"
#include "UObject/ObjectMacros.h"

#include "GameWeaponExtension.generated.h"

class AActor;
class UGameWeaponDefinition;

/**
 * Base class for everything a mod contributes to the "game.weapon" extension point.
 *
 * A weapon extension does exactly two things: it names a weapon (by returning a
 * UGameWeaponDefinition the mod authored) and it adjusts a damage number the game hands it. It never
 * spawns, equips, traces or applies anything. Applying the result is the game's job, through
 * UGameCombatModAPI - which is why this class can live in an SDK a mod author installs without ever
 * having the game's source.
 *
 * A mod author subclasses this in Blueprint and implements the hooks that matter to them; the ones
 * they leave alone keep their native defaults, so a weapon extension that only supplies a definition
 * does not accidentally zero every damage number in the game.
 *
 * ExtensionPointId is filled in by the constructor. A Blueprint author never has to know the id.
 *
 * Ordering and conflicts: "game.weapon" resolves with EModConflictPolicy::Priority, so the
 * highest-Priority extension claiming a resource id wins outright. Set Priority and
 * ClaimedResourceIds in the Blueprint's class defaults - both are inherited from UModExtension.
 *
 * The point is server authoritative. The game must only apply damage adjustments where it already
 * has authority; this class is a declaration of intent, not an enforcement mechanism.
 *
 * Everything a mod returns here is untrusted. The native hooks below already reject non-finite
 * numbers and clamp absurd ones, and the game should still treat a returned definition asset as
 * data that may be null or nonsense.
 */
UCLASS(Abstract, Blueprintable, BlueprintType, meta = (ModPublic, ModExtensionPoint = "game.weapon"))
class GAMEMODSDK_API UGameWeaponExtension : public UModExtension
{
	GENERATED_BODY()

public:
	/** Stamps ExtensionPointId with GameModExtensionPoints::Weapon so a Blueprint author never does. */
	UGameWeaponExtension();

	// --- Blueprint hooks ---------------------------------------------------------------------------
	// Implement these in a Blueprint subclass. Do not call them directly from game code: call the
	// Native* counterparts below, which validate what a mod returned and supply a default when the
	// mod implemented nothing.

	/**
	 * The weapon this extension contributes, or nothing when it only adjusts damage.
	 *
	 * Return an asset the mod ships. The game reads it; the mod never applies it.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|Weapon", meta = (DisplayName = "Get Weapon Definition"))
	UGameWeaponDefinition* GetWeaponDefinition() const;

	/**
	 * Adjusts one outgoing damage number.
	 *
	 * @param BaseDamage  What the game would have applied, already including every earlier extension
	 *                    in the point's order - so returning BaseDamage unchanged is always correct.
	 * @param Target      The actor about to be damaged. May be null. Supplied so a mod can vary the
	 *                    result by target; damaging it here is not this hook's job.
	 * @return The damage the game should apply instead.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|Weapon", meta = (DisplayName = "Modify Outgoing Damage"))
	float ModifyOutgoingDamage(float BaseDamage, AActor* Target) const;

	// --- Native counterparts -----------------------------------------------------------------------
	// What the game calls. Each one dispatches to the Blueprint hook only when a Blueprint subclass
	// actually implements it, and otherwise returns the documented default. They are BlueprintCallable
	// rather than BlueprintPure on purpose: they run mod-authored script, so they must never sit on a
	// pure node that silently re-executes.

	/** The Blueprint hook's definition, or nullptr when the mod implemented nothing. Never asserts. */
	UFUNCTION(BlueprintCallable, Category = "Game|Weapon", meta = (DisplayName = "Resolve Weapon Definition"))
	virtual UGameWeaponDefinition* NativeGetWeaponDefinition() const;

	/**
	 * BaseDamage passed through the Blueprint hook, then sanitised.
	 *
	 * Returns BaseDamage unchanged when the mod implemented nothing, when the extension cannot be
	 * dispatched into script (a class default object, or an object already collected during
	 * teardown), or when the mod returned a NaN or an infinity. A finite result is clamped into
	 * [0, MaxOutgoingDamage]: a damage hook may not heal its target, and may not hand the game a
	 * number large enough to break the arithmetic downstream of it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|Weapon", meta = (DisplayName = "Resolve Outgoing Damage"))
	virtual float NativeModifyOutgoingDamage(float BaseDamage, AActor* Target) const;

	/**
	 * Ceiling applied to whatever a mod returns from ModifyOutgoingDamage.
	 *
	 * A sanity bound, not a balance decision: it exists so that a mod returning 1e30 cannot turn
	 * every accumulated damage total in the game into an infinity. Games wanting a real balance cap
	 * apply their own in UGameCombatModAPI, where the rest of the balance already lives.
	 */
	static constexpr float MaxOutgoingDamage = 1.0e6f;
};
