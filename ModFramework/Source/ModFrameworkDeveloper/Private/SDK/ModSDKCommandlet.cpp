// Copyright (c) 2026. Licensed for use in your own projects.

#include "SDK/ModSDKCommandlet.h"

#include "Containers/UnrealString.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ModFrameworkDeveloperModule.h"
#include "PluginDescriptor.h"
#include "PluginReferenceDescriptor.h"
#include "SDK/ModPublicApiScanner.h"
#include "SDK/ModSDKGenerator.h"
#include "Templates/SharedPointer.h"

namespace ModSDKCommandletPrivate
{
	const TCHAR* const CodeBadArgument = TEXT("Sdk.BadArgument");

	/**
	 * Deliberately not named MakeError. Templates/ValueOrError.h declares a global variadic
	 * MakeError(ArgTypes&&...) that wins overload resolution at any call site reached through a
	 * using-directive and yields an unreadable TValueOrError_ErrorProxy diagnostic.
	 */
	FModDiagnostic MakeCommandletDiagnostic(EModDiagnosticSeverity InSeverity, const TCHAR* InCode,
		FString InMessage, FString InContext = FString())
	{
		return FModDiagnostic(InSeverity, FName(InCode), MoveTemp(InMessage), MoveTemp(InContext));
	}

	/** Writes every diagnostic to LogModFrameworkDeveloper at the verbosity matching its severity. */
	void LogDiagnostics(const TArray<FModDiagnostic>& InDiagnostics, bool bQuiet)
	{
		for (const FModDiagnostic& Diagnostic : InDiagnostics)
		{
			switch (Diagnostic.Severity)
			{
			case EModDiagnosticSeverity::Error:
				UE_LOG(LogModFrameworkDeveloper, Error, TEXT("%s"), *Diagnostic.ToString());
				break;

			case EModDiagnosticSeverity::Warning:
				UE_LOG(LogModFrameworkDeveloper, Warning, TEXT("%s"), *Diagnostic.ToString());
				break;

			default:
				if (!bQuiet)
				{
					UE_LOG(LogModFrameworkDeveloper, Display, TEXT("%s"), *Diagnostic.ToString());
				}
				break;
			}
		}
	}

	/** Reads a -Key=Value switch, rejecting an empty value rather than silently accepting it. */
	bool ParseStringSwitch(const TCHAR* InCommandLine, const TCHAR* InKey, FString& OutValue,
		TArray<FModDiagnostic>& OutDiagnostics)
	{
		FString Value;
		if (!FParse::Value(InCommandLine, InKey, Value))
		{
			return false;
		}

		Value.TrimStartAndEndInline();
		Value.TrimQuotesInline();
		Value.TrimStartAndEndInline();

		if (Value.IsEmpty())
		{
			OutDiagnostics.Add(MakeCommandletDiagnostic(EModDiagnosticSeverity::Warning, CodeBadArgument,
				FString::Printf(TEXT("-%s was given no value and is ignored."), InKey)));
			return false;
		}

		OutValue = MoveTemp(Value);
		return true;
	}

	/** Turns a possibly relative directory into an absolute one, based on the project directory. */
	FString ResolveDirectoryArgument(const FString& InValue)
	{
		if (FPaths::IsRelative(InValue))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), InValue);
		}
		return FPaths::ConvertRelativePathToFull(InValue);
	}
}

UGenerateModSDKCommandlet::UGenerateModSDKCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = true;
	UseCommandletResultAsExitCode = true;

	HelpDescription = TEXT("Generates a publishable mod SDK bundle from this project's ModPublic reflection data.");
	HelpUsage = TEXT("-run=GenerateModSDK -SdkPlugin=<PluginName> [-Output=<Directory>] [-SdkVersion=<Semver>]");

	HelpParamNames.Add(TEXT("SdkPlugin"));
	HelpParamDescriptions.Add(TEXT("Plugin holding the game's public modding surface. Derived when omitted."));

	HelpParamNames.Add(TEXT("Output"));
	HelpParamDescriptions.Add(TEXT("Directory the bundle folder is created in. Default <Project>/Saved/ModSDK."));

	HelpParamNames.Add(TEXT("SdkVersion"));
	HelpParamDescriptions.Add(TEXT("Version stamped on the bundle. Default: project settings, then the plugin descriptor."));

	HelpParamNames.Add(TEXT("ScanOnly"));
	HelpParamDescriptions.Add(TEXT("Scan and report without writing a bundle."));

	HelpParamNames.Add(TEXT("FailOnScanErrors"));
	HelpParamDescriptions.Add(TEXT("Exit non-zero when the public API scan reports errors."));
}

