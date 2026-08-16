// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModSDKPanel.h"

#include "Widgets/SModApiInspectorPanel.h"
#include "Widgets/SModExtensionInspectorPanel.h"
#include "Widgets/SModPermissionInspectorPanel.h"
#include "Widgets/SModSDKGeneratePanel.h"

#include "Internationalization/Internationalization.h"
#include "Layout/Margin.h"
#include "SDK/ModPublicApiScanner.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"

#define LOCTEXT_NAMESPACE "SModSDKPanel"

void SModSDKPanel::Construct(const FArguments& InArgs)
{
	// Built first: the generate panel hands its report to this one, so it has to exist by then.
	SAssignNew(ApiInspector, SModApiInspectorPanel);

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(12.0f, 10.0f, 12.0f, 0.0f)
		[
			SNew(SSegmentedControl<int32>)
			.Value(this, &SModSDKPanel::GetActiveTab)
			.OnValueChanged(this, &SModSDKPanel::HandleTabChanged)
			+ SSegmentedControl<int32>::Slot(static_cast<int32>(ETab::Generate))
			.Text(LOCTEXT("TabGenerate", "Generate SDK"))
			.ToolTip(LOCTEXT("TabGenerateTooltip", "Assemble the bundle a mod author installs."))
			+ SSegmentedControl<int32>::Slot(static_cast<int32>(ETab::ApiInspector))
			.Text(LOCTEXT("TabApi", "API Inspector"))
			.ToolTip(LOCTEXT("TabApiTooltip",
				"Every ModPublic symbol, and the marking problems that would break a generated SDK."))
			+ SSegmentedControl<int32>::Slot(static_cast<int32>(ETab::Extensions))
			.Text(LOCTEXT("TabExtensions", "Extensions"))
			.ToolTip(LOCTEXT("TabExtensionsTooltip", "Registered extension points and what mods filed under them."))
			+ SSegmentedControl<int32>::Slot(static_cast<int32>(ETab::Permissions))
			.Text(LOCTEXT("TabPermissions", "Permissions"))
			.ToolTip(LOCTEXT("TabPermissionsTooltip", "The permission catalogue and every mod's resolved decisions."))
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			[
				SAssignNew(Switcher, SWidgetSwitcher)
				.WidgetIndex(this, &SModSDKPanel::GetActiveTab)

				+ SWidgetSwitcher::Slot()
				[
					SNew(SModSDKGeneratePanel)
					.OnApiReportGenerated(this, &SModSDKPanel::HandleApiReportGenerated)
				]

				+ SWidgetSwitcher::Slot()
				[
					ApiInspector.ToSharedRef()
				]

				+ SWidgetSwitcher::Slot()
				[
					SNew(SModExtensionInspectorPanel)
				]

				+ SWidgetSwitcher::Slot()
				[
					SNew(SModPermissionInspectorPanel)
				]
			]
		]
	];
}

int32 SModSDKPanel::GetActiveTab() const
{
	return ActiveTab;
}

void SModSDKPanel::HandleTabChanged(int32 InNewTab)
{
	ActiveTab = InNewTab;
}

void SModSDKPanel::HandleApiReportGenerated(const FModPublicApiReport& InReport)
{
	if (ApiInspector.IsValid())
	{
		ApiInspector->SetReport(InReport);
	}
}

#undef LOCTEXT_NAMESPACE
