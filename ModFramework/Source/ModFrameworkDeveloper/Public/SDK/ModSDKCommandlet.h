// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Commandlets/Commandlet.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"

#include "ModSDKCommandlet.generated.h"

struct FModSDKGenerateOptions;

/**
 * `-run=GenerateModSDK` - builds a publishable SDK bundle from the project's reflection data.
 *
 * Typical use, from the directory holding the engine's UnrealEditor-Cmd executable:
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=GenerateModSDK ^
 *       -SdkPlugin=GameModSDK -Output="D:/Releases" -SdkVersion="1.0.0"
 *
 * Switches:
 *   -SdkPlugin=<name>      Plugin holding the game's public modding surface. When omitted, the
 *                          commandlet uses the single enabled plugin that depends on ModFramework,
 *                          and fails if that is ambiguous.
 *   -Output=<directory>    Where the bundle folder is created. Default <Project>/Saved/ModSDK.
 *   -SdkName=<name>        Bundle name. Default: the SDK plugin's name.
 *   -SdkVersion=<semver>   Version stamped on the bundle. Default: project settings, then the SDK
 *                          plugin descriptor's VersionName.
 *   -Docs=<directory>      Documentation folder to publish. Default: the "docs" folder beside the
 *                          ModFramework plugin. `docs/internal` is always excluded.
 *   -Template=<name>       Folder name of the generated mod project. Default "ModProject".
 *   -Engine=<x.y>          EngineAssociation for the generated .uproject. Default: this engine.
 *   -ReportPath=<file>     Also write the full scan report here, outside the bundle.
 *   -NoDocs                Do not publish documentation.
 *   -NoTemplate            Do not generate the mod project template.
 *   -NoApiReport           Do not write ApiReport.json into the bundle.
 *   -IncludeBinaries       Ship each plugin's compiled Binaries/ as well as its source.
 *   -IncludeGeneratorSource Ship the SDK generator's own source inside the bundle. Needed only when
 *                          something else in the bundle references FModSDKGenerator.
 *   -NoOverwrite           Fail rather than replace an existing bundle of the same name.
 *   -FailOnScanErrors      Refuse to write a bundle when the API scan reports errors.
 *   -ScanOnly              Scan and report, write no bundle. Use it in CI to police the surface.
 *   -Quiet                 Log only warnings and errors from the scan.
 *   -Help                  Print the usage block and exit.
 *
 * Exit code is 0 on success and 1 on failure, so a build script can gate a release on it. With
 * -FailOnScanErrors, an SDK that would not compile for a mod author fails the build instead.
 *
 * ---------------------------------------------------------------------------------------------
 * POWERSHELL: QUOTE ANY VALUE THAT STARTS WITH A DIGIT
 * ---------------------------------------------------------------------------------------------
 * PowerShell splits an unquoted argument like `-SdkVersion=1.2.3` into two arguments,
 * `-SdkVersion=1` and `.2.3`, because it reads the leading digit as a number and the dot as the
 * start of a new token. The commandlet then stamps the bundle "1" and nothing looks wrong until a
 * mod author's version range fails to match. Write `-SdkVersion="1.2.3"` (or prefix the whole
 * command with PowerShell's `--%` stop-parsing token). cmd.exe and bash are unaffected, and a path
 * value is unaffected because it does not start with a digit.
 *
 * The commandlet warns when a supplied SDK version is not valid semver, which catches this - a bare
 * "1" parses as 1.0.0 under partial parsing, so the warning fires only on the messier cases. Quote
 * the value and the question does not arise.
 *
 * This is editor-time tooling: it reads UCLASS metadata, which only exists in builds that have
 * editor-only data. Running it from a cooked target reports that and exits non-zero rather than
 * quietly producing an empty SDK.
 */
UCLASS()
class MODFRAMEWORKDEVELOPER_API UGenerateModSDKCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGenerateModSDKCommandlet(const FObjectInitializer& ObjectInitializer);

	//~ Begin UCommandlet interface
	virtual int32 Main(const FString& Params) override;
	//~ End UCommandlet interface

	/** The usage block printed by -Help and on a bad argument. */
	static FString GetUsageText();

	/**
	 * Turns a command line into options. Untrusted input: every value is validated and a bad one
	 * produces a diagnostic rather than an assert.
	 *
	 * Returns false when the command line is unusable. Note that a true return still leaves blanks
	 * for FModSDKGenerator::ResolveOptions to fill.
	 */
	static bool ParseOptions(const FString& InParams, FModSDKGenerateOptions& OutOptions,
		bool& bOutScanOnly, FString& OutReportPath, TArray<FModDiagnostic>& OutDiagnostics);

	/**
	 * The one enabled plugin that declares ModFramework as a dependency, ignoring ModFramework
	 * itself. Empty when there is none or when several qualify - guessing between two candidate
	 * SDKs would publish the wrong one under the right name.
	 */
	static FString FindLikelySdkPluginName(TArray<FModDiagnostic>& OutDiagnostics);
};
