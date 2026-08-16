// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Fonts/SlateFontInfo.h"
#include "Internationalization/Text.h"
#include "Styling/SlateColor.h"

struct FSlateBrush;

/**
 * Presentation helpers shared by the Mod Developer panels.
 *
 * They exist so that a mod state is coloured and spelled the same way in the mod list, the
 * validation list and the dependency tree. The text always comes from ModFrameworkEnums::ToString,
 * which returns the stable enumerator identifier rather than a display name - a mod author reading
 * "DependenciesResolved" in this window can grep the docs and the log for the same word.
 */
namespace ModDeveloperUI
{
	/** Stable enumerator spelling of a lifecycle state, e.g. "Activated". */
	FText StateText(EModState In);

	/** Green for running, red for failed, amber for in-flight, grey for inert. */
	FSlateColor StateColor(EModState In);

	/** One sentence describing what a state means, for a tooltip. */
	FText StateTooltip(EModState In);

	FText SeverityText(EModDiagnosticSeverity In);

	FSlateColor SeverityColor(EModDiagnosticSeverity In);

	/** Icons.Error / Icons.Warning / Icons.Info from FAppStyle. Never null. */
	const FSlateBrush* SeverityIcon(EModDiagnosticSeverity In);

	FText FailureReasonText(EModLoadFailureReason In);

	FText ConflictPolicyText(EModConflictPolicy In);

	/** Monospaced font for the dependency tree and the packaging log, where alignment carries meaning. */
	FSlateFontInfo MonospaceFont(float InSize = 9.0f);

	/** The worst severity present, or Info for an empty array. */
	EModDiagnosticSeverity WorstSeverity(const TArray<FModDiagnostic>& In);

	/** "3 errors, 1 warning" - or "No problems found." when there is nothing to report. */
	FText SummariseDiagnostics(const TArray<FModDiagnostic>& In);
}
