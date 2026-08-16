// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Layout/Visibility.h"
#include "Manifest/ModManifest.h"
#include "Templates/SharedPointer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FModDeveloperModel;
class ITableRow;
class SEditableTextBox;
class STableViewBase;

/**
 * One packaging run, handed between the game thread and the worker that does the writing.
 *
 * Shared rather than captured field by field so the completion hop back to the game thread needs one
 * capture, and so the panel can drop its reference without stranding the worker.
 */
struct FModPackageJob
{
	FString SourceDirectory;
	FString DestinationFile;
	TArray<FModDiagnostic> Diagnostics;
	bool bSucceeded = false;
	double DurationSeconds = 0.0;
	int64 ResultBytes = 0;
};

/**
 * The packaging tab: pick a mod folder, pick where the `.mod` goes, press one button.
 *
 * THE OUTPUT DIRECTORY IS REMEMBERED PER MOD
 * A mod author packages the same mod into the same folder - their game's Mods directory, usually -
 * dozens of times a day. UModPackagingSettings stores that choice keyed by mod id in the editor's
 * per-project user config, so it is picked once and never again, and it does not end up in a config
 * file under source control.
 *
 * THE WRITE HAPPENS OFF THE GAME THREAD
 * FModPackageWriter::PackageDirectory reads and compresses every file in the mod, which for a real
 * mod is seconds, not milliseconds. Running that inline would freeze the editor, so it runs on a
 * worker and hops back to the game thread to report - which is also why the panel holds the job
 * through a shared pointer and re-checks itself with a weak pointer when the job lands.
 *
 * Everything it reads is untrusted: a mod.json somebody else wrote, in a folder somebody else made.
 * Nothing here asserts on any of it; a folder with no manifest, an unwritable destination and a
 * manifest that does not parse are all ordinary outcomes reported as diagnostics.
 */
class SModPackagePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModPackagePanel) {}

		SLATE_ARGUMENT(TSharedPtr<FModDeveloperModel>, Model)

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual ~SModPackagePanel() override;

	/**
	 * Points the packager at one mod folder, re-reads its manifest and restores the output directory
	 * remembered for it. Ignored while a packaging run is in flight.
	 */
	void SetSourceDirectory(const FString& InDirectory);

private:
	void HandleModelChanged();

	/** Re-reads the source folder's mod.json and refreshes everything derived from it. */
	void RefreshSourceManifest();

	/** Restores the remembered output directory for the current source, if there is one. */
	void RestoreRememberedOutput();

	FReply HandleBrowseSourceClicked();

	FReply HandleBrowseOutputClicked();

	FReply HandleUseSelectedModClicked();

	FReply HandlePackageClicked();

	FReply HandleRevealClicked();

	void HandleSourceTextCommitted(const FText& NewText, ETextCommit::Type CommitType);

	void HandleOutputTextCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/** Kicks the worker off. Assumes the inputs have already been checked. */
	void StartPackaging(const FString& InSource, const FString& InDestination);

	/** Game-thread landing point for a finished job. */
	void FinishPackaging(TSharedPtr<FModPackageJob> Job);

	/** The `.mod` file name this mod would be written as, e.g. "bettercombat-1.2.0.mod". */
	FString MakeOutputFileName() const;

	FText GetSourceSummaryText() const;

	FText GetOutputFileText() const;

	FText GetProgressText() const;

	FText GetResultText() const;

	bool IsPackageButtonEnabled() const;

	EVisibility GetProgressVisibility() const;

	EVisibility GetRevealVisibility() const;

	TSharedRef<ITableRow> HandleGenerateDiagnosticRow(TSharedPtr<FModDiagnostic> Item, const TSharedRef<STableViewBase>& OwnerTable);

	TSharedPtr<FModDeveloperModel> Model;

	TSharedPtr<SEditableTextBox> SourceTextBox;

	TSharedPtr<SEditableTextBox> OutputTextBox;

	TSharedPtr<SListView<TSharedPtr<FModDiagnostic>>> DiagnosticList;

	FString SourceDirectory;

	FString OutputDirectory;

	/** The manifest read from SourceDirectory, valid only when bSourceValid is true. */
	FModManifest SourceManifest;

	bool bSourceValid = false;

	/** What the manifest read had to say - shown even when it succeeded, so warnings are not hidden. */
	FString SourceStatus;

	/** True from the moment the worker is dispatched until it reports back. */
	bool bPackaging = false;

	/** Wall-clock start of the current run, for the "Packaging... (3.2s)" readout. */
	double PackageStartTime = 0.0;

	/** How many files the run is expected to write, counted before it starts. */
	int32 PendingFileCount = 0;

	/** Diagnostics from the last finished run. */
	TArray<TSharedPtr<FModDiagnostic>> ResultDiagnostics;

	/** Absolute path of the last `.mod` written, or empty when the last run failed or never ran. */
	FString LastPackagePath;

	bool bLastRunSucceeded = false;

	FText LastResultSummary;
};
