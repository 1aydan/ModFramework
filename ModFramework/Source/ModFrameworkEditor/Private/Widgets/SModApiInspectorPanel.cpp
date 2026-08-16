// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModApiInspectorPanel.h"

#include "ModFrameworkEditorModule.h"
#include "Widgets/ModSDKWidgetUtils.h"
#include "Widgets/SModDiagnosticList.h"
#include "Widgets/SModInspectorRow.h"

#include "Containers/Map.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Internationalization/Internationalization.h"
#include "Layout/Margin.h"
#include "Logging/LogMacros.h"
#include "Misc/CString.h"
#include "Misc/ScopedSlowTask.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Textures/SlateIcon.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "SModApiInspectorPanel"

namespace ModApiInspectorPrivate
{
	const FName ColumnSymbol(TEXT("Symbol"));
	const FName ColumnId(TEXT("Id"));
	const FName ColumnVersion(TEXT("Version"));
	const FName ColumnPermissions(TEXT("Permissions"));
	const FName ColumnAuthority(TEXT("Authority"));
	const FName ColumnModule(TEXT("Module"));
	const FName ColumnHeader(TEXT("Header"));

	/** "Class" / "Interface" / "Struct" / "Enum", for tooltips. */
	FText DescribeKind(EModPublicSymbolKind InKind)
	{
		switch (InKind)
		{
		case EModPublicSymbolKind::Interface:
			return LOCTEXT("KindInterface", "Interface");
		case EModPublicSymbolKind::Struct:
			return LOCTEXT("KindStruct", "Struct");
		case EModPublicSymbolKind::Enum:
			return LOCTEXT("KindEnum", "Enum");
		default:
			return LOCTEXT("KindClass", "Class");
		}
	}

	/** Everything worth knowing about a scanned type that does not fit in a column. */
	FText BuildTypeToolTip(const FModPublicTypeInfo& InType, const FModPublicApiEntry* InApiEntry)
	{
		TArray<FText> Lines;

		Lines.Add(FText::Format(LOCTEXT("ToolTipKindFmt", "{0}  ·  {1}"),
			DescribeKind(InType.Kind), FText::FromString(InType.PathName)));

		if (!InType.SuperName.IsEmpty())
		{
			Lines.Add(FText::Format(LOCTEXT("ToolTipSuperFmt", "Derives from {0}"), FText::FromString(InType.SuperName)));
		}

		TArray<FText> Flags;
		if (InType.bIsAbstract)
		{
			Flags.Add(LOCTEXT("FlagAbstract", "abstract"));
		}
		if (InType.bIsBlueprintable)
		{
			Flags.Add(LOCTEXT("FlagBlueprintable", "Blueprintable"));
		}
		if (InType.bIsBlueprintType)
		{
			Flags.Add(LOCTEXT("FlagBlueprintType", "BlueprintType"));
		}
		if (!InType.bIsNative)
		{
			Flags.Add(LOCTEXT("FlagBlueprintGenerated", "Blueprint-generated"));
		}
		if (Flags.Num() > 0)
		{
			Lines.Add(FText::Join(LOCTEXT("FlagSeparator", ", "), Flags));
		}

		if (!InType.Metadata.Since.IsEmpty())
		{
			Lines.Add(FText::Format(LOCTEXT("ToolTipSinceFmt", "Since SDK {0}"), FText::FromString(InType.Metadata.Since)));
		}
		if (InType.Metadata.IsDeprecated())
		{
			Lines.Add(FText::Format(LOCTEXT("ToolTipDeprecatedFmt", "DEPRECATED in SDK {0}"),
				FText::FromString(InType.Metadata.Deprecated)));
		}

		if (InApiEntry)
		{
			if (!InApiEntry->bNativeIdentity)
			{
				Lines.Add(LOCTEXT("ToolTipNoNativeIdentity",
					"Identity comes from class metadata only. Metadata is stripped from cooked builds, so a shipped game "
					"registers this API under a different id than the editor does. Override NativeGetApiId."));
			}
			else if (!InApiEntry->NativeApiId.IsEmpty() && !InApiEntry->NativeApiId.Equals(InApiEntry->ApiId, ESearchCase::IgnoreCase))
			{
				Lines.Add(FText::Format(LOCTEXT("ToolTipIdentityMismatchFmt",
					"NativeGetApiId returns \"{0}\", which disagrees with the declared id."),
					FText::FromString(InApiEntry->NativeApiId)));
			}
		}

		Lines.Add(FText::Format(LOCTEXT("ToolTipMemberCountFmt", "{0} marked function(s), {1} marked property(s)"),
			FText::AsNumber(InType.CountMarkedFunctions()),
			FText::AsNumber(InType.CountMarkedProperties())));

		return FText::Join(FText::FromString(TEXT("\n")), Lines);
	}

