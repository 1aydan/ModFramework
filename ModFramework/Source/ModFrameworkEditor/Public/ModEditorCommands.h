// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Styling/AppStyle.h"

/**
 * Command set for the Mod Framework editor tooling.
 *
 * TCommands supplies the static Register()/Unregister() that FModFrameworkEditorModule calls.
 */
class MODFRAMEWORKEDITOR_API FModEditorCommands : public TCommands<FModEditorCommands>
{
public:
	FModEditorCommands();

	//~ Begin TCommands
	virtual void RegisterCommands() override;
	//~ End TCommands

	/** Opens the Mod Developer window. */
	TSharedPtr<FUICommandInfo> OpenModDeveloper;
};
