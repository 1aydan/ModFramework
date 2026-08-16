// Copyright (c) 2026. Licensed for use in your own projects.

#include "Settings/ModFrameworkSettings.h"

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"
#include "UObject/UObjectGlobals.h"

namespace ModFrameworkSettingsPrivate
{
	/** Path tokens understood by ModSearchDirectories, expanded in this order. */
	constexpr const TCHAR* ProjectToken = TEXT("{Project}");
	constexpr const TCHAR* ProjectSavedToken = TEXT("{ProjectSaved}");
	constexpr const TCHAR* ProjectUserToken = TEXT("{ProjectUser}");
	constexpr const TCHAR* EngineToken = TEXT("{Engine}");

	/** Command line switches that add extra search directories. */
	constexpr const TCHAR* ModsSwitch = TEXT("-mods=");
	constexpr const TCHAR* ModDirSwitch = TEXT("-moddir=");

	/**
	 * Bounds the icon caps are clamped into.
	 *
	 * The upper icon size bound stays well inside int32 so that the byte count can be handed to
	 * TArray<uint8>, whose index type is int32, without any further checking downstream.
	 */
	constexpr int64 MinIconFileBytes = 1024;
	constexpr int64 MaxIconFileBytesLimit = 64 * 1024 * 1024;
	constexpr int32 MinIconDimension = 16;
	constexpr int32 MaxIconDimensionLimit = 8192;

	/**
	 * Trailing separators are stripped from every token value so that the documented spelling
	 * "{ProjectSaved}/Mods" produces one separator rather than two.
	 */
	FString TrimTrailingSeparators(const FString& In)
	{
		int32 End = In.Len();
		while (End > 0 && (In[End - 1] == TEXT('/') || In[End - 1] == TEXT('\\')))
		{
			--End;
		}

		return In.Left(End);
	}

	FString ExpandTokens(const FString& In)
	{
		// {ProjectSaved} and {ProjectUser} are expanded before {Project} would be relevant, but the
		// braces make the tokens unambiguous either way; the order here is only for readability.
		FString Result = In;
		Result = Result.Replace(ProjectSavedToken, *TrimTrailingSeparators(FPaths::ProjectSavedDir()), ESearchCase::IgnoreCase);
		Result = Result.Replace(ProjectUserToken, *TrimTrailingSeparators(FPaths::ProjectUserDir()), ESearchCase::IgnoreCase);
		Result = Result.Replace(ProjectToken, *TrimTrailingSeparators(FPaths::ProjectDir()), ESearchCase::IgnoreCase);
		Result = Result.Replace(EngineToken, *TrimTrailingSeparators(FPaths::EngineDir()), ESearchCase::IgnoreCase);
		return Result;
	}

	/** Absolute, forward-slashed, without a trailing separator and with "a/../b" collapsed. */
	FString NormalizeDirectory(const FString& In)
	{
		FString Result = In;
		FPaths::NormalizeFilename(Result);
		FPaths::RemoveDuplicateSlashes(Result);
		Result = FPaths::ConvertRelativePathToFull(MoveTemp(Result));
		FPaths::CollapseRelativeDirectories(Result);
		FPaths::RemoveDuplicateSlashes(Result);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	/** Appends Directory unless an equivalent directory is already present. */
	void AddUniqueDirectory(TArray<FString>& InOutDirectories, FString Directory)
	{
		if (Directory.IsEmpty())
		{
			return;
		}

		for (const FString& Existing : InOutDirectories)
		{
			// FPaths::IsSamePath applies the platform's own case rules, so two spellings of the same
			// directory collapse on Windows without merging genuinely distinct paths elsewhere.
			if (FPaths::IsSamePath(Existing, Directory))
			{
				return;
			}
		}

		InOutDirectories.Add(MoveTemp(Directory));
	}

	/**
	 * Collects every value of one command line switch.
	 *
	 * bShouldStopOnSeparator is false so a comma separated list survives FParse; the list is split
	 * here instead. Repeated switches are found by restarting the search from the position FParse
	 * reports it stopped at.
	 */
	void CollectSwitchValues(const TCHAR* CommandLine, const TCHAR* Switch, TArray<FString>& OutValues)
	{
		if (CommandLine == nullptr)
		{
			return;
		}

		const TCHAR* Stream = CommandLine;
		while (Stream != nullptr && *Stream != TEXT('\0'))
		{
			FString Value;
			const TCHAR* Next = nullptr;
			if (!FParse::Value(Stream, Switch, Value, /*bShouldStopOnSeparator=*/false, &Next))
			{
				break;
			}

			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/true);
			for (FString& Part : Parts)
			{
				FString Trimmed = Part.TrimStartAndEnd();
				if (!Trimmed.IsEmpty())
				{
					OutValues.Add(MoveTemp(Trimmed));
				}
			}

			// Defensive: FParse always reports a position past the match, but a stalled pointer would
			// otherwise spin forever on untrusted input.
			if (Next == nullptr || Next <= Stream)
			{
				break;
			}

			Stream = Next;
		}
	}
}

