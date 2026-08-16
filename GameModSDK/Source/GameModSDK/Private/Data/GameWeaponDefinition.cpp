// Copyright (c) 2026. Licensed for use in your own projects.

#include "Data/GameWeaponDefinition.h"

#include "CoreTypes.h"
#include "Math/UnrealMathUtility.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/UObjectBaseUtility.h"

namespace
{
	/**
	 * The literal every UGameWeaponDefinition publishes itself under.
	 *
	 * It is a hard-coded string rather than the class name so that renaming the C++ class can never
	 * silently repoint every mod's weapons at a type the game's asset manager is not scanning.
	 */
	const TCHAR* const GameWeaponDefinitionAssetTypeString = TEXT("GameWeaponDefinition");
}

FPrimaryAssetType UGameWeaponDefinition::GetPrimaryAssetTypeStatic()
{
	return FPrimaryAssetType(FName(GameWeaponDefinitionAssetTypeString));
}

float UGameWeaponDefinition::GetSanitizedDamage() const
{
	// A NaN or infinite value would survive Clamp untouched and then poison every calculation that
	// touched it, so it is replaced outright rather than clamped.
	if (!FMath::IsFinite(Damage))
	{
		return 0.0f;
	}

	return FMath::Clamp(Damage, 0.0f, DamageClampMax);
}

float UGameWeaponDefinition::GetSanitizedAttackRate() const
{
	if (!FMath::IsFinite(AttackRate))
	{
		return 1.0f;
	}

	return FMath::Clamp(AttackRate, AttackRateClampMin, AttackRateClampMax);
}

int32 UGameWeaponDefinition::GetSanitizedComboCount() const
{
	return FMath::Clamp(ComboCount, ComboCountClampMin, ComboCountClampMax);
}

FPrimaryAssetId UGameWeaponDefinition::GetPrimaryAssetId() const
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
