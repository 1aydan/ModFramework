// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"

/**
 * Log category for the editor-facing UI: the mod developer window, the editor commands and the
 * detail customisations. Separate from LogModFramework and LogModFrameworkDeveloper so UI noise
 * never pollutes the runtime or the packaging logs.
 */
MODFRAMEWORKEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogModFrameworkEditor, Log, All);

/**
 * Editor module of the mod framework.
 *
 * "Editor" type, so it only ever exists inside the editor. It owns the user-facing surface:
 * the mod developer window (FModDeveloperWindow) and the editor command set
 * (FModEditorCommands). Both live in this module and are registered from StartupModule and
 * torn down in the reverse order from ShutdownModule.
 */
class MODFRAMEWORKEDITOR_API FModFrameworkEditorModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	/** Returns the loaded module, loading it first if necessary. Fatal if it cannot be loaded. */
	static FModFrameworkEditorModule& Get();

	/** True when the module is already loaded, so Get() will not trigger a load. */
	static bool IsAvailable();

	/** The module name as registered with FModuleManager. */
	static const TCHAR* GetModuleName();
};
