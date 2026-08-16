// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Engine/DataAsset.h"
#include "Internationalization/Text.h"
#include "UObject/ObjectMacros.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/SoftObjectPtr.h"

#include "GameEnemyDefinition.generated.h"

class AActor;
class UTexture2D;

/**
 * The data-driven way to add an enemy: a mod ships one of these instead of any code at all.
 *
 * A mod author creates a Data Asset of this class (or a Blueprint subclass of it, then a data-only
 * asset of that), fills in the numbers and points EnemyClass at the actor to spawn. The game picks
 * it up through the asset manager, and UGameWorldModAPI::SpawnEnemy names it by its primary asset
 * name - which is why a mod can spawn its own enemy without ever handing the game a raw UClass.
 *
 * ---------------------------------------------------------------------------------------------
 * PRIMARY ASSET IDENTITY
 * ---------------------------------------------------------------------------------------------
 * GetPrimaryAssetId is overridden to pin the PrimaryAssetType to the literal "GameEnemyDefinition"
 * for every instance. Without the override, UPrimaryDataAsset derives the type from the nearest
 * native class going up the hierarchy, so a mod that subclassed this in Blueprint would publish its
 * enemies under a *different* type - and the game's asset manager, which scans for
 * "GameEnemyDefinition", would never see them. Pinning the type is what makes a Blueprint subclass
 * in a mod work exactly like a plain data asset.
 *
 * The game must add a PrimaryAssetType entry named "GameEnemyDefinition" to its asset manager
 * settings, pointing at the directories mods are mounted under, or nothing here is ever scanned.
 *
 * ---------------------------------------------------------------------------------------------
 * SOFT REFERENCES
 * ---------------------------------------------------------------------------------------------
 * Icon is a TSoftObjectPtr and EnemyClass is a TSoftClassPtr rather than hard references. That is
 * not a style preference: the asset manager loads every definition it scans, so a hard reference
 * would drag one mod's enemy Blueprint - and through it its mesh, animations and AI assets - into
 * memory the moment its data asset was discovered, before any enemy ever spawned, multiplied by
 * every mod installed. Resolve them through FStreamableManager at spawn time.
 *
 * ---------------------------------------------------------------------------------------------
 * UNTRUSTED DATA
 * ---------------------------------------------------------------------------------------------
 * Everything below was authored by a mod, so every number is untrusted: it can be negative, zero,
 * enormous, or NaN, and the ClampMin/ClampMax metadata only constrains what the *editor* lets an
 * author type - a hand-edited or cooked asset can carry anything. Game code should read the
 * GetSanitized* accessors rather than the raw fields; they clamp into a documented range and
 * substitute the default for a non-finite value. EnemyClass is untrusted too: it may be unset,
 * point at a deleted asset, or resolve to an abstract class, so always test the resolved UClass
 * before spawning it. Nothing here ever asserts.
 */
UCLASS(BlueprintType, meta = (ModPublic))
class GAMEMODSDK_API UGameEnemyDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** "GameEnemyDefinition" - the PrimaryAssetType every instance reports, subclassing included. */
	static FPrimaryAssetType GetPrimaryAssetTypeStatic();

	// --- Presentation ----------------------------------------------------------------------------

	/** Name shown in the game's UI. Localisable, and untrusted: cap its length before rendering it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Presentation", meta = (ModPublic))
	FText DisplayName;

	/** Codex / health-bar icon. Soft: load it only when the enemy is shown. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Presentation", meta = (ModPublic))
	TSoftObjectPtr<UTexture2D> Icon;

	// --- Spawning --------------------------------------------------------------------------------

	/**
	 * The actor UGameWorldModAPI::SpawnEnemy instantiates for this definition.
	 *
	 * Typed as AActor rather than APawn so a game is free to represent an enemy as something other
	 * than a pawn. It is soft, so discovering the definition does not load the Blueprint. Resolving
	 * it can fail - the referenced asset may not be mounted, or may have been deleted - and the
	 * result may be abstract or an entirely unrelated actor; validate before spawning.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Spawning", meta = (ModPublic))
	TSoftClassPtr<AActor> EnemyClass;

	// --- Combat numbers --------------------------------------------------------------------------

	/** Health the enemy spawns with. Read it through GetSanitizedHealth, which never returns zero. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat", meta = (ModPublic, ClampMin = "1.0", ClampMax = "10000000.0"))
	float Health = 100.0f;

	/** Damage one of its attacks deals. Read it through GetSanitizedDamage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat", meta = (ModPublic, ClampMin = "0.0", ClampMax = "100000.0"))
	float Damage = 10.0f;

	/** Ground speed in cm/s. Read it through GetSanitizedMoveSpeed. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat", meta = (ModPublic, ClampMin = "0.0", ClampMax = "100000.0"))
	float MoveSpeed = 400.0f;

	// --- Sanitised reads -------------------------------------------------------------------------

	/**
	 * Health clamped to [1, 10000000].
	 *
	 * Guaranteed strictly positive, so it is always safe as the denominator of a health fraction -
	 * which is exactly the division a mod-authored 0 would break.
	 */
	UFUNCTION(BlueprintCallable, Category = "Enemy|Combat", meta = (ModPublic))
	float GetSanitizedHealth() const;

	/** Damage clamped to [0, 100000]. A non-finite authored value yields 0 instead. */
	UFUNCTION(BlueprintCallable, Category = "Enemy|Combat", meta = (ModPublic))
	float GetSanitizedDamage() const;

	/** Move speed clamped to [0, 100000] cm/s. A non-finite authored value yields 0 instead. */
	UFUNCTION(BlueprintCallable, Category = "Enemy|Combat", meta = (ModPublic))
	float GetSanitizedMoveSpeed() const;

	//~ Begin UObject interface
	/**
	 * "GameEnemyDefinition:<asset name>", pinned so a Blueprint subclass in a mod still publishes
	 * under the type the game scans for. Class default objects return an invalid id, matching what
	 * UPrimaryDataAsset does for a native class.
	 */
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;
	//~ End UObject interface

	/** Bounds GetSanitizedHealth clamps to. The minimum is what keeps a health fraction finite. */
	static constexpr float HealthClampMin = 1.0f;
	static constexpr float HealthClampMax = 10000000.0f;

	/** Upper bound GetSanitizedDamage clamps to. */
	static constexpr float DamageClampMax = 100000.0f;

	/** Upper bound GetSanitizedMoveSpeed clamps to, in cm/s. */
	static constexpr float MoveSpeedClampMax = 100000.0f;
};
