// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModDeveloperWindow.h"

#include "HAL/Platform.h"
#include "Input/Reply.h"
#include "Layout/Margin.h"
#include "ModDeveloperModel.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SModConflictPanel.h"
#include "Widgets/SModDependencyPanel.h"
#include "Widgets/SModListPanel.h"
#include "Widgets/SModPackagePanel.h"
#include "Widgets/SModValidationPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SModDeveloperWindow"

void SModDeveloperWindow::Construct(const FArguments& /*InArgs*/)
{
	// One model for the whole window. Created before the panels because each of them binds to it.
	Model = FModDeveloperModel::Create();

	const FOnModFocusRequested FocusDelegate =
		FOnModFocusRequested::CreateSP(this, &SModDeveloperWindow::HandleModFocusRequested);

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Header ----------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f, 8.0f, 8.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(this, &SModDeveloperWindow::GetStatusText)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ContentPadding(FMargin(10.0f, 3.0f))
				.ToolTipText(LOCTEXT("RefreshAllTip",
					"Rescan and rebuild every tab. Mods are otherwise refreshed automatically when the "
					"running game changes their state."))
				.OnClicked(this, &SModDeveloperWindow::HandleRefreshClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("RefreshAll", "Refresh"))
				]
			]
		]

		// --- Tab strip -------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.0f, 8.0f, 8.0f, 0.0f)
		[
			BuildTabStrip()
		]

		// --- Pages -----------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(8.0f, 6.0f, 8.0f, 8.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			.Padding(8.0f)
			[
				SAssignNew(Switcher, SWidgetSwitcher)

				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(ListPanel, SModListPanel)
					.Model(Model)
					.OnPackageRequested(FSimpleDelegate::CreateSP(this, &SModDeveloperWindow::HandlePackageRequested))
				]

				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(ValidationPanel, SModValidationPanel)
					.Model(Model)
					.OnModFocusRequested(FocusDelegate)
				]

				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(DependencyPanel, SModDependencyPanel)
					.Model(Model)
				]

				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(ConflictPanel, SModConflictPanel)
					.Model(Model)
					.OnModFocusRequested(FocusDelegate)
				]

				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(PackagePanel, SModPackagePanel)
					.Model(Model)
				]
			]
		]
	];

	SetActiveTab(EModDeveloperTab::Mods);
}

SModDeveloperWindow::~SModDeveloperWindow()
{
	// The panels hold shared references to the model and unbind themselves as they go away. Dropping
	// this last reference here is what finally lets the model unsubscribe from the subsystem.
	Model.Reset();
}

TSharedRef<SWidget> SModDeveloperWindow::BuildTabStrip()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 2.0f, 0.0f)
		[
			BuildTabButton(EModDeveloperTab::Mods,
				LOCTEXT("TabMods", "Mods"),
				LOCTEXT("TabModsTip", "Every mod the framework can see, with its state and its lifecycle actions."))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			BuildTabButton(EModDeveloperTab::Validation,
				LOCTEXT("TabValidation", "Validation"),
				LOCTEXT("TabValidationTip", "Re-run manifest validation and read every diagnostic in one list."))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			BuildTabButton(EModDeveloperTab::Dependencies,
				LOCTEXT("TabDependencies", "Dependencies"),
				LOCTEXT("TabDependenciesTip", "What one mod needs, what needs it, and how each edge resolved."))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			BuildTabButton(EModDeveloperTab::Conflicts,
				LOCTEXT("TabConflicts", "Conflicts"),
				LOCTEXT("TabConflictsTip", "Resources two or more mods claim, and what the conflict policy did about it."))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			BuildTabButton(EModDeveloperTab::Package,
				LOCTEXT("TabPackage", "Package"),
				LOCTEXT("TabPackageTip", "Write a mod folder into a distributable .mod container."))
		];
}

TSharedRef<SWidget> SModDeveloperWindow::BuildTabButton(EModDeveloperTab InTab, const FText& Label, const FText& Tooltip)
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), TEXT("DetailsView.SectionButton"))
		.Padding(FMargin(14.0f, 4.0f))
		.ToolTipText(Tooltip)
		.IsChecked(this, &SModDeveloperWindow::GetTabCheckState, InTab)
		.OnCheckStateChanged(this, &SModDeveloperWindow::HandleTabCheckStateChanged, InTab)
		[
			SNew(STextBlock)
			.Text(Label)
		];
}

ECheckBoxState SModDeveloperWindow::GetTabCheckState(EModDeveloperTab InTab) const
{
	return (ActiveTab == InTab) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SModDeveloperWindow::HandleTabCheckStateChanged(ECheckBoxState NewState, EModDeveloperTab InTab)
{
	// A tab strip is radio behaviour, not a set of independent toggles: clicking the active tab again
	// must leave it active rather than leaving the window with no page showing.
	if (NewState == ECheckBoxState::Checked || ActiveTab == InTab)
	{
		SetActiveTab(InTab);
	}
}

void SModDeveloperWindow::SetActiveTab(EModDeveloperTab InTab)
{
	if (InTab == EModDeveloperTab::Count)
	{
		return;
	}

	ActiveTab = InTab;

	if (Switcher.IsValid())
	{
		Switcher->SetActiveWidgetIndex(static_cast<int32>(InTab));
	}
}

void SModDeveloperWindow::HandleModFocusRequested(FModId ModId)
{
	if (!ModId.IsValid())
	{
		return;
	}

	if (ListPanel.IsValid())
	{
		ListPanel->SelectMod(ModId);
	}

	// Pointing the dependency view at the same mod costs nothing and is almost always where the
	// author goes next after reading a resolution error.
	if (DependencyPanel.IsValid())
	{
		DependencyPanel->SetSelectedMod(ModId);
	}

	SetActiveTab(EModDeveloperTab::Mods);
}

void SModDeveloperWindow::HandlePackageRequested()
{
	if (PackagePanel.IsValid() && ListPanel.IsValid())
	{
		const TSharedPtr<FModDeveloperModRow> Row = ListPanel->GetSelectedRow();

		// A `.mod` package has no folder to package; only an unpacked mod root is a valid source.
		if (Row.IsValid() && !Row->bPackaged && !Row->RootPath.IsEmpty())
		{
			PackagePanel->SetSourceDirectory(Row->RootPath);
		}
	}

	SetActiveTab(EModDeveloperTab::Package);
}

FReply SModDeveloperWindow::HandleRefreshClicked()
{
	if (Model.IsValid())
	{
		Model->Refresh();
	}

	return FReply::Handled();
}

FText SModDeveloperWindow::GetStatusText() const
{
	if (!Model.IsValid())
	{
		return LOCTEXT("NoModel", "The mod developer window failed to create its data source.");
	}

	return Model->GetSourceDescription();
}

#undef LOCTEXT_NAMESPACE
