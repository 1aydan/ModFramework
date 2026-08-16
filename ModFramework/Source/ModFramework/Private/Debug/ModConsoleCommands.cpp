// Copyright (c) 2026. Licensed for use in your own projects.

#include "Debug/ModConsoleCommands.h"

#include "Misc/Build.h"

#if !UE_BUILD_SHIPPING || ALLOW_CONSOLE

#include "API/ModAPI.h"
#include "API/ModAPIRegistry.h"
#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Content/ModContentManager.h"
#include "Content/ModContentTypes.h"
#include "Content/ModIconCache.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "Core/ModFrameworkVersion.h"
#include "CoreGlobals.h"
#include "CoreTypes.h"
#include "Dependencies/ModDependencyTypes.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Events/ModEventBus.h"
#include "Events/ModEventTypes.h"
#include "Extensions/ModExtension.h"
#include "Extensions/ModExtensionRegistry.h"
#include "HAL/IConsoleManager.h"
#include "Internationalization/Text.h"
#include "Logging/LogVerbosity.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModManifestParser.h"
#include "Manifest/ModVersion.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Net/ModSessionManifest.h"
#include "Permissions/ModPermissionRegistry.h"
#include "Permissions/ModPermissions.h"
#include "Registry/ModInfo.h"
#include "Registry/ModRegistry.h"
#include "Scripting/ModScriptManager.h"
#include "Scripting/ModScriptRuntime.h"
#include "Settings/ModFrameworkSettings.h"
#include "Subsystem/ModSubsystem.h"
#include "Templates/SubclassOf.h"
#include "Templates/Tuple.h"
#include "Templates/UniquePtr.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectPtr.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectBase.h"

/**
 * Everything below is private to this translation unit.
 *
 * A named namespace rather than an anonymous one so that a symbol showing up in a stack trace or a
 * linker message says where it came from.
 */
namespace ModConsoleCommandsPrivate
{
	// --- Output ----------------------------------------------------------------------------------

	/**
	 * Writes one line to LogModFramework and, when the console handed us one, to its output device.
	 *
	 * The output device is deliberately skipped when it *is* GLog: UE_LOG has already written there,
	 * and echoing would double every line of every table in the editor's Output Log.
	 */
	void EmitLine(FOutputDevice* Ar, ELogVerbosity::Type Verbosity, const FString& Line)
	{
		switch (Verbosity)
		{
		case ELogVerbosity::Error:
			UE_LOG(LogModFramework, Error, TEXT("%s"), *Line);
			break;

		case ELogVerbosity::Warning:
			UE_LOG(LogModFramework, Warning, TEXT("%s"), *Line);
			break;

		default:
			UE_LOG(LogModFramework, Display, TEXT("%s"), *Line);
			break;
		}

		if (Ar != nullptr && Ar != static_cast<FOutputDevice*>(GLog))
		{
			Ar->Log(Verbosity, Line);
		}
	}

	/** Ordinary output. */
	void Emit(FOutputDevice* Ar, const FString& Line)
	{
		EmitLine(Ar, ELogVerbosity::Display, Line);
	}

	/** Something the operator should notice but that did not stop the command. */
	void EmitWarning(FOutputDevice* Ar, const FString& Line)
	{
		EmitLine(Ar, ELogVerbosity::Warning, Line);
	}

	/** The command could not do what was asked. */
	void EmitError(FOutputDevice* Ar, const FString& Line)
	{
		EmitLine(Ar, ELogVerbosity::Error, Line);
	}

	/** A blank separator line, so consecutive sections do not run together. */
	void EmitBlank(FOutputDevice* Ar)
	{
		Emit(Ar, FString());
	}

	// --- Small string helpers --------------------------------------------------------------------

	/** Trimmed value, or "-" when there is nothing to show. Keeps tables free of empty cells. */
	FString OrDash(const FString& In)
	{
		const FString Trimmed = In.TrimStartAndEnd();
		return Trimmed.IsEmpty() ? FString(TEXT("-")) : Trimmed;
	}

	FString YesNo(bool bValue)
	{
		return bValue ? FString(TEXT("yes")) : FString(TEXT("no"));
	}

