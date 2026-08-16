// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

MODFRAMEWORKLUA_API DECLARE_LOG_CATEGORY_EXTERN(LogModLua, Log, All);

/**
 * The Lua scripting runtime for ModFramework.
 *
 * Separate from the ModFramework runtime module on purpose: it vendors a third-party C library, and
 * the core framework is otherwise dependency-free. Keeping it apart means a game that does not want
 * scripting simply does not enable this module, and the framework's own build stays clean.
 *
 * MODFRAMEWORK_WITH_LUA is 1 only when the Lua sources are present under
 * Private/ThirdParty/Lua/src. Without them this module still compiles and registers nothing, so a
 * missing dependency degrades to "scripting unavailable" rather than a build failure.
 */
class MODFRAMEWORKLUA_API FModFrameworkLuaModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface

	/** True when this build actually has an interpreter behind it. */
	static bool IsLuaAvailable();

	/** e.g. "Lua 5.4.8", or a message explaining why there is none. */
	static FString GetLuaVersionString();

	static const TCHAR* GetModuleName();
};
