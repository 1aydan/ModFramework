// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModSDKGeneratePanel.h"

#include "ModFrameworkEditorModule.h"
#include "Widgets/ModSDKWidgetUtils.h"
#include "Widgets/SModDiagnosticList.h"

#include "Core/ModFrameworkTypes.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IDesktopPlatform.h"
#include "Internationalization/Internationalization.h"
#include "Layout/Margin.h"
#include "Logging/LogMacros.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "SDK/ModPublicApiScanner.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"
#include "Textures/SlateIcon.h"
#include "Types/SlateEnums.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SModSDKGeneratePanel"

namespace ModSDKGeneratePanelPrivate
{
	/** Width reserved for the form labels so every field lines up. */
	constexpr float LabelWidth = 150.0f;
}

void SModSDKGeneratePanel::Construct(const FArguments& InArgs)
{
	OnApiReportGenerated = InArgs._OnApiReportGenerated;

	// The list has to exist before ResolveDefaults runs: deriving the defaults is itself a call into
	// FModSDKGenerator::ResolveOptions, and what it says about a project with no SDK plugin - or an
	// unparseable SDK version - is exactly the first thing worth showing.
	SAssignNew(DiagnosticList, SModDiagnosticList)
		.EmptyMessage(LOCTEXT("NoGenerationYet",
			"No bundle generated yet. Diagnostics from the run - including every warning the public API scan produced - appear here."))
		.MaxHeight(300.0f);

	ResolveDefaults();

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(12.0f)
		[
			SNew(SVerticalBox)

			//~ Header ------------------------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				ModSDKWidgetUtils::MakeSectionHeader(
					LOCTEXT("GenerateTitle", "Generate SDK bundle"),
					LOCTEXT("GenerateSubtitle",
						"Assembles the plugins, documentation, project template and API index a mod author installs. "
						"This is the same work the GenerateModSDK commandlet does, with the same defaults."))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f)
			[
				SNew(SSeparator)
			]

			//~ Output directory --------------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeLabelledRow(
					LOCTEXT("OutputDirLabel", "Output directory"),
					LOCTEXT("OutputDirTooltip",
						"The folder the bundle folder is created inside - not the bundle itself. Defaults to <Project>/Saved/ModSDK."),
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						// Bound rather than assigned: Browse and Reset write the member, and the box
						// picks the change up on its own instead of needing a second SetText path.
						SNew(SEditableTextBox)
						.Text(this, &SModSDKGeneratePanel::GetOutputDirectoryText)
						.OnTextCommitted(this, &SModSDKGeneratePanel::HandleOutputDirectoryCommitted)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ToolTipText(LOCTEXT("BrowseTooltip", "Choose the output directory."))
						.OnClicked(this, &SModSDKGeneratePanel::HandleBrowseClicked)
						[
							SNew(STextBlock).Text(LOCTEXT("BrowseLabel", "Browse..."))
						]
					])
			]

			//~ SDK plugin --------------------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeLabelledRow(
					LOCTEXT("SdkPluginLabel", "SDK plugin"),
					LOCTEXT("SdkPluginTooltip",
						"The plugin holding the game's public modding surface. Located through IPluginManager, never by path, "
						"so it has to be discovered - it does not have to be enabled."),
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(SEditableTextBox)
						.Text(this, &SModSDKGeneratePanel::GetSdkPluginNameText)
						.OnTextCommitted(this, &SModSDKGeneratePanel::HandleSdkPluginNameCommitted)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SComboButton)
						.ToolTipText(LOCTEXT("SdkPluginPickTooltip", "Pick from the plugins this project has discovered."))
						.OnGetMenuContent(this, &SModSDKGeneratePanel::BuildSdkPluginMenu)
						.ButtonContent()
						[
							SNew(STextBlock).Text(LOCTEXT("SdkPluginPickLabel", "Discovered"))
						]
					])
			]

			//~ Bundle name and version -------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeLabelledRow(
					LOCTEXT("SdkNameLabel", "SDK name"),
					LOCTEXT("SdkNameTooltip", "Bundle name. The folder is \"<name>-<version>\". Defaults to the SDK plugin name."),
					SNew(SEditableTextBox)
					.Text(this, &SModSDKGeneratePanel::GetSdkNameText)
					.OnTextCommitted(this, &SModSDKGeneratePanel::HandleSdkNameCommitted))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeLabelledRow(
					LOCTEXT("SdkVersionLabel", "SDK version"),
					LOCTEXT("SdkVersionTooltip",
						"Stamped into SDKVersion.json and into the bundle folder name. Every published mod pins this with a range "
						"expression, so it has to be semver. Empty means: take it from Project Settings, then from the plugin descriptor."),
					SNew(SEditableTextBox)
					.Text(this, &SModSDKGeneratePanel::GetSdkVersionText)
					.OnTextCommitted(this, &SModSDKGeneratePanel::HandleSdkVersionCommitted))
			]

			//~ Bundle preview ----------------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(ModSDKGeneratePanelPrivate::LabelWidth + 8.0f, 2.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FStyleColors::Foreground)
				.Text(this, &SModSDKGeneratePanel::GetBundlePreviewText)
			]

			//~ Contents ----------------------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 4.0f)
			[
				ModSDKWidgetUtils::MakeSectionHeader(LOCTEXT("ContentsTitle", "Bundle contents"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				+ SWrapBox::Slot()
				.Padding(0.0f, 2.0f, 16.0f, 2.0f)
				[
					MakeOptionCheckBox(
						LOCTEXT("IncludeDocsLabel", "Documentation"),
						LOCTEXT("IncludeDocsTooltip", "Copy the repository's public docs into Docs/. docs/internal is always excluded."),
						&bIncludeDocs)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 2.0f, 16.0f, 2.0f)
				[
					MakeOptionCheckBox(
						LOCTEXT("IncludeTemplateLabel", "Project template"),
						LOCTEXT("IncludeTemplateTooltip", "Emit Templates/<name>/, a project that enables only the SDK plugin."),
						&bIncludeTemplateProject)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 2.0f, 16.0f, 2.0f)
				[
					MakeOptionCheckBox(
						LOCTEXT("WriteApiReportLabel", "API report"),
						LOCTEXT("WriteApiReportTooltip", "Emit ApiReport.json - the whole public API scan, diagnostics included."),
						&bWriteApiReport)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 2.0f, 16.0f, 2.0f)
				[
					MakeOptionCheckBox(
						LOCTEXT("IncludeBinariesLabel", "Compiled binaries"),
						LOCTEXT("IncludeBinariesTooltip",
							"Ship each plugin's Binaries/ alongside its source. Off by default: binaries are per-platform, "
							"per-configuration and stale the moment the engine patch version moves, and a source bundle builds anywhere."),
						&bIncludeBinaries)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 2.0f, 16.0f, 2.0f)
				[
					MakeOptionCheckBox(
						LOCTEXT("IncludeGeneratorLabel", "SDK generator source"),
						LOCTEXT("IncludeGeneratorTooltip",
							"Ship the tool that produced this bundle. Off by default - a mod author has no use for it. "
							"Turn it on only if something else in the bundle references FModSDKGenerator or FModPublicApiScanner."),
						&bIncludeGeneratorSource)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 2.0f, 16.0f, 2.0f)
				[
					MakeOptionCheckBox(
						LOCTEXT("OverwriteLabel", "Overwrite existing"),
						LOCTEXT("OverwriteTooltip", "Delete an existing bundle folder of the same name first."),
						&bOverwriteExisting)
				]
				+ SWrapBox::Slot()
				.Padding(0.0f, 2.0f, 16.0f, 2.0f)
				[
					MakeOptionCheckBox(
						LOCTEXT("FailOnScanLabel", "Fail on scan errors"),
						LOCTEXT("FailOnScanTooltip",
							"Refuse to write a bundle when the public API scan reported errors. Off by default: the author usually "
							"needs the bundle and the report side by side to see what to fix."),
						&bFailOnScanErrors)
				]
			]

			//~ Actions -----------------------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>(TEXT("PrimaryButton")))
					.IsEnabled(this, &SModSDKGeneratePanel::IsGenerateEnabled)
					.ToolTipText(LOCTEXT("GenerateTooltip", "Build the bundle now. This copies plugin trees and scans the whole reflection database."))
					.OnClicked(this, &SModSDKGeneratePanel::HandleGenerateClicked)
					.ContentPadding(FMargin(16.0f, 4.0f))
					[
						SNew(STextBlock).Text(LOCTEXT("GenerateLabel", "Generate"))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Visibility(this, &SModSDKGeneratePanel::GetRevealVisibility)
					.ToolTipText(LOCTEXT("RevealTooltip", "Open the generated bundle folder in the file browser."))
					.OnClicked(this, &SModSDKGeneratePanel::HandleRevealBundleClicked)
					.ContentPadding(FMargin(12.0f, 4.0f))
					[
						SNew(STextBlock).Text(LOCTEXT("RevealLabel", "Show bundle folder"))
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNullWidget::NullWidget
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ToolTipText(LOCTEXT("ResetTooltip", "Re-derive every field from the project settings and the plugin descriptors."))
					.OnClicked(this, &SModSDKGeneratePanel::HandleResetDefaultsClicked)
					.ContentPadding(FMargin(12.0f, 4.0f))
					[
						SNew(STextBlock).Text(LOCTEXT("ResetLabel", "Reset to defaults"))
					]
				]
			]

			//~ Result ------------------------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SBorder)
				.Visibility(this, &SModSDKGeneratePanel::GetResultVisibility)
				.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
				.Padding(10.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(this, &SModSDKGeneratePanel::GetResultSummaryText)
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				ModSDKWidgetUtils::MakeSectionHeader(LOCTEXT("DiagnosticsTitle", "Diagnostics"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				DiagnosticList.ToSharedRef()
			]
		]
	];
}

void SModSDKGeneratePanel::ResolveDefaults()
{
	FModSDKGenerateOptions Options;
	Options.SdkPluginName = ModSDKWidgetUtils::PickDefaultSdkPluginName();

	// ResolveOptions refuses to derive anything without a plugin name, which is the honest answer when
	// a project has no SDK plugin at all. The fields are still filled with what can be known so the
	// form is usable, and the Generate button reports the real problem when it is pressed.
	TArray<FModDiagnostic> Diagnostics;
	FModSDKGenerator::ResolveOptions(Options, Diagnostics);

	OutputDirectory = Options.OutputDirectory.IsEmpty()
		? FModSDKGenerator::GetDefaultOutputDirectory()
		: Options.OutputDirectory;
	SdkPluginName = Options.SdkPluginName;
	SdkName = Options.SdkName.IsEmpty() ? Options.SdkPluginName : Options.SdkName;
	SdkVersion = Options.SdkVersion;

	bIncludeDocs = Options.bIncludeDocs;
	bIncludeTemplateProject = Options.bIncludeTemplateProject;
	bWriteApiReport = Options.bWriteApiReport;
	bIncludeBinaries = Options.bIncludeBinaries;
	bIncludeGeneratorSource = Options.bIncludeGeneratorSource;
	bOverwriteExisting = Options.bOverwriteExisting;
	bFailOnScanErrors = Options.bFailOnScanErrors;

	if (DiagnosticList.IsValid())
	{
		DiagnosticList->SetDiagnostics(Diagnostics);
	}
}

FText SModSDKGeneratePanel::GetOutputDirectoryText() const
{
	return FText::FromString(OutputDirectory);
}

void SModSDKGeneratePanel::HandleOutputDirectoryCommitted(const FText& InText, ETextCommit::Type InCommitType)
{
	OutputDirectory = InText.ToString().TrimStartAndEnd();
}

FText SModSDKGeneratePanel::GetSdkPluginNameText() const
{
	return FText::FromString(SdkPluginName);
}

void SModSDKGeneratePanel::HandleSdkPluginNameCommitted(const FText& InText, ETextCommit::Type InCommitType)
{
	SdkPluginName = InText.ToString().TrimStartAndEnd();
}

FText SModSDKGeneratePanel::GetSdkNameText() const
{
	return FText::FromString(SdkName);
}

void SModSDKGeneratePanel::HandleSdkNameCommitted(const FText& InText, ETextCommit::Type InCommitType)
{
	SdkName = InText.ToString().TrimStartAndEnd();
}

FText SModSDKGeneratePanel::GetSdkVersionText() const
{
	return FText::FromString(SdkVersion);
}

void SModSDKGeneratePanel::HandleSdkVersionCommitted(const FText& InText, ETextCommit::Type InCommitType)
{
	SdkVersion = InText.ToString().TrimStartAndEnd();
}

TSharedRef<SWidget> SModSDKGeneratePanel::BuildSdkPluginMenu()
{
	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection*/ true, /*InCommandList*/ nullptr);

	const TArray<FString> Candidates = ModSDKWidgetUtils::GetCandidateSdkPluginNames();
	if (Candidates.Num() == 0)
	{
		MenuBuilder.AddWidget(
			SNew(STextBlock)
			.Text(LOCTEXT("NoCandidatePlugins", "No project-side plugins were discovered.")),
			FText::GetEmpty());

		return MenuBuilder.MakeWidget();
	}

	for (const FString& Candidate : Candidates)
	{
		MenuBuilder.AddMenuEntry(
			FText::FromString(Candidate),
			FText::Format(LOCTEXT("PickPluginTooltipFmt", "Build the bundle around the {0} plugin."), FText::FromString(Candidate)),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SModSDKGeneratePanel::HandleSdkPluginPicked, Candidate)));
	}

	return MenuBuilder.MakeWidget();
}

