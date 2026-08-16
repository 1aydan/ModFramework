// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ModScriptManager.generated.h"

class IModScriptRuntime;
class UModContext;
class UModSubsystem;
struct FModInfo;

/**
 * The optional global functions a mod's scripts may define.
 *
 * Named here rather than spelled at each call site so the framework, the runtimes and
 * docs/Scripting.md cannot drift apart over a capital letter - a mismatch nothing would report,
 * because an absent script function is legitimately not an error.
 */
namespace ModScriptFunctions
{
	/** "OnModActivated" - raised by UModSubsystem::ActivateMod, before the entry point's own hook. */
	MODFRAMEWORK_API extern const FName OnModActivated;

	/** "OnModDeactivated" - raised by UModSubsystem::DeactivateMod, after the entry point's own hook. */
	MODFRAMEWORK_API extern const FName OnModDeactivated;
}

/**
 * Builds one IModScriptRuntime on demand.
 *
 * The indirection exists because of a lifetime mismatch that has no other clean answer: a language
 * module (ModFrameworkLua, say) starts up with the engine, while UModScriptManager is owned by a
 * game instance subsystem that may be created much later, several times over, or never at all. A
 * module cannot hand a runtime to an object that does not exist yet, so it hands over a recipe
 * instead and every script manager builds its own.
 */
DECLARE_DELEGATE_RetVal(TSharedPtr<IModScriptRuntime>, FModScriptRuntimeFactory);

/**
 * The registry of script runtimes, and the thing that actually gets a mod's scripts off disk and
 * running.
 *
 * Owned by UModSubsystem exactly as UModConfigManager is: created in Initialize, torn down in
 * Deinitialize, and never constructed by anything else.
 *
 * THE FRAMEWORK NEVER NAMES A LANGUAGE. Nothing in ModFramework mentions Lua, includes a Lua header
 * or depends on the ModFrameworkLua module - the dependency points the other way, and that is what
 * lets a game ship no scripting at all, or ship a second runtime later, without this class changing.
 * Runtimes arrive through RegisterRuntimeFactory, which is a *static, module-global* list precisely
 * so a plain runtime module can populate it from StartupModule, long before any game instance (and
 * therefore any script manager) exists. Initialize then builds one runtime per registered factory.
 *
 * WHY EACH MANAGER BUILDS ITS OWN RUNTIME INSTANCES rather than sharing one: a runtime owns per-mod
 * VM state, and two game instances (PIE with two clients, say) load the same mods independently. A
 * shared runtime would give them one set of mod states to fight over, which is the same isolation
 * failure IModScriptRuntime forbids between two mods, one level up. Draining the factory list is
 * therefore non-destructive - the recipes stay registered for the next game instance.
 *
 * WHERE THE EXTENSION CHECK LIVES. FModManifestParser deliberately does not check a script's
 * extension against its runtime, because manifests are parsed by packaging tools on machines with no
 * runtimes registered at all. This class is the first place both facts are known, so the check
 * happens here, against IModScriptRuntime::GetSourceExtensions.
 *
 * UNTRUSTED INPUT. A script is a file a stranger wrote, named by a path a stranger wrote. Nothing
 * here asserts on any of it: the declared path is re-checked for containment even though the
 * manifest validator already checked it (an FModInfo can be built without ever going through the
 * validator), the file size is tested before a byte is read, and the runtime is handed bytes rather
 * than being asked to open anything itself. Each failure produces one of these stable codes:
 *
 *   Script.UnknownRuntime         the manifest names a runtime nothing registered
 *   Script.NoScripts              the scripting section is half declared, so nothing would run
 *   Script.ContextFailed          the runtime refused to create the mod's execution context
 *   Script.UnsafePath             a script path escapes the mod directory or names something odd
 *   Script.UnsupportedExtension   the runtime does not claim that file extension
 *   Script.NotFound               the file named by the manifest is not on disk
 *   Script.TooLarge               the file exceeds MaxScriptFileBytes
 *   Script.ReadFailed             the bytes could not be read
 *   Script.LoadFailed             the runtime rejected the script (syntax error, budget, ...)
 *   Script.CallFailed             a script entry point raised an error
 *   Script.InvalidRuntime         a runtime was registered that cannot be used (registration only)
 *   Script.DuplicateRuntime       a second runtime claimed an id already taken (registration only)
 *
 * Unlike an icon, a broken script IS fatal to its mod. Scripts are code the author expects to run,
 * and a mod that loads with half of its logic missing is worse than one that refuses to load: see
 * UModSubsystem::LoadMod, which fails the mod with EModLoadFailureReason::EntryPointInvalid.
 *
 * Threading: game thread only, like every other UObject in the framework.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModScriptManager : public UObject
{
	GENERATED_BODY()

public:
	// --- Lifetime --------------------------------------------------------------------------------

	/**
	 * Binds the manager to its owning subsystem and builds one runtime per registered factory.
	 *
	 * A factory that is unbound, that answers null, or whose runtime is refused by RegisterRuntime is
	 * logged and skipped: one broken language module must not cost a game every other runtime.
	 */
	void Initialize(UModSubsystem* InSubsystem);

	/** Destroys every live script context, then releases every runtime. Safe when Initialize never ran. */
	void Shutdown();

	// --- Runtime factories (static, module-global) -----------------------------------------------

	/**
	 * Records how to build a runtime. Call from a language module's StartupModule.
	 *
	 * Registering the same id twice replaces the recipe, which is what makes a hot-reloaded module
	 * behave. This touches no UObject and no CDO, so it is safe at any loading phase.
	 *
	 * Managers that already ran Initialize are NOT retro-fitted: a factory registered late applies to
	 * the next game instance. Language modules load at Default phase, long before a game instance
	 * exists, so this is a note rather than a limitation.
	 */
	static void RegisterRuntimeFactory(FName RuntimeId, FModScriptRuntimeFactory Factory);

	/** Forgets a recipe. Runtimes already built from it keep running until their manager shuts down. */
	static void UnregisterRuntimeFactory(FName RuntimeId);

	/** Every registered factory id, sorted. Useful for diagnosing "no runtime is registered". */
	static TArray<FName> GetRuntimeFactoryIds();

	// --- Runtimes --------------------------------------------------------------------------------

	/**
	 * Adds a runtime under the id it reports.
	 *
	 * Refuses a runtime with no id, one that claims no source extensions (nothing could ever be
	 * matched to it, so it would only ever produce the silent-no-op outcome this class exists to
	 * avoid), and one whose id is already taken.
	 */
	bool RegisterRuntime(TSharedRef<IModScriptRuntime> Runtime, FModDiagnostic& OutError);

	/** Removes a runtime, destroying every script context it is still holding. */
	bool UnregisterRuntime(FName RuntimeId);

	/** Borrowed pointer to a registered runtime, or nullptr. Never store it across a shutdown. */
	IModScriptRuntime* FindRuntime(FName RuntimeId) const;

	/** Every registered runtime id, sorted so console output and diagnostics are reproducible. */
	TArray<FName> GetRuntimeIds() const;

	// --- Per-mod scripts -------------------------------------------------------------------------

	/**
	 * Creates the mod's execution context and runs every script it declares, in the order declared.
	 *
	 * A manifest that names no runtime is a successful no-op - the overwhelmingly common case. A
	 * manifest that names a runtime nothing registered is a FAILURE with a diagnostic naming the id
	 * and listing what is registered, never a quiet skip.
	 *
	 * Aborts on the first script that fails and destroys the context on the way out, so a mod is
	 * never left running half of its own logic.
	 */
	bool LoadModScripts(const FModInfo& ModInfo, UModContext* Context, TArray<FModDiagnostic>& OutDiagnostics);

	/**
	 * Destroys one mod's script context and forgets its record.
	 *
	 * @return true when there was something to destroy. False - a mod with no scripts - is normal.
	 */
	bool UnloadModScripts(const FModId& ModId);

	/**
	 * Calls a global script function, if the mod has scripts and defines it.
	 *
	 * @return true only when the function existed AND ran cleanly. An absent function answers false
	 *         without a diagnostic: every script entry point is optional. A function that errored
	 *         answers false with a Script.CallFailed diagnostic on the mod's record.
	 */
	bool CallModFunction(const FModId& ModId, FName FunctionName);

	// --- Introspection ---------------------------------------------------------------------------

	/** The runtime handling one mod's scripts, or NAME_None when it has none. */
	UFUNCTION(BlueprintPure, Category = "Mod|Scripting")
	FName GetModRuntimeId(FModId ModId) const;

	/** How many scripts the mod's manifest declared, or 0. Not "how many ran" - see AreScriptsLoaded. */
	UFUNCTION(BlueprintPure, Category = "Mod|Scripting")
	int32 GetModScriptCount(FModId ModId) const;

	/** True when every declared script ran and the mod's context is live. */
	UFUNCTION(BlueprintPure, Category = "Mod|Scripting")
	bool AreScriptsLoaded(FModId ModId) const;

	/** Every mod this manager has a script record for, whether or not its scripts loaded. */
	TArray<FModId> GetScriptedMods() const;

	/**
	 * Largest script file this class will read off disk.
	 *
	 * A runaway guard, not a budget: 4 MiB is roughly a hundred thousand lines of source, far more
	 * than any mod needs and far less than a hostile file needs to matter. A runtime is free to be
	 * stricter about what it will then parse (FModLuaRuntime is), because this bound is about how
	 * many untrusted bytes the framework pulls into memory before anything looks at them.
	 */
	static constexpr int64 MaxScriptFileBytes = 4 * 1024 * 1024;

