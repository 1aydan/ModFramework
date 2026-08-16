// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModInspectorRow.h"

#include "Containers/UnrealString.h"
#include "Layout/Margin.h"
#include "Misc/CString.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"

bool FModInspectorNode::MatchesFilter(const FString& InSearchText) const
{
	if (InSearchText.IsEmpty())
	{
		return true;
	}

	for (const TPair<FName, FText>& Pair : Columns)
	{
		if (Pair.Value.ToString().Contains(InSearchText, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return !ToolTip.IsEmpty() && ToolTip.ToString().Contains(InSearchText, ESearchCase::IgnoreCase);
}

void SModInspectorRow::Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
{
	Node = InArgs._Node;
	PrimaryColumn = InArgs._PrimaryColumn;

	FSuperRowType::Construct(FSuperRowType::FTableRowArgs(), InOwnerTable);
}

TSharedRef<SWidget> SModInspectorRow::GenerateWidgetForColumn(const FName& InColumnName)
{
	// A row whose node went away between generation and layout is possible in principle; render an
	// empty cell rather than dereferencing. Nothing in an inspector is worth a crash.
	if (!Node.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	const FSlateColor TextColor = Node->bIsDimmed ? FStyleColors::Foreground : FSlateColor::UseForeground();

	TSharedRef<STextBlock> TextBlock = SNew(STextBlock)
		.Text(Node->GetColumn(InColumnName))
		.ColorAndOpacity(TextColor)
		.Font(Node->bIsGroup && InColumnName == PrimaryColumn
			? FAppStyle::Get().GetFontStyle(TEXT("NormalFontBold"))
			: FAppStyle::Get().GetFontStyle(TEXT("NormalFont")));

	if (!Node->ToolTip.IsEmpty())
	{
		TextBlock->SetToolTipText(Node->ToolTip);
	}

	if (InColumnName != PrimaryColumn)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(4.0f, 2.0f)
			[
				TextBlock
			];
	}

	TSharedRef<SHorizontalBox> Primary = SNew(SHorizontalBox);

	Primary->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SExpanderArrow, SharedThis(this))
			.ShouldDrawWires(true)
		];

	if (Node->Icon)
	{
		Primary->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SImage)
				.Image(Node->Icon)
				.ColorAndOpacity(Node->IconColor)
			];
	}

	Primary->AddSlot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		.Padding(0.0f, 2.0f, 4.0f, 2.0f)
		[
			TextBlock
		];

	return Primary;
}
