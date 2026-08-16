// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "SDK/ModPublicApiScanner.h"
#include "Templates/Function.h"

/** Stable machine-readable code of every diagnostic SDK generation can emit. */
namespace ModSDKGeneratorCodes
{
	/** A required option is missing or nonsensical. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const InvalidOptions;

	/** IPluginManager could not find a plugin the bundle needs. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const PluginNotFound;

	/** A directory could not be created, cleared or written to. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const OutputFailed;

	/** One or more files could not be copied. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const CopyFailed;

	/** A generated file could not be written. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const WriteFailed;

	/** The repo docs folder was requested but does not exist. Informational, not fatal. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const DocsMissing;

	/** The public API scan reported errors and bFailOnScanErrors was set. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ScanRejected;

	/** Informational: the bundle was written. Carries the file and byte counts. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const BundleWritten;
}

/**
 * What to build, and where.
 *
 * Everything that can be derived is derived: call FModSDKGenerator::ResolveOptions (GenerateBundle
 * calls it for you) to fill the blanks from IPluginManager and UModFrameworkSettings.
 */
struct MODFRAMEWORKDEVELOPER_API FModSDKGenerateOptions
{
	/**
	 * Directory the bundle folder is created inside. Not the bundle itself.
	 * Defaults to <Project>/Saved/ModSDK.
	 */
	FString OutputDirectory;

	/** Plugin name of the game's SDK, e.g. "GameModSDK". Located with IPluginManager, never by path. */
	FString SdkPluginName;

	/** Bundle name; the folder is "<SdkName>-<SdkVersion>". Defaults to SdkPluginName. */
	FString SdkName;

	/**
	 * Version stamped on the bundle. When empty it comes from UModFrameworkSettings::SdkVersion, and
	 * failing that from the SDK plugin descriptor's VersionName.
	 */
	FString SdkVersion;

	/**
	 * Repository documentation folder to copy into Docs/. When empty it defaults to the folder named
	 * "docs" beside the ModFramework plugin, which is where this repository keeps it. `docs/internal`
	 * is ALWAYS excluded - it holds the implementation contract and the internal status document.
	 */
	FString DocsDirectory;

	/** Folder name of the generated mod project under Templates/. */
	FString TemplateProjectName = TEXT("ModProject");

	/** Engine association written into the generated .uproject, e.g. "5.8". */
	FString EngineAssociation;

	/** Copy the docs folder. */
	bool bIncludeDocs = true;

	/** Emit Templates/<TemplateProjectName>/. */
	bool bIncludeTemplateProject = true;

	/** Emit ApiReport.json next to SDKVersion.json - the whole scan, diagnostics included. */
	bool bWriteApiReport = true;

	/**
	 * Ship each plugin's compiled Binaries/ alongside its source.
	 *
	 * Off by default: binaries are per-platform, per-configuration and stale the moment the engine
	 * patch version moves, and a source bundle builds anywhere. Turn it on when publishing to mod
	 * authors who should never have to compile - and then the generated template project's C++
	 * scaffolding becomes optional for them.
	 */
	bool bIncludeBinaries = false;

	/**
	 * Ship the SDK generator's own source - Source/ModFrameworkDeveloper/{Public,Private}/SDK.
	 *
	 * Off by default: a mod author has no use for the tool that produced their SDK. It is excluded
	 * as a matched PAIR, never one half. UGenerateModSDKCommandlet is a UCLASS, so UHT emits
	 * reflection code for any header it finds and that generated code references the constructor in
	 * the .cpp - dropping only Private/SDK would leave the shipped plugin with an unresolved
	 * external and no way for the mod author to build it.
	 *
	 * Turn it on if anything else in the shipped bundle ever references FModSDKGenerator or
	 * FModPublicApiScanner - a future editor tool in ModFrameworkEditor, for instance.
	 */
	bool bIncludeGeneratorSource = false;

	/** Delete an existing bundle folder of the same name first. Off leaves an existing bundle alone. */
	bool bOverwriteExisting = true;

	/**
	 * Refuse to write a bundle when the API scan reported errors.
	 *
	 * Off by default. An SDK with a leaked type is broken, but the author needs the bundle and the
	 * report side by side to see what to fix, and the diagnostics are logged as errors either way.
	 */
	bool bFailOnScanErrors = false;

	/** Passed straight to FModPublicApiScanner. ShippingPluginNames gains SdkPluginName. */
	FModPublicApiScanOptions ScanOptions;
};

/** What one generation produced. */
struct MODFRAMEWORKDEVELOPER_API FModSDKGenerateResult
{
	bool bSucceeded = false;

	/** Absolute path of the bundle folder, i.e. <Output>/<SdkName>-<SdkVersion>. */
	FString BundleDirectory;

	int32 FilesCopied = 0;
	int64 BytesCopied = 0;

	/** Files this generator authored rather than copied: the .uproject, README.md, SDKVersion.json... */
	int32 FilesGenerated = 0;

	//~ What the bundle actually contains ----------------------------------------------------------
	// Requesting a part and getting it are different things - a docs folder that does not exist is
	// skipped with an Info diagnostic rather than failing the run - and the generated README has to
	// describe the bundle that exists, not the one that was asked for.

