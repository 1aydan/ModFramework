// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Manifest/ModVersion.h"
#include "Scripting/ModScriptRuntime.h"
#include "Templates/UniquePtr.h"
#include "UObject/NameTypes.h"

class UModContext;

/** One mod's Lua state. Defined in ModLuaRuntime.cpp; nothing outside it needs the layout. */
struct FModLuaState;

/**
 * Limits the Lua runtime enforces.
 *
 * These are RUNAWAY GUARDS, not budgets a mod is expected to live near - the same posture as
 * UModContext::MaxEventSubscriptions. A number tight enough to shape how a mod is written would
 * make the framework a performance authority it has no business being; a number loose enough to
 * never fire would make the guard decorative. They sit deliberately between the two.
 */
namespace ModLuaLimits
{
	/**
	 * Bytes one mod's state may hold before the allocator starts refusing. 64 MiB is far more than
	 * a script mod needs and far less than a leak needs to take a game down.
	 */
	inline constexpr int64 DefaultMemoryBudgetBytes = 64 * 1024 * 1024;

	/**
	 * Floor for SetMemoryBudget. Below roughly this the sandbox itself cannot be built, so a caller
	 * asking for less would get a mod that fails to initialise for reasons that read like a bug.
	 */
	inline constexpr int64 MinMemoryBudgetBytes = 512 * 1024;

	/**
	 * VM instructions one entry point may execute before the state is aborted. PER CALL, reset at
	 * every entry point: a mod that legitimately does work every frame must not be killed by
	 * cumulative drift, which is what a lifetime budget would do to it.
	 */
	inline constexpr int64 DefaultInstructionsPerCall = 10 * 1000 * 1000;

	/**
	 * How many instructions pass between count-hook firings. The hook is the only thing standing
	 * between a `while true do end` and a hung game, so it has to fire often enough to matter and
	 * rarely enough not to dominate the profile.
	 */
	inline constexpr int32 HookGranularity = 1000;

	/** Largest script source accepted by LoadScript. Untrusted bytes; the parser is not free. */
	inline constexpr int64 MaxScriptBytes = 8 * 1024 * 1024;

	/** Scripts remembered per mod for hot reload, so a pathological loader cannot grow forever. */
	inline constexpr int32 MaxRememberedScripts = 512;

	/** Characters kept from a Lua-produced string (log line, error, traceback) before truncation. */
	inline constexpr int32 MaxLuaTextChars = 16 * 1024;
}

/** Per-mod resource limits, settable before the mod's state exists. */
struct FModLuaBudget
{
	/** Bytes. Zero means unlimited - the allocator stops counting rather than stops refusing. */
	int64 MaxBytes = ModLuaLimits::DefaultMemoryBudgetBytes;

	/** VM instructions per entry point. Zero means unlimited - the count hook is not installed. */
	int64 MaxInstructionsPerCall = ModLuaLimits::DefaultInstructionsPerCall;
};

/**
 * IModScriptRuntime backed by Lua 5.5.
 *
 * ONE STATE PER MOD, NEVER SHARED. Two mods in one lua_State would share `_G`, so either could
 * read or clobber the other's data by accident or on purpose, and neither could be unloaded
 * without disturbing the other. ModScriptRuntime.h says isolation is not optional; a TMap keyed by
 * FModId is how that promise is kept. Nothing is ever hoisted into a shared parent state, not even
 * "harmless" caches - a shared cache is a shared mutable global with a friendlier name.
 *
 * THE SANDBOX IS THE WHOLE POINT. luaL_openlibs is never called: it opens `io`, `os`, `package`
 * and `debug` wholesale, which would hand every mod the filesystem, process control, arbitrary
 * chunk loading and the ability to walk out of the sandbox through the debug library. Only base,
 * table, string, math and coroutine are opened, individually, and the base library is then pruned.
 * Each removal is justified where it happens in ModLuaRuntime.cpp - read those comments before
 * "simplifying" the setup back into one call, which is exactly the change that silently deletes
 * the security boundary this module exists to provide.
 *
 * A CAPABILITY BOUNDARY IS NOT A RESOURCE BOUNDARY, so both are enforced. Every state is created
 * with an allocator that tracks the mod's running total and returns null past its budget, which
 * Lua turns into a clean memory error instead of an out-of-memory crash; and every entry point
 * runs with a count hook that aborts a script which will not stop. SupportsResourceLimits is
 * therefore true and means it.
 *
 * NO LUA ERROR EVER REACHES UE. Every call in goes through lua_pcall with a message handler that
 * captures a traceback, and comes back as an FModDiagnostic carrying the script name and line -
 * "a mod's script errored" without a location is not a diagnostic, it is a shrug.
 *
 * Threading: game thread only, like everything else in the framework. A lua_State is not
 * thread-safe and this class adds no locking.
 *
 * Without the Lua sources on disk (MODFRAMEWORK_WITH_LUA == 0) every method here fails cleanly
 * with a diagnostic saying so, so a game that has not vendored Lua still links and still runs.
 */