void SModSDKGeneratePanel::HandleSdkPluginPicked(FString InPluginName)
{
	SdkPluginName = InPluginName;

	// The bundle name defaults to the plugin name, and a stale name from a previous pick would end up
	// stamped into SDKVersion.json. Only overwrite a name that was already tracking the plugin.
	if (SdkName.IsEmpty())
	{
		SdkName = MoveTemp(InPluginName);
	}
}

FReply SModSDKGeneratePanel::HandleBrowseClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::TryGet();
	if (!DesktopPlatform || !FSlateApplication::IsInitialized())
	{
		UE_LOG(LogModFrameworkEditor, Warning,
			TEXT("Cannot open a directory dialog: the desktop platform module or Slate is unavailable."));
		return FReply::Handled();
	}

	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());

	const FString StartDirectory = OutputDirectory.IsEmpty()
		? FModSDKGenerator::GetDefaultOutputDirectory()
		: OutputDirectory;

	FString ChosenDirectory;
	const bool bPicked = DesktopPlatform->OpenDirectoryDialog(
		ParentWindowHandle,
		LOCTEXT("ChooseOutputDirTitle", "Choose the directory the SDK bundle folder is created in").ToString(),
		StartDirectory,
		ChosenDirectory);

	if (bPicked && !ChosenDirectory.IsEmpty())
	{
		// The text box reads OutputDirectory through a bound attribute, so writing the member is the
		// whole update: there is no second copy of this string to keep in step.
		OutputDirectory = FPaths::ConvertRelativePathToFull(ChosenDirectory);
	}

	return FReply::Handled();
}