	FText BuildFunctionToolTip(const FModPublicFunctionInfo& InFunction)
	{
		TArray<FText> Flags;
		if (InFunction.bBlueprintCallable)
		{
			Flags.Add(LOCTEXT("FnBlueprintCallable", "BlueprintCallable"));
		}
		if (InFunction.bBlueprintPure)
		{
			Flags.Add(LOCTEXT("FnBlueprintPure", "BlueprintPure"));
		}
		if (InFunction.bBlueprintEvent)
		{
			Flags.Add(LOCTEXT("FnBlueprintEvent", "BlueprintEvent"));
		}
		if (InFunction.bBlueprintImplementable)
		{
			Flags.Add(LOCTEXT("FnBlueprintImplementable", "BlueprintImplementableEvent"));
		}
		if (InFunction.bStatic)
		{
			Flags.Add(LOCTEXT("FnStatic", "static"));
		}
		if (!InFunction.bNative)
		{
			Flags.Add(LOCTEXT("FnScript", "script-implemented"));
		}

		TArray<FText> Lines;
		Lines.Add(FText::FromString(InFunction.Signature));
		Lines.Add(FText::Format(LOCTEXT("FnOwnerFmt", "Declared by {0}"), FText::FromString(InFunction.OwnerPath)));
		if (Flags.Num() > 0)
		{
			Lines.Add(FText::Join(LOCTEXT("FlagSeparator", ", "), Flags));
		}
		if (InFunction.Metadata.IsDeprecated())
		{
			Lines.Add(FText::Format(LOCTEXT("FnDeprecatedFmt", "DEPRECATED in SDK {0}"),
				FText::FromString(InFunction.Metadata.Deprecated)));
		}

		return FText::Join(FText::FromString(TEXT("\n")), Lines);
	}

	FText BuildPropertyToolTip(const FModPublicPropertyInfo& InProperty)
	{
		TArray<FText> Flags;
		if (InProperty.bBlueprintVisible)
		{
			Flags.Add(LOCTEXT("PropBlueprintVisible", "Blueprint visible"));
		}
		if (InProperty.bBlueprintReadOnly)
		{
			Flags.Add(LOCTEXT("PropBlueprintReadOnly", "Blueprint read-only"));
		}
		if (InProperty.bEditable)
		{
			Flags.Add(LOCTEXT("PropEditable", "editable"));
		}
		if (InProperty.bConfig)
		{
			Flags.Add(LOCTEXT("PropConfig", "config"));
		}

		TArray<FText> Lines;
		Lines.Add(FText::Format(LOCTEXT("PropSignatureFmt", "{0} {1}"),
			FText::FromString(InProperty.CppType), FText::FromString(InProperty.Name)));
		Lines.Add(FText::Format(LOCTEXT("PropOwnerFmt", "Declared by {0}"), FText::FromString(InProperty.OwnerPath)));
		if (Flags.Num() > 0)
		{
			Lines.Add(FText::Join(LOCTEXT("FlagSeparator", ", "), Flags));
		}
		if (InProperty.Metadata.IsDeprecated())
		{
			Lines.Add(FText::Format(LOCTEXT("PropDeprecatedFmt", "DEPRECATED in SDK {0}"),
				FText::FromString(InProperty.Metadata.Deprecated)));
		}

		return FText::Join(FText::FromString(TEXT("\n")), Lines);
	}
}

