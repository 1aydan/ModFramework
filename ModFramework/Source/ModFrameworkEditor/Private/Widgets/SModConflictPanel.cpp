// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModConflictPanel.h"

#include "Conflicts/ModConflictDetector.h"
#include "Containers/UnrealString.h"
#include "HAL/Platform.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Input/Reply.h"
#include "Layout/Margin.h"
#include "ModDeveloperModel.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/ModDeveloperUI.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SModConflictPanel"

namespace ModConflictPanelPrivate
{
	static const FName ColBlocking(TEXT("Blocking"));
	static const FName ColPoint(TEXT("ExtensionPoint"));
	static const FName ColResource(TEXT("Resource"));
	static const FName ColContenders(TEXT("Contenders"));
	static const FName ColPolicy(TEXT("Policy"));
	static const FName ColWinner(TEXT("Winner"));

	FString JoinModIds(const TArray<FModId>& Ids)
	{
		TArray<FString> Names;
		Names.Reserve(Ids.Num());
		for (const FModId& Id : Ids)
		{
			Names.Add(Id.ToString());
		}

		return FString::Join(Names, TEXT(", "));
	}
}

/** One conflict row. Blocking conflicts are marked with an error icon so they read at a glance. */
class SModConflictRow : public SMultiColumnTableRow<TSharedPtr<FModConflict>>
{
public:
	SLATE_BEGIN_ARGS(SModConflictRow) {}
		SLATE_ARGUMENT(TSharedPtr<FModConflict>, Item)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
	{
		Item = InArgs._Item;
		SMultiColumnTableRow<TSharedPtr<FModConflict>>::Construct(FSuperRowType::FArguments(), InOwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		using namespace ModConflictPanelPrivate;

		if (!Item.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		if (ColumnName == ColBlocking)
		{
			return SNew(SBox)
				.WidthOverride(16.0f)
				.HeightOverride(16.0f)
				.ToolTipText(Item->bBlocking
					? LOCTEXT("BlockingTip", "Blocking: the applied policy is Error, so every contender is refused.")
					: LOCTEXT("ResolvedTip", "Resolved: the applied policy picked an outcome without refusing anyone."))
				[
					SNew(SImage)
					.Image(ModDeveloperUI::SeverityIcon(Item->bBlocking
						? EModDiagnosticSeverity::Error
						: EModDiagnosticSeverity::Warning))
				];
		}

		if (ColumnName == ColPoint)
		{
			return SNew(STextBlock)
				.Text(FText::FromName(Item->ExtensionPointId))
				.Font(ModDeveloperUI::MonospaceFont());
		}

		if (ColumnName == ColResource)
		{
			return SNew(STextBlock)
				.Text(FText::FromName(Item->ResourceId))
				.Font(ModDeveloperUI::MonospaceFont());
		}

		if (ColumnName == ColContenders)
		{
			return SNew(STextBlock)
				.Text(FText::FromString(ModConflictPanelPrivate::JoinModIds(Item->Contenders)))
				.ToolTipText(LOCTEXT("ContendersTip", "Every distinct mod claiming this resource, in load order then id."));
		}

		if (ColumnName == ColPolicy)
		{
			return SNew(STextBlock)
				.Text(ModDeveloperUI::ConflictPolicyText(Item->AppliedPolicy))
				.ColorAndOpacity(Item->bBlocking ? FStyleColors::Error : FStyleColors::Foreground);
		}

		if (ColumnName == ColWinner)
		{
			return SNew(STextBlock)
				.Text(Item->Winner.IsValid()
					? FText::FromString(Item->Winner.ToString())
					: LOCTEXT("NoWinner", "(none)"))
				.ToolTipText(Item->Winner.IsValid()
					? LOCTEXT("WinnerTip", "This mod's claim takes effect; the others are overridden.")
					: LOCTEXT("NoWinnerTip", "Error and Merge pick no winner: Error blocks everyone, Merge keeps everyone."));
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FModConflict> Item;
};

void SModConflictPanel::Construct(const FArguments& InArgs)
{
	using namespace ModConflictPanelPrivate;

	Model = InArgs._Model;
	OnModFocusRequested = InArgs._OnModFocusRequested;

	if (Model.IsValid())
	{
		Model->OnChanged.AddSP(this, &SModConflictPanel::HandleModelChanged);
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(this, &SModConflictPanel::GetSummaryText)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.ToolTipText(LOCTEXT("CopyConflictTip", "Copy the framework's own plain-text conflict report to the clipboard."))
				.OnClicked(this, &SModConflictPanel::HandleCopyClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("CopyConflict", "Copy report"))
				]
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(ListView, SListView<TSharedPtr<FModConflict>>)
			.ListItemsSource(&Entries)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SModConflictPanel::HandleGenerateRow)
			.OnSelectionChanged(this, &SModConflictPanel::HandleSelectionChanged)
			.HeaderRow
			(
				SNew(SHeaderRow)
				+ SHeaderRow::Column(ColBlocking)
					.DefaultLabel(FText::GetEmpty())
					.FixedWidth(26.0f)
				+ SHeaderRow::Column(ColPoint)
					.DefaultLabel(LOCTEXT("HeaderPoint", "Extension point"))
					.FillWidth(0.22f)
				+ SHeaderRow::Column(ColResource)
					.DefaultLabel(LOCTEXT("HeaderResource", "Resource"))
					.FillWidth(0.24f)
				+ SHeaderRow::Column(ColContenders)
					.DefaultLabel(LOCTEXT("HeaderContenders", "Contenders"))
					.FillWidth(0.28f)
				+ SHeaderRow::Column(ColPolicy)
					.DefaultLabel(LOCTEXT("HeaderPolicy", "Applied policy"))
					.FixedWidth(130.0f)
				+ SHeaderRow::Column(ColWinner)
					.DefaultLabel(LOCTEXT("HeaderWinner", "Winner"))
					.FillWidth(0.18f)
			)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.MaxDesiredHeight(180.0f)
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

	RebuildEntries();
	RebuildDetails();
}

SModConflictPanel::~SModConflictPanel()
{
	if (Model.IsValid())
	{
		Model->OnChanged.RemoveAll(this);
	}
}

void SModConflictPanel::HandleModelChanged()
{
	RebuildEntries();
	RebuildDetails();
}

void SModConflictPanel::RebuildEntries()
{
	Entries.Reset();
	SelectedIndex = INDEX_NONE;

	if (Model.IsValid())
	{
		for (const FModConflict& Conflict : Model->GetConflicts())
		{
			Entries.Add(MakeShared<FModConflict>(Conflict));
		}
	}

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SModConflictPanel::HandleGenerateRow(TSharedPtr<FModConflict> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SModConflictRow, OwnerTable)
		.Item(Item);
}

void SModConflictPanel::HandleSelectionChanged(TSharedPtr<FModConflict> Item, ESelectInfo::Type /*SelectInfo*/)
{
	SelectedIndex = INDEX_NONE;

	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (Entries[Index] == Item)
		{
			SelectedIndex = Index;
			break;
		}
	}

	RebuildDetails();
}

void SModConflictPanel::RebuildDetails()
{
	if (!DetailsBox.IsValid())
	{
		return;
	}

	DetailsBox->ClearChildren();

	if (!Entries.IsValidIndex(SelectedIndex) || !Entries[SelectedIndex].IsValid())
	{
		DetailsBox->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(Entries.Num() > 0
				? LOCTEXT("SelectConflict", "Select a conflict to read the framework's explanation of it.")
				: LOCTEXT("NoConflicts", "No resource is claimed by more than one mod."))
			.ColorAndOpacity(FStyleColors::Foreground)
		];
		return;
	}

	const FModConflict& Conflict = *Entries[SelectedIndex];

	DetailsBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 6.0f)
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Text(FText::FromString(Conflict.Explanation))
	];

	// One button per contender: reading a conflict almost always ends with wanting to look at one of
	// the mods involved, and hunting for it in the list by hand is the annoying part.
	TSharedRef<SHorizontalBox> ContenderRow = SNew(SHorizontalBox);
	ContenderRow->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(0.0f, 0.0f, 6.0f, 0.0f)
	[
		SNew(STextBlock)
		.Text(LOCTEXT("GoTo", "Go to mod:"))
	];

	for (const FModId& Contender : Conflict.Contenders)
	{
		const bool bIsWinner = Conflict.Winner.IsValid() && Contender == Conflict.Winner;
		const FModId ContenderId = Contender;
		FOnModFocusRequested FocusDelegate = OnModFocusRequested;

		ContenderRow->AddSlot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SNew(SButton)
			.ContentPadding(FMargin(6.0f, 2.0f))
			.ToolTipText(bIsWinner
				? LOCTEXT("WinnerButtonTip", "This mod's claim is the one that takes effect.")
				: LOCTEXT("ContenderButtonTip", "Show this mod on the Mods tab."))
			.OnClicked_Lambda([ContenderId, FocusDelegate]() -> FReply
			{
				FocusDelegate.ExecuteIfBound(ContenderId);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(Contender.ToString()))
				.ColorAndOpacity(bIsWinner ? FStyleColors::AccentGreen : FStyleColors::Foreground)
			]
		];
	}

	DetailsBox->AddSlot()
	.AutoHeight()
	[
		ContenderRow
	];

	if (Conflict.Losers.Num() > 0)
	{
		DetailsBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(FText::Format(
				LOCTEXT("Losers", "Overridden: {0}"),
				FText::FromString(ModConflictPanelPrivate::JoinModIds(Conflict.Losers))))
			.ColorAndOpacity(FStyleColors::Foreground)
		];
	}
}

