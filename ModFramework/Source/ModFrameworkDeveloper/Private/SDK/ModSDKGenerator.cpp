// Copyright (c) 2026. Licensed for use in your own projects.

#include "SDK/ModSDKGenerator.h"

#include "Containers/UnrealString.h"
#include "Core/ModFrameworkVersion.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Manifest/ModVersion.h"
#include "Misc/Char.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModFrameworkDeveloperModule.h"
#include "ModuleDescriptor.h"
#include "PluginDescriptor.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "SDK/ModPublicApiScanner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/ModFrameworkSettings.h"
#include "Templates/SharedPointer.h"

namespace ModSDKGeneratorCodes
{
	const TCHAR* const InvalidOptions = TEXT("Sdk.InvalidOptions");
	const TCHAR* const PluginNotFound = TEXT("Sdk.PluginNotFound");
	const TCHAR* const OutputFailed = TEXT("Sdk.OutputFailed");
	const TCHAR* const CopyFailed = TEXT("Sdk.CopyFailed");
	const TCHAR* const WriteFailed = TEXT("Sdk.WriteFailed");
	const TCHAR* const DocsMissing = TEXT("Sdk.DocsMissing");
	const TCHAR* const ScanRejected = TEXT("Sdk.ScanRejected");
	const TCHAR* const BundleWritten = TEXT("Sdk.BundleWritten");
}

namespace ModSDKGeneratorPrivate
{
	const TCHAR* const FrameworkPluginName = TEXT("ModFramework");

	/** Stops a symlink or junction loop from turning a copy into an infinite walk. */
	constexpr int32 MaxCopyDepth = 64;

	/**
	 * Deliberately not named MakeError. Templates/ValueOrError.h declares a global variadic
	 * MakeError(ArgTypes&&...) that wins overload resolution at any call site reached through a
	 * using-directive and yields an unreadable TValueOrError_ErrorProxy diagnostic.
	 */
	FModDiagnostic MakeGeneratorDiagnostic(EModDiagnosticSeverity InSeverity, const TCHAR* InCode,
		FString InMessage, FString InContext = FString())
	{
		return FModDiagnostic(InSeverity, FName(InCode), MoveTemp(InMessage), MoveTemp(InContext));
	}

	/** Forward slashes, no trailing separator, absolute. */
	FString NormalizeDirectory(const FString& InPath)
	{
		FString Result = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	/** True when any path segment starts with a dot: .dev, .git, .vs, .vscode, .DS_Store. */
	bool HasDotPrefixedSegment(const FString& InRelativePath)
	{
		TArray<FString> Segments;
		InRelativePath.ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty*/ true);
		for (const FString& Segment : Segments)
		{
			if (Segment.StartsWith(TEXT(".")))
			{
				return true;
			}
		}
		return false;
	}

	/** Case-insensitive comparison of the first path segment. */
	bool FirstSegmentIs(const FString& InRelativePath, const TCHAR* InSegment)
	{
		FString First;
		FString Remainder;
		if (!InRelativePath.Split(TEXT("/"), &First, &Remainder))
		{
			First = InRelativePath;
		}
		return First.Equals(InSegment, ESearchCase::IgnoreCase);
	}

	/**
	 * A C++ identifier derived from a folder name, so the generated template's module compiles even
	 * when someone names the template "My Mod (v2)".
	 */
	FString MakeModuleIdentifier(const FString& InName)
	{
		FString Result;
		Result.Reserve(InName.Len());
		for (const TCHAR Character : InName)
		{
			if (FChar::IsAlnum(Character) || Character == TEXT('_'))
			{
				Result.AppendChar(Character);
			}
		}

		if (Result.IsEmpty())
		{
			return TEXT("ModProject");
		}
		if (FChar::IsDigit(Result[0]))
		{
			Result.InsertAt(0, TEXT('M'));
		}
		return Result;
	}