void SModApiInspectorPanel::Construct(const FArguments& InArgs)
{
	using namespace ModApiInspectorPrivate;

	SdkPluginName = ModSDKWidgetUtils::PickDefaultSdkPluginName();

	SAssignNew(DiagnosticList, SModDiagnosticList)
		.EmptyMessage(LOCTEXT("NoScanProblems",
			"The scan found nothing that would break a generated SDK. Every marked signature names a type a mod author will have."))
		.MaxHeight(260.0f);

	SAssignNew(TreeView, STreeView<TSharedPtr<FModInspectorNode>>)
		.TreeItemsSource(&RootNodes)
		.SelectionMode(ESelectionMode::Single)
		.OnGenerateRow(this, &SModApiInspectorPanel::HandleGenerateRow)
		.OnGetChildren(this, &SModApiInspectorPanel::HandleGetChildren)
		.HeaderRow(
			SNew(SHeaderRow)
			+ SHeaderRow::Column(ColumnSymbol)
			.DefaultLabel(LOCTEXT("ColSymbol", "Symbol"))
			.FillWidth(0.34f)
			+ SHeaderRow::Column(ColumnId)
			.DefaultLabel(LOCTEXT("ColId", "Id"))
			.FillWidth(0.14f)
			+ SHeaderRow::Column(ColumnVersion)
			.DefaultLabel(LOCTEXT("ColVersion", "Version"))
			.FillWidth(0.07f)
			+ SHeaderRow::Column(ColumnPermissions)
			.DefaultLabel(LOCTEXT("ColPermissions", "Permissions"))
			.FillWidth(0.14f)
			+ SHeaderRow::Column(ColumnAuthority)
			.DefaultLabel(LOCTEXT("ColAuthority", "Authority"))
			.FillWidth(0.08f)
			+ SHeaderRow::Column(ColumnModule)
			.DefaultLabel(LOCTEXT("ColModule", "Module"))
			.FillWidth(0.10f)
			+ SHeaderRow::Column(ColumnHeader)
			.DefaultLabel(LOCTEXT("ColHeader", "Header"))
			.FillWidth(0.13f));

	ChildSlot
	.Padding(12.0f)
	[
		SNew(SVerticalBox)

		//~ Header --------------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			ModSDKWidgetUtils::MakeSectionHeader(
				LOCTEXT("InspectorTitle", "Public modding surface"),
				LOCTEXT("InspectorSubtitle",
					"Every symbol this project marks ModPublic, as FModPublicApiScanner sees it. This is what a generated SDK "
					"documents, so what is missing here is missing from the SDK too."))
		]

		//~ Toolbar -------------------------------------------------------------------------------
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
				.HintText(LOCTEXT("SearchHint", "Filter symbols, ids, modules and headers"))
				.OnTextChanged(this, &SModApiInspectorPanel::HandleSearchTextChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SComboButton)
				.ToolTipText(LOCTEXT("SdkPluginScopeTooltip",
					"Which plugin ships inside the bundle. Unmarked types from it - and from ModFramework - are reachable by a "
					"mod author and so are not leaks. Get this wrong and every helper type in your SDK is reported as one."))
				.OnGetMenuContent(this, &SModApiInspectorPanel::BuildSdkPluginMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text(this, &SModApiInspectorPanel::GetSdkPluginButtonText)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("IncludeBlueprintTooltip",
					"Include Blueprint-generated classes. Off by default: an SDK is a C++ surface and Blueprint classes are content."))
				.IsChecked(this, &SModApiInspectorPanel::GetIncludeBlueprintTypesState)
				.OnCheckStateChanged(this, &SModApiInspectorPanel::HandleIncludeBlueprintTypesChanged)
				[
					SNew(STextBlock).Text(LOCTEXT("IncludeBlueprintLabel", "Blueprint types"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ToolTipText(LOCTEXT("RescanTooltip", "Walk the reflection database again."))
				.OnClicked(this, &SModApiInspectorPanel::HandleRescanClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("RescanLabel", "Rescan"))
				]
			]
		]

		//~ Stale / unavailable notices -----------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.Visibility(this, &SModApiInspectorPanel::GetStaleNoticeVisibility)
			[
				ModSDKWidgetUtils::MakeNotice(
					LOCTEXT("StaleTitle", "Reflection data changed since this scan"),
					LOCTEXT("StaleBody",
						"Code was recompiled, so the classes below may no longer exist. Press Rescan."),
					EModDiagnosticSeverity::Warning)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.Visibility(this, &SModApiInspectorPanel::GetMetadataUnavailableVisibility)
			[
				ModSDKWidgetUtils::MakeNotice(
					LOCTEXT("NoMetadataTitle", "Reflection metadata is compiled out of this build"),
					LOCTEXT("NoMetadataBody",
						"WITH_METADATA is 0 here, so no ModPublic marking can be seen at all and the scan can only report nothing. "
						"An empty report in this state means \"could not look\", not \"nothing is marked\". Run the editor, or the "
						"GenerateModSDK commandlet against an editor build."),
					EModDiagnosticSeverity::Error)
			]
		]

		//~ The warnings, first and expanded -------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SExpandableArea)
			.InitiallyCollapsed(false)
			.HeaderContent()
			[
				SNew(STextBlock)
				.Font(FAppStyle::Get().GetFontStyle(TEXT("NormalFontBold")))
				.Text(this, &SModApiInspectorPanel::GetWarningHeaderText)
			]
			.BodyContent()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 6.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FStyleColors::Foreground)
					.Text(LOCTEXT("WarningsExplainer",
						"An unmarked type named by a marked signature is what makes a generated SDK refuse to compile in the mod "
						"author's project, and it is invisible until they try. Everything listed here is a defect in the published "
						"surface, not in the framework."))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					DiagnosticList.ToSharedRef()
				]
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f)
		[
			SNew(SSeparator)
		]

		//~ Summary + tree -------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FStyleColors::Foreground)
			.Text(this, &SModApiInspectorPanel::GetSummaryText)
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

	// Reflection data is replaced wholesale by live coding and hot reload. Rather than rescan behind
	// the user's back - which would stall the editor at an arbitrary moment - the panel just admits
	// that what is on screen describes the past.
	ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddSP(
		this, &SModApiInspectorPanel::HandleReloadComplete);

	RunScan(/*bAllowProgressDialog*/ false);
}

