// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModDeveloperWindow.h"

#include "ModFrameworkEditorModule.h"

#include "Framework/Docking/TabManager.h"
#include "Misc/CommandLine.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

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
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Settings")))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FModDeveloperWindow::Unregister()
{
	if (IsRunningCommandlet())
	{
		return;
	}

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

TSharedRef<SDockTab> FModDeveloperWindow::SpawnTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Placeholder", "Mod Developer"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("PlaceholderDetail",
						"Tooling is not implemented yet. Use the Mod.* console commands for now - "
						"see docs/ConsoleCommands.md."))
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