FString UGenerateModSDKCommandlet::GetUsageText()
{
	return
		TEXT("GenerateModSDK - builds a publishable mod SDK bundle from this project's reflection data.\n")
		TEXT("\n")
		TEXT("  UnrealEditor-Cmd.exe <Project>.uproject -run=GenerateModSDK [switches]\n")
		TEXT("\n")
		TEXT("  -SdkPlugin=<name>     Plugin holding the game's public modding surface.\n")
		TEXT("                        Omit it and the single enabled plugin depending on ModFramework is used.\n")
		TEXT("  -Output=<directory>   Where the bundle folder is created. Default <Project>/Saved/ModSDK.\n")
		TEXT("  -SdkName=<name>       Bundle name. Default: the SDK plugin's name.\n")
		TEXT("  -SdkVersion=<semver>  Version stamped on the bundle.\n")
		TEXT("  -Docs=<directory>     Documentation to publish. docs/internal is always excluded.\n")
		TEXT("  -Template=<name>      Folder name of the generated mod project. Default ModProject.\n")
		TEXT("  -Engine=<x.y>         EngineAssociation for the generated .uproject.\n")
		TEXT("  -ReportPath=<file>    Also write the full scan report here, outside the bundle.\n")
		TEXT("  -NoDocs               Do not publish documentation.\n")
		TEXT("  -NoTemplate           Do not generate the mod project template.\n")
		TEXT("  -NoApiReport          Do not write ApiReport.json into the bundle.\n")
		TEXT("  -IncludeBinaries      Ship compiled Binaries/ as well as source.\n")
		TEXT("  -IncludeGeneratorSource  Ship the SDK generator's own source inside the bundle.\n")
		TEXT("  -NoOverwrite          Fail rather than replace an existing bundle.\n")
		TEXT("  -FailOnScanErrors     Refuse to write a bundle when the scan reports errors.\n")
		TEXT("  -ScanOnly             Scan and report, write nothing.\n")
		TEXT("  -Quiet                Log only warnings and errors.\n")
		TEXT("  -Help                 Print this text.\n")
		TEXT("\n")
		TEXT("PowerShell splits an unquoted -SdkVersion=1.2.3 into \"-SdkVersion=1\" and \".2.3\", so the\n")
		TEXT("bundle would be stamped \"1\". Quote any value starting with a digit: -SdkVersion=\"1.2.3\".\n");
}

FString UGenerateModSDKCommandlet::FindLikelySdkPluginName(TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModSDKCommandletPrivate;

	static const FString FrameworkPluginName(TEXT("ModFramework"));

	const TArray<TSharedRef<IPlugin>> EnabledPlugins = IPluginManager::Get().GetEnabledPlugins();

	TArray<FString> Candidates;
	for (const TSharedRef<IPlugin>& Plugin : EnabledPlugins)
	{
		if (Plugin->GetName() == FrameworkPluginName)
		{
			continue;
		}

		for (const FPluginReferenceDescriptor& Reference : Plugin->GetDescriptor().Plugins)
		{
			if (Reference.bEnabled && Reference.Name == FrameworkPluginName)
			{
				Candidates.AddUnique(Plugin->GetName());
				break;
			}
		}
	}

	if (Candidates.Num() == 1)
	{
		OutDiagnostics.Add(MakeCommandletDiagnostic(EModDiagnosticSeverity::Info, CodeBadArgument,
			FString::Printf(TEXT("-SdkPlugin was not supplied; using \"%s\", the only enabled plugin that depends on ModFramework."),
				*Candidates[0])));
		return Candidates[0];
	}

	if (Candidates.Num() == 0)
	{
		OutDiagnostics.Add(MakeCommandletDiagnostic(EModDiagnosticSeverity::Error, CodeBadArgument,
			TEXT("-SdkPlugin was not supplied and no enabled plugin depends on ModFramework. Pass -SdkPlugin=<PluginName>.")));
		return FString();
	}

	Candidates.Sort();
	OutDiagnostics.Add(MakeCommandletDiagnostic(EModDiagnosticSeverity::Error, CodeBadArgument,
		FString::Printf(TEXT("-SdkPlugin was not supplied and %d plugins depend on ModFramework (%s). Name the one to publish."),
			Candidates.Num(), *FString::Join(Candidates, TEXT(", ")))));
	return FString();
}

