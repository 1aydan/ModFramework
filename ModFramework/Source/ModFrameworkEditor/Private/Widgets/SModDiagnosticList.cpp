// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModDiagnosticList.h"

#include "Widgets/ModSDKWidgetUtils.h"

#include "Algo/StableSort.h"
#include "Containers/UnrealString.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Internationalization/Internationalization.h"
#include "Layout/Margin.h"
#include "Misc/Attribute.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SModDiagnosticList"

namespace ModDiagnosticListPrivate
{
	/** Errors first, then warnings, then notes. Higher number sorts earlier. */
	int32 SeverityRank(EModDiagnosticSeverity InSeverity)
	{
		switch (InSeverity)
		{
		case EModDiagnosticSeverity::Error:
			return 2;
		case EModDiagnosticSeverity::Warning:
			return 1;
		default:
			return 0;
		}
	}
}

void SModDiagnosticList::Construct(const FArguments& InArgs)
{
	EmptyMessage = InArgs._EmptyMessage.IsEmpty()
		? LOCTEXT("DefaultEmptyMessage", "Nothing to report.")
		: InArgs._EmptyMessage;

	const float MaxHeight = InArgs._MaxHeight > 0.0f ? InArgs._MaxHeight : 240.0f;

	SAssignNew(ListView, SListView<TSharedPtr<FModDiagnostic>>)
		.ListItemsSource(&VisibleItems)
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SModDiagnosticList::HandleGenerateRow);

	TSharedRef<SVerticalBox> Root = SNew(SVerticalBox);

	if (InArgs._ShowToolbar)
	{
		Root->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SModDiagnosticList::GetSummaryText)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNullWidget::NullWidget
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked(this, &SModDiagnosticList::HandleIsSeverityChecked, EModDiagnosticSeverity::Error)
					.OnCheckStateChanged(this, &SModDiagnosticList::HandleSeverityChanged, EModDiagnosticSeverity::Error)
					[
						SNew(STextBlock)
						.Text(this, &SModDiagnosticList::GetSeverityFilterLabel, EModDiagnosticSeverity::Error)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked(this, &SModDiagnosticList::HandleIsSeverityChecked, EModDiagnosticSeverity::Warning)
					.OnCheckStateChanged(this, &SModDiagnosticList::HandleSeverityChanged, EModDiagnosticSeverity::Warning)
					[
						SNew(STextBlock)
						.Text(this, &SModDiagnosticList::GetSeverityFilterLabel, EModDiagnosticSeverity::Warning)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked(this, &SModDiagnosticList::HandleIsSeverityChecked, EModDiagnosticSeverity::Info)
					.OnCheckStateChanged(this, &SModDiagnosticList::HandleSeverityChanged, EModDiagnosticSeverity::Info)
					[
						SNew(STextBlock)
						.Text(this, &SModDiagnosticList::GetSeverityFilterLabel, EModDiagnosticSeverity::Info)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(10.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ToolTipText(LOCTEXT("CopyTooltip", "Copy every visible diagnostic to the clipboard, one per line."))
					.OnClicked(this, &SModDiagnosticList::HandleCopyClicked)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("CopyLabel", "Copy"))
					]
				]
			];
	}

	Root->AddSlot()
		.AutoHeight()
		[
			SNew(SBox)
			.MaxDesiredHeight(MaxHeight)
			.Visibility(this, &SModDiagnosticList::GetListVisibility)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
				.Padding(2.0f)
				[
					ListView.ToSharedRef()
				]
			]
		];

	Root->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FStyleColors::Foreground)
			.Visibility(this, &SModDiagnosticList::GetEmptyMessageVisibility)
			.Text(EmptyMessage)
		];

	ChildSlot
	[
		Root
	];
}

void SModDiagnosticList::SetDiagnostics(const TArray<FModDiagnostic>& InDiagnostics)
{
	AllItems.Reset(InDiagnostics.Num());
	for (const FModDiagnostic& Diagnostic : InDiagnostics)
	{
		AllItems.Add(MakeShared<FModDiagnostic>(Diagnostic));
	}

	// Stable so that, within one severity, the order the producer emitted them in survives. For SDK
	// generation that is the order the steps ran in, which is what makes the list readable.
	Algo::StableSort(AllItems,
		[](const TSharedPtr<FModDiagnostic>& A, const TSharedPtr<FModDiagnostic>& B)
		{
			const int32 RankA = A.IsValid() ? ModDiagnosticListPrivate::SeverityRank(A->Severity) : -1;
			const int32 RankB = B.IsValid() ? ModDiagnosticListPrivate::SeverityRank(B->Severity) : -1;
			return RankA > RankB;
		});

	RebuildVisibleItems();
}

void SModDiagnosticList::ClearDiagnostics()
{
	AllItems.Reset();
	RebuildVisibleItems();
}

int32 SModDiagnosticList::CountBySeverity(EModDiagnosticSeverity InSeverity) const
{
	int32 Count = 0;
	for (const TSharedPtr<FModDiagnostic>& Item : AllItems)
	{
		if (Item.IsValid() && Item->Severity == InSeverity)
		{
			++Count;
		}
	}
	return Count;
}