SModApiInspectorPanel::~SModApiInspectorPanel()
{
	if (ReloadCompleteHandle.IsValid())
	{
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteHandle);
		ReloadCompleteHandle.Reset();
	}
}

void SModApiInspectorPanel::SetReport(const FModPublicApiReport& InReport)
{
	Report = InReport;
	bReportIsStale = false;

	if (DiagnosticList.IsValid())
	{
		DiagnosticList->SetDiagnostics(Report.Diagnostics);
	}

	RebuildTree();
}

void SModApiInspectorPanel::Rescan()
{
	RunScan(/*bAllowProgressDialog*/ true);
}

void SModApiInspectorPanel::RunScan(bool bAllowProgressDialog)
{
	FModPublicApiScanOptions Options;
	Options.bIncludeBlueprintGeneratedTypes = bIncludeBlueprintGeneratedTypes;
	Options.ShippingPluginNames.AddUnique(ModSDKWidgetUtils::FrameworkPluginName);
	if (!SdkPluginName.IsEmpty())
	{
		Options.ShippingPluginNames.AddUnique(SdkPluginName);
	}

	if (bAllowProgressDialog)
	{
		FScopedSlowTask SlowTask(1.0f, LOCTEXT("ScanningSlowTask", "Scanning the public modding surface..."));
		SlowTask.MakeDialog();
		SlowTask.EnterProgressFrame(1.0f);

		SetReport(FModPublicApiScanner::Scan(Options));
	}
	else
	{
		SetReport(FModPublicApiScanner::Scan(Options));
	}

	UE_LOG(LogModFrameworkEditor, Verbose, TEXT("Public API scan: %s (%d types examined in %.3fs)"),
		*Report.DescribeCounts(), Report.TypesExamined, Report.ScanSeconds);
}

