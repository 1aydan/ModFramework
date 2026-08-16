// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/ModSDKWidgetUtils.h"

#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/GameInstance.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Internationalization.h"
#include "Layout/Margin.h"
#include "Misc/AssertionMacros.h"
#include "Misc/CString.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystem/ModSubsystem.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ModSDKWidgetUtils"

namespace ModSDKWidgetUtils
{
	const TCHAR* const FrameworkPluginName = TEXT("ModFramework");

	UModSubsystem* FindLiveModSubsystem()
	{
		// GEngine is null very early and during shutdown, and every world context is a plain struct
		// that may hold a null game instance. Nothing here dereferences without checking: an editor
		// with no PIE session is the *normal* state for these windows, not an error.
		if (!GEngine)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			const bool bIsPlayableWorld = Context.WorldType == EWorldType::PIE
				|| Context.WorldType == EWorldType::Game;

			if (!bIsPlayableWorld)
			{
				continue;
			}

			if (UGameInstance* GameInstance = Context.OwningGameInstance)
			{
				if (UModSubsystem* Subsystem = GameInstance->GetSubsystem<UModSubsystem>())
				{
					return Subsystem;
				}
			}
		}

		return nullptr;
	}

	TArray<FString> GetCandidateSdkPluginNames()
	{
		TArray<FString> Names;

		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
		{
			const EPluginType Type = Plugin->GetType();
			const bool bIsProjectSide = Type == EPluginType::Project
				|| Type == EPluginType::Mod
				|| Type == EPluginType::External;

			if (!bIsProjectSide)
			{
				continue;
			}

			const FString Name = Plugin->GetName();
			if (Name.Equals(FrameworkPluginName, ESearchCase::IgnoreCase))
			{
				continue;
			}

			Names.AddUnique(Name);
		}

		Names.Sort();
		return Names;
	}

	FString PickDefaultSdkPluginName()
	{
		const TArray<FString> Candidates = GetCandidateSdkPluginNames();

		for (const FString& Candidate : Candidates)
		{
			if (Candidate.Contains(TEXT("SDK"), ESearchCase::IgnoreCase))
			{
				return Candidate;
			}
		}

		return Candidates.Num() > 0 ? Candidates[0] : FString();
	}

	const FSlateBrush* GetSeverityBrush(EModDiagnosticSeverity InSeverity)
	{
		switch (InSeverity)
		{
		case EModDiagnosticSeverity::Error:
			return FAppStyle::GetBrush(TEXT("Icons.Error"));
		case EModDiagnosticSeverity::Warning:
			return FAppStyle::GetBrush(TEXT("Icons.Warning"));
		default:
			return FAppStyle::GetBrush(TEXT("Icons.Info"));
		}
	}

	FSlateColor GetSeverityColor(EModDiagnosticSeverity InSeverity)
	{
		switch (InSeverity)
		{
		case EModDiagnosticSeverity::Error:
			return FStyleColors::Error;
		case EModDiagnosticSeverity::Warning:
			return FStyleColors::Warning;
		default:
			return FStyleColors::AccentBlue;
		}
	}

	FText GetSeverityLabel(EModDiagnosticSeverity InSeverity)
	{
		switch (InSeverity)
		{
		case EModDiagnosticSeverity::Error:
			return LOCTEXT("SeverityError", "Error");
		case EModDiagnosticSeverity::Warning:
			return LOCTEXT("SeverityWarning", "Warning");
		default:
			return LOCTEXT("SeverityInfo", "Info");
		}
	}

	FText MakeCountSummary(int32 InErrors, int32 InWarnings, int32 InInfos)
	{
		if (InErrors <= 0 && InWarnings <= 0 && InInfos <= 0)
		{
			return LOCTEXT("NoProblems", "No problems reported.");
		}

		TArray<FText> Parts;
		if (InErrors > 0)
		{
			Parts.Add(FText::Format(LOCTEXT("ErrorCountFmt", "{0} error(s)"), FText::AsNumber(InErrors)));
		}
		if (InWarnings > 0)
		{
			Parts.Add(FText::Format(LOCTEXT("WarningCountFmt", "{0} warning(s)"), FText::AsNumber(InWarnings)));
		}
		if (InInfos > 0)
		{
			Parts.Add(FText::Format(LOCTEXT("InfoCountFmt", "{0} note(s)"), FText::AsNumber(InInfos)));
		}

		return FText::Join(LOCTEXT("CountSeparator", ", "), Parts);
	}

	TSharedRef<SWidget> MakeSectionHeader(const FText& InTitle, const FText& InSubtitle)
	{
		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

		Box->AddSlot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Font(FAppStyle::Get().GetFontStyle(TEXT("NormalFontBold")))
				.Text(InTitle)
			];

		if (!InSubtitle.IsEmpty())
		{
			Box->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FStyleColors::Foreground)
					.Text(InSubtitle)
				];
		}

		return Box;
	}

	TSharedRef<SWidget> MakeNotice(const FText& InTitle, const FText& InBody, EModDiagnosticSeverity InSeverity)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			.Padding(10.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(0.0f, 2.0f, 8.0f, 0.0f)
				[
					SNew(SImage)
					.Image(GetSeverityBrush(InSeverity))
					.ColorAndOpacity(GetSeverityColor(InSeverity))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Font(FAppStyle::Get().GetFontStyle(TEXT("NormalFontBold")))
						.AutoWrapText(true)
						.Text(InTitle)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FStyleColors::Foreground)
						.Text(InBody)
					]
				]
			];
	}

	FText JoinNames(const TArray<FName>& InNames)
	{
		if (InNames.Num() == 0)
		{
			return LOCTEXT("EmptyList", "—");
		}

		TArray<FText> Parts;
		Parts.Reserve(InNames.Num());
		for (const FName& Name : InNames)
		{
			Parts.Add(FText::FromName(Name));
		}

		return FText::Join(LOCTEXT("ListSeparator", ", "), Parts);
	}

	FText JoinStrings(const TArray<FString>& InStrings)
	{
		if (InStrings.Num() == 0)
		{
			return LOCTEXT("EmptyList", "—");
		}

		TArray<FText> Parts;
		Parts.Reserve(InStrings.Num());
		for (const FString& Value : InStrings)
		{
			Parts.Add(FText::FromString(Value));
		}

		return FText::Join(LOCTEXT("ListSeparator", ", "), Parts);
	}

	FText YesNo(bool bValue)
	{
		return bValue ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No");
	}

	FText OrDash(const FString& InValue)
	{
		return InValue.IsEmpty() ? LOCTEXT("EmptyList", "—") : FText::FromString(InValue);
	}

	FText OrDash(FName InValue)
	{
		return InValue.IsNone() ? LOCTEXT("EmptyList", "—") : FText::FromName(InValue);
	}
}

#undef LOCTEXT_NAMESPACE
