// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Manifest/ModVersion.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "ModAPI.generated.h"

class UClass;
class UModAPIRegistry;

/**
 * The public, serialisable description of one registered game API.
 *
 * A descriptor is a snapshot taken by UModAPIRegistry at registration time; it never changes
 * afterwards. Everything except ProviderModId comes from the API object itself through
 * UModAPI::BuildDescriptor.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModAPIDescriptor
{
	GENERATED_BODY()

	/** Stable, lowercase identifier mods use to look the API up. See UModAPI::GetApiId. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName ApiId;

	/** Semantic version of the API surface. Mods request a range and are refused when it misses. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModVersion Version;

	/** Full object path of the API's UClass, for diagnostics and SDK generation. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString ClassPath;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bServerAuthoritative = false;

	/** Every one of these must be granted to a mod before UModAPIRegistry::RequestAPI hands it over. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FName> RequiredPermissions;

	/** Invalid FModId when the API is provided by the game rather than by a mod. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId ProviderModId;
};

/**
 * Base class for every game-exposed modding API. The framework never understands what an API
 * does - it only stores, versions and permission-gates it.
 *
 * Subclasses declare identity with class metadata:
 *   UCLASS(BlueprintType, meta = (ModPublic, ModApiId = "game.gameplay", ModApiVersion = "1.0.0"))
 *
 * ---------------------------------------------------------------------------------------------
 * SHIPPING-BUILD HAZARD - READ THIS BEFORE RELYING ON CLASS METADATA
 * ---------------------------------------------------------------------------------------------
 * UCLASS metadata is editor-only data. `WITH_METADATA` defaults to `WITH_EDITORONLY_DATA`, so in a
 * cooked Shipping (or Test) build `UField::FindMetaData` does not even exist and every ModApi*
 * key silently disappears. An API whose id came only from metadata would therefore register under
 * one id in the editor and under a *different*, class-name derived id in the shipped game - the
 * kind of defect that only surfaces after release, when every published mod stops finding it.
 *
 * The identity of an API is consequently resolved through an explicit fallback chain, in this
 * order, and the result is cached in a transient member the first time it is asked for:
 *
 *   1. The Native* hook  (NativeGetApiId / NativeGetApiVersion / ...).
 *      Compiled in, present in every build configuration, cannot be stripped and cannot be
 *      changed by data. This is the recommended way for a C++ API to declare itself.
 *   2. The authored override property (ApiIdOverride, ApiVersionOverride, ...).
 *      The only route available to a Blueprint subclass, which cannot carry UCLASS metadata.
 *      Set it in the class defaults, always before the API is registered.
 *   3. The class metadata (ModApiId, ModApiVersion, ModApiPermissions,
 *      ModApiServerAuthoritative), searched up the superclass chain so a Blueprint subclass
 *      inherits the identity of its native parent. AVAILABLE IN EDITOR BUILDS ONLY.
 *   4. A derived default: a sanitised class name for the id (see MakeFallbackApiId), 1.0.0 for
 *      the version, an empty permission list, and non-server-authoritative.
 *
 * In short: if the API must work in a shipping build, do not stop at step 3.
 *
 * Because the resolved identity is cached, the override properties have to be assigned before the
 * first call to GetApiId / GetApiVersion / GetApiDescription / IsServerAuthoritative /
 * GetRequiredPermissions - in the constructor or in the class defaults. Call
 * InvalidateResolvedIdentity() if they genuinely have to change later.
 *
 * Identity resolution is not thread-safe; APIs are registered and queried on the game thread.
 */
