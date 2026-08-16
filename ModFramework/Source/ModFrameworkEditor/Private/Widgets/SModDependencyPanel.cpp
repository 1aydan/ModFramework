// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModDependencyPanel.h"

#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Dependencies/ModDependencyTypes.h"
#include "HAL/Platform.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Input/Reply.h"
#include "Layout/Margin.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "ModDeveloperModel.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/ModDeveloperUI.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SModDependencyPanel"

namespace ModDependencyPanelPrivate
{
	/** Deep enough for any sane mod graph, shallow enough that a hostile one cannot exhaust the stack. */
	constexpr int32 MaxTreeDepth = 12;

	FString Indent(int32 Depth)
	{
		FString Result;
		for (int32 Index = 0; Index < Depth; ++Index)
		{
			Result += TEXT("    ");
		}
		return Result;
	}

	FString DescribeRange(const FModVersionRange& Range)
	{
		if (Range.IsAny())
		{
			return TEXT("any version");
		}

		const FString Expression = Range.ToString();
		return Expression.IsEmpty() ? TEXT("any version") : Expression;
	}

	/** "corelib 1.4.2 (order 1, Activated)" for an installed mod. */
	FString DescribeInstalled(const FModDeveloperModRow& Row)
	{
		const FString OrderText = (Row.LoadOrder == INDEX_NONE)
			? FString(TEXT("not ordered"))
			: FString::Printf(TEXT("order %d"), Row.LoadOrder);

		return FString::Printf(TEXT("%s %s (%s, %s)"),
			*Row.Id.ToString(),
			*Row.VersionText,
			*OrderText,
			*ModFrameworkEnums::ToString(Row.State));
	}

	/**
	 * Recursively writes one mod's required and optional dependencies.
	 *
	 * Path carries the ids on the current branch, which is both the cycle guard and what makes the
	 * "cycle" marker able to name the mod it looped back to.
	 */
	void AppendDependencyTree(const FModDeveloperModel& Model, const FModDeveloperModRow& Row,
		int32 Depth, TSet<FModId>& Path, TArray<FString>& OutLines)
	{
		if (Row.Manifest.Dependencies.Num() == 0)
		{
			OutLines.Add(Indent(Depth) + TEXT("(nothing)"));
			return;
		}

		for (const FModDependency& Dependency : Row.Manifest.Dependencies)
		{
			const FString RangeText = DescribeRange(Dependency.VersionRange);
			const TCHAR* Kind = Dependency.bOptional ? TEXT("optional") : TEXT("required");

			const TSharedPtr<FModDeveloperModRow> Installed = Model.FindMod(Dependency.Id);
			if (!Installed.IsValid())
			{
				OutLines.Add(FString::Printf(TEXT("%s[%s] %s %s -> NOT INSTALLED%s"),
					*Indent(Depth),
					Dependency.bOptional ? TEXT(" ok ") : TEXT("MISS"),
					*Dependency.Id.ToString(),
					*RangeText,
					Dependency.bOptional ? TEXT(" (optional, so this is not a problem)") : TEXT("")));

				if (!Dependency.Reason.IsEmpty())
				{
					OutLines.Add(Indent(Depth + 1) + TEXT("author's note: ") + Dependency.Reason);
				}
				continue;
			}

			const bool bSatisfied = Dependency.VersionRange.Satisfies(Installed->Manifest.Version);
			OutLines.Add(FString::Printf(TEXT("%s[%s] %s %s -> %s"),
				*Indent(Depth),
				bSatisfied ? TEXT(" ok ") : TEXT("BAD "),
				*Dependency.Id.ToString(),
				*RangeText,
				*DescribeInstalled(*Installed)));

			if (!bSatisfied)
			{
				OutLines.Add(FString::Printf(TEXT("%sinstalled %s does not satisfy %s (%s dependency)"),
					*Indent(Depth + 1),
					*Installed->VersionText,
					*RangeText,
					Kind));

				if (!Dependency.Reason.IsEmpty())
				{
					OutLines.Add(Indent(Depth + 1) + TEXT("author's note: ") + Dependency.Reason);
				}
			}

			if (Path.Contains(Dependency.Id))
			{
				OutLines.Add(Indent(Depth + 1) + TEXT("^ cycle: this mod is already on the current branch."));
				continue;
			}

			if (Depth + 1 >= MaxTreeDepth)
			{
				OutLines.Add(Indent(Depth + 1) + TEXT("... deeper dependencies not shown (depth limit)."));
				continue;
			}

			if (Installed->Manifest.Dependencies.Num() > 0)
			{
				Path.Add(Dependency.Id);
				AppendDependencyTree(Model, *Installed, Depth + 1, Path, OutLines);
				Path.Remove(Dependency.Id);
			}
		}
	}

