// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModListPanel.h"

#include "Content/ModIconCache.h"
#include "Engine/Texture2D.h"
#include "HAL/Platform.h"
#include "HAL/PlatformProcess.h"
#include "Input/Reply.h"
#include "Layout/Margin.h"
#include "Layout/Visibility.h"
#include "Manifest/ModManifest.h"
#include "ModDeveloperModel.h"
#include "ModFrameworkEditorModule.h"
#include "Slate/DeferredCleanupSlateBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/StyleColors.h"
#include "Subsystem/ModSubsystem.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/ModDeveloperUI.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SModListPanel"

DECLARE_DELEGATE_TwoParams(FOnModListAction, FModId /*ModId*/, EModListAction /*Action*/);

namespace ModListPanelPrivate
{
	// Deliberately not named ColumnName/ColumnState/...: GenerateWidgetForColumn takes a parameter
	// called ColumnName, and a namespace constant of the same name would be shadowed inside it.
	static const FName ColIcon(TEXT("Icon"));
	static const FName ColName(TEXT("Name"));
	static const FName ColId(TEXT("Id"));
	static const FName ColVersion(TEXT("Version"));
	static const FName ColState(TEXT("State"));
	static const FName ColOrder(TEXT("Order"));
	static const FName ColActions(TEXT("Actions"));

	/**
	 * Mirrors FModInfo::IsLoaded for a state we hold as a plain value.
	 *
	 * Deliberately still true for Deactivated: a deactivated mod is loaded but idle, and unloading it
	 * is a separate step. Keeping this identical to the runtime definition is what stops the buttons
	 * from disagreeing with the subsystem about what is possible.
	 */
	bool IsLoadedState(EModState State)
	{
		return static_cast<uint8>(State) >= static_cast<uint8>(EModState::Loaded)
			&& State != EModState::Unmounted
			&& State != EModState::Failed
			&& State != EModState::Disabled;
	}

	/** Whether one action is legal for a row right now, independent of whether a game is running. */
	bool IsActionApplicable(const FModDeveloperModRow& Row, EModListAction Action)
	{
		switch (Action)
		{
		case EModListAction::Load:
			return !IsLoadedState(Row.State)
				&& (Row.State == EModState::DependenciesResolved
					|| Row.State == EModState::Mounted
					|| Row.State == EModState::Unmounted);

		case EModListAction::Unload:
		case EModListAction::Reload:
			return IsLoadedState(Row.State);

		case EModListAction::Activate:
			return Row.State == EModState::Loaded || Row.State == EModState::Deactivated;

		case EModListAction::Deactivate:
			return Row.State == EModState::Activated;

		case EModListAction::Enable:
			return !Row.bEnabled;

		case EModListAction::Disable:
			return Row.bEnabled;

		default:
			return false;
		}
	}

	/** Sorts by load order ascending with unordered mods last, then by display name, then by id. */
	bool RowSortPredicate(const TSharedPtr<FModDeveloperModRow>& A, const TSharedPtr<FModDeveloperModRow>& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return B.IsValid();
		}

		const int32 OrderA = (A->LoadOrder == INDEX_NONE) ? MAX_int32 : A->LoadOrder;
		const int32 OrderB = (B->LoadOrder == INDEX_NONE) ? MAX_int32 : B->LoadOrder;
		if (OrderA != OrderB)
		{
			return OrderA < OrderB;
		}

		const int32 NameCompare = A->DisplayName.Compare(B->DisplayName, ESearchCase::IgnoreCase);
		if (NameCompare != 0)
		{
			return NameCompare < 0;
		}

		return A->Id < B->Id;
	}

	bool MatchesFilter(const FModDeveloperModRow& Row, const FString& Filter)
	{
		if (Filter.IsEmpty())
		{
			return true;
		}

		return Row.DisplayName.Contains(Filter)
			|| Row.Id.ToString().Contains(Filter)
			|| Row.Author.Contains(Filter);
	}
}

