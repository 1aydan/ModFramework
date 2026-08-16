// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Templates/SharedPointer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

class FModDeveloperModel;
class SWidget;
struct FModDeveloperModRow;

/**
 * The dependency view for one mod: what it needs, what needs it, how each edge resolved, and what
 * the resolver finally decided about it.
 *
 * Deliberately a text tree rather than a node graph. A dependency problem is read, not navigated -
 * "uiframework ^2.0.0 -> installed 1.9.4, does not satisfy" is the whole answer, and it is one line.
 * A graph widget would cost a lot of code to say less.
 *
 * The transitive tree is cycle-safe: a mod already on the current path is printed as a cycle marker
 * and not descended into, and depth is capped, so a hostile manifest cannot make this recurse
 * forever.
 */
class SModDependencyPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModDependencyPanel) {}

		SLATE_ARGUMENT(TSharedPtr<FModDeveloperModel>, Model)

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual ~SModDependencyPanel() override;

	/** Points the view at one mod. Ignores an id the model does not know. */
	void SetSelectedMod(const FModId& InId);

private:
	void HandleModelChanged();

	/** Rebuilds the mod picker's options, keeping the current selection when it still exists. */
	void RebuildOptions();

	/** Regenerates the report text for the current selection. */
	void RebuildReport();

	TSharedRef<SWidget> HandleGenerateComboEntry(TSharedPtr<FModDeveloperModRow> Item);

	void HandleComboSelectionChanged(TSharedPtr<FModDeveloperModRow> Item, ESelectInfo::Type SelectInfo);

	FText GetComboLabel() const;

	FText GetReportText() const;

	FReply HandleCopyClicked();

	TSharedPtr<FModDeveloperModel> Model;

	TSharedPtr<SComboBox<TSharedPtr<FModDeveloperModRow>>> ModCombo;

	TArray<TSharedPtr<FModDeveloperModRow>> Options;

	FModId SelectedId;

	FText ReportText;
};
