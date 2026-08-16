// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModPackagePanel.h"

#include "Async/Async.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/Platform.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IDesktopPlatform.h"
#include "Input/Reply.h"
#include "Layout/Margin.h"
#include "Manifest/ModManifestParser.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"
#include "ModDeveloperModel.h"
#include "ModFrameworkEditorModule.h"
#include "ModPackagingSettings.h"
#include "Packaging/ModPackageFormat.h"
#include "Packaging/ModPackageWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Types/SlateEnums.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/ModDeveloperUI.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SModPackagePanel"

namespace ModPackagePanelPrivate
{
	/** Diagnostic code the panel itself raises, kept distinct from the writer's own Package.* codes. */
	static const FName CodeBadSource(TEXT("Editor.Package.BadSource"));
	static const FName CodeBadOutput(TEXT("Editor.Package.BadOutput"));

	/**
	 * How many files a run would write.
	 *
	 * Purely for the progress readout, so it mirrors FModPackageWriter::PackageDirectory's own
	 * filtering rather than trying to be exact: mod.json is stored in the header region rather than
	 * as an entry, and build artefacts are excluded.
	 */
	int32 CountPackageableFiles(const FString& SourceDirectory)
	{
		TArray<FString> FoundFiles;
		IFileManager::Get().FindFilesRecursive(FoundFiles, *SourceDirectory, TEXT("*"),
			/*Files*/ true, /*Directories*/ false);

		int32 Count = 0;
		for (const FString& AbsoluteFile : FoundFiles)
		{
			FString RelativePath = AbsoluteFile;
			FPaths::MakePathRelativeTo(RelativePath, *(SourceDirectory / TEXT("")));
			RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);

			if (RelativePath.Equals(FModManifestParser::GetManifestFileName(), ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (FModPackageWriter::ShouldExcludeFromPackage(RelativePath))
			{
				continue;
			}

			++Count;
		}

		return Count;
	}

	/** The handle a modal dialog should parent itself to, or null when Slate has no window to offer. */
	const void* GetDialogParentWindow(const TSharedPtr<SWidget>& InWidget)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return nullptr;
		}

		return FSlateApplication::Get().FindBestParentWindowHandleForDialogs(InWidget);
	}
}

