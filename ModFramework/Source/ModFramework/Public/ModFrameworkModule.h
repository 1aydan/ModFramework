// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Modules/ModuleInterface.h"

/**
 * Runtime module of the mod framework.
 *
 * Loaded at LoadingPhase "PostConfigInit", i.e. before the UObject system exists. Anything this
 * module does during StartupModule must therefore be free of UObject/CDO access - the actual mod
 * lifecycle lives in UModSubsystem (a UGameInstanceSubsystem), which comes up much later.
 *
 * Startup here is limited to owning the LogModFramework category and registering the debug console
 * commands, which only touch IConsoleManager.
 */
class MODFRAMEWORK_API FModFrameworkModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	/**
	 * Returns the loaded module, loading it first if necessary.
	 * Fatal if the module cannot be loaded; call IsAvailable() first if that is a possibility
	 * (for example during engine shutdown, or from another module's ShutdownModule).
	 */
	static FModFrameworkModule& Get();

	/** True when the module is already loaded, so Get() will not trigger a load. */
	static bool IsAvailable();

	/** The module name as registered with FModuleManager. */
	static const TCHAR* GetModuleName();
};
