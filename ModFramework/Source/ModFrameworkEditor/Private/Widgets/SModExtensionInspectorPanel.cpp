// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModExtensionInspectorPanel.h"

#include "Widgets/ModSDKWidgetUtils.h"
#include "Widgets/SModInspectorRow.h"

#include "Editor.h"
#include "Extensions/ModExtension.h"
#include "Extensions/ModExtensionRegistry.h"
#include "Internationalization/Internationalization.h"
#include "Layout/Margin.h"
#include "Manifest/ModVersion.h"
#include "Registry/ModRegistry.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystem/ModSubsystem.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "SModExtensionInspectorPanel"

namespace ModExtensionInspectorPrivate
{
	const FName ColumnName(TEXT("Name"));
	const FName ColumnOwner(TEXT("Owner"));
	const FName ColumnPriority(TEXT("Priority"));
	const FName ColumnState(TEXT("State"));
	const FName ColumnDetail(TEXT("Detail"));

	/** "policy: LastWins · base: UGameWeaponExtension · one per mod · requires: gameplay.modify" */
	FText DescribeExtensionPoint(const FModExtensionPointDescriptor& InDescriptor)
	{
		TArray<FText> Parts;

		Parts.Add(FText::Format(LOCTEXT("PointPolicyFmt", "policy: {0}"),
			FText::FromString(ModFrameworkEnums::ToString(InDescriptor.DefaultConflictPolicy))));

		const UClass* BaseClass = InDescriptor.RequiredBaseClass.Get();
		Parts.Add(FText::Format(LOCTEXT("PointBaseFmt", "base: {0}"),
			BaseClass ? FText::FromString(BaseClass->GetName()) : LOCTEXT("PointBaseAny", "any UModExtension")));

		if (!InDescriptor.bAllowMultiplePerMod)
		{
			Parts.Add(LOCTEXT("PointOnePerMod", "one per mod"));
		}
		if (InDescriptor.bServerAuthoritative)
		{
			Parts.Add(LOCTEXT("PointServerAuth", "server authoritative"));
		}
		if (InDescriptor.RequiredPermissions.Num() > 0)
		{
			Parts.Add(FText::Format(LOCTEXT("PointPermsFmt", "requires: {0}"),
				ModSDKWidgetUtils::JoinNames(InDescriptor.RequiredPermissions)));
		}

		return FText::Join(LOCTEXT("PointDetailSeparator", "  ·  "), Parts);
	}
}

void SModExtensionInspectorPanel::Construct(const FArguments& InArgs)
{
	using namespace ModExtensionInspectorPrivate;

	SAssignNew(TreeView, STreeView<TSharedPtr<FModInspectorNode>>)
		.TreeItemsSource(&RootNodes)
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SModExtensionInspectorPanel::HandleGenerateRow)
		.OnGetChildren(this, &SModExtensionInspectorPanel::HandleGetChildren)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SHeaderRow::Column(ColumnName)
			.DefaultLabel(LOCTEXT("ColName", "Extension point / extension"))
			.FillWidth(0.32f)
			+ SHeaderRow::Column(ColumnOwner)
			.DefaultLabel(LOCTEXT("ColOwner", "Owning mod"))
			.FillWidth(0.16f)
			+ SHeaderRow::Column(ColumnPriority)
			.DefaultLabel(LOCTEXT("ColPriority", "Priority"))
			.FillWidth(0.07f)
			+ SHeaderRow::Column(ColumnState)
			.DefaultLabel(LOCTEXT("ColState", "State"))
			.FillWidth(0.11f)
			+ SHeaderRow::Column(ColumnDetail)
			.DefaultLabel(LOCTEXT("ColDetail", "Details"))
			.FillWidth(0.34f));

	ChildSlot
	.Padding(12.0f)
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			ModSDKWidgetUtils::MakeSectionHeader(
				LOCTEXT("ExtensionsTitle", "Extension points"),
				LOCTEXT("ExtensionsSubtitle",
					"Every slot the game opened with RegisterExtensionPoint, and what the loaded mods filed under each. "
					"Only activated extensions are returned to game code by GetExtensions, so the State column is the one "
					"that decides whether a mod's contribution is actually running."))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Filter points, extensions, mods and claimed resources"))
				.OnTextChanged(this, &SModExtensionInspectorPanel::HandleSearchTextChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ToolTipText(LOCTEXT("RefreshTooltip",
					"Re-read the registries. The panel already refreshes on registry and mod-state delegates; this is for the "
					"cases nothing broadcasts, such as a game that registered a point outside the normal path."))
				.OnClicked(this, &SModExtensionInspectorPanel::HandleRefreshClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("RefreshLabel", "Refresh"))
				]
			]
		]

		//~ No game instance ----------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.Visibility(this, &SModExtensionInspectorPanel::GetNoSubsystemVisibility)
			[
				ModSDKWidgetUtils::MakeNotice(
					LOCTEXT("NoSubsystemTitle", "No mod subsystem is running"),
					LOCTEXT("NoSubsystemBody",
						"UModSubsystem is a game instance subsystem, so the extension registry only exists while a game is "
						"running. Start Play In Editor and this fills in by itself - the panel is watching for it."),
					EModDiagnosticSeverity::Info)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 6.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FStyleColors::Foreground)
			.Visibility(this, &SModExtensionInspectorPanel::GetTreeVisibility)
			.Text(this, &SModExtensionInspectorPanel::GetSummaryText)
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBorder)
			.Visibility(this, &SModExtensionInspectorPanel::GetTreeVisibility)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			.Padding(2.0f)
			[
				TreeView.ToSharedRef()
			]
		]
	];

	// PIE is what makes a subsystem appear and disappear, so those two are the events that decide
	// whether this panel has anything to show at all.
	PIEStartedHandle = FEditorDelegates::PostPIEStarted.AddSP(this, &SModExtensionInspectorPanel::HandlePIEStarted);
	PIEEndedHandle = FEditorDelegates::EndPIE.AddSP(this, &SModExtensionInspectorPanel::HandlePIEEnded);

	Reattach();
}

