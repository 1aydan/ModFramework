// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "Containers/Array.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Events/ModEventTypes.h"
#include "Manifest/ModManifest.h"
#include "Registry/ModInfo.h"
#include "StructUtils/InstancedStruct.h"
#include "Templates/SubclassOf.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ModContext.generated.h"

class UClass;
class UModAPI;
class UModExtension;
class UModSubsystem;
class UWorld;

/**
 * The one and only handle a mod receives.
 *
 * Everything a mod is allowed to do passes through this object, which makes it the framework's
 * security boundary as much as its convenience layer. Three rules follow from that, and every method
 * here obeys all three:
 *
 *  1. **Identity is stamped, never accepted.** A context knows which mod it belongs to and supplies
 *     that id itself to every registry it talks to. There is no overload that lets a mod name a
 *     different mod, so a mod cannot register an extension as somebody else, request an API under
 *     another mod's permissions, raise an event attributed to a rival, or read another mod's save
 *     record. Calls that name an existing object - UnregisterExtension, UnsubscribeFromEvent - check
 *     ownership before acting.
 *
 *  2. **Nothing internal is handed out.** The context returns values, copies and permission-gated
 *     objects. The one exception is GetSubsystem, which is documented at its declaration and is the
 *     first thing to delete when hardening a game against untrusted mods.
 *
 *  3. **A dead context fails safe, never fatally.** The owning subsystem is held weakly, so a mod
 *     that keeps its context past a level change, a subsystem teardown or its own unload gets a
 *     logged refusal and a null/false/empty result. Nothing here asserts on anything a mod supplies:
 *     mod data is untrusted data.
 *
 * Object lifetime: a context is outered to the mod subsystem, whose own outer is the game instance,
 * which is what makes GetWorld work and what lets objects created through CreateModObject reach a
 * world. Objects a mod creates through the context are tracked by UModRegistry and released when the
 * mod unloads, so a mod cannot leak UObjects past its own lifetime.
 *
 * Threading: game thread only, like every other UObject in the framework.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModContext : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Binds a freshly created context to its subsystem and its mod. Called by UModSubsystem; a mod
	 * never sees an uninitialised context.
	 *
	 * The subsystem is stored weakly on purpose - see rule 3 in the class comment. Re-initialising an
	 * existing context is allowed and simply re-points it; the set of event handles it has issued is
	 * cleared with it, because those belonged to the previous binding.
	 */
	void InitializeContext(UModSubsystem* InSubsystem, const FModId& InModId);

	// --- Identity ------------------------------------------------------------------------------

	/** The mod this context speaks for. Fixed at InitializeContext time and never mod-writable. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	FModId GetModId() const;

	/** A copy of the mod's parsed mod.json. False - and OutManifest untouched - when unavailable. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	bool GetManifest(FModManifest& OutManifest) const;

	/** A snapshot of the registry's record for this mod. False when the registry cannot be reached. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	bool GetModInfo(FModInfo& OutInfo) const;

	/** Absolute path of the folder holding the mod's mod.json, or an empty string when unavailable. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	FString GetModRootPath() const;

	// --- Game APIs -----------------------------------------------------------------------------

	/**
	 * Asks for one of the game's APIs.
	 *
	 * The requesting mod id is supplied by the context, so the permission gate in
	 * UModAPIRegistry::RequestAPI always runs against *this* mod. VersionRange is the textual form
	 * documented on FModVersionRange ("^1.0.0", ">=1.2 <2.0.0", "*"); an empty string means any
	 * version. A null ApiClass skips the type check.
	 *
	 * Returns nullptr with OutError describing why on every refusal: no such API, version mismatch,
	 * class mismatch, missing permission, or a torn-down context.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|API", meta = (DeterminesOutputType = "ApiClass"))
	UModAPI* RequestAPI(FName ApiId, TSubclassOf<UModAPI> ApiClass, const FString& VersionRange, FModDiagnostic& OutError);

	// --- Extensions ----------------------------------------------------------------------------

	/**
	 * Instantiates ExtensionClass, files it under the extension point its class defaults name, and
	 * tracks it so unloading the mod releases it.
	 *
	 * The new object is outered to this context, which is how a Blueprint extension gets world
	 * context. Its OwningModId is stamped by the registry from the id this context carries.
	 *
	 * Returns nullptr with OutError filled for a null, abstract or non-UModExtension class, and for
	 * anything UModExtensionRegistry::RegisterExtension itself rejects - an unknown extension point,
	 * a base-class mismatch, a duplicate id, a per-mod quota or a missing permission. A rejected
	 * extension is untracked again, so a failed registration leaks nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Extensions")
	UModExtension* RegisterExtension(TSubclassOf<UModExtension> ExtensionClass, FModDiagnostic& OutError);

	/**
	 * Registers an extension the mod built itself, for the cases where it has to configure the object
	 * before the registry sees it.
	 *
	 * Refuses an object another mod already owns, so this is not a route to hijacking somebody else's
	 * extension. On success the object is tracked like one from RegisterExtension.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Extensions")
	bool RegisterExtensionInstance(UModExtension* Extension, FModDiagnostic& OutError);

	/**
	 * Removes one of *this mod's* extensions. An extension owned by another mod is refused with a
	 * logged warning, and an id nothing matches simply returns false.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Extensions")
	bool UnregisterExtension(FName ExtensionPointId, FName ExtensionId);

	// --- Events --------------------------------------------------------------------------------

	/**
	 * Subscribes a Blueprint delegate to an event, attributed to this mod so that unloading the mod
	 * cancels it automatically.
	 *
	 * Returns an invalid handle for an empty event id, an unbound delegate, a torn-down context, or a
	 * mod that has already reached MaxEventSubscriptions live handles.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Events")
	FModEventHandle SubscribeToEvent(FName EventId, FModEventDynamicDelegate Delegate, int32 Priority = 0);

	/**
	 * Cancels a subscription this context issued. Handles are sequential integers, so a handle the
	 * context did not hand out is refused rather than passed to the bus - otherwise a mod could
	 * silence another mod's handlers by guessing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Events")
	bool UnsubscribeFromEvent(FModEventHandle Handle);

	/**
	 * Raises an event as this mod.
	 *
	 * The source mod id is stamped by the context, so the event bus's permission gate and every
	 * handler see the true origin. Returns false for an empty event id, a torn-down context, or a
	 * mod missing one of the event descriptor's RequiredPermissions. A true return means the
	 * broadcast was dispatched, not that any handler did anything with it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Events")
	bool BroadcastEvent(FName EventId, const TInstancedStruct<FModEventPayload>& Payload);

	// --- Permissions ---------------------------------------------------------------------------

	/** True only when this mod's request for PermissionId ended in EModPermissionState::Granted. */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	bool HasPermission(FName PermissionId) const;

	/** Every permission granted to this mod, sorted by id. Empty when the context cannot ask. */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	TArray<FName> GetGrantedPermissions() const;

	// --- Save data -----------------------------------------------------------------------------

	/**
	 * Stores Json as this mod's save record. Routed through UModSaveDataManager, which enforces the
	 * "save.modify" permission and the payload size cap, so a mod cannot write to the save file - or
	 * to any other mod's record - without holding it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool SaveJson(const FString& Json, int32 DataVersion = 1);

	/** Reads this mod's own save record back. Reading needs no permission; other mods stay invisible. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Save")
	bool LoadJson(FString& OutJson, int32& OutDataVersion) const;

	// --- Content -------------------------------------------------------------------------------

	/**
	 * Every asset the asset registry knows about under *this mod's* mount points. A null ClassFilter
	 * returns everything; otherwise subclasses are included.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Assets")
	TArray<FAssetData> GetOwnAssets(UClass* ClassFilter) const;

	/**
	 * Loads one of this mod's assets by its short name - the name shown in the content browser,
	 * without any path - so a mod never has to hard-code the package path it was mounted at.
	 *
	 * Returns nullptr when the mod has no such asset, when the asset is not an AssetClass, or when the
	 * package fails to load. This blocks; prefer FStreamableManager for anything on a hot path.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Assets", meta = (DeterminesOutputType = "AssetClass"))
	UObject* LoadOwnAsset(FName AssetName, UClass* AssetClass) const;

	// --- Object lifetime -----------------------------------------------------------------------

	/**
	 * Creates a UObject owned by this mod. The object is outered to the context and registered with
	 * UModRegistry::TrackModObject, which is both what keeps it alive and what releases it when the
	 * mod unloads.
	 *
	 * Refuses a null, abstract or stale class, a UClass-derived class, and an AActor subclass -
	 * actors have to be spawned into a world, and one built with NewObject would be silently broken.
	 * Also refuses when the registry cannot be reached, rather than returning an untracked object
	 * that garbage collection would quietly take away again.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod", meta = (DeterminesOutputType = "ObjectClass"))
	UObject* CreateModObject(TSubclassOf<UObject> ObjectClass);

	// --- Logging -------------------------------------------------------------------------------

	/**
	 * Writes an attributable line to LogModFramework.
	 *
	 * Every message is prefixed with "[<mod id>] " - on *every* line of a multi-line message, so a mod
	 * cannot forge output that appears to come from another mod by embedding newlines. Messages longer
	 * than MaxLogMessageChars are truncated. Logging never needs the subsystem, so it keeps working
	 * even for a context whose subsystem has gone away, which is exactly when it is most wanted.
	 *
	 * LogInfo uses Display verbosity so that a mod author's own output reaches the in-game console
	 * rather than only the log file; the framework's internal chatter stays at Log and below.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod")
	void LogInfo(const FString& Message) const;

	UFUNCTION(BlueprintCallable, Category = "Mod")
	void LogWarning(const FString& Message) const;

	UFUNCTION(BlueprintCallable, Category = "Mod")
	void LogError(const FString& Message) const;

	// --- Escape hatch --------------------------------------------------------------------------

	/**
	 * The owning mod subsystem, or nullptr once it has gone away.
	 *
	 * ESCAPE HATCH - READ BEFORE SHIPPING WITH IT.
	 *
	 * This is the one method on the context that hands back a framework internal. Everything reached
	 * through it is *not* permission gated: from the subsystem a mod can reach the registry, the API
	 * registry, the extension registry, the event bus, the permission registry itself and the save
	 * data manager, and can call them with any mod id it likes. That defeats every guarantee the rest
	 * of this class makes.
	 *
	 * It exists because a game whose mods are trusted content - a first-party DLC pipeline, an
	 * in-house tools mod, a modding SDK sample - should not have to fork the framework to reach one
	 * subsystem method the context does not wrap.
	 *
	 * A game hardening against untrusted, player-installed mods should delete this method (and the
	 * matching entry in the generated SDK) and expose whatever it actually needs through a UModAPI,
	 * which is permission gated by construction.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod")
	UModSubsystem* GetSubsystem() const;

	//~ Begin UObject interface
	/**
	 * The world the mod is running in, resolved through the owning subsystem's game instance. Falls
	 * back to the outer chain when the subsystem is gone, and returns nullptr for class default
	 * objects and when no world can be resolved at all.
	 */
	virtual UWorld* GetWorld() const override;

