// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Templates/SharedPointer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SModValidationPanel.h"
#include "Widgets/Views/SListView.h"

class FModDeveloperModel;
class ITableRow;
class STableViewBase;
class SVerticalBox;

/**
 * The conflict report: every resource two or more mods claim, the policy that was applied to it, who
 * won, and whether the conflict blocks the mods involved from loading at all.
 *
 * With a game running this is FModConflictDetector's own output as the subsystem recorded it during
 * the last refresh, extension-raised claims included. Offline it is the same detector run over the
 * claims mods declare in their manifests - which is most of them, but not the ones an extension
 * object raises at registration time, because those need the mods loaded. The panel says which of
 * the two it is showing rather than letting a mod author guess.
 */
class SModConflictPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModConflictPanel) {}

		SLATE_ARGUMENT(TSharedPtr<FModDeveloperModel>, Model)

		SLATE_EVENT(FOnModFocusRequested, OnModFocusRequested)

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual ~SModConflictPanel() override;

private:
	void HandleModelChanged();

	void RebuildEntries();

	void RebuildDetails();

	TSharedRef<ITableRow> HandleGenerateRow(TSharedPtr<FModConflict> Item, const TSharedRef<STableViewBase>& OwnerTable);

	void HandleSelectionChanged(TSharedPtr<FModConflict> Item, ESelectInfo::Type SelectInfo);

	FReply HandleCopyClicked();

	FText GetSummaryText() const;

	TSharedPtr<FModDeveloperModel> Model;

	FOnModFocusRequested OnModFocusRequested;

	TSharedPtr<SListView<TSharedPtr<FModConflict>>> ListView;

	TSharedPtr<SVerticalBox> DetailsBox;

	/** One entry per conflict, copied out of the model so the list owns stable pointers. */
	TArray<TSharedPtr<FModConflict>> Entries;

	int32 SelectedIndex = INDEX_NONE;
};