SModExtensionInspectorPanel::~SModExtensionInspectorPanel()
{
	UnbindFromRegistries();

	if (PIEStartedHandle.IsValid())
	{
		FEditorDelegates::PostPIEStarted.Remove(PIEStartedHandle);
		PIEStartedHandle.Reset();
	}
	if (PIEEndedHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(PIEEndedHandle);
		PIEEndedHandle.Reset();
	}
}

void SModExtensionInspectorPanel::BindToRegistries()
{
	UModSubsystem* Subsystem = ModSDKWidgetUtils::FindLiveModSubsystem();
	if (!Subsystem)
	{
		return;
	}

	if (UModExtensionRegistry* ExtensionRegistry = Subsystem->GetExtensionRegistry())
	{
		BoundExtensionRegistry = ExtensionRegistry;
		ExtensionsChangedHandle = ExtensionRegistry->OnExtensionsChanged.AddSP(
			this, &SModExtensionInspectorPanel::HandleExtensionsChanged);
		ExtensionPointsChangedHandle = ExtensionRegistry->OnExtensionPointsChanged.AddSP(
			this, &SModExtensionInspectorPanel::HandleExtensionsChanged);
	}

	if (UModRegistry* ModRegistry = Subsystem->GetRegistry())
	{
		BoundModRegistry = ModRegistry;
		ModStateChangedHandle = ModRegistry->OnModStateChanged.AddSP(
			this, &SModExtensionInspectorPanel::HandleModStateChanged);
	}
}

void SModExtensionInspectorPanel::UnbindFromRegistries()
{
	// The registries may already be collected - EndPIE tears down the game instance - so every removal
	// is guarded by the weak pointer rather than assuming the object outlived the handle.
	if (UModExtensionRegistry* ExtensionRegistry = BoundExtensionRegistry.Get())
	{
		if (ExtensionsChangedHandle.IsValid())
		{
			ExtensionRegistry->OnExtensionsChanged.Remove(ExtensionsChangedHandle);
		}
		if (ExtensionPointsChangedHandle.IsValid())
		{
			ExtensionRegistry->OnExtensionPointsChanged.Remove(ExtensionPointsChangedHandle);
		}
	}

	if (UModRegistry* ModRegistry = BoundModRegistry.Get())
	{
		if (ModStateChangedHandle.IsValid())
		{
			ModRegistry->OnModStateChanged.Remove(ModStateChangedHandle);
		}
	}

	ExtensionsChangedHandle.Reset();
	ExtensionPointsChangedHandle.Reset();
	ModStateChangedHandle.Reset();
	BoundExtensionRegistry.Reset();
	BoundModRegistry.Reset();
}

void SModExtensionInspectorPanel::Reattach()
{
	UnbindFromRegistries();
	BindToRegistries();
	RebuildTree();
}

void SModExtensionInspectorPanel::HandleExtensionsChanged(FName InExtensionPointId, const FModId& InModId)
{
	RebuildTree();
}