void SModPackagePanel::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;

	if (Model.IsValid())
	{
		Model->OnChanged.AddSP(this, &SModPackagePanel::HandleModelChanged);
	}

	if (const UModPackagingSettings* Settings = UModPackagingSettings::Get())
	{
		SourceDirectory = Settings->LastSourceDirectory;
		OutputDirectory = Settings->LastOutputDirectory;
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Source ----------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SourceLabel", "Mod folder"))
			.Font(FAppStyle::GetFontStyle(TEXT("PropertyWindow.BoldFont")))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SAssignNew(SourceTextBox, SEditableTextBox)
				.Text(FText::FromString(SourceDirectory))
				.HintText(LOCTEXT("SourceHint", "The folder containing this mod's mod.json"))
				.OnTextCommitted(this, &SModPackagePanel::HandleSourceTextCommitted)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.ToolTipText(LOCTEXT("BrowseSourceTip", "Choose the mod folder to package."))
				.OnClicked(this, &SModPackagePanel::HandleBrowseSourceClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("Browse", "Browse..."))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.ToolTipText(LOCTEXT("UseSelectedTip", "Fill this in from the mod selected on the Mods tab."))
				.OnClicked(this, &SModPackagePanel::HandleUseSelectedModClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("UseSelected", "Use listed mod"))
				]
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(this, &SModPackagePanel::GetSourceSummaryText)
		]

		// --- Output ----------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("OutputLabel", "Output directory"))
			.Font(FAppStyle::GetFontStyle(TEXT("PropertyWindow.BoldFont")))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SAssignNew(OutputTextBox, SEditableTextBox)
				.Text(FText::FromString(OutputDirectory))
				.HintText(LOCTEXT("OutputHint", "Where the .mod file is written - remembered per mod"))
				.OnTextCommitted(this, &SModPackagePanel::HandleOutputTextCommitted)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.ToolTipText(LOCTEXT("BrowseOutputTip", "Choose where the packaged .mod file goes. The choice is remembered for this mod."))
				.OnClicked(this, &SModPackagePanel::HandleBrowseOutputClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("BrowseOut", "Browse..."))
				]
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock)
			.Text(this, &SModPackagePanel::GetOutputFileText)
			.Font(ModDeveloperUI::MonospaceFont())
		]

		// --- Action + progress -----------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ContentPadding(FMargin(14.0f, 4.0f))
				.IsEnabled(this, &SModPackagePanel::IsPackageButtonEnabled)
				.ToolTipText(LOCTEXT("PackageTip", "Write the mod folder into a single .mod container, ready to hand to a player."))
				.OnClicked(this, &SModPackagePanel::HandlePackageClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("Package", "Package mod"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility(this, &SModPackagePanel::GetProgressVisibility)
				[
					SNew(SThrobber)
					.Animate(SThrobber::Horizontal)
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SModPackagePanel::GetProgressText)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.Visibility(this, &SModPackagePanel::GetRevealVisibility)
				.ToolTipText(LOCTEXT("RevealTip", "Show the packaged .mod file in the file explorer."))
				.OnClicked(this, &SModPackagePanel::HandleRevealClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("Reveal", "Show in Explorer"))
				]
			]
		]

		// --- Result ----------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 4.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(this, &SModPackagePanel::GetResultText)
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			.Padding(2.0f)
			[
				SAssignNew(DiagnosticList, SListView<TSharedPtr<FModDiagnostic>>)
				.ListItemsSource(&ResultDiagnostics)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SModPackagePanel::HandleGenerateDiagnosticRow)
			]
		]
	];

	RefreshSourceManifest();
	RestoreRememberedOutput();
}

SModPackagePanel::~SModPackagePanel()
{
	if (Model.IsValid())
	{
		Model->OnChanged.RemoveAll(this);
	}
}

void SModPackagePanel::HandleModelChanged()
{
	// Nothing the model holds feeds the packager directly; a mod list refresh only matters because
	// "Use listed mod" reads the current selection at click time.
}

void SModPackagePanel::SetSourceDirectory(const FString& InDirectory)
{
	if (bPackaging || InDirectory.IsEmpty())
	{
		return;
	}

	SourceDirectory = FPaths::ConvertRelativePathToFull(InDirectory);

	if (SourceTextBox.IsValid())
	{
		SourceTextBox->SetText(FText::FromString(SourceDirectory));
	}

	RefreshSourceManifest();
	RestoreRememberedOutput();
}

void SModPackagePanel::RefreshSourceManifest()
{
	SourceManifest = FModManifest();
	bSourceValid = false;
	SourceStatus.Reset();

	if (SourceDirectory.IsEmpty())
	{
		SourceStatus = TEXT("Choose the folder that contains the mod's mod.json.");
		return;
	}

	if (!IFileManager::Get().DirectoryExists(*SourceDirectory))
	{
		SourceStatus = TEXT("That folder does not exist.");
		return;
	}

	const FString ManifestPath = FPaths::Combine(SourceDirectory, FModManifestParser::GetManifestFileName());
	if (!FPaths::FileExists(ManifestPath))
	{
		SourceStatus = FString::Printf(TEXT("No %s at the root of that folder. A mod folder must contain one."),
			FModManifestParser::GetManifestFileName());
		return;
	}

	const FModManifestParseResult Parsed = FModManifestParser::ParseFromFile(ManifestPath);
	SourceManifest = Parsed.Manifest;
	bSourceValid = Parsed.bSuccess;

	if (!Parsed.bSuccess)
	{
		SourceStatus = FString::Printf(TEXT("The manifest did not validate:\n%s"),
			*ModDiagnostics::Join(Parsed.Diagnostics));
		return;
	}

	SourceStatus = FString::Printf(TEXT("%s %s by %s"),
		*SourceManifest.GetDisplayNameOrId(),
		*SourceManifest.Version.ToString(),
		SourceManifest.Author.IsEmpty() ? TEXT("an unnamed author") : *SourceManifest.Author);

	// A manifest can validate and still have something to say. Saying nothing about that here would
	// mean an author only ever sees their warnings if they happen to open the Validation tab.
	const int32 NoteCount = Parsed.Diagnostics.Num();
	if (NoteCount > 0)
	{
		SourceStatus += FString::Printf(
			TEXT("\n%d manifest note(s) - see the Validation tab for the detail."), NoteCount);
	}
}

