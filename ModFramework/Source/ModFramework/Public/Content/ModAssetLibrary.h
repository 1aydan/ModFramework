// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "Containers/Array.h"
#include "Content/ModContentTypes.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "ModAssetLibrary.generated.h"

class UClass;
class UObject;

/**
 * Blueprint access to the content a mod brought with it.
 *
 * Everything here is scoped to a single mod's mounts, which is the point: a game's UI can list what
 * a mod added, and a mod's own Blueprints can find their assets by name without hard-coding the
 * package path they ended up mounted at.
 *
 * All of it needs a live UModSubsystem, so pass a world context object that is actually in a world.
 * When there is none the functions return empty results and say so in the log rather than failing.
 */
UCLASS()
class MODFRAMEWORK_API UModAssetLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Every asset the asset registry knows about under the mod's mount points.
	 * A null ClassFilter returns everything; otherwise subclasses are included.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Assets", meta = (WorldContext = "WorldContextObject"))
	static TArray<FAssetData> GetModAssets(const UObject* WorldContextObject, FModId ModId, UClass* ClassFilter);

	/**
	 * Loads one asset of a mod by its short name (the name shown in the content browser, without any
	 * path). Returns nullptr when the mod has no such asset, when it is not of AssetClass, or when
	 * the package fails to load.
	 *
	 * This blocks. Prefer streaming through FStreamableManager for anything on a hot path.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Assets",
		meta = (WorldContext = "WorldContextObject", DeterminesOutputType = "AssetClass"))
	static UObject* LoadModAsset(const UObject* WorldContextObject, FModId ModId, FName AssetName, UClass* AssetClass);

	/**
	 * Loads and returns every class a mod ships that derives from BaseClass - both Blueprint classes
	 * and any native classes stored as assets. A null BaseClass returns all of them.
	 *
	 * This loads each candidate class, which is the only way to answer the question, so it is a
	 * loading-screen operation and not a per-frame one.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Assets", meta = (WorldContext = "WorldContextObject"))
	static TArray<UClass*> GetModClasses(const UObject* WorldContextObject, FModId ModId, UClass* BaseClass);

	/** Which mod an object came from. An invalid id means it is base game or engine content. */
	UFUNCTION(BlueprintPure, Category = "Mod|Assets", meta = (WorldContext = "WorldContextObject"))
	static FModId FindOwningMod(const UObject* WorldContextObject, const UObject* Asset);

	/** Every live content mount of every mod, for mod-manager and debug UI. */
	UFUNCTION(BlueprintPure, Category = "Mod|Assets", meta = (WorldContext = "WorldContextObject"))
	static TArray<FModContentMount> GetModContentMounts(const UObject* WorldContextObject);
};
