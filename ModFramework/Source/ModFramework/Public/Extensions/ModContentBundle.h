// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "CoreTypes.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Extensions/ModExtension.h"
#include "Internationalization/Text.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/SoftObjectPtr.h"

#include "ModContentBundle.generated.h"

/**
 * Data-driven registration payload: the "no C++ required" path into the framework.
 *
 * A mod author creates one of these assets, lists the extension classes the mod contributes and the
 * data assets and tables it ships, and names the bundle in `entryPoint.contentBundles` in mod.json.
 * When the mod activates, the framework instantiates each listed extension class and hands it to
 * UModExtensionRegistry - so a mod can extend a game without shipping a single line of code or even
 * an entry point class.
 *
 * Everything here is a *soft* reference on purpose. The bundle asset itself lives inside the mod's
 * mounted content, and the framework has to be able to inspect what a bundle declares without
 * forcing the whole payload into memory. It also means a bundle whose targets failed to cook still
 * loads: the framework reports the unresolved references as diagnostics rather than refusing the mod.
 *
 * Treat every field as untrusted. A class listed in ExtensionClasses is validated against its
 * extension point at registration time, exactly like a natively registered extension, and one that
 * fails validation only costs its own diagnostic - the rest of the bundle still applies.
 *
 * The primary asset type is "ModContentBundle"; add that type to the game's Asset Manager settings
 * if bundles should be discoverable through the asset manager.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModContentBundle : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Shown in mod-management UI. Falls back to the asset name when the author leaves it empty. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mod")
	FText DisplayName;

	/**
	 * Extension classes instantiated and registered when the owning mod activates, in this order.
	 * Each class carries its own ExtensionPointId, ExtensionId, Priority and ClaimedResourceIds in
	 * its class defaults.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mod")
	TArray<TSoftClassPtr<UModExtension>> ExtensionClasses;

	/** Primary data assets the mod ships; loaded alongside the bundle so game systems can find them. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mod")
	TArray<TSoftObjectPtr<UPrimaryDataAsset>> DataAssets;

	/** Data tables the mod ships, for games that drive content from row handles. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mod")
	TArray<TSoftObjectPtr<UDataTable>> DataTables;

	/**
	 * Extension points this bundle needs the game to have opened.
	 *
	 * Purely declarative: it lets a mod manager tell a player "this mod needs a feature this build of
	 * the game does not have" before anything is loaded, instead of surfacing a pile of
	 * Extension.PointNotFound diagnostics after activation.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mod")
	TArray<FName> RequiredExtensionPoints;

	//~ Begin UObject interface
	/** Always the type "ModContentBundle", so bundles from every mod share one primary asset type. */
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;
	//~ End UObject interface
};
