// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "ModPackagingSettings.generated.h"

/**
 * Where the Mod Developer window's packaging tab remembers a mod author's choices.
 *
 * WHY PER-PROJECT USER CONFIG RATHER THAN PROJECT SETTINGS
 * An output directory is a fact about one person's machine ("D:/Steam/.../mods"), not about the
 * project, so writing it to DefaultGame.ini would put a local absolute path under source control and
 * hand it to everybody else on the team. `config = EditorPerProjectUserSettings` lands it in
 * Saved/Config/<Platform>/EditorPerProjectUserSettings.ini instead: per user, per project, never
 * committed. The class still shows up under Editor Preferences so the remembered paths can be
 * inspected and cleared by hand.
 *
 * Everything stored here is a path the user picked in a directory dialog. It is re-checked at use
 * time rather than trusted: a remembered directory that has since been deleted or moved is offered
 * as a default and reported if it does not exist, never assumed to be writable.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Mod Packaging"))
class MODFRAMEWORKEDITOR_API UModPackagingSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UModPackagingSettings();

	/** Read-only access to the settings object. Never null once class default objects exist. */
	static const UModPackagingSettings* Get();

	/** Mutable access, for the window's own writes. Follow a write with SaveNow(). */
	static UModPackagingSettings* GetMutable();

	/**
	 * Output directory chosen for each mod, keyed by MakeModKey().
	 *
	 * This is the whole point of the class: a mod author packages the same mod into the same folder
	 * dozens of times a day, and should pick that folder exactly once.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Mod Packaging")
	TMap<FString, FString> OutputDirectoryByMod;

	/** Offered for a mod that has no remembered directory of its own. */
	UPROPERTY(config, EditAnywhere, Category = "Mod Packaging")
	FString LastOutputDirectory;

	/** Where the "browse for a mod folder" dialog opens. */
	UPROPERTY(config, EditAnywhere, Category = "Mod Packaging")
	FString LastSourceDirectory;

	/** Open the containing folder and select the `.mod` file once packaging succeeds. */
	UPROPERTY(config, EditAnywhere, Category = "Mod Packaging")
	bool bRevealPackageInExplorer = true;

	/**
	 * Upper bound on OutputDirectoryByMod, so an author who packages hundreds of one-off folders does
	 * not grow the ini file without limit. Once the map is full, new mods fall back to
	 * LastOutputDirectory instead of getting an entry of their own.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Mod Packaging", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 MaxRememberedOutputDirectories = 256;

	/** The remembered directory for a key, or LastOutputDirectory when there is none. May be empty. */
	FString FindOutputDirectory(const FString& InKey) const;

	/** Records a directory for a key and as the global fallback. Ignores an empty key or directory. */
	void RememberOutputDirectory(const FString& InKey, const FString& InDirectory);

	/** Records where the source-folder dialog should open next time. Ignores an empty directory. */
	void RememberSourceDirectory(const FString& InDirectory);

	/** Writes the ini immediately rather than waiting for editor shutdown to flush it. */
	void SaveNow();

	/**
	 * The map key for one mod: its manifest id when it has a usable one, otherwise the lower-cased
	 * absolute source directory.
	 *
	 * Keying by id is what makes the remembered path survive the author moving the mod folder; the
	 * path fallback is what keeps a folder whose mod.json does not parse from colliding with every
	 * other unparseable folder under one empty key.
	 */
	static FString MakeModKey(const FString& InModId, const FString& InSourceDirectory);
};
