// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModFrameworkLuaModule.h"

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

void FModFrameworkLuaModule::StartupModule()
{
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