FReply SModSDKGeneratePanel::HandleGenerateClicked()
{
	FModSDKGenerateOptions Options;
	Options.OutputDirectory = OutputDirectory;
	Options.SdkPluginName = SdkPluginName;
	Options.SdkName = SdkName;
	Options.SdkVersion = SdkVersion;
	Options.bIncludeDocs = bIncludeDocs;
	Options.bIncludeTemplateProject = bIncludeTemplateProject;
	Options.bWriteApiReport = bWriteApiReport;
	Options.bIncludeBinaries = bIncludeBinaries;
	Options.bIncludeGeneratorSource = bIncludeGeneratorSource;
	Options.bOverwriteExisting = bOverwriteExisting;
	Options.bFailOnScanErrors = bFailOnScanErrors;

	TArray<FModDiagnostic> Diagnostics;
	FModSDKGenerateResult Result;

	{
		// Copying two plugin trees and walking the whole reflection database takes seconds, not
		// milliseconds. A modal slow task is honest about that; a spinner that lies is not.
		FScopedSlowTask SlowTask(1.0f, LOCTEXT("GeneratingSlowTask", "Generating the SDK bundle..."));
		SlowTask.MakeDialog();
		SlowTask.EnterProgressFrame(1.0f);

		bLastRunSucceeded = FModSDKGenerator::GenerateBundle(Options, Result, Diagnostics);
	}

	LastResult = Result;
	bHasResult = true;

	if (DiagnosticList.IsValid())
	{
		DiagnosticList->SetDiagnostics(Diagnostics);
	}

	// The scan carried out of the generator is the one that produced the shipped index, so the API
	// inspector shows exactly what the bundle documents rather than a second, separate scan.
	if (OnApiReportGenerated.IsBound())
	{
		OnApiReportGenerated.Execute(Result.ApiReport);
	}

	FNotificationInfo Info(bLastRunSucceeded
		? FText::Format(LOCTEXT("GenerateSucceededFmt", "SDK bundle written to {0}"), FText::FromString(Result.BundleDirectory))
		: LOCTEXT("GenerateFailed", "SDK generation failed. See the diagnostics below."));
	Info.ExpireDuration = bLastRunSucceeded ? 6.0f : 10.0f;
	Info.bFireAndForget = true;

	const TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid())
	{
		Notification->SetCompletionState(bLastRunSucceeded
			? SNotificationItem::CS_Success
			: SNotificationItem::CS_Fail);
	}

	UE_LOG(LogModFrameworkEditor, Log,
		TEXT("SDK generation %s. Bundle=\"%s\" Files copied=%d Generated=%d Diagnostics=%d"),
		bLastRunSucceeded ? TEXT("succeeded") : TEXT("failed"),
		*Result.BundleDirectory, Result.FilesCopied, Result.FilesGenerated, Diagnostics.Num());

	// A bundle is something you then zip up and hand to somebody, so the folder is opened on success
	// rather than merely named. The button below stays for every later visit.
	if (bLastRunSucceeded && !Result.BundleDirectory.IsEmpty()
		&& IFileManager::Get().DirectoryExists(*Result.BundleDirectory))
	{
		FPlatformProcess::ExploreFolder(*Result.BundleDirectory);
	}

	return FReply::Handled();
}

