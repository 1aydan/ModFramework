// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModDeveloperWindow.h"

#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SModDeveloperWindow.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "FModDeveloperWindow"

const FName FModDeveloperWindow::TabId(TEXT("ModDeveloper"));

void FModDeveloperWindow::Register()
{
	// Commandlets (cook, SDK generation, automated packaging) run without a slate application, and
	// registering a nomad tab spawner there asserts. Guarded symmetrically with Unregister().
	if (IsRunningCommandlet())
	{
		return;
	}

	FGlobalTabmanager::Get()
		->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateStatic(&FModDeveloperWindow::SpawnTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Mod Developer"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Inspect, validate and package mods."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Package")))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	// No Tools menu entry is added here on purpose. FModSDKWindow::Register owns the single
	// "Mod Framework" section and files this window's entry into it alongside its own, so the two
	// windows cannot end up in two separately-labelled sections whose order depends on which startup
	// callback fired first. The tab is independently reachable through Window > Tools via the
	// workspace group set above, so the menu is a convenience rather than the only route in.
}

void FModDeveloperWindow::Unregister()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	// Nothing to withdraw from UToolMenus here: FModSDKWindow::Unregister drops the whole
	// "Mod Framework" section, including this window's entry, under its own menu owner.
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
}

void FModDeveloperWindow::Open()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	FGlobalTabmanager::Get()->TryInvokeTab(TabId);
}

TSharedRef<SDockTab> FModDeveloperWindow::SpawnTab(const FSpawnTabArgs& /*Args*/)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SModDeveloperWindow)
		];
}

#undef LOCTEXT_NAMESPACE