	bool WriteTextFile(const FString& InAbsolutePath, const FString& InContents,
		int32& OutFilesGenerated, TArray<FModDiagnostic>& OutDiagnostics)
	{
		const FString Directory = FPaths::GetPath(InAbsolutePath);
		if (!Directory.IsEmpty() && !IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true))
		{
			OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
				ModSDKGeneratorCodes::WriteFailed, TEXT("Could not create a directory in the bundle."), Directory));
			return false;
		}

		if (!FFileHelper::SaveStringToFile(InContents, *InAbsolutePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
				ModSDKGeneratorCodes::WriteFailed, TEXT("Could not write a generated file."), InAbsolutePath));
			return false;
		}

		++OutFilesGenerated;
		return true;
	}

	/**
	 * Renders a JSON object with the pretty printer used everywhere else in the framework.
	 *
	 * The object is handed over as a TSharedPtr because FJsonSerializer::Serialize takes
	 * Policy::FMapOfValues, which is exactly TSharedPtr<FJsonObject>; and the writer closes itself,
	 * since bCloseWriter defaults to true.
	 */
	bool SerializeJson(const TSharedRef<FJsonObject>& InObject, FString& OutJson)
	{
		OutJson.Reset();

		const TSharedPtr<FJsonObject> Object = InObject;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);

		return FJsonSerializer::Serialize(Object, Writer);
	}

	/** Recursive half of CopyTreeFiltered. Returns false only on a real IO failure. */
	bool CopyDirectoryRecursive(const FString& InSourceDirectory, const FString& InDestinationDirectory,
		const FString& InRelativePrefix, int32 InDepth,
		TFunctionRef<bool(const FString&, bool)> InShouldExclude,
		int32& OutFilesCopied, int64& OutBytesCopied, TArray<FModDiagnostic>& OutDiagnostics)
	{
		if (InDepth > MaxCopyDepth)
		{
			OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Warning,
				ModSDKGeneratorCodes::CopyFailed,
				FString::Printf(TEXT("Stopped descending at depth %d; the tree is either pathological or contains a link loop."),
					MaxCopyDepth),
				InSourceDirectory));
			return true;
		}

		IFileManager& FileManager = IFileManager::Get();

		TArray<FString> Subdirectories;
		bool bOk = true;

		FileManager.IterateDirectory(*InSourceDirectory,
			[&](const TCHAR* InPath, bool bIsDirectory) -> bool
			{
				const FString Leaf = FPaths::GetCleanFilename(InPath);
				if (Leaf.IsEmpty())
				{
					return true;
				}

				const FString Relative = InRelativePrefix.IsEmpty() ? Leaf : InRelativePrefix / Leaf;
				if (InShouldExclude(Relative, bIsDirectory))
				{
					return true;
				}

				if (bIsDirectory)
				{
					Subdirectories.Add(Leaf);
					return true;
				}

				const FString SourceFile = InSourceDirectory / Leaf;
				const FString DestinationFile = InDestinationDirectory / Leaf;

				if (FileManager.Copy(*DestinationFile, *SourceFile, /*Replace*/ true, /*EvenIfReadOnly*/ true) != COPY_OK)
				{
					OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
						ModSDKGeneratorCodes::CopyFailed, TEXT("Could not copy a file into the bundle."), SourceFile));
					bOk = false;
					return true;
				}

				++OutFilesCopied;
				const int64 Size = FileManager.FileSize(*DestinationFile);
				if (Size > 0)
				{
					OutBytesCopied += Size;
				}
				return true;
			});

		for (const FString& Subdirectory : Subdirectories)
		{
			const FString SourceChild = InSourceDirectory / Subdirectory;
			const FString DestinationChild = InDestinationDirectory / Subdirectory;
			const FString RelativeChild = InRelativePrefix.IsEmpty() ? Subdirectory : InRelativePrefix / Subdirectory;

			if (!FileManager.MakeDirectory(*DestinationChild, /*Tree*/ true))
			{
				OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
					ModSDKGeneratorCodes::OutputFailed, TEXT("Could not create a directory in the bundle."),
					DestinationChild));
				bOk = false;
				continue;
			}

			bOk &= CopyDirectoryRecursive(SourceChild, DestinationChild, RelativeChild, InDepth + 1,
				InShouldExclude, OutFilesCopied, OutBytesCopied, OutDiagnostics);
		}

		return bOk;
	}

	/** "5.8" for a 5.8.1 engine - what an .uproject's EngineAssociation wants. */
	FString MakeEngineAssociation()
	{
		const FEngineVersion& Version = FEngineVersion::Current();
		return FString::Printf(TEXT("%u.%u"),
			static_cast<uint32>(Version.GetMajor()), static_cast<uint32>(Version.GetMinor()));
	}
}

//////////////////////////////////////////////////////////////////////////
// Exclusion rules

