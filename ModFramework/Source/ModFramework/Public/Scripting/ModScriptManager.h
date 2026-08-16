// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Core/ModFrameworkTypes.h"
#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"
#include "ModScriptManager.generated.h"

class IModScriptRuntime;
class UModContext;
class UModSubsystem;
struct FModInfo;

/** Builds one script runtime. Called once per UModScriptManager, never once per process. */
DECLARE_DELEGATE_RetVal(TSharedPtr<IModScriptRuntime>, FModScriptRuntimeFactory);

/**
 * The global functions a mod's scripts may define. All optional - a script that defines none is
 * perfectly valid, and one that defines only some is normal.
 *
 * These names are a published contract: they appear in docs/Scripting.md and in every example
 * script, so renaming one silently stops existing mods from being called.
 */
namespace ModScriptFunctions
{
	/** Called when the mod activates, after its content bundles are applied. */
	MODFRAMEWORK_API extern const FName OnModActivated;

	/** Called when the mod deactivates, before its extensions are switched off. */
	MODFRAMEWORK_API extern const FName OnModDeactivated;
}

/**
 * The registry of script runtimes, and the thing that actually runs a mod's scripts.
 *
 * Owned by UModSubsystem, like UModConfigManager. The framework never names a language: a runtime
 * arrives through the factory list below, so a game that wants no scripting simply does not enable
 * any module that registers one, and nothing here changes.
 *
 * WHY FACTORIES RATHER THAN INSTANCES
 * A module that implements a runtime (ModFrameworkLua, say) starts with the engine, long before any
 * game instance exists - and there may eventually be several (Play In Editor with two clients), or
 * none at all in a commandlet. It therefore cannot hand an IModScriptRuntime to a manager that does
 * not exist yet. Instead it registers a *recipe* on a module-global list, and every manager that
 * comes up later builds its own runtime from it. That is what keeps one script state per mod per
 * game instance, which is the isolation IModScriptRuntime demands, one level up.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModScriptManager : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Adds a recipe for a runtime. Module-global and safe to call before any game instance exists;
	 * a manager already running will NOT pick it up, since it built its runtimes at Initialize.
	 *
	 * The id must match what the runtime reports from GetRuntimeId, or a manifest naming it would
	 * find nothing. Registering an id twice replaces the earlier recipe and logs a warning.
	 */
	static void RegisterRuntimeFactory(FName RuntimeId, FModScriptRuntimeFactory Factory);

	/** Removes a recipe. Runtimes already built from it are owned by their manager and unaffected. */
	static void UnregisterRuntimeFactory(FName RuntimeId);

	/** The ids with a registered recipe, whether or not any manager has built one. */
	static TArray<FName> GetRegisteredFactoryIds();

	void Initialize(UModSubsystem* InSubsystem);
	void Shutdown();

	/** Runtime for an id, or null. */
	IModScriptRuntime* FindRuntime(FName RuntimeId) const;

	UFUNCTION(BlueprintPure, Category = "Mod|Scripting")
	bool HasRuntime(FName RuntimeId) const;

	UFUNCTION(BlueprintPure, Category = "Mod|Scripting")
	TArray<FName> GetRuntimeIds() const;

	/**
	 * Creates the mod's script context and runs its scripts, in manifest order.
	 *
	 * A mod that declares no runtime succeeds trivially. A mod that names a runtime nobody
	 * registered FAILS, with a diagnostic listing what is registered - "the mod loaded and quietly
	 * did nothing" is the outcome this exists to prevent.
	 */
	bool LoadModScripts(const FModInfo& ModInfo, UModContext* Context, TArray<FModDiagnostic>& OutDiagnostics);

	/** Destroys the mod's script context. Safe when it never had one. */
	bool UnloadModScripts(const FModId& ModId);

	/** Calls a global function in the mod's script context. Absent function is not an error. */
	bool CallModFunction(const FModId& ModId, FName FunctionName);

	/** True when this mod has a live script context with its scripts loaded. */
	UFUNCTION(BlueprintPure, Category = "Mod|Scripting")
	bool AreScriptsLoaded(FModId ModId) const;

	/** Every mod currently holding a script context, sorted by id so output is stable. */
	TArray<FModId> GetScriptedMods() const;

	/** Which runtime a mod's scripts were loaded into, or NAME_None. */
	UFUNCTION(BlueprintPure, Category = "Mod|Scripting")
	FName GetModRuntimeId(FModId ModId) const;

	UFUNCTION(BlueprintPure, Category = "Mod|Scripting")
	int32 GetModScriptCount(FModId ModId) const;

	/** One script file is source, not a payload. Anything larger is an authoring mistake. */
	static constexpr int64 MaxScriptFileBytes = 8 * 1024 * 1024;

private:
	/** What a mod's scripts were loaded into, so unload and dispatch reach the right runtime. */
	struct FModScriptBinding
	{
		FName RuntimeId;
		int32 ScriptCount = 0;
	};

	// Shared pointers, not UPROPERTYs: IModScriptRuntime is a plain interface, not a UObject.
	TMap<FName, TSharedPtr<IModScriptRuntime>> Runtimes;
	TMap<FModId, FModScriptBinding> ModBindings;

	TWeakObjectPtr<UModSubsystem> Subsystem;
};
