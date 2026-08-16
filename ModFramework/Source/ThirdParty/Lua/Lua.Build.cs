// Copyright (c) 2026. Licensed for use in your own projects.

using System.IO;
using UnrealBuildTool;

/// <summary>
/// Lua 5.5, compiled from vendored source.
///
/// Laid out to match the engine's own compiled-from-source third-party code (see
/// Engine/Source/ThirdParty/libpas). Keeping it a separate module rather than folding the .c files
/// into a consumer buys three things: licence auditing and SDK bundling find it where they expect,
/// upgrading is "replace this one folder", and none of the compiler relaxations below leak into our
/// own C++ - they apply to this module alone.
///
/// UPGRADING: drop a newer src/ in and rebuild. Do not edit the vendored sources - a local patch is
/// how a dependency becomes unupgradable, and every reason to reach for one is handled here instead.
/// If the API changed, the runtime that consumes it is ModFrameworkLua, not this module.
/// </summary>
public class Lua : ModuleRules
{
	public Lua(ReadOnlyTargetRules Target) : base(Target)
	{
		// Pure C with no UE types in it, so there is no module object to implement and nothing to
		// load at runtime - it links into its consumer. Saves putting a UE glue file inside vendored
		// third-party code.
		bRequiresImplementModule = false;

		// A C++ shared PCH cannot be injected into C translation units.
		PCHUsage = ModuleRules.PCHUsageMode.NoSharedPCHs;

		// Lua's .c files deliberately reuse short static names across translation units (luaO_*, and
		// numerous file-local helpers). Unity concatenates files, which turns that into redefinition
		// errors, so C is excluded from unity here. Our own C++ elsewhere keeps unity builds.
		bUseUnity = false;

		// Third-party C written to its own standards. UE promotes several of its idioms to errors,
		// and editing vendored source to silence them defeats the point of vendoring. The engine does
		// exactly this for libpas, including the same C4702.
		CppCompileWarningSettings.UnreachableCodeWarningLevel = WarningLevel.Off;
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Off;
		bEnableUndefinedIdentifierWarnings = false;

		string LuaSource = Path.Combine(ModuleDirectory, "src");

		// Consumers include <lua.h> directly, so the source directory is a PUBLIC include path.
		PublicIncludePaths.Add(LuaSource);

		// Lua's default on a C++ compiler is to signal errors with C++ exceptions, which UE builds
		// with disabled. Forcing setjmp/longjmp keeps error handling working - but note the
		// consequence, documented for whoever writes bindings: a Lua error unwinds the C stack
		// WITHOUT running C++ destructors.
		PublicDefinitions.Add("LUA_USE_LONGJMP=1");

		// REQUIRED because this is its own module. UE builds each module as a separate DLL, and
		// Lua's LUA_API expands to a bare `extern` by default - so every lua_* symbol came back
		// unresolved the moment the interpreter stopped compiling directly into its consumer.
		//
		// This is Lua's own mechanism (luaconf.h): with LUA_BUILD_AS_DLL, LUA_API becomes
		// __declspec(dllexport) in translation units that define LUA_CORE or LUA_LIB, and
		// __declspec(dllimport) everywhere else. Every vendored .c file already defines one of those
		// two itself, so exporting from here and importing in consumers falls out with no per-file
		// configuration. It is a PUBLIC definition precisely so consumers see the dllimport half.
		PublicDefinitions.Add("LUA_BUILD_AS_DLL=1");

		// Presence check, not a configuration switch. The sources are vendored rather than fetched,
		// so a fresh clone that skipped them still builds - the runtime compiles to a stub and
		// reports scripting unavailable instead of failing everyone's build.
		//
		// There is deliberately no LUA_ANSI define: it was removed after Lua 5.1 and is a no-op in
		// 5.5. Setting it would look like it compiled out io/os/package while doing nothing, which is
		// worse than not setting it. The sandbox is enforced at runtime in FModLuaRuntime, which
		// never calls luaL_openlibs.
		bool bHasSources = File.Exists(Path.Combine(LuaSource, "lua.h"))
			&& File.Exists(Path.Combine(LuaSource, "lapi.c"));

		PublicDefinitions.Add("MODFRAMEWORK_WITH_LUA=" + (bHasSources ? "1" : "0"));
	}
}
