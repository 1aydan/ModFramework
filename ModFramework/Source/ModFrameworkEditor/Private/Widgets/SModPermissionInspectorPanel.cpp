// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModPermissionInspectorPanel.h"

#include "Widgets/ModSDKWidgetUtils.h"
#include "Widgets/SModInspectorRow.h"

#include "Editor.h"
#include "Internationalization/Internationalization.h"
#include "Layout/Margin.h"
#include "Manifest/ModManifest.h"
#include "Permissions/ModPermissionRegistry.h"
#include "Permissions/ModPermissions.h"
#include "Registry/ModInfo.h"
#include "Registry/ModRegistry.h"
#include "Settings/ModFrameworkSettings.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystem/ModSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "SModPermissionInspectorPanel"

namespace ModPermissionInspectorPrivate
{
	const FName ColumnName(TEXT("Name"));
	const FName ColumnState(TEXT("State"));
	const FName ColumnRule(TEXT("Rule"));
	const FName ColumnDetail(TEXT("Detail"));

	FText DescribeState(EModPermissionState InState)
	{
		switch (InState)
		{
		case EModPermissionState::Granted:
			return LOCTEXT("StateGranted", "Granted");
		case EModPermissionState::Denied:
			return LOCTEXT("StateDenied", "Denied");
		case EModPermissionState::Pending:
			return LOCTEXT("StatePending", "Pending");
		default:
			return LOCTEXT("StateNotRequested", "Not evaluated");
		}
	}
}

void SModPermissionInspectorPanel::Construct(const FArguments& InArgs)
{
	using namespace ModPermissionInspectorPrivate;

	SAssignNew(TreeView, STreeView<TSharedPtr<FModInspectorNode>>)
		.TreeItemsSource(&RootNodes)
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SModPermissionInspectorPanel::HandleGenerateRow)
		.OnGetChildren(this, &SModPermissionInspectorPanel::HandleGetChildren)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SHeaderRow::Column(ColumnName)
			.DefaultLabel(LOCTEXT("ColName", "Permission / mod"))
			.FillWidth(0.26f)
			+ SHeaderRow::Column(ColumnState)
			.DefaultLabel(LOCTEXT("ColState", "State"))
			.FillWidth(0.14f)
			+ SHeaderRow::Column(ColumnRule)
			.DefaultLabel(LOCTEXT("ColRule", "Deciding rule"))
			.FillWidth(0.30f)
			+ SHeaderRow::Column(ColumnDetail)
			.DefaultLabel(LOCTEXT("ColDetail", "Description"))
			.FillWidth(0.30f));

	ChildSlot
	.Padding(12.0f)
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			ModSDKWidgetUtils::MakeSectionHeader(
				LOCTEXT("PermissionsTitle", "Permissions"),
				LOCTEXT("PermissionsSubtitle",
					"The capability catalogue this game publishes, and what each loaded mod was actually allowed. Permissions are "
					"API access control, not a sandbox: they decide which of the game's APIs and extension points a mod can reach."))
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
				.HintText(LOCTEXT("SearchHint", "Filter permissions, mods and rules"))
				.OnTextChanged(this, &SModPermissionInspectorPanel::HandleSearchTextChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ToolTipText(LOCTEXT("RefreshTooltip",
					"Re-read the registries. Decisions already refresh on the registry's own change delegates."))
				.OnClicked(this, &SModPermissionInspectorPanel::HandleRefreshClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("RefreshLabel", "Refresh"))
				]
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.Visibility(this, &SModPermissionInspectorPanel::GetNoSubsystemVisibility)
			[
				ModSDKWidgetUtils::MakeNotice(
					LOCTEXT("NoSubsystemTitle", "Showing the builtin catalogue only"),
					LOCTEXT("NoSubsystemBody",
						"No game instance is running, so there is no permission registry: the permissions a game registers at "
						"runtime, and every per-mod decision, are missing. The builtin capabilities below are compiled in and are "
						"correct as shown. Start Play In Editor for the rest."),
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
			.Text(this, &SModPermissionInspectorPanel::GetSummaryText)
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			.Padding(2.0f)
			[
				TreeView.ToSharedRef()
			]
		]
	];

	PIEStartedHandle = FEditorDelegates::PostPIEStarted.AddSP(this, &SModPermissionInspectorPanel::HandlePIEStarted);
	PIEEndedHandle = FEditorDelegates::EndPIE.AddSP(this, &SModPermissionInspectorPanel::HandlePIEEnded);

	Reattach();
}

