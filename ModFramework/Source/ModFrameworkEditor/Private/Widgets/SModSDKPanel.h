// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Templates/SharedPointer.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SModApiInspectorPanel;
class SWidgetSwitcher;
struct FModPublicApiReport;

/**
 * Root of the Mod SDK window: a segmented tab bar over the four game-developer panels.
 *
 * A widget switcher rather than a nested FTabManager, because these four are one workflow rather than
 * four documents - you generate a bundle, read the warnings the scan produced, then check that the
 * extension points and permissions the SDK advertises are the ones the game actually opened.
 *
 * All four are constructed up front. That costs one public API scan on open, which is the point of
 * opening the window, and it means the panels are already attached to their delegates before the user
 * reaches them rather than missing the events that happened on the way.
 */
class SModSDKPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModSDKPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	/** Indices into the switcher. Kept as an enum so the tab bar and the switcher cannot drift apart. */
	enum class ETab : int32
	{
		Generate = 0,
		ApiInspector = 1,
		Extensions = 2,
		Permissions = 3
	};

	int32 GetActiveTab() const;
	void HandleTabChanged(int32 InNewTab);

	/** Forwards the report SDK generation produced into the API inspector. */
	void HandleApiReportGenerated(const FModPublicApiReport& InReport);

	int32 ActiveTab = static_cast<int32>(ETab::Generate);

	TSharedPtr<SWidgetSwitcher> Switcher;
	TSharedPtr<SModApiInspectorPanel> ApiInspector;
};