FReply SModSDKGeneratePanel::HandleRevealBundleClicked()
{
	if (bHasResult && !LastResult.BundleDirectory.IsEmpty())
	{
		FPlatformProcess::ExploreFolder(*LastResult.BundleDirectory);
	}

	return FReply::Handled();
}

FReply SModSDKGeneratePanel::HandleResetDefaultsClicked()
{
	bHasResult = false;
	bLastRunSucceeded = false;
	LastResult = FModSDKGenerateResult();
	ResolveDefaults();
	return FReply::Handled();
}

bool SModSDKGeneratePanel::IsGenerateEnabled() const
{
	return !SdkPluginName.IsEmpty() && !OutputDirectory.IsEmpty();
}

FText SModSDKGeneratePanel::GetBundlePreviewText() const
{
	if (OutputDirectory.IsEmpty())
	{
		return LOCTEXT("NoOutputDirectory", "Choose an output directory.");
	}

	const FString EffectiveName = SdkName.IsEmpty() ? SdkPluginName : SdkName;
	const FString Folder = FModSDKGenerator::MakeBundleFolderName(EffectiveName, SdkVersion);

	return FText::Format(LOCTEXT("BundlePreviewFmt", "Bundle folder: {0}"),
		FText::FromString(OutputDirectory / Folder));
}

