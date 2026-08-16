// Copyright (c) 2026. Licensed for use in your own projects.

#include "Extensions/GameUIExtension.h"

#include "Blueprint/UserWidget.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "Extensions/GameModSDKExtensionPoints.h"
#include "GameModSDKModule.h"
#include "Logging/LogMacros.h"
#include "Misc/AssertionMacros.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

namespace GameUIExtensionPrivate
{
	/**
	 * "<mod id> (<class name>)" for a log line.
	 *
	 * Resolved defensively: this runs against mod-authored objects that may never have been
	 * registered and may be part-way through teardown, so nothing here dereferences without asking.
	 */
	FString DescribeExtension(const UGameUIExtension& Extension)
	{
		const FModId& OwningModId = Extension.OwningModId;
		const FString ModIdText = OwningModId.IsValid() ? OwningModId.ToString() : FString(TEXT("<unregistered>"));

		const UClass* Class = Extension.GetClass();
		const FString ClassName = Class ? Class->GetName() : FString(TEXT("<null class>"));

		return FString::Printf(TEXT("%s (%s)"), *ModIdText, *ClassName);
	}

	/** True when HookName can be dispatched into script AND a Blueprint subclass implements it. */
	bool CanRunHook(const UGameUIExtension& Extension, const FName HookName)
	{
		// A class default object has no mod behind it, and an object that has already been collected
		// during teardown cannot survive a ProcessEvent. A mod's extension can be either, so both are
		// refusals rather than assertions.
		if (Extension.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !::IsValid(&Extension))
		{
			return false;
		}

		// A BlueprintImplementableEvent nobody implemented resolves to a UFunction with no script;
		// ProcessEvent ignores it and the generated thunk hands back a zeroed return value. A zeroed
		// sort priority is not the same as "the extension's own Priority", so ask before dispatching.
		const UClass* Class = Extension.GetClass();
		return Class != nullptr && Class->IsFunctionImplementedInScript(HookName);
	}
}

UGameUIExtension::UGameUIExtension()
{
	// Stamped here so that a Blueprint author never has to know - or mistype - the point id. A mod
	// that deliberately changes it in the class defaults simply registers against a different point,
	// and is rejected with Extension.PointNotFound if that point does not exist.
	ExtensionPointId = GameModExtensionPoints::UI;
}

TSubclassOf<UUserWidget> UGameUIExtension::NativeGetWidgetClass() const
{
	if (!GameUIExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameUIExtension, GetWidgetClass)))
	{
		return nullptr;
	}

	// TSubclassOf::Get already returns null for anything that is not a UUserWidget subclass, so a mod
	// that wired the wrong class into the node is handled by the same path as a mod that returned
	// nothing at all.
	UClass* WidgetClass = GetWidgetClass().Get();
	if (WidgetClass == nullptr)
	{
		return nullptr;
	}

	// Everything from here down is a class the game would otherwise hand to CreateWidget. Each of
	// these states makes that fail - loudly and far from the mod that caused it - so they are refused
	// here, where the mod can still be named in the message.
	if (!::IsValid(WidgetClass))
	{
		UE_LOG(LogGameModSDK, Warning,
			TEXT("UI extension %s returned a widget class that is no longer valid; no widget will be created for it."),
			*GameUIExtensionPrivate::DescribeExtension(*this));
		return nullptr;
	}

	if (WidgetClass->HasAnyClassFlags(CLASS_Abstract))
	{
		UE_LOG(LogGameModSDK, Warning,
			TEXT("UI extension %s returned the abstract widget class '%s'; abstract classes cannot be constructed, so no widget will be created for it."),
			*GameUIExtensionPrivate::DescribeExtension(*this), *WidgetClass->GetName());
		return nullptr;
	}

	if (WidgetClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		// CLASS_NewerVersionExists is the state a Blueprint class is left in after a recompile in the
		// editor: the object is still reachable but nothing may be constructed from it any more.
		UE_LOG(LogGameModSDK, Warning,
			TEXT("UI extension %s returned the deprecated or superseded widget class '%s'; no widget will be created for it."),
			*GameUIExtensionPrivate::DescribeExtension(*this), *WidgetClass->GetName());
		return nullptr;
	}

	return WidgetClass;
}

int32 UGameUIExtension::NativeGetSortPriority() const
{
	if (!GameUIExtensionPrivate::CanRunHook(*this, GET_FUNCTION_NAME_CHECKED(UGameUIExtension, GetSortPriority)))
	{
		// The extension's own Priority is the honest default: it is already what the registry sorts
		// this point by, so a mod that never implemented the hook still lands where it expects to.
		return Priority;
	}

	return GetSortPriority();
}
