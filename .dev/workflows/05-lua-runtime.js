export const meta = {
	name: 'modframework-lua-runtime',
	description: 'FModLuaRuntime implementing IModScriptRuntime, plus the UModContext binding layer and sandbox',
	phases: [
		{ title: 'Runtime', detail: 'state lifecycle, sandbox, CPU and memory budgets' },
		{ title: 'Bindings', detail: 'UModContext exposed to Lua' },
	],
}

const REPO = 'F:/SelfProjects/Unreal/Plugins/ModFramework'
const MOD = `${REPO}/ModFramework/Source/ModFrameworkLua`
const LUA_SRC = `${MOD}/Private/ThirdParty/Lua/src`

const PREAMBLE = `You are implementing the Lua scripting runtime for a UE 5.8 modding framework that compiles clean.
Do not break that. Verify with ${REPO}/.dev/build-harness.ps1.

THE LUA VERSION IS 5.5.1, NOT 5.4. Its headers are on disk at ${LUA_SRC} and they are the ONLY
authority - do NOT write from Lua 5.4 knowledge, several signatures changed. Already verified:

  lua_State *lua_newstate(lua_Alloc f, void *ud, unsigned seed);   // THREE args in 5.5, two in 5.4
  typedef void *(*lua_Alloc)(void *ud, void *ptr, size_t osize, size_t nsize);
  void lua_setallocf(lua_State *L, lua_Alloc f, void *ud);
  void lua_sethook(lua_State *L, lua_Hook func, int mask, int count);   // LUA_MASKCOUNT exists
  #define lua_pcall(L,n,r,f) lua_pcallk(L,(n),(r),(f),0,NULL)
  void lua_setglobal(lua_State *L, const char *name);
  void luaL_requiref(lua_State *L, const char *modname, ...);

grep ${LUA_SRC}/lua.h, lauxlib.h and lualib.h for anything else BEFORE using it. A wrong signature
here is a silent memory-corruption bug, not a compile error, because these are C.

Lua is included like this - it is C, so extern "C" is mandatory or nothing links:
  THIRD_PARTY_INCLUDES_START
  extern "C" { #include "lua.h" \\n #include "lauxlib.h" \\n #include "lualib.h" }
  THIRD_PARTY_INCLUDES_END
Everything must be wrapped in #if MODFRAMEWORK_WITH_LUA so the module still builds without the
sources present. See the existing ${MOD}/Private/ModFrameworkLuaModule.cpp for the established
pattern - read it first.

READ THESE BEFORE WRITING:
  ${REPO}/ModFramework/Source/ModFramework/Public/Scripting/ModScriptRuntime.h   (the interface; its
      comments state the design intent and the reasoning - honour them)
  ${REPO}/ModFramework/Source/ModFramework/Public/Runtime/ModContext.h           (the ONLY thing bound)
  ${REPO}/ModFramework/Source/ModFramework/Public/Core/ModFrameworkTypes.h
  ${REPO}/docs/internal/Status.md                                                (state + gotchas)

ENGINE SOURCE: F:/SelfProjects/Unreal/UE_5.8/Engine/Source - grep to verify any UE API.

Style: Epic conventions, tabs, Allman braces, #pragma once, strict IWYU, every file starts with
  // Copyright (c) 2026. Licensed for use in your own projects.
then a blank line. Scripts are UNTRUSTED INPUT - never check() on anything a script controls.

Write complete implementations. No TODO stubs.

Return JSON: { "files": [...], "notes": "...", "concerns": [...] }`

const SCHEMA = {
	type: 'object',
	additionalProperties: false,
	required: ['files', 'notes', 'concerns'],
	properties: {
		files: { type: 'array', items: { type: 'string' } },
		notes: { type: 'string' },
		concerns: { type: 'array', items: { type: 'string' } },
	},
}

phase('Runtime')

