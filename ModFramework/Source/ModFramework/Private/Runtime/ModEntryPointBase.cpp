// Copyright (c) 2026. Licensed for use in your own projects.

#include "Runtime/ModEntryPointBase.h"

#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "Logging/LogMacros.h"
#include "Runtime/ModContext.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

namespace ModEntryPointPrivate
{
	/**
	 * "<mod id> (<entry point class>)" for a log line, resolved defensively.
	 *
	 * An entry point is mod-authored, so this runs against an object that may be mid-teardown, may
	 * have lost its context, and may never have been given one.
	 */
	FString DescribeEntryPoint(const UModEntryPointBase& EntryPoint)
	{
		FString ModIdText(TEXT("<no context>"));
		if (const UModContext* ModContext = EntryPoint.Context.Get())
		{
			const FModId ModId = ModContext->GetModId();
			ModIdText = ModId.IsValid() ? ModId.ToString() : FString(TEXT("<unidentified mod>"));
		}

		const UClass* Class = EntryPoint.GetClass();
		const FString ClassName = Class ? Class->GetName() : FString(TEXT("<null class>"));

		return FString::Printf(TEXT("%s (%s)"), *ModIdText, *ClassName);
	}
}

void UModEntryPointBase::NativeOnModLoaded()
{
	UE_LOG(LogModFramework, Verbose, TEXT("Mod entry point %s: loaded."),
		*ModEntryPointPrivate::DescribeEntryPoint(*this));

	if (!CanDispatchScriptEvent(TEXT("NativeOnModLoaded")))
	{
		return;
	}

	// A BlueprintImplementableEvent that no Blueprint implements resolves to a UFunction with no
	// script, and ProcessEvent on it is a no-op. Calling this unconditionally is correct.
	OnModLoaded();
}

void UModEntryPointBase::NativeOnModActivated()
{
	UE_LOG(LogModFramework, Verbose, TEXT("Mod entry point %s: activated."),
		*ModEntryPointPrivate::DescribeEntryPoint(*this));

	if (!CanDispatchScriptEvent(TEXT("NativeOnModActivated")))
	{
		return;
	}

	OnModActivated();
}

void UModEntryPointBase::NativeOnModDeactivated()
{
	UE_LOG(LogModFramework, Verbose, TEXT("Mod entry point %s: deactivated."),
		*ModEntryPointPrivate::DescribeEntryPoint(*this));

	if (!CanDispatchScriptEvent(TEXT("NativeOnModDeactivated")))
	{
		return;
	}

	OnModDeactivated();
}

void UModEntryPointBase::NativeOnModUnloaded()
{
	UE_LOG(LogModFramework, Verbose, TEXT("Mod entry point %s: unloaded."),
		*ModEntryPointPrivate::DescribeEntryPoint(*this));

	if (!CanDispatchScriptEvent(TEXT("NativeOnModUnloaded")))
	{
		return;
	}

	OnModUnloaded();
}

UWorld* UModEntryPointBase::GetWorld() const
{
	// A class default object has the package as its outer and no meaningful world. Returning one would
	// let Blueprint latent nodes bind to the wrong context.
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return nullptr;
	}

	// The context is the intended route: it resolves the world through the mod subsystem's game
	// instance, which is the same world the rest of the mod runs in.
	if (const UModContext* ModContext = Context.Get())
	{
		if (UWorld* World = ModContext->GetWorld())
		{
			return World;
		}
	}

	// No context, or a context that could not resolve one. The outer chain is the fallback: an entry
	// point is outered to its context, so this normally reaches the same place by a longer road.
	if (const UObject* Owner = GetOuter())
	{
		return Owner->GetWorld();
	}

	return nullptr;
}

bool UModEntryPointBase::CanDispatchScriptEvent(const TCHAR* CallingFunction) const
{
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModEntryPointBase::%s was called on the class default object of '%s'. Blueprint events are not dispatched on CDOs; the call was ignored."),
			CallingFunction, *ModEntryPointPrivate::DescribeEntryPoint(*this));
		return false;
	}

	// Unloading a mod happens during teardown, where the entry point may already have been marked
	// garbage by an earlier release pass. ProcessEvent on such an object is not survivable, so the
	// lifecycle call is dropped instead.
	if (!::IsValid(this))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModEntryPointBase::%s was called on entry point %s after it became garbage; the call was ignored."),
			CallingFunction, *ModEntryPointPrivate::DescribeEntryPoint(*this));
		return false;
	}

	return true;
}
