// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Styling/SlateTypes.h"
#include "Templates/SharedPointer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class FModDeveloperModel;
class SModConflictPanel;
class SModDependencyPanel;
class SModListPanel;
class SModPackagePanel;
class SModValidationPanel;
class SWidget;
class SWidgetSwitcher;

/** The five pages of the Mod Developer window, in tab-strip order. */
enum class EModDeveloperTab : uint8
{
	Mods,
	Validation,
	Dependencies,
	Conflicts,
	Package,

	Count
};

/**
 * The Mod Developer window: everything a mod author needs between "I have a folder" and "I have a
 * `.mod` somebody else can install".
 *
 * ONE MODEL, FIVE VIEWS
 * The five panels share a single FModDeveloperModel. They never read UModSubsystem themselves, so
 * they cannot disagree with each other about what state a mod is in, and one refresh updates all of
 * them. The model is event driven - it listens to the subsystem and to the PIE delegates - so the
 * window costs nothing while it sits open and idle.
 *
 * The panels are cross-wired where it saves a click: a validation row focuses its mod on the Mods
 * tab and points the dependency view at it, a conflict contender does the same, and the Mods tab's
 * Package button opens the Package tab with the folder already filled in.
 */
class SModDeveloperWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModDeveloperWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual ~SModDeveloperWindow() override;

	/** Switches the visible page. */
	void SetActiveTab(EModDeveloperTab InTab);

private:
	TSharedRef<SWidget> BuildTabStrip();

	TSharedRef<SWidget> BuildTabButton(EModDeveloperTab InTab, const FText& Label, const FText& Tooltip);

	ECheckBoxState GetTabCheckState(EModDeveloperTab InTab) const;

	void HandleTabCheckStateChanged(ECheckBoxState NewState, EModDeveloperTab InTab);

	/** Brings one mod into focus: Mods tab, selected, with the dependency view pointed at it too. */
	void HandleModFocusRequested(FModId ModId);

	/** Opens the Package tab with the mod selected on the Mods tab already filled in. */
	void HandlePackageRequested();

	FReply HandleRefreshClicked();

	FText GetStatusText() const;

	TSharedPtr<FModDeveloperModel> Model;

	TSharedPtr<SWidgetSwitcher> Switcher;

	TSharedPtr<SModListPanel> ListPanel;
	TSharedPtr<SModValidationPanel> ValidationPanel;
	TSharedPtr<SModDependencyPanel> DependencyPanel;
	TSharedPtr<SModConflictPanel> ConflictPanel;
	TSharedPtr<SModPackagePanel> PackagePanel;

	EModDeveloperTab ActiveTab = EModDeveloperTab::Mods;
};