/**
 * One row of the mod list.
 *
 * The row owns a shared reference to its icon brush rather than a raw pointer into the panel's map,
 * so a refresh that rebuilds those brushes cannot leave a live row pointing at a freed one.
 */
class SModListRow : public SMultiColumnTableRow<TSharedPtr<FModDeveloperModRow>>
{
public:
	SLATE_BEGIN_ARGS(SModListRow)
		: _LifecycleAvailable(false)
		{}
		SLATE_ARGUMENT(TSharedPtr<FModDeveloperModRow>, Item)
		SLATE_ARGUMENT(TSharedPtr<FDeferredCleanupSlateBrush>, IconBrush)
		SLATE_ARGUMENT(bool, LifecycleAvailable)
		SLATE_EVENT(FOnModListAction, OnAction)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
	{
		Item = InArgs._Item;
		IconBrush = InArgs._IconBrush;
		bLifecycleAvailable = InArgs._LifecycleAvailable;
		OnAction = InArgs._OnAction;

		SMultiColumnTableRow<TSharedPtr<FModDeveloperModRow>>::Construct(FSuperRowType::FArguments(), InOwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		using namespace ModListPanelPrivate;

		if (!Item.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		if (ColumnName == ColIcon)
		{
			return SNew(SBox)
				.WidthOverride(28.0f)
				.HeightOverride(28.0f)
				.Padding(2.0f)
				[
					SNew(SImage)
					.Image(GetIconBrush())
				];
		}

		if (ColumnName == ColName)
		{
			return SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->DisplayName))
					.Font(FAppStyle::GetFontStyle(TEXT("PropertyWindow.BoldFont")))
					.ToolTipText(FText::FromString(Item->RootPath))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(Item->Author.IsEmpty()
						? LOCTEXT("UnknownAuthor", "unknown author")
						: FText::FromString(Item->Author))
					.ColorAndOpacity(FStyleColors::Foreground)
					.TextStyle(FAppStyle::Get(), TEXT("SmallText"))
				];
		}

		if (ColumnName == ColId)
		{
			return SNew(STextBlock)
				.Text(FText::FromString(Item->Id.ToString()))
				.ToolTipText(LOCTEXT("IdTooltip", "The stable machine id. This is what other mods depend on."));
		}

		if (ColumnName == ColVersion)
		{
			return SNew(STextBlock)
				.Text(FText::FromString(Item->VersionText));
		}

