// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModEditorCommands.h"

#define LOCTEXT_NAMESPACE "FModEditorCommands"

FModEditorCommands::FModEditorCommands()
	: TCommands<FModEditorCommands>(
		TEXT("ModFramework"),
		NSLOCTEXT("Contexts", "ModFramework", "Mod Framework"),
		NAME_None,
		FAppStyle::GetAppStyleSetName())
{
}

void FModEditorCommands::RegisterCommands()
{
	UI_COMMAND(OpenModDeveloper, "Mod Developer",
		"Open the Mod Developer window: mod list, validation, dependency graph, packaging.",
		EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