bool FModSDKGenerator::ShouldExcludeFromPluginCopy(const FString& InRelativePath, bool bIsDirectory,
	bool bIncludeBinaries, bool bIncludeGeneratorSource)
{
	using namespace ModSDKGeneratorPrivate;

	// .dev, .git, .vs, .vscode, .DS_Store - local tooling and editor state, never part of a release.
	if (HasDotPrefixedSegment(InRelativePath))
	{
		return true;
	}

	if (bIsDirectory)
	{
		const FString Leaf = FPaths::GetCleanFilename(InRelativePath);
		if (Leaf.Equals(TEXT("Intermediate"), ESearchCase::IgnoreCase)
			|| Leaf.Equals(TEXT("Saved"), ESearchCase::IgnoreCase)
			|| Leaf.Equals(TEXT("DerivedDataCache"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		if (!bIncludeBinaries && Leaf.Equals(TEXT("Binaries"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	// The SDK generator itself. Shipping it is harmless - it only reads the reflection data of
	// whatever project it runs in - but a mod author has no use for the tool that made their SDK.
	//
	// BOTH halves go or NEITHER does. UGenerateModSDKCommandlet is a UCLASS, so UHT generates
	// reflection code for whatever header it finds and that code calls the constructor defined in
	// the .cpp. Dropping Private/SDK while keeping Public/SDK would hand the mod author a plugin
	// with an unresolved external and no source to fix it with.
	if (!bIncludeGeneratorSource)
	{
		if (InRelativePath.StartsWith(TEXT("Source/ModFrameworkDeveloper/Private/SDK"), ESearchCase::IgnoreCase)
			|| InRelativePath.StartsWith(TEXT("Source/ModFrameworkDeveloper/Public/SDK"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return false;
}

bool FModSDKGenerator::ShouldExcludeFromDocsCopy(const FString& InRelativePath, bool bIsDirectory)
{
	using namespace ModSDKGeneratorPrivate;

	if (HasDotPrefixedSegment(InRelativePath))
	{
		return true;
	}

	// docs/internal holds the implementation contract and the internal status document. It must
	// never reach a mod author, and the check is on the first segment so a legitimately named
	// "Internal" page deeper in the tree is not caught by accident.
	if (FirstSegmentIs(InRelativePath, TEXT("internal")))
	{
		return true;
	}

	(void)bIsDirectory;
	return false;
}

bool FModSDKGenerator::CopyTreeFiltered(const FString& InSourceDirectory, const FString& InDestinationDirectory,
	TFunctionRef<bool(const FString&, bool)> InShouldExclude,
	int32& OutFilesCopied, int64& OutBytesCopied, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModSDKGeneratorPrivate;

	const FString Source = NormalizeDirectory(InSourceDirectory);
	const FString Destination = NormalizeDirectory(InDestinationDirectory);

	if (!IFileManager::Get().DirectoryExists(*Source))
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::CopyFailed, TEXT("The source directory does not exist."), Source));
		return false;
	}

	if (!IFileManager::Get().MakeDirectory(*Destination, /*Tree*/ true))
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::OutputFailed, TEXT("Could not create the destination directory."), Destination));
		return false;
	}

	return CopyDirectoryRecursive(Source, Destination, FString(), 0, InShouldExclude,
		OutFilesCopied, OutBytesCopied, OutDiagnostics);
}

//////////////////////////////////////////////////////////////////////////
// Options

FString FModSDKGenerator::GetDefaultOutputDirectory()
{
	return ModSDKGeneratorPrivate::NormalizeDirectory(FPaths::ProjectSavedDir() / TEXT("ModSDK"));
}

FString FModSDKGenerator::MakeBundleFolderName(const FString& InSdkName, const FString& InSdkVersion)
{
	const FString Name = InSdkName.IsEmpty() ? TEXT("ModSDK") : InSdkName;
	return InSdkVersion.IsEmpty() ? Name : FString::Printf(TEXT("%s-%s"), *Name, *InSdkVersion);
}

bool FModSDKGenerator::ResolveOptions(FModSDKGenerateOptions& InOutOptions, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModSDKGeneratorPrivate;

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();

	if (InOutOptions.SdkPluginName.IsEmpty())
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::InvalidOptions,
			TEXT("No SDK plugin name was supplied. Pass -SdkPlugin=<PluginName>; it is the plugin that holds the game's public modding surface.")));
		return false;
	}

	const TSharedPtr<IPlugin> SdkPlugin = IPluginManager::Get().FindPlugin(InOutOptions.SdkPluginName);
	if (!SdkPlugin.IsValid())
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::PluginNotFound,
			FString::Printf(TEXT("No plugin named \"%s\" is known to this project. SDK generation locates plugins through IPluginManager, so the plugin has to be discovered (it does not have to be enabled)."),
				*InOutOptions.SdkPluginName)));
		return false;
	}

	const TSharedPtr<IPlugin> FrameworkPlugin = IPluginManager::Get().FindPlugin(FrameworkPluginName);
	if (!FrameworkPlugin.IsValid())
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::PluginNotFound,
			TEXT("The ModFramework plugin could not be located. A bundle without it is not installable.")));
		return false;
	}

	if (InOutOptions.SdkName.IsEmpty())
	{
		InOutOptions.SdkName = InOutOptions.SdkPluginName;
	}

	if (InOutOptions.SdkVersion.IsEmpty() && Settings)
	{
		InOutOptions.SdkVersion = Settings->SdkVersion;
	}
	if (InOutOptions.SdkVersion.IsEmpty())
	{
		InOutOptions.SdkVersion = SdkPlugin->GetDescriptor().VersionName;
	}
	if (InOutOptions.SdkVersion.IsEmpty())
	{
		InOutOptions.SdkVersion = TEXT("0.1.0");
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Warning,
			ModSDKGeneratorCodes::InvalidOptions,
			TEXT("Neither the project settings nor the SDK plugin descriptor declares an SDK version; the bundle is stamped 0.1.0. ")
			TEXT("Set SdkVersion under Project Settings > Plugins > Mod Framework before publishing - every mod pins it.")));
	}
	else
	{
		FModVersion Parsed;
		FString ParseError;
		if (!FModVersion::Parse(InOutOptions.SdkVersion, Parsed, &ParseError, /*bAllowPartial*/ true))
		{
			OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Warning,
				ModSDKGeneratorCodes::InvalidOptions,
				FString::Printf(TEXT("The SDK version \"%s\" is not valid semver (%s). Mods pin this with a range expression, which will not match."),
					*InOutOptions.SdkVersion, *ParseError)));
		}
	}

	if (InOutOptions.OutputDirectory.IsEmpty())
	{
		InOutOptions.OutputDirectory = GetDefaultOutputDirectory();
	}
	InOutOptions.OutputDirectory = NormalizeDirectory(InOutOptions.OutputDirectory);

	if (InOutOptions.DocsDirectory.IsEmpty() && InOutOptions.bIncludeDocs)
	{
		// This repository keeps its public documentation in a "docs" folder beside the plugin
		// folders. Derived from the plugin's own location rather than hardcoded, so a project that
		// junctions the plugin in still finds the right tree - and simply has no docs when it does
		// not, which is not an error.
		const FString FrameworkBaseDir = NormalizeDirectory(FrameworkPlugin->GetBaseDir());
		InOutOptions.DocsDirectory = NormalizeDirectory(FPaths::GetPath(FrameworkBaseDir) / TEXT("docs"));
	}

	if (InOutOptions.TemplateProjectName.IsEmpty())
	{
		InOutOptions.TemplateProjectName = TEXT("ModProject");
	}

	if (InOutOptions.EngineAssociation.IsEmpty())
	{
		InOutOptions.EngineAssociation = MakeEngineAssociation();
	}

	// The scanner must know which modules ship inside the bundle, otherwise every unmarked helper
	// type in the SDK plugin itself would be reported as a leak.
	InOutOptions.ScanOptions.ShippingPluginNames.AddUnique(FrameworkPluginName);
	InOutOptions.ScanOptions.ShippingPluginNames.AddUnique(InOutOptions.SdkPluginName);

	return true;
}

//////////////////////////////////////////////////////////////////////////
// Generated files

bool FModSDKGenerator::BuildVersionJson(const FModSDKGenerateOptions& InOptions,
	const FModSDKGenerateResult& InResult, FString& OutJson)
{
	using namespace ModSDKGeneratorPrivate;

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("sdkName"), InOptions.SdkName);
	Root->SetStringField(TEXT("generatedAtUtc"), FDateTime::UtcNow().ToIso8601());

	// The five versions a mod author has to pin, plus the two wire-format numbers the framework
	// itself validates a mod against.
	const TSharedRef<FJsonObject> Versions = MakeShared<FJsonObject>();
	Versions->SetStringField(TEXT("engine"), InResult.EngineVersion);
	Versions->SetStringField(TEXT("game"), InResult.GameVersion);
	Versions->SetStringField(TEXT("framework"), InResult.FrameworkVersion);
	Versions->SetStringField(TEXT("sdk"), InResult.SdkVersion);
	Versions->SetNumberField(TEXT("manifest"), InResult.ManifestVersion);
	Versions->SetNumberField(TEXT("packageFormat"), InResult.PackageFormatVersion);
	Root->SetObjectField(TEXT("versions"), Versions);

	const TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
	Identity->SetStringField(TEXT("gameId"), InResult.GameId);
	Identity->SetStringField(TEXT("sdkId"), InResult.SdkId);
	Root->SetObjectField(TEXT("identity"), Identity);

	// What a mod.json in this SDK should declare, ready to paste.
	const TSharedRef<FJsonObject> Requirements = MakeShared<FJsonObject>();
	Requirements->SetStringField(TEXT("game"), FString::Printf(TEXT("^%s"), *InResult.GameVersion));
	Requirements->SetStringField(TEXT("framework"), FString::Printf(TEXT("^%s"), *InResult.FrameworkVersion));
	Requirements->SetStringField(TEXT("sdk"), FString::Printf(TEXT("^%s"), *InResult.SdkVersion));
	Root->SetObjectField(TEXT("suggestedManifestRanges"), Requirements);

	Root->SetObjectField(TEXT("api"), FModPublicApiScanner::BuildApiIndexJson(InResult.ApiReport));

	return SerializeJson(Root, OutJson);
}