	bool bDocsIncluded = false;
	bool bTemplateIncluded = false;
	bool bApiReportIncluded = false;

	/** The scan that produced the API index. Carried out so callers can render it. */
	FModPublicApiReport ApiReport;

	//~ The versions stamped into SDKVersion.json ---------------------------------------------------
	FString EngineVersion;
	FString GameId;
	FString GameVersion;
	FString FrameworkVersion;
	FString SdkId;
	FString SdkVersion;
	int32 ManifestVersion = 0;
	int32 PackageFormatVersion = 0;
};

/**
 * Assembles a publishable SDK bundle: the plugins a mod author installs, the documentation they
 * read, a project template they start from, and a machine-readable index of the game's public
 * modding surface.
 *
 * ---------------------------------------------------------------------------------------------
 * BUNDLE LAYOUT
 * ---------------------------------------------------------------------------------------------
 *   <Output>/<SdkName>-<SdkVersion>/
 *     Plugins/ModFramework/          the framework, source and content
 *     Plugins/<SdkPluginName>/       the game's SDK, source and content
 *     Templates/<TemplateProject>/   a project enabling ONLY the SDK plugin
 *     Docs/                          the repository's public documentation
 *     SDKVersion.json                every version the mod author has to pin, plus the API index
 *     ApiReport.json                 the full scan, diagnostics included (optional)
 *     README.md                      generated, mod-author facing
 *
 * The template's .uproject enables exactly one plugin. The framework arrives because the SDK's own
 * descriptor lists it as a dependency, which is the arrangement Templates/ModAuthorSample already
 * proves works: a mod author never installs, chooses, or thinks about the framework. If the template
 * ever needs ModFramework spelled out, the SDK's descriptor is wrong.
 *
 * ---------------------------------------------------------------------------------------------
 * WHAT NEVER SHIPS
 * ---------------------------------------------------------------------------------------------
 * Build output (Binaries, Intermediate, Saved, DerivedDataCache), anything dot-prefixed (.dev, .git,
 * .vs, .vscode), and `docs/internal`, which holds the implementation contract and internal status
 * documents. The generator's own source under
 * Source/ModFrameworkDeveloper/Private/SDK is dropped too - shipping it is harmless, but a mod
 * author has no use for the tool that produced their SDK.
 *
 * Nothing here is required at runtime by a mod: this whole class lives in an UncookedOnly module.
 */
class MODFRAMEWORKDEVELOPER_API FModSDKGenerator
{
public:
	/** Builds the bundle. Diagnostics describe every step; the return value is the summary. */
	static bool GenerateBundle(const FModSDKGenerateOptions& InOptions, TArray<FModDiagnostic>& OutDiagnostics);

	/** As above, with the full result. */
	static bool GenerateBundle(const FModSDKGenerateOptions& InOptions, FModSDKGenerateResult& OutResult,
		TArray<FModDiagnostic>& OutDiagnostics);

	/**
	 * Fills every blank option from IPluginManager, UModFrameworkSettings and the engine version.
	 * Returns false when something required could not be derived; OutDiagnostics says what.
	 */
	static bool ResolveOptions(FModSDKGenerateOptions& InOutOptions, TArray<FModDiagnostic>& OutDiagnostics);

	/**
	 * True when a path relative to a plugin's root is build output, editor state, or the SDK
	 * generator's own source. Directory-aware: pass bIsDirectory for a directory entry.
	 */
	static bool ShouldExcludeFromPluginCopy(const FString& InRelativePath, bool bIsDirectory,
		bool bIncludeBinaries, bool bIncludeGeneratorSource);

	/** True when a path relative to the docs root must not be published. `internal` and dotfiles. */
	static bool ShouldExcludeFromDocsCopy(const FString& InRelativePath, bool bIsDirectory);

	/**
	 * Copies a directory tree, asking InShouldExclude about every entry before descending into it.
	 *
	 * The predicate receives the path relative to InSourceDirectory (forward slashes, no leading
	 * slash) and whether the entry is a directory. Excluded directories are never walked, so a
	 * plugin's Intermediate folder costs nothing.
	 */
	static bool CopyTreeFiltered(const FString& InSourceDirectory, const FString& InDestinationDirectory,
		TFunctionRef<bool(const FString& /*RelativePath*/, bool /*bIsDirectory*/)> InShouldExclude,
		int32& OutFilesCopied, int64& OutBytesCopied, TArray<FModDiagnostic>& OutDiagnostics);

	/** <Project>/Saved/ModSDK, absolute. */
	static FString GetDefaultOutputDirectory();

	/** The bundle folder name for a name and version, e.g. "GameModSDK-1.2.0". */
	static FString MakeBundleFolderName(const FString& InSdkName, const FString& InSdkVersion);

	/** The mod-author facing README the bundle ships. Exposed so tooling can preview it. */
	static FString BuildReadme(const FModSDKGenerateOptions& InOptions, const FModSDKGenerateResult& InResult);

	/** The SDKVersion.json body. Exposed for the same reason. */
	static bool BuildVersionJson(const FModSDKGenerateOptions& InOptions, const FModSDKGenerateResult& InResult,
		FString& OutJson);
};
