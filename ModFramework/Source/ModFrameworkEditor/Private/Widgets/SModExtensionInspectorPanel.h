// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/IDelegateInstance.h"
#include "Input/Reply.h"
#include "Internationalization/Text.h"
#include "Layout/Visibility.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class ITableRow;
class STableViewBase;
class UModExtensionRegistry;
class UModRegistry;
struct FModInspectorNode;

/**
 * The "Extensions" tab: every extension point the game opened, and what each loaded mod filed
 * under it.
 *
 * This one needs a live UModSubsystem, which is a UGameInstanceSubsystem: a plain editor session has
 * no game instance and therefore no registries at all. That is the normal state, not an error, so the
 * panel says so plainly and re-acquires when a PIE session starts. It never asserts, never caches a
 * raw registry pointer across a frame, and never polls: it rebuilds from the registry's own
 * OnExtensionsChanged / OnExtensionPointsChanged delegates and from mod state changes.
 */
class SModExtensionInspectorPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModExtensionInspectorPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SModExtensionInspectorPanel() override;

private:
	//~ Registry attachment -------------------------------------------------------------------------
	void BindToRegistries();
	void UnbindFromRegistries();

	/** Unbind, rebind to whatever is live now, rebuild. Cheap enough to call on any interesting event. */
	void Reattach();

	//~ Delegate handlers ---------------------------------------------------------------------------
	void HandleExtensionsChanged(FName InExtensionPointId, const FModId& InModId);
	void HandleModStateChanged(const FModId& InModId, EModState InOldState, EModState InNewState);
	void HandlePIEStarted(const bool bIsSimulating);
	void HandlePIEEnded(const bool bIsSimulating);

	//~ Tree ----------------------------------------------------------------------------------------
	TSharedRef<ITableRow> HandleGenerateRow(TSharedPtr<FModInspectorNode> InNode, const TSharedRef<STableViewBase>& InOwnerTable);
	void HandleGetChildren(TSharedPtr<FModInspectorNode> InNode, TArray<TSharedPtr<FModInspectorNode>>& OutChildren);
	void RebuildTree();

	/**
	 * Empties the tree without consulting the registries.
	 *
	 * Used on EndPIE, where re-reading would be actively wrong: the world contexts still exist when
	 * that fires, so a rebuild would happily show live data one frame after this panel unbound from
	 * the delegates that keep it true. Showing nothing is the honest answer.
	 */
	void ClearTree();

	void HandleSearchTextChanged(const FText& InText);
	FReply HandleRefreshClicked();

	FText GetSummaryText() const;
	EVisibility GetNoSubsystemVisibility() const;
	EVisibility GetTreeVisibility() const;

	/** Registries the delegates below are attached to. Weak: a PIE teardown collects them. */
	TWeakObjectPtr<UModExtensionRegistry> BoundExtensionRegistry;
	TWeakObjectPtr<UModRegistry> BoundModRegistry;

	FDelegateHandle ExtensionsChangedHandle;
	FDelegateHandle ExtensionPointsChangedHandle;
	FDelegateHandle ModStateChangedHandle;
	FDelegateHandle PIEStartedHandle;
	FDelegateHandle PIEEndedHandle;

	FString SearchText;

	/**
	 * Whether a subsystem was found the last time the tree was rebuilt.
	 *
	 * Cached rather than re-queried from the visibility attributes: those are evaluated every frame,
	 * and walking the engine's world contexts on every paint is exactly the polling this window is
	 * supposed to avoid. PIE start and end both rebuild, so the cache cannot go stale.
	 */
	bool bHasSubsystem = false;

	/** Counts for the summary line, refreshed with the tree. */
	int32 ExtensionPointCount = 0;
	int32 ExtensionCount = 0;
	int32 ActiveExtensionCount = 0;

	TArray<TSharedPtr<FModInspectorNode>> RootNodes;
	TSharedPtr<STreeView<TSharedPtr<FModInspectorNode>>> TreeView;
};
