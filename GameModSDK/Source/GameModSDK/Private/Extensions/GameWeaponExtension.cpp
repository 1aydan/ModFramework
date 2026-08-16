// Copyright (c) 2026. Licensed for use in your own projects.

#include "Extensions/GameWeaponExtension.h"

#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "Extensions/GameModSDKExtensionPoints.h"
#include "GameModSDKModule.h"
#include "Logging/LogMacros.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AssertionMacros.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

namespace GameWeaponExtensionPrivate
{
	/**
	 * "<mod id> (<class name>)" for a log line.
	 *
	 * Resolved defensively: this runs against mod-authored objects that may never have been
	 * registered and may be part-way through teardown, so nothing here dereferences without asking.
	 */
	FString DescribeExtension(const UGameWeaponExtension& Extension)
	{
		const FModId& OwningModId = Extension.OwningModId;
		const FString ModIdText = OwningModId.IsValid() ? OwningModId.ToString() : FString(TEXT("<unregistered>"));

		const UClass* Class = Extension.GetClass();
		const FString ClassName = Class ? Class->GetName() : FString(TEXT("<null class>"));

		return FString::Printf(TEXT("%s (%s)"), *ModIdText, *ClassName);
	}

	/** True when HookName can be dispatched into script AND a Blueprint subclass implements it. */
	bool CanRunHook(const UGameWeaponExtension& Extension, const FName HookName)
	{
		// A class default object has no mod behind it, and an object that has already been collected
		// during teardown cannot survive a ProcessEvent. A mod's extension can be either, so both are
		// refusals rather than assertions.
		if (Extension.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !::IsValid(&Extension))
		{
			return false;
		}

		// A BlueprintImplementableEvent nobody implemented resolves to a UFunction with no script;
		// ProcessEvent ignores it and the generated thunk hands back a zeroed return value. For a
		// damage hook that zero is catastrophically wrong, so ask before dispatching rather than
		// after.
		const UClass* Class = Extension.GetClass();
		return Class != nullptr && Class->IsFunctionImplementedInScript(HookName);
	}

	/**
	 * Makes a mod's damage number safe to hand back to the game.
	 *
	 * NaN and the infinities are rejected outright in favour of Fallback - they propagate through
	 * every later calculation and are impossible to trace back to the mod that produced them.
	 * Anything finite is clamped into [0, MaxOutgoingDamage]: a damage hook may not heal its target
	 * by returning a negative, and may not hand the game a number big enough to saturate the totals
	 * it feeds.
	 */
	float SanitizeDamage(const UGameWeaponExtension& Extension, const float Value, const float Fallback)
	{
		if (!FMath::IsFinite(Value))
		{
			UE_LOG(LogGameModSDK, Warning,
				TEXT("Weapon extension %s returned a non-finite value from ModifyOutgoingDamage; the unmodified damage %.3f was used instead."),
				*DescribeExtension(Extension), Fallback);
			return Fallback;
		}

		const float Clamped = FMath::Clamp(Value, 0.0f, UGameWeaponExtension::MaxOutgoingDamage);
		if (Clamped != Value)
		{
			// Verbose, not Warning: this can happen on every hit, and a mod that clamps every frame
			// must not be able to make the log unusable for everybody else.
			UE_LOG(LogGameModSDK, Verbose,
				TEXT("Weapon extension %s returned %.3f from ModifyOutgoingDamage; clamped to %.3f."),
				*DescribeExtension(Extension), Value, Clamped);
		}

		return Clamped;
	}
}

UGameWeaponExtension::UGameWeaponExtension()
{
	// Stamped here so that a Blueprint author never has to know - or mistype - the point id. A mod
	// that deliberately changes it in the class defaults simply registers against a different point,
	// and is rejected with Extension.PointNotFound if that point does not exist.
	ExtensionPointId = GameModExtensionPoints::Weapon;
}

UGameWeaponDefinition* UGameWeaponExtension::NativeGetWeaponDefinition() const
{
	if (!GameWeaponExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameWeaponExtension, GetWeaponDefinition)))
	{
		return nullptr;
	}

	// Whatever the mod returns is untrusted data: it may be null, and it may be an asset the game has
	// never heard of. The game validates it before using it - this class only carries it across.
	return GetWeaponDefinition();
}

float UGameWeaponExtension::NativeModifyOutgoingDamage(float BaseDamage, AActor* Target) const
{
	if (!GameWeaponExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameWeaponExtension, ModifyOutgoingDamage)))
	{
		// Not implemented, or not dispatchable. Returning the game's own number is the only answer
		// that leaves the game exactly as it was.
		return BaseDamage;
	}

	const float Modified = ModifyOutgoingDamage(BaseDamage, Target);
	return GameWeaponExtensionPrivate::SanitizeDamage(*this, Modified, BaseDamage);
}