class MODFRAMEWORKLUA_API FModLuaRuntime final : public IModScriptRuntime
{
public:
	FModLuaRuntime();

	/** Closes every live state. Declared out of line so FModLuaState can stay opaque here. */
	virtual ~FModLuaRuntime() override;

	/** A runtime owns lua_States; copying one would double-close them. */
	FModLuaRuntime(const FModLuaRuntime&) = delete;
	FModLuaRuntime& operator=(const FModLuaRuntime&) = delete;

	//~ Begin IModScriptRuntime interface
	virtual FName GetRuntimeId() const override;
	virtual FText GetDisplayName() const override;
	virtual FModVersion GetRuntimeVersion() const override;
	virtual TArray<FString> GetSourceExtensions() const override;

	virtual bool CreateContext(const FModId& ModId, UModContext* Context, FModDiagnostic& OutError) override;
	virtual bool DestroyContext(const FModId& ModId) override;
	virtual bool HasContext(const FModId& ModId) const override;

	virtual bool LoadScript(const FModId& ModId, const FString& VirtualPath,
		TArrayView<const uint8> Source, FModDiagnostic& OutError) override;

	virtual bool CallFunction(const FModId& ModId, FName FunctionName, FModDiagnostic& OutError) override;
	virtual bool HasFunction(const FModId& ModId, FName FunctionName) const override;

	virtual bool SupportsResourceLimits() const override { return true; }
	virtual void SetInstructionBudget(const FModId& ModId, int64 MaxInstructionsPerCall) override;
	virtual void SetMemoryBudget(const FModId& ModId, int64 MaxBytes) override;

	virtual void CollectGarbage(const FModId& ModId) override;

	virtual bool SupportsHotReload() const override { return true; }
	virtual bool ReloadScripts(const FModId& ModId, FModDiagnostic& OutError) override;
	//~ End IModScriptRuntime interface

	/** True when this build actually vendored Lua. False makes every method above fail cleanly. */
	static bool IsAvailable();

	/** Closes every state. Safe to call twice; called by the destructor. */
	void DestroyAllContexts();

	/** Mods with a live state, in no particular order. */
	TArray<FModId> GetActiveContexts() const;

	/** Bytes currently held by one mod's state, or -1 when it has none. */
	int64 GetMemoryUsedBytes(const FModId& ModId) const;

	/** High-water mark of the above, or -1 when the mod has no state. */
	int64 GetPeakMemoryBytes(const FModId& ModId) const;

	/**
	 * Instructions the mod's most recent entry point executed, rounded up to the hook granularity,
	 * or -1 when it has no state. The cheap way to see which mod is eating the frame.
	 */
	int64 GetLastCallInstructions(const FModId& ModId) const;

	/** Scripts remembered for hot reload, or -1 when the mod has no state. */
	int32 GetLoadedScriptCount(const FModId& ModId) const;

	/** The limits that apply - or would apply - to one mod. */
	FModLuaBudget GetBudget(const FModId& ModId) const;

private:
#if MODFRAMEWORK_WITH_LUA
	/**
	 * The mod's state, or null. Const because the query methods are, and TUniquePtr hands back a
	 * mutable pointee from a const accessor - which is right here: the const-ness is the runtime's
	 * map, not the VM behind it.
	 */
	FModLuaState* FindState(const FModId& ModId) const;

	/**
	 * Builds a fully sandboxed, fully bound state for one mod without touching the map, so
	 * ReloadScripts can construct a replacement and only adopt it once every script has run.
	 */
	bool BuildState(const FModId& ModId, UModContext* Context,
		TUniquePtr<FModLuaState>& OutState, FModDiagnostic& OutError) const;

	/** Compiles Source as text-only and runs it, budgeted and protected. */
	bool CompileAndRun(FModLuaState& State, const FString& VirtualPath,
		TArrayView<const uint8> Source, FModDiagnostic& OutError) const;

	/**
	 * One mod's states, keyed by mod. TUniquePtr because a lua_State's allocator is handed the
	 * record's address - the map may rehash, so the record must not move with it.
	 */
	TMap<FModId, TUniquePtr<FModLuaState>> States;
#endif

	/**
	 * Limits per mod, kept separately from the states so SetMemoryBudget can be called before
	 * CreateContext - which it has to be, because a state's allocator is chosen at creation and
	 * cannot be retrofitted with a budget it never had.
	 */
	TMap<FModId, FModLuaBudget> Budgets;
};
