// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Engine/DataAsset.h"
#include "Internationalization/Text.h"
#include "UObject/ObjectMacros.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/SoftObjectPtr.h"

#include "GameWeaponDefinition.generated.h"

class UAnimMontage;
class UStaticMesh;
class UTexture2D;

/**
 * The data-driven way to add a weapon: a mod ships one of these instead of any code at all.
 *
 * A mod author creates a Data Asset of this class (or a Blueprint subclass of it, then a data-only
 * asset of that), fills in the numbers and the content references, and the game picks it up through
 * the asset manager. No C++, no Blueprint logic, no extension class - which is why this is the path
 * most mods should take.
 *
 * ---------------------------------------------------------------------------------------------
 * PRIMARY ASSET IDENTITY
 * ---------------------------------------------------------------------------------------------
 * GetPrimaryAssetId is overridden to pin the PrimaryAssetType to the literal "GameWeaponDefinition"
 * for every instance. Without the override, UPrimaryDataAsset derives the type from the nearest
 * native class going up the hierarchy, so a mod that subclassed this in Blueprint would publish its
 * weapons under a *different* type - and the game's asset manager, which scans for
 * "GameWeaponDefinition", would never see them. Pinning the type is what makes a Blueprint subclass
 * in a mod work exactly like a plain data asset.
 *
 * The game must add a PrimaryAssetType entry named "GameWeaponDefinition" to its asset manager
 * settings, pointing at the directories mods are mounted under, or nothing here is ever scanned.
 *
 * ---------------------------------------------------------------------------------------------
 * SOFT REFERENCES
 * ---------------------------------------------------------------------------------------------
 * Icon, Mesh and the montages are TSoftObjectPtr rather than hard pointers. That is not a style
 * preference: the asset manager loads every definition it scans, so a hard reference would drag one
 * mod's meshes, textures and animations into memory the moment its data asset was discovered -
 * before the player ever chose that weapon, and multiplied by every mod installed. Resolve them
 * through FStreamableManager when the weapon is actually equipped.
 *
 * ---------------------------------------------------------------------------------------------
 * UNTRUSTED DATA
 * ---------------------------------------------------------------------------------------------
 * Everything below was authored by a mod, so every number is untrusted: it can be negative, zero,
 * enormous, or NaN, and the ClampMin/ClampMax metadata only constrains what the *editor* lets an
 * author type - a hand-edited or cooked asset can carry anything. Game code should read the
 * GetSanitized* accessors rather than the raw fields; they clamp into a documented range and
 * substitute the default for a non-finite value. Nothing here ever asserts.
 */
UCLASS(BlueprintType, meta = (ModPublic))
class GAMEMODSDK_API UGameWeaponDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** "GameWeaponDefinition" - the PrimaryAssetType every instance reports, subclassing included. */
	static FPrimaryAssetType GetPrimaryAssetTypeStatic();

	// --- Presentation ----------------------------------------------------------------------------

	/** Name shown in the game's UI. Localisable, and untrusted: cap its length before rendering it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Presentation", meta = (ModPublic))
	FText DisplayName;

	/** Inventory / HUD icon. Soft: load it only when the weapon is shown. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Presentation", meta = (ModPublic))
	TSoftObjectPtr<UTexture2D> Icon;

	/** The weapon's visual prop, attached to the wielder. Soft: load it only when equipped. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Presentation", meta = (ModPublic))
	TSoftObjectPtr<UStaticMesh> Mesh;

	// --- Combat numbers --------------------------------------------------------------------------

	/** Damage per landed hit, in the game's own units. Read it through GetSanitizedDamage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Combat", meta = (ModPublic, ClampMin = "0.0", ClampMax = "100000.0"))
	float Damage = 10.0f;

	/** Attacks per second. Read it through GetSanitizedAttackRate, which never returns zero. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Combat", meta = (ModPublic, ClampMin = "0.01", ClampMax = "100.0"))
	float AttackRate = 1.0f;

	/** How many chained attacks the combo has. Read it through GetSanitizedComboCount. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Combat", meta = (ModPublic, ClampMin = "1", ClampMax = "32"))
	int32 ComboCount = 3;

	// --- Animation -------------------------------------------------------------------------------

	/** Montage driving the combo chain. Its sections are the game's business, not the framework's. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Animation", meta = (ModPublic))
	TSoftObjectPtr<UAnimMontage> ComboAttackMontage;

	/** Montage played for the charged attack. May be left unset for a weapon that has none. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Animation", meta = (ModPublic))
	TSoftObjectPtr<UAnimMontage> ChargedAttackMontage;

	// --- Sanitised reads -------------------------------------------------------------------------

	/** Damage clamped to [0, 100000]. A non-finite authored value yields the class default instead. */
	UFUNCTION(BlueprintCallable, Category = "Weapon|Combat", meta = (ModPublic))
	float GetSanitizedDamage() const;

	/**
	 * Attack rate clamped to [0.01, 100] attacks per second.
	 *
	 * Guaranteed strictly positive, so `1.0f / GetSanitizedAttackRate()` is always a safe way to get
	 * the interval between attacks - which is exactly the division a mod-authored 0 would break.
	 */
	UFUNCTION(BlueprintCallable, Category = "Weapon|Combat", meta = (ModPublic))
	float GetSanitizedAttackRate() const;

	/** Combo count clamped to [1, 32]. Guaranteed at least 1, so it is always a usable array bound. */
	UFUNCTION(BlueprintCallable, Category = "Weapon|Combat", meta = (ModPublic))
	int32 GetSanitizedComboCount() const;

	//~ Begin UObject interface
	/**
	 * "GameWeaponDefinition:<asset name>", pinned so a Blueprint subclass in a mod still publishes
	 * under the type the game scans for. Class default objects return an invalid id, matching what
	 * UPrimaryDataAsset does for a native class.
	 */
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;
	//~ End UObject interface

	/** Upper bound GetSanitizedDamage clamps to. */
	static constexpr float DamageClampMax = 100000.0f;

	/** Bounds GetSanitizedAttackRate clamps to. The minimum is what keeps the reciprocal finite. */
	static constexpr float AttackRateClampMin = 0.01f;
	static constexpr float AttackRateClampMax = 100.0f;

	/** Bounds GetSanitizedComboCount clamps to. */
	static constexpr int32 ComboCountClampMin = 1;
	static constexpr int32 ComboCountClampMax = 32;
};