void SModPackagePanel::RestoreRememberedOutput()
{
	const UModPackagingSettings* Settings = UModPackagingSettings::Get();
	if (Settings == nullptr)
	{
		return;
	}

	const FString Key = UModPackagingSettings::MakeModKey(
		bSourceValid ? SourceManifest.Id.ToString() : FString(),
		SourceDirectory);

	const FString Remembered = Settings->FindOutputDirectory(Key);
	if (Remembered.IsEmpty())
	{
		return;
	}

	OutputDirectory = Remembered;
	if (OutputTextBox.IsValid())
	{
		OutputTextBox->SetText(FText::FromString(OutputDirectory));
	}
}

FReply SModPackagePanel::HandleBrowseSourceClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform == nullptr)
	{
		UE_LOG(LogModFrameworkEditor, Warning, TEXT("No desktop platform module: cannot open a directory dialog."));
		return FReply::Handled();
	}

	FString DefaultPath = SourceDirectory;
	if (DefaultPath.IsEmpty())
	{
		if (const UModPackagingSettings* Settings = UModPackagingSettings::Get())
		{
			DefaultPath = Settings->LastSourceDirectory;
		}
	}

	if (DefaultPath.IsEmpty())
	{
		DefaultPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}

	FString Chosen;
	const bool bPicked = DesktopPlatform->OpenDirectoryDialog(
		ModPackagePanelPrivate::GetDialogParentWindow(AsShared()),
		LOCTEXT("PickSourceTitle", "Choose the mod folder to package").ToString(),
		DefaultPath,
		Chosen);

	if (!bPicked || Chosen.IsEmpty())
	{
		return FReply::Handled();
	}

	SourceDirectory = FPaths::ConvertRelativePathToFull(Chosen);
	if (SourceTextBox.IsValid())
	{
		SourceTextBox->SetText(FText::FromString(SourceDirectory));
	}

	if (UModPackagingSettings* Settings = UModPackagingSettings::GetMutable())
	{
		Settings->RememberSourceDirectory(SourceDirectory);
		Settings->SaveNow();
	}

	RefreshSourceManifest();
	RestoreRememberedOutput();
	return FReply::Handled();
}

FReply SModPackagePanel::HandleBrowseOutputClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform == nullptr)
	{
		UE_LOG(LogModFrameworkEditor, Warning, TEXT("No desktop platform module: cannot open a directory dialog."));
		return FReply::Handled();
	}

	FString DefaultPath = OutputDirectory;
	if (DefaultPath.IsEmpty())
	{
		DefaultPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	}

	FString Chosen;
	const bool bPicked = DesktopPlatform->OpenDirectoryDialog(
		ModPackagePanelPrivate::GetDialogParentWindow(AsShared()),
		LOCTEXT("PickOutputTitle", "Choose where the .mod file is written").ToString(),
		DefaultPath,
		Chosen);

	if (!bPicked || Chosen.IsEmpty())
	{
		return FReply::Handled();
	}

	OutputDirectory = FPaths::ConvertRelativePathToFull(Chosen);
	if (OutputTextBox.IsValid())
	{
		OutputTextBox->SetText(FText::FromString(OutputDirectory));
	}

	// Remembering here rather than at package time means the choice survives even if the run itself
	// then fails, which is exactly when an author is most likely to try again.
	if (UModPackagingSettings* Settings = UModPackagingSettings::GetMutable())
	{
		const FString Key = UModPackagingSettings::MakeModKey(
			bSourceValid ? SourceManifest.Id.ToString() : FString(),
			SourceDirectory);

		Settings->RememberOutputDirectory(Key, OutputDirectory);
		Settings->SaveNow();
	}

	return FReply::Handled();
}

