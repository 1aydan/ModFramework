// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Styling/SlateColor.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class SWidget;
class UModSubsystem;
struct FSlateBrush;

/**
 * Small shared pieces of the game-developer inspector windows.
 *
 * Nothing here is game specific and nothing here is mod-author facing: these windows are the tooling
 * a studio uses to publish an SDK, and they ship in ModFrameworkEditor, which no bundle ever copies.
 */
namespace ModSDKWidgetUtils
{
	/**
	 * The mod subsystem of whatever game instance is currently live, or nullptr.
	 *
	 * UModSubsystem is a UGameInstanceSubsystem, so in a plain editor session there is no instance of
	 * it at all: the editor world has no game instance. Every inspector therefore has to work when
	 * this returns null and re-acquire when a PIE session starts. Never assert on the result.
	 */
	UModSubsystem* FindLiveModSubsystem();

	/**
	 * Names of every discovered plugin a bundle could plausibly be built from, sorted.
	 *
	 * Engine plugins are excluded because an SDK is game code, and ModFramework is excluded because
	 * it is copied into every bundle by definition and so is never the answer.
	 */
	TArray<FString> GetCandidateSdkPluginNames();

	/**
	 * The best guess at which discovered plugin holds the game's public modding surface.
	 *
	 * A name containing "SDK" wins; failing that the first candidate, which is a better starting point
	 * for a form field than an empty string. Empty when the project has no project-side plugin at all.
	 */
	FString PickDefaultSdkPluginName();

	/** Plugin name of the framework itself. Always part of a bundle, never the SDK. */
	extern const TCHAR* const FrameworkPluginName;

	/** Icon for a diagnostic severity. Never null. */
	const FSlateBrush* GetSeverityBrush(EModDiagnosticSeverity InSeverity);

	/** Tint for a diagnostic severity, from the editor style's own error/warning colours. */
	FSlateColor GetSeverityColor(EModDiagnosticSeverity InSeverity);

	/** "Error" / "Warning" / "Info". */
	FText GetSeverityLabel(EModDiagnosticSeverity InSeverity);

	/** "2 errors, 5 warnings, 1 info", skipping the zeroes. "No problems" when everything is zero. */
	FText MakeCountSummary(int32 InErrors, int32 InWarnings, int32 InInfos);

	/** A bold title with an optional grey subtitle underneath. */
	TSharedRef<SWidget> MakeSectionHeader(const FText& InTitle, const FText& InSubtitle = FText::GetEmpty());

	/** A boxed, severity-tinted explanatory message. Used for "nothing to show, and here is why". */
	TSharedRef<SWidget> MakeNotice(const FText& InTitle, const FText& InBody, EModDiagnosticSeverity InSeverity);

	/** Comma separated, or an em dash when the list is empty. */
	FText JoinNames(const TArray<FName>& InNames);

	/** Comma separated, or an em dash when the list is empty. */
	FText JoinStrings(const TArray<FString>& InStrings);

	/** "Yes" / "No". */
	FText YesNo(bool bValue);

	/** The string as text, or an em dash when it is empty. */
	FText OrDash(const FString& InValue);

	/** The name as text, or an em dash when it is None. */
	FText OrDash(FName InValue);
}
