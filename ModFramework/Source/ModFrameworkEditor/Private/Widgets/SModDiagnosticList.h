// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Input/Reply.h"
#include "Internationalization/Text.h"
#include "Layout/Visibility.h"
#include "Styling/SlateTypes.h"
#include "Templates/SharedPointer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class ITableRow;
class STableViewBase;

/**
 * A filterable list of FModDiagnostic, shared by every panel in the SDK window.
 *
 * Diagnostics are the framework's one structured output format, so the SDK generator, the public API
 * scanner and the runtime pipeline all produce them. Rendering them in one place means an error looks
 * the same wherever it came from, and the severity filter behaves the same too.
 *
 * Errors sort first, then warnings, then notes; within a severity the producer's order is preserved,
 * because that order is usually the order the work happened in.
 */
class SModDiagnosticList : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModDiagnosticList)
		: _EmptyMessage()
		, _ShowToolbar(true)
		, _MaxHeight(240.0f)
	{}
		/** Shown instead of the list when there is nothing to show. */
		SLATE_ARGUMENT(FText, EmptyMessage)

		/** Severity toggles and the copy button. Turn off for a list that is already inside a header. */
		SLATE_ARGUMENT(bool, ShowToolbar)

		/** The list scrolls past this height rather than growing without bound. */
		SLATE_ARGUMENT(float, MaxHeight)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Replaces the contents. Safe to call with an empty array. */
	void SetDiagnostics(const TArray<FModDiagnostic>& InDiagnostics);

	/** Drops everything, returning the widget to its empty message. */
	void ClearDiagnostics();

	/** How many entries carry this severity. */
	int32 CountBySeverity(EModDiagnosticSeverity InSeverity) const;

	/** Total entries held, whatever the current filter shows. */
	int32 Num() const { return AllItems.Num(); }

private:
	TSharedRef<ITableRow> HandleGenerateRow(TSharedPtr<FModDiagnostic> InItem, const TSharedRef<STableViewBase>& InOwnerTable);

	/** Re-applies the severity filter to AllItems and refreshes the list view. */
	void RebuildVisibleItems();

	ECheckBoxState HandleIsSeverityChecked(EModDiagnosticSeverity InSeverity) const;
	void HandleSeverityChanged(ECheckBoxState InNewState, EModDiagnosticSeverity InSeverity);

	/** Copies every *visible* entry to the clipboard, one FModDiagnostic::ToString() per line. */
	FReply HandleCopyClicked();

	EVisibility GetListVisibility() const;
	EVisibility GetEmptyMessageVisibility() const;
	FText GetSummaryText() const;
	FText GetSeverityFilterLabel(EModDiagnosticSeverity InSeverity) const;

	/** Everything handed to SetDiagnostics, already sorted by severity. */
	TArray<TSharedPtr<FModDiagnostic>> AllItems;

	/** The subset the filter currently admits. Bound as the list view's item source. */
	TArray<TSharedPtr<FModDiagnostic>> VisibleItems;

	TSharedPtr<SListView<TSharedPtr<FModDiagnostic>>> ListView;

	FText EmptyMessage;

	bool bShowErrors = true;
	bool bShowWarnings = true;
	bool bShowInfos = true;
};