	FString BuildReport(const FModDeveloperModel& Model, const FModDeveloperModRow& Row)
	{
		TArray<FString> Lines;

		Lines.Add(FString::Printf(TEXT("%s  %s"), *Row.DisplayName, *Row.VersionText));
		Lines.Add(FString::Printf(TEXT("id: %s"), *Row.Id.ToString()));
		Lines.Add(FString::Printf(TEXT("state: %s%s"),
			*ModFrameworkEnums::ToString(Row.State),
			Row.bEnabled ? TEXT("") : TEXT("  (disabled)")));
		Lines.Add(FString::Printf(TEXT("load order: %s"),
			Row.LoadOrder == INDEX_NONE ? TEXT("not in the load order") : *FString::FromInt(Row.LoadOrder)));
		Lines.Add(TEXT(""));

		Lines.Add(TEXT("NEEDS"));
		Lines.Add(TEXT("  Each line is one dependency: [ ok ] satisfied, [MISS] absent, [BAD ] wrong version."));
		{
			TSet<FModId> Path;
			Path.Add(Row.Id);
			AppendDependencyTree(Model, Row, 1, Path, Lines);
		}
		Lines.Add(TEXT(""));

		Lines.Add(TEXT("NEEDED BY"));
		{
			int32 DependentCount = 0;
			for (const TSharedPtr<FModDeveloperModRow>& Other : Model.GetMods())
			{
				if (!Other.IsValid() || Other->Id == Row.Id)
				{
					continue;
				}

				const FModDependency* Dependency = Other->Manifest.FindDependency(Row.Id);
				if (Dependency == nullptr)
				{
					continue;
				}

				++DependentCount;
				const bool bSatisfied = Dependency->VersionRange.Satisfies(Row.Manifest.Version);
				Lines.Add(FString::Printf(TEXT("%s[%s] %s wants %s %s"),
					*Indent(1),
					bSatisfied ? TEXT(" ok ") : TEXT("BAD "),
					*Other->Id.ToString(),
					*Row.Id.ToString(),
					*DescribeRange(Dependency->VersionRange)));

				if (Dependency->bOptional)
				{
					Lines.Add(Indent(2) + TEXT("optional: that mod loads with or without this one."));
				}
			}

			if (DependentCount == 0)
			{
				Lines.Add(Indent(1) + TEXT("(nothing)"));
			}
		}
		Lines.Add(TEXT(""));

		Lines.Add(TEXT("ORDERING"));
		{
			bool bAnyOrdering = false;

			for (const FModId& Target : Row.Manifest.LoadAfter)
			{
				bAnyOrdering = true;
				const bool bPresent = Model.FindMod(Target).IsValid();
				Lines.Add(FString::Printf(TEXT("%sloads after  %s%s"),
					*Indent(1), *Target.ToString(),
					bPresent ? TEXT("") : TEXT("   (not installed - the edge is ignored)")));
			}

			for (const FModId& Target : Row.Manifest.LoadBefore)
			{
				bAnyOrdering = true;
				const bool bPresent = Model.FindMod(Target).IsValid();
				Lines.Add(FString::Printf(TEXT("%sloads before %s%s"),
					*Indent(1), *Target.ToString(),
					bPresent ? TEXT("") : TEXT("   (not installed - the edge is ignored)")));
			}

			if (Row.Manifest.Priority != 0)
			{
				bAnyOrdering = true;
				Lines.Add(FString::Printf(TEXT("%spriority %d (higher loads earlier among mods the graph leaves unordered)"),
					*Indent(1), Row.Manifest.Priority));
			}

			if (!bAnyOrdering)
			{
				Lines.Add(Indent(1) + TEXT("(no ordering hints)"));
			}
		}
		Lines.Add(TEXT(""));

		Lines.Add(TEXT("RESOLUTION"));
		{
			const FModResolveResult& Resolve = Model.GetResolveResult();
			const int32 OrderIndex = Resolve.LoadOrder.IndexOfByKey(Row.Id);

			if (OrderIndex != INDEX_NONE)
			{
				Lines.Add(FString::Printf(TEXT("%sAccepted: position %d of %d in the resolved load order."),
					*Indent(1), OrderIndex, Resolve.LoadOrder.Num()));
			}
			else if (const FModRejection* Rejection = Resolve.FindRejection(Row.Id))
			{
				Lines.Add(FString::Printf(TEXT("%sRejected (%s)."),
					*Indent(1), *ModFrameworkEnums::ToString(Rejection->Reason)));
				Lines.Add(Indent(1) + Rejection->Message);

				if (Rejection->RelatedMods.Num() > 0)
				{
					TArray<FString> Related;
					Related.Reserve(Rejection->RelatedMods.Num());
					for (const FModId& RelatedId : Rejection->RelatedMods)
					{
						Related.Add(RelatedId.ToString());
					}

					Lines.Add(FString::Printf(TEXT("%smods involved: %s"),
						*Indent(1), *FString::Join(Related, TEXT(" -> "))));
				}
			}
			else
			{
				Lines.Add(Indent(1) + TEXT("No resolve pass has run over this mod yet."));
			}
		}

		return FString::Join(Lines, TEXT("\n"));
	}
}

