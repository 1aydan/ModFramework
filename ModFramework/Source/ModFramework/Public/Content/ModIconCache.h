// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ModIconCache.generated.h"

class IImageWrapperModule;
class UModSubsystem;
class UTexture2D;

/**
 * Delivers a mod's icon. Icon is never the caller's problem to null-check: a mod without an icon, or
 * whose icon could not be read, is answered with UModIconCache::GetDefaultIcon().
 */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnModIconLoaded, FModId, ModId, UTexture2D*, Icon);

/**
 * Loads and keeps the icon images mods ship, so a mod browser can draw a list of them.
 *
 * WHY THE ICON IS A LOOSE IMAGE FILE
 * A mod list has to show icons for mods that are merely discovered, that the player has switched
 * off, or that the framework *rejected* - which is exactly the moment a player most needs to tell
 * one mod from another. An icon therefore cannot be a `.uasset` inside the mod's pak, because that
 * would only be readable after the mod mounted. It is a plain PNG or JPEG at the mod root, named by
 * `FModManifest::IconPath`, and this cache reads it straight off the filesystem (for an unpacked
 * mod) or straight out of the container through FModPackageReader (for a `.mod` package), in both
 * cases without mounting anything.
 *
 * THREADING
 * RequestIcon is the asynchronous path and the one UI should use. The file read, the size cap, the
 * format sniff and the dimension check all happen on a task thread; the texture import and the
 * delegate happen on the game thread. Concurrent requests for the same mod are coalesced into one
 * read. A request for an icon that is already cached still fires its delegate on a later tick rather
 * than inline, so a caller's ordering never depends on whether the cache happened to be warm.
 * Every other function on this class is game thread only.
 *
 * UNTRUSTED INPUT
 * An icon is a file a stranger wrote. Nothing here asserts on it: the declared path is re-checked
 * for containment even though the manifest validator already checked it, the file size is tested
 * before the bytes are read, the format is sniffed from the bytes rather than believed from the
 * extension, and the dimensions are read out of the image header before any decode happens. Each
 * failure produces one of these stable FModDiagnostic codes, logs a warning, and yields the default
 * icon:
 *
 *   Icon.NotFound            the mod is unknown, or the declared file or package entry is missing
 *   Icon.TooLarge            the file exceeds UModFrameworkSettings::GetEffectiveMaxIconFileBytes
 *   Icon.UnsupportedFormat   the bytes are not PNG or JPEG, whatever the extension claimed
 *   Icon.DecodeFailed        the image header or the pixels could not be read
 *   Icon.DimensionsExceeded  an edge exceeds UModFrameworkSettings::GetEffectiveMaxIconDimension
 *   Icon.UnsafePath          the declared path escapes the mod directory or names something odd
 *
 * A broken icon never affects whether the mod itself loads. It is cosmetic, and it is treated as
 * cosmetic.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModIconCache : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Binds the cache to the subsystem it reads mod records from and resolves the project's default
	 * icon. Idempotent; calling it again keeps everything already cached.
	 */
	void Initialize(UModSubsystem* InSubsystem);

	/**
	 * Drops every cached texture, answers everything still waiting with the default icon so no caller
	 * is left holding a delegate that will never fire, and makes any read still in flight a no-op when
	 * it lands. Safe to call without a prior Initialize, and safe to call twice.
	 */
	void Shutdown();

	/** Cached texture or nullptr. Never blocks, never decodes. Safe to call every UI frame. */
	UFUNCTION(BlueprintPure, Category = "Mod|Icons")
	UTexture2D* FindIcon(FModId ModId) const;

	/**
	 * Reads and validates off the game thread, then imports the texture on it.
	 *
	 * OnLoaded always fires exactly once, on the game thread, on a later tick - never inside this
	 * call, even when the icon is already cached or the mod has no icon at all. It receives the
	 * default icon rather than null whenever the real icon is unavailable. The one case where it does
	 * not fire is a cache that is destroyed outright before the read lands, which only happens when
	 * the game instance is going away and the caller is going away with it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Icons")
	void RequestIcon(FModId ModId, FOnModIconLoaded OnLoaded);

	/**
	 * Blocking. Editor/tooling only - do not call from gameplay.
	 *
	 * Returns the cached icon when there is one, otherwise reads, validates and imports it inline and
	 * caches the result. Returns GetDefaultIcon() on every failure, with OutError describing what went
	 * wrong; OutError is left with an empty Code when nothing did.
	 */
	UTexture2D* LoadIconSynchronous(const FModId& ModId, FModDiagnostic& OutError);

	/**
	 * Forgets one mod's cached texture, letting garbage collection reclaim it. A read already in
	 * flight for that mod is not cancelled and will repopulate the entry when it lands.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Icons")
	void ReleaseIcon(FModId ModId);

	/** Forgets every cached texture. Reads in flight are left alone and repopulate as they land. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Icons")
	void ReleaseAll();

	/**
	 * Fallback for mods that ship no icon. From UModFrameworkSettings::DefaultModIcon, and therefore
	 * null only when the project configured no default at all.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|Icons")
	UTexture2D* GetDefaultIcon() const;

	/**
	 * True when the mod is registered and declares an icon this cache is willing to try to load.
	 * It says nothing about whether that icon is cached yet - FindIcon answers that.
	 *
	 * Unlike FindIcon this stats the mod root to tell an unpacked mod from a `.mod` package, so it
	 * belongs in a list refresh rather than in a per-frame paint.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|Icons")
	bool HasIcon(FModId ModId) const;

private:
	/** Resolves UModFrameworkSettings::DefaultModIcon once. Cheap and idempotent after the first call. */
	void EnsureDefaultIcon();

	/**
	 * Works out where a mod's icon bytes live.
	 *
	 * Returns false when there is nothing to load. An empty OutError.Code distinguishes the ordinary
	 * "this mod ships no icon" case, which is silent, from a real failure worth logging.
	 *
	 * @param OutFilePath    Absolute path of the image file, or of the `.mod` package holding it.
	 * @param OutEntryPath   Relative path of the entry inside the package; empty for a loose file.
	 * @param bOutFromPackage True when the bytes have to come out of a `.mod` container.
	 */
	bool ResolveIconSource(const FModId& ModId, FString& OutFilePath, FString& OutEntryPath, bool& bOutFromPackage, FModDiagnostic& OutError) const;

	/** Fires OnLoaded with Icon (or the default, if Icon dies first) on a later tick, never inline. */
	void DispatchDeferred(const FModId& ModId, UTexture2D* Icon, const FOnModIconLoaded& OnLoaded) const;

	/**
	 * Game-thread landing point for a read that ran on a task thread. Drops out when InGeneration no
	 * longer matches, i.e. when Shutdown ran while the read was in flight.
	 */
	void FinishAsyncRead(const FModId& ModId, uint64 InGeneration, TArray<uint8> Bytes, FModDiagnostic Error, bool bReadSucceeded);

	/** Decodes validated bytes into a transient texture. Null with OutError set on failure. */
	UTexture2D* ImportIconBytes(const FModId& ModId, const TArray<uint8>& Bytes, int32 MaxDimension, FModDiagnostic& OutError) const;

	/** Hands Icon to every callback waiting on ModId and forgets the request. */
	void FlushPendingCallbacks(const FModId& ModId, UTexture2D* Icon);

	/**
	 * The ImageWrapper module interface, loaded on the game thread and then reused from task threads.
	 * Module loading is game-thread only, but a loaded module's interface pointer is stable and its
	 * CreateImageWrapper is a plain factory, so handing the pointer to a worker is safe.
	 */
	IImageWrapperModule* GetImageWrapperModule() const;

	/** Decoded icons, keyed by mod. A UPROPERTY so a cached icon is not collected under the UI. */
	UPROPERTY()
	TMap<FModId, TObjectPtr<UTexture2D>> Icons;

	/** UModFrameworkSettings::DefaultModIcon, resolved once and kept alive for as long as the cache is. */
	UPROPERTY()
	TObjectPtr<UTexture2D> DefaultIcon;

	/** Where mod records are read from. Weak: the subsystem owns this object, not the other way round. */
	TWeakObjectPtr<UModSubsystem> Subsystem;

	/**
	 * Callbacks waiting on a read that is already running, keyed by mod. This is how concurrent
	 * requests for one mod coalesce into a single file read. Deliberately not a UPROPERTY: a dynamic
	 * delegate holds its object weakly, so an entry here can never keep a dead widget alive.
	 */
	TMap<FModId, TArray<FOnModIconLoaded>> PendingRequests;

	/** Cached across calls; see GetImageWrapperModule. Mutable because resolving it is not a state change. */
	mutable IImageWrapperModule* ImageWrapperModule = nullptr;

	/** Bumped by Shutdown so a read that lands afterwards can recognise itself as stale and stop. */
	uint64 Generation = 1;

	/**
	 * True once the configured default icon has been looked up, whether or not it resolved. Keeps a
	 * project that names a missing asset from re-attempting the load, and re-logging it, per request.
	 */
	bool bDefaultIconResolved = false;
};
