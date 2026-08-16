// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPtr.h"

#include "ModFrameworkSettings.generated.h"

class UTexture2D;

/**
 * Project settings for the mod framework, saved to DefaultGame.ini and edited under
 * Project Settings > Plugins > Mod Framework.
 *
 * Nothing here is per-mod: this is how the *game* describes itself to mods (its id and version, the
 * SDK it ships) and how it wants the framework to behave (where to look for mods, what to grant
 * automatically, how strict to be about conflicts).
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Mod Framework"))
class MODFRAMEWORK_API UModFrameworkSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UModFrameworkSettings();

	/** The project's settings. Valid from the moment class default objects exist. */
	static const UModFrameworkSettings* Get();

	//~ Begin UDeveloperSettings interface
	virtual FName GetCategoryName() const override;
	//~ End UDeveloperSettings interface

	/**
	 * Reverse-DNS identifier of this game, matched against every manifest's `game.id`.
	 * Change it once, before shipping: every published mod pins it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Identity")
	FString GameId;

	/** Semantic version of the game, matched against every manifest's `game.version` range. */
	UPROPERTY(config, EditAnywhere, Category = "Identity")
	FString GameVersion;

	/** Identifier of the game's modding SDK. Empty when the game ships no SDK. */
	UPROPERTY(config, EditAnywhere, Category = "Identity")
	FString SdkId;

	/** Semantic version of the modding SDK, matched against every manifest's `sdk.version` range. */
	UPROPERTY(config, EditAnywhere, Category = "Identity")
	FString SdkVersion;

	/**
	 * Directories scanned for mods, in order.
	 * Supports the tokens {Project}, {ProjectSaved}, {ProjectUser} and {Engine}; see
	 * GetResolvedSearchDirectories.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Discovery")
	TArray<FString> ModSearchDirectories;

	/** Also honour -mods=<path> and -moddir=<path> on the command line. */
	UPROPERTY(config, EditAnywhere, Category = "Discovery")
	bool bScanCommandLineDirectories = true;

	/** Upper bound on how many mods the framework will register in one session. */
	UPROPERTY(config, EditAnywhere, Category = "Discovery")
	int32 MaxMods = 512;

	UPROPERTY(config, EditAnywhere, Category = "Loading")
	bool bAutoDiscoverOnStartup = true;

	UPROPERTY(config, EditAnywhere, Category = "Loading")
	bool bAutoLoadDiscoveredMods = true;

	UPROPERTY(config, EditAnywhere, Category = "Loading")
	bool bAutoActivateLoadedMods = true;

	/** Mod ids that are never loaded, whatever the providers discover. */
	UPROPERTY(config, EditAnywhere, Category = "Loading")
	TArray<FString> DisabledModIds;

	/** Loose (unpacked) content mounts are a development convenience; leave this off when shipping. */
	UPROPERTY(config, EditAnywhere, Category = "Content")
	bool bAllowLooseContentMounts = false;

	UPROPERTY(config, EditAnywhere, Category = "Content")
	bool bVerifyContentHashes = true;

	/** Permissions granted without asking, provided the permission is not marked dangerous. */
	UPROPERTY(config, EditAnywhere, Category = "Permissions")
	TArray<FName> AutoGrantedPermissions;

	/** Permissions no mod ever receives, whatever a permission policy says. */
	UPROPERTY(config, EditAnywhere, Category = "Permissions")
	TArray<FName> AlwaysDeniedPermissions;

	/** When true a permission nothing registered is denied outright rather than left pending. */
	UPROPERTY(config, EditAnywhere, Category = "Permissions")
	bool bDenyUnknownPermissions = true;

	/**
	 * Largest icon file UModIconCache will read, in bytes.
	 *
	 * A mod's icon is an arbitrary file supplied by whoever wrote the mod, so the size is checked
	 * before the bytes are loaded rather than after. Use GetEffectiveMaxIconFileBytes rather than this
	 * field: DefaultGame.ini is hand-editable and a nonsensical value there must not disable icons.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Icons")
	int64 MaxIconFileBytes = 2 * 1024 * 1024;

	/**
	 * Largest icon edge, in pixels. An image wider or taller than this is rejected from its header,
	 * before anything decodes it, so a mod cannot cost the game a gigabyte of pixels. Read it through
	 * GetEffectiveMaxIconDimension.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Icons")
	int32 MaxIconDimension = 1024;

	/**
	 * Icon handed out for a mod that ships none, and for every mod whose icon failed to load. Leave it
	 * unset only if the game's mod UI is prepared to draw nothing at all.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Icons")
	TSoftObjectPtr<UTexture2D> DefaultModIcon;

	/** Used for extension points that do not declare a policy of their own. */
	UPROPERTY(config, EditAnywhere, Category = "Conflicts")
	EModConflictPolicy DefaultConflictPolicy = EModConflictPolicy::Error;

	UPROPERTY(config, EditAnywhere, Category = "Debug")
	bool bEnableConsoleCommands = true;

	/**
	 * Expands ModSearchDirectories into absolute, normalised, de-duplicated directory paths.
	 *
	 * Tokens, replaced case insensitively anywhere in the string:
	 *   {Project}       FPaths::ProjectDir()
	 *   {ProjectSaved}  FPaths::ProjectSavedDir()
	 *   {ProjectUser}   FPaths::ProjectUserDir()
	 *   {Engine}        FPaths::EngineDir()
	 *
	 * When bScanCommandLineDirectories is set, every -mods=<path> and -moddir=<path> value on the
	 * command line is appended afterwards. Both switches accept a comma separated list and may be
	 * repeated; a path containing spaces has to be quoted, as -mods="C:/My Mods".
	 *
	 * Entries that resolve to the same directory are collapsed, keeping the first occurrence so the
	 * configured order still decides discovery order.
	 */
	TArray<FString> GetResolvedSearchDirectories() const;

	/**
	 * MaxIconFileBytes clamped to [1 KiB, 64 MiB].
	 *
	 * The ini file this is read from can be edited by hand, and a zero or negative cap would silently
	 * stop every icon in the game from loading. Clamping keeps a typo from looking like a bug in the
	 * framework, and keeps the cap inside what a single TArray<uint8> can hold.
	 */
	int64 GetEffectiveMaxIconFileBytes() const;

	/** MaxIconDimension clamped to [16, 8192] pixels, for the same reason. */
	int32 GetEffectiveMaxIconDimension() const;
};