TSharedRef<ITableRow> SModDiagnosticList::HandleGenerateRow(TSharedPtr<FModDiagnostic> InItem,
	const TSharedRef<STableViewBase>& InOwnerTable)
{
	// An entry can only be invalid if something handed the list a null shared pointer, which nothing
	// does - but the row generator must still return a widget rather than dereference blindly.
	if (!InItem.IsValid())
	{
		return SNew(STableRow<TSharedPtr<FModDiagnostic>>, InOwnerTable)
			[
				SNew(STextBlock).Text(LOCTEXT("InvalidDiagnostic", "<invalid diagnostic>"))
			];
	}

	const EModDiagnosticSeverity Severity = InItem->Severity;
	const FText CodeText = ModSDKWidgetUtils::OrDash(InItem->Code);
	const FText MessageText = FText::FromString(InItem->Message);
	const bool bHasContext = !InItem->Context.IsEmpty() || InItem->ModId.IsValid();

	FText ContextText;
	if (InItem->ModId.IsValid() && !InItem->Context.IsEmpty())
	{
		ContextText = FText::Format(LOCTEXT("ContextWithModFmt", "{0}  ·  mod: {1}"),
			FText::FromString(InItem->Context), FText::FromString(InItem->ModId.ToString()));
	}
	else if (InItem->ModId.IsValid())
	{
		ContextText = FText::Format(LOCTEXT("ContextModOnlyFmt", "mod: {0}"),
			FText::FromString(InItem->ModId.ToString()));
	}
	else
	{
		ContextText = FText::FromString(InItem->Context);
	}

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	Body->AddSlot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(FAppStyle::Get().GetFontStyle(TEXT("NormalFontBold")))
				.ColorAndOpacity(ModSDKWidgetUtils::GetSeverityColor(Severity))
				.Text(CodeText)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(MessageText)
			]
		];

	if (bHasContext)
	{
		Body->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FStyleColors::Foreground)
				.Text(ContextText)
			];
	}

	return SNew(STableRow<TSharedPtr<FModDiagnostic>>, InOwnerTable)
		.ToolTipText(FText::FromString(InItem->ToString()))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(4.0f, 4.0f, 6.0f, 4.0f)
			[
				SNew(SImage)
				.Image(ModSDKWidgetUtils::GetSeverityBrush(Severity))
				.ColorAndOpacity(ModSDKWidgetUtils::GetSeverityColor(Severity))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(0.0f, 3.0f, 4.0f, 3.0f)
			[
				Body
			]
		];
}

void SModDiagnosticList::RebuildVisibleItems()
{
	VisibleItems.Reset(AllItems.Num());
	for (const TSharedPtr<FModDiagnostic>& Item : AllItems)
	{
		if (!Item.IsValid())
		{
			continue;
		}

		const bool bAdmit =
			(Item->Severity == EModDiagnosticSeverity::Error && bShowErrors)
			|| (Item->Severity == EModDiagnosticSeverity::Warning && bShowWarnings)
			|| (Item->Severity == EModDiagnosticSeverity::Info && bShowInfos);

		if (bAdmit)
		{
			VisibleItems.Add(Item);
		}
	}

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

ECheckBoxState SModDiagnosticList::HandleIsSeverityChecked(EModDiagnosticSeverity InSeverity) const
{
	bool bChecked = false;
	switch (InSeverity)
	{
	case EModDiagnosticSeverity::Error:
		bChecked = bShowErrors;
		break;
	case EModDiagnosticSeverity::Warning:
		bChecked = bShowWarnings;
		break;
	default:
		bChecked = bShowInfos;
		break;
	}

	return bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SModDiagnosticList::HandleSeverityChanged(ECheckBoxState InNewState, EModDiagnosticSeverity InSeverity)
{
	const bool bChecked = InNewState == ECheckBoxState::Checked;

	switch (InSeverity)
	{
	case EModDiagnosticSeverity::Error:
		bShowErrors = bChecked;
		break;
	case EModDiagnosticSeverity::Warning:
		bShowWarnings = bChecked;
		break;
	default:
		bShowInfos = bChecked;
		break;
	}

	RebuildVisibleItems();
}

FReply SModDiagnosticList::HandleCopyClicked()
{
	FString Text;
	for (const TSharedPtr<FModDiagnostic>& Item : VisibleItems)
	{
		if (!Item.IsValid())
		{
			continue;
		}
		Text += Item->ToString();
		Text += LINE_TERMINATOR;
	}

	FPlatformApplicationMisc::ClipboardCopy(*Text);
	return FReply::Handled();
}

EVisibility SModDiagnosticList::GetListVisibility() const
{
	return VisibleItems.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SModDiagnosticList::GetEmptyMessageVisibility() const
{
	return VisibleItems.Num() > 0 ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SModDiagnosticList::GetSummaryText() const
{
	return ModSDKWidgetUtils::MakeCountSummary(
		CountBySeverity(EModDiagnosticSeverity::Error),
		CountBySeverity(EModDiagnosticSeverity::Warning),
		CountBySeverity(EModDiagnosticSeverity::Info));
}

FText SModDiagnosticList::GetSeverityFilterLabel(EModDiagnosticSeverity InSeverity) const
{
	return FText::Format(LOCTEXT("SeverityFilterFmt", "{0} ({1})"),
		ModSDKWidgetUtils::GetSeverityLabel(InSeverity),
		FText::AsNumber(CountBySeverity(InSeverity)));
}

#undef LOCTEXT_NAMESPACE