bool UGenerateModSDKCommandlet::ParseOptions(const FString& InParams, FModSDKGenerateOptions& OutOptions,
	bool& bOutScanOnly, FString& OutReportPath, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModSDKCommandletPrivate;

	const TCHAR* CommandLine = *InParams;

	bOutScanOnly = FParse::Param(CommandLine, TEXT("ScanOnly"));

	FString Value;

	if (ParseStringSwitch(CommandLine, TEXT("SdkPlugin="), Value, OutDiagnostics))
	{
		OutOptions.SdkPluginName = Value;
	}
	else
	{
		OutOptions.SdkPluginName = FindLikelySdkPluginName(OutDiagnostics);
		if (OutOptions.SdkPluginName.IsEmpty())
		{
			return false;
		}
	}

	if (ParseStringSwitch(CommandLine, TEXT("Output="), Value, OutDiagnostics))
	{
		OutOptions.OutputDirectory = ResolveDirectoryArgument(Value);
	}

	if (ParseStringSwitch(CommandLine, TEXT("SdkName="), Value, OutDiagnostics))
	{
		OutOptions.SdkName = Value;
	}

	if (ParseStringSwitch(CommandLine, TEXT("SdkVersion="), Value, OutDiagnostics))
	{
		OutOptions.SdkVersion = Value;
	}

	if (ParseStringSwitch(CommandLine, TEXT("Docs="), Value, OutDiagnostics))
	{
		OutOptions.DocsDirectory = ResolveDirectoryArgument(Value);
	}

	if (ParseStringSwitch(CommandLine, TEXT("Template="), Value, OutDiagnostics))
	{
		// The name becomes a folder and a C++ module identifier, so path separators cannot survive.
		if (Value.Contains(TEXT("/")) || Value.Contains(TEXT("\\")) || Value.Contains(TEXT("..")))
		{
			OutDiagnostics.Add(MakeCommandletDiagnostic(EModDiagnosticSeverity::Error, CodeBadArgument,
				TEXT("-Template must be a single folder name, not a path."), Value));
			return false;
		}
		OutOptions.TemplateProjectName = Value;
	}

	if (ParseStringSwitch(CommandLine, TEXT("Engine="), Value, OutDiagnostics))
	{
		OutOptions.EngineAssociation = Value;
	}

	if (ParseStringSwitch(CommandLine, TEXT("ReportPath="), Value, OutDiagnostics))
	{
		OutReportPath = FPaths::IsRelative(Value)
			? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Value)
			: FPaths::ConvertRelativePathToFull(Value);
	}

	OutOptions.bIncludeDocs = !FParse::Param(CommandLine, TEXT("NoDocs"));
	OutOptions.bIncludeTemplateProject = !FParse::Param(CommandLine, TEXT("NoTemplate"));
	OutOptions.bWriteApiReport = !FParse::Param(CommandLine, TEXT("NoApiReport"));
	OutOptions.bIncludeBinaries = FParse::Param(CommandLine, TEXT("IncludeBinaries"));
	OutOptions.bIncludeGeneratorSource = FParse::Param(CommandLine, TEXT("IncludeGeneratorSource"));
	OutOptions.bOverwriteExisting = !FParse::Param(CommandLine, TEXT("NoOverwrite"));
	OutOptions.bFailOnScanErrors = FParse::Param(CommandLine, TEXT("FailOnScanErrors"));

	return true;
}