TSharedRef<ITableRow> SModApiInspectorPanel::HandleGenerateRow(TSharedPtr<FModInspectorNode> InNode,
	const TSharedRef<STableViewBase>& InOwnerTable)
{
	return SNew(SModInspectorRow, InOwnerTable)
		.Node(InNode)
		.PrimaryColumn(ModApiInspectorPrivate::ColumnSymbol);
}

void SModApiInspectorPanel::HandleGetChildren(TSharedPtr<FModInspectorNode> InNode,
	TArray<TSharedPtr<FModInspectorNode>>& OutChildren)
{
	if (InNode.IsValid())
	{
		OutChildren = InNode->Children;
	}
}

TSharedPtr<FModInspectorNode> SModApiInspectorPanel::BuildTypeNode(const FModPublicTypeInfo& InType,
	const FModPublicApiEntry* InApiEntry) const
{
	using namespace ModApiInspectorPrivate;

	TSharedRef<FModInspectorNode> Node = MakeShared<FModInspectorNode>();

	Node->SetColumn(ColumnSymbol, FText::FromString(InType.Name));
	Node->SetColumn(ColumnModule, ModSDKWidgetUtils::OrDash(InType.ModuleName));
	Node->SetColumn(ColumnHeader, ModSDKWidgetUtils::OrDash(InType.HeaderPath));
	Node->ToolTip = BuildTypeToolTip(InType, InApiEntry);
	Node->bIsDimmed = InType.Metadata.IsDeprecated();

	if (InApiEntry)
	{
		Node->SetColumn(ColumnId, ModSDKWidgetUtils::OrDash(InApiEntry->ApiId));
		Node->SetColumn(ColumnVersion, ModSDKWidgetUtils::OrDash(InApiEntry->Version));
		Node->SetColumn(ColumnPermissions, ModSDKWidgetUtils::JoinStrings(InApiEntry->RequiredPermissions));
		Node->SetColumn(ColumnAuthority, InApiEntry->bServerAuthoritative
			? LOCTEXT("AuthorityServer", "Server only")
			: LOCTEXT("AuthorityAny", "Any"));

		// An id that exists only in editor metadata is a published-contract hazard, so it is flagged
		// on the row itself rather than left to the tooltip.
		if (!InApiEntry->bNativeIdentity)
		{
			Node->Icon = ModSDKWidgetUtils::GetSeverityBrush(EModDiagnosticSeverity::Warning);
			Node->IconColor = ModSDKWidgetUtils::GetSeverityColor(EModDiagnosticSeverity::Warning);
		}
	}
	else
	{
		Node->SetColumn(ColumnId, ModSDKWidgetUtils::OrDash(InType.Metadata.ExtensionPointId));
		Node->SetColumn(ColumnVersion, ModSDKWidgetUtils::OrDash(InType.Metadata.Since));
		Node->SetColumn(ColumnPermissions, ModSDKWidgetUtils::OrDash(FString()));
		Node->SetColumn(ColumnAuthority, ModSDKWidgetUtils::OrDash(FString()));
	}

	//~ Marked members ------------------------------------------------------------------------------

	for (const FModPublicFunctionInfo& Function : InType.Functions)
	{
		if (!Function.bMarkedPublic)
		{
			continue;
		}

		TSharedRef<FModInspectorNode> Child = MakeShared<FModInspectorNode>();
		Child->SetColumn(ColumnSymbol, FText::FromString(Function.Signature.IsEmpty() ? Function.Name : Function.Signature));
		Child->SetColumn(ColumnId, ModSDKWidgetUtils::OrDash(FString()));
		Child->SetColumn(ColumnVersion, ModSDKWidgetUtils::OrDash(Function.Metadata.Since));
		Child->SetColumn(ColumnPermissions, ModSDKWidgetUtils::OrDash(FString()));
		Child->SetColumn(ColumnAuthority, ModSDKWidgetUtils::OrDash(FString()));
		Child->SetColumn(ColumnModule, LOCTEXT("MemberFunction", "function"));
		Child->SetColumn(ColumnHeader, ModSDKWidgetUtils::OrDash(Function.HeaderPath));
		Child->ToolTip = BuildFunctionToolTip(Function);
		Child->bIsDimmed = Function.Metadata.IsDeprecated();
		Node->Children.Add(Child);
	}

	for (const FModPublicPropertyInfo& Property : InType.Properties)
	{
		if (!Property.bMarkedPublic)
		{
			continue;
		}

		TSharedRef<FModInspectorNode> Child = MakeShared<FModInspectorNode>();
		Child->SetColumn(ColumnSymbol, FText::Format(LOCTEXT("PropertyRowFmt", "{0} {1}"),
			FText::FromString(Property.CppType), FText::FromString(Property.Name)));
		Child->SetColumn(ColumnId, ModSDKWidgetUtils::OrDash(FString()));
		Child->SetColumn(ColumnVersion, ModSDKWidgetUtils::OrDash(Property.Metadata.Since));
		Child->SetColumn(ColumnPermissions, ModSDKWidgetUtils::OrDash(FString()));
		Child->SetColumn(ColumnAuthority, ModSDKWidgetUtils::OrDash(FString()));
		Child->SetColumn(ColumnModule, LOCTEXT("MemberProperty", "property"));
		Child->SetColumn(ColumnHeader, ModSDKWidgetUtils::OrDash(Property.HeaderPath));
		Child->ToolTip = BuildPropertyToolTip(Property);
		Child->bIsDimmed = Property.Metadata.IsDeprecated();
		Node->Children.Add(Child);
	}

	for (const FString& Enumerator : InType.Enumerators)
	{
		TSharedRef<FModInspectorNode> Child = MakeShared<FModInspectorNode>();
		Child->SetColumn(ColumnSymbol, FText::FromString(Enumerator));
		Child->SetColumn(ColumnModule, LOCTEXT("MemberEnumerator", "enumerator"));
		Node->Children.Add(Child);
	}

	//~ Filtering -----------------------------------------------------------------------------------

	if (!SearchText.IsEmpty() && !Node->MatchesFilter(SearchText))
	{
		Node->Children.RemoveAll([this](const TSharedPtr<FModInspectorNode>& InChild)
		{
			return !InChild.IsValid() || !InChild->MatchesFilter(SearchText);
		});

		if (Node->Children.Num() == 0)
		{
			return nullptr;
		}
	}

	return Node;
}