SModPermissionInspectorPanel::~SModPermissionInspectorPanel()
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

void SModPermissionInspectorPanel::BindToRegistries()
{
	UModSubsystem* Subsystem = ModSDKWidgetUtils::FindLiveModSubsystem();
	if (!Subsystem)
	{
		return;
	}

	if (UModPermissionRegistry* PermissionRegistry = Subsystem->GetPermissionRegistry())
	{
		BoundPermissionRegistry = PermissionRegistry;
		PermissionChangedHandle = PermissionRegistry->OnPermissionChanged.AddSP(
			this, &SModPermissionInspectorPanel::HandlePermissionChanged);
		PermissionRequestedHandle = PermissionRegistry->OnPermissionRequested.AddSP(
			this, &SModPermissionInspectorPanel::HandlePermissionChanged);
	}

	if (UModRegistry* ModRegistry = Subsystem->GetRegistry())
	{
		BoundModRegistry = ModRegistry;
		ModStateChangedHandle = ModRegistry->OnModStateChanged.AddSP(
			this, &SModPermissionInspectorPanel::HandleModStateChanged);
	}
}

void SModPermissionInspectorPanel::UnbindFromRegistries()
{
	if (UModPermissionRegistry* PermissionRegistry = BoundPermissionRegistry.Get())
	{
		if (PermissionChangedHandle.IsValid())
		{
			PermissionRegistry->OnPermissionChanged.Remove(PermissionChangedHandle);
		}
		if (PermissionRequestedHandle.IsValid())
		{
			PermissionRegistry->OnPermissionRequested.Remove(PermissionRequestedHandle);
		}
	}

	if (UModRegistry* ModRegistry = BoundModRegistry.Get())
	{
		if (ModStateChangedHandle.IsValid())
		{
			ModRegistry->OnModStateChanged.Remove(ModStateChangedHandle);
		}
	}

	PermissionChangedHandle.Reset();
	PermissionRequestedHandle.Reset();
	ModStateChangedHandle.Reset();
	BoundPermissionRegistry.Reset();
	BoundModRegistry.Reset();
}

void SModPermissionInspectorPanel::Reattach()
{
	UnbindFromRegistries();
	BindToRegistries();
	RebuildTree();
}

void SModPermissionInspectorPanel::HandlePermissionChanged(const FModId& InModId, FName InPermissionId)
{
	RebuildTree();
}

void SModPermissionInspectorPanel::HandleModStateChanged(const FModId& InModId, EModState InOldState, EModState InNewState)
{
	RebuildTree();
}

void SModPermissionInspectorPanel::HandlePIEStarted(const bool bIsSimulating)
{
	Reattach();
}

void SModPermissionInspectorPanel::HandlePIEEnded(const bool bIsSimulating)
{
	UnbindFromRegistries();
	ClearLiveState();
}