int32 UGenerateModSDKCommandlet::Main(const FString& Params)
{
	using namespace ModSDKCommandletPrivate;

	const bool bQuiet = FParse::Param(*Params, TEXT("Quiet"));

	if (FParse::Param(*Params, TEXT("Help")) || FParse::Param(*Params, TEXT("?")))
	{
		UE_LOG(LogModFrameworkDeveloper, Display, TEXT("%s"), *GetUsageText());
		return 0;
	}

	TArray<FModDiagnostic> Diagnostics;

	FModSDKGenerateOptions Options;
	bool bScanOnly = false;
	FString ReportPath;

	if (!ParseOptions(Params, Options, bScanOnly, ReportPath, Diagnostics))
	{
		LogDiagnostics(Diagnostics, bQuiet);
		UE_LOG(LogModFrameworkDeveloper, Display, TEXT("%s"), *GetUsageText());
		return 1;
	}

	if (bScanOnly)
	{
		// Still resolve, because ResolveOptions is what tells the scanner which plugins ship - and
		// therefore which unmarked types count as a leak. Without it a scan-only run would report
		// the SDK plugin's own internals as leaks.
		if (!FModSDKGenerator::ResolveOptions(Options, Diagnostics))
		{
			LogDiagnostics(Diagnostics, bQuiet);
			return 1;
		}

		const FModPublicApiReport Report = FModPublicApiScanner::Scan(Options.ScanOptions);
		Diagnostics.Append(Report.Diagnostics);
		LogDiagnostics(Diagnostics, bQuiet);

		UE_LOG(LogModFrameworkDeveloper, Display, TEXT("Public API scan: %s."), *Report.DescribeCounts());

		if (!ReportPath.IsEmpty())
		{
			FModDiagnostic ReportError;
			if (!FModPublicApiScanner::WriteReportToFile(Report, ReportPath, ReportError))
			{
				UE_LOG(LogModFrameworkDeveloper, Error, TEXT("%s"), *ReportError.ToString());
				return 1;
			}
			UE_LOG(LogModFrameworkDeveloper, Display, TEXT("Scan report written to %s."), *ReportPath);
		}

		if (!Report.bMetadataAvailable)
		{
			return 1;
		}

		return (Options.bFailOnScanErrors && Report.HasErrors()) ? 1 : 0;
	}

	FModSDKGenerateResult Result;
	const bool bGenerated = FModSDKGenerator::GenerateBundle(Options, Result, Diagnostics);

	LogDiagnostics(Diagnostics, bQuiet);

	if (!ReportPath.IsEmpty())
	{
		FModDiagnostic ReportError;
		if (!FModPublicApiScanner::WriteReportToFile(Result.ApiReport, ReportPath, ReportError))
		{
			// The bundle, if it was written, is still valid; a missing side report is not fatal.
			UE_LOG(LogModFrameworkDeveloper, Warning, TEXT("%s"), *ReportError.ToString());
		}
		else
		{
			UE_LOG(LogModFrameworkDeveloper, Display, TEXT("Scan report written to %s."), *ReportPath);
		}
	}

	if (!bGenerated)
	{
		UE_LOG(LogModFrameworkDeveloper, Error, TEXT("SDK generation failed. Nothing publishable was produced."));
		return 1;
	}

	UE_LOG(LogModFrameworkDeveloper, Display,
		TEXT("SDK bundle ready: %s"), *Result.BundleDirectory);
	UE_LOG(LogModFrameworkDeveloper, Display,
		TEXT("  engine %s | game %s %s | framework %s | sdk %s %s | manifest v%d | package v%d"),
		*Result.EngineVersion, *Result.GameId, *Result.GameVersion, *Result.FrameworkVersion,
		*Result.SdkId, *Result.SdkVersion, Result.ManifestVersion, Result.PackageFormatVersion);
	UE_LOG(LogModFrameworkDeveloper, Display,
		TEXT("  %s"), *Result.ApiReport.DescribeCounts());
	UE_LOG(LogModFrameworkDeveloper, Display,
		TEXT("  %d file(s) copied, %d generated."), Result.FilesCopied, Result.FilesGenerated);

	if (Result.ApiReport.HasErrors())
	{
		UE_LOG(LogModFrameworkDeveloper, Warning,
			TEXT("The bundle was written, but the API scan reported errors. Read ApiReport.json before publishing: ")
			TEXT("a leaked unmarked type means this SDK will not compile in a mod author's project."));
	}

	return 0;
}
