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
class UModPermissionRegistry;
class UModRegistry;
struct FModInspectorNode;
struct FModPermissionDescriptor;

/**
 * The "Permissions" tab: what capabilities exist, and what every mod actually got.
 *
 * THE CATALOGUE WORKS WITHOUT A GAME
 * The builtin descriptors are available from ModPermissions::GetBuiltinPermissionDescriptors()
 * without a registry, so the catalogue half of this tab is useful in a plain editor session. Only the
 * permissions a *game* registered at runtime, and the per-mod decisions, need a live subsystem.
 *
 * THE DECIDING RULE IS INFERRED, AND SAYS SO
 * UModPermissionRegistry stores the answer, not the reasoning - and it cannot store the reasoning,
 * because a game-supplied IModPermissionPolicy is consulted first and is free to decide anything. So
 * this panel replays the framework's documented default rules against the project settings and the
 * descriptor, and compares the prediction with the state that was actually recorded. When they agree
 * it names the rule; when they disagree it says the decision came from somewhere else - a policy or
 * an explicit grant - and reports what the defaults would have done instead. That disagreement is
 * itself the useful signal: it is exactly where a game's own policy is taking over.
 */
class SModPermissionInspectorPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SModPermissionInspectorPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SModPermissionInspectorPanel() override;

private:
	//~ Registry attachment -------------------------------------------------------------------------
	void BindToRegistries();
	void UnbindFromRegistries();
	void Reattach();

	//~ Delegate handlers ---------------------------------------------------------------------------
	void HandlePermissionChanged(const FModId& InModId, FName InPermissionId);
	void HandleModStateChanged(const FModId& InModId, EModState InOldState, EModState InNewState);
	void HandlePIEStarted(const bool bIsSimulating);
	void HandlePIEEnded(const bool bIsSimulating);

	//~ Tree ----------------------------------------------------------------------------------------
	TSharedRef<ITableRow> HandleGenerateRow(TSharedPtr<FModInspectorNode> InNode, const TSharedRef<STableViewBase>& InOwnerTable);
	void HandleGetChildren(TSharedPtr<FModInspectorNode> InNode, TArray<TSharedPtr<FModInspectorNode>>& OutChildren);
	void RebuildTree();

	/**
	 * Drops the per-mod decisions and falls back to the builtin catalogue, without consulting the
	 * registry. Used on EndPIE: the world contexts still exist when that fires, so a full rebuild
	 * would repopulate from a registry this panel has just unbound from.
	 */
	void ClearLiveState();

	/** The catalogue half: every descriptor the registry holds, or the builtins when there is none. */
	TSharedPtr<FModInspectorNode> BuildCatalogueNode(const UModPermissionRegistry* InRegistry);

	/** The decisions half. Null when no registry is live. */
	TSharedPtr<FModInspectorNode> BuildDecisionsNode(const UModPermissionRegistry* InRegistry, const UModRegistry* InModRegistry);

	/**
	 * Names the rule that produced InObserved for InPermissionId, per the class comment.
	 * InRegistry may be null, in which case only the settings-driven rules can be replayed.
	 */
	FText DescribeDecidingRule(FName InPermissionId, EModPermissionState InObserved, const UModPermissionRegistry* InRegistry) const;

	void HandleSearchTextChanged(const FText& InText);
	FReply HandleRefreshClicked();

	FText GetSummaryText() const;
	EVisibility GetNoSubsystemVisibility() const;

	TWeakObjectPtr<UModPermissionRegistry> BoundPermissionRegistry;
	TWeakObjectPtr<UModRegistry> BoundModRegistry;

	FDelegateHandle PermissionChangedHandle;
	FDelegateHandle PermissionRequestedHandle;
	FDelegateHandle ModStateChangedHandle;
	FDelegateHandle PIEStartedHandle;
	FDelegateHandle PIEEndedHandle;

	FString SearchText;

	/** Cached rather than re-queried per paint; see the note in the extensions panel. */
	bool bHasSubsystem = false;

	int32 CataloguedPermissionCount = 0;
	int32 DecidedModCount = 0;
	int32 GrantedCount = 0;
	int32 PendingCount = 0;
	int32 DeniedCount = 0;

	TArray<TSharedPtr<FModInspectorNode>> RootNodes;
	TSharedPtr<STreeView<TSharedPtr<FModInspectorNode>>> TreeView;
};