void SModApiInspectorPanel::AddCategory(TArray<TSharedPtr<FModInspectorNode>>& OutRoots, const FText& InLabel,
	TArray<TSharedPtr<FModInspectorNode>>&& InChildren) const
{
	if (InChildren.Num() == 0)
	{
		return;
	}

	TSharedRef<FModInspectorNode> Category = MakeShared<FModInspectorNode>();
	Category->bIsGroup = true;
	Category->SetColumn(ModApiInspectorPrivate::ColumnSymbol,
		FText::Format(LOCTEXT("CategoryFmt", "{0} ({1})"), InLabel, FText::AsNumber(InChildren.Num())));
	Category->Children = MoveTemp(InChildren);

	OutRoots.Add(Category);
}

void SModApiInspectorPanel::RebuildTree()
{
	RootNodes.Reset();

	// Index the derived API entries by class path so a class row can show the id, version, permissions
	// and authority the scanner resolved for it rather than re-deriving them here.
	TMap<FString, const FModPublicApiEntry*> ApiEntriesByPath;
	for (const FModPublicApiEntry& Entry : Report.Apis)
	{
		ApiEntriesByPath.Add(Entry.ClassPath, &Entry);
	}

	TArray<TSharedPtr<FModInspectorNode>> ApiNodes;
	TArray<TSharedPtr<FModInspectorNode>> ExtensionNodes;
	TArray<TSharedPtr<FModInspectorNode>> ClassNodes;

	for (const FModPublicTypeInfo& Type : Report.Classes)
	{
		const FModPublicApiEntry* const* Found = ApiEntriesByPath.Find(Type.PathName);
		const FModPublicApiEntry* ApiEntry = Found ? *Found : nullptr;

		TSharedPtr<FModInspectorNode> Node = BuildTypeNode(Type, ApiEntry);
		if (!Node.IsValid())
		{
			continue;
		}

		if (Type.bIsModAPI)
		{
			ApiNodes.Add(Node);
		}
		else if (Type.bIsModExtension)
		{
			ExtensionNodes.Add(Node);
		}
		else
		{
			ClassNodes.Add(Node);
		}
	}

	auto BuildSimpleCategory = [this](const TArray<FModPublicTypeInfo>& InTypes)
	{
		TArray<TSharedPtr<FModInspectorNode>> Nodes;
		for (const FModPublicTypeInfo& Type : InTypes)
		{
			if (TSharedPtr<FModInspectorNode> Node = BuildTypeNode(Type, nullptr))
			{
				Nodes.Add(Node);
			}
		}
		return Nodes;
	};

	AddCategory(RootNodes, LOCTEXT("CategoryApis", "Mod APIs"), MoveTemp(ApiNodes));
	AddCategory(RootNodes, LOCTEXT("CategoryExtensionBases", "Extension base classes"), MoveTemp(ExtensionNodes));
	AddCategory(RootNodes, LOCTEXT("CategoryClasses", "Classes"), MoveTemp(ClassNodes));
	AddCategory(RootNodes, LOCTEXT("CategoryInterfaces", "Interfaces"), BuildSimpleCategory(Report.Interfaces));
	AddCategory(RootNodes, LOCTEXT("CategoryStructs", "Structs"), BuildSimpleCategory(Report.Structs));
	AddCategory(RootNodes, LOCTEXT("CategoryEnums", "Enums"), BuildSimpleCategory(Report.Enums));

	if (TreeView.IsValid())
	{
		TreeView->RequestTreeRefresh();
		ApplyDefaultExpansion();
	}
}