FText SModSDKGeneratePanel::GetResultSummaryText() const
{
	if (!bHasResult)
	{
		return FText::GetEmpty();
	}

	if (!bLastRunSucceeded)
	{
		return LOCTEXT("ResultFailed",
			"Generation failed and no bundle was written. The diagnostics below name the step that stopped it.");
	}

	TArray<FText> Included;
	if (LastResult.bDocsIncluded)
	{
		Included.Add(LOCTEXT("IncludedDocs", "docs"));
	}
	if (LastResult.bTemplateIncluded)
	{
		Included.Add(LOCTEXT("IncludedTemplate", "project template"));
	}
	if (LastResult.bApiReportIncluded)
	{
		Included.Add(LOCTEXT("IncludedApiReport", "API report"));
	}

	const FText IncludedText = Included.Num() > 0
		? FText::Join(LOCTEXT("IncludedSeparator", ", "), Included)
		: LOCTEXT("IncludedNothingExtra", "plugins only");

	return FText::Format(
		LOCTEXT("ResultSucceededFmt",
			"{0}\n\n"
			"{1} file(s) copied, {2} generated, {3} bytes.\n"
			"Contains: {4}.\n"
			"Stamped: SDK {5} {6} · game {7} {8} · framework {9} · engine {10} · manifest v{11} · package format v{12}.\n"
			"Public surface: {13}."),
		FText::FromString(LastResult.BundleDirectory),
		FText::AsNumber(LastResult.FilesCopied),
		FText::AsNumber(LastResult.FilesGenerated),
		FText::AsNumber(LastResult.BytesCopied),
		IncludedText,
		ModSDKWidgetUtils::OrDash(LastResult.SdkId),
		ModSDKWidgetUtils::OrDash(LastResult.SdkVersion),
		ModSDKWidgetUtils::OrDash(LastResult.GameId),
		ModSDKWidgetUtils::OrDash(LastResult.GameVersion),
		ModSDKWidgetUtils::OrDash(LastResult.FrameworkVersion),
		ModSDKWidgetUtils::OrDash(LastResult.EngineVersion),
		FText::AsNumber(LastResult.ManifestVersion),
		FText::AsNumber(LastResult.PackageFormatVersion),
		FText::FromString(LastResult.ApiReport.DescribeCounts()));
}

