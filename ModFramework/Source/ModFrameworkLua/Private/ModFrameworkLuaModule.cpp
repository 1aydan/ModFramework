// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModFrameworkLuaModule.h"

#include "ModLuaRuntime.h"
#include "Scripting/ModScriptManager.h"
#include "Scripting/ModScriptRuntime.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

#if MODFRAMEWORK_WITH_LUA
// Lua is C. Everything from it must be reached through extern "C" or the symbols will not link.
THIRD_PARTY_INCLUDES_START
extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
THIRD_PARTY_INCLUDES_END
#endif

DEFINE_LOG_CATEGORY(LogModLua);

/**
 * Private to this translation unit. A named namespace rather than an anonymous one so that a symbol
 * in a stack trace or a linker message says where it came from.
 */
namespace ModFrameworkLuaModulePrivate
{
	/**
	 * The id a manifest writes in `entryPoint.scriptRuntime`, and the id FModLuaRuntime reports.
	 *
	 * One spelling, used by both the factory registration and its removal. It has to agree with
	 * FModLuaRuntime::GetRuntimeId or UModScriptManager registers the runtime under its own id and
	 * ShutdownModule takes back a recipe nobody registered.
	 */
	FName GetLuaRuntimeId()
	{
		return FName(TEXT("Lua"));
	}

	/**
	 * Builds one Lua runtime. Called once per UModScriptManager, never once per process.
	 *
	 * A runtime is produced even in a build with no interpreter. FModLuaRuntime answers every call
	 * with a diagnostic that says the Lua sources were not vendored, and that is a far more useful
	 * answer for a mod author than "the 'Lua' runtime is not registered", which is what refusing to
	 * register here would produce.
	 */
	TSharedPtr<IModScriptRuntime> CreateLuaRuntime()
	{
		return MakeShared<FModLuaRuntime>();
	}
}

void FModFrameworkLuaModule::StartupModule()
{
	// HOW A RUNTIME REACHES A GAME.
	//
	// This is a plain runtime module: it starts with the engine, and it cannot hand an
	// IModScriptRuntime to UModScriptManager because there is no game instance yet - there may
	// eventually be several (Play In Editor with two clients), and there may never be one at all.
	// So it registers a *recipe* on a module-global list, and every script manager that comes up
	// later builds its own runtime from it. One state per mod per game instance, which is the
	// isolation IModScriptRuntime demands, one level up.
	//
	// The dependency only ever points this way. ModFramework never names Lua, never includes a Lua
	// header and never links this module; a game that wants no scripting simply does not enable it.
	UModScriptManager::RegisterRuntimeFactory(
		ModFrameworkLuaModulePrivate::GetLuaRuntimeId(),
		FModScriptRuntimeFactory::CreateStatic(&ModFrameworkLuaModulePrivate::CreateLuaRuntime));

#if MODFRAMEWORK_WITH_LUA
	// Prove the interpreter is genuinely linked, not merely compiled: create a state, run a trivial
	// chunk and tear it down. If Lua were misconfigured - wrong error mode, missing symbols - this
	// fails loudly at startup instead of at the moment a mod first runs a script.
	lua_State* Probe = luaL_newstate();
	if (Probe == nullptr)
	{
		UE_LOG(LogModLua, Error, TEXT("Lua is compiled in but a state could not be created."));
		return;
	}

	const int Result = luaL_dostring(Probe, "return 1 + 1");
	const bool bWorks = (Result == LUA_OK) && lua_isnumber(Probe, -1) && (lua_tointeger(Probe, -1) == 2);
	lua_close(Probe);

	if (bWorks)
	{
		UE_LOG(LogModLua, Log, TEXT("%s ready."), *GetLuaVersionString());
	}
	else
	{
		UE_LOG(LogModLua, Error, TEXT("Lua linked but failed a basic evaluation; scripting will be unavailable."));
	}
#else
	UE_LOG(LogModLua, Log,
		TEXT("Built without Lua. Drop the Lua 5.4 sources into ")
		TEXT("ModFramework/Source/ModFrameworkLua/Private/ThirdParty/Lua/src and rebuild."));
#endif
}

void FModFrameworkLuaModule::ShutdownModule()
{
	// Runtimes a script manager already built from this recipe are owned by that manager and go with
	// it; taking the recipe back only stops a manager created after this point from building another
	// out of code that is about to be unloaded.
	UModScriptManager::UnregisterRuntimeFactory(ModFrameworkLuaModulePrivate::GetLuaRuntimeId());
}

bool FModFrameworkLuaModule::IsLuaAvailable()
{
#if MODFRAMEWORK_WITH_LUA
	return true;
#else
	return false;
#endif
}

FString FModFrameworkLuaModule::GetLuaVersionString()
{
#if MODFRAMEWORK_WITH_LUA
	return FString(ANSI_TO_TCHAR(LUA_RELEASE));
#else
	return TEXT("Lua is not compiled into this build");
#endif
}

const TCHAR* FModFrameworkLuaModule::GetModuleName()
{
	return TEXT("ModFrameworkLua");
}

IMPLEMENT_MODULE(FModFrameworkLuaModule, ModFrameworkLua)