void SModApiInspectorPanel::ApplyDefaultExpansion()
{
	if (!TreeView.IsValid())
	{
		return;
	}

	const bool bFiltering = !SearchText.IsEmpty();

	for (const TSharedPtr<FModInspectorNode>& Category : RootNodes)
	{
		if (!Category.IsValid())
		{
			continue;
		}

		TreeView->SetItemExpansion(Category, true);

		if (!bFiltering)
		{
			continue;
		}

		// While filtering, the matches are the point - showing them collapsed would hide the answer.
		for (const TSharedPtr<FModInspectorNode>& Child : Category->Children)
		{
			if (Child.IsValid() && Child->Children.Num() > 0)
			{
				TreeView->SetItemExpansion(Child, true);
			}
		}
	}
}

void SModApiInspectorPanel::HandleSearchTextChanged(const FText& InText)
{
	SearchText = InText.ToString().TrimStartAndEnd();
	RebuildTree();
}

FReply SModApiInspectorPanel::HandleRescanClicked()
{
	Rescan();
	return FReply::Handled();
}

TSharedRef<SWidget> SModApiInspectorPanel::BuildSdkPluginMenu()
{
	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection*/ true, /*InCommandList*/ nullptr);

	const TArray<FString> Candidates = ModSDKWidgetUtils::GetCandidateSdkPluginNames();
	if (Candidates.Num() == 0)
	{
		MenuBuilder.AddWidget(
			SNew(STextBlock).Text(LOCTEXT("NoCandidatePlugins", "No project-side plugins were discovered.")),
			FText::GetEmpty());

		return MenuBuilder.MakeWidget();
	}

	for (const FString& Candidate : Candidates)
	{
		MenuBuilder.AddMenuEntry(
			FText::FromString(Candidate),
			FText::Format(LOCTEXT("ScopeToPluginFmt", "Treat {0} as the plugin that ships inside the bundle."),
				FText::FromString(Candidate)),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SModApiInspectorPanel::HandleSdkPluginScopePicked, Candidate)));
	}

	return MenuBuilder.MakeWidget();
}