#if WITH_EDITOR
	/** Tells the Blueprint compiler that GetWorld above is a real implementation. */
	virtual bool ImplementsGetWorld() const override { return true; }
#endif
	//~ End UObject interface

	/**
	 * Upper bound on the characters of one LogInfo/LogWarning/LogError message. A mod is not allowed
	 * to make the log unreadable, or unwritable, for everybody else.
	 */
	static constexpr int32 MaxLogMessageChars = 8192;

	/**
	 * Upper bound on how many live event subscriptions one mod may hold through its context.
	 * Deliberately generous: it is a runaway guard, not a budget.
	 */
	static constexpr int32 MaxEventSubscriptions = 8192;

private:
	/**
	 * The owning subsystem, or nullptr with a logged warning naming CallingFunction when the context
	 * has outlived it. Every public method that needs the framework starts here.
	 */
	UModSubsystem* ResolveSubsystem(const TCHAR* CallingFunction) const;

	/** "[<mod id>] " prefixed, newline-safe, length-capped form of a mod-supplied log message. */
	FString BuildAttributedMessage(const FString& Message) const;

	/**
	 * The subsystem that created this context. Weak on purpose: a mod may hold its context past the
	 * subsystem's teardown, and every method has to survive that rather than crash in it.
	 */
	TWeakObjectPtr<UModSubsystem> Subsystem;

	/** The mod this context speaks for. Assigned once by InitializeContext. */
	UPROPERTY()
	FModId ModId;

	/**
	 * Ids of the event subscriptions this context handed out, so UnsubscribeFromEvent can refuse a
	 * handle belonging to somebody else. Not a UPROPERTY: these are plain integers and reference
	 * nothing the garbage collector cares about.
	 */
	TSet<int64> IssuedEventHandles;
};
