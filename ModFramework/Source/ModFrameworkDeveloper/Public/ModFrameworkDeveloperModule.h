// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"

/**
 * Log category for the authoring-side tooling: the .mod package writer, manifest authoring helpers
 * and the SDK generator. Kept separate from LogModFramework so a game shipping the runtime module
 * never sees tooling chatter, and so tooling verbosity can be raised on its own.
 */
MODFRAMEWORKDEVELOPER_API DECLARE_LOG_CATEGORY_EXTERN(LogModFrameworkDeveloper, Log, All);

/**
 * Authoring-side module of the mod framework.
 *
 * "UncookedOnly": present in editor and program targets, never in a cooked game. It owns the
 * pieces that create mods rather than consume them - FModPackageWriter, the SDK generator and the
 * manifest authoring helpers - so none of that code can be reached from a shipped title.
 *
 * It carries no headless state: startup and shutdown only bracket the log category.
 */
class MODFRAMEWORKDEVELOPER_API FModFrameworkDeveloperModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	/** Returns the loaded module, loading it first if necessary. Fatal if it cannot be loaded. */
	static FModFrameworkDeveloperModule& Get();

	/** True when the module is already loaded, so Get() will not trigger a load. */
	static bool IsAvailable();

	/** The module name as registered with FModuleManager. */
	static const TCHAR* GetModuleName();
};
