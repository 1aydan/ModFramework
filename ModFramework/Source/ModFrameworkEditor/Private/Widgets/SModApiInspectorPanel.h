// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Delegates/IDelegateInstance.h"
#include "Input/Reply.h"
#include "Internationalization/Text.h"
#include "Layout/Visibility.h"
#include "SDK/ModPublicApiScanner.h"
#include "Styling/SlateTypes.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class ITableRow;
class SModDiagnosticList;
class STableViewBase;
class SWidget;
struct FModInspectorNode;

/**
 * The "API Inspector" tab: what FModPublicApiScanner sees when it looks at this project.
 *
 * THE WARNING LIST IS THE POINT
 * The symbol tree is reference material. The diagnostics above it are the reason to open this tab:
 * a marked signature that names an unmarked type produces an SDK that will not compile in the mod
 * author's project, and a UModAPI whose ModApiId metadata disagrees with its NativeGetApiId override
 * registers under one id in the editor and another in the shipped game. Both are invisible until a
 * mod author hits them, and both are listed here before the bundle is ever built. The warnings are
 * therefore rendered first, expanded, above the tree - not tucked into a status bar.
 *
 * A scan is not free: it walks the entire reflection database. It runs on construction, when the
 * Rescan button is pressed, and when SDK generation hands over the report it just produced. Nothing
 * here polls, and nothing here rescans on a timer.
 */
class SModApiInspectorPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModApiInspectorPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SModApiInspectorPanel() override;

	/**
	 * Adopts a report produced elsewhere - specifically the one SDK generation carried out of
	 * FModSDKGenerateResult - so the tab shows the surface the bundle actually documents rather than
	 * a second, separate scan of the same project.
	 */
	void SetReport(const FModPublicApiReport& InReport);

	/** Runs a fresh scan with the current options, behind a modal progress dialog. */
	void Rescan();

private:
	/**
	 * The scan itself.
	 *
	 * @param bAllowProgressDialog false during Construct: raising a modal window while the widget
	 *        tree that owns this panel is still being built is not something Slate promises to
	 *        survive, and the first scan happens because the user opened the window anyway.
	 */
	void RunScan(bool bAllowProgressDialog);

	//~ Tree ----------------------------------------------------------------------------------------
	TSharedRef<ITableRow> HandleGenerateRow(TSharedPtr<FModInspectorNode> InNode, const TSharedRef<STableViewBase>& InOwnerTable);
	void HandleGetChildren(TSharedPtr<FModInspectorNode> InNode, TArray<TSharedPtr<FModInspectorNode>>& OutChildren);

	/** Rebuilds RootNodes from Report, honouring the current search text. */
	void RebuildTree();

	/** Builds one type row plus its marked members. Returns null when the filter rejects the whole subtree. */
	TSharedPtr<FModInspectorNode> BuildTypeNode(const FModPublicTypeInfo& InType, const FModPublicApiEntry* InApiEntry) const;

	/** Adds InCategory to OutRoots when it kept any child. */
	void AddCategory(TArray<TSharedPtr<FModInspectorNode>>& OutRoots, const FText& InLabel,
		TArray<TSharedPtr<FModInspectorNode>>&& InChildren) const;

	/** Expands every category, and every surviving type while a filter is active. */
	void ApplyDefaultExpansion();

	//~ Toolbar -------------------------------------------------------------------------------------
	void HandleSearchTextChanged(const FText& InText);
	FReply HandleRescanClicked();
	TSharedRef<SWidget> BuildSdkPluginMenu();

	/**
	 * Chosen from the discovered-plugins menu; rescans with the new shipping scope.
	 *
	 * A member rather than a lambda so the entry can hold it through FExecuteAction::CreateSP - the
	 * menu is a separate window and can outlive this panel.
	 */
	void HandleSdkPluginScopePicked(FString InPluginName);
	FText GetSdkPluginButtonText() const;
	ECheckBoxState GetIncludeBlueprintTypesState() const;
	void HandleIncludeBlueprintTypesChanged(ECheckBoxState InNewState);

	//~ Status --------------------------------------------------------------------------------------
	FText GetSummaryText() const;
	FText GetWarningHeaderText() const;
	EVisibility GetStaleNoticeVisibility() const;
	EVisibility GetMetadataUnavailableVisibility() const;

	/** Live coding and hot reload replace UClass objects, so an existing report describes the past. */
	void HandleReloadComplete(EReloadCompleteReason InReason);

	FModPublicApiReport Report;

	/** Scoped so that unmarked types in the shipping plugins are not reported as leaks. */
	FString SdkPluginName;

	bool bIncludeBlueprintGeneratedTypes = false;

	/** True once reflection data changed under a report that is still on screen. */
	bool bReportIsStale = false;

	FString SearchText;

	TArray<TSharedPtr<FModInspectorNode>> RootNodes;
	TSharedPtr<STreeView<TSharedPtr<FModInspectorNode>>> TreeView;
	TSharedPtr<SModDiagnosticList> DiagnosticList;

	FDelegateHandle ReloadCompleteHandle;
};
