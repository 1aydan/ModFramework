// Copyright (c) 2026. Licensed for use in your own projects.

#include "Extensions/ModExtension.h"

#include "Containers/UnrealString.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

void UModExtension::OnExtensionRegistered()
{
	ReceiveOnRegistered();
}

void UModExtension::OnExtensionActivated()
{
	ReceiveOnActivated();
}

void UModExtension::OnExtensionDeactivated()
{
	ReceiveOnDeactivated();
}

void UModExtension::OnExtensionUnregistered()
{
	ReceiveOnUnregistered();
}

FName UModExtension::GetResolvedExtensionId() const
{
	if (!ExtensionId.IsNone())
	{
		return ExtensionId;
	}

	// GetClass() is never null for a live UObject, but this runs on mod-authored objects that may be
	// in odd states during teardown, so fall back to a stable literal rather than dereferencing null.
	const UClass* Class = GetClass();
	const FString ClassName = Class ? Class->GetName() : TEXT("UnknownClass");

	return FName(*(OwningModId.ToString() + TEXT(":") + ClassName));
}

bool UModExtension::IsExtensionActive() const
{
	return bExtensionActive;
}

UWorld* UModExtension::GetWorld() const
{
	// A class default object (or an archetype) has the package as its outer and no meaningful world.
	// Returning a world for it would let Blueprint latent nodes bind to the wrong context.
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return nullptr;
	}

	// Mods are handed a UModContext whose outer chain ends at the game instance, so walking outwards
	// gives a Blueprint extension the same world its owning mod runs in. UObject::GetWorld already
	// terminates at a null outer, which is what a transient-package-owned extension hits.
	if (const UObject* Owner = GetOuter())
	{
		return Owner->GetWorld();
	}

	return nullptr;
}
