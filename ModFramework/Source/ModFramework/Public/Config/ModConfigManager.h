// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Core/ModFrameworkTypes.h"
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ModConfigManager.generated.h"

class FJsonObject;
class UModSubsystem;

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnModConfigChanged, const FModId& /*ModId*/, FName /*Key*/);

/**
 * Per-mod configuration - the framework's equivalent of a mod loader's config system.
 *
 * Configuration is basic modding infrastructure, not a game concern, so it lives here rather than in
 * a game's SDK. The framework never interprets a key; it only stores, persists and namespaces them.
 *
 * TWO LAYERS, AND THE REASON FOR THEM
 *
 *   defaults   <ModRoot>/Config/*.json          shipped by the mod author, never written
 *   values     {ProjectSaved}/ModConfigs/<id>.json   edited by the player or the mod, written back
 *
 * A read falls through to the default when the user layer has no entry. That is what makes a mod
 * update safe: version 2 can add new settings and change the defaults of old ones, and the player's
 * existing edits survive untouched because they live in a different file. Flattening these into one
 * file - the obvious simplification - means every update either clobbers player edits or silently
 * keeps stale defaults forever.
 *
 * Keys are flat FNames. Nested structure is available through the JSON accessors for mods that want
 * it, but the typed accessors deliberately stay flat so a config file is something a player can
 * actually read and edit.
 *
 * Writing config needs no permission: it is the mod's own namespace, written by the framework to a
 * path the mod never names. A mod cannot reach another mod's config, and cannot write anywhere else.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModConfigManager : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UModSubsystem* InSubsystem);
	void Shutdown();

	/** Absolute path of the user-editable config file for a mod. */
	UFUNCTION(BlueprintPure, Category = "Mod|Config")
	FString GetUserConfigPath(FModId ModId) const;

	/**
	 * Loads a mod's defaults from its Config/ folder and overlays the user file.
	 * Called by the lifecycle when a mod loads; safe to call again to reload from disk.
	 */
	bool LoadConfig(const FModId& ModId, const FString& ModRootPath, TArray<FModDiagnostic>& OutDiagnostics);

	/** Writes the user layer only. Defaults are never written back. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Config")
	bool SaveConfig(FModId ModId);

	UFUNCTION(BlueprintCallable, Category = "Mod|Config")
	void UnloadConfig(FModId ModId);

	/** Drops every user value so reads fall through to the shipped defaults. Does not save. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Config")
	void ResetToDefaults(FModId ModId);

	UFUNCTION(BlueprintPure, Category = "Mod|Config") bool HasConfig(FModId ModId) const;
	UFUNCTION(BlueprintPure, Category = "Mod|Config") bool HasKey(FModId ModId, FName Key) const;
	UFUNCTION(BlueprintPure, Category = "Mod|Config") TArray<FName> GetKeys(FModId ModId) const;

	UFUNCTION(BlueprintPure, Category = "Mod|Config") bool GetBool(FModId ModId, FName Key, bool DefaultValue = false) const;
	UFUNCTION(BlueprintPure, Category = "Mod|Config") int32 GetInt(FModId ModId, FName Key, int32 DefaultValue = 0) const;
	UFUNCTION(BlueprintPure, Category = "Mod|Config") float GetFloat(FModId ModId, FName Key, float DefaultValue = 0.0f) const;
	UFUNCTION(BlueprintPure, Category = "Mod|Config") FString GetString(FModId ModId, FName Key, const FString& DefaultValue = TEXT("")) const;

	UFUNCTION(BlueprintCallable, Category = "Mod|Config") void SetBool(FModId ModId, FName Key, bool Value);
	UFUNCTION(BlueprintCallable, Category = "Mod|Config") void SetInt(FModId ModId, FName Key, int32 Value);
	UFUNCTION(BlueprintCallable, Category = "Mod|Config") void SetFloat(FModId ModId, FName Key, float Value);
	UFUNCTION(BlueprintCallable, Category = "Mod|Config") void SetString(FModId ModId, FName Key, const FString& Value);

	/** The merged view (defaults overlaid with user values), for a mod that wants structure. */
	UFUNCTION(BlueprintPure, Category = "Mod|Config") bool GetConfigJson(FModId ModId, FString& OutJson) const;
	/** Replaces the whole user layer. Rejects malformed JSON rather than corrupting the store. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Config") bool SetConfigJson(FModId ModId, const FString& Json);

	/** Fires after a value changes. Key is NAME_None for whole-store changes (reload, reset). */
	FOnModConfigChanged OnConfigChanged;

	/** A single config file is a settings list, not a database. */
	static constexpr int64 MaxConfigFileBytes = 1024 * 1024;

private:
	struct FModConfigStore
	{
		TSharedPtr<FJsonObject> Defaults;
		TSharedPtr<FJsonObject> Values;
	};

	const FModConfigStore* FindStore(const FModId& ModId) const;
	FModConfigStore* FindOrAddStore(const FModId& ModId);
	/** User layer first, then defaults. Null when neither has the key. */
	TSharedPtr<class FJsonValue> ResolveValue(const FModId& ModId, FName Key) const;
	void NotifyChanged(const FModId& ModId, FName Key);

	// Plain map: FJsonObject is not a UObject, so there is nothing here for GC to trace.
	TMap<FModId, FModConfigStore> Stores;

	TWeakObjectPtr<UModSubsystem> Subsystem;
};