namespace ModSDKGeneratorPrivate
{
	/** The .uproject the generated template ships. Exactly one plugin, by design. */
	FString BuildTemplateUProject(const FModSDKGenerateOptions& InOptions, const FString& InModuleName)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("FileVersion"), 3);
		Root->SetStringField(TEXT("EngineAssociation"), InOptions.EngineAssociation);
		Root->SetStringField(TEXT("Category"), TEXT("Modding"));
		Root->SetStringField(TEXT("Description"),
			FString::Printf(TEXT("Starting point for a mod built against the %s. Contains the SDK and nothing from the game itself."),
				*InOptions.SdkName));

		const TSharedRef<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
		ModuleObject->SetStringField(TEXT("Name"), InModuleName);
		ModuleObject->SetStringField(TEXT("Type"), TEXT("Runtime"));
		ModuleObject->SetStringField(TEXT("LoadingPhase"), TEXT("Default"));

		TArray<TSharedPtr<FJsonValue>> ModuleValues;
		ModuleValues.Add(MakeShared<FJsonValueObject>(ModuleObject));
		Root->SetArrayField(TEXT("Modules"), ModuleValues);

		// ONE plugin. The SDK's own descriptor lists ModFramework as a dependency, so the engine
		// enables it automatically and a mod author never installs, chooses, or thinks about the
		// framework. If this list ever needs ModFramework spelled out, the SDK descriptor is wrong.
		const TSharedRef<FJsonObject> PluginObject = MakeShared<FJsonObject>();
		PluginObject->SetStringField(TEXT("Name"), InOptions.SdkPluginName);
		PluginObject->SetBoolField(TEXT("Enabled"), true);

		TArray<TSharedPtr<FJsonValue>> PluginValues;
		PluginValues.Add(MakeShared<FJsonValueObject>(PluginObject));
		Root->SetArrayField(TEXT("Plugins"), PluginValues);

		FString Json;
		SerializeJson(Root, Json);
		return Json;
	}

	FString BuildTemplateTargetCs(const FString& InModuleName, bool bEditorTarget)
	{
		return FString::Printf(
			TEXT("// Copyright (c) 2026. Licensed for use in your own projects.\r\n")
			TEXT("\r\n")
			TEXT("using UnrealBuildTool;\r\n")
			TEXT("\r\n")
			TEXT("public class %s%sTarget : TargetRules\r\n")
			TEXT("{\r\n")
			TEXT("\tpublic %s%sTarget(TargetInfo Target) : base(Target)\r\n")
			TEXT("\t{\r\n")
			TEXT("\t\tType = TargetType.%s;\r\n")
			TEXT("\t\tDefaultBuildSettings = BuildSettingsVersion.V7;\r\n")
			TEXT("\t\tIncludeOrderVersion = EngineIncludeOrderVersion.Latest;\r\n")
			TEXT("\t\tExtraModuleNames.Add(\"%s\");\r\n")
			TEXT("\t}\r\n")
			TEXT("}\r\n"),
			*InModuleName, bEditorTarget ? TEXT("Editor") : TEXT(""),
			*InModuleName, bEditorTarget ? TEXT("Editor") : TEXT(""),
			bEditorTarget ? TEXT("Editor") : TEXT("Game"),
			*InModuleName);
	}

	FString BuildTemplateBuildCs(const FString& InModuleName, const FString& InSdkModuleName)
	{
		return FString::Printf(
			TEXT("// Copyright (c) 2026. Licensed for use in your own projects.\r\n")
			TEXT("\r\n")
			TEXT("using UnrealBuildTool;\r\n")
			TEXT("\r\n")
			TEXT("public class %s : ModuleRules\r\n")
			TEXT("{\r\n")
			TEXT("\tpublic %s(ReadOnlyTargetRules Target) : base(Target)\r\n")
			TEXT("\t{\r\n")
			TEXT("\t\tPCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;\r\n")
			TEXT("\r\n")
			TEXT("\t\t// The SDK, and nothing else. ModFramework is not listed: the SDK depends on it\r\n")
			TEXT("\t\t// publicly, so its headers arrive transitively. If something a mod needs is not\r\n")
			TEXT("\t\t// reachable from the SDK, the game has to expose it through a UModAPI - widening\r\n")
			TEXT("\t\t// this list is not the fix, because the game's own modules do not exist here.\r\n")
			TEXT("\t\tPublicDependencyModuleNames.AddRange(new string[]\r\n")
			TEXT("\t\t{\r\n")
			TEXT("\t\t\t\"Core\",\r\n")
			TEXT("\t\t\t\"CoreUObject\",\r\n")
			TEXT("\t\t\t\"Engine\",\r\n")
			TEXT("\t\t\t\"%s\"\r\n")
			TEXT("\t\t});\r\n")
			TEXT("\t}\r\n")
			TEXT("}\r\n"),
			*InModuleName, *InModuleName, *InSdkModuleName);
	}

	FString BuildTemplateModuleCpp(const FString& InModuleName)
	{
		return FString::Printf(
			TEXT("// Copyright (c) 2026. Licensed for use in your own projects.\r\n")
			TEXT("\r\n")
			TEXT("#include \"CoreMinimal.h\"\r\n")
			TEXT("#include \"Modules/ModuleManager.h\"\r\n")
			TEXT("\r\n")
			TEXT("// A Blueprint and Data Asset mod needs no C++ at all. This module exists so the project\r\n")
			TEXT("// can compile the bundled plugins from source; delete nothing, add your own C++ here if\r\n")
			TEXT("// you want it.\r\n")
			TEXT("IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, %s, \"%s\");\r\n"),
			*InModuleName, *InModuleName);
	}

	FString BuildTemplateDefaultEngineIni()
	{
		return
			TEXT("[/Script/EngineSettings.GameMapsSettings]\r\n")
			TEXT("GameDefaultMap=/Engine/Maps/Templates/OpenWorld\r\n")
			TEXT("EditorStartupMap=/Engine/Maps/Templates/OpenWorld\r\n");
	}

	FString BuildTemplateDefaultGameIni(const FString& InProjectName, const FModSDKGenerateResult& InResult)
	{
		return FString::Printf(
			TEXT("[/Script/EngineSettings.GeneralProjectSettings]\r\n")
			TEXT("ProjectName=%s\r\n")
			TEXT("\r\n")
			TEXT("; The identity below describes the GAME THIS MOD TARGETS, not this project. The mod\r\n")
			TEXT("; developer tooling validates a mod's manifest against it before packaging.\r\n")
			TEXT("[/Script/ModFramework.ModFrameworkSettings]\r\n")
			TEXT("GameId=%s\r\n")
			TEXT("GameVersion=%s\r\n")
			TEXT("SdkId=%s\r\n")
			TEXT("SdkVersion=%s\r\n")
			TEXT("; This project authors a mod, it does not run one - nothing is discovered automatically.\r\n")
			TEXT("bAutoDiscoverOnStartup=False\r\n")
			TEXT("bAutoLoadDiscoveredMods=False\r\n")
			TEXT("bAutoActivateLoadedMods=False\r\n")
			TEXT("bAllowLooseContentMounts=True\r\n")
			TEXT("bEnableConsoleCommands=True\r\n"),
			*InProjectName, *InResult.GameId, *InResult.GameVersion, *InResult.SdkId, *InResult.SdkVersion);
	}

	/** A starter mod.json, pre-filled with everything this SDK already knows. */
	FString BuildTemplateModManifest(const FModSDKGenerateResult& InResult)
	{
		const FString GameRange = InResult.GameVersion.IsEmpty()
			? TEXT("*") : FString::Printf(TEXT("^%s"), *InResult.GameVersion);
		const FString SdkRange = InResult.SdkVersion.IsEmpty()
			? TEXT("*") : FString::Printf(TEXT("^%s"), *InResult.SdkVersion);

		return FString::Printf(
			TEXT("{\r\n")
			TEXT("\t\"manifestVersion\": %d,\r\n")
			TEXT("\r\n")
			TEXT("\t\"id\": \"com.example.mymod\",\r\n")
			TEXT("\t\"name\": \"My Mod\",\r\n")
			TEXT("\t\"description\": \"Describe what this mod changes.\",\r\n")
			TEXT("\t\"author\": \"Your name\",\r\n")
			TEXT("\t\"version\": \"1.0.0\",\r\n")
			TEXT("\r\n")
			TEXT("\t\"game\": { \"id\": \"%s\", \"version\": \"%s\" },\r\n")
			TEXT("\t\"framework\": { \"version\": \"^%s\" },\r\n")
			TEXT("\t\"sdk\": { \"id\": \"%s\", \"version\": \"%s\" },\r\n")
			TEXT("\r\n")
			TEXT("\t\"dependencies\": [],\r\n")
			TEXT("\t\"priority\": 0,\r\n")
			TEXT("\r\n")
			TEXT("\t\"network\": { \"scope\": \"ClientAndServer\", \"requiredForNetworkPlay\": true },\r\n")
			TEXT("\r\n")
			TEXT("\t\"permissions\": [],\r\n")
			TEXT("\r\n")
			TEXT("\t\"entryPoint\": { \"class\": \"\" },\r\n")
			TEXT("\r\n")
			TEXT("\t\"content\": []\r\n")
			TEXT("}\r\n"),
			InResult.ManifestVersion, *InResult.GameId, *GameRange, *InResult.FrameworkVersion,
			*InResult.SdkId, *SdkRange);
	}
}

