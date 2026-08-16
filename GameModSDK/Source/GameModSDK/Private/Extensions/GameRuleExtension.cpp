// Copyright (c) 2026. Licensed for use in your own projects.

#include "Extensions/GameRuleExtension.h"

#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "Extensions/GameModSDKExtensionPoints.h"
#include "GameModSDKModule.h"
#include "Logging/LogMacros.h"
#include "Misc/AssertionMacros.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

namespace GameRuleExtensionPrivate
{
	/**
	 * "<mod id> (<class name>)" for a log line.
	 *
	 * Resolved defensively: this runs against mod-authored objects that may never have been
	 * registered and may be part-way through teardown, so nothing here dereferences without asking.
	 */
	FString DescribeExtension(const UGameRuleExtension& Extension)
	{
		const FModId& OwningModId = Extension.OwningModId;
		const FString ModIdText = OwningModId.IsValid() ? OwningModId.ToString() : FString(TEXT("<unregistered>"));

		const UClass* Class = Extension.GetClass();
		const FString ClassName = Class ? Class->GetName() : FString(TEXT("<null class>"));

		return FString::Printf(TEXT("%s (%s)"), *ModIdText, *ClassName);
	}

	/** True when HookName can be dispatched into script AND a Blueprint subclass implements it. */
	bool CanRunHook(const UGameRuleExtension& Extension, const FName HookName)
	{
		// A class default object has no mod behind it, and an object that has already been collected
		// during teardown cannot survive a ProcessEvent. A mod's extension can be either, so both are
		// refusals rather than assertions.
		if (Extension.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !::IsValid(&Extension))
		{
			return false;
		}

		// A BlueprintImplementableEvent nobody implemented resolves to a UFunction with no script;
		// ProcessEvent ignores it and the generated thunk hands back a zeroed return value. For
		// ShouldAllowSpawn that zero reads as "veto", which would let a mod that implemented nothing
		// stop the game from spawning anything at all. Ask before dispatching.
		const UClass* Class = Extension.GetClass();
		return Class != nullptr && Class->IsFunctionImplementedInScript(HookName);
	}
}

UGameRuleExtension::UGameRuleExtension()
{
	// Stamped here so that a Blueprint author never has to know - or mistype - the point id. A mod
	// that deliberately changes it in the class defaults simply registers against a different point,
	// and is rejected with Extension.PointNotFound if that point does not exist.
	ExtensionPointId = GameModExtensionPoints::GameRule;
}

void UGameRuleExtension::NativeOnWaveStarted(int32 WaveNumber)
{
	if (!GameRuleExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameRuleExtension, OnWaveStarted)))
	{
		return;
	}

	// WaveNumber comes from the game, not from a mod, and is forwarded exactly as given: a rule that
	// wants to reject wave 0 or a wave that went backwards has to be able to see that it happened.
	OnWaveStarted(WaveNumber);
}

void UGameRuleExtension::NativeOnGameStarted()
{
	if (!GameRuleExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameRuleExtension, OnGameStarted)))
	{
		return;
	}

	OnGameStarted();
}

bool UGameRuleExtension::NativeShouldAllowSpawn(FName EnemyId) const
{
	if (!GameRuleExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameRuleExtension, ShouldAllowSpawn)))
	{
		// A rule that cannot be consulted must never block the game: an extension whose mod is being
		// torn down, or one that implements no veto at all, has no opinion about this spawn.
		return true;
	}

	const bool bAllowed = ShouldAllowSpawn(EnemyId);
	if (!bAllowed)
	{
		// Verbose, not Warning: vetoing is the whole point of the hook, so it is trace output rather
		// than a problem. It is logged at all because "the wave is empty and nobody knows why" is
		// otherwise very hard to attribute to the mod that caused it.
		UE_LOG(LogGameModSDK, Verbose,
			TEXT("Rule extension %s vetoed the spawn of '%s'."),
			*GameRuleExtensionPrivate::DescribeExtension(*this), *EnemyId.ToString());
	}

	return bAllowed;
}
