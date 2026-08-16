// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Delegates/IDelegateInstance.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class FSpawnTabArgs;
class SDockTab;

/**
 * The Mod SDK window: the *game developer's* tooling.
 *
 * This is the studio side of the framework and it never ships to anybody. It generates the SDK bundle
 * a mod author installs, and it inspects the three things that bundle promises: the public API
 * surface, the extension points the game opened, and the permission catalogue. Nothing here is useful
 * to somebody who received an SDK, which is why it lives in ModFrameworkEditor - a module no bundle
 * ever copies - and behind its own menu entry rather than inside the mod-author window.
 *
 * ONE MENU SECTION, REGISTERED ONCE
 * Register() also installs the shared "Mod Framework" section under Tools, which carries the entries
 * for *both* windows: this one and FModDeveloperWindow. Putting both in one place keeps the section
 * from being created twice under two owners and then ordered arbitrarily. FModDeveloperWindow
 * therefore has no menu registration of its own - it only registers its tab spawner - and both are
 * still registered from FModFrameworkEditorModule::StartupModule.
 */
class MODFRAMEWORKEDITOR_API FModSDKWindow
{
public:
	/**
	 * Registers the nomad tab spawner and the shared Tools menu section.
	 *
	 * A no-op during commandlet runs: SDK generation itself runs as a commandlet, and there is no tab
	 * manager or tool menu to register against in that process.
	 */
	static void Register();

	/** Symmetric with Register, including the menu owner and the deferred startup callback. */
	static void Unregister();

	/** Brings the window up, invoking the tab if it is not already open. */
	static void Open();

	static const FName TabId;

	/** Owner every menu entry this class adds is filed under, so Unregister can drop them all at once. */
	static const FName MenuOwnerName;

private:
	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);

	/**
	 * Adds the "Mod Framework" section to the Tools menu.
	 *
	 * Runs through UToolMenus::RegisterStartupCallback rather than directly, because module startup
	 * can happen before UToolMenus has finished its own registration pass; the callback fires
	 * immediately when it is already up.
	 */
	static void RegisterMenus();

	/** Handle of the deferred RegisterMenus callback, so Unregister can withdraw it if it never ran. */
	static FDelegateHandle ToolMenusStartupHandle;
};