void SModExtensionInspectorPanel::HandleModStateChanged(const FModId& InModId, EModState InOldState, EModState InNewState)
{
	RebuildTree();
}

void SModExtensionInspectorPanel::HandlePIEStarted(const bool bIsSimulating)
{
	Reattach();
}

void SModExtensionInspectorPanel::HandlePIEEnded(const bool bIsSimulating)
{
	// Unbind while the registries are still alive, then clear rather than rebuild: EndPIE fires
	// before the world context goes away, so a rebuild would repopulate from a registry this panel
	// has just stopped listening to and then quietly rot.
	UnbindFromRegistries();
	ClearTree();
}

void SModExtensionInspectorPanel::ClearTree()
{
	RootNodes.Reset();
	ExtensionPointCount = 0;
	ExtensionCount = 0;
	ActiveExtensionCount = 0;
	bHasSubsystem = false;

	if (TreeView.IsValid())
	{
		TreeView->RequestTreeRefresh();
	}
}

TSharedRef<ITableRow> SModExtensionInspectorPanel::HandleGenerateRow(TSharedPtr<FModInspectorNode> InNode,
	const TSharedRef<STableViewBase>& InOwnerTable)
{
	return SNew(SModInspectorRow, InOwnerTable)
		.Node(InNode)
		.PrimaryColumn(ModExtensionInspectorPrivate::ColumnName);
}

void SModExtensionInspectorPanel::HandleGetChildren(TSharedPtr<FModInspectorNode> InNode,
	TArray<TSharedPtr<FModInspectorNode>>& OutChildren)
{
	if (InNode.IsValid())
	{
		OutChildren = InNode->Children;
	}
}

