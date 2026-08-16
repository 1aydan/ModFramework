// Copyright (c) 2026. Licensed for use in your own projects.

#include "Data/GameEnemyDefinition.h"

#include "CoreTypes.h"
#include "Math/UnrealMathUtility.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/UObjectBaseUtility.h"

namespace
{
	/**
	 * The literal every UGameEnemyDefinition publishes itself under.
	 *
	 * It is a hard-coded string rather than the class name so that renaming the C++ class can never
	 * silently repoint every mod's enemies at a type the game's asset manager is not scanning.
	 */
	const TCHAR* const GameEnemyDefinitionAssetTypeString = TEXT("GameEnemyDefinition");
}

FPrimaryAssetType UGameEnemyDefinition::GetPrimaryAssetTypeStatic()
{
	return FPrimaryAssetType(FName(GameEnemyDefinitionAssetTypeString));
}

float UGameEnemyDefinition::GetSanitizedHealth() const
{
	// A NaN or infinite value would survive Clamp untouched and then poison every calculation that
	// touched it, so it is replaced outright rather than clamped.
	if (!FMath::IsFinite(Health))
	{
		return HealthClampMin;
	}

	return FMath::Clamp(Health, HealthClampMin, HealthClampMax);
}

float UGameEnemyDefinition::GetSanitizedDamage() const
{
	if (!FMath::IsFinite(Damage))
	{
		return 0.0f;
	}

	return FMath::Clamp(Damage, 0.0f, DamageClampMax);
}

float UGameEnemyDefinition::GetSanitizedMoveSpeed() const
{
	if (!FMath::IsFinite(MoveSpeed))
	{
		return 0.0f;
	}

	return FMath::Clamp(MoveSpeed, 0.0f, MoveSpeedClampMax);
}

FPrimaryAssetId UGameEnemyDefinition::GetPrimaryAssetId() const
{
	// Class default objects are not assets. UPrimaryDataAsset returns an invalid id for the CDO of a
	// native class for exactly this reason, and the asset manager treats an invalid id as "not a
	// primary asset" rather than as an error.
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return FPrimaryAssetId();
	}

	return FPrimaryAssetId(GetPrimaryAssetTypeStatic(), GetFName());
}