		if (ColumnName == ColState)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.Text(ModDeveloperUI::StateText(Item->State))
					.ColorAndOpacity(ModDeveloperUI::StateColor(Item->State))
					.ToolTipText(MakeStateTooltip())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Visibility(Item->bEnabled ? EVisibility::Collapsed : EVisibility::Visible)
					.Text(LOCTEXT("DisabledSuffix", "(off)"))
					.ColorAndOpacity(FStyleColors::Warning)
				];
		}

		if (ColumnName == ColOrder)
		{
			return SNew(STextBlock)
				.Text(Item->LoadOrder == INDEX_NONE
					? LOCTEXT("NoOrder", "-")
					: FText::AsNumber(Item->LoadOrder))
				.ToolTipText(LOCTEXT("OrderTooltip", "Position in the resolved load order. '-' means the mod is not in it."));
		}

		if (ColumnName == ColActions)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
				[
					MakeActionButton(EModListAction::Load, LOCTEXT("ActionLoad", "Load"),
						LOCTEXT("ActionLoadTip", "Mount the mod's content if needed, then give it its context and entry point. This does not start it."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
				[
					MakeActionButton(EModListAction::Unload, LOCTEXT("ActionUnload", "Unload"),
						LOCTEXT("ActionUnloadTip", "Deactivate if needed, release everything the mod created and unmount its content."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
				[
					MakeActionButton(EModListAction::Activate, LOCTEXT("ActionActivate", "Activate"),
						LOCTEXT("ActionActivateTip", "Apply the mod's content bundles and switch its extensions on."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
				[
					MakeActionButton(EModListAction::Deactivate, LOCTEXT("ActionDeactivate", "Deactivate"),
						LOCTEXT("ActionDeactivateTip", "Switch the mod's extensions off. It stays loaded, so reactivating is cheap."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
				[
					MakeActionButton(EModListAction::Reload, LOCTEXT("ActionReload", "Reload"),
						LOCTEXT("ActionReloadTip", "Unload the mod and bring it straight back, restoring whether it was active."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 1.0f, 0.0f)
				[
					MakeActionButton(EModListAction::Enable, LOCTEXT("ActionEnable", "Enable"),
						LOCTEXT("ActionEnableTip", "Switch the mod on for this session. It is not written to DefaultGame.ini."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(1.0f, 0.0f)
				[
					MakeActionButton(EModListAction::Disable, LOCTEXT("ActionDisable", "Disable"),
						LOCTEXT("ActionDisableTip", "Switch the mod off for this session, tearing it down first if it is running."))
				];
		}

		return SNullWidget::NullWidget;
	}

private:
	const FSlateBrush* GetIconBrush() const
	{
		if (IconBrush.IsValid())
		{
			return IconBrush->GetSlateBrush();
		}

		return FAppStyle::GetBrush(TEXT("Icons.Package"));
	}

	FText MakeStateTooltip() const
	{
		if (!Item.IsValid())
		{
			return FText::GetEmpty();
		}

		if (Item->State == EModState::Failed && !Item->FailureMessage.IsEmpty())
		{
			return FText::Format(
				LOCTEXT("FailedTooltip", "{0}: {1}"),
				ModDeveloperUI::FailureReasonText(Item->FailureReason),
				FText::FromString(Item->FailureMessage));
		}

		return ModDeveloperUI::StateTooltip(Item->State);
	}

	TSharedRef<SWidget> MakeActionButton(EModListAction Action, const FText& Label, const FText& Tooltip)
	{
		const bool bApplicable = Item.IsValid() && ModListPanelPrivate::IsActionApplicable(*Item, Action);
		const bool bEnabled = bApplicable && bLifecycleAvailable;

		FText EffectiveTooltip = Tooltip;
		if (!bLifecycleAvailable)
		{
			EffectiveTooltip = LOCTEXT("NoGameInstanceTip",
				"No game instance is running. Start Play In Editor to drive a mod's lifecycle.");
		}
		else if (!bApplicable && Item.IsValid())
		{
			EffectiveTooltip = FText::Format(
				LOCTEXT("NotApplicableTip", "Not possible while the mod is {0}."),
				ModDeveloperUI::StateText(Item->State));
		}

		const FModId ModId = Item.IsValid() ? Item->Id : FModId();
		FOnModListAction ActionDelegate = OnAction;

		return SNew(SButton)
			.ContentPadding(FMargin(6.0f, 2.0f))
			.IsEnabled(bEnabled)
			.ToolTipText(EffectiveTooltip)
			.OnClicked_Lambda([ModId, Action, ActionDelegate]() -> FReply
			{
				ActionDelegate.ExecuteIfBound(ModId, Action);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(Label)
				.TextStyle(FAppStyle::Get(), TEXT("SmallText"))
			];
	}

	TSharedPtr<FModDeveloperModRow> Item;
	TSharedPtr<FDeferredCleanupSlateBrush> IconBrush;
	FOnModListAction OnAction;
	bool bLifecycleAvailable = false;
};

void SModListPanel::Construct(const FArguments& InArgs)
{
	using namespace ModListPanelPrivate;

	Model = InArgs._Model;
	OnPackageRequested = InArgs._OnPackageRequested;

	if (Model.IsValid())
	{
		Model->OnChanged.AddSP(this, &SModListPanel::HandleModelChanged);
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Offline banner --------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SBorder)
			.Visibility(this, &SModListPanel::GetOfflineBannerVisibility)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.LightGroupBorder")))
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(TEXT("Icons.Info")))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("OfflineBanner",
						"No game instance is running, so this is an offline scan of the configured mod "
						"directories. Manifests, dependencies and declared conflicts are all real; live "
						"state and the lifecycle buttons need Play In Editor."))
				]
			]
		]

		// --- Toolbar ---------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SAssignNew(SearchBox, SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Filter by name, id or author"))
				.OnTextChanged(this, &SModListPanel::HandleSearchTextChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.ToolTipText(LOCTEXT("RefreshTip", "Rescan and rebuild everything this window shows."))
				.OnClicked(this, &SModListPanel::HandleRefreshClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("Refresh", "Refresh"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.ToolTipText(LOCTEXT("PackageSelectedTip", "Open the Package tab with this mod's folder already filled in."))
				.OnClicked(this, &SModListPanel::HandlePackageSelectedClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("PackageSelected", "Package..."))
				]
			]
		]

		// --- List ------------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(ListView, SListView<TSharedPtr<FModDeveloperModRow>>)
			.ListItemsSource(&VisibleRows)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SModListPanel::HandleGenerateRow)
			.OnSelectionChanged(this, &SModListPanel::HandleSelectionChanged)
			.HeaderRow
			(
				SNew(SHeaderRow)
				+ SHeaderRow::Column(ColIcon)
					.DefaultLabel(FText::GetEmpty())
					.FixedWidth(34.0f)
				+ SHeaderRow::Column(ColName)
					.DefaultLabel(LOCTEXT("HeaderName", "Mod"))
					.FillWidth(0.30f)
				+ SHeaderRow::Column(ColId)
					.DefaultLabel(LOCTEXT("HeaderId", "Id"))
					.FillWidth(0.22f)
				+ SHeaderRow::Column(ColVersion)
					.DefaultLabel(LOCTEXT("HeaderVersion", "Version"))
					.FixedWidth(96.0f)
				+ SHeaderRow::Column(ColState)
					.DefaultLabel(LOCTEXT("HeaderState", "State"))
					.FixedWidth(150.0f)
				+ SHeaderRow::Column(ColOrder)
					.DefaultLabel(LOCTEXT("HeaderOrder", "Order"))
					.FixedWidth(54.0f)
				+ SHeaderRow::Column(ColActions)
					.DefaultLabel(LOCTEXT("HeaderActions", "Actions"))
					.FixedWidth(430.0f)
			)
		]

		// --- Details ---------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.MaxDesiredHeight(210.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
				.Padding(8.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(DetailsBox, SVerticalBox)
					]
				]
			]
		]
	];

	RebuildIconBrushes();
	RebuildVisibleRows();
	RebuildDetails();
}

SModListPanel::~SModListPanel()
{
	if (Model.IsValid())
	{
		Model->OnChanged.RemoveAll(this);
	}
}

void SModListPanel::HandleModelChanged()
{
	RebuildIconBrushes();
	RebuildVisibleRows();
	RebuildDetails();
}

void SModListPanel::RebuildVisibleRows()
{
	VisibleRows.Reset();

	if (Model.IsValid())
	{
		for (const TSharedPtr<FModDeveloperModRow>& Row : Model->GetMods())
		{
			if (Row.IsValid() && ModListPanelPrivate::MatchesFilter(*Row, FilterText))
			{
				VisibleRows.Add(Row);
			}
		}
	}

	// Every entry was null-checked on the way in, so the predicate can never be handed an empty
	// pointer. TSharedPtr is not one of the types TDereferenceWrapper unwraps, so the predicate sees
	// the pointers themselves rather than the pointees - but the check stays for the same reason.
	// The lambda is deliberate: TArray::Sort deduces its predicate as `const PREDICATE_CLASS&`, and a
	// class type is a less exotic deduction than a bare function type.
	VisibleRows.Sort([](const TSharedPtr<FModDeveloperModRow>& A, const TSharedPtr<FModDeveloperModRow>& B)
	{
		return ModListPanelPrivate::RowSortPredicate(A, B);
	});

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();

		// A rebuild replaces every row object, so the previous selection has to be re-applied by id.
		if (SelectedId.IsValid())
		{
			for (const TSharedPtr<FModDeveloperModRow>& Row : VisibleRows)
			{
				if (Row.IsValid() && Row->Id == SelectedId)
				{
					ListView->SetSelection(Row, ESelectInfo::Direct);
					break;
				}
			}
		}
	}
}

void SModListPanel::RebuildIconBrushes()
{
	IconBrushes.Reset();

	if (!Model.IsValid())
	{
		return;
	}

	UModSubsystem* Subsystem = Model->GetSubsystem();
	if (Subsystem == nullptr)
	{
		return;
	}

	UModIconCache* IconCache = Subsystem->GetIconCache();
	if (IconCache == nullptr)
	{
		return;
	}

	for (const TSharedPtr<FModDeveloperModRow>& Row : Model->GetMods())
	{
		if (!Row.IsValid() || !Row->Id.IsValid())
		{
			continue;
		}

		if (!IconCache->HasIcon(Row->Id))
		{
			continue;
		}

		// LoadIconSynchronous is documented as editor/tooling only and blocks. This runs once per
		// refresh, not per paint, and the cache short-circuits every call after the first.
		FModDiagnostic IconError;
		UTexture2D* Texture = IconCache->LoadIconSynchronous(Row->Id, IconError);
		if (Texture == nullptr)
		{
			if (!IconError.Code.IsNone())
			{
				UE_LOG(LogModFrameworkEditor, Verbose, TEXT("Mod icon unavailable for '%s': %s"),
					*Row->Id.ToString(), *IconError.ToString());
			}
			continue;
		}

		IconBrushes.Add(Row->Id, FDeferredCleanupSlateBrush::CreateBrush(Texture));
	}
}

TSharedRef<ITableRow> SModListPanel::HandleGenerateRow(TSharedPtr<FModDeveloperModRow> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	TSharedPtr<FDeferredCleanupSlateBrush> Brush;
	if (Item.IsValid())
	{
		if (const TSharedPtr<FDeferredCleanupSlateBrush>* Found = IconBrushes.Find(Item->Id))
		{
			Brush = *Found;
		}
	}

	const bool bLifecycleAvailable = Model.IsValid() && Model->GetSubsystem() != nullptr;

	return SNew(SModListRow, OwnerTable)
		.Item(Item)
		.IconBrush(Brush)
		.LifecycleAvailable(bLifecycleAvailable)
		.OnAction(FOnModListAction::CreateSP(this, &SModListPanel::ExecuteAction));
}

void SModListPanel::HandleSelectionChanged(TSharedPtr<FModDeveloperModRow> Item, ESelectInfo::Type /*SelectInfo*/)
{
	SelectedId = Item.IsValid() ? Item->Id : FModId();
	RebuildDetails();
}

void SModListPanel::HandleSearchTextChanged(const FText& NewText)
{
	FilterText = NewText.ToString().TrimStartAndEnd();
	RebuildVisibleRows();
}

FReply SModListPanel::HandleRefreshClicked()
{
	if (Model.IsValid())
	{
		Model->Refresh();
	}

	return FReply::Handled();
}

FReply SModListPanel::HandlePackageSelectedClicked()
{
	OnPackageRequested.ExecuteIfBound();
	return FReply::Handled();
}

FReply SModListPanel::HandleOpenModFolderClicked()
{
	const TSharedPtr<FModDeveloperModRow> Row = GetSelectedRow();
	if (!Row.IsValid() || Row->RootPath.IsEmpty())
	{
		return FReply::Handled();
	}

	// ExploreFolder selects the item when it is a file and opens it when it is a directory, which is
	// exactly right for a mod root that may be either a folder or a `.mod` container.
	FPlatformProcess::ExploreFolder(*Row->RootPath);
	return FReply::Handled();
}

EVisibility SModListPanel::GetOfflineBannerVisibility() const
{
	return (Model.IsValid() && Model->IsLive()) ? EVisibility::Collapsed : EVisibility::Visible;
}

void SModListPanel::SelectMod(const FModId& InId)
{
	if (!InId.IsValid() || !ListView.IsValid())
	{
		return;
	}

	// A filter that hides the mod would make "focus this mod" silently do nothing, so clear it.
	bool bPresent = false;
	for (const TSharedPtr<FModDeveloperModRow>& Row : VisibleRows)
	{
		if (Row.IsValid() && Row->Id == InId)
		{
			bPresent = true;
			break;
		}
	}

	if (!bPresent && !FilterText.IsEmpty())
	{
		FilterText.Reset();
		if (SearchBox.IsValid())
		{
			SearchBox->SetText(FText::GetEmpty());
		}
		RebuildVisibleRows();
	}

	for (const TSharedPtr<FModDeveloperModRow>& Row : VisibleRows)
	{
		if (Row.IsValid() && Row->Id == InId)
		{
			SelectedId = InId;
			ListView->SetSelection(Row, ESelectInfo::Direct);
			ListView->RequestScrollIntoView(Row);
			RebuildDetails();
			return;
		}
	}
}

FModId SModListPanel::GetSelectedModId() const
{
	return SelectedId;
}

TSharedPtr<FModDeveloperModRow> SModListPanel::GetSelectedRow() const
{
	if (!Model.IsValid())
	{
		return nullptr;
	}

	return Model->FindMod(SelectedId);
}

void SModListPanel::ExecuteAction(FModId ModId, EModListAction Action)
{
	if (!Model.IsValid() || !ModId.IsValid())
	{
		return;
	}

	UModSubsystem* Subsystem = Model->GetSubsystem();
	if (Subsystem == nullptr)
	{
		// The buttons are disabled without a subsystem, so this only happens when PIE ended between
		// the paint and the click. Refusing quietly is the right answer; nothing has been touched.
		UE_LOG(LogModFrameworkEditor, Log,
			TEXT("Mod action ignored for '%s': the game instance went away before the click was handled."),
			*ModId.ToString());
		Model->RequestRefresh();
		return;
	}

	switch (Action)
	{
	case EModListAction::Load:
	{
		// "Load" in the UI means "get this mod running as far as Loaded". Mounting is a separate
		// lifecycle step the subsystem requires first, and a mod author should not have to know that.
		const TSharedPtr<FModDeveloperModRow> Row = Model->FindMod(ModId);
		if (Row.IsValid() && Row->State != EModState::Mounted && !Subsystem->MountMod(ModId))
		{
			break;
		}

		Subsystem->LoadMod(ModId);
		break;
	}

	case EModListAction::Unload:
		Subsystem->UnloadMod(ModId);
		break;

	case EModListAction::Activate:
		Subsystem->ActivateMod(ModId);
		break;

	case EModListAction::Deactivate:
		Subsystem->DeactivateMod(ModId);
		break;

	case EModListAction::Reload:
		Subsystem->ReloadMod(ModId);
		break;

	case EModListAction::Enable:
		Subsystem->SetModEnabled(ModId, true);
		break;

	case EModListAction::Disable:
		Subsystem->SetModEnabled(ModId, false);
		break;

	default:
		break;
	}

	// Every one of those refuses illegal requests with false and a log line rather than asserting, so
	// there is nothing to handle here beyond bringing the window back in step.
	Model->RequestRefresh();
}

void SModListPanel::RebuildDetails()
{
	if (!DetailsBox.IsValid())
	{
		return;
	}

	DetailsBox->ClearChildren();

	const TSharedPtr<FModDeveloperModRow> Row = GetSelectedRow();
	if (!Row.IsValid())
	{
		DetailsBox->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NoSelection", "Select a mod to see its manifest details."))
			.ColorAndOpacity(FStyleColors::Foreground)
		];
		return;
	}

	auto AddField = [this](const FText& Label, const FText& Value)
	{
		if (Value.IsEmpty())
		{
			return;
		}

		DetailsBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 1.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(150.0f)
				[
					SNew(STextBlock)
					.Text(Label)
					.ColorAndOpacity(FStyleColors::Foreground)
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(Value)
				.AutoWrapText(true)
			]
		];
	};

	DetailsBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 4.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Row->DisplayName))
			.Font(FAppStyle::GetFontStyle(TEXT("PropertyWindow.BoldFont")))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SButton)
			.ContentPadding(FMargin(6.0f, 2.0f))
			.ToolTipText(LOCTEXT("OpenFolderTip", "Show the mod's folder (or its .mod file) in the file explorer."))
			.OnClicked(this, &SModListPanel::HandleOpenModFolderClicked)
			[
				SNew(STextBlock).Text(LOCTEXT("OpenFolder", "Show in Explorer"))
			]
		]
	];

	AddField(LOCTEXT("FieldId", "Id"), FText::FromString(Row->Id.ToString()));
	AddField(LOCTEXT("FieldVersion", "Version"), FText::FromString(Row->VersionText));
	AddField(LOCTEXT("FieldAuthor", "Author"), FText::FromString(Row->Author));
	AddField(LOCTEXT("FieldDescription", "Description"), FText::FromString(Row->Manifest.Description));
	AddField(LOCTEXT("FieldHomepage", "Homepage"), FText::FromString(Row->Manifest.Homepage));
	AddField(LOCTEXT("FieldLicense", "License"), FText::FromString(Row->Manifest.License));
	AddField(LOCTEXT("FieldGame", "Requires game"), FText::FromString(
		Row->Manifest.Game.GameId + TEXT(" ") + Row->Manifest.Game.VersionRange.ToString()));
	AddField(LOCTEXT("FieldRoot", "Root path"), FText::FromString(Row->RootPath));
	AddField(LOCTEXT("FieldState", "State"), ModDeveloperUI::StateText(Row->State));

	if (Row->State == EModState::Failed || Row->FailureReason != EModLoadFailureReason::None)
	{
		AddField(LOCTEXT("FieldFailure", "Failure"), FText::Format(
			LOCTEXT("FailureValue", "{0}: {1}"),
			ModDeveloperUI::FailureReasonText(Row->FailureReason),
			FText::FromString(Row->FailureMessage)));
	}

	if (Row->Manifest.RequestedPermissions.Num() > 0)
	{
		TArray<FString> PermissionNames;
		PermissionNames.Reserve(Row->Manifest.RequestedPermissions.Num());
		for (const FName& Permission : Row->Manifest.RequestedPermissions)
		{
			PermissionNames.Add(Permission.ToString());
		}

		AddField(LOCTEXT("FieldPermissions", "Requests permissions"),
			FText::FromString(FString::Join(PermissionNames, TEXT(", "))));
	}

	if (Row->Manifest.ContentRoots.Num() > 0)
	{
		TArray<FString> RootDescriptions;
		RootDescriptions.Reserve(Row->Manifest.ContentRoots.Num());
		for (const FModContentRoot& ContentRoot : Row->Manifest.ContentRoots)
		{
			RootDescriptions.Add(FString::Printf(TEXT("%s (%s -> %s)"),
				*ContentRoot.RelativePath,
				*ModFrameworkEnums::ToString(ContentRoot.Type),
				*ContentRoot.MountPoint));
		}

		AddField(LOCTEXT("FieldContent", "Content roots"),
			FText::FromString(FString::Join(RootDescriptions, TEXT("\n"))));
	}

	if (!Row->Manifest.EntryPoint.EntryClass.ToString().IsEmpty())
	{
		AddField(LOCTEXT("FieldEntry", "Entry point"),
			FText::FromString(Row->Manifest.EntryPoint.EntryClass.ToString()));
	}

	AddField(LOCTEXT("FieldDiagnostics", "Diagnostics"),
		ModDeveloperUI::SummariseDiagnostics(Row->Diagnostics));
}

#undef LOCTEXT_NAMESPACE