void SModApiInspectorPanel::HandleSdkPluginScopePicked(FString InPluginName)
{
	if (SdkPluginName == InPluginName)
	{
		return;
	}

	SdkPluginName = MoveTemp(InPluginName);
	Rescan();
}

FText SModApiInspectorPanel::GetSdkPluginButtonText() const
{
	return SdkPluginName.IsEmpty()
		? LOCTEXT("NoSdkPluginScope", "SDK plugin: none")
		: FText::Format(LOCTEXT("SdkPluginScopeFmt", "SDK plugin: {0}"), FText::FromString(SdkPluginName));
}

ECheckBoxState SModApiInspectorPanel::GetIncludeBlueprintTypesState() const
{
	return bIncludeBlueprintGeneratedTypes ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SModApiInspectorPanel::HandleIncludeBlueprintTypesChanged(ECheckBoxState InNewState)
{
	const bool bNewValue = InNewState == ECheckBoxState::Checked;
	if (bNewValue == bIncludeBlueprintGeneratedTypes)
	{
		return;
	}

	bIncludeBlueprintGeneratedTypes = bNewValue;
	Rescan();
}

FText SModApiInspectorPanel::GetSummaryText() const
{
	if (!Report.bMetadataAvailable)
	{
		return LOCTEXT("SummaryNoMetadata", "No scan is possible in this build configuration.");
	}

	const FText ModulesText = Report.Modules.Num() > 0
		? ModSDKWidgetUtils::JoinStrings(Report.Modules)
		: LOCTEXT("NoContributingModules", "no module contributed a marked symbol");

	return FText::Format(
		LOCTEXT("SummaryFmt", "{0}  ·  {1} reflected type(s) examined in {2}s  ·  contributed by: {3}"),
		FText::FromString(Report.DescribeCounts()),
		FText::AsNumber(Report.TypesExamined),
		FText::AsNumber(Report.ScanSeconds),
		ModulesText);
}

FText SModApiInspectorPanel::GetWarningHeaderText() const
{
	const int32 ErrorCount = DiagnosticList.IsValid() ? DiagnosticList->CountBySeverity(EModDiagnosticSeverity::Error) : 0;
	const int32 WarningCount = DiagnosticList.IsValid() ? DiagnosticList->CountBySeverity(EModDiagnosticSeverity::Warning) : 0;

	if (ErrorCount == 0 && WarningCount == 0)
	{
		return LOCTEXT("WarningHeaderClean", "Marking problems — none found");
	}

	return FText::Format(LOCTEXT("WarningHeaderFmt", "Marking problems — {0}"),
		ModSDKWidgetUtils::MakeCountSummary(ErrorCount, WarningCount, /*InInfos*/ 0));
}

EVisibility SModApiInspectorPanel::GetStaleNoticeVisibility() const
{
	return bReportIsStale ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SModApiInspectorPanel::GetMetadataUnavailableVisibility() const
{
	return Report.bMetadataAvailable ? EVisibility::Collapsed : EVisibility::Visible;
}

void SModApiInspectorPanel::HandleReloadComplete(EReloadCompleteReason InReason)
{
	bReportIsStale = true;
}

#undef LOCTEXT_NAMESPACE
