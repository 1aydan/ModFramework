// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Internationalization/Text.h"
#include "Templates/SharedPointer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FModDeveloperModel;
class ITableRow;
class SSearchBox;
class STableViewBase;

/** Asks the window to bring one mod into focus on the Mods tab. */
DECLARE_DELEGATE_OneParam(FOnModFocusRequested, FModId /*ModId*/);

/** One line of the validation report: a diagnostic plus the mod it belongs to. */
struct FModValidationEntry
{
	FModDiagnostic Diagnostic;

	/** The owning mod. Invalid for a pipeline diagnostic that belongs to no particular mod. */
	FModId ModId;

	/** Display name of the owning mod, or a label for the pipeline itself. */
	FString ModDisplayName;
};

/**
 * The validation report: every diagnostic the framework has about the current mod set, re-derived
 * from the manifests on disk rather than read out of a cache.
 *
 * "Re-run" re-parses each unpacked mod's mod.json through FModManifestParser and re-validates each
 * packaged mod's embedded manifest, so a mod author can edit a manifest in a text editor, press one
 * button and see the result - without restarting the editor and without a game running.
 *
 * Clicking a row focuses the offending mod on the Mods tab.
 */
class SModValidationPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModValidationPanel) {}

		SLATE_ARGUMENT(TSharedPtr<FModDeveloperModel>, Model)

		SLATE_EVENT(FOnModFocusRequested, OnModFocusRequested)

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual ~SModValidationPanel() override;

	/** Re-parses and re-validates every mod, then rebuilds the list. */
	void RunValidation();

private:
	void HandleModelChanged();

	void RebuildVisibleEntries();

	TSharedRef<ITableRow> HandleGenerateRow(TSharedPtr<FModValidationEntry> Item, const TSharedRef<STableViewBase>& OwnerTable);

	void HandleSelectionChanged(TSharedPtr<FModValidationEntry> Item, ESelectInfo::Type SelectInfo);

	void HandleSearchTextChanged(const FText& NewText);

	FReply HandleRevalidateClicked();

	FReply HandleCopyClicked();

	ECheckBoxState GetSeverityFilterState(EModDiagnosticSeverity Severity) const;

	void HandleSeverityFilterChanged(ECheckBoxState NewState, EModDiagnosticSeverity Severity);

	FText GetSummaryText() const;

	TSharedPtr<FModDeveloperModel> Model;

	FOnModFocusRequested OnModFocusRequested;

	TSharedPtr<SListView<TSharedPtr<FModValidationEntry>>> ListView;

	TSharedPtr<SSearchBox> SearchBox;

	/** Everything the last validation pass produced, unfiltered. */
	TArray<TSharedPtr<FModValidationEntry>> AllEntries;

	/** What the list is showing after the severity and text filters. */
	TArray<TSharedPtr<FModValidationEntry>> VisibleEntries;

	bool bShowErrors = true;
	bool bShowWarnings = true;
	bool bShowInfo = true;

	FString FilterText;
};