UModFrameworkSettings::UModFrameworkSettings()
{
	// Mirrors GetCategoryName() below for the engine code paths that read the member directly.
	CategoryName = FName(TEXT("Plugins"));

	GameId = TEXT("com.yourstudio.yourgame");
	GameVersion = TEXT("1.0.0");
	SdkId = TEXT("");
	SdkVersion = TEXT("1.0.0");

	// Saved/Mods first: it is the writable location a mod manager installs into, and it is already
	// excluded from source control in a stock project.
	ModSearchDirectories.Add(TEXT("{ProjectSaved}/Mods"));
	ModSearchDirectories.Add(TEXT("{Project}/Mods"));

	AutoGrantedPermissions.Add(FName(TEXT("assets.read")));
	AlwaysDeniedPermissions.Add(FName(TEXT("native_code")));
}

const UModFrameworkSettings* UModFrameworkSettings::Get()
{
	return GetDefault<UModFrameworkSettings>();
}

FName UModFrameworkSettings::GetCategoryName() const
{
	return FName(TEXT("Plugins"));
}

TArray<FString> UModFrameworkSettings::GetResolvedSearchDirectories() const
{
	using namespace ModFrameworkSettingsPrivate;

	TArray<FString> Resolved;
	Resolved.Reserve(ModSearchDirectories.Num() + 2);

	for (const FString& Configured : ModSearchDirectories)
	{
		const FString Trimmed = Configured.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			continue;
		}

		AddUniqueDirectory(Resolved, NormalizeDirectory(ExpandTokens(Trimmed)));
	}

	if (bScanCommandLineDirectories && FCommandLine::IsInitialized())
	{
		const TCHAR* CommandLine = FCommandLine::Get();

		TArray<FString> CommandLineValues;
		CollectSwitchValues(CommandLine, ModsSwitch, CommandLineValues);
		CollectSwitchValues(CommandLine, ModDirSwitch, CommandLineValues);

		for (const FString& Value : CommandLineValues)
		{
			// Command line paths get the same token expansion, so -mods={Project}/TestMods works.
			AddUniqueDirectory(Resolved, NormalizeDirectory(ExpandTokens(Value)));
		}
	}

	return Resolved;
}

int64 UModFrameworkSettings::GetEffectiveMaxIconFileBytes() const
{
	using namespace ModFrameworkSettingsPrivate;

	return FMath::Clamp(MaxIconFileBytes, MinIconFileBytes, MaxIconFileBytesLimit);
}

int32 UModFrameworkSettings::GetEffectiveMaxIconDimension() const
{
	using namespace ModFrameworkSettingsPrivate;

	return FMath::Clamp(MaxIconDimension, MinIconDimension, MaxIconDimensionLimit);
}