	/** Joins names for a single table cell. Answers "-" for an empty list. */
	FString JoinNames(const TArray<FName>& In, const TCHAR* Separator = TEXT(", "))
	{
		if (In.Num() == 0)
		{
			return FString(TEXT("-"));
		}

		FString Result;
		for (int32 Index = 0; Index < In.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += Separator;
			}
			Result += In[Index].ToString();
		}
		return Result;
	}

	/** JoinNames for mod ids. */
	FString JoinModIds(const TArray<FModId>& In, const TCHAR* Separator = TEXT(", "))
	{
		if (In.Num() == 0)
		{
			return FString(TEXT("-"));
		}

		FString Result;
		for (int32 Index = 0; Index < In.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += Separator;
			}
			Result += In[Index].ToString();
		}
		return Result;
	}

	/** Version range as written by the mod author, with "*" for the empty expression. */
	FString DescribeRange(const FModVersionRange& Range)
	{
		FString Text = Range.Expression.TrimStartAndEnd();
		if (Text.IsEmpty())
		{
			Text = TEXT("*");
		}

		if (!Range.IsValid())
		{
			Text += TEXT("  <-- does not parse");
		}

		return Text;
	}

	/** Load order as a printable cell; unordered mods show "-" rather than -1. */
	FString DescribeLoadOrder(int32 LoadOrder)
	{
		return LoadOrder == INDEX_NONE ? FString(TEXT("-")) : FString::FromInt(LoadOrder);
	}

	// --- Table writer ----------------------------------------------------------------------------

	/**
	 * Collects rows and prints them with every column padded to the width of its widest cell, which
	 * is what makes the list commands readable when mod ids differ wildly in length.
	 *
	 * The final column is never padded, so no line carries trailing whitespace.
	 */
	class FTableWriter
	{
	public:
		FTableWriter(const TCHAR* InIndent, TArray<FString> InHeaders)
			: Indent(InIndent)
			, Headers(MoveTemp(InHeaders))
		{
		}

		void AddRow(TArray<FString> InCells)
		{
			Rows.Emplace(MoveTemp(InCells));
		}

		int32 NumRows() const
		{
			return Rows.Num();
		}

		void Flush(FOutputDevice* Ar) const
		{
			const int32 ColumnCount = Headers.Num();
			if (ColumnCount == 0)
			{
				return;
			}

			TArray<int32> Widths;
			Widths.Reserve(ColumnCount);
			for (const FString& Header : Headers)
			{
				Widths.Add(Header.Len());
			}

			for (const TArray<FString>& Row : Rows)
			{
				const int32 Limit = FMath::Min(ColumnCount, Row.Num());
				for (int32 Column = 0; Column < Limit; ++Column)
				{
					Widths[Column] = FMath::Max(Widths[Column], Row[Column].Len());
				}
			}

			TArray<FString> RuleCells;
			RuleCells.Reserve(ColumnCount);
			for (int32 Column = 0; Column < ColumnCount; ++Column)
			{
				RuleCells.Add(FString::ChrN(Widths[Column], TEXT('-')));
			}

			Emit(Ar, ComposeRow(Widths, Headers));
			Emit(Ar, ComposeRow(Widths, RuleCells));
			for (const TArray<FString>& Row : Rows)
			{
				Emit(Ar, ComposeRow(Widths, Row));
			}
		}

	private:
		FString ComposeRow(const TArray<int32>& Widths, const TArray<FString>& Cells) const
		{
			const int32 ColumnCount = Widths.Num();

			FString Line = Indent;
			for (int32 Column = 0; Column < ColumnCount; ++Column)
			{
				const FString Cell = Cells.IsValidIndex(Column) ? Cells[Column] : FString();
				if (Column == ColumnCount - 1)
				{
					Line += Cell;
				}
				else
				{
					Line += Cell.RightPad(Widths[Column]);
					Line += TEXT("  ");
				}
			}

			return Line;
		}

		FString Indent;
		TArray<FString> Headers;
		TArray<TArray<FString>> Rows;
	};

	/** "Key   Value" pairs with the key column sized from the data. Used by the detail commands. */
	class FFieldWriter
	{
	public:
		explicit FFieldWriter(const TCHAR* InIndent)
			: Indent(InIndent)
		{
		}

		void Add(const TCHAR* Key, const FString& Value)
		{
			Fields.Emplace(FString(Key), Value);
		}

		void Flush(FOutputDevice* Ar) const
		{
			int32 KeyWidth = 0;
			for (const TPair<FString, FString>& Field : Fields)
			{
				KeyWidth = FMath::Max(KeyWidth, Field.Key.Len());
			}

			for (const TPair<FString, FString>& Field : Fields)
			{
				Emit(Ar, Indent + Field.Key.RightPad(KeyWidth) + TEXT("  ") + Field.Value);
			}
		}

	private:
		FString Indent;
		TArray<TPair<FString, FString>> Fields;
	};

	// --- Mod id suggestions ----------------------------------------------------------------------

	/**
	 * Case-insensitive Levenshtein distance.
	 *
	 * Mod ids are reverse-DNS strings up to 128 characters long, so they get mistyped constantly and
	 * a bare "no such mod" is close to useless. Cost is bounded by the id length rule in
	 * FModId::IsValidIdString, which makes the worst case 129 x 129 integer operations.
	 */
	int32 EditDistance(const FString& InA, const FString& InB)
	{
		const FString A = InA.ToLower();
		const FString B = InB.ToLower();

		const int32 LenA = A.Len();
		const int32 LenB = B.Len();
		if (LenA == 0)
		{
			return LenB;
		}
		if (LenB == 0)
		{
			return LenA;
		}

		TArray<int32> Previous;
		TArray<int32> Current;
		Previous.SetNumUninitialized(LenB + 1);
		Current.SetNumUninitialized(LenB + 1);

		for (int32 Column = 0; Column <= LenB; ++Column)
		{
			Previous[Column] = Column;
		}

		for (int32 Row = 1; Row <= LenA; ++Row)
		{
			Current[0] = Row;
			for (int32 Column = 1; Column <= LenB; ++Column)
			{
				const int32 Substitution = Previous[Column - 1] + (A[Row - 1] == B[Column - 1] ? 0 : 1);
				const int32 Deletion = Previous[Column] + 1;
				const int32 Insertion = Current[Column - 1] + 1;
				Current[Column] = FMath::Min(Substitution, FMath::Min(Deletion, Insertion));
			}

			Previous = Current;
		}

		return Previous[LenB];
	}

	/** One candidate answer to "did you mean...?". Lower Score is a better match. */
	struct FModIdSuggestion
	{
		FModId Id;
		int32 Score = 0;
	};

	/**
	 * The closest known ids to a string the operator typed.
	 *
	 * A known id that *contains* the typed text scores 0 - typing "combat" for
	 * "com.example.bettercombat" is the overwhelmingly common case and edit distance handles it
	 * badly - and everything else scores its edit distance, culled at a threshold that scales with
	 * how much was typed so a two letter typo does not print the whole mod list.
	 */
	TArray<FModId> SuggestModIds(const TArray<FModId>& Known, const FString& Query, int32 MaxResults)
	{
		TArray<FModIdSuggestion> Scored;
		Scored.Reserve(Known.Num());

		const int32 Threshold = FMath::Max(3, Query.Len() / 2);

		for (const FModId& Candidate : Known)
		{
			const FString CandidateText = Candidate.ToString();

			int32 Score = 0;
			if (!Query.IsEmpty() && CandidateText.Contains(Query))
			{
				Score = 0;
			}
			else
			{
				Score = EditDistance(CandidateText, Query);
				if (Score > Threshold)
				{
					continue;
				}
			}

			Scored.Add(FModIdSuggestion{ Candidate, Score });
		}

		Scored.Sort([](const FModIdSuggestion& A, const FModIdSuggestion& B)
		{
			if (A.Score != B.Score)
			{
				return A.Score < B.Score;
			}
			return A.Id < B.Id;
		});

		TArray<FModId> Result;
		const int32 Count = FMath::Min(MaxResults, Scored.Num());
		Result.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Result.Add(Scored[Index].Id);
		}

		return Result;
	}

	// --- Subsystem plumbing ----------------------------------------------------------------------

	/**
	 * The bEnableConsoleCommands gate.
	 *
	 * This runs at *execution* time, never at registration time: the module starts at PostConfigInit,
	 * long before UModFrameworkSettings has a class default object to read.
	 *
	 * UObjectInitialized() is checked *before* the settings accessor rather than merely null checking
	 * its result, because UModFrameworkSettings::Get() goes through GetDefault<>, and
	 * UClass::GetDefaultObject() *constructs* a missing CDO instead of answering null. A command run
	 * from an early -ExecCmds startup line would therefore force a CDO into existence mid-bootstrap
	 * rather than returning something to null check. The null check stays as a second line of
	 * defence for the window where the system is up but this particular class is not.
	 */
	bool AreCommandsEnabled(FOutputDevice* Ar)
	{
		if (!UObjectInitialized())
		{
			EmitWarning(Ar, TEXT("Mod framework console commands are not available yet - the UObject system has not finished starting up. Try again once the game is running."));
			return false;
		}

		const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
		if (Settings == nullptr)
		{
			EmitWarning(Ar, TEXT("Mod framework settings are not available yet - the UObject system has not finished starting up. Try again once the game is running."));
			return false;
		}

		if (!Settings->bEnableConsoleCommands)
		{
			EmitWarning(Ar, TEXT("Mod framework console commands are disabled. Set bEnableConsoleCommands under Project Settings > Plugins > Mod Framework to use them."));
			return false;
		}

		return true;
	}

	/**
	 * The mod subsystem for the world the console executed against.
	 *
	 * The console hands us a world for an in-game or PIE console and nothing at all for the editor's
	 * command line before play starts, so a null world falls back to scanning the engine's world
	 * contexts for a game instance. When there is genuinely no game instance - the editor at rest, a
	 * commandlet, a dedicated cook - this explains that rather than returning something to crash on.
	 */
	UModSubsystem* ResolveSubsystem(UWorld* World, FOutputDevice* Ar, const TCHAR* CommandName)
	{
		if (World != nullptr)
		{
			if (UModSubsystem* FromWorld = UModSubsystem::Get(World))
			{
				return FromWorld;
			}
		}

		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
				{
					continue;
				}

				if (UGameInstance* GameInstance = Context.OwningGameInstance)
				{
					if (UModSubsystem* FromContext = UModSubsystem::Get(GameInstance))
					{
						return FromContext;
					}
				}
			}
		}

		EmitError(Ar, FString::Printf(
			TEXT("%s: no game instance is running, so there is no mod subsystem to talk to. Start the game (or Play In Editor) and try again."),
			CommandName));
		return nullptr;
	}

	/** The mod table, or null with an explanation. A partly initialised subsystem has no registry. */
	UModRegistry* ResolveRegistry(UModSubsystem* Subsystem, FOutputDevice* Ar, const TCHAR* CommandName)
	{
		UModRegistry* Registry = Subsystem != nullptr ? Subsystem->GetRegistry() : nullptr;
		if (Registry == nullptr)
		{
			EmitError(Ar, FString::Printf(
				TEXT("%s: the mod subsystem has no registry - it failed to initialise. Check the log for earlier errors."),
				CommandName));
		}

		return Registry;
	}

	/** Notes arguments the command is about to ignore instead of silently dropping them. */
	void WarnAboutExtraArgs(const TArray<FString>& Args, int32 ExpectedCount, FOutputDevice* Ar, const TCHAR* CommandName)
	{
		if (Args.Num() > ExpectedCount)
		{
			EmitWarning(Ar, FString::Printf(TEXT("%s: ignoring %d extra argument(s)."),
				CommandName, Args.Num() - ExpectedCount));
		}
	}

	/** One compact "<id>  <state>" line per known mod, used by the usage messages. */
	void EmitKnownModIds(const UModRegistry& Registry, FOutputDevice* Ar)
	{
		const TArray<FModInfo> Mods = Registry.GetAllMods();
		if (Mods.Num() == 0)
		{
			Emit(Ar, TEXT("  (no mods are registered - run Mod.Refresh to discover some)"));
			return;
		}

		Emit(Ar, FString::Printf(TEXT("  Known mod ids (%d):"), Mods.Num()));

		FTableWriter Table(TEXT("    "), { TEXT("ID"), TEXT("VERSION"), TEXT("STATE") });
		for (const FModInfo& Info : Mods)
		{
			Table.AddRow({
				Info.GetId().ToString(),
				Info.Manifest.Version.ToString(),
				ModFrameworkEnums::ToString(Info.State) });
		}
		Table.Flush(Ar);
	}

	/**
	 * Turns the first argument into a registered mod id.
	 *
	 * No argument prints the usage line and the full list of valid ids; an unknown argument prints
	 * "no such mod" plus the closest matches by edit distance. Either way the caller gets false and
	 * has nothing left to do.
	 */
	bool ResolveModIdArgument(const TArray<FString>& Args, const UModRegistry& Registry, FOutputDevice* Ar,
		const TCHAR* CommandName, const TCHAR* Usage, FModId& OutModId)
	{
		const FString Raw = Args.Num() > 0 ? Args[0].TrimStartAndEnd() : FString();

		if (Raw.IsEmpty())
		{
			EmitWarning(Ar, FString::Printf(TEXT("%s: a mod id is required."), CommandName));
			Emit(Ar, FString::Printf(TEXT("  Usage: %s"), Usage));
			EmitKnownModIds(Registry, Ar);
			return false;
		}

		const FModId Candidate = FModId::FromString(Raw);
		if (Registry.IsModRegistered(Candidate))
		{
			OutModId = Candidate;
			return true;
		}

		EmitError(Ar, FString::Printf(TEXT("%s: no such mod '%s'."), CommandName, *Raw));

		FString ParseError;
		FModId Parsed;
		if (!FModId::TryParse(Raw, Parsed, ParseError))
		{
			Emit(Ar, FString::Printf(TEXT("  ('%s' is not a well-formed mod id: %s)"), *Raw, *ParseError));
		}

		const TArray<FModId> Suggestions = SuggestModIds(Registry.GetAllModIds(), Candidate.ToString(), 5);
		if (Suggestions.Num() > 0)
		{
			Emit(Ar, FString::Printf(TEXT("  Did you mean: %s"), *JoinModIds(Suggestions)));
		}
		else
		{
			EmitKnownModIds(Registry, Ar);
		}

		return false;
	}

	/** Snapshot of a mod's record, or null when the id vanished between calls. */
	const FModInfo* FindModOrExplain(const UModRegistry& Registry, const FModId& ModId, FOutputDevice* Ar, const TCHAR* CommandName)
	{
		const FModInfo* Info = Registry.FindMod(ModId);
		if (Info == nullptr)
		{
			EmitError(Ar, FString::Printf(TEXT("%s: mod '%s' is no longer registered."), CommandName, *ModId.ToString()));
		}

		return Info;
	}

	/** The state a mod is in right now, or Unknown when it is not registered. */
	EModState GetModState(const UModRegistry& Registry, const FModId& ModId)
	{
		const FModInfo* Info = Registry.FindMod(ModId);
		return Info != nullptr ? Info->State : EModState::Unknown;
	}

	/** "mod 'x' moved from Loaded to Activated" / "mod 'x' is still Loaded". */
	void EmitTransition(FOutputDevice* Ar, const TCHAR* CommandName, const FModId& ModId, EModState Before, EModState After)
	{
		if (Before == After)
		{
			Emit(Ar, FString::Printf(TEXT("%s: mod '%s' is still %s."),
				CommandName, *ModId.ToString(), *ModFrameworkEnums::ToString(After)));
		}
		else
		{
			Emit(Ar, FString::Printf(TEXT("%s: mod '%s' moved %s -> %s."),
				CommandName, *ModId.ToString(),
				*ModFrameworkEnums::ToString(Before), *ModFrameworkEnums::ToString(After)));
		}
	}

	/** Prints a mod's failure reason and message when it has one. */
	void EmitFailureIfAny(FOutputDevice* Ar, const UModRegistry& Registry, const FModId& ModId)
	{
		const FModInfo* Info = Registry.FindMod(ModId);
		if (Info == nullptr || Info->FailureReason == EModLoadFailureReason::None)
		{
			return;
		}

		EmitWarning(Ar, FString::Printf(TEXT("  Failure: %s - %s"),
			*ModFrameworkEnums::ToString(Info->FailureReason),
			*OrDash(Info->FailureMessage)));
	}

	/** Counts of the states worth summarising after a bulk operation. */
	struct FModStateTally
	{
		int32 Total = 0;
		int32 Activated = 0;
		int32 Loaded = 0;
		int32 Mounted = 0;
		int32 Failed = 0;
		int32 Disabled = 0;

		FString ToString() const
		{
			return FString::Printf(TEXT("%d mod(s): %d activated, %d loaded, %d mounted, %d failed, %d disabled"),
				Total, Activated, Loaded, Mounted, Failed, Disabled);
		}
	};

	FModStateTally TallyMods(const UModRegistry& Registry)
	{
		FModStateTally Tally;
		for (const FModInfo& Info : Registry.GetAllMods())
		{
			++Tally.Total;
			switch (Info.State)
			{
			case EModState::Activated:
				++Tally.Activated;
				break;
			case EModState::Loaded:
			case EModState::Deactivated:
				++Tally.Loaded;
				break;
			case EModState::Mounted:
				++Tally.Mounted;
				break;
			case EModState::Failed:
				++Tally.Failed;
				break;
			case EModState::Disabled:
				++Tally.Disabled;
				break;
			default:
				break;
			}
		}

		return Tally;
	}

	// --- Mod.List --------------------------------------------------------------------------------

	/** Filter argument matching: a state name prefix, or a substring of the id or the display name. */
	bool MatchesFilter(const FModInfo& Info, const FString& Filter)
	{
		if (Filter.IsEmpty())
		{
			return true;
		}

		if (ModFrameworkEnums::ToString(Info.State).StartsWith(Filter))
		{
			return true;
		}

		return Info.GetId().ToString().Contains(Filter) || Info.Manifest.DisplayName.Contains(Filter);
	}

	void CmdList(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.List");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		const FString Filter = Args.Num() > 0 ? Args[0].TrimStartAndEnd() : FString();
		const TArray<FModInfo> Mods = Registry->GetAllMods();

		if (Mods.Num() == 0)
		{
			Emit(&Ar, TEXT("Mod.List: no mods are registered. Run Mod.Refresh to discover mods."));
			return;
		}

		FTableWriter Table(TEXT("  "), {
			TEXT("ORDER"), TEXT("ID"), TEXT("VERSION"), TEXT("STATE"), TEXT("ON"), TEXT("MOUNTS"), TEXT("NAME") });

		int32 Shown = 0;
		for (const FModInfo& Info : Mods)
		{
			if (!MatchesFilter(Info, Filter))
			{
				continue;
			}

			++Shown;
			Table.AddRow({
				DescribeLoadOrder(Info.LoadOrder),
				Info.GetId().ToString(),
				Info.Manifest.Version.ToString(),
				ModFrameworkEnums::ToString(Info.State),
				YesNo(Info.bEnabled),
				FString::FromInt(Info.MountedPaths.Num()),
				OrDash(Info.Manifest.GetDisplayNameOrId()) });
		}

		if (Shown == 0)
		{
			Emit(&Ar, FString::Printf(TEXT("Mod.List: none of the %d registered mods match '%s'."), Mods.Num(), *Filter));
			return;
		}

		if (Filter.IsEmpty())
		{
			Emit(&Ar, FString::Printf(TEXT("Mod.List: %d mod(s)."), Shown));
		}
		else
		{
			Emit(&Ar, FString::Printf(TEXT("Mod.List: %d of %d mod(s) matching '%s'."), Shown, Mods.Num(), *Filter));
		}

		Table.Flush(&Ar);
		Emit(&Ar, FString::Printf(TEXT("  %s"), *TallyMods(*Registry).ToString()));
	}

	// --- Mod.Info --------------------------------------------------------------------------------

	/** How one declared dependency actually stands right now. */
	FString DescribeDependencyStatus(const UModRegistry& Registry, const FModDependency& Dependency)
	{
		const FModInfo* Installed = Registry.FindMod(Dependency.Id);
		if (Installed == nullptr)
		{
			return Dependency.bOptional ? FString(TEXT("absent (optional)")) : FString(TEXT("MISSING"));
		}

		if (!Dependency.VersionRange.Satisfies(Installed->Manifest.Version))
		{
			return FString(TEXT("VERSION MISMATCH"));
		}

		if (Installed->State == EModState::Failed)
		{
			return FString(TEXT("dependency FAILED"));
		}

		if (!Installed->bEnabled)
		{
			return FString(TEXT("present but disabled"));
		}

		if (!Installed->IsLoaded())
		{
			return FString(TEXT("present, not loaded"));
		}

		return FString(TEXT("ok"));
	}

	void CmdInfo(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Info");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		FModId ModId;
		if (!ResolveModIdArgument(Args, *Registry, &Ar, CommandName, TEXT("Mod.Info <ModId>"), ModId))
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		const FModInfo* InfoPtr = FindModOrExplain(*Registry, ModId, &Ar, CommandName);
		if (InfoPtr == nullptr)
		{
			return;
		}

		// Copied out because everything below queries the registry again, and FindMod's pointer is
		// invalidated by any mutation.
		const FModInfo Info = *InfoPtr;
		const FModManifest& Manifest = Info.Manifest;

		Emit(&Ar, FString::Printf(TEXT("Mod.Info: %s"), *ModId.ToString()));

		FFieldWriter Fields(TEXT("  "));
		Fields.Add(TEXT("Name"), OrDash(Manifest.DisplayName));
		Fields.Add(TEXT("Version"), Manifest.Version.ToString());
		Fields.Add(TEXT("State"), ModFrameworkEnums::ToString(Info.State));
		Fields.Add(TEXT("Enabled"), YesNo(Info.bEnabled));
		Fields.Add(TEXT("Load order"), DescribeLoadOrder(Info.LoadOrder));
		Fields.Add(TEXT("Author"), OrDash(Manifest.Author));
		Fields.Add(TEXT("Homepage"), OrDash(Manifest.Homepage));
		Fields.Add(TEXT("License"), OrDash(Manifest.License));
		Fields.Add(TEXT("Description"), OrDash(Manifest.Description));
		Fields.Add(TEXT("Manifest version"), FString::FromInt(Manifest.ManifestVersion));
		Fields.Add(TEXT("Provider"), OrDash(Info.ProviderId.ToString()));
		Fields.Add(TEXT("Root path"), OrDash(Info.RootPath));
		Fields.Add(TEXT("Icon"), OrDash(Manifest.IconPath));
		Fields.Add(TEXT("Icon file"), OrDash(Info.ResolvedIconPath));
		Fields.Add(TEXT("Game"), FString::Printf(TEXT("%s %s"),
			*OrDash(Manifest.Game.GameId), *DescribeRange(Manifest.Game.VersionRange)));
		Fields.Add(TEXT("Framework"), DescribeRange(Manifest.FrameworkVersionRange));
		Fields.Add(TEXT("SDK"), Manifest.Sdk.SdkId.IsEmpty()
			? FString(TEXT("-"))
			: FString::Printf(TEXT("%s %s"), *Manifest.Sdk.SdkId, *DescribeRange(Manifest.Sdk.VersionRange)));
		Fields.Add(TEXT("Priority"), FString::FromInt(Manifest.Priority));
		Fields.Add(TEXT("Network scope"), ModFrameworkEnums::ToString(Manifest.NetworkScope));
		Fields.Add(TEXT("Required for net play"), YesNo(Manifest.bRequiredForNetworkPlay));
		Fields.Add(TEXT("Load before"), JoinModIds(Manifest.LoadBefore));
		Fields.Add(TEXT("Load after"), JoinModIds(Manifest.LoadAfter));
		Fields.Add(TEXT("Entry class"), Manifest.EntryPoint.EntryClass.IsNull()
			? FString(TEXT("- (pure asset mod)"))
			: Manifest.EntryPoint.EntryClass.ToString());
		Fields.Add(TEXT("Native module"), OrDash(Manifest.EntryPoint.NativeModule));
		Fields.Add(TEXT("Context"), Subsystem->GetModContext(ModId) != nullptr
			? FString(TEXT("live"))
			: FString(TEXT("none")));
		Fields.Add(TEXT("Permissions requested"), JoinNames(Manifest.RequestedPermissions));
		Fields.Add(TEXT("Permissions granted"), JoinNames(Info.GrantedPermissions));
		Fields.Flush(&Ar);

		if (Info.FailureReason != EModLoadFailureReason::None)
		{
			EmitBlank(&Ar);
			EmitWarning(&Ar, FString::Printf(TEXT("  Failure: %s"), *ModFrameworkEnums::ToString(Info.FailureReason)));
			EmitWarning(&Ar, FString::Printf(TEXT("           %s"), *OrDash(Info.FailureMessage)));
		}

		if (Manifest.Dependencies.Num() > 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Dependencies (%d):"), Manifest.Dependencies.Num()));

			FTableWriter Table(TEXT("    "), {
				TEXT("ID"), TEXT("REQUIRES"), TEXT("KIND"), TEXT("INSTALLED"), TEXT("STATUS") });
			for (const FModDependency& Dependency : Manifest.Dependencies)
			{
				const FModInfo* Installed = Registry->FindMod(Dependency.Id);
				Table.AddRow({
					Dependency.Id.ToString(),
					DescribeRange(Dependency.VersionRange),
					Dependency.bOptional ? TEXT("optional") : TEXT("required"),
					Installed != nullptr ? Installed->Manifest.Version.ToString() : FString(TEXT("-")),
					DescribeDependencyStatus(*Registry, Dependency) });
			}
			Table.Flush(&Ar);
		}

		if (Manifest.ContentRoots.Num() > 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Content roots (%d):"), Manifest.ContentRoots.Num()));

			FTableWriter Table(TEXT("    "), { TEXT("PATH"), TEXT("TYPE"), TEXT("MOUNT POINT"), TEXT("ORDER") });
			for (const FModContentRoot& Root : Manifest.ContentRoots)
			{
				Table.AddRow({
					OrDash(Root.RelativePath),
					ModFrameworkEnums::ToString(Root.Type),
					OrDash(Root.MountPoint),
					FString::FromInt(Root.MountOrder) });
			}
			Table.Flush(&Ar);
		}

		if (Info.MountedPaths.Num() > 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Mounted at (%d):"), Info.MountedPaths.Num()));
			for (const FString& Path : Info.MountedPaths)
			{
				Emit(&Ar, FString::Printf(TEXT("    %s"), *Path));
			}
		}

		if (Manifest.EntryPoint.ContentBundles.Num() > 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Content bundles (%d):"), Manifest.EntryPoint.ContentBundles.Num()));
			for (const FSoftObjectPath& Bundle : Manifest.EntryPoint.ContentBundles)
			{
				Emit(&Ar, FString::Printf(TEXT("    %s"), *Bundle.ToString()));
			}
		}

		if (Manifest.Claims.Num() > 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Declared claims (%d):"), Manifest.Claims.Num()));

			FTableWriter Table(TEXT("    "), { TEXT("EXTENSION POINT"), TEXT("RESOURCE"), TEXT("PREFERRED POLICY") });
			for (const FModResourceClaimDeclaration& Claim : Manifest.Claims)
			{
				Table.AddRow({
					OrDash(Claim.ExtensionPointId.ToString()),
					OrDash(Claim.ResourceId.ToString()),
					ModFrameworkEnums::ToString(Claim.PreferredPolicy) });
			}
			Table.Flush(&Ar);
		}

		if (Manifest.Metadata.Num() > 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Metadata (%d):"), Manifest.Metadata.Num()));

			TArray<FString> Keys;
			Manifest.Metadata.GetKeys(Keys);
			Keys.Sort();

			FFieldWriter MetadataFields(TEXT("    "));
			for (const FString& Key : Keys)
			{
				if (const FString* Value = Manifest.Metadata.Find(Key))
				{
					MetadataFields.Add(*Key, *Value);
				}
			}
			MetadataFields.Flush(&Ar);
		}

		if (Info.Diagnostics.Num() > 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Diagnostics (%d):"), Info.Diagnostics.Num()));
			for (const FModDiagnostic& Diagnostic : Info.Diagnostics)
			{
				EmitLine(&Ar,
					Diagnostic.IsError() ? ELogVerbosity::Warning : ELogVerbosity::Display,
					FString::Printf(TEXT("    %s"), *Diagnostic.ToString()));
			}
		}
	}

	// --- Lifecycle commands ----------------------------------------------------------------------

	void CmdLoad(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Load");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		FModId ModId;
		if (!ResolveModIdArgument(Args, *Registry, &Ar, CommandName, TEXT("Mod.Load <ModId>"), ModId))
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		const EModState Before = GetModState(*Registry, ModId);
		if (Before == EModState::Disabled)
		{
			EmitWarning(&Ar, FString::Printf(
				TEXT("%s: mod '%s' is disabled. Run Mod.Enable %s first."),
				CommandName, *ModId.ToString(), *ModId.ToString()));
			return;
		}

		// Loading requires EModState::Mounted, so mount first when the mod is somewhere the state
		// machine allows a mount from. Both calls refuse politely from anywhere else.
		EModState Current = Before;
		if (Current == EModState::DependenciesResolved || Current == EModState::Unmounted)
		{
			if (!Subsystem->MountMod(ModId))
			{
				EmitError(&Ar, FString::Printf(TEXT("%s: could not mount mod '%s'."), CommandName, *ModId.ToString()));
				EmitFailureIfAny(&Ar, *Registry, ModId);
				return;
			}

			Current = GetModState(*Registry, ModId);
			Emit(&Ar, FString::Printf(TEXT("%s: mounted '%s'."), CommandName, *ModId.ToString()));
		}

		if (!Subsystem->LoadMod(ModId))
		{
			EmitError(&Ar, FString::Printf(
				TEXT("%s: could not load mod '%s' (it is %s; loading requires Mounted)."),
				CommandName, *ModId.ToString(), *ModFrameworkEnums::ToString(Current)));
			EmitFailureIfAny(&Ar, *Registry, ModId);
			return;
		}

		EmitTransition(&Ar, CommandName, ModId, Before, GetModState(*Registry, ModId));
	}

	void CmdUnload(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Unload");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		FModId ModId;
		if (!ResolveModIdArgument(Args, *Registry, &Ar, CommandName, TEXT("Mod.Unload <ModId>"), ModId))
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		const EModState Before = GetModState(*Registry, ModId);
		if (!Subsystem->UnloadMod(ModId))
		{
			EmitError(&Ar, FString::Printf(TEXT("%s: could not unload mod '%s' (it is %s)."),
				CommandName, *ModId.ToString(), *ModFrameworkEnums::ToString(Before)));
			EmitFailureIfAny(&Ar, *Registry, ModId);
			return;
		}

		EmitTransition(&Ar, CommandName, ModId, Before, GetModState(*Registry, ModId));
	}

	void CmdActivate(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Activate");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		FModId ModId;
		if (!ResolveModIdArgument(Args, *Registry, &Ar, CommandName, TEXT("Mod.Activate <ModId>"), ModId))
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		const EModState Before = GetModState(*Registry, ModId);
		if (Before == EModState::Activated)
		{
			Emit(&Ar, FString::Printf(TEXT("%s: mod '%s' is already activated."), CommandName, *ModId.ToString()));
			return;
		}

		if (Before == EModState::Disabled)
		{
			EmitWarning(&Ar, FString::Printf(
				TEXT("%s: mod '%s' is disabled. Run Mod.Enable %s first."),
				CommandName, *ModId.ToString(), *ModId.ToString()));
			return;
		}

		// Activation requires Loaded or Deactivated, so bring a merely mounted mod up first.
		if (GetModState(*Registry, ModId) == EModState::Mounted)
		{
			if (!Subsystem->LoadMod(ModId))
			{
				EmitError(&Ar, FString::Printf(TEXT("%s: could not load mod '%s' before activating it."),
					CommandName, *ModId.ToString()));
				EmitFailureIfAny(&Ar, *Registry, ModId);
				return;
			}

			Emit(&Ar, FString::Printf(TEXT("%s: loaded '%s'."), CommandName, *ModId.ToString()));
		}

		if (!Subsystem->ActivateMod(ModId))
		{
			EmitError(&Ar, FString::Printf(
				TEXT("%s: could not activate mod '%s' (it is %s; activation requires Loaded or Deactivated)."),
				CommandName, *ModId.ToString(), *ModFrameworkEnums::ToString(GetModState(*Registry, ModId))));
			EmitFailureIfAny(&Ar, *Registry, ModId);
			return;
		}

		EmitTransition(&Ar, CommandName, ModId, Before, GetModState(*Registry, ModId));
	}

	void CmdDeactivate(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Deactivate");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		FModId ModId;
		if (!ResolveModIdArgument(Args, *Registry, &Ar, CommandName, TEXT("Mod.Deactivate <ModId>"), ModId))
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		const EModState Before = GetModState(*Registry, ModId);
		if (!Subsystem->DeactivateMod(ModId))
		{
			EmitError(&Ar, FString::Printf(
				TEXT("%s: could not deactivate mod '%s' (it is %s; only an activated mod can be deactivated)."),
				CommandName, *ModId.ToString(), *ModFrameworkEnums::ToString(Before)));
			return;
		}

		EmitTransition(&Ar, CommandName, ModId, Before, GetModState(*Registry, ModId));
	}

	void CmdReload(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Reload");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		FModId ModId;
		if (!ResolveModIdArgument(Args, *Registry, &Ar, CommandName, TEXT("Mod.Reload <ModId>"), ModId))
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		const EModState Before = GetModState(*Registry, ModId);
		if (!Subsystem->ReloadMod(ModId))
		{
			EmitError(&Ar, FString::Printf(TEXT("%s: could not reload mod '%s' (it is %s)."),
				CommandName, *ModId.ToString(), *ModFrameworkEnums::ToString(Before)));
			EmitFailureIfAny(&Ar, *Registry, ModId);
			return;
		}

		EmitTransition(&Ar, CommandName, ModId, Before, GetModState(*Registry, ModId));
	}

	void CmdReloadAll(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.ReloadAll");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		const FModStateTally Before = TallyMods(*Registry);
		Emit(&Ar, FString::Printf(TEXT("Mod.ReloadAll: before - %s"), *Before.ToString()));

		Subsystem->ReloadAllMods();

		const FModStateTally After = TallyMods(*Registry);
		Emit(&Ar, FString::Printf(TEXT("Mod.ReloadAll: after  - %s"), *After.ToString()));
	}

	void CmdRefresh(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Refresh");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		bool bActivate = true;
		if (Args.Num() > 0)
		{
			const FString Option = Args[0].TrimStartAndEnd().ToLower();
			if (Option == TEXT("noactivate") || Option == TEXT("-noactivate")
				|| Option == TEXT("0") || Option == TEXT("false"))
			{
				bActivate = false;
			}
			else if (Option != TEXT("activate") && Option != TEXT("1") && Option != TEXT("true"))
			{
				EmitWarning(&Ar, FString::Printf(
					TEXT("%s: unrecognised option '%s'. Use 'noactivate' to stop after loading."),
					CommandName, *Args[0]));
			}
		}

		const FModStateTally Before = TallyMods(*Registry);
		Emit(&Ar, FString::Printf(TEXT("Mod.Refresh: before - %s"), *Before.ToString()));

		Subsystem->RefreshMods(bActivate);

		const FModStateTally After = TallyMods(*Registry);
		Emit(&Ar, FString::Printf(TEXT("Mod.Refresh: after  - %s%s"),
			*After.ToString(), bActivate ? TEXT("") : TEXT(" (activation skipped)")));

		const FModResolveResult Result = Subsystem->GetLastResolveResult();
		if (Result.Rejections.Num() > 0)
		{
			EmitWarning(&Ar, FString::Printf(TEXT("  %d mod(s) were rejected - run Mod.DumpRegistry for the detail."),
				Result.Rejections.Num()));
		}
	}

	// --- Mod.Validate ----------------------------------------------------------------------------

	/**
	 * Re-validates one mod.
	 *
	 * The manifest is re-read from disk when the mod is an unpacked folder, because a manifest edited
	 * since discovery is exactly what the operator is usually trying to check. A `.mod` package (whose
	 * manifest lives inside the container) falls back to validating the registered copy.
	 */
	void ValidateOneMod(const FModInfo& Info, FOutputDevice* Ar, int32& InOutErrorCount, int32& InOutWarningCount)
	{
		TArray<FModDiagnostic> Diagnostics;
		FString Source;

		const FString ManifestPath = Info.RootPath.IsEmpty()
			? FString()
			: FPaths::Combine(Info.RootPath, FModManifestParser::GetManifestFileName());

		if (!ManifestPath.IsEmpty() && FPaths::FileExists(ManifestPath))
		{
			const FModManifestParseResult Parsed = FModManifestParser::ParseFromFile(ManifestPath);
			Diagnostics = Parsed.Diagnostics;
			Source = ManifestPath;
		}
		else
		{
			FModManifestParser::ValidateManifest(Info.Manifest, Info.RootPath, Diagnostics);
			Source = TEXT("registered manifest (no readable mod.json on disk)");
		}

		int32 Errors = 0;
		int32 Warnings = 0;
		for (const FModDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Severity == EModDiagnosticSeverity::Error)
			{
				++Errors;
			}
			else if (Diagnostic.Severity == EModDiagnosticSeverity::Warning)
			{
				++Warnings;
			}
		}

		InOutErrorCount += Errors > 0 ? 1 : 0;
		InOutWarningCount += (Errors == 0 && Warnings > 0) ? 1 : 0;

		const TCHAR* Verdict = Errors > 0 ? TEXT("FAILED") : (Warnings > 0 ? TEXT("ok, with warnings") : TEXT("ok"));
		EmitLine(Ar,
			Errors > 0 ? ELogVerbosity::Warning : ELogVerbosity::Display,
			FString::Printf(TEXT("  %s: %s (%d error(s), %d warning(s))"),
				*Info.GetId().ToString(), Verdict, Errors, Warnings));

		if (Diagnostics.Num() > 0)
		{
			Emit(Ar, FString::Printf(TEXT("    source: %s"), *Source));
			for (const FModDiagnostic& Diagnostic : Diagnostics)
			{
				EmitLine(Ar,
					Diagnostic.IsError() ? ELogVerbosity::Warning : ELogVerbosity::Display,
					FString::Printf(TEXT("    %s"), *Diagnostic.ToString()));
			}
		}
	}

	void CmdValidate(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Validate");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		TArray<FModInfo> Targets;
		if (Args.Num() > 0 && !Args[0].TrimStartAndEnd().IsEmpty())
		{
			FModId ModId;
			if (!ResolveModIdArgument(Args, *Registry, &Ar, CommandName, TEXT("Mod.Validate [ModId]"), ModId))
			{
				return;
			}

			if (const FModInfo* Info = Registry->FindMod(ModId))
			{
				Targets.Add(*Info);
			}
		}
		else
		{
			Targets = Registry->GetAllMods();
		}

		if (Targets.Num() == 0)
		{
			Emit(&Ar, TEXT("Mod.Validate: no mods are registered."));
			return;
		}

		Emit(&Ar, FString::Printf(TEXT("Mod.Validate: re-validating %d mod(s)."), Targets.Num()));

		int32 WithErrors = 0;
		int32 WithWarnings = 0;
		for (const FModInfo& Info : Targets)
		{
			ValidateOneMod(Info, &Ar, WithErrors, WithWarnings);
		}

		EmitLine(&Ar,
			WithErrors > 0 ? ELogVerbosity::Warning : ELogVerbosity::Display,
			FString::Printf(TEXT("Mod.Validate: %d mod(s) checked, %d with errors, %d with warnings only."),
				Targets.Num(), WithErrors, WithWarnings));
	}

	// --- Mod.Dependencies ------------------------------------------------------------------------

	void CmdDependencies(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Dependencies");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		FModId ModId;
		if (!ResolveModIdArgument(Args, *Registry, &Ar, CommandName, TEXT("Mod.Dependencies <ModId>"), ModId))
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		const FModInfo* InfoPtr = FindModOrExplain(*Registry, ModId, &Ar, CommandName);
		if (InfoPtr == nullptr)
		{
			return;
		}

		const FModInfo Info = *InfoPtr;
		const TArray<FModInfo> AllMods = Registry->GetAllMods();

		Emit(&Ar, FString::Printf(TEXT("Mod.Dependencies: %s (%s, load order %s)"),
			*ModId.ToString(),
			*ModFrameworkEnums::ToString(Info.State),
			*DescribeLoadOrder(Info.LoadOrder)));

		if (Info.Manifest.Dependencies.Num() == 0)
		{
			Emit(&Ar, TEXT("  Depends on: nothing."));
		}
		else
		{
			Emit(&Ar, FString::Printf(TEXT("  Depends on (%d):"), Info.Manifest.Dependencies.Num()));

			FTableWriter Table(TEXT("    "), {
				TEXT("ID"), TEXT("REQUIRES"), TEXT("KIND"), TEXT("INSTALLED"), TEXT("STATE"), TEXT("STATUS"), TEXT("REASON") });
			for (const FModDependency& Dependency : Info.Manifest.Dependencies)
			{
				const FModInfo* Installed = Registry->FindMod(Dependency.Id);
				Table.AddRow({
					Dependency.Id.ToString(),
					DescribeRange(Dependency.VersionRange),
					Dependency.bOptional ? TEXT("optional") : TEXT("required"),
					Installed != nullptr ? Installed->Manifest.Version.ToString() : FString(TEXT("-")),
					Installed != nullptr ? ModFrameworkEnums::ToString(Installed->State) : FString(TEXT("-")),
					DescribeDependencyStatus(*Registry, Dependency),
					OrDash(Dependency.Reason) });
			}
			Table.Flush(&Ar);
		}

		// Reverse edges: who would break if this mod went away.
		FTableWriter Dependents(TEXT("    "), { TEXT("ID"), TEXT("REQUIRES"), TEXT("KIND"), TEXT("STATE") });
		for (const FModInfo& Other : AllMods)
		{
			if (Other.GetId() == ModId)
			{
				continue;
			}

			if (const FModDependency* Dependency = Other.Manifest.FindDependency(ModId))
			{
				Dependents.AddRow({
					Other.GetId().ToString(),
					DescribeRange(Dependency->VersionRange),
					Dependency->bOptional ? TEXT("optional") : TEXT("required"),
					ModFrameworkEnums::ToString(Other.State) });
			}
		}

		EmitBlank(&Ar);
		if (Dependents.NumRows() == 0)
		{
			Emit(&Ar, TEXT("  Depended on by: nothing."));
		}
		else
		{
			Emit(&Ar, FString::Printf(TEXT("  Depended on by (%d):"), Dependents.NumRows()));
			Dependents.Flush(&Ar);
		}

		EmitBlank(&Ar);
		FFieldWriter Ordering(TEXT("  "));
		Ordering.Add(TEXT("Load before"), JoinModIds(Info.Manifest.LoadBefore));
		Ordering.Add(TEXT("Load after"), JoinModIds(Info.Manifest.LoadAfter));
		Ordering.Add(TEXT("Priority"), FString::FromInt(Info.Manifest.Priority));
		Ordering.Flush(&Ar);

		const FModResolveResult Result = Subsystem->GetLastResolveResult();
		if (const FModRejection* Rejection = Result.FindRejection(ModId))
		{
			EmitBlank(&Ar);
			EmitWarning(&Ar, FString::Printf(TEXT("  Rejected by the resolver: %s"),
				*ModFrameworkEnums::ToString(Rejection->Reason)));
			EmitWarning(&Ar, FString::Printf(TEXT("    %s"), *OrDash(Rejection->Message)));
			if (Rejection->RelatedMods.Num() > 0)
			{
				EmitWarning(&Ar, FString::Printf(TEXT("    Related: %s"), *JoinModIds(Rejection->RelatedMods)));
			}
		}
	}

	// --- Mod.Mounts ------------------------------------------------------------------------------

	void CmdMounts(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Mounts");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		const TArray<FModContentMount> Mounts = Subsystem->GetContentManager().GetMounts();
		if (Mounts.Num() == 0)
		{
			Emit(&Ar, TEXT("Mod.Mounts: nothing is mounted."));
			return;
		}

		Emit(&Ar, FString::Printf(TEXT("Mod.Mounts: %d mount(s)."), Mounts.Num()));

		FTableWriter Table(TEXT("  "), {
			TEXT("MOD"), TEXT("MOUNT POINT"), TEXT("TYPE"), TEXT("ORDER"), TEXT("LIVE"), TEXT("MOUNTER"), TEXT("PKGS"), TEXT("PHYSICAL PATH") });

		int32 TotalPackages = 0;
		for (const FModContentMount& Mount : Mounts)
		{
			TotalPackages += Mount.DiscoveredPackages.Num();
			Table.AddRow({
				OrDash(Mount.OwnerModId.ToString()),
				OrDash(Mount.VirtualMountPoint),
				ModFrameworkEnums::ToString(Mount.Type),
				FString::FromInt(Mount.MountOrder),
				YesNo(Mount.bMounted),
				OrDash(Mount.MounterId.ToString()),
				FString::FromInt(Mount.DiscoveredPackages.Num()),
				OrDash(Mount.PhysicalPath) });
		}

		Table.Flush(&Ar);
		Emit(&Ar, FString::Printf(TEXT("  %d package(s) discovered across all mounts."), TotalPackages));
	}

	// --- Mod.DumpRegistry ------------------------------------------------------------------------

	void CmdDumpRegistry(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.DumpRegistry");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		const FModEnvironment Environment = Subsystem->GetEnvironment();

		Emit(&Ar, TEXT("Mod.DumpRegistry:"));

		FFieldWriter Header(TEXT("  "));
		Header.Add(TEXT("Framework"), ModFrameworkVersion::GetString());
		Header.Add(TEXT("Game"), FString::Printf(TEXT("%s %s"),
			*OrDash(Environment.GameId), *Environment.GameVersion.ToString()));
		Header.Add(TEXT("SDK"), Environment.SdkId.IsEmpty()
			? FString(TEXT("-"))
			: FString::Printf(TEXT("%s %s"), *Environment.SdkId, *Environment.SdkVersion.ToString()));
		Header.Add(TEXT("Providers"), JoinNames(Subsystem->GetProviderIds()));
		Header.Add(TEXT("Mods"), TallyMods(*Registry).ToString());
		Header.Flush(&Ar);

		const FModResolveResult Result = Subsystem->GetLastResolveResult();

		EmitBlank(&Ar);
		Emit(&Ar, FString::Printf(TEXT("  Last resolve: %s, %d mod(s) ordered, %d rejection(s), %d diagnostic(s)."),
			Result.bSuccess ? TEXT("succeeded") : TEXT("did not fully succeed"),
			Result.LoadOrder.Num(), Result.Rejections.Num(), Result.Diagnostics.Num()));

		if (Result.LoadOrder.Num() > 0)
		{
			Emit(&Ar, FString::Printf(TEXT("  Load order: %s"), *JoinModIds(Result.LoadOrder, TEXT(" -> "))));
		}

		const TArray<FModInfo> Mods = Registry->GetAllMods();
		if (Mods.Num() == 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, TEXT("  No mods are registered."));
		}
		else
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Mods (%d):"), Mods.Num()));

			FTableWriter Table(TEXT("    "), {
				TEXT("ORDER"), TEXT("ID"), TEXT("VERSION"), TEXT("STATE"), TEXT("ON"), TEXT("FAILURE"),
				TEXT("PROVIDER"), TEXT("PERMS"), TEXT("DIAGS"), TEXT("ROOT") });

			for (const FModInfo& Info : Mods)
			{
				Table.AddRow({
					DescribeLoadOrder(Info.LoadOrder),
					Info.GetId().ToString(),
					Info.Manifest.Version.ToString(),
					ModFrameworkEnums::ToString(Info.State),
					YesNo(Info.bEnabled),
					Info.FailureReason == EModLoadFailureReason::None
						? FString(TEXT("-"))
						: ModFrameworkEnums::ToString(Info.FailureReason),
					OrDash(Info.ProviderId.ToString()),
					FString::FromInt(Info.GrantedPermissions.Num()),
					FString::FromInt(Info.Diagnostics.Num()),
					OrDash(Info.RootPath) });
			}

			Table.Flush(&Ar);
		}

		if (Result.Rejections.Num() > 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Rejections (%d):"), Result.Rejections.Num()));
			for (const FModRejection& Rejection : Result.Rejections)
			{
				EmitWarning(&Ar, FString::Printf(TEXT("    %s [%s] %s"),
					*Rejection.ModId.ToString(),
					*ModFrameworkEnums::ToString(Rejection.Reason),
					*OrDash(Rejection.Message)));

				if (Rejection.RelatedMods.Num() > 0)
				{
					EmitWarning(&Ar, FString::Printf(TEXT("      Related: %s"), *JoinModIds(Rejection.RelatedMods)));
				}
			}
		}

		const TArray<FModDiagnostic> Diagnostics = Subsystem->GetDiagnostics();
		if (Diagnostics.Num() > 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  Diagnostics (%d):"), Diagnostics.Num()));
			for (const FModDiagnostic& Diagnostic : Diagnostics)
			{
				EmitLine(&Ar,
					Diagnostic.IsError() ? ELogVerbosity::Warning : ELogVerbosity::Display,
					FString::Printf(TEXT("    %s"), *Diagnostic.ToString()));
			}
		}
	}

	// --- Mod.DumpAPIs ----------------------------------------------------------------------------

	void CmdDumpAPIs(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.DumpAPIs");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		UModAPIRegistry* APIRegistry = Subsystem->GetAPIRegistry();
		if (APIRegistry == nullptr)
		{
			EmitError(&Ar, TEXT("Mod.DumpAPIs: the API registry does not exist - the subsystem failed to initialise."));
			return;
		}

		const TArray<FModAPIDescriptor> APIs = APIRegistry->GetAllAPIs();
		if (APIs.Num() == 0)
		{
			Emit(&Ar, TEXT("Mod.DumpAPIs: no APIs are registered. The game registers these with UModSubsystem::RegisterGameAPI."));
			return;
		}

		Emit(&Ar, FString::Printf(TEXT("Mod.DumpAPIs: %d API(s)."), APIs.Num()));

		FTableWriter Table(TEXT("  "), {
			TEXT("API ID"), TEXT("VERSION"), TEXT("SERVER"), TEXT("PROVIDER"), TEXT("PERMISSIONS"), TEXT("CLASS") });

		for (const FModAPIDescriptor& Descriptor : APIs)
		{
			Table.AddRow({
				OrDash(Descriptor.ApiId.ToString()),
				Descriptor.Version.ToString(),
				YesNo(Descriptor.bServerAuthoritative),
				Descriptor.ProviderModId.IsValid() ? Descriptor.ProviderModId.ToString() : FString(TEXT("game")),
				JoinNames(Descriptor.RequiredPermissions),
				OrDash(Descriptor.ClassPath) });
		}

		Table.Flush(&Ar);
	}

	// --- Mod.DumpExtensions ----------------------------------------------------------------------

	void CmdDumpExtensions(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.DumpExtensions");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		UModExtensionRegistry* ExtensionRegistry = Subsystem->GetExtensionRegistry();
		if (ExtensionRegistry == nullptr)
		{
			EmitError(&Ar, TEXT("Mod.DumpExtensions: the extension registry does not exist - the subsystem failed to initialise."));
			return;
		}

		const TArray<FModExtensionPointDescriptor> Points = ExtensionRegistry->GetExtensionPoints();
		if (Points.Num() == 0)
		{
			Emit(&Ar, TEXT("Mod.DumpExtensions: no extension points are open. The game opens these with UModSubsystem::RegisterExtensionPoint."));
			return;
		}

		Emit(&Ar, FString::Printf(TEXT("Mod.DumpExtensions: %d extension point(s)."), Points.Num()));

		int32 TotalExtensions = 0;
		for (const FModExtensionPointDescriptor& Point : Points)
		{
			EmitBlank(&Ar);
			Emit(&Ar, FString::Printf(TEXT("  %s (v%s)"),
				*OrDash(Point.ExtensionPointId.ToString()), *Point.Version.ToString()));

			const UClass* BaseClass = Point.RequiredBaseClass.Get();

			FFieldWriter Fields(TEXT("    "));
			Fields.Add(TEXT("Display name"), OrDash(Point.DisplayName.ToString()));
			Fields.Add(TEXT("Description"), OrDash(Point.Description.ToString()));
			Fields.Add(TEXT("Base class"), BaseClass != nullptr ? BaseClass->GetPathName() : FString(TEXT("any UModExtension")));
			Fields.Add(TEXT("Conflict policy"), ModFrameworkEnums::ToString(Point.DefaultConflictPolicy));
			Fields.Add(TEXT("Multiple per mod"), YesNo(Point.bAllowMultiplePerMod));
			Fields.Add(TEXT("Server authoritative"), YesNo(Point.bServerAuthoritative));
			Fields.Add(TEXT("Permissions"), JoinNames(Point.RequiredPermissions));
			Fields.Flush(&Ar);

			TArray<UModExtension*> Extensions = ExtensionRegistry->GetAllExtensions(Point.ExtensionPointId);

			// Garbage collection can leave a null behind between a mod unloading and the registry
			// noticing, and nothing below is worth crashing a debug command over.
			Extensions.RemoveAll([](const UModExtension* Extension) { return Extension == nullptr; });

			TotalExtensions += Extensions.Num();

			if (Extensions.Num() == 0)
			{
				Emit(&Ar, TEXT("    (no extensions registered)"));
				continue;
			}

			// The registry keeps every point's list in its own total order, so it is printed as found.
			FTableWriter Table(TEXT("    "), {
				TEXT("EXTENSION ID"), TEXT("MOD"), TEXT("PRIORITY"), TEXT("ACTIVE"), TEXT("CLAIMS"), TEXT("CLASS") });

			for (const UModExtension* Extension : Extensions)
			{
				Table.AddRow({
					OrDash(Extension->GetResolvedExtensionId().ToString()),
					Extension->OwningModId.IsValid() ? Extension->OwningModId.ToString() : FString(TEXT("game")),
					FString::FromInt(Extension->Priority),
					YesNo(Extension->IsExtensionActive()),
					JoinNames(Extension->ClaimedResourceIds),
					Extension->GetClass() != nullptr ? Extension->GetClass()->GetName() : FString(TEXT("-")) });
			}

			Table.Flush(&Ar);
		}

		EmitBlank(&Ar);
		Emit(&Ar, FString::Printf(TEXT("  %d extension(s) across %d point(s)."), TotalExtensions, Points.Num()));
	}

	// --- Mod.DumpEvents --------------------------------------------------------------------------

	void CmdDumpEvents(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.DumpEvents");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		UModEventBus* EventBus = Subsystem->GetEventBus();
		if (EventBus == nullptr)
		{
			EmitError(&Ar, TEXT("Mod.DumpEvents: the event bus does not exist - the subsystem failed to initialise."));
			return;
		}

		const TArray<FModEventDescriptor> Events = EventBus->GetEventTypes();
		if (Events.Num() == 0)
		{
			Emit(&Ar, TEXT("Mod.DumpEvents: no event types are declared."));
			return;
		}

		Emit(&Ar, FString::Printf(TEXT("Mod.DumpEvents: %d declared event type(s)%s."),
			Events.Num(), EventBus->IsBroadcasting() ? TEXT(" (a broadcast is in flight)") : TEXT("")));

		FTableWriter Table(TEXT("  "), {
			TEXT("EVENT ID"), TEXT("SOURCE"), TEXT("PAYLOAD"), TEXT("SERVER"), TEXT("SUBS"), TEXT("UNDECLARED"), TEXT("PERMISSIONS"), TEXT("DESCRIPTION") });

		int32 TotalSubscribers = 0;
		for (const FModEventDescriptor& Descriptor : Events)
		{
			const int32 Subscribers = EventBus->GetSubscriberCount(Descriptor.EventId);
			TotalSubscribers += Subscribers;

			const UScriptStruct* PayloadType = Descriptor.PayloadType;

			Table.AddRow({
				OrDash(Descriptor.EventId.ToString()),
				UModEventBus::IsFrameworkEventId(Descriptor.EventId) ? TEXT("framework") : TEXT("game"),
				PayloadType != nullptr ? PayloadType->GetName() : FString(TEXT("none")),
				YesNo(Descriptor.bServerAuthoritative),
				FString::FromInt(Subscribers),
				FString::FromInt(EventBus->GetUnregisteredBroadcastCount(Descriptor.EventId)),
				JoinNames(Descriptor.RequiredPermissions),
				OrDash(Descriptor.Description) });
		}

		Table.Flush(&Ar);
		Emit(&Ar, FString::Printf(TEXT("  %d live subscription(s) in total."), TotalSubscribers));
	}

	// --- Mod.DumpPermissions ---------------------------------------------------------------------

	void CmdDumpPermissions(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.DumpPermissions");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		UModPermissionRegistry* PermissionRegistry = Subsystem->GetPermissionRegistry();
		if (PermissionRegistry == nullptr)
		{
			EmitError(&Ar, TEXT("Mod.DumpPermissions: the permission registry does not exist - the subsystem failed to initialise."));
			return;
		}

		const TArray<FModPermissionDescriptor> Permissions = PermissionRegistry->GetAllPermissions();

		Emit(&Ar, FString::Printf(TEXT("Mod.DumpPermissions: %d registered permission(s)."), Permissions.Num()));

		if (Permissions.Num() > 0)
		{
			FTableWriter Table(TEXT("  "), { TEXT("PERMISSION"), TEXT("DANGEROUS"), TEXT("NAME"), TEXT("DESCRIPTION") });
			for (const FModPermissionDescriptor& Descriptor : Permissions)
			{
				Table.AddRow({
					OrDash(Descriptor.PermissionId.ToString()),
					YesNo(Descriptor.bDangerous),
					OrDash(Descriptor.DisplayName.ToString()),
					OrDash(Descriptor.Description.ToString()) });
			}
			Table.Flush(&Ar);
		}

		UModRegistry* Registry = Subsystem->GetRegistry();
		if (Registry == nullptr)
		{
			return;
		}

		const TArray<FModInfo> Mods = Registry->GetAllMods();
		if (Mods.Num() == 0)
		{
			EmitBlank(&Ar);
			Emit(&Ar, TEXT("  No mods are registered, so nothing has been decided yet."));
			return;
		}

		EmitBlank(&Ar);
		Emit(&Ar, FString::Printf(TEXT("  Per-mod decisions (%d mod(s)):"), Mods.Num()));

		FTableWriter Table(TEXT("    "), { TEXT("MOD"), TEXT("REQUESTED"), TEXT("GRANTED"), TEXT("PENDING"), TEXT("DENIED") });
		for (const FModInfo& Info : Mods)
		{
			const FModId& ModId = Info.GetId();
			Table.AddRow({
				ModId.ToString(),
				JoinNames(Info.Manifest.RequestedPermissions),
				JoinNames(PermissionRegistry->GetGrantedPermissions(ModId)),
				JoinNames(PermissionRegistry->GetPendingPermissions(ModId)),
				JoinNames(PermissionRegistry->GetDeniedPermissions(ModId)) });
		}
		Table.Flush(&Ar);
	}

	// --- Mod.Conflicts ---------------------------------------------------------------------------

	void CmdConflicts(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Conflicts");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		const TArray<FModConflict> Conflicts = Subsystem->GetConflicts();
		if (Conflicts.Num() == 0)
		{
			Emit(&Ar, TEXT("Mod.Conflicts: no conflicts were found during the last refresh."));
			return;
		}

		int32 Blocking = 0;
		for (const FModConflict& Conflict : Conflicts)
		{
			Blocking += Conflict.bBlocking ? 1 : 0;
		}

		EmitLine(&Ar,
			Blocking > 0 ? ELogVerbosity::Warning : ELogVerbosity::Display,
			FString::Printf(TEXT("Mod.Conflicts: %d conflict(s), %d blocking."), Conflicts.Num(), Blocking));

		FTableWriter Table(TEXT("  "), {
			TEXT("EXTENSION POINT"), TEXT("RESOURCE"), TEXT("POLICY"), TEXT("WINNER"), TEXT("BLOCKING"), TEXT("CONTENDERS") });

		for (const FModConflict& Conflict : Conflicts)
		{
			Table.AddRow({
				OrDash(Conflict.ExtensionPointId.ToString()),
				OrDash(Conflict.ResourceId.ToString()),
				ModFrameworkEnums::ToString(Conflict.AppliedPolicy),
				Conflict.Winner.IsValid() ? Conflict.Winner.ToString() : FString(TEXT("none")),
				YesNo(Conflict.bBlocking),
				JoinModIds(Conflict.Contenders) });
		}
		Table.Flush(&Ar);

		EmitBlank(&Ar);
		for (const FModConflict& Conflict : Conflicts)
		{
			EmitLine(&Ar,
				Conflict.bBlocking ? ELogVerbosity::Warning : ELogVerbosity::Display,
				FString::Printf(TEXT("  %s:%s - %s"),
					*Conflict.ExtensionPointId.ToString(),
					*Conflict.ResourceId.ToString(),
					*OrDash(Conflict.Explanation)));
		}
	}

	// --- Mod.SessionManifest ---------------------------------------------------------------------

	void CmdSessionManifest(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.SessionManifest");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		const FModSessionManifest Manifest = Subsystem->BuildSessionManifest();
		const FString Encoded = Manifest.ToBase64();

		Emit(&Ar, TEXT("Mod.SessionManifest:"));

		FFieldWriter Fields(TEXT("  "));
		Fields.Add(TEXT("Format version"), FString::FromInt(Manifest.FormatVersion));
		Fields.Add(TEXT("Game"), FString::Printf(TEXT("%s %s"),
			*OrDash(Manifest.GameId), *Manifest.GameVersion.ToString()));
		Fields.Add(TEXT("Framework"), Manifest.FrameworkVersion.ToString());
		Fields.Add(TEXT("SDK"), Manifest.SdkId.IsEmpty()
			? FString(TEXT("-"))
			: FString::Printf(TEXT("%s %s"), *Manifest.SdkId, *Manifest.SdkVersion.ToString()));
		Fields.Add(TEXT("Entries"), FString::FromInt(Manifest.Entries.Num()));
		Fields.Add(TEXT("Digest"), OrDash(Manifest.ComputeDigest()));
		Fields.Add(TEXT("Encoded length"), FString::Printf(TEXT("%d character(s)"), Encoded.Len()));
		Fields.Flush(&Ar);

		if (Manifest.Entries.Num() > 0)
		{
			EmitBlank(&Ar);
			FTableWriter Table(TEXT("  "), {
				TEXT("MOD"), TEXT("VERSION"), TEXT("SCOPE"), TEXT("REQUIRED"), TEXT("HASH"), TEXT("NAME") });

			for (const FModNetworkEntry& Entry : Manifest.Entries)
			{
				Table.AddRow({
					OrDash(Entry.ModId.ToString()),
					Entry.Version.ToString(),
					ModFrameworkEnums::ToString(Entry.Scope),
					YesNo(Entry.bRequired),
					OrDash(Entry.ContentHash),
					OrDash(Entry.DisplayName) });
			}
			Table.Flush(&Ar);
		}

		EmitBlank(&Ar);
		Emit(&Ar, TEXT("  Base64 (paste into a travel URL as ?ModManifest=<value>):"));
		Emit(&Ar, FString::Printf(TEXT("  %s"), *OrDash(Encoded)));
	}

	// --- Mod.Enable / Mod.Disable ----------------------------------------------------------------

	void SetEnabled(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar, bool bEnable)
	{
		const TCHAR* CommandName = bEnable ? TEXT("Mod.Enable") : TEXT("Mod.Disable");
		const TCHAR* Usage = bEnable ? TEXT("Mod.Enable <ModId>") : TEXT("Mod.Disable <ModId>");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		FModId ModId;
		if (!ResolveModIdArgument(Args, *Registry, &Ar, CommandName, Usage, ModId))
		{
			return;
		}

		WarnAboutExtraArgs(Args, 1, &Ar, CommandName);

		const FModInfo* Before = Registry->FindMod(ModId);
		const bool bWasEnabled = Before != nullptr ? Before->bEnabled : false;
		const EModState StateBefore = Before != nullptr ? Before->State : EModState::Unknown;

		if (bWasEnabled == bEnable)
		{
			Emit(&Ar, FString::Printf(TEXT("%s: mod '%s' is already %s."),
				CommandName, *ModId.ToString(), bEnable ? TEXT("enabled") : TEXT("disabled")));
			return;
		}

		if (!Subsystem->SetModEnabled(ModId, bEnable))
		{
			EmitError(&Ar, FString::Printf(TEXT("%s: could not %s mod '%s'."),
				CommandName, bEnable ? TEXT("enable") : TEXT("disable"), *ModId.ToString()));
			return;
		}

		EmitTransition(&Ar, CommandName, ModId, StateBefore, GetModState(*Registry, ModId));

		if (bEnable)
		{
			Emit(&Ar, TEXT("  Enabling only clears the flag. Run Mod.Refresh (or Mod.Load <ModId>) to bring the mod up."));
		}

		// The change is session-only by design; say so, because the obvious expectation is that it
		// sticks across a restart.
		Emit(&Ar, TEXT("  This is a session-only change; it is not written to DefaultGame.ini."));
	}

	void CmdEnable(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		SetEnabled(Args, World, Ar, true);
	}

	void CmdDisable(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		SetEnabled(Args, World, Ar, false);
	}

	// --- Mod.Icons -------------------------------------------------------------------------------

	/** "1024x1024" for a cached texture, "no" otherwise. */
	FString DescribeCachedIcon(const UTexture2D* Icon)
	{
		if (Icon == nullptr)
		{
			return FString(TEXT("no"));
		}

		return FString::Printf(TEXT("%dx%d"), Icon->GetSizeX(), Icon->GetSizeY());
	}

	/** Where the bytes would come from, judged from the record alone - no file is opened here. */
	FString DescribeIconSource(const FModInfo& Info)
	{
		if (Info.Manifest.IconPath.IsEmpty())
		{
			return FString(TEXT("-"));
		}

		return Info.ResolvedIconPath.IsEmpty() ? FString(TEXT("package")) : FString(TEXT("loose file"));
	}

	void CmdIcons(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Icons");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		UModIconCache* IconCache = Subsystem->GetIconCache();
		if (IconCache == nullptr)
		{
			EmitError(&Ar, TEXT("Mod.Icons: the icon cache does not exist - the subsystem failed to initialise."));
			return;
		}

		// Mod.Icons load <ModId> forces one icon through the blocking path, which is the only way to
		// see the diagnostic behind an icon that silently falls back to the default.
		const FString FirstArg = Args.Num() > 0 ? Args[0].TrimStartAndEnd().ToLower() : FString();
		if (FirstArg == TEXT("load"))
		{
			TArray<FString> Remainder;
			for (int32 Index = 1; Index < Args.Num(); ++Index)
			{
				Remainder.Add(Args[Index]);
			}

			FModId ModId;
			if (!ResolveModIdArgument(Remainder, *Registry, &Ar, CommandName, TEXT("Mod.Icons load <ModId>"), ModId))
			{
				return;
			}

			WarnAboutExtraArgs(Args, 2, &Ar, CommandName);

			FModDiagnostic Error;
			const UTexture2D* Icon = IconCache->LoadIconSynchronous(ModId, Error);

			if (!Error.Code.IsNone())
			{
				EmitWarning(&Ar, FString::Printf(TEXT("Mod.Icons: '%s' - %s"), *ModId.ToString(), *Error.ToString()));
			}

			Emit(&Ar, FString::Printf(TEXT("Mod.Icons: '%s' resolved to %s%s."),
				*ModId.ToString(),
				*DescribeCachedIcon(Icon),
				Icon != nullptr && Icon == IconCache->GetDefaultIcon() ? TEXT(" (the project default icon)") : TEXT("")));
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		const TArray<FModInfo> Mods = Registry->GetAllMods();
		if (Mods.Num() == 0)
		{
			Emit(&Ar, TEXT("Mod.Icons: no mods are registered."));
			return;
		}

		Emit(&Ar, FString::Printf(TEXT("Mod.Icons: %d mod(s)."), Mods.Num()));

		FTableWriter Table(TEXT("  "), {
			TEXT("MOD"), TEXT("DECLARED"), TEXT("SOURCE"), TEXT("LOADABLE"), TEXT("CACHED"), TEXT("PATH") });

		int32 Declared = 0;
		int32 Cached = 0;
		for (const FModInfo& Info : Mods)
		{
			const FModId& ModId = Info.GetId();
			const UTexture2D* Icon = IconCache->FindIcon(ModId);

			Declared += Info.Manifest.IconPath.IsEmpty() ? 0 : 1;
			Cached += Icon != nullptr ? 1 : 0;

			Table.AddRow({
				ModId.ToString(),
				OrDash(Info.Manifest.IconPath),
				DescribeIconSource(Info),
				YesNo(IconCache->HasIcon(ModId)),
				DescribeCachedIcon(Icon),
				OrDash(Info.ResolvedIconPath) });
		}

		Table.Flush(&Ar);

		const UTexture2D* DefaultIcon = IconCache->GetDefaultIcon();
		Emit(&Ar, FString::Printf(TEXT("  %d mod(s) declare an icon, %d cached. Project default icon: %s."),
			Declared, Cached, *DescribeCachedIcon(DefaultIcon)));
		Emit(&Ar, TEXT("  Use 'Mod.Icons load <ModId>' to force one icon through the blocking loader and see why it failed."));
	}

	// --- Mod.Scripts -----------------------------------------------------------------------------

	/** ".lua, .luau" for one runtime's claimed source extensions. */
	FString DescribeExtensions(const IModScriptRuntime& Runtime)
	{
		const TArray<FString> Extensions = Runtime.GetSourceExtensions();
		return Extensions.Num() > 0 ? FString::Join(Extensions, TEXT(", ")) : FString(TEXT("-"));
	}

	/**
	 * The runtime a mod asked for, flagged when nothing answers to that name.
	 *
	 * The flag matters more than it looks: a manifest naming an unregistered runtime is the one way a
	 * mod can fail with all of its code intact and nothing obviously wrong with it.
	 */
	FString DescribeRequestedRuntime(const FModInfo& Info, const UModScriptManager& ScriptManager)
	{
		const FName Requested = Info.Manifest.EntryPoint.ScriptRuntime;
		if (Requested.IsNone())
		{
			return FString(TEXT("-"));
		}

		FString Text = Requested.ToString();
		if (ScriptManager.FindRuntime(Requested) == nullptr)
		{
			Text += TEXT("  <-- not registered");
		}

		return Text;
	}

	/** True when the manifest declares any part of a scripting section. */
	bool DeclaresScripting(const FModInfo& Info)
	{
		return !Info.Manifest.EntryPoint.ScriptRuntime.IsNone() || Info.Manifest.EntryPoint.Scripts.Num() > 0;
	}

	void CmdScripts(const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
	{
		static const TCHAR* CommandName = TEXT("Mod.Scripts");

		UModSubsystem* Subsystem = ResolveSubsystem(World, &Ar, CommandName);
		if (Subsystem == nullptr)
		{
			return;
		}

		UModRegistry* Registry = ResolveRegistry(Subsystem, &Ar, CommandName);
		if (Registry == nullptr)
		{
			return;
		}

		UModScriptManager* ScriptManager = Subsystem->GetScriptManager();
		if (ScriptManager == nullptr)
		{
			EmitError(&Ar, TEXT("Mod.Scripts: the script manager does not exist - the subsystem failed to initialise."));
			return;
		}

		WarnAboutExtraArgs(Args, 0, &Ar, CommandName);

		// --- The registered runtimes ---------------------------------------------------------
		const TArray<FName> RuntimeIds = ScriptManager->GetRuntimeIds();

		Emit(&Ar, FString::Printf(TEXT("Mod.Scripts: %d registered runtime(s)."), RuntimeIds.Num()));

		if (RuntimeIds.Num() == 0)
		{
			EmitWarning(&Ar, TEXT("  No script runtime is registered, so any mod naming one will fail to load."));
			Emit(&Ar, TEXT("  Enable a scripting module (ModFrameworkLua ships with the framework), or register your own"));
			Emit(&Ar, TEXT("  IModScriptRuntime through UModScriptManager::RegisterRuntimeFactory from your module's StartupModule."));
		}
		else
		{
			FTableWriter Runtimes(TEXT("  "), {
				TEXT("RUNTIME"), TEXT("VERSION"), TEXT("HOT RELOAD"), TEXT("LIMITS"), TEXT("CONTEXTS"), TEXT("EXTENSIONS") });

			for (const FName& RuntimeId : RuntimeIds)
			{
				const IModScriptRuntime* Runtime = ScriptManager->FindRuntime(RuntimeId);
				if (Runtime == nullptr)
				{
					continue;
				}

				int32 LiveContexts = 0;
				for (const FModId& ScriptedMod : ScriptManager->GetScriptedMods())
				{
					LiveContexts += ScriptManager->GetModRuntimeId(ScriptedMod) == RuntimeId ? 1 : 0;
				}

				Runtimes.AddRow({
					RuntimeId.ToString(),
					Runtime->GetRuntimeVersion().ToString(),
					YesNo(Runtime->SupportsHotReload()),
					YesNo(Runtime->SupportsResourceLimits()),
					FString::FromInt(LiveContexts),
					DescribeExtensions(*Runtime) });
			}

			Runtimes.Flush(&Ar);
		}

		// --- The mods that ship scripts ------------------------------------------------------
		const TArray<FModInfo> Mods = Registry->GetAllMods();

		FTableWriter Table(TEXT("  "), {
			TEXT("MOD"), TEXT("RUNTIME"), TEXT("SCRIPTS"), TEXT("LOADED"), TEXT("STATE"), TEXT("FILES") });

		int32 LoadedCount = 0;
		for (const FModInfo& Info : Mods)
		{
			if (!DeclaresScripting(Info))
			{
				continue;
			}

			const FModId& ModId = Info.GetId();
			const TArray<FString>& Scripts = Info.Manifest.EntryPoint.Scripts;
			const bool bLoaded = ScriptManager->AreScriptsLoaded(ModId);
			LoadedCount += bLoaded ? 1 : 0;

			Table.AddRow({
				ModId.ToString(),
				DescribeRequestedRuntime(Info, *ScriptManager),
				FString::FromInt(Scripts.Num()),
				YesNo(bLoaded),
				ModFrameworkEnums::ToString(Info.State),
				Scripts.Num() > 0 ? FString::Join(Scripts, TEXT(", ")) : FString(TEXT("-")) });
		}

		EmitBlank(&Ar);

		if (Table.NumRows() == 0)
		{
			Emit(&Ar, TEXT("  No registered mod declares any scripting."));
			return;
		}

		Emit(&Ar, FString::Printf(TEXT("  %d mod(s) declare scripts, %d with scripts loaded."),
			Table.NumRows(), LoadedCount));
		Table.Flush(&Ar);

		// Scripts run between the context being created and the entry point being instantiated, which
		// is a guarantee mod authors are told about, so the reminder belongs where they will see it.
		Emit(&Ar, TEXT("  Scripts run after the mod's context exists and before its entry point class is instantiated."));
	}

	// --- Registration ----------------------------------------------------------------------------

	/** Signature every command body above shares. */
	using FModCommandHandler = void (*)(const TArray<FString>&, UWorld*, FOutputDevice&);

	/**
	 * The live command objects.
	 *
	 * Held by pointer so that Unregister can destroy them: FAutoConsoleObject unregisters itself in
	 * its destructor, which is the only way to take a console command back off IConsoleManager.
	 */
	TArray<TUniquePtr<FAutoConsoleCommandWithWorldArgsAndOutputDevice>> Commands;

	/**
	 * Builds one console command.
	 *
	 * The settings gate is applied here, once, inside the execution lambda - never at registration
	 * time, because at PostConfigInit there is no UModFrameworkSettings CDO to read.
	 */
	void AddCommand(const TCHAR* Name, const TCHAR* Help, FModCommandHandler Handler)
	{
		Commands.Emplace(MakeUnique<FAutoConsoleCommandWithWorldArgsAndOutputDevice>(
			Name,
			Help,
			FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateLambda(
				[Handler](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
				{
					if (!AreCommandsEnabled(&Ar))
					{
						return;
					}

					Handler(Args, World, Ar);
				})));
	}
}

void FModConsoleCommands::Register()
{
	using namespace ModConsoleCommandsPrivate;

	// Idempotent: a second Register would otherwise leave the first set of objects registered and
	// unreachable, and IConsoleManager would warn about every duplicate name.
	if (Commands.Num() > 0)
	{
		return;
	}

	// NOTHING HERE MAY TOUCH A CDO. The module loads at PostConfigInit, before the UObject system
	// exists, so UModFrameworkSettings cannot be read until a command actually runs.
	Commands.Reserve(23);

	AddCommand(TEXT("Mod.List"),
		TEXT("Lists every registered mod. Optional argument filters by state name, mod id or display name."),
		&CmdList);

	AddCommand(TEXT("Mod.Info"),
		TEXT("Mod.Info <ModId> - everything the framework knows about one mod."),
		&CmdInfo);

	AddCommand(TEXT("Mod.Load"),
		TEXT("Mod.Load <ModId> - mounts the mod if needed, then loads it. Does not activate it."),
		&CmdLoad);

	AddCommand(TEXT("Mod.Unload"),
		TEXT("Mod.Unload <ModId> - deactivates, unloads and unmounts one mod."),
		&CmdUnload);

	AddCommand(TEXT("Mod.Activate"),
		TEXT("Mod.Activate <ModId> - loads the mod if needed, then activates it."),
		&CmdActivate);

	AddCommand(TEXT("Mod.Deactivate"),
		TEXT("Mod.Deactivate <ModId> - switches one mod's extensions off. The mod stays loaded."),
		&CmdDeactivate);

	AddCommand(TEXT("Mod.Reload"),
		TEXT("Mod.Reload <ModId> - unloads one mod and brings it straight back."),
		&CmdReload);

	AddCommand(TEXT("Mod.ReloadAll"),
		TEXT("Unloads every mod in reverse load order and brings them all back."),
		&CmdReloadAll);

	AddCommand(TEXT("Mod.Refresh"),
		TEXT("Runs the whole discover/resolve/mount/load pipeline. Pass 'noactivate' to stop after loading."),
		&CmdRefresh);

	AddCommand(TEXT("Mod.Validate"),
		TEXT("Mod.Validate [ModId] - re-runs manifest validation, re-reading mod.json from disk where possible."),
		&CmdValidate);

	AddCommand(TEXT("Mod.Dependencies"),
		TEXT("Mod.Dependencies <ModId> - what one mod needs, what needs it, and how the resolver decided."),
		&CmdDependencies);

	AddCommand(TEXT("Mod.Mounts"),
		TEXT("Lists every live mod content mount."),
		&CmdMounts);

	AddCommand(TEXT("Mod.DumpRegistry"),
		TEXT("Dumps providers, the environment, the resolved load order and the full mod table."),
		&CmdDumpRegistry);

	AddCommand(TEXT("Mod.DumpAPIs"),
		TEXT("Lists every API the game has published to mods."),
		&CmdDumpAPIs);

	AddCommand(TEXT("Mod.DumpExtensions"),
		TEXT("Lists every extension point and the extensions registered against it."),
		&CmdDumpExtensions);

	AddCommand(TEXT("Mod.DumpEvents"),
		TEXT("Lists every declared event type with its payload, gates and subscriber count."),
		&CmdDumpEvents);

	AddCommand(TEXT("Mod.DumpPermissions"),
		TEXT("Lists the permission catalogue and what each mod was granted, refused or left pending."),
		&CmdDumpPermissions);

	AddCommand(TEXT("Mod.Conflicts"),
		TEXT("Lists contested resources from the last refresh and how each conflict was reconciled."),
		&CmdConflicts);

	AddCommand(TEXT("Mod.SessionManifest"),
		TEXT("Prints what this install advertises to the other side of a network session."),
		&CmdSessionManifest);

	AddCommand(TEXT("Mod.Enable"),
		TEXT("Mod.Enable <ModId> - switches one mod on for this session."),
		&CmdEnable);

	AddCommand(TEXT("Mod.Disable"),
		TEXT("Mod.Disable <ModId> - switches one mod off for this session, tearing it down if it is running."),
		&CmdDisable);

	AddCommand(TEXT("Mod.Icons"),
		TEXT("Lists which mods ship an icon and which icons are cached. 'Mod.Icons load <ModId>' forces one load."),
		&CmdIcons);

	AddCommand(TEXT("Mod.Scripts"),
		TEXT("Lists the registered script runtimes and, for every mod that ships scripts, its runtime, script count and load state."),
		&CmdScripts);

	UE_LOG(LogModFramework, Verbose, TEXT("Registered %d Mod.* console command(s)."), Commands.Num());
}

void FModConsoleCommands::Unregister()
{
	using namespace ModConsoleCommandsPrivate;

	// Destroying each FAutoConsoleObject is what removes it from the console manager.
	Commands.Empty();
}

#else // !UE_BUILD_SHIPPING || ALLOW_CONSOLE

// A build with no reachable console still links against these, because ModFrameworkModule guards the
// calls with the same condition but other translation units may not.

void FModConsoleCommands::Register()
{
}

void FModConsoleCommands::Unregister()
{
}

#endif // !UE_BUILD_SHIPPING || ALLOW_CONSOLE
