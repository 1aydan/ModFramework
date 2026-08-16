// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModSDKWindow.h"

#include "ModDeveloperWindow.h"
#include "ModEditorCommands.h"
#include "ModFrameworkEditorModule.h"
#include "Widgets/SModSDKPanel.h"

#include "CoreGlobals.h"
#include "Delegates/Delegate.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Docking/TabManager.h"
#include "Internationalization/Internationalization.h"
#include "Logging/LogMacros.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "FModSDKWindow"

const FName FModSDKWindow::TabId(TEXT("ModSDK"));
const FName FModSDKWindow::MenuOwnerName(TEXT("ModFrameworkEditor.ModFrameworkMenuSection"));

FDelegateHandle FModSDKWindow::ToolMenusStartupHandle;

namespace ModSDKWindowPrivate
{
	/** The Tools menu of the level editor. Registered by FLevelEditorMenu; extended, never created. */
	const TCHAR* const ToolsMenuName = TEXT("LevelEditor.MainMenu.Tools");

	/** Section both windows' entries live in. */
	const FName SectionName(TEXT("ModFramework"));
}

void FModSDKWindow::Register()
{
	// Commandlets - including GenerateModSDK, which does this window's job unattended - run without a
	// slate application, and registering a nomad tab spawner there asserts.
	if (IsRunningCommandlet())
	{
		return;
	}

	FGlobalTabmanager::Get()
		->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateStatic(&FModSDKWindow::SpawnTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Mod SDK"))
		.SetTooltipText(LOCTEXT("TabTooltip",
			"Generate the SDK bundle and inspect the public API, extension points and permissions it publishes."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Settings")))
		// Also listed under Window > Tools, so both windows stay reachable even on an editor layout
		// where the Tools *menu* could not be extended.
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	if (UToolMenus::IsToolMenuUIEnabled())
	{
		ToolMenusStartupHandle = UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateStatic(&FModSDKWindow::RegisterMenus));
	}
}

void FModSDKWindow::Unregister()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	if (ToolMenusStartupHandle.IsValid())
	{
		UToolMenus::UnRegisterStartupCallback(ToolMenusStartupHandle);
		ToolMenusStartupHandle.Reset();
	}

	// The static spelling goes through TryGet, so it is a no-op once the ToolMenus singleton is gone -
	// which is the normal case during editor shutdown.
	UToolMenus::UnregisterOwner(MenuOwnerName);

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
}

void FModSDKWindow::Open()
{
	if (IsRunningCommandlet())
	{
		return;
	}

	FGlobalTabmanager::Get()->TryInvokeTab(TabId);
}

TSharedRef<SDockTab> FModSDKWindow::SpawnTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SModSDKPanel)
		];
}

void FModSDKWindow::RegisterMenus()
{
	UToolMenus* ToolMenus = UToolMenus::TryGet();
	if (!ToolMenus)
	{
		return;
	}

	// Everything below is filed under one owner so Unregister can drop the whole section in one call.
	FToolMenuOwnerScoped OwnerScoped(MenuOwnerName);

	UToolMenu* ToolsMenu = ToolMenus->ExtendMenu(ModSDKWindowPrivate::ToolsMenuName);
	if (!ToolsMenu)
	{
		UE_LOG(LogModFrameworkEditor, Verbose,
			TEXT("The level editor Tools menu is not available, so the Mod Framework menu section was not added. ")
			TEXT("Both windows are still reachable through Window > tab search."));
		return;
	}

	FToolMenuSection& Section = ToolsMenu->FindOrAddSection(
		ModSDKWindowPrivate::SectionName,
		LOCTEXT("SectionLabel", "Mod Framework"));

	// Plain execute actions rather than FUICommandInfo *entries*: a command entry renders disabled
	// unless the menu is built with a command list that has the command bound, and the level editor's
	// Tools menu carries its own. The label and tooltip are still read off the command set where one
	// exists, so a menu entry and any future keybinding always describe the same action.
	const TSharedPtr<FUICommandInfo>& DeveloperCommand = FModEditorCommands::Get().OpenModDeveloper;

	Section.AddMenuEntry(
		TEXT("OpenModDeveloperWindow"),
		DeveloperCommand.IsValid()
			? DeveloperCommand->GetLabel()
			: LOCTEXT("OpenModDeveloperLabel", "Mod Developer"),
		DeveloperCommand.IsValid()
			? DeveloperCommand->GetDescription()
			: LOCTEXT("OpenModDeveloperTooltip",
				"The mod-author tools: browse mods, validate manifests, and cook and package a mod."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Package")),
		FToolUIActionChoice(FExecuteAction::CreateStatic(&FModDeveloperWindow::Open)));

	Section.AddMenuEntry(
		TEXT("OpenModSDKWindow"),
		LOCTEXT("OpenModSDKLabel", "Mod SDK"),
		LOCTEXT("OpenModSDKTooltip",
			"The game-developer tools: generate the SDK bundle, and inspect the public API, extension points and "
			"permissions it publishes."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Settings")),
		FToolUIActionChoice(FExecuteAction::CreateStatic(&FModSDKWindow::Open)));
}

#undef LOCTEXT_NAMESPACE