FReply SModPackagePanel::HandleUseSelectedModClicked()
{
	// The window fills the source in directly when the Mods tab asks to package something, so this
	// button only has to cover the case where the user came straight here.
	if (!Model.IsValid())
	{
		return FReply::Handled();
	}

	for (const TSharedPtr<FModDeveloperModRow>& Row : Model->GetMods())
	{
		if (Row.IsValid() && !Row->bPackaged && !Row->RootPath.IsEmpty())
		{
			SetSourceDirectory(Row->RootPath);
			break;
		}
	}

	return FReply::Handled();
}

void SModPackagePanel::HandleSourceTextCommitted(const FText& NewText, ETextCommit::Type /*CommitType*/)
{
	const FString Typed = NewText.ToString().TrimStartAndEnd();
	SourceDirectory = Typed.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Typed);

	RefreshSourceManifest();
	RestoreRememberedOutput();
}

void SModPackagePanel::HandleOutputTextCommitted(const FText& NewText, ETextCommit::Type /*CommitType*/)
{
	const FString Typed = NewText.ToString().TrimStartAndEnd();
	OutputDirectory = Typed.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Typed);
}

FString SModPackagePanel::MakeOutputFileName() const
{
	FString BaseName;
	if (bSourceValid && SourceManifest.Id.IsValid())
	{
		BaseName = SourceManifest.Id.ToString();

		const FString VersionText = SourceManifest.Version.ToString();
		if (!VersionText.IsEmpty())
		{
			BaseName += TEXT("-") + VersionText;
		}
	}
	else if (!SourceDirectory.IsEmpty())
	{
		BaseName = FPaths::GetCleanFilename(SourceDirectory);
	}

	if (BaseName.IsEmpty())
	{
		BaseName = TEXT("mod");
	}

	// A mod id is already restricted to [a-z0-9._-], but a version may carry build metadata with a
	// '+' in it and the folder-name fallback is whatever the filesystem allowed. Neither is trusted.
	return FPaths::MakeValidFileName(BaseName, TEXT('_')) + ModPackage::GetFileExtension();
}

bool SModPackagePanel::IsPackageButtonEnabled() const
{
	return !bPackaging && bSourceValid && !OutputDirectory.IsEmpty();
}

