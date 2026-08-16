// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Input/Reply.h"
#include "Internationalization/Text.h"
#include "Layout/Visibility.h"
#include "SDK/ModSDKGenerator.h"
#include "Styling/SlateTypes.h"
#include "Templates/SharedPointer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SModDiagnosticList;
class SWidget;
struct FModPublicApiReport;

/** Raised after a successful generation so the API inspector can show the report that actually shipped. */
DECLARE_DELEGATE_OneParam(FOnModApiReportGenerated, const FModPublicApiReport& /*Report*/);

/**
 * The "Generate SDK" tab: pick an output directory, name and version the bundle, press the button.
 *
 * Every field here maps onto exactly one FModSDKGenerateOptions member, and the defaults come from
 * FModSDKGenerator::ResolveOptions so that the window and the GenerateModSDK commandlet agree about
 * what an unattended run would have produced. Generation is synchronous: it copies plugin trees and
 * walks the whole reflection database, so it runs behind a modal slow task rather than pretending
 * the editor is still responsive.
 */
class SModSDKGeneratePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModSDKGeneratePanel) {}
		SLATE_EVENT(FOnModApiReportGenerated, OnApiReportGenerated)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	/** Fills the editable fields from FModSDKGenerator::ResolveOptions, reporting whatever it says. */
	void ResolveDefaults();

	//~ Field accessors bound into the Slate widgets ------------------------------------------------
	FText GetOutputDirectoryText() const;
	void HandleOutputDirectoryCommitted(const FText& InText, ETextCommit::Type InCommitType);
	FText GetSdkPluginNameText() const;
	void HandleSdkPluginNameCommitted(const FText& InText, ETextCommit::Type InCommitType);
	FText GetSdkNameText() const;
	void HandleSdkNameCommitted(const FText& InText, ETextCommit::Type InCommitType);
	FText GetSdkVersionText() const;
	void HandleSdkVersionCommitted(const FText& InText, ETextCommit::Type InCommitType);

	//~ Buttons -------------------------------------------------------------------------------------
	FReply HandleBrowseClicked();
	FReply HandleGenerateClicked();
	FReply HandleRevealBundleClicked();
	FReply HandleResetDefaultsClicked();
	TSharedRef<SWidget> BuildSdkPluginMenu();

	/**
	 * Chosen from the discovered-plugins menu.
	 *
	 * A member rather than a lambda so the menu entry can hold it through FExecuteAction::CreateSP:
	 * the menu is a separate window and can outlive this panel, and a captured raw `this` would then
	 * be a dangling write.
	 */
	void HandleSdkPluginPicked(FString InPluginName);

	bool IsGenerateEnabled() const;
	FText GetBundlePreviewText() const;
	FText GetResultSummaryText() const;
	EVisibility GetResultVisibility() const;
	EVisibility GetRevealVisibility() const;

	/** One labelled row of the options form. */
	TSharedRef<SWidget> MakeLabelledRow(const FText& InLabel, const FText& InToolTip, TSharedRef<SWidget> InContent) const;

	/** One option checkbox bound to a bool member. */
	TSharedRef<SWidget> MakeOptionCheckBox(const FText& InLabel, const FText& InToolTip, bool* InFlag);

	ECheckBoxState GetFlagCheckState(bool* InFlag) const;
	void HandleFlagChanged(ECheckBoxState InNewState, bool* InFlag);

	//~ Edited state --------------------------------------------------------------------------------
	FString OutputDirectory;
	FString SdkPluginName;
	FString SdkName;
	FString SdkVersion;

	bool bIncludeDocs = true;
	bool bIncludeTemplateProject = true;
	bool bWriteApiReport = true;
	bool bIncludeBinaries = false;
	bool bIncludeGeneratorSource = false;
	bool bOverwriteExisting = true;
	bool bFailOnScanErrors = false;

	//~ Last run ------------------------------------------------------------------------------------
	FModSDKGenerateResult LastResult;
	bool bHasResult = false;
	bool bLastRunSucceeded = false;

	TSharedPtr<SModDiagnosticList> DiagnosticList;

	FOnModApiReportGenerated OnApiReportGenerated;
};
