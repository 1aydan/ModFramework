// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

/**
 * Registration for the Mod.* console commands - the framework's primary debugging surface.
 *
 * The command set is:
 *
 *   Mod.List [filter]        every registered mod, one aligned row each
 *   Mod.Info <ModId>         everything the framework knows about one mod
 *   Mod.Load <ModId>         mount (if needed) and load one mod
 *   Mod.Unload <ModId>       deactivate, unload and unmount one mod
 *   Mod.Activate <ModId>     load (if needed) and activate one mod
 *   Mod.Deactivate <ModId>   deactivate one mod, leaving it loaded
 *   Mod.Reload <ModId>       unload and bring one mod straight back
 *   Mod.ReloadAll            unload and bring every mod back
 *   Mod.Refresh [noactivate] run the whole discover/resolve/mount/load pipeline
 *   Mod.Validate [ModId]     re-run manifest validation, from disk where possible
 *   Mod.Dependencies <ModId> what one mod needs, what needs it, and how that resolved
 *   Mod.Mounts               every live content mount
 *   Mod.DumpRegistry         providers, environment, load order and the full mod table
 *   Mod.DumpAPIs             every registered game API
 *   Mod.DumpExtensions       every extension point and the extensions filed under it
 *   Mod.DumpEvents           every declared event type and its subscriber count
 *   Mod.DumpPermissions      the permission catalogue and every mod's decisions
 *   Mod.Conflicts            contested resources and how each was reconciled
 *   Mod.SessionManifest      what this install advertises across the network
 *   Mod.Enable <ModId>       switch one mod on for this session
 *   Mod.Disable <ModId>      switch one mod off for this session
 *   Mod.Icons [load <ModId>] which mods ship an icon and which icons are cached
 *
 * ORDERING HAZARD - READ BEFORE EDITING THE IMPLEMENTATION
 * ModFramework loads at PostConfigInit, BEFORE the UObject system exists. Register() must not read
 * UModFrameworkSettings and must not touch any CDO: the console command objects are created
 * unconditionally and the UModFrameworkSettings::bEnableConsoleCommands gate is evaluated inside
 * each command's execution callback. Getting that wrong crashes at engine start rather than at
 * command time, which is why it is called out here.
 *
 * Even at execution time the gate calls UObjectInitialized() before reading the settings, because a
 * command dispatched from an early -ExecCmds line can still beat the UObject system up: reading the
 * settings goes through GetDefault<>, and UClass::GetDefaultObject() constructs a missing CDO
 * rather than answering null, so a bare null check would not save it.
 *
 * Both functions are game thread only and are safe to call in either order, more than once, or not
 * at all. In a build where the console cannot be reached (`UE_BUILD_SHIPPING && !ALLOW_CONSOLE`)
 * they compile to empty bodies so the module still links.
 */
class FModConsoleCommands
{
public:
	/** Creates and registers every Mod.* command. A second call is ignored. */
	static void Register();

	/** Destroys the command objects, which unregisters them from the console manager. */
	static void Unregister();
};