FString FModSDKGenerator::BuildReadme(const FModSDKGenerateOptions& InOptions, const FModSDKGenerateResult& InResult)
{
	const FModPublicApiReport& Report = InResult.ApiReport;

	FString Text;
	Text.Reserve(8192);

	Text += FString::Printf(TEXT("# %s %s\r\n\r\n"), *InOptions.SdkName, *InResult.SdkVersion);
	Text += FString::Printf(
		TEXT("The modding SDK for **%s** %s. Everything a mod needs to build against the game, and nothing from inside it.\r\n\r\n"),
		InResult.GameId.IsEmpty() ? TEXT("this game") : *InResult.GameId, *InResult.GameVersion);

	Text += TEXT("## Versions\r\n\r\n");
	Text += TEXT("| Component | Version |\r\n|---|---|\r\n");
	Text += FString::Printf(TEXT("| Unreal Engine | %s |\r\n"), *InResult.EngineVersion);
	Text += FString::Printf(TEXT("| Game | %s |\r\n"), *InResult.GameVersion);
	Text += FString::Printf(TEXT("| Mod Framework | %s |\r\n"), *InResult.FrameworkVersion);
	Text += FString::Printf(TEXT("| SDK | %s |\r\n"), *InResult.SdkVersion);
	Text += FString::Printf(TEXT("| `mod.json` schema | %d |\r\n"), InResult.ManifestVersion);
	Text += FString::Printf(TEXT("| `.mod` package format | %d |\r\n\r\n"), InResult.PackageFormatVersion);
	Text += TEXT("The same numbers are in `SDKVersion.json`, along with a machine-readable index of everything below.\r\n\r\n");

	Text += TEXT("## What is in here\r\n\r\n");
	Text += TEXT("- `Plugins/ModFramework/` - the mod loader: discovery, manifests, lifecycle, permissions, packaging.\r\n");
	Text += FString::Printf(
		TEXT("- `Plugins/%s/` - the game's public modding surface: APIs, extension points, events, data assets.\r\n"),
		*InOptions.SdkPluginName);
	if (InResult.bTemplateIncluded)
	{
		Text += FString::Printf(TEXT("- `Templates/%s/` - a project to start your mod from.\r\n"),
			*InOptions.TemplateProjectName);
	}
	if (InResult.bDocsIncluded)
	{
		Text += TEXT("- `Docs/` - manifest format, permissions, packaging, multiplayer, versioning, security.\r\n");
	}
	Text += TEXT("- `SDKVersion.json` - every version to pin, plus a machine-readable index of the surface below.\r\n");
	if (InResult.bApiReportIncluded)
	{
		Text += TEXT("- `ApiReport.json` - the full reflection scan this bundle was built from, diagnostics included.\r\n");
	}
	Text += TEXT("\r\n");

	Text += TEXT("## Getting started\r\n\r\n");
	if (InResult.bTemplateIncluded)
	{
		Text += FString::Printf(
			TEXT("1. Copy `Templates/%s` somewhere and rename it.\r\n"), *InOptions.TemplateProjectName);
		Text += FString::Printf(
			TEXT("2. Copy this bundle's `Plugins/ModFramework` and `Plugins/%s` into your copy's `Plugins/` folder.\r\n"),
			*InOptions.SdkPluginName);
		Text += TEXT("3. Right-click the `.uproject`, generate project files, and build once. ")
			TEXT("The plugins ship as source, so the first build compiles them.\r\n");
		Text += TEXT("4. Open the project. Author your mod as Blueprints and Data Assets under `Content/`.\r\n");
		Text += TEXT("5. Fill in `Mod/mod.json`, then package with the `PackageMod` commandlet or the Mod Developer window.\r\n\r\n");
		Text += FString::Printf(
			TEXT("The template's `.uproject` enables exactly one plugin - `%s`. The framework arrives automatically because the SDK declares it as a dependency; you never install it yourself.\r\n\r\n"),
			*InOptions.SdkPluginName);
	}
	else
	{
		Text += FString::Printf(
			TEXT("Copy `Plugins/ModFramework` and `Plugins/%s` into your project's `Plugins/` folder and enable `%s`. ")
			TEXT("The framework arrives automatically because the SDK declares it as a dependency.\r\n\r\n"),
			*InOptions.SdkPluginName, *InOptions.SdkPluginName);
	}

	if (Report.Apis.Num() > 0)
	{
		Text += TEXT("## APIs\r\n\r\n");
		Text += TEXT("Request one from your mod's `UModContext`. Every listed permission has to be granted first, ")
			TEXT("and a server-authoritative API refuses calls made on a client.\r\n\r\n");
		Text += TEXT("| Id | Version | Class | Permissions | Server authoritative |\r\n|---|---|---|---|---|\r\n");
		for (const FModPublicApiEntry& Api : Report.Apis)
		{
			const FString Permissions = Api.RequiredPermissions.Num() > 0
				? FString::Join(Api.RequiredPermissions, TEXT(", "))
				: TEXT("none");
			Text += FString::Printf(TEXT("| `%s` | %s | `U%s` | %s | %s |\r\n"),
				*Api.ApiId,
				Api.Version.IsEmpty() ? TEXT("1.0.0") : *Api.Version,
				*Api.ClassName, *Permissions,
				Api.bServerAuthoritative ? TEXT("yes") : TEXT("no"));
		}
		Text += TEXT("\r\n");
	}

	if (Report.ExtensionPoints.Num() > 0)
	{
		Text += TEXT("## Extension points\r\n\r\n");
		Text += TEXT("Subclass the base class - in Blueprint is fine - and register an instance from your mod's entry point.\r\n\r\n");
		Text += TEXT("| Point | Base class |\r\n|---|---|\r\n");
		for (const FModPublicExtensionPointEntry& Point : Report.ExtensionPoints)
		{
			Text += FString::Printf(TEXT("| `%s` | `U%s` |\r\n"), *Point.ExtensionPointId, *Point.BaseClassName);
		}
		Text += TEXT("\r\n");
	}

	if (Report.Structs.Num() > 0 || Report.Enums.Num() > 0)
	{
		Text += TEXT("## Data types\r\n\r\n");
		for (const FModPublicTypeInfo& Type : Report.Structs)
		{
			const FString DeprecationNote = Type.Metadata.IsDeprecated()
				? FString::Printf(TEXT(" *(deprecated since %s)*"), *Type.Metadata.Deprecated)
				: FString();
			Text += FString::Printf(TEXT("- `F%s`%s\r\n"), *Type.Name, *DeprecationNote);
		}
		for (const FModPublicTypeInfo& Type : Report.Enums)
		{
			Text += FString::Printf(TEXT("- `E%s`\r\n"), *Type.Name);
		}
		Text += TEXT("\r\n");
	}

	Text += TEXT("## Writing your `mod.json`\r\n\r\n");
	Text += TEXT("```json\r\n");
	Text += FString::Printf(TEXT("\"game\":      { \"id\": \"%s\", \"version\": \"^%s\" },\r\n"),
		*InResult.GameId, *InResult.GameVersion);
	Text += FString::Printf(TEXT("\"framework\": { \"version\": \"^%s\" },\r\n"), *InResult.FrameworkVersion);
	Text += FString::Printf(TEXT("\"sdk\":       { \"id\": \"%s\", \"version\": \"^%s\" }\r\n"),
		*InResult.SdkId, *InResult.SdkVersion);
	Text += TEXT("```\r\n\r\n");
	if (InResult.bDocsIncluded)
	{
		Text += TEXT("`Docs/ManifestFormat.md` documents every field. `Docs/Permissions.md` lists what a permission means, ")
			TEXT("`Docs/Packaging.md` covers building a `.mod`, and `Docs/Multiplayer.md` explains what happens when your mod joins a session.\r\n\r\n");
	}

	Text += TEXT("## Compatibility\r\n\r\n");
	Text += TEXT("The SDK version follows semantic versioning. A MINOR bump only adds; a MAJOR bump can remove or change ")
		TEXT("something you depend on. Pin a caret range (`^");
	Text += InResult.SdkVersion;
	Text += TEXT("`) and your mod keeps working across additive updates.\r\n\r\n");

	if (Report.Apis.Num() > 0)
	{
		int32 NonNativeCount = 0;
		for (const FModPublicApiEntry& Api : Report.Apis)
		{
			NonNativeCount += Api.bNativeIdentity ? 0 : 1;
		}
		if (NonNativeCount > 0)
		{
			Text += FString::Printf(
				TEXT("> **Note:** %d of the APIs above resolve their id from editor-only metadata. ")
				TEXT("Check `SDKVersion.json` (`nativeIdentity`) before depending on one in a shipped build.\r\n\r\n"),
				NonNativeCount);
		}
	}

	Text += TEXT("---\r\n\r\n");
	Text += FString::Printf(TEXT("Generated by the Mod Framework SDK generator on %s (UTC).\r\n"),
		*FDateTime::UtcNow().ToString());

	return Text;
}