void SModDependencyPanel::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;

	if (Model.IsValid())
	{
		Model->OnChanged.AddSP(this, &SModDependencyPanel::HandleModelChanged);
	}

	RebuildOptions();

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ModLabel", "Mod"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.MinDesiredWidth(320.0f)
				[
					SAssignNew(ModCombo, SComboBox<TSharedPtr<FModDeveloperModRow>>)
					.OptionsSource(&Options)
					.OnGenerateWidget(this, &SModDependencyPanel::HandleGenerateComboEntry)
					.OnSelectionChanged(this, &SModDependencyPanel::HandleComboSelectionChanged)
					[
						SNew(STextBlock)
						.Text(this, &SModDependencyPanel::GetComboLabel)
					]
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNullWidget::NullWidget
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.ToolTipText(LOCTEXT("CopyDependencyTip", "Copy this dependency report to the clipboard."))
				.OnClicked(this, &SModDependencyPanel::HandleCopyClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("CopyDependency", "Copy"))
				]
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			.Padding(4.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AlwaysShowScrollbars(false)
				.Font(ModDeveloperUI::MonospaceFont())
				.Text(this, &SModDependencyPanel::GetReportText)
			]
		]
	];

	RebuildReport();
}

SModDependencyPanel::~SModDependencyPanel()
{
	if (Model.IsValid())
	{
		Model->OnChanged.RemoveAll(this);
	}
}

void SModDependencyPanel::HandleModelChanged()
{
	RebuildOptions();
	RebuildReport();
}

void SModDependencyPanel::RebuildOptions()
{
	Options.Reset();

	if (Model.IsValid())
	{
		Options = Model->GetMods();
	}

	// Keep the selection when the mod still exists; otherwise fall back to the first one so the panel
	// is never blank just because a refresh happened.
	if (!SelectedId.IsValid() || !Model.IsValid() || !Model->FindMod(SelectedId).IsValid())
	{
		SelectedId = Options.Num() > 0 && Options[0].IsValid() ? Options[0]->Id : FModId();
	}

	if (ModCombo.IsValid())
	{
		ModCombo->RefreshOptions();

		for (const TSharedPtr<FModDeveloperModRow>& Option : Options)
		{
			if (Option.IsValid() && Option->Id == SelectedId)
			{
				ModCombo->SetSelectedItem(Option);
				break;
			}
		}
	}
}

void SModDependencyPanel::RebuildReport()
{
	if (!Model.IsValid())
	{
		ReportText = LOCTEXT("NoModel", "The mod developer window has no data source.");
		return;
	}

	const TSharedPtr<FModDeveloperModRow> Row = Model->FindMod(SelectedId);
	if (!Row.IsValid())
	{
		ReportText = LOCTEXT("NoModsAtAll",
			"No mods were found. Check the search directories under Project Settings > Plugins > Mod Framework, "
			"then press Refresh on the Mods tab.");
		return;
	}

	ReportText = FText::FromString(ModDependencyPanelPrivate::BuildReport(*Model, *Row));
}

TSharedRef<SWidget> SModDependencyPanel::HandleGenerateComboEntry(TSharedPtr<FModDeveloperModRow> Item)
{
	const FText Label = Item.IsValid()
		? FText::FromString(FString::Printf(TEXT("%s  (%s)"), *Item->DisplayName, *Item->Id.ToString()))
		: LOCTEXT("InvalidEntry", "(invalid)");

	return SNew(STextBlock).Text(Label);
}

void SModDependencyPanel::HandleComboSelectionChanged(TSharedPtr<FModDeveloperModRow> Item, ESelectInfo::Type /*SelectInfo*/)
{
	if (Item.IsValid())
	{
		SelectedId = Item->Id;
		RebuildReport();
	}
}

FText SModDependencyPanel::GetComboLabel() const
{
	if (Model.IsValid())
	{
		if (const TSharedPtr<FModDeveloperModRow> Row = Model->FindMod(SelectedId))
		{
			return FText::FromString(FString::Printf(TEXT("%s  (%s)"), *Row->DisplayName, *Row->Id.ToString()));
		}
	}

	return LOCTEXT("SelectMod", "Select a mod");
}

FText SModDependencyPanel::GetReportText() const
{
	return ReportText;
}

FReply SModDependencyPanel::HandleCopyClicked()
{
	const FString Report = ReportText.ToString();
	if (!Report.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*Report);
	}

	return FReply::Handled();
}

void SModDependencyPanel::SetSelectedMod(const FModId& InId)
{
	if (!Model.IsValid() || !Model->FindMod(InId).IsValid())
	{
		return;
	}

	SelectedId = InId;

	if (ModCombo.IsValid())
	{
		for (const TSharedPtr<FModDeveloperModRow>& Option : Options)
		{
			if (Option.IsValid() && Option->Id == InId)
			{
				ModCombo->SetSelectedItem(Option);
				break;
			}
		}
	}

	RebuildReport();
}

#undef LOCTEXT_NAMESPACE
