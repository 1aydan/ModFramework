// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Content/ModContentManager.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Dependencies/ModDependencyTypes.h"
#include "Events/ModEventTypes.h"
#include "Extensions/ModExtension.h"
#include "Net/ModSessionManifest.h"
#include "Providers/ModProvider.h"
#include "Registry/ModInfo.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/SharedPointer.h"
#include "Templates/SubclassOf.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "ModSubsystem.generated.h"

class FLocalFileModProvider;
class FSubsystemCollectionBase;
class UGameInstance;
class UModAPI;
class UModAPIRegistry;
class UModContext;
class UModEntryPointBase;
class UModEventBus;
class UModExtensionRegistry;
class UModIconCache;
class UModPermissionRegistry;
class UModRegistry;
class UModSaveDataManager;
class UWorld;

/** Blueprint-facing mirror of UModRegistry::OnModStateChanged. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnModStateChangedBP, FModId, ModId, EModState, OldState, EModState, NewState);

/** Raised once at the end of every UModSubsystem::RefreshMods pass, however it went. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnModsRefreshedBP);

/**
 * The UObjects one mod's content bundles brought into memory, kept alive for as long as that mod is
 * loaded.
 *
 * A bundle references its extension classes, data assets and data tables *softly*, so nothing keeps
 * them resident once the bundle asset itself is collected. Applying a bundle therefore has to hold a
 * hard reference to everything it resolved, and the presence of an entry for a mod doubles as the
 * "bundles have already been applied" flag that stops a deactivate/activate cycle from registering
 * every extension twice.
 *
 * A struct rather than a bare TArray because a UPROPERTY TMap cannot hold a container as its value.
 */
USTRUCT()
struct FModActiveBundleSet
{
	GENERATED_BODY()

	/** Bundle assets plus every data asset and data table they name, in application order. */
	UPROPERTY()
	TArray<TObjectPtr<UObject>> Objects;
};

