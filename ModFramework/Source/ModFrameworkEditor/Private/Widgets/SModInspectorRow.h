// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Styling/SlateColor.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Views/STableRow.h"

class STableViewBase;
class SWidget;
struct FSlateBrush;

/**
 * One row of an inspector tree, described as data rather than as widgets.
 *
 * The three inspectors (API, extensions, permissions) show different things but want the same row:
 * an icon, an expander, a name, and a handful of aligned text columns. Modelling a row as a column
 * map lets all three share SModInspectorRow instead of growing three near-identical row classes that
 * then drift apart.
 *
 * Nodes hold FText, never live pointers into the registries. That matters: a refresh rebuilds the
 * node list from scratch, and anything a mod unloaded between two refreshes must not be dereferenced
 * by a row that has not been regenerated yet.
 */
struct FModInspectorNode
{
	/** Column id -> rendered cell text. A column with no entry renders empty. */
	TMap<FName, FText> Columns;

	/** Row tooltip. Empty means no tooltip. */
	FText ToolTip;

	/** Leading icon. Null means no icon and no reserved space. */
	const FSlateBrush* Icon = nullptr;

	FSlateColor IconColor = FSlateColor::UseForeground();

	/** Group rows (an extension point, a category) render their primary column in bold. */
	bool bIsGroup = false;

	/** Dimmed rows are still true - a deprecated symbol, an inactive extension - just not current. */
	bool bIsDimmed = false;

	TArray<TSharedPtr<FModInspectorNode>> Children;

	void SetColumn(FName InColumnId, FText InValue)
	{
		Columns.Add(InColumnId, MoveTemp(InValue));
	}

	FText GetColumn(FName InColumnId) const
	{
		const FText* Found = Columns.Find(InColumnId);
		return Found ? *Found : FText::GetEmpty();
	}

	/** True when any column, or the tooltip, contains InSearchText. Case insensitive. */
	bool MatchesFilter(const FString& InSearchText) const;
};

/** Renders an FModInspectorNode into whichever columns its owning tree declares. */
class SModInspectorRow : public SMultiColumnTableRow<TSharedPtr<FModInspectorNode>>
{
public:
	SLATE_BEGIN_ARGS(SModInspectorRow)
		: _Node()
		, _PrimaryColumn(NAME_None)
	{}
		SLATE_ARGUMENT(TSharedPtr<FModInspectorNode>, Node)

		/** The column that carries the expander arrow and the icon. */
		SLATE_ARGUMENT(FName, PrimaryColumn)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable);

	//~ Begin SMultiColumnTableRow
	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& InColumnName) override;
	//~ End SMultiColumnTableRow

private:
	TSharedPtr<FModInspectorNode> Node;
	FName PrimaryColumn;
};