//////////////////////////////////////////////////////////////////////////
// Generation

bool FModSDKGenerator::GenerateBundle(const FModSDKGenerateOptions& InOptions, TArray<FModDiagnostic>& OutDiagnostics)
{
	FModSDKGenerateResult Result;
	return GenerateBundle(InOptions, Result, OutDiagnostics);
}

bool FModSDKGenerator::GenerateBundle(const FModSDKGenerateOptions& InOptions, FModSDKGenerateResult& OutResult,
	TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModSDKGeneratorPrivate;

	OutResult = FModSDKGenerateResult();

	FModSDKGenerateOptions Options = InOptions;
	if (!ResolveOptions(Options, OutDiagnostics))
	{
		return false;
	}

	const TSharedPtr<IPlugin> FrameworkPlugin = IPluginManager::Get().FindPlugin(FrameworkPluginName);
	const TSharedPtr<IPlugin> SdkPlugin = IPluginManager::Get().FindPlugin(Options.SdkPluginName);
	if (!FrameworkPlugin.IsValid() || !SdkPlugin.IsValid())
	{
		// ResolveOptions already proved both exist; this only guards a plugin unloaded in between.
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::PluginNotFound, TEXT("A required plugin disappeared between validation and generation.")));
		return false;
	}

	//~ Versions and identity -----------------------------------------------------------------------

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();

	OutResult.EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	OutResult.FrameworkVersion = ModFrameworkVersion::GetString();
	OutResult.SdkVersion = Options.SdkVersion;
	OutResult.GameId = Settings ? Settings->GameId : FString();
	OutResult.GameVersion = Settings ? Settings->GameVersion : FString();
	OutResult.SdkId = Settings ? Settings->SdkId : FString();
	OutResult.ManifestVersion = MODFRAMEWORK_MANIFEST_VERSION;
	OutResult.PackageFormatVersion = MODFRAMEWORK_PACKAGE_FORMAT_VERSION;

	if (OutResult.GameId.IsEmpty())
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Warning,
			ModSDKGeneratorCodes::InvalidOptions,
			TEXT("The project declares no GameId, so the generated manifest template and README cannot name the game a mod targets. ")
			TEXT("Set it under Project Settings > Plugins > Mod Framework.")));
	}
	if (OutResult.GameVersion.IsEmpty())
	{
		OutResult.GameVersion = TEXT("1.0.0");
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Warning,
			ModSDKGeneratorCodes::InvalidOptions,
			TEXT("The project declares no GameVersion; the bundle is stamped 1.0.0.")));
	}
	if (OutResult.SdkId.IsEmpty())
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Warning,
			ModSDKGeneratorCodes::InvalidOptions,
			TEXT("The project declares no SdkId, so the starter manifest pins an empty one - which the resolver reads as ")
			TEXT("\"any SDK\" and silently stops checking. Set it under Project Settings > Plugins > Mod Framework.")));
	}

	//~ Scan --------------------------------------------------------------------------------------

	OutResult.ApiReport = FModPublicApiScanner::Scan(Options.ScanOptions);
	OutDiagnostics.Append(OutResult.ApiReport.Diagnostics);

	if (!OutResult.ApiReport.bMetadataAvailable)
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::ScanRejected,
			TEXT("Reflection metadata is not present in this build, so the bundle would carry an empty API index. Run from the editor or an editor commandlet.")));
		return false;
	}

	if (Options.bFailOnScanErrors && OutResult.ApiReport.HasErrors())
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::ScanRejected,
			TEXT("The public API scan reported errors and -FailOnScanErrors was set, so nothing was written.")));
		return false;
	}

	if (OutResult.ApiReport.GetTypeCount() == 0)
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Warning,
			ModSDKGeneratorCodes::ScanRejected,
			TEXT("Nothing in this project is marked ModPublic, so the SDK index is empty. See Docs/PublicAPIMarking.md.")));
	}

	//~ Bundle root -------------------------------------------------------------------------------

	const FString BundleDirectory = NormalizeDirectory(
		Options.OutputDirectory / MakeBundleFolderName(Options.SdkName, Options.SdkVersion));
	OutResult.BundleDirectory = BundleDirectory;

	IFileManager& FileManager = IFileManager::Get();

	if (FileManager.DirectoryExists(*BundleDirectory))
	{
		if (!Options.bOverwriteExisting)
		{
			OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
				ModSDKGeneratorCodes::OutputFailed,
				TEXT("A bundle of this name already exists and overwriting was not requested."), BundleDirectory));
			return false;
		}

		if (!FileManager.DeleteDirectory(*BundleDirectory, /*RequireExists*/ false, /*Tree*/ true))
		{
			OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
				ModSDKGeneratorCodes::OutputFailed,
				TEXT("The previous bundle could not be removed. Close anything holding a file inside it."),
				BundleDirectory));
			return false;
		}
	}

	if (!FileManager.MakeDirectory(*BundleDirectory, /*Tree*/ true))
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::OutputFailed, TEXT("Could not create the bundle directory."), BundleDirectory));
		return false;
	}

	//~ Plugins -----------------------------------------------------------------------------------

	const bool bIncludeBinaries = Options.bIncludeBinaries;
	const bool bIncludeGeneratorSource = Options.bIncludeGeneratorSource;
	auto PluginFilter = [bIncludeBinaries, bIncludeGeneratorSource](const FString& InRelativePath, bool bIsDirectory)
	{
		return ShouldExcludeFromPluginCopy(InRelativePath, bIsDirectory, bIncludeBinaries, bIncludeGeneratorSource);
	};

	const FString FrameworkSource = NormalizeDirectory(FrameworkPlugin->GetBaseDir());
	const FString FrameworkDestination = BundleDirectory / TEXT("Plugins") / FrameworkPlugin->GetName();
	if (!CopyTreeFiltered(FrameworkSource, FrameworkDestination, PluginFilter,
		OutResult.FilesCopied, OutResult.BytesCopied, OutDiagnostics))
	{
		return false;
	}

	const FString SdkSource = NormalizeDirectory(SdkPlugin->GetBaseDir());
	const FString SdkDestination = BundleDirectory / TEXT("Plugins") / SdkPlugin->GetName();
	if (!CopyTreeFiltered(SdkSource, SdkDestination, PluginFilter,
		OutResult.FilesCopied, OutResult.BytesCopied, OutDiagnostics))
	{
		return false;
	}

	//~ Docs --------------------------------------------------------------------------------------

	if (Options.bIncludeDocs)
	{
		if (FileManager.DirectoryExists(*Options.DocsDirectory))
		{
			auto DocsFilter = [](const FString& InRelativePath, bool bIsDirectory)
			{
				return ShouldExcludeFromDocsCopy(InRelativePath, bIsDirectory);
			};

			if (!CopyTreeFiltered(Options.DocsDirectory, BundleDirectory / TEXT("Docs"), DocsFilter,
				OutResult.FilesCopied, OutResult.BytesCopied, OutDiagnostics))
			{
				return false;
			}

			OutResult.bDocsIncluded = true;
		}
		else
		{
			OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Info,
				ModSDKGeneratorCodes::DocsMissing,
				TEXT("No documentation folder was found, so the bundle ships without Docs/. Pass -Docs=<path> to supply one."),
				Options.DocsDirectory));
		}
	}

	//~ Template project ---------------------------------------------------------------------------

	if (Options.bIncludeTemplateProject)
	{
		const FString ModuleName = MakeModuleIdentifier(Options.TemplateProjectName);
		const FString TemplateRoot = BundleDirectory / TEXT("Templates") / Options.TemplateProjectName;

		const FString SdkModuleName = SdkPlugin->GetDescriptor().Modules.Num() > 0
			? SdkPlugin->GetDescriptor().Modules[0].Name.ToString()
			: Options.SdkPluginName;

		const bool bTemplateWritten =
			WriteTextFile(TemplateRoot / (ModuleName + TEXT(".uproject")),
				BuildTemplateUProject(Options, ModuleName), OutResult.FilesGenerated, OutDiagnostics)
			&& WriteTextFile(TemplateRoot / TEXT("Source") / (ModuleName + TEXT(".Target.cs")),
				BuildTemplateTargetCs(ModuleName, /*bEditorTarget*/ false), OutResult.FilesGenerated, OutDiagnostics)
			&& WriteTextFile(TemplateRoot / TEXT("Source") / (ModuleName + TEXT("Editor.Target.cs")),
				BuildTemplateTargetCs(ModuleName, /*bEditorTarget*/ true), OutResult.FilesGenerated, OutDiagnostics)
			&& WriteTextFile(TemplateRoot / TEXT("Source") / ModuleName / (ModuleName + TEXT(".Build.cs")),
				BuildTemplateBuildCs(ModuleName, SdkModuleName), OutResult.FilesGenerated, OutDiagnostics)
			&& WriteTextFile(TemplateRoot / TEXT("Source") / ModuleName / (ModuleName + TEXT(".cpp")),
				BuildTemplateModuleCpp(ModuleName), OutResult.FilesGenerated, OutDiagnostics)
			&& WriteTextFile(TemplateRoot / TEXT("Config") / TEXT("DefaultEngine.ini"),
				BuildTemplateDefaultEngineIni(), OutResult.FilesGenerated, OutDiagnostics)
			&& WriteTextFile(TemplateRoot / TEXT("Config") / TEXT("DefaultGame.ini"),
				BuildTemplateDefaultGameIni(Options.TemplateProjectName, OutResult), OutResult.FilesGenerated, OutDiagnostics)
			&& WriteTextFile(TemplateRoot / TEXT("Mod") / TEXT("mod.json"),
				BuildTemplateModManifest(OutResult), OutResult.FilesGenerated, OutDiagnostics);

		if (!bTemplateWritten)
		{
			return false;
		}

		OutResult.bTemplateIncluded = true;
	}

	//~ Generated files ----------------------------------------------------------------------------

	FString VersionJson;
	if (!BuildVersionJson(Options, OutResult, VersionJson))
	{
		OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Error,
			ModSDKGeneratorCodes::WriteFailed, TEXT("SDKVersion.json could not be serialised.")));
		return false;
	}
	if (!WriteTextFile(BundleDirectory / TEXT("SDKVersion.json"), VersionJson,
		OutResult.FilesGenerated, OutDiagnostics))
	{
		return false;
	}

	if (Options.bWriteApiReport)
	{
		FModDiagnostic ReportError;
		if (!FModPublicApiScanner::WriteReportToFile(OutResult.ApiReport,
			BundleDirectory / TEXT("ApiReport.json"), ReportError))
		{
			// A missing report does not invalidate the bundle; say so and carry on.
			OutDiagnostics.Add(ReportError);
		}
		else
		{
			++OutResult.FilesGenerated;
			OutResult.bApiReportIncluded = true;
		}
	}

	if (!WriteTextFile(BundleDirectory / TEXT("README.md"), BuildReadme(Options, OutResult),
		OutResult.FilesGenerated, OutDiagnostics))
	{
		return false;
	}

	OutResult.bSucceeded = true;

	OutDiagnostics.Add(MakeGeneratorDiagnostic(EModDiagnosticSeverity::Info,
		ModSDKGeneratorCodes::BundleWritten,
		FString::Printf(TEXT("Bundle written: %d file(s) copied (%.2f MiB), %d generated."),
			OutResult.FilesCopied, static_cast<double>(OutResult.BytesCopied) / (1024.0 * 1024.0),
			OutResult.FilesGenerated),
		BundleDirectory));

	UE_LOG(LogModFrameworkDeveloper, Log,
		TEXT("SDK bundle generated at %s (%d copied, %d generated, %s)."),
		*BundleDirectory, OutResult.FilesCopied, OutResult.FilesGenerated, *OutResult.ApiReport.DescribeCounts());

	return true;
}