/**
 * The orchestrator: the one object that drives a mod from "a folder somebody dropped in Mods/" to
 * "running code and mounted content", and back again.
 *
 * Everything else in the framework is a passive component. Providers find mods, the parser reads
 * them, the resolver orders them, the content manager mounts them, the registries store them - and
 * none of those ever calls another. This subsystem is what wires them together, in one place, in one
 * order, so that the lifecycle has exactly one implementation.
 *
 * THE STATE MACHINE IS THE CONTRACT
 * Every transition goes through UModRegistry::SetModState, and the matching ModFrameworkEvents::*
 * event is raised centrally from the registry's OnModStateChanged handler rather than at each call
 * site. Nothing here ever assigns FModInfo::State. That is what makes "a mod is Activated" mean the
 * same thing to the console, to a mod browser and to another mod, and what makes it impossible for a
 * new code path to move a mod without telling anyone.
 *
 *   Discovered -> Validated -> DependenciesResolved -> Mounted -> Loading -> Loaded -> Activated
 *
 * LOADED AND ACTIVATED ARE DIFFERENT THINGS
 * LoadMod requires EModState::Mounted and gives the mod its UModContext and its entry point object.
 * ActivateMod requires EModState::Loaded and applies the mod's content bundles and switches its
 * extensions on. A mod may sit Loaded and idle for as long as the game likes, and DeactivateMod
 * returns it there without unloading anything.
 *
 * EVERY ENTRY POINT IS TOTAL
 * A console command, a Blueprint or another mod may call any of these in any order, in any state,
 * with any id, including during teardown. Nothing here asserts and nothing crashes: an impossible
 * request is refused with false and a log line that names the mod and the state it is actually in.
 * Mod data is untrusted data, and so is the order somebody calls this class in.
 *
 * FAILURE NEVER STOPS THE BATCH
 * RefreshMods runs discovery, validation, permission evaluation, resolution, conflict detection,
 * mounting, loading and activation over every mod. A mod that fails any stage is moved to
 * EModState::Failed with a reason and a diagnostic, and the pass continues with the others. One
 * broken mod must never cost a player the rest of their mod list.
 *
 * Threading: game thread only, like every other UObject in the framework.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem interface
	/**
	 * Creates every owned object, injects the permission gate into the API registry, the extension
	 * registry and the event bus, registers FLocalFileModProvider and the builtin permissions, binds
	 * the world delegates, and - when UModFrameworkSettings::bAutoDiscoverOnStartup is set - runs a
	 * full RefreshMods pass.
	 *
	 * A partial failure here (no settings object, a registry that could not be created) is logged and
	 * survived: the subsystem stays alive and every accessor answers null rather than crashing.
	 */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/**
	 * Deactivates and unloads every mod in reverse load order, then shuts down everything this object
	 * owns and unbinds every delegate it bound. Safe when Initialize never ran or failed halfway.
	 */
	virtual void Deinitialize() override;
	//~ End USubsystem interface

	/**
	 * The subsystem of the game instance the context object belongs to, or nullptr.
	 *
	 * Resolves a UModSubsystem, a UGameInstance, anything with a world, and anything outered to a game
	 * instance. Returns nullptr - never asserts - when there is no game instance to be found, which is
	 * the normal answer for a class default object, an editor preview world or a commandlet.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod", meta = (WorldContext = "WorldContextObject"))
	static UModSubsystem* Get(const UObject* WorldContextObject);

	// --- Lifecycle -------------------------------------------------------------------------------

	/**
	 * Asks every registered provider what it can see and files the results in the registry.
	 *
	 * Each newly registered mod is moved to EModState::Validated, or to Disabled when the project
	 * turned it off, or to Failed when its manifest did not survive validation. A mod that is already
	 * registered is left exactly as it is, so this is safe to call repeatedly.
	 *
	 * @return how many mods were newly registered.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	int32 DiscoverMods();

	/**
	 * Runs FModDependencyResolver over every registered mod, applies the resulting load order to the
	 * registry and the extension registry, and moves every accepted mod to
	 * EModState::DependenciesResolved. Rejected mods are failed with the resolver's own reason.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	FModResolveResult ResolveDependencies();

	/**
	 * Acquires the mod from its provider (installing a `.mod` package if that is what it still is) and
	 * mounts every content root it declares. A mod with no content roots mounts trivially.
	 *
	 * Requires EModState::DependenciesResolved or Unmounted. Already-mounted mods succeed silently.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	bool MountMod(FModId ModId);

	/**
	 * Releases a mod's content mounts and moves it to EModState::Unmounted.
	 *
	 * Refuses an activated or loading mod - unmounting content the game is running on is not
	 * survivable - and tells the caller to use UnloadMod instead. A mod that has nothing mounted is a
	 * successful no-op.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	bool UnmountMod(FModId ModId);

	/**
	 * Gives a mounted mod its UModContext and, when the manifest names one, its entry point object,
	 * then calls UModEntryPointBase::NativeOnModLoaded.
	 *
	 * Requires EModState::Mounted. A manifest with no `entryPoint.class` is entirely legal - that is
	 * the pure-asset mod - and loads successfully with a context but no entry point. A named class
	 * that cannot be resolved fails the mod with EntryPointMissing; one that resolves to something
	 * that is not a concrete UModEntryPointBase fails it with EntryPointInvalid.
	 *
	 * Loading does NOT start the mod. Call ActivateMod for that.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	bool LoadMod(FModId ModId);

	/**
	 * Tears a mod down completely: deactivates it if needed, calls NativeOnModUnloaded, unregisters
	 * everything it contributed (extensions, then APIs, then event subscriptions), releases every
	 * object it created, and finally unmounts its content. Release always happens before unmount.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	bool UnloadMod(FModId ModId);

	/**
	 * Applies a loaded mod's content bundles, registers the extension classes they name, switches the
	 * mod's extensions on and calls NativeOnModActivated.
	 *
	 * Requires EModState::Loaded or Deactivated. Bundles are applied once per load: re-activating a
	 * deactivated mod only flips its extensions back on.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	bool ActivateMod(FModId ModId);

	/** Switches a mod's extensions off and calls NativeOnModDeactivated. The mod stays loaded. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	bool DeactivateMod(FModId ModId);

	/** Unloads one mod and brings it straight back, restoring whether it was active. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	bool ReloadMod(FModId ModId);

	/**
	 * The whole pipeline: tear down what is loaded, discover, validate, evaluate permissions, resolve
	 * dependencies, detect conflicts, mount, load and - when bActivate is set - activate.
	 *
	 * Honours UModFrameworkSettings::bAutoLoadDiscoveredMods for the load step, so a project that
	 * wants to drive loading itself gets mods mounted and ordered and nothing more.
	 *
	 * Never throws and never gives up early: a mod that fails a stage is failed individually.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	void RefreshMods(bool bActivate = true);

	/** Loads every mounted mod in load order. @return how many moved to EModState::Loaded. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	int32 LoadAllMods();

	/** Activates every loaded mod in load order. @return how many moved to EModState::Activated. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	int32 ActivateAllMods();

	/** Deactivates every active mod in reverse load order. @return how many were deactivated. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	int32 DeactivateAllMods();

	/** Unloads and unmounts every live mod in reverse load order. @return how many were torn down. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	int32 UnloadAllMods();

	/** Unloads everything in reverse load order and brings it all back in load order. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	void ReloadAllMods();

	/**
	 * Switches one mod on or off for this session.
	 *
	 * Disabling a mod that is running tears it down first. Enabling one only clears the flag and
	 * returns it to EModState::Discovered - run RefreshMods, or mount and load it yourself, to
	 * actually bring it up. The change is not written to DefaultGame.ini; that is the game's call.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Lifecycle")
	bool SetModEnabled(FModId ModId, bool bEnabled);

	// --- Registration (game side) ----------------------------------------------------------------

	/** Publishes one of the game's APIs to mods. The subsystem does not take ownership of the object. */
	UFUNCTION(BlueprintCallable, Category = "Mod|API")
	bool RegisterGameAPI(UModAPI* Api);

	/** Instantiates ApiClass, outered to this subsystem, and registers it. */
	UFUNCTION(BlueprintCallable, Category = "Mod|API")
	bool RegisterGameAPIByClass(TSubclassOf<UModAPI> ApiClass);

	/** Opens an extension point. Do this before any mod loads: an unknown point rejects extensions. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Extensions")
	bool RegisterExtensionPoint(const FModExtensionPointDescriptor& Descriptor);

	/** Declares an event type on the bus, which is what gives it a payload check and a permission gate. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Events")
	bool RegisterEventType(const FModEventDescriptor& Descriptor);

	/** Adds a mod source. A provider whose id is already registered replaces the previous one. */
	void RegisterProvider(TSharedRef<IModProvider> Provider);

	/** Removes a mod source. Mods it already produced keep their records until the next refresh. */
	void UnregisterProvider(FName ProviderId);

	/** Every registered provider id, in registration order. */
	TArray<FName> GetProviderIds() const;

	/** Borrowed pointer to a registered provider, or nullptr. Never store it across a refresh. */
	IModProvider* FindProvider(FName ProviderId) const;

	// --- Accessors -------------------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Mod")
	UModRegistry* GetRegistry() const;

	UFUNCTION(BlueprintPure, Category = "Mod")
	UModAPIRegistry* GetAPIRegistry() const;

	UFUNCTION(BlueprintPure, Category = "Mod")
	UModExtensionRegistry* GetExtensionRegistry() const;

	UFUNCTION(BlueprintPure, Category = "Mod")
	UModEventBus* GetEventBus() const;

	UFUNCTION(BlueprintPure, Category = "Mod")
	UModPermissionRegistry* GetPermissionRegistry() const;

	UFUNCTION(BlueprintPure, Category = "Mod")
	UModSaveDataManager* GetSaveDataManager() const;

	/** The mod icon cache, for mod-browser UI and the `Mod.Icons` console command. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	UModIconCache* GetIconCache() const;

	/** The context handed to one loaded mod, or nullptr when that mod is not loaded. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	UModContext* GetModContext(FModId ModId) const;

	/** The content manager. Not a UFUNCTION: FModContentManager is a plain C++ type. */
	FModContentManager& GetContentManager();
	const FModContentManager& GetContentManager() const;

	/** How this game describes itself to mods, rebuilt from the project settings on every call. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	FModEnvironment GetEnvironment() const;

	/** The result of the last ResolveDependencies call. Default-constructed before the first one. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	FModResolveResult GetLastResolveResult() const;

	/** Conflicts found during the last refresh, sorted by extension point then resource. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	TArray<FModConflict> GetConflicts() const;

	/** What this install advertises to the other side of a network session. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	FModSessionManifest BuildSessionManifest() const;

	/**
	 * Everything the framework has to say about the current mod set: the subsystem's own pipeline
	 * diagnostics first, then each registered mod's, in load order.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod")
	TArray<FModDiagnostic> GetDiagnostics() const;

	// --- Delegates -------------------------------------------------------------------------------

	/** Mirrors UModRegistry::OnModStateChanged for Blueprint listeners. */
	UPROPERTY(BlueprintAssignable, Category = "Mod")
	FOnModStateChangedBP OnModStateChanged;

	/** Fires once at the end of every RefreshMods pass. */
	UPROPERTY(BlueprintAssignable, Category = "Mod")
	FOnModsRefreshedBP OnModsRefreshed;

	/**
	 * Upper bound on how many pipeline diagnostics the subsystem keeps. A pathological mod folder is
	 * not allowed to grow this array without limit; once it is full, further entries are logged only.
	 */
	static constexpr int32 MaxRetainedDiagnostics = 4096;