EVisibility SModPackagePanel::GetProgressVisibility() const
{
	return bPackaging ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SModPackagePanel::GetRevealVisibility() const
{
	return (!bPackaging && bLastRunSucceeded && !LastPackagePath.IsEmpty())
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FText SModPackagePanel::GetSourceSummaryText() const
{
	return FText::FromString(SourceStatus);
}

FText SModPackagePanel::GetOutputFileText() const
{
	if (OutputDirectory.IsEmpty())
	{
		return LOCTEXT("NoOutput", "Pick an output directory. The choice is remembered for this mod.");
	}

	return FText::Format(
		LOCTEXT("WillWrite", "Will write:  {0}"),
		FText::FromString(FPaths::Combine(OutputDirectory, MakeOutputFileName())));
}

FText SModPackagePanel::GetProgressText() const
{
	if (!bPackaging)
	{
		return FText::GetEmpty();
	}

	const double Elapsed = FPlatformTime::Seconds() - PackageStartTime;
	return FText::Format(
		LOCTEXT("Packaging", "Packaging {0} file(s)...  {1}s"),
		FText::AsNumber(PendingFileCount),
		FText::AsNumber(FMath::FloorToInt(static_cast<float>(Elapsed))));
}

FText SModPackagePanel::GetResultText() const
{
	return LastResultSummary;
}

FReply SModPackagePanel::HandlePackageClicked()
{
	if (bPackaging)
	{
		return FReply::Handled();
	}

	ResultDiagnostics.Reset();
	bLastRunSucceeded = false;
	LastPackagePath.Reset();
	LastResultSummary = FText::GetEmpty();

	auto Fail = [this](FName Code, const FString& Message, const FString& Context)
	{
		ResultDiagnostics.Add(MakeShared<FModDiagnostic>(FModDiagnostic::Error(Code, Message, Context)));
		LastResultSummary = FText::FromString(Message);

		if (DiagnosticList.IsValid())
		{
			DiagnosticList->RequestListRefresh();
		}
	};

	if (SourceDirectory.IsEmpty() || !IFileManager::Get().DirectoryExists(*SourceDirectory))
	{
		Fail(ModPackagePanelPrivate::CodeBadSource,
			TEXT("The mod folder does not exist. Pick one that contains a mod.json."),
			SourceDirectory);
		return FReply::Handled();
	}

	if (OutputDirectory.IsEmpty())
	{
		Fail(ModPackagePanelPrivate::CodeBadOutput,
			TEXT("No output directory chosen."),
			FString());
		return FReply::Handled();
	}

	// Creating the directory now rather than letting the writer discover it is missing means the
	// author gets "I could not create that folder" before a minute of compression, not after it.
	if (!IFileManager::Get().DirectoryExists(*OutputDirectory)
		&& !IFileManager::Get().MakeDirectory(*OutputDirectory, /*Tree*/ true))
	{
		Fail(ModPackagePanelPrivate::CodeBadOutput,
			TEXT("The output directory does not exist and could not be created. Check the path and that you can write to it."),
			OutputDirectory);
		return FReply::Handled();
	}

	const FString Destination = FPaths::Combine(OutputDirectory, MakeOutputFileName());

	if (UModPackagingSettings* Settings = UModPackagingSettings::GetMutable())
	{
		const FString Key = UModPackagingSettings::MakeModKey(
			bSourceValid ? SourceManifest.Id.ToString() : FString(),
			SourceDirectory);

		Settings->RememberOutputDirectory(Key, OutputDirectory);
		Settings->RememberSourceDirectory(SourceDirectory);
		Settings->SaveNow();
	}

	StartPackaging(SourceDirectory, Destination);
	return FReply::Handled();
}

void SModPackagePanel::StartPackaging(const FString& InSource, const FString& InDestination)
{
	PendingFileCount = ModPackagePanelPrivate::CountPackageableFiles(InSource);
	PackageStartTime = FPlatformTime::Seconds();
	bPackaging = true;

	TSharedPtr<FModPackageJob> Job = MakeShared<FModPackageJob>();
	Job->SourceDirectory = InSource;
	Job->DestinationFile = InDestination;

	TWeakPtr<SModPackagePanel> WeakSelf = SharedThis(this);

	// PackageDirectory is pure file I/O plus the manifest parser - no UObjects, no Slate - so it is
	// safe off the game thread, and running it there is what keeps the editor responsive while a
	// few hundred megabytes of mod content are hashed and compressed.
	Async(EAsyncExecution::ThreadPool, [Job, WeakSelf]()
	{
		const double Start = FPlatformTime::Seconds();
		Job->bSucceeded = FModPackageWriter::PackageDirectory(
			Job->SourceDirectory, Job->DestinationFile, Job->Diagnostics);
		Job->DurationSeconds = FPlatformTime::Seconds() - Start;

		if (Job->bSucceeded)
		{
			Job->ResultBytes = IFileManager::Get().FileSize(*Job->DestinationFile);
		}

		AsyncTask(ENamedThreads::GameThread, [Job, WeakSelf]()
		{
			if (const TSharedPtr<SModPackagePanel> Pinned = WeakSelf.Pin())
			{
				Pinned->FinishPackaging(Job);
			}
		});
	});
}

void SModPackagePanel::FinishPackaging(TSharedPtr<FModPackageJob> Job)
{
	bPackaging = false;

	if (!Job.IsValid())
	{
		LastResultSummary = LOCTEXT("LostJob", "The packaging run reported back with no result. Check the log.");
		return;
	}

	ResultDiagnostics.Reset();
	ResultDiagnostics.Reserve(Job->Diagnostics.Num());
	for (const FModDiagnostic& Diagnostic : Job->Diagnostics)
	{
		ResultDiagnostics.Add(MakeShared<FModDiagnostic>(Diagnostic));
	}

	bLastRunSucceeded = Job->bSucceeded;
	LastPackagePath = Job->bSucceeded ? Job->DestinationFile : FString();

	if (Job->bSucceeded)
	{
		const double Megabytes = static_cast<double>(Job->ResultBytes) / (1024.0 * 1024.0);
		LastResultSummary = FText::Format(
			LOCTEXT("PackageSucceeded", "Packaged to {0}  ({1})."),
			FText::FromString(Job->DestinationFile),
			FText::FromString(FString::Printf(TEXT("%.2f MB, %.2f s"), Megabytes, Job->DurationSeconds)));

		UE_LOG(LogModFrameworkEditor, Log, TEXT("Packaged '%s' to '%s' in %.2fs."),
			*Job->SourceDirectory, *Job->DestinationFile, Job->DurationSeconds);

		const UModPackagingSettings* Settings = UModPackagingSettings::Get();
		if (Settings != nullptr && Settings->bRevealPackageInExplorer)
		{
			FPlatformProcess::ExploreFolder(*Job->DestinationFile);
		}
	}
	else
	{
		LastResultSummary = FText::Format(
			LOCTEXT("PackageFailed", "Packaging failed after {0}. Nothing was written to the output directory."),
			FText::FromString(FString::Printf(TEXT("%.2f s"), Job->DurationSeconds)));

		UE_LOG(LogModFrameworkEditor, Warning, TEXT("Packaging '%s' failed: %s"),
			*Job->SourceDirectory, *ModDiagnostics::Join(Job->Diagnostics, TEXT(" | ")));
	}

	if (DiagnosticList.IsValid())
	{
		DiagnosticList->RequestListRefresh();
	}
}

FReply SModPackagePanel::HandleRevealClicked()
{
	if (!LastPackagePath.IsEmpty())
	{
		FPlatformProcess::ExploreFolder(*LastPackagePath);
	}

	return FReply::Handled();
}

TSharedRef<ITableRow> SModPackagePanel::HandleGenerateDiagnosticRow(TSharedPtr<FModDiagnostic> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const EModDiagnosticSeverity Severity = Item.IsValid() ? Item->Severity : EModDiagnosticSeverity::Info;

	return SNew(STableRow<TSharedPtr<FModDiagnostic>>, OwnerTable)
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(16.0f)
				.HeightOverride(16.0f)
				[
					SNew(SImage)
					.Image(ModDeveloperUI::SeverityIcon(Severity))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Item.IsValid() ? FText::FromName(Item->Code) : FText::GetEmpty())
				.Font(ModDeveloperUI::MonospaceFont())
				.ColorAndOpacity(ModDeveloperUI::SeverityColor(Severity))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(Item.IsValid() ? FText::FromString(Item->Message) : FText::GetEmpty())
				.ToolTipText(Item.IsValid() ? FText::FromString(Item->Context) : FText::GetEmpty())
			]
		];
}

#undef LOCTEXT_NAMESPACE