FReply SModConflictPanel::HandleCopyClicked()
{
	TArray<FModConflict> Flattened;
	Flattened.Reserve(Entries.Num());
	for (const TSharedPtr<FModConflict>& Entry : Entries)
	{
		if (Entry.IsValid())
		{
			Flattened.Add(*Entry);
		}
	}

	// The same report the `Mod.Conflicts` console command prints, so a bug report pasted from here
	// and one pasted from the console are the same text.
	FPlatformApplicationMisc::ClipboardCopy(*FModConflictDetector::BuildReport(Flattened));
	return FReply::Handled();
}

FText SModConflictPanel::GetSummaryText() const
{
	int32 Blocking = 0;
	for (const TSharedPtr<FModConflict>& Entry : Entries)
	{
		if (Entry.IsValid() && Entry->bBlocking)
		{
			++Blocking;
		}
	}

	const bool bLive = Model.IsValid() && Model->IsLive();

	if (Entries.Num() == 0)
	{
		return bLive
			? LOCTEXT("SummaryNoneLive", "No mod conflicts detected in the running game.")
			: LOCTEXT("SummaryNoneOffline",
				"No conflicts among the claims mods declare in their manifests. Claims raised by "
				"extension objects only appear with a game running.");
	}

	return FText::Format(
		bLive
			? LOCTEXT("SummaryLive", "{0} contested resource(s), {1} of them blocking.")
			: LOCTEXT("SummaryOffline",
				"{0} contested resource(s), {1} of them blocking - from manifest-declared claims only. "
				"Claims raised by extension objects need a running game to appear."),
		FText::AsNumber(Entries.Num()),
		FText::AsNumber(Blocking));
}

#undef LOCTEXT_NAMESPACE
