// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class FSpawnTabArgs;
class SDockTab;

/**
 * The Mod Developer window: the mod author's primary tool surface, and the one piece of editor
 * tooling that ships inside the SDK bundle rather than staying with the game team.
 *
 * This class owns nothing but the registration. The window itself is SModDeveloperWindow, and its
 * five pages - mods, validation, dependencies, conflicts and packaging - are the S*Panel widgets
 * beside it. Keeping the registration separate is what lets the tab be spawned by the editor's own
 * layout restore, by the Tools menu and by Open() without three copies of the construction code.
 *
 * Everything here is guarded on !IsRunningCommandlet(). A commandlet - cook, SDK generation,
 * automated packaging - runs without a Slate application, and registering a nomad tab spawner there
 * asserts. Register, Unregister and Open all check it, so the guard cannot go one-sided.
 */
class MODFRAMEWORKEDITOR_API FModDeveloperWindow
{
public:
	/**
	 * Registers the nomad tab spawner. No-op during commandlet runs.
	 *
	 * The Tools menu entry for this window is *not* registered here: FModSDKWindow::Register owns the
	 * single "Mod Framework" section that carries the entries for both windows. Two registrations
	 * would produce two sections, with their labels and ordering decided by whichever startup
	 * callback happened to fire first.
	 */
	static void Register();

	/** Removes the tab spawner. Safe when Register never ran. */
	static void Unregister();

	/** Brings the window up, invoking the tab if it is not already open. */
	static void Open();

	static const FName TabId;

private:
	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);
};
