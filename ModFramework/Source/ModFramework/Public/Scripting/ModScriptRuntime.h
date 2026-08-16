// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Core/ModFrameworkTypes.h"
#include "CoreMinimal.h"
#include "Manifest/ModVersion.h"

class UModContext;

/**
 * The seam a scripting language plugs into. NO IMPLEMENTATION SHIPS WITH THE FRAMEWORK.
 *
 * Scripting is deliberately not part of the MVP (see docs/internal/Status.md). This interface exists
 * now so that adding a runtime later is one implementation rather than a refactor - the same reason
 * IModProvider hides where mods come from and IModContentMounter hides Pak/IoStore.
 *
 * LUA IS THE INTENDED FIRST IMPLEMENTATION. The reasoning, recorded so it is not re-argued:
 *
 *   - It has ZERO ENGINE COUPLING. A studio that forks the engine, changes struct layouts, or ships
 *     on a console that forbids dynamic code loading breaks native mods completely and may break
 *     Blueprint cooking - but a Lua script is unaffected, because it only ever sees the API bound
 *     into it. It is the only scripting path that survives a decade of engine divergence.
 *   - It is the lingua franca of game modding, so the largest number of people can write a mod on
 *     day one. Reach beats elegance for a modding scene.
 *   - It is the first path where the permission system becomes ENFORCEABLE rather than advisory.
 *     Blueprint runs in-process with engine-wide reach; docs/Security.md is explicit that
 *     permissions are API access control, not a sandbox. A script VM only sees what is bound, so
 *     "this mod may not touch the filesystem" can finally be true.
 *
 * AngelScript was considered. Vanilla embedded AngelScript is viable and its static typing fits this
 * framework's habit of failing before loading rather than during play. UnrealEngineAngelscript is
 * NOT viable here: it is an engine fork, and its bindings are auto-generated from UE reflection, so
 * scripts would reach everything reflected - the opposite of a capability boundary.
 *
 * WebAssembly is the only option offering real MEMORY isolation rather than API-surface gating. This
 * interface deliberately does not assume a source-text interpreter, so a WASM runtime could
 * implement it: LoadScript takes bytes, not a string.
 */
/**
 * DELIBERATELY NOT MODFRAMEWORK_API, unlike this framework's other interfaces.
 *
 * Every member here is pure virtual or inline, and the class comment above says an implementation
 * never ships with the framework - so nothing inside ModFramework ever constructs one. On MSVC a
 * dllexported class whose implicit constructor and destructor are never emitted exports no such
 * symbols, while a *consumer* in another module sees them as dllimport and refuses to generate its
 * own. The result is that the first person to implement this interface - which is the only way it
 * is ever used - gets two unresolved externals for functions that do nothing. Without the macro
 * they are generated inline in each implementing module, which is what a header-only interface
 * wants (IModuleInterface is declared the same way). Do not "restore" it for consistency.
 */
class IModScriptRuntime
{
public:
	virtual ~IModScriptRuntime() = default;

	/** Stable id, e.g. "Lua". Used by a manifest to request a runtime. */
	virtual FName GetRuntimeId() const = 0;
	virtual FText GetDisplayName() const = 0;
	/** The runtime's own version, so a mod can require a range of it like any other dependency. */
	virtual FModVersion GetRuntimeVersion() const = 0;
	/** Lower-case, dot-prefixed, e.g. { ".lua" }. */
	virtual TArray<FString> GetSourceExtensions() const = 0;

	/**
	 * Creates an isolated execution context for one mod.
	 *
	 * ISOLATION IS NOT OPTIONAL. One context per mod, with no shared mutable global state: two mods
	 * must not be able to see or clobber each other's globals. A runtime that cannot guarantee that
	 * should refuse to initialise rather than pretend.
	 *
	 * THE CONTEXT IS THE ONLY THING BOUND. An implementation binds UModContext's surface and nothing
	 * else - no raw UObject access, no reflection walker, no engine globals. That restraint is the
	 * entire reason a script sandbox is worth more than Blueprint, and UModContext is deliberately
	 * the single mod-facing object so the binding list stays bounded and auditable.
	 */
	virtual bool CreateContext(const FModId& ModId, UModContext* Context, FModDiagnostic& OutError) = 0;
	virtual bool DestroyContext(const FModId& ModId) = 0;
	virtual bool HasContext(const FModId& ModId) const = 0;

	/**
	 * Compiles and runs a script. Source is bytes rather than a string so a bytecode or WASM runtime
	 * can implement this too. VirtualPath is for diagnostics and stack traces.
	 */
	virtual bool LoadScript(const FModId& ModId, const FString& VirtualPath,
		TArrayView<const uint8> Source, FModDiagnostic& OutError) = 0;

	/** Calls a global function in a mod's context. Absent function is not an error - return false. */
	virtual bool CallFunction(const FModId& ModId, FName FunctionName, FModDiagnostic& OutError) = 0;
	virtual bool HasFunction(const FModId& ModId, FName FunctionName) const = 0;

	/**
	 * Resource limits. A capability boundary is not a resource boundary: gating the API surface does
	 * nothing about a script that allocates until the process dies or spins forever in a loop. A
	 * runtime that cannot enforce these should say so via SupportsResourceLimits so a game can decide
	 * whether that is acceptable, rather than the framework quietly implying protection it lacks.
	 */
	virtual bool SupportsResourceLimits() const { return false; }
	virtual void SetInstructionBudget(const FModId& ModId, int64 MaxInstructionsPerCall) {}
	virtual void SetMemoryBudget(const FModId& ModId, int64 MaxBytes) {}

	/** Reclaims what it can. Called on a per-mod basis so one mod's churn does not stall others. */
	virtual void CollectGarbage(const FModId& ModId) {}

	/** Reloads a mod's scripts in place. Scripts are text; unlike native modules this is safe. */
	virtual bool SupportsHotReload() const { return false; }
	virtual bool ReloadScripts(const FModId& ModId, FModDiagnostic& OutError) { return false; }
};
