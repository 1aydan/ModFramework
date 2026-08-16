// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModPackagingSettings.h"

#include "HAL/Platform.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

UModPackagingSettings::UModPackagingSettings()
{
	// Editor Preferences groups per-project user settings under "General" by default. Packaging is a
	// plugin feature, so it belongs beside the other plugin pages instead.
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Mod Packaging");
}

const UModPackagingSettings* UModPackagingSettings::Get()
{
	return GetDefault<UModPackagingSettings>();
}

UModPackagingSettings* UModPackagingSettings::GetMutable()
{
	return GetMutableDefault<UModPackagingSettings>();
}

FString UModPackagingSettings::FindOutputDirectory(const FString& InKey) const
{
	if (!InKey.IsEmpty())
	{
		if (const FString* Found = OutputDirectoryByMod.Find(InKey))
		{
			if (!Found->IsEmpty())
			{
				return *Found;
			}
		}
	}

	return LastOutputDirectory;
}

void UModPackagingSettings::RememberOutputDirectory(const FString& InKey, const FString& InDirectory)
{
	if (InDirectory.IsEmpty())
	{
		return;
	}

	const FString Normalised = FPaths::ConvertRelativePathToFull(InDirectory);
	LastOutputDirectory = Normalised;

	if (InKey.IsEmpty())
	{
		return;
	}

	// Updating an existing key is always allowed; only *growing* past the cap is refused, so an author
	// who is already over the limit keeps working with the mods they have packaged before.
	const int32 Cap = FMath::Max(1, MaxRememberedOutputDirectories);
	if (!OutputDirectoryByMod.Contains(InKey) && OutputDirectoryByMod.Num() >= Cap)
	{
		return;
	}

	OutputDirectoryByMod.Add(InKey, Normalised);
}

void UModPackagingSettings::RememberSourceDirectory(const FString& InDirectory)
{
	if (InDirectory.IsEmpty())
	{
		return;
	}

	LastSourceDirectory = FPaths::ConvertRelativePathToFull(InDirectory);
}

void UModPackagingSettings::SaveNow()
{
	SaveConfig();

	// SaveConfig only marks the branch dirty; without this the file is not on disk until the editor
	// exits cleanly, and a crash in between loses the author's chosen output folder.
	if (GConfig != nullptr)
	{
		const FString ConfigFilename = GetClass()->GetConfigName();
		if (!ConfigFilename.IsEmpty())
		{
			GConfig->Flush(/*bRead*/ false, ConfigFilename);
		}
	}
}

FString UModPackagingSettings::MakeModKey(const FString& InModId, const FString& InSourceDirectory)
{
	const FString TrimmedId = InModId.TrimStartAndEnd();
	if (!TrimmedId.IsEmpty())
	{
		return TrimmedId.ToLower();
	}

	if (InSourceDirectory.IsEmpty())
	{
		return FString();
	}

	FString PathKey = FPaths::ConvertRelativePathToFull(InSourceDirectory);
	FPaths::NormalizeDirectoryName(PathKey);
	return PathKey.ToLower();
}