void SModExtensionInspectorPanel::RebuildTree()
{
	using namespace ModExtensionInspectorPrivate;

	RootNodes.Reset();
	ExtensionPointCount = 0;
	ExtensionCount = 0;
	ActiveExtensionCount = 0;

	UModSubsystem* Subsystem = ModSDKWidgetUtils::FindLiveModSubsystem();
	UModExtensionRegistry* ExtensionRegistry = Subsystem ? Subsystem->GetExtensionRegistry() : nullptr;
	bHasSubsystem = ExtensionRegistry != nullptr;

	if (!ExtensionRegistry)
	{
		if (TreeView.IsValid())
		{
			TreeView->RequestTreeRefresh();
		}
		return;
	}

	for (const FModExtensionPointDescriptor& Descriptor : ExtensionRegistry->GetExtensionPoints())
	{
		++ExtensionPointCount;

		TSharedRef<FModInspectorNode> PointNode = MakeShared<FModInspectorNode>();
		PointNode->bIsGroup = true;

		const FText PointLabel = Descriptor.DisplayName.IsEmpty()
			? FText::FromName(Descriptor.ExtensionPointId)
			: FText::Format(LOCTEXT("PointLabelFmt", "{0}  ({1})"),
				Descriptor.DisplayName, FText::FromName(Descriptor.ExtensionPointId));

		PointNode->SetColumn(ColumnName, PointLabel);
		PointNode->SetColumn(ColumnOwner, LOCTEXT("PointOwner", "game"));
		PointNode->SetColumn(ColumnPriority, ModSDKWidgetUtils::OrDash(FString()));
		PointNode->SetColumn(ColumnDetail, DescribeExtensionPoint(Descriptor));

		FText PointToolTip = FText::Format(LOCTEXT("PointToolTipFmt", "{0}\nContract version {1}"),
			FText::FromName(Descriptor.ExtensionPointId),
			FText::FromString(Descriptor.Version.ToString()));
		if (!Descriptor.Description.IsEmpty())
		{
			PointToolTip = FText::Format(LOCTEXT("PointToolTipDescFmt", "{0}\n\n{1}"), PointToolTip, Descriptor.Description);
		}
		PointNode->ToolTip = PointToolTip;

		// GetAllExtensions rather than GetExtensions: a mod that is loaded but not activated has
		// contributed something real, and hiding it would make "my extension is not running" harder
		// to diagnose, not easier.
		int32 PointActiveCount = 0;
		const TArray<UModExtension*> Extensions = ExtensionRegistry->GetAllExtensions(Descriptor.ExtensionPointId);
		for (const UModExtension* Extension : Extensions)
		{
			if (!IsValid(Extension))
			{
				continue;
			}

			++ExtensionCount;
			const bool bActive = Extension->IsExtensionActive();
			if (bActive)
			{
				++PointActiveCount;
				++ActiveExtensionCount;
			}

			TSharedRef<FModInspectorNode> ExtensionNode = MakeShared<FModInspectorNode>();
			ExtensionNode->bIsDimmed = !bActive;
			ExtensionNode->SetColumn(ColumnName, FText::FromName(Extension->GetResolvedExtensionId()));
			ExtensionNode->SetColumn(ColumnOwner, ModSDKWidgetUtils::OrDash(Extension->OwningModId.Value));
			ExtensionNode->SetColumn(ColumnPriority, FText::AsNumber(Extension->Priority));
			ExtensionNode->SetColumn(ColumnState, bActive
				? LOCTEXT("ExtensionActive", "Active")
				: LOCTEXT("ExtensionInactive", "Registered, inactive"));

			const UClass* ExtensionClass = Extension->GetClass();
			const FText ClassText = ExtensionClass
				? FText::FromString(ExtensionClass->GetName())
				: LOCTEXT("UnknownClass", "<unknown class>");

			ExtensionNode->SetColumn(ColumnDetail, Extension->ClaimedResourceIds.Num() > 0
				? FText::Format(LOCTEXT("ExtensionDetailClaimsFmt", "{0}  ·  claims: {1}"),
					ClassText, ModSDKWidgetUtils::JoinNames(Extension->ClaimedResourceIds))
				: ClassText);

			ExtensionNode->ToolTip = FText::Format(
				LOCTEXT("ExtensionToolTipFmt", "{0}\nOwning mod: {1}\nPriority: {2}\n{3}"),
				ExtensionClass ? FText::FromString(ExtensionClass->GetPathName()) : ClassText,
				ModSDKWidgetUtils::OrDash(Extension->OwningModId.Value),
				FText::AsNumber(Extension->Priority),
				bActive
					? LOCTEXT("ExtensionToolTipActive", "Returned by GetExtensions, so game code sees it.")
					: LOCTEXT("ExtensionToolTipInactive",
						"Not returned by GetExtensions. The owning mod is loaded but not activated."));

			PointNode->Children.Add(ExtensionNode);
		}

		PointNode->SetColumn(ColumnState, Extensions.Num() == 0
			? LOCTEXT("PointEmpty", "No extensions")
			: FText::Format(LOCTEXT("PointCountFmt", "{0} of {1} active"),
				FText::AsNumber(PointActiveCount), FText::AsNumber(Extensions.Num())));

		//~ Filtering -------------------------------------------------------------------------------

		if (!SearchText.IsEmpty() && !PointNode->MatchesFilter(SearchText))
		{
			PointNode->Children.RemoveAll([this](const TSharedPtr<FModInspectorNode>& InChild)
			{
				return !InChild.IsValid() || !InChild->MatchesFilter(SearchText);
			});

			if (PointNode->Children.Num() == 0)
			{
				continue;
			}
		}

		RootNodes.Add(PointNode);
	}

	if (TreeView.IsValid())
	{
		TreeView->RequestTreeRefresh();

		for (const TSharedPtr<FModInspectorNode>& Node : RootNodes)
		{
			if (Node.IsValid())
			{
				TreeView->SetItemExpansion(Node, true);
			}
		}
	}
}

void SModExtensionInspectorPanel::HandleSearchTextChanged(const FText& InText)
{
	SearchText = InText.ToString().TrimStartAndEnd();
	RebuildTree();
}

FReply SModExtensionInspectorPanel::HandleRefreshClicked()
{
	Reattach();
	return FReply::Handled();
}

FText SModExtensionInspectorPanel::GetSummaryText() const
{
	if (ExtensionPointCount == 0)
	{
		return LOCTEXT("NoExtensionPoints",
			"This game has not opened any extension points. Mods cannot contribute anything until it does: an extension whose "
			"point is unknown is rejected with Extension.PointNotFound rather than queued.");
	}

	return FText::Format(
		LOCTEXT("ExtensionSummaryFmt", "{0} extension point(s)  ·  {1} extension(s) registered, {2} active"),
		FText::AsNumber(ExtensionPointCount),
		FText::AsNumber(ExtensionCount),
		FText::AsNumber(ActiveExtensionCount));
}

EVisibility SModExtensionInspectorPanel::GetNoSubsystemVisibility() const
{
	return bHasSubsystem ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SModExtensionInspectorPanel::GetTreeVisibility() const
{
	return bHasSubsystem ? EVisibility::Visible : EVisibility::Collapsed;
}

#undef LOCTEXT_NAMESPACE
