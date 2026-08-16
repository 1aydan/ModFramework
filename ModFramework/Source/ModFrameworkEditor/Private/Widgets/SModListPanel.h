// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Internationalization/Text.h"
#include "Templates/SharedPointer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FDeferredCleanupSlateBrush;
class FModDeveloperModel;
class ITableRow;
class SSearchBox;
class STableViewBase;
class SVerticalBox;
struct FModDeveloperModRow;

/** One lifecycle button on a mod row. The panel maps each to the matching UModSubsystem call. */
enum class EModListAction : uint8
{
	Load,
	Unload,
	Activate,
	Deactivate,
	Reload,
	Enable,
	Disable
};

/**
 * The mod list: every mod the framework can see, with its icon, identity, state, load order and the
 * seven lifecycle actions a mod author actually reaches for.
 *
 * Lifecycle actions need a running game instance, because the lifecycle *is* the subsystem. With no
 * game instance the rows still list everything an offline scan found - which is the useful half when
 * you are editing manifests - and every action button is disabled with a tooltip saying why.
 *
 * Nothing here polls. The panel redraws when FModDeveloperModel broadcasts, and the model broadcasts
 * when UModSubsystem tells it to.
 */
class SModListPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModListPanel) {}

		/** The shared snapshot every panel of the window reads. Required. */
		SLATE_ARGUMENT(TSharedPtr<FModDeveloperModel>, Model)

		/** Raised when the user asks to package the selected mod; the window switches tabs. */
		SLATE_EVENT(FSimpleDelegate, OnPackageRequested)

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual ~SModListPanel() override;

	/** Selects, scrolls to and shows the details of one mod. Clears the filter if it hides the mod. */
	void SelectMod(const FModId& InId);

	/** The selected mod's id, or an invalid id when nothing is selected. */
	FModId GetSelectedModId() const;

	/** The selected row, or an invalid pointer. */
	TSharedPtr<FModDeveloperModRow> GetSelectedRow() const;

private:
	void HandleModelChanged();

	/** Re-applies the search filter and the sort to the model's rows. */
	void RebuildVisibleRows();

	/** Reloads the icon brushes of every visible mod. Called on refresh, never per paint. */
	void RebuildIconBrushes();

	TSharedRef<ITableRow> HandleGenerateRow(TSharedPtr<FModDeveloperModRow> Item, const TSharedRef<STableViewBase>& OwnerTable);

	void HandleSelectionChanged(TSharedPtr<FModDeveloperModRow> Item, ESelectInfo::Type SelectInfo);

	void HandleSearchTextChanged(const FText& NewText);

	/** Runs one lifecycle action against the live subsystem. Refuses cleanly when there is none. */
	void ExecuteAction(FModId ModId, EModListAction Action);

	FReply HandleOpenModFolderClicked();

	FReply HandlePackageSelectedClicked();

	FReply HandleRefreshClicked();

	EVisibility GetOfflineBannerVisibility() const;

	/** Regenerates the detail strip under the list from the current selection. */
	void RebuildDetails();

	TSharedPtr<FModDeveloperModel> Model;

	FSimpleDelegate OnPackageRequested;

	TSharedPtr<SListView<TSharedPtr<FModDeveloperModRow>>> ListView;

	TSharedPtr<SSearchBox> SearchBox;

	/** Rebuilt from the selected row rather than driven by attributes, so it can vary in shape. */
	TSharedPtr<SVerticalBox> DetailsBox;

	/** The rows the list is currently showing, filtered and sorted. */
	TArray<TSharedPtr<FModDeveloperModRow>> VisibleRows;

	/** One brush per mod that has a loadable icon. Rebuilt on refresh; rows hold their own reference. */
	TMap<FModId, TSharedPtr<FDeferredCleanupSlateBrush>> IconBrushes;

	FString FilterText;

	/** Kept across a refresh so a rebuild does not throw the user's selection away. */
	FModId SelectedId;
};