EVisibility SModSDKGeneratePanel::GetResultVisibility() const
{
	return bHasResult ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SModSDKGeneratePanel::GetRevealVisibility() const
{
	if (!bHasResult || !bLastRunSucceeded || LastResult.BundleDirectory.IsEmpty())
	{
		return EVisibility::Collapsed;
	}

	return IFileManager::Get().DirectoryExists(*LastResult.BundleDirectory)
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

TSharedRef<SWidget> SModSDKGeneratePanel::MakeLabelledRow(const FText& InLabel, const FText& InToolTip,
	TSharedRef<SWidget> InContent) const
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(ModSDKGeneratePanelPrivate::LabelWidth)
			[
				SNew(STextBlock)
				.Text(InLabel)
				.ToolTipText(InToolTip)
			]
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			InContent
		];
}

TSharedRef<SWidget> SModSDKGeneratePanel::MakeOptionCheckBox(const FText& InLabel, const FText& InToolTip, bool* InFlag)
{
	return SNew(SCheckBox)
		.ToolTipText(InToolTip)
		.IsChecked(this, &SModSDKGeneratePanel::GetFlagCheckState, InFlag)
		.OnCheckStateChanged(this, &SModSDKGeneratePanel::HandleFlagChanged, InFlag)
		[
			SNew(STextBlock)
			.Text(InLabel)
			.ToolTipText(InToolTip)
		];
}

ECheckBoxState SModSDKGeneratePanel::GetFlagCheckState(bool* InFlag) const
{
	return (InFlag && *InFlag) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SModSDKGeneratePanel::HandleFlagChanged(ECheckBoxState InNewState, bool* InFlag)
{
	if (InFlag)
	{
		*InFlag = InNewState == ECheckBoxState::Checked;
	}
}

#undef LOCTEXT_NAMESPACE
