// Copyright (c) 2026. Licensed for use in your own projects.

#include "Events/ModEventTypes.h"

#include "UObject/NameTypes.h"

/**
 * The framework's own event ids.
 *
 * The string values are part of the binding contract: they are what a mod author types into a
 * Blueprint "Subscribe To Mod Event" node and what tooling prints, so they must never change.
 * FName construction during static initialisation is safe - the name table initialises lazily on
 * first use - and doing it here means the ids are interned exactly once per process.
 */
namespace ModFrameworkEvents
{
	const FName ModDiscovered(TEXT("Mod.Discovered"));
	const FName ModValidated(TEXT("Mod.Validated"));
	const FName ModMounted(TEXT("Mod.Mounted"));
	const FName ModLoaded(TEXT("Mod.Loaded"));
	const FName ModActivated(TEXT("Mod.Activated"));
	const FName ModDeactivated(TEXT("Mod.Deactivated"));
	const FName ModUnloaded(TEXT("Mod.Unloaded"));
	const FName ModUnmounted(TEXT("Mod.Unmounted"));
	const FName ModFailed(TEXT("Mod.Failed"));
	const FName ModsRefreshed(TEXT("Mod.Refreshed"));
	const FName WorldCreated(TEXT("World.Created"));
	const FName WorldDestroyed(TEXT("World.Destroyed"));
	const FName GameStarted(TEXT("Game.Started"));
}