private:
	/** What one mod's scripting looks like, for teardown and for the Mod.Scripts command. */
	struct FModScriptRecord
	{
		/** The runtime that owns this mod's context. Never NAME_None in a stored record. */
		FName RuntimeId;

		/** Scripts the manifest declared. Kept even when loading failed, so the report is honest. */
		int32 DeclaredScripts = 0;

		/** How many actually ran before the first failure (or all of them). */
		int32 LoadedScripts = 0;

		/** True only when every declared script ran. */
		bool bLoaded = false;
	};

	/** Adds a diagnostic to the mod's registry record when it has one, and logs it either way. */
	void ReportDiagnostic(const FModId& ModId, const FModDiagnostic& Diagnostic) const;

	/** "Lua, Wren" or "none are registered", for the unknown-runtime diagnostic. */
	FString DescribeRegisteredRuntimes() const;

	/**
	 * Runtimes by the id they report. A shared pointer rather than a raw one because a runtime is a
	 * plain C++ object handed over by a module that may outlive or predecease this manager, and
	 * TSharedRef is how every other borrowed-implementation seam in the framework (IModProvider,
	 * IModContentMounter) is held.
	 */
	TMap<FName, TSharedRef<IModScriptRuntime>> Runtimes;

	/** One record per mod that declared scripts, live or failed. Not UObjects, so not reflected. */
	TMap<FModId, FModScriptRecord> ModRecords;

	TWeakObjectPtr<UModSubsystem> Subsystem;
};