void SModPermissionInspectorPanel::ClearLiveState()
{
	RootNodes.Reset();
	CataloguedPermissionCount = 0;
	DecidedModCount = 0;
	GrantedCount = 0;
	PendingCount = 0;
	DeniedCount = 0;
	bHasSubsystem = false;

	// The builtin catalogue is compiled in and stays true with no game running, so the tab keeps the
	// half of its content that is still correct rather than going blank.
	if (TSharedPtr<FModInspectorNode> Catalogue = BuildCatalogueNode(/*InRegistry*/ nullptr))
	{
		RootNodes.Add(Catalogue);
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

TSharedRef<ITableRow> SModPermissionInspectorPanel::HandleGenerateRow(TSharedPtr<FModInspectorNode> InNode,
	const TSharedRef<STableViewBase>& InOwnerTable)
{
	return SNew(SModInspectorRow, InOwnerTable)
		.Node(InNode)
		.PrimaryColumn(ModPermissionInspectorPrivate::ColumnName);
}

void SModPermissionInspectorPanel::HandleGetChildren(TSharedPtr<FModInspectorNode> InNode,
	TArray<TSharedPtr<FModInspectorNode>>& OutChildren)
{
	if (InNode.IsValid())
	{
		OutChildren = InNode->Children;
	}
}

TSharedPtr<FModInspectorNode> SModPermissionInspectorPanel::BuildCatalogueNode(const UModPermissionRegistry* InRegistry)
{
	using namespace ModPermissionInspectorPrivate;

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();

	// Without a registry the builtins are still knowable: ModPermissions exposes its descriptors
	// precisely so tooling can describe them without spinning one up.
	const TArray<FModPermissionDescriptor> Descriptors = InRegistry
		? InRegistry->GetAllPermissions()
		: ModPermissions::GetBuiltinPermissionDescriptors();

	CataloguedPermissionCount = Descriptors.Num();

	TSharedRef<FModInspectorNode> Root = MakeShared<FModInspectorNode>();
	Root->bIsGroup = true;
	Root->SetColumn(ColumnName, FText::Format(
		LOCTEXT("CatalogueFmt", "Permission catalogue ({0})"), FText::AsNumber(Descriptors.Num())));
	Root->SetColumn(ColumnState, InRegistry
		? LOCTEXT("CatalogueLive", "Registered")
		: LOCTEXT("CatalogueBuiltinOnly", "Builtins only"));
	Root->ToolTip = InRegistry
		? LOCTEXT("CatalogueToolTipLive", "Every permission the framework and the game registered in this session.")
		: LOCTEXT("CatalogueToolTipBuiltin",
			"The framework's builtin capabilities. Anything the game registers with RegisterPermission is missing until a game "
			"instance exists.");

	for (const FModPermissionDescriptor& Descriptor : Descriptors)
	{
		TSharedRef<FModInspectorNode> Node = MakeShared<FModInspectorNode>();
		Node->SetColumn(ColumnName, ModSDKWidgetUtils::OrDash(Descriptor.PermissionId));
		Node->SetColumn(ColumnState, Descriptor.bDangerous
			? LOCTEXT("PermissionDangerous", "Dangerous")
			: LOCTEXT("PermissionOrdinary", "Ordinary"));

		TArray<FText> Standing;
		if (Settings && Settings->AlwaysDeniedPermissions.Contains(Descriptor.PermissionId))
		{
			Standing.Add(LOCTEXT("StandingAlwaysDenied", "listed in AlwaysDeniedPermissions"));
		}
		if (Settings && Settings->AutoGrantedPermissions.Contains(Descriptor.PermissionId))
		{
			Standing.Add(Descriptor.bDangerous
				? LOCTEXT("StandingAutoGrantIgnored",
					"listed in AutoGrantedPermissions, but dangerous - the framework refuses to auto-grant it")
				: LOCTEXT("StandingAutoGrant", "listed in AutoGrantedPermissions"));
		}

		Node->SetColumn(ColumnRule, Standing.Num() > 0
			? FText::Join(LOCTEXT("StandingSeparator", "; "), Standing)
			: LOCTEXT("StandingNone", "no standing project rule"));

		const FText Description = Descriptor.Description.IsEmpty()
			? Descriptor.DisplayName
			: Descriptor.Description;
		Node->SetColumn(ColumnDetail, Description.IsEmpty()
			? ModSDKWidgetUtils::OrDash(FString())
			: Description);

		Node->ToolTip = FText::Format(LOCTEXT("PermissionToolTipFmt", "{0}\n\n{1}"),
			Descriptor.DisplayName.IsEmpty() ? FText::FromName(Descriptor.PermissionId) : Descriptor.DisplayName,
			Description);

		if (Descriptor.bDangerous)
		{
			Node->Icon = ModSDKWidgetUtils::GetSeverityBrush(EModDiagnosticSeverity::Warning);
			Node->IconColor = ModSDKWidgetUtils::GetSeverityColor(EModDiagnosticSeverity::Warning);
		}

		Root->Children.Add(Node);
	}

	return Root;
}

TSharedPtr<FModInspectorNode> SModPermissionInspectorPanel::BuildDecisionsNode(const UModPermissionRegistry* InRegistry,
	const UModRegistry* InModRegistry)
{
	using namespace ModPermissionInspectorPrivate;

	if (!InRegistry || !InModRegistry)
	{
		return nullptr;
	}

	const TArray<FModInfo> Mods = InModRegistry->GetAllMods();

	TSharedRef<FModInspectorNode> Root = MakeShared<FModInspectorNode>();
	Root->bIsGroup = true;
	Root->ToolTip = LOCTEXT("DecisionsToolTip",
		"One row per permission each mod asked for, plus anything it holds that its manifest did not request.");

	for (const FModInfo& Info : Mods)
	{
		++DecidedModCount;

		TSharedRef<FModInspectorNode> ModNode = MakeShared<FModInspectorNode>();
		ModNode->bIsGroup = true;
		ModNode->bIsDimmed = !Info.bEnabled;

		const FString ModIdString = Info.GetId().ToString();
		const FString DisplayName = Info.Manifest.GetDisplayNameOrId();

		ModNode->SetColumn(ColumnName, DisplayName.Equals(ModIdString)
			? FText::FromString(ModIdString)
			: FText::Format(LOCTEXT("ModLabelFmt", "{0}  ({1})"),
				FText::FromString(DisplayName), FText::FromString(ModIdString)));
		ModNode->SetColumn(ColumnDetail, FText::FromString(ModFrameworkEnums::ToString(Info.State)));

		// The union of "asked for" and "holds": a permission granted outside the manifest still
		// governs what the mod can reach, so hiding it would misrepresent the mod's real reach.
		TArray<FName> Permissions = Info.Manifest.RequestedPermissions;
		for (const FName& Granted : Info.GrantedPermissions)
		{
			Permissions.AddUnique(Granted);
		}
		Permissions.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

		int32 ModGranted = 0;
		int32 ModPending = 0;
		int32 ModDenied = 0;

		for (const FName& PermissionId : Permissions)
		{
			if (PermissionId.IsNone())
			{
				continue;
			}

			const EModPermissionState State = InRegistry->GetPermissionState(Info.GetId(), PermissionId);
			switch (State)
			{
			case EModPermissionState::Granted:
				++ModGranted;
				++GrantedCount;
				break;
			case EModPermissionState::Pending:
				++ModPending;
				++PendingCount;
				break;
			case EModPermissionState::Denied:
				++ModDenied;
				++DeniedCount;
				break;
			default:
				break;
			}

			TSharedRef<FModInspectorNode> PermissionNode = MakeShared<FModInspectorNode>();
			PermissionNode->SetColumn(ColumnName, FText::FromName(PermissionId));
			PermissionNode->SetColumn(ColumnState, DescribeState(State));
			PermissionNode->SetColumn(ColumnRule, DescribeDecidingRule(PermissionId, State, InRegistry));

			FModPermissionDescriptor Descriptor;
			const bool bRegistered = InRegistry->GetPermission(PermissionId, Descriptor);
			if (bRegistered)
			{
				PermissionNode->SetColumn(ColumnDetail, Descriptor.Description.IsEmpty()
					? Descriptor.DisplayName
					: Descriptor.Description);
			}
			else
			{
				// Nothing registered this id. That is almost always a typo in the manifest, and it is
				// worth shouting about because the mod will silently run without the capability.
				PermissionNode->SetColumn(ColumnDetail, LOCTEXT("PermissionUnregistered",
					"Nothing registered this permission. Almost always a typo in the mod's manifest."));
				PermissionNode->Icon = ModSDKWidgetUtils::GetSeverityBrush(EModDiagnosticSeverity::Error);
				PermissionNode->IconColor = ModSDKWidgetUtils::GetSeverityColor(EModDiagnosticSeverity::Error);
			}

			if (bRegistered && State == EModPermissionState::Pending)
			{
				PermissionNode->Icon = ModSDKWidgetUtils::GetSeverityBrush(EModDiagnosticSeverity::Warning);
				PermissionNode->IconColor = ModSDKWidgetUtils::GetSeverityColor(EModDiagnosticSeverity::Warning);
			}

			PermissionNode->bIsDimmed = State == EModPermissionState::NotRequested;
			PermissionNode->ToolTip = FText::Format(
				LOCTEXT("DecisionToolTipFmt", "{0}\nMod: {1}\nState: {2}\n{3}"),
				FText::FromName(PermissionId),
				FText::FromString(ModIdString),
				DescribeState(State),
				DescribeDecidingRule(PermissionId, State, InRegistry));

			ModNode->Children.Add(PermissionNode);
		}

		ModNode->SetColumn(ColumnState, Permissions.Num() == 0
			? LOCTEXT("ModNoPermissions", "Requests nothing")
			: FText::Format(LOCTEXT("ModPermissionCountFmt", "{0} granted, {1} pending, {2} denied"),
				FText::AsNumber(ModGranted), FText::AsNumber(ModPending), FText::AsNumber(ModDenied)));

		Root->Children.Add(ModNode);
	}

	Root->SetColumn(ColumnName, FText::Format(
		LOCTEXT("DecisionsFmt", "Resolved decisions ({0} mod(s))"), FText::AsNumber(Mods.Num())));

	return Root;
}

FText SModPermissionInspectorPanel::DescribeDecidingRule(FName InPermissionId, EModPermissionState InObserved,
	const UModPermissionRegistry* InRegistry) const
{
	if (InObserved == EModPermissionState::NotRequested)
	{
		return LOCTEXT("RuleNeverEvaluated",
			"Never evaluated for this mod - the manifest was not run through EvaluateManifest.");
	}

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();

	const bool bAlwaysDenied = Settings && Settings->AlwaysDeniedPermissions.Contains(InPermissionId);
	const bool bAutoGranted = Settings && Settings->AutoGrantedPermissions.Contains(InPermissionId);
	const bool bDenyUnknown = !Settings || Settings->bDenyUnknownPermissions;

	FModPermissionDescriptor Descriptor;
	const bool bRegistered = InRegistry && InRegistry->GetPermission(InPermissionId, Descriptor);
	const bool bDangerous = bRegistered && Descriptor.bDangerous;

	// Replay of the documented order in UModPermissionRegistry. Rule 1 - the game's own policy - is
	// deliberately not modelled: it is arbitrary game code and the registry exposes no way to ask
	// what it would say, which is exactly why a disagreement below is reported rather than guessed at.
	EModPermissionState Predicted = EModPermissionState::Pending;
	FText Rule;

	if (bAlwaysDenied)
	{
		Predicted = EModPermissionState::Denied;
		Rule = LOCTEXT("RuleAlwaysDenied", "Project setting: AlwaysDeniedPermissions.");
	}
	else if (!bRegistered)
	{
		Predicted = bDenyUnknown ? EModPermissionState::Denied : EModPermissionState::Pending;
		Rule = bDenyUnknown
			? LOCTEXT("RuleUnknownDenied", "Nothing registered this permission, and bDenyUnknownPermissions is set.")
			: LOCTEXT("RuleUnknownPending", "Nothing registered this permission; bDenyUnknownPermissions is off.");
	}
	else if (bAutoGranted && !bDangerous)
	{
		Predicted = EModPermissionState::Granted;
		Rule = LOCTEXT("RuleAutoGranted", "Project setting: AutoGrantedPermissions, and not dangerous.");
	}
	else if (bAutoGranted && bDangerous)
	{
		Predicted = EModPermissionState::Pending;
		Rule = LOCTEXT("RuleDangerousNotAutoGranted",
			"Listed in AutoGrantedPermissions but marked dangerous, so the framework refuses to auto-grant it.");
	}
	else
	{
		Predicted = EModPermissionState::Pending;
		Rule = LOCTEXT("RuleDefaultPending", "No rule applies, so the request awaits a decision.");
	}

	if (Predicted == InObserved)
	{
		return Rule;
	}

	return FText::Format(
		LOCTEXT("RuleOverriddenFmt",
			"Decided by a permission policy or an explicit grant. The default rules would have said {0}: {1}"),
		ModPermissionInspectorPrivate::DescribeState(Predicted),
		Rule);
}

void SModPermissionInspectorPanel::RebuildTree()
{
	RootNodes.Reset();
	CataloguedPermissionCount = 0;
	DecidedModCount = 0;
	GrantedCount = 0;
	PendingCount = 0;
	DeniedCount = 0;

	UModSubsystem* Subsystem = ModSDKWidgetUtils::FindLiveModSubsystem();
	const UModPermissionRegistry* PermissionRegistry = Subsystem ? Subsystem->GetPermissionRegistry() : nullptr;
	const UModRegistry* ModRegistry = Subsystem ? Subsystem->GetRegistry() : nullptr;
	bHasSubsystem = PermissionRegistry != nullptr;

	TArray<TSharedPtr<FModInspectorNode>> Candidates;
	Candidates.Add(BuildCatalogueNode(PermissionRegistry));
	Candidates.Add(BuildDecisionsNode(PermissionRegistry, ModRegistry));

	for (const TSharedPtr<FModInspectorNode>& Candidate : Candidates)
	{
		if (!Candidate.IsValid())
		{
			continue;
		}

		if (!SearchText.IsEmpty())
		{
			// Group rows are structural, so a group only survives on the strength of its children.
			Candidate->Children.RemoveAll([this](const TSharedPtr<FModInspectorNode>& InChild)
			{
				if (!InChild.IsValid())
				{
					return true;
				}
				if (InChild->MatchesFilter(SearchText))
				{
					return false;
				}

				InChild->Children.RemoveAll([this](const TSharedPtr<FModInspectorNode>& InGrandChild)
				{
					return !InGrandChild.IsValid() || !InGrandChild->MatchesFilter(SearchText);
				});

				return InChild->Children.Num() == 0;
			});

			if (Candidate->Children.Num() == 0)
			{
				continue;
			}
		}

		RootNodes.Add(Candidate);
	}

	if (TreeView.IsValid())
	{
		TreeView->RequestTreeRefresh();

		for (const TSharedPtr<FModInspectorNode>& Node : RootNodes)
		{
			if (!Node.IsValid())
			{
				continue;
			}

			TreeView->SetItemExpansion(Node, true);

			// Mod rows are the second level of the decisions branch; expanding them while filtering
			// is what makes a search for a permission id actually show the mods that asked for it.
			if (!SearchText.IsEmpty())
			{
				for (const TSharedPtr<FModInspectorNode>& Child : Node->Children)
				{
					if (Child.IsValid() && Child->Children.Num() > 0)
					{
						TreeView->SetItemExpansion(Child, true);
					}
				}
			}
		}
	}
}

void SModPermissionInspectorPanel::HandleSearchTextChanged(const FText& InText)
{
	SearchText = InText.ToString().TrimStartAndEnd();
	RebuildTree();
}

FReply SModPermissionInspectorPanel::HandleRefreshClicked()
{
	Reattach();
	return FReply::Handled();
}

FText SModPermissionInspectorPanel::GetSummaryText() const
{
	if (!bHasSubsystem)
	{
		return FText::Format(
			LOCTEXT("SummaryBuiltinFmt", "{0} builtin permission(s). No per-mod decisions without a running game instance."),
			FText::AsNumber(CataloguedPermissionCount));
	}

	return FText::Format(
		LOCTEXT("SummaryLiveFmt",
			"{0} permission(s) registered  ·  {1} mod(s)  ·  {2} granted, {3} pending, {4} denied"),
		FText::AsNumber(CataloguedPermissionCount),
		FText::AsNumber(DecidedModCount),
		FText::AsNumber(GrantedCount),
		FText::AsNumber(PendingCount),
		FText::AsNumber(DeniedCount));
}

EVisibility SModPermissionInspectorPanel::GetNoSubsystemVisibility() const
{
	return bHasSubsystem ? EVisibility::Collapsed : EVisibility::Visible;
}

#undef LOCTEXT_NAMESPACE
