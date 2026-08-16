// Copyright (c) 2026. Licensed for use in your own projects.

#include "Widgets/ModDeveloperUI.h"

#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/StyleColors.h"
#include "UObject/NameTypes.h"

#define LOCTEXT_NAMESPACE "ModDeveloperUI"

namespace ModDeveloperUI
{
	FText StateText(EModState In)
	{
		return FText::FromString(ModFrameworkEnums::ToString(In));
	}

	FSlateColor StateColor(EModState In)
	{
		switch (In)
		{
		case EModState::Activated:
			return FStyleColors::AccentGreen;

		case EModState::Loaded:
		case EModState::Mounted:
		case EModState::DependenciesResolved:
		case EModState::Validated:
			return FStyleColors::AccentBlue;

		case EModState::Loading:
			return FStyleColors::AccentYellow;

		case EModState::Failed:
			return FStyleColors::Error;

		case EModState::Deactivated:
		case EModState::Unmounted:
		case EModState::Disabled:
		case EModState::Discovered:
		case EModState::Unknown:
		default:
			return FStyleColors::Foreground;
		}
	}

	FText StateTooltip(EModState In)
	{
		switch (In)
		{
		case EModState::Unknown:
			return LOCTEXT("StateUnknown", "The mod has a record but has not entered the lifecycle yet.");
		case EModState::Discovered:
			return LOCTEXT("StateDiscovered", "A provider found the mod. Its manifest has not been validated yet.");
		case EModState::Validated:
			return LOCTEXT("StateValidated", "The manifest parsed and validated. The mod is waiting on dependency resolution.");
		case EModState::DependenciesResolved:
			return LOCTEXT("StateResolved", "The mod has a place in the load order and is ready to mount.");
		case EModState::Mounted:
			return LOCTEXT("StateMounted", "The mod's content roots are mounted. Its code has not been loaded.");
		case EModState::Loading:
			return LOCTEXT("StateLoading", "The mod is being given its context and entry point right now.");
		case EModState::Loaded:
			return LOCTEXT("StateLoaded", "The mod has a context and an entry point but is idle. Activate it to switch it on.");
		case EModState::Activated:
			return LOCTEXT("StateActivated", "The mod is running: its bundles are applied and its extensions are live.");
		case EModState::Deactivated:
			return LOCTEXT("StateDeactivated", "The mod is still loaded but switched off. Activating it again is cheap.");
		case EModState::Unmounted:
			return LOCTEXT("StateUnmounted", "The mod's content has been released. It can be mounted again.");
		case EModState::Failed:
			return LOCTEXT("StateFailed", "The mod could not be brought up. The failure reason says why.");
		case EModState::Disabled:
			return LOCTEXT("StateDisabled", "The mod is switched off for this session and will not load.");
		default:
			return FText::GetEmpty();
		}
	}

	FText SeverityText(EModDiagnosticSeverity In)
	{
		switch (In)
		{
		case EModDiagnosticSeverity::Error:
			return LOCTEXT("SeverityError", "Error");
		case EModDiagnosticSeverity::Warning:
			return LOCTEXT("SeverityWarning", "Warning");
		case EModDiagnosticSeverity::Info:
		default:
			return LOCTEXT("SeverityInfo", "Info");
		}
	}

	FSlateColor SeverityColor(EModDiagnosticSeverity In)
	{
		switch (In)
		{
		case EModDiagnosticSeverity::Error:
			return FStyleColors::Error;
		case EModDiagnosticSeverity::Warning:
			return FStyleColors::Warning;
		case EModDiagnosticSeverity::Info:
		default:
			return FStyleColors::Foreground;
		}
	}

	const FSlateBrush* SeverityIcon(EModDiagnosticSeverity In)
	{
		switch (In)
		{
		case EModDiagnosticSeverity::Error:
			return FAppStyle::GetBrush(TEXT("Icons.Error"));
		case EModDiagnosticSeverity::Warning:
			return FAppStyle::GetBrush(TEXT("Icons.Warning"));
		case EModDiagnosticSeverity::Info:
		default:
			return FAppStyle::GetBrush(TEXT("Icons.Info"));
		}
	}

	FText FailureReasonText(EModLoadFailureReason In)
	{
		return FText::FromString(ModFrameworkEnums::ToString(In));
	}

	FText ConflictPolicyText(EModConflictPolicy In)
	{
		return FText::FromString(ModFrameworkEnums::ToString(In));
	}

	FSlateFontInfo MonospaceFont(float InSize)
	{
		return FCoreStyle::GetDefaultFontStyle(TEXT("Mono"), InSize);
	}

	EModDiagnosticSeverity WorstSeverity(const TArray<FModDiagnostic>& In)
	{
		EModDiagnosticSeverity Worst = EModDiagnosticSeverity::Info;
		for (const FModDiagnostic& Diagnostic : In)
		{
			if (Diagnostic.Severity == EModDiagnosticSeverity::Error)
			{
				return EModDiagnosticSeverity::Error;
			}

			if (Diagnostic.Severity == EModDiagnosticSeverity::Warning)
			{
				Worst = EModDiagnosticSeverity::Warning;
			}
		}

		return Worst;
	}

	FText SummariseDiagnostics(const TArray<FModDiagnostic>& In)
	{
		int32 Errors = 0;
		int32 Warnings = 0;
		int32 Infos = 0;

		for (const FModDiagnostic& Diagnostic : In)
		{
			switch (Diagnostic.Severity)
			{
			case EModDiagnosticSeverity::Error:
				++Errors;
				break;
			case EModDiagnosticSeverity::Warning:
				++Warnings;
				break;
			default:
				++Infos;
				break;
			}
		}

		if (Errors == 0 && Warnings == 0 && Infos == 0)
		{
			return LOCTEXT("NoDiagnostics", "No problems found.");
		}

		return FText::Format(
			LOCTEXT("DiagnosticSummary", "{0} error(s), {1} warning(s), {2} note(s)."),
			FText::AsNumber(Errors),
			FText::AsNumber(Warnings),
			FText::AsNumber(Infos));
	}
}

#undef LOCTEXT_NAMESPACE
