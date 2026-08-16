// Copyright (c) 2026. Licensed for use in your own projects.

#include "Extensions/GameEnemyExtension.h"

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

namespace GameEnemyExtensionPrivate
{
	/**
	 * "<mod id> (<class name>)" for a log line.
	 *
	 * Resolved defensively: this runs against mod-authored objects that may never have been
	 * registered and may be part-way through teardown, so nothing here dereferences without asking.
	 */
	FString DescribeExtension(const UGameEnemyExtension& Extension)
	{
		const FModId& OwningModId = Extension.OwningModId;
		const FString ModIdText = OwningModId.IsValid() ? OwningModId.ToString() : FString(TEXT("<unregistered>"));

		const UClass* Class = Extension.GetClass();
		const FString ClassName = Class ? Class->GetName() : FString(TEXT("<null class>"));

		return FString::Printf(TEXT("%s (%s)"), *ModIdText, *ClassName);
	}

	/** True when HookName can be dispatched into script AND a Blueprint subclass implements it. */
	bool CanRunHook(const UGameEnemyExtension& Extension, const FName HookName)
	{
		// A class default object has no mod behind it, and an object that has already been collected
		// during teardown cannot survive a ProcessEvent. A mod's extension can be either, so both are
		// refusals rather than assertions.
		if (Extension.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !::IsValid(&Extension))
		{
			return false;
		}

		// A BlueprintImplementableEvent nobody implemented resolves to a UFunction with no script;
		// ProcessEvent ignores it and the generated thunk hands back a zeroed return value. An enemy
		// silently given zero health is exactly the bug that would cause, so ask before dispatching.
		const UClass* Class = Extension.GetClass();
		return Class != nullptr && Class->IsFunctionImplementedInScript(HookName);
	}

	/**
	 * Makes a mod's health number safe to hand back to the game.
	 *
	 * NaN and the infinities are rejected in favour of Fallback - they propagate through every later
	 * calculation and are impossible to trace back to the mod that produced them. Anything finite is
	 * clamped into [0, MaxEnemyHealth]; zero survives the clamp because "spawns dead" is a legitimate
	 * thing for a mod to ask for, and what it means is the game's decision, not this class's.
	 */
	float SanitizeHealth(const UGameEnemyExtension& Extension, const float Value, const float Fallback)
	{
		if (!FMath::IsFinite(Value))
		{
			UE_LOG(LogGameModSDK, Warning,
				TEXT("Enemy extension %s returned a non-finite value from ModifyEnemyHealth; the unmodified health %.3f was used instead."),
				*DescribeExtension(Extension), Fallback);
			return Fallback;
		}

		const float Clamped = FMath::Clamp(Value, 0.0f, UGameEnemyExtension::MaxEnemyHealth);
		if (Clamped != Value)
		{
			// Verbose, not Warning: this runs once per spawn and a mod that clamps every time must not
			// be able to make the log unusable for everybody else.
			UE_LOG(LogGameModSDK, Verbose,
				TEXT("Enemy extension %s returned %.3f from ModifyEnemyHealth; clamped to %.3f."),
				*DescribeExtension(Extension), Value, Clamped);
		}

		return Clamped;
	}
}

UGameEnemyExtension::UGameEnemyExtension()
{
	// Stamped here so that a Blueprint author never has to know - or mistype - the point id. A mod
	// that deliberately changes it in the class defaults simply registers against a different point,
	// and is rejected with Extension.PointNotFound if that point does not exist.
	ExtensionPointId = GameModExtensionPoints::Enemy;
}

UGameEnemyDefinition* UGameEnemyExtension::NativeGetEnemyDefinition() const
{
	if (!GameEnemyExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameEnemyExtension, GetEnemyDefinition)))
	{
		return nullptr;
	}

	// Whatever the mod returns is untrusted data: it may be null, and it may be an asset the game has
	// never heard of. The game validates it before using it - this class only carries it across.
	return GetEnemyDefinition();
}

float UGameEnemyExtension::NativeModifyEnemyHealth(float Base) const
{
	if (!GameEnemyExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameEnemyExtension, ModifyEnemyHealth)))
	{
		// Not implemented, or not dispatchable. Returning the game's own number is the only answer
		// that leaves the game exactly as it was.
		return Base;
	}

	const float Modified = ModifyEnemyHealth(Base);
	return GameEnemyExtensionPrivate::SanitizeHealth(*this, Modified, Base);
}

void UGameEnemyExtension::NativeOnEnemySpawned(AActor* Enemy)
{
	if (!GameEnemyExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameEnemyExtension, OnEnemySpawned)))
	{
		return;
	}

	// A null Enemy is forwarded rather than dropped: "something spawned the game could not name" is
	// a case a mod may legitimately want to see, and a Blueprint that dereferences it gets the
	// ordinary Blueprint null-access warning rather than a crash.
	OnEnemySpawned(Enemy);
}
