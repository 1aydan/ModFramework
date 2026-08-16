// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/SModValidationPanel.h"

#include "HAL/Platform.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Input/Reply.h"
#include "Layout/Margin.h"
#include "Manifest/ModManifestParser.h"
#include "Misc/Paths.h"
#include "ModDeveloperModel.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/ModDeveloperUI.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SModValidationPanel"

namespace ModValidationPanelPrivate
{
	static const FName ColSeverity(TEXT("Severity"));
	static const FName ColMod(TEXT("Mod"));
	static const FName ColCode(TEXT("Code"));
	static const FName ColMessage(TEXT("Message"));
	static const FName ColContext(TEXT("Context"));

	/** Label used for a diagnostic that belongs to the pipeline rather than to any one mod. */
	FString PipelineLabel()
	{
		return TEXT("(framework)");
	}

	bool MatchesText(const FModValidationEntry& Entry, const FString& Filter)
	{
		if (Filter.IsEmpty())
		{
			return true;
		}

		return Entry.Diagnostic.Message.Contains(Filter)
			|| Entry.Diagnostic.Code.ToString().Contains(Filter)
			|| Entry.Diagnostic.Context.Contains(Filter)
			|| Entry.ModDisplayName.Contains(Filter)
			|| Entry.ModId.ToString().Contains(Filter);
	}

	/**
	 * Errors first, then warnings, then notes; within a severity, by mod then by code.
	 *
	 * A mod author reading this list wants the thing that will stop their mod loading at the top, not
	 * the alphabetically first thing.
	 */
	bool EntrySortPredicate(const TSharedPtr<FModValidationEntry>& A, const TSharedPtr<FModValidationEntry>& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return B.IsValid();
		}

		// EModDiagnosticSeverity is Info(0) < Warning(1) < Error(2), so the comparison is inverted to
		// put the most severe first.
		if (A->Diagnostic.Severity != B->Diagnostic.Severity)
		{
			return static_cast<uint8>(A->Diagnostic.Severity) > static_cast<uint8>(B->Diagnostic.Severity);
		}

		const int32 NameCompare = A->ModDisplayName.Compare(B->ModDisplayName, ESearchCase::IgnoreCase);
		if (NameCompare != 0)
		{
			return NameCompare < 0;
		}

		return A->Diagnostic.Code.LexicalLess(B->Diagnostic.Code);
	}
}