UCLASS(Abstract, BlueprintType)
class MODFRAMEWORK_API UModAPI : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Stable identifier mods use to request this API.
	 *
	 * Resolution order: NativeGetApiId(), then ApiIdOverride, then the ModApiId class metadata
	 * (editor builds only), then MakeFallbackApiId(GetClass()). The result is lowercased and
	 * cached; it is never NAME_None for a real class.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|API")
	virtual FName GetApiId() const;

	/**
	 * Semantic version of the API surface.
	 *
	 * Resolution order: NativeGetApiVersion(), then ApiVersionOverride, then the ModApiVersion
	 * class metadata (editor builds only), then 1.0.0. A version of 0.0.0 is treated as "not set"
	 * at every step, so it can never be the answer.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|API")
	virtual FModVersion GetApiVersion() const;

	/**
	 * Free-form human description shown by mod tooling.
	 *
	 * Resolution order: NativeGetApiDescription(), then ApiDescriptionOverride, then empty.
	 * Deliberately NOT sourced from the class tooltip: that would make the shipped description
	 * differ from the one seen in the editor.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|API")
	virtual FString GetApiDescription() const;

	/**
	 * Server-authoritative APIs refuse client calls when the world is a client.
	 *
	 * Resolution order: NativeGetServerAuthoritative(), then bOverrideServerAuthoritative /
	 * bServerAuthoritativeOverride, then the ModApiServerAuthoritative class metadata (editor
	 * builds only), then false.
	 *
	 * This flag is only a declaration. Implementations still have to guard their mutating entry
	 * points with VerifyServerAuthority.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|API")
	virtual bool IsServerAuthoritative() const;

	/**
	 * Permissions a mod must hold to obtain this API. From ModApiPermissions metadata (comma separated).
	 *
	 * Resolution order: NativeGetRequiredPermissions(), then RequiredPermissionsOverride, then the
	 * comma separated ModApiPermissions class metadata (editor builds only), then empty. The lists
	 * are alternatives, never merged: the first non-empty one wins.
	 */
	virtual TArray<FName> GetRequiredPermissions() const;

	/** Called by UModAPIRegistry once the API is live. The base implementation raises the Blueprint event. */
	virtual void OnAPIRegistered(UModAPIRegistry* InRegistry);

	/** Called by UModAPIRegistry just before the API is dropped. */
	virtual void OnAPIUnregistered();

	/** Snapshot of everything the registry publishes about this API. ProviderModId is left invalid. */
	FModAPIDescriptor BuildDescriptor() const;

	/**
	 * Guard helper for server-authoritative implementations. Logs + returns false on clients.
	 *
	 * Resolves a UWorld from WorldContext (falling back to this object's own world when
	 * WorldContext is null) and then:
	 *   - returns true when no world can be resolved, so editor tooling, commandlets and unit
	 *     tests are never blocked;
	 *   - returns true when the world's net mode is not NM_Client, i.e. standalone, listen server
	 *     and dedicated server all pass;
	 *   - otherwise logs a warning naming CallingFunction and this API's id, and returns false.
	 *
	 * @param WorldContext     Any object with world context; may be null.
	 * @param CallingFunction  Name of the guarded function, for the log line. Pass __FUNCTION__.
	 */
	bool VerifyServerAuthority(const UObject* WorldContext, const TCHAR* CallingFunction) const;

	UFUNCTION(BlueprintImplementableEvent, Category = "Mod|API", meta = (DisplayName = "On API Registered"))
	void ReceiveOnAPIRegistered();

	//~ Metadata-independent identity hooks -------------------------------------------------------
	// Override these in C++ when the API has to keep its identity in a cooked Shipping build, where
	// UCLASS metadata does not exist. Each one returns an "unset" value by default so the rest of
	// the fallback chain still runs.

	/** Return the api id, or NAME_None to defer to the rest of the chain. */
	virtual FName NativeGetApiId() const;

	/** Return the api version, or a zero version (the default) to defer to the rest of the chain. */
	virtual FModVersion NativeGetApiVersion() const;

	/** Return the description, or an empty string to defer to the rest of the chain. */
	virtual FString NativeGetApiDescription() const;

	/**
	 * Write the server-authoritative flag into bOutServerAuthoritative and return true, or return
	 * false (the default) to defer to the rest of the chain. The two-part signature exists because
	 * "explicitly false" and "not specified" have to stay distinguishable.
	 */
	virtual bool NativeGetServerAuthoritative(bool& bOutServerAuthoritative) const;

	/** Return the required permissions, or an empty array to defer to the rest of the chain. */
	virtual TArray<FName> NativeGetRequiredPermissions() const;

	/**
	 * Drops the cached identity so the next query re-runs the whole fallback chain.
	 * Only needed when an override property changes after the API has already been queried.
	 */
	void InvalidateResolvedIdentity();

	/**
	 * Last-resort api id derived from a class name, used when nothing else declared one.
	 *
	 * Steps, in order:
	 *   - a trailing "_C" is removed, so the Blueprint class "BP_LootAPI_C" behaves like "BP_LootAPI";
	 *   - a leading 'U' is removed only when the remainder still looks like a C++ prefixed
	 *     PascalCase identifier ("UGameplayAPI" -> "GameplayAPI"). Reflection names normally have
	 *     the prefix stripped already, so the guard exists to protect names that genuinely start
	 *     with a U: "UIWidgetAPI" and "UtilityAPI" are left alone;
	 *   - a trailing "API" is removed when something would remain;
	 *   - the result is lowercased.
	 *
	 * "UGameplayAPI" -> "gameplay", "LootTableAPI" -> "loottable", "BP_QuestsAPI_C" -> "bp_quests".
	 * Returns NAME_None for a null class.
	 *
	 * This is a fallback, not a naming scheme: two differently named classes can still collide, and
	 * renaming a class silently renames its API. Declare ModApiId or NativeGetApiId for anything
	 * mods are expected to depend on.
	 */
	static FName MakeFallbackApiId(const UClass* InClass);

protected:
	/**
	 * Authored api id. NAME_None means "not set". Takes precedence over class metadata, which is
	 * what makes a Blueprint subclass able to declare an identity at all.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod|API")
	FName ApiIdOverride;

	/** Authored api version. A zero version (0.0.0) means "not set". */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod|API")
	FModVersion ApiVersionOverride;

	/** Authored description. An empty string means "not set". */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod|API")
	FString ApiDescriptionOverride;

	/** Authored permission list. An empty array means "not set"; it is never merged with metadata. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod|API")
	TArray<FName> RequiredPermissionsOverride;

	/** Set this to make bServerAuthoritativeOverride count. Without it "false" cannot be authored. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod|API")
	bool bOverrideServerAuthoritative = false;

	/** Only consulted when bOverrideServerAuthoritative is true. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod|API", meta = (EditCondition = "bOverrideServerAuthoritative"))
	bool bServerAuthoritativeOverride = false;

private:
	/** Runs the fallback chain once and fills the caches below. Const because every getter is. */
	void ResolveIdentity() const;

	// Transient, deliberately not UPROPERTY: none of these reference a UObject, none of them should
	// ever be saved, and they must not be copied into a cooked package. They are mutable so the
	// BlueprintPure getters can stay const.
	mutable bool bIdentityResolved = false;
	mutable FName CachedApiId;
	mutable FModVersion CachedApiVersion;
	mutable FString CachedApiDescription;
	mutable bool bCachedServerAuthoritative = false;
	mutable TArray<FName> CachedRequiredPermissions;
};