private:
	// --- Pipeline stages -------------------------------------------------------------------------

	/** Registers, validates and files one discovery result. Returns true when a record was created. */
	bool RegisterDiscoveryResult(const FModDiscoveryResult& Result);

	/** Runs the permission registry over every non-failed mod and mirrors the answers into the records. */
	void EvaluatePermissions();

	/**
	 * Builds the claim list, applies the game's policy table and fails every mod caught in a blocking
	 * conflict, removing it from InOutLoadOrder so it is never mounted.
	 */
	void DetectConflicts(TArray<FModId>& InOutLoadOrder);

	/** Applies one resolve result: load order, extension ordering and every rejection. */
	void ApplyResolveResult(const FModResolveResult& Result);

	/** Resolves and hard-references a mod's content bundles, registering the extensions they name. */
	void ApplyContentBundles(const FModId& ModId, UModContext* Context);

	// --- Helpers ---------------------------------------------------------------------------------

	/** Fails a mod through the registry, which records the reason and moves it to EModState::Failed. */
	void FailMod(const FModId& ModId, EModLoadFailureReason Reason, const FString& Message);

	/** Stores a diagnostic on the subsystem and, when the mod is registered, on its record. */
	void RecordDiagnostic(const FModId& ModId, const FModDiagnostic& Diagnostic);

	/** RecordDiagnostic for a whole batch. */
	void RecordDiagnostics(const FModId& ModId, const TArray<FModDiagnostic>& InDiagnostics);

	/** Registered mod ids in load order, optionally reversed. Unordered mods come last (first). */
	TArray<FModId> GetLoadOrderedIds(bool bReverse) const;

	/** The entry point object of a loaded mod, or nullptr. */
	UModEntryPointBase* FindEntryPoint(const FModId& ModId) const;

	/** Drops the context, the entry point and the applied bundles of one mod and releases its objects. */
	void ReleaseModLoadState(const FModId& ModId);

	/** Re-points the built-in local provider at the currently configured search directories. */
	void RefreshLocalProviderDirectories();

	/** True when the project settings list this id in DisabledModIds. */
	static bool IsDisabledByProject(const FModId& ModId);

	// --- Delegate handlers -----------------------------------------------------------------------

	/** The permission gate injected into the API registry, the extension registry and the event bus. */
	bool HandlePermissionCheck(const FModId& ModId, FName PermissionId);

	/** Mirrors registry state changes to Blueprint and raises the matching lifecycle event. */
	void HandleModStateChanged(const FModId& ModId, EModState OldState, EModState NewState);

	/** Raises ModFrameworkEvents::WorldCreated for a world belonging to this game instance. */
	void HandleWorldCreated(UWorld* World);

	/** Raises ModFrameworkEvents::WorldDestroyed for a world belonging to this game instance. */
	void HandleWorldDestroyed(UWorld* World);

	/** Raises ModFrameworkEvents::GameStarted, at most once, for this game instance. */
	void HandleGameInstanceStarted(UGameInstance* InGameInstance);

	/** Broadcasts a payload-free framework event. Does nothing when the bus is gone. */
	void BroadcastFrameworkEvent(FName EventId, UObject* WorldContext);

	// --- Owned state -----------------------------------------------------------------------------

	/** The authoritative mod table, and through it the API and extension sub-registries. */
	UPROPERTY()
	TObjectPtr<UModRegistry> Registry;

	UPROPERTY()
	TObjectPtr<UModEventBus> EventBus;

	UPROPERTY()
	TObjectPtr<UModPermissionRegistry> PermissionRegistry;

	UPROPERTY()
	TObjectPtr<UModSaveDataManager> SaveDataManager;

	UPROPERTY()
	TObjectPtr<UModIconCache> IconCache;

	/** One context per loaded mod. Cleared entry by entry as mods unload. */
	UPROPERTY()
	TMap<FModId, TObjectPtr<UModContext>> ModContexts;

	/** One entry point per loaded mod that declared one. Mods without an entry class have no entry. */
	UPROPERTY()
	TMap<FModId, TObjectPtr<UModEntryPointBase>> EntryPoints;

	/** Content bundle payloads per mod. Presence also means "bundles already applied for this load". */
	UPROPERTY()
	TMap<FModId, FModActiveBundleSet> AppliedBundles;

	/**
	 * Content mounts. Held by value rather than by pointer so GetContentManager can hand back a
	 * reference that is valid for the whole lifetime of the subsystem, including a partly failed
	 * Initialize.
	 */
	FModContentManager ContentManager;

	/** Mod sources, consulted in registration order. Not UObjects, so not reflected. */
	TArray<TSharedRef<IModProvider>> Providers;

	/** The built-in local-filesystem provider, kept typed so its search directories can be refreshed. */
	TSharedPtr<FLocalFileModProvider> LocalProvider;

	/** Output of the last ResolveDependencies call. */
	FModResolveResult LastResolveResult;

	/** Conflicts found during the last DetectConflicts pass. */
	TArray<FModConflict> Conflicts;

	/** Pipeline-level diagnostics, capped at MaxRetainedDiagnostics. */
	TArray<FModDiagnostic> Diagnostics;

	FDelegateHandle StateChangedHandle;
	FDelegateHandle WorldCreatedHandle;
	FDelegateHandle WorldDestroyedHandle;
	FDelegateHandle GameStartedHandle;

	/** False until Initialize finished, and again from the start of Deinitialize. */
	bool bInitialized = false;

	/** Guards RefreshMods against a handler that calls straight back into it. */
	bool bRefreshInProgress = false;

	/** Set once GameStarted has been raised, so a re-entered game instance does not raise it twice. */
	bool bGameStartedBroadcast = false;
};