/** One diagnostic row. Read-only: every action lives on the panel's toolbar or on the row's click. */
class SModValidationRow : public SMultiColumnTableRow<TSharedPtr<FModValidationEntry>>
{
public:
	SLATE_BEGIN_ARGS(SModValidationRow) {}
		SLATE_ARGUMENT(TSharedPtr<FModValidationEntry>, Item)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
	{
		Item = InArgs._Item;
		SMultiColumnTableRow<TSharedPtr<FModValidationEntry>>::Construct(FSuperRowType::FArguments(), InOwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		using namespace ModValidationPanelPrivate;

		if (!Item.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		if (ColumnName == ColSeverity)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SBox)
					.WidthOverride(16.0f)
					.HeightOverride(16.0f)
					[
						SNew(SImage)
						.Image(ModDeveloperUI::SeverityIcon(Item->Diagnostic.Severity))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(ModDeveloperUI::SeverityText(Item->Diagnostic.Severity))
					.ColorAndOpacity(ModDeveloperUI::SeverityColor(Item->Diagnostic.Severity))
				];
		}

		if (ColumnName == ColMod)
		{
			return SNew(STextBlock)
				.Text(FText::FromString(Item->ModDisplayName))
				.ToolTipText(Item->ModId.IsValid()
					? FText::FromString(Item->ModId.ToString())
					: LOCTEXT("PipelineTooltip", "This message is about the mod set as a whole, not about one mod."));
		}

		if (ColumnName == ColCode)
		{
			return SNew(STextBlock)
				.Text(FText::FromName(Item->Diagnostic.Code))
				.Font(ModDeveloperUI::MonospaceFont())
				.ToolTipText(LOCTEXT("CodeTooltip", "Stable diagnostic code. These never get renamed, so they are safe to search the docs for."));
		}

		if (ColumnName == ColMessage)
		{
			return SNew(STextBlock)
				.Text(FText::FromString(Item->Diagnostic.Message))
				.ToolTipText(FText::FromString(Item->Diagnostic.Message))
				.AutoWrapText(true);
		}

		if (ColumnName == ColContext)
		{
			return SNew(STextBlock)
				.Text(FText::FromString(Item->Diagnostic.Context))
				.ToolTipText(FText::FromString(Item->Diagnostic.Context))
				.ColorAndOpacity(FStyleColors::Foreground);
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FModValidationEntry> Item;
};

void SModValidationPanel::Construct(const FArguments& InArgs)
{
	using namespace ModValidationPanelPrivate;

	Model = InArgs._Model;
	OnModFocusRequested = InArgs._OnModFocusRequested;

	if (Model.IsValid())
	{
		Model->OnChanged.AddSP(this, &SModValidationPanel::HandleModelChanged);
	}

	auto MakeSeverityToggle = [this](EModDiagnosticSeverity Severity, const FText& Label) -> TSharedRef<SWidget>
	{
		return SNew(SCheckBox)
			.Style(FAppStyle::Get(), TEXT("ToggleButtonCheckbox"))
			.Padding(FMargin(8.0f, 2.0f))
			.IsChecked(this, &SModValidationPanel::GetSeverityFilterState, Severity)
			.OnCheckStateChanged(this, &SModValidationPanel::HandleSeverityFilterChanged, Severity)
			[
				SNew(STextBlock)
				.Text(Label)
				.ColorAndOpacity(ModDeveloperUI::SeverityColor(Severity))
			];
	};

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Toolbar ---------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.ToolTipText(LOCTEXT("RevalidateTip",
					"Re-read every mod.json from disk and run the manifest validator over it again. "
					"Use this after editing a manifest by hand."))
				.OnClicked(this, &SModValidationPanel::HandleRevalidateClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("Revalidate", "Re-run validation"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 2.0f, 0.0f)
			[
				MakeSeverityToggle(EModDiagnosticSeverity::Error, LOCTEXT("FilterErrors", "Errors"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f)
			[
				MakeSeverityToggle(EModDiagnosticSeverity::Warning, LOCTEXT("FilterWarnings", "Warnings"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f, 8.0f, 0.0f)
			[
				MakeSeverityToggle(EModDiagnosticSeverity::Info, LOCTEXT("FilterInfo", "Notes"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SAssignNew(SearchBox, SSearchBox)
				.HintText(LOCTEXT("ValidationSearchHint", "Filter by message, code, context or mod"))
				.OnTextChanged(this, &SModValidationPanel::HandleSearchTextChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.ToolTipText(LOCTEXT("CopyTip", "Copy every visible diagnostic to the clipboard, one per line."))
				.OnClicked(this, &SModValidationPanel::HandleCopyClicked)
				[
					SNew(STextBlock).Text(LOCTEXT("Copy", "Copy"))
				]
			]
		]

		// --- Summary ---------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(STextBlock)
			.Text(this, &SModValidationPanel::GetSummaryText)
		]

		// --- List ------------------------------------------------------------------------------
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(ListView, SListView<TSharedPtr<FModValidationEntry>>)
			.ListItemsSource(&VisibleEntries)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SModValidationPanel::HandleGenerateRow)
			.OnSelectionChanged(this, &SModValidationPanel::HandleSelectionChanged)
			.HeaderRow
			(
				SNew(SHeaderRow)
				+ SHeaderRow::Column(ColSeverity)
					.DefaultLabel(LOCTEXT("HeaderSeverity", "Severity"))
					.FixedWidth(110.0f)
				+ SHeaderRow::Column(ColMod)
					.DefaultLabel(LOCTEXT("HeaderMod", "Mod"))
					.FillWidth(0.18f)
				+ SHeaderRow::Column(ColCode)
					.DefaultLabel(LOCTEXT("HeaderCode", "Code"))
					.FillWidth(0.20f)
				+ SHeaderRow::Column(ColMessage)
					.DefaultLabel(LOCTEXT("HeaderMessage", "Message"))
					.FillWidth(0.40f)
				+ SHeaderRow::Column(ColContext)
					.DefaultLabel(LOCTEXT("HeaderContext", "Context"))
					.FillWidth(0.22f)
			)
		]
	];

	RunValidation();
}

SModValidationPanel::~SModValidationPanel()
{
	if (Model.IsValid())
	{
		Model->OnChanged.RemoveAll(this);
	}
}

void SModValidationPanel::HandleModelChanged()
{
	RunValidation();
}

void SModValidationPanel::RunValidation()
{
	AllEntries.Reset();

	if (!Model.IsValid())
	{
		RebuildVisibleEntries();
		return;
	}

	auto AddDiagnostic = [this](const FModDiagnostic& Diagnostic, const FModId& ModId, const FString& DisplayName)
	{
		TSharedRef<FModValidationEntry> Entry = MakeShared<FModValidationEntry>();
		Entry->Diagnostic = Diagnostic;
		Entry->ModId = ModId.IsValid() ? ModId : Diagnostic.ModId;
		Entry->ModDisplayName = DisplayName;
		AllEntries.Add(Entry);
	};

	for (const TSharedPtr<FModDeveloperModRow>& Row : Model->GetMods())
	{
		if (!Row.IsValid())
		{
			continue;
		}

		TArray<FModDiagnostic> Fresh;

		if (Row->bPackaged)
		{
			// The manifest of a `.mod` lives inside the container and was already read out of it by
			// the provider. Re-validating the parsed form is the honest equivalent of re-reading a
			// loose mod.json: it re-applies every semantic rule without extracting the package.
			FModManifestParser::ValidateManifest(Row->Manifest, Row->RootPath, Fresh);
		}
		else
		{
			const FString ManifestPath = FPaths::Combine(Row->RootPath, FModManifestParser::GetManifestFileName());
			const FModManifestParseResult Parsed = FModManifestParser::ParseFromFile(ManifestPath);
			Fresh = Parsed.Diagnostics;

			if (Parsed.bSuccess && Fresh.Num() == 0)
			{
				Fresh.Add(FModDiagnostic::Info(FName(TEXT("Manifest.Valid")),
					TEXT("The manifest parsed and validated with nothing to report."),
					ManifestPath));
			}
		}

		for (const FModDiagnostic& Diagnostic : Fresh)
		{
			AddDiagnostic(Diagnostic, Row->Id, Row->DisplayName);
		}

		// A live mod also carries whatever mounting, loading and activation had to say about it, and
		// none of that can be re-derived from the manifest.
		if (Row->bLive)
		{
			for (const FModDiagnostic& Diagnostic : Row->Diagnostics)
			{
				AddDiagnostic(Diagnostic, Row->Id, Row->DisplayName);
			}
		}
	}

	for (const FModDiagnostic& Diagnostic : Model->GetPipelineDiagnostics())
	{
		const TSharedPtr<FModDeveloperModRow> Owner = Model->FindMod(Diagnostic.ModId);
		AddDiagnostic(Diagnostic, Diagnostic.ModId,
			Owner.IsValid() ? Owner->DisplayName : ModValidationPanelPrivate::PipelineLabel());
	}

	RebuildVisibleEntries();
}

void SModValidationPanel::RebuildVisibleEntries()
{
	VisibleEntries.Reset();

	for (const TSharedPtr<FModValidationEntry>& Entry : AllEntries)
	{
		if (!Entry.IsValid())
		{
			continue;
		}

		const bool bSeverityVisible =
			(Entry->Diagnostic.Severity == EModDiagnosticSeverity::Error && bShowErrors)
			|| (Entry->Diagnostic.Severity == EModDiagnosticSeverity::Warning && bShowWarnings)
			|| (Entry->Diagnostic.Severity == EModDiagnosticSeverity::Info && bShowInfo);

		if (!bSeverityVisible)
		{
			continue;
		}

		if (!ModValidationPanelPrivate::MatchesText(*Entry, FilterText))
		{
			continue;
		}

		VisibleEntries.Add(Entry);
	}

	VisibleEntries.Sort([](const TSharedPtr<FModValidationEntry>& A, const TSharedPtr<FModValidationEntry>& B)
	{
		return ModValidationPanelPrivate::EntrySortPredicate(A, B);
	});

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SModValidationPanel::HandleGenerateRow(TSharedPtr<FModValidationEntry> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SModValidationRow, OwnerTable)
		.Item(Item);
}

void SModValidationPanel::HandleSelectionChanged(TSharedPtr<FModValidationEntry> Item, ESelectInfo::Type SelectInfo)
{
	// Direct selection is what the panel itself does when it rebuilds; only a real click should pull
	// the user over to another tab.
	if (SelectInfo == ESelectInfo::Direct || !Item.IsValid() || !Item->ModId.IsValid())
	{
		return;
	}

	OnModFocusRequested.ExecuteIfBound(Item->ModId);

	// Dropping the selection matters: this list has no selected state worth keeping, and leaving the
	// row selected would make clicking it a second time - after coming back from the Mods tab - do
	// nothing at all, because the selection would not have changed.
	if (ListView.IsValid())
	{
		ListView->ClearSelection();
	}
}

void SModValidationPanel::HandleSearchTextChanged(const FText& NewText)
{
	FilterText = NewText.ToString().TrimStartAndEnd();
	RebuildVisibleEntries();
}

FReply SModValidationPanel::HandleRevalidateClicked()
{
	if (Model.IsValid())
	{
		// Refresh() rebuilds the mod set first, so a mod added or deleted since the window opened is
		// picked up as well as an edited manifest. It broadcasts, which re-enters RunValidation.
		Model->Refresh();
	}
	else
	{
		RunValidation();
	}

	return FReply::Handled();
}

FReply SModValidationPanel::HandleCopyClicked()
{
	TArray<FString> Lines;
	Lines.Reserve(VisibleEntries.Num());

	for (const TSharedPtr<FModValidationEntry>& Entry : VisibleEntries)
	{
		if (Entry.IsValid())
		{
			Lines.Add(FString::Printf(TEXT("%s\t%s"), *Entry->ModDisplayName, *Entry->Diagnostic.ToString()));
		}
	}

	if (Lines.Num() > 0)
	{
		FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Lines, TEXT("\n")));
	}

	return FReply::Handled();
}

ECheckBoxState SModValidationPanel::GetSeverityFilterState(EModDiagnosticSeverity Severity) const
{
	bool bChecked = false;
	switch (Severity)
	{
	case EModDiagnosticSeverity::Error:
		bChecked = bShowErrors;
		break;
	case EModDiagnosticSeverity::Warning:
		bChecked = bShowWarnings;
		break;
	default:
		bChecked = bShowInfo;
		break;
	}

	return bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SModValidationPanel::HandleSeverityFilterChanged(ECheckBoxState NewState, EModDiagnosticSeverity Severity)
{
	const bool bChecked = NewState == ECheckBoxState::Checked;

	switch (Severity)
	{
	case EModDiagnosticSeverity::Error:
		bShowErrors = bChecked;
		break;
	case EModDiagnosticSeverity::Warning:
		bShowWarnings = bChecked;
		break;
	default:
		bShowInfo = bChecked;
		break;
	}

	RebuildVisibleEntries();
}

FText SModValidationPanel::GetSummaryText() const
{
	TArray<FModDiagnostic> Flattened;
	Flattened.Reserve(AllEntries.Num());
	for (const TSharedPtr<FModValidationEntry>& Entry : AllEntries)
	{
		if (Entry.IsValid())
		{
			Flattened.Add(Entry->Diagnostic);
		}
	}

	return FText::Format(
		LOCTEXT("ValidationSummary", "{0}  Showing {1} of {2}."),
		ModDeveloperUI::SummariseDiagnostics(Flattened),
		FText::AsNumber(VisibleEntries.Num()),
		FText::AsNumber(AllEntries.Num()));
}

#undef LOCTEXT_NAMESPACE