const runtime = await agent(`${PREAMBLE}

YOUR SCOPE: FModLuaRuntime - state lifecycle, the sandbox, and resource budgets.
Write ${MOD}/Public/ModLuaRuntime.h and ${MOD}/Private/ModLuaRuntime.cpp.

class FModLuaRuntime : public IModScriptRuntime - implement EVERY member of the interface, including
the optional ones (SupportsResourceLimits returns true, SupportsHotReload returns true).

STATE PER MOD. One lua_State per FModId, held in a TMap. Two mods must never share a state: shared
globals would let one mod read or clobber another's data, and the interface's comments say isolation
is not optional.

THE SANDBOX IS THE ENTIRE POINT. Do NOT call luaL_openlibs - it opens io, os and package wholesale.
Open ONLY: base, table, string, math, and coroutine (via luaL_requiref with the individual
luaopen_* functions from lualib.h). Then, from the base library's globals, REMOVE:
  dofile, loadfile, load, require, collectgarbage, rawset/rawget are fine, print
Replace print with one that routes to UModContext::LogInfo so script output is attributed to the mod
rather than vanishing. Add a minimal os table with ONLY time and clock if you want them - never
os.execute, os.exit, os.remove, os.getenv. Document each removal with WHY in a comment: a future
reader will otherwise "helpfully" restore luaL_openlibs.

MEMORY BUDGET. Create the state with lua_newstate(Alloc, Ud, Seed) passing your own lua_Alloc. The
allocator tracks the running total for that mod and returns NULL once the budget is exceeded, which
Lua turns into a clean memory error rather than a crash. Pick the seed deterministically (a hash of
the mod id) rather than from a clock - determinism matters here for the same reason load order is
deterministic, and Date/rand are unavailable in this codebase by convention.

CPU BUDGET. lua_sethook with LUA_MASKCOUNT and an instruction count; the hook calls lua_error (or
luaL_error) to abort a runaway script. Budgets are per-call: reset the counter before each entry
point so a mod that legitimately does work every frame is not killed by cumulative drift.

ERROR HANDLING. Every call into Lua goes through lua_pcall with a message handler that captures a
traceback (luaL_traceback). Convert failures into FModDiagnostic with the script name and line -
"a mod's script errored" with no location is useless. Never let a Lua error escape into UE.

LoadScript takes bytes: use luaL_loadbufferx with mode "t" (TEXT ONLY - never allow precompiled
bytecode, which bypasses the parser and is a documented way to crash Lua deliberately).

Also declare, but do NOT implement, the binding entry point another agent is writing in parallel:
  namespace ModLuaBindings { bool InstallModContext(lua_State* L, UModContext* Context, FModDiagnostic& OutError); }
in a shared private header ${MOD}/Private/ModLuaBindings.h. Call it after building the sandbox.`,
	{ label: 'lua:runtime', phase: 'Runtime', schema: SCHEMA })

phase('Bindings')

const bindings = await agent(`${PREAMBLE}

YOUR SCOPE: the UModContext binding layer - the security-critical half. Implement
  ${MOD}/Private/ModLuaBindings.h   (declaration; another agent may have created it - read first)
  ${MOD}/Private/ModLuaBindings.cpp

READ ${MOD}/Public/ModLuaRuntime.h and ${MOD}/Private/ModLuaRuntime.cpp FIRST - they exist by now and
define how states are created and how errors are reported. Match their conventions exactly.

Expose UModContext as a global table named "mod". Bind ONLY what UModContext itself offers - that
class is deliberately the single mod-facing surface, so the binding list stays bounded and auditable.
Do not reach past it to UModSubsystem, UObject, reflection or engine globals; that would recreate
exactly the unrestricted access Blueprint has and throw away the reason Lua is here.

  mod.id()                                  -> string
  mod.log(msg) / mod.warn(msg) / mod.error(msg)
  mod.config_get(key, default)              -> typed by the default's Lua type
  mod.config_set(key, value)
  mod.config_save()
  mod.save(json_string) / mod.load()        -> string
  mod.has_permission(name)                  -> bool
  mod.request_api(id, version_range)        -> userdata handle or nil + error string
  mod.broadcast(event_id)                   -> bool
  mod.subscribe(event_id, fn)               -> handle
  mod.unsubscribe(handle)                   -> bool

RULES THAT MATTER MORE THAN THE FEATURE LIST:
- The UModContext pointer is held as a TWeakObjectPtr in userdata, NEVER a raw pointer captured in a
  closure. A script can outlive its context during teardown; a raw pointer there is a use-after-free
  reachable from mod content.
- Every binding validates its arguments with luaL_check*/luaL_opt* and fails with luaL_error rather
  than reading garbage off the stack. A script controls these values entirely.
- Every binding checks the context still resolves and returns nil + a message when it does not.
- Cap string arguments (reuse UModContext's MaxLogMessageChars where it applies) so a script cannot
  hand the engine a gigabyte string.
- mod.subscribe stores the Lua function in the registry (luaL_ref) keyed per mod, and the C++ side
  unrefs on unsubscribe and on context destruction - otherwise every subscription leaks a reference
  and pins the closure forever.
- Callbacks from C++ into Lua go through the same protected-call path the runtime uses, so an error
  inside a subscriber cannot propagate into UE's event dispatch.

Include a short comment block at the top of the .cpp stating plainly that this file IS the sandbox
boundary: anything added here is reachable by untrusted mod code, and additions should be weighed
accordingly.`,
	{ label: 'lua:bindings', phase: 'Bindings', schema: SCHEMA })

return { runtime, bindings }
