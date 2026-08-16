// Copyright (c) 2026. Licensed for use in your own projects.

#include "Dependencies/ModDependencyResolver.h"

#include "Algo/Reverse.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "Core/ModFrameworkVersion.h"
#include "CoreTypes.h"
#include "Dependencies/ModDependencyTypes.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CString.h"
#include "Misc/CoreMiscDefines.h"
#include "Settings/ModFrameworkSettings.h"
#include "Templates/Tuple.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"

namespace
{
	/** Stable machine-readable diagnostic codes. Games and tests match on these, so they never change. */
	namespace ResolveCodes
	{
		const TCHAR* const InvalidManifest   = TEXT("Resolve.InvalidManifest");
		const TCHAR* const DuplicateId       = TEXT("Resolve.DuplicateId");
		const TCHAR* const Disabled          = TEXT("Resolve.Disabled");
		const TCHAR* const Environment       = TEXT("Resolve.Environment");
		const TCHAR* const MissingDependency = TEXT("Resolve.MissingDependency");
		const TCHAR* const VersionConflict   = TEXT("Resolve.VersionConflict");
		const TCHAR* const Cycle             = TEXT("Resolve.Cycle");
		const TCHAR* const CascadeReject     = TEXT("Resolve.CascadeReject");
		const TCHAR* const OptionalMissing   = TEXT("Resolve.OptionalMissing");
		const TCHAR* const OrderingIgnored   = TEXT("Resolve.OrderingIgnored");
	}

	//~ ---------------------------------------------------------------------------------------------
	//~ Message formatting. Every rejection message is a complete sentence naming the mod, what it
	//~ wanted and what is actually installed, because these strings end up in front of a player who
	//~ has no idea what a topological sort is.
	//~ ---------------------------------------------------------------------------------------------

	/** "Mod 'Better Combat' (com.example.bettercombat)", or "Mod 'com.example.x'" when there is no name. */
	FString DescribeMod(const FModManifest& Manifest)
	{
		if (Manifest.DisplayName.IsEmpty())
		{
			return FString::Printf(TEXT("Mod '%s'"), *Manifest.Id.ToString());
		}

		return FString::Printf(TEXT("Mod '%s' (%s)"), *Manifest.DisplayName, *Manifest.Id.ToString());
	}

	/** True when the range really narrows anything down: non-empty, parses, and is not "any". */
	bool RangeConstrains(const FModVersionRange& Range)
	{
		return !Range.Expression.IsEmpty() && Range.IsValid() && !Range.IsAny();
	}

	/** True when a non-empty range expression failed to parse - a manifest defect, never a rejection. */
	bool RangeIsMalformed(const FModVersionRange& Range)
	{
		return !Range.Expression.IsEmpty() && !Range.IsValid();
	}

	/** "'com.example.corelib' >=2.0.0", or "'com.example.ui' (any version)". */
	FString DescribeRequirement(const FModId& DependencyId, const FModVersionRange& Range)
	{
		if (!RangeConstrains(Range))
		{
			return FString::Printf(TEXT("'%s' (any version)"), *DependencyId.ToString());
		}

		return FString::Printf(TEXT("'%s' %s"), *DependencyId.ToString(), *Range.Expression);
	}

	/** The author's explanation of why a dependency exists, ready to append to a sentence. */
	FString DescribeDependencyReason(const FModDependency& Dependency)
	{
		if (Dependency.Reason.IsEmpty())
		{
			return FString();
		}

		return FString::Printf(TEXT(" The mod author notes: \"%s\""), *Dependency.Reason);
	}

	FString JoinIds(const TArray<FModId>& Ids, const TCHAR* Separator)
	{
		FString Result;
		for (int32 Index = 0; Index < Ids.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += Separator;
			}

			Result += Ids[Index].ToString();
		}

		return Result;
	}

	FModDiagnostic MakeDiagnostic(EModDiagnosticSeverity Severity, const TCHAR* Code, const FModId& ModId,
		FString Message, FString Context = FString())
	{
		FModDiagnostic Diagnostic(Severity, FName(Code), MoveTemp(Message), MoveTemp(Context));
		Diagnostic.ModId = ModId;
		return Diagnostic;
	}

	//~ ---------------------------------------------------------------------------------------------
	//~ Resolver working state.
	//~ ---------------------------------------------------------------------------------------------

	/**
	 * Mutable bookkeeping for one Resolve() call.
	 *
	 * Manifests holds only the winner of each duplicate id and never changes afterwards, so a raw
	 * pointer into the caller's Candidates array stays valid for the whole call. AllIds is that map's
	 * key set, sorted: every loop that can influence the output walks AllIds rather than the map, so
	 * hash order can never leak into the result.
	 */
	struct FResolveState
	{
		TMap<FModId, const FModManifest*> Manifests;
		TArray<FModId> AllIds;
		TSet<FModId> Rejected;

		/** Index into FModResolveResult::Rejections of the first rejection recorded for a mod. */
		TMap<FModId, int32> FirstRejection;
	};

	const FModManifest* FindManifest(const FResolveState& State, const FModId& Id)
	{
		const FModManifest* const* Found = State.Manifests.Find(Id);
		return Found != nullptr ? *Found : nullptr;
	}

	/** Ids that are still in the running, in sorted order. */
	void CollectAccepted(const FResolveState& State, TArray<FModId>& OutAccepted)
	{
		OutAccepted.Reset();
		OutAccepted.Reserve(State.AllIds.Num());
		for (const FModId& Id : State.AllIds)
		{
			if (!State.Rejected.Contains(Id))
			{
				OutAccepted.Add(Id);
			}
		}
	}

	/** Records a rejection and its diagnostic. Returns the index of the new FModRejection. */
	int32 AddRejectionRecord(FModResolveResult& Result, const FModId& ModId, EModLoadFailureReason Reason,
		const TCHAR* Code, const FString& Message, TArray<FModId> RelatedMods,
		EModDiagnosticSeverity Severity = EModDiagnosticSeverity::Error)
	{
		FModRejection Rejection;
		Rejection.ModId = ModId;
		Rejection.Reason = Reason;
		Rejection.Message = Message;
		Rejection.RelatedMods = MoveTemp(RelatedMods);

		Result.Diagnostics.Add(MakeDiagnostic(Severity, Code, ModId, Message));
		return Result.Rejections.Add(MoveTemp(Rejection));
	}

	/** Records a rejection and takes the mod out of the running. */
	void RejectMod(FModResolveResult& Result, FResolveState& State, const FModId& ModId,
		EModLoadFailureReason Reason, const TCHAR* Code, const FString& Message, TArray<FModId> RelatedMods,
		EModDiagnosticSeverity Severity = EModDiagnosticSeverity::Error)
	{
		const int32 Index = AddRejectionRecord(Result, ModId, Reason, Code, Message, MoveTemp(RelatedMods), Severity);

		State.Rejected.Add(ModId);
		if (!State.FirstRejection.Contains(ModId))
		{
			State.FirstRejection.Add(ModId, Index);
		}
	}

	//~ ---------------------------------------------------------------------------------------------
	//~ Environment checks.
	//~ ---------------------------------------------------------------------------------------------

	/**
	 * The single implementation behind FModDependencyResolver::CheckEnvironment.
	 *
	 * OutDiagnostics may be null; the public entry point has nowhere to put warnings. Everything that
	 * makes a check impossible - a missing id on either side, a version the host never configured, a
	 * range that does not parse - degrades to "assume compatible" plus a warning, never to a
	 * rejection. A player should not lose their mod list because the game shipped an empty
	 * GameVersion or because one author mistyped a caret.
	 */
	void CheckEnvironmentInternal(const FModManifest& Manifest, const FModEnvironment& Environment,
		TArray<FModRejection>& OutRejections, TArray<FModDiagnostic>* OutDiagnostics)
	{
		const FString Who = DescribeMod(Manifest);

		auto Reject = [&OutRejections, &Manifest](EModLoadFailureReason Reason, FString Message)
		{
			FModRejection Rejection;
			Rejection.ModId = Manifest.Id;
			Rejection.Reason = Reason;
			Rejection.Message = MoveTemp(Message);
			OutRejections.Add(MoveTemp(Rejection));
		};

		auto Warn = [OutDiagnostics, &Manifest](FString Message)
		{
			if (OutDiagnostics != nullptr)
			{
				OutDiagnostics->Add(MakeDiagnostic(EModDiagnosticSeverity::Warning, ResolveCodes::Environment,
					Manifest.Id, MoveTemp(Message)));
			}
		};

		// --- Game identity -------------------------------------------------------------------------
		if (Manifest.Game.GameId.IsEmpty())
		{
			Warn(FString::Printf(
				TEXT("%s does not declare a game id; its game requirement is treated as 'any game'."), *Who));
		}
		else if (Environment.GameId.IsEmpty())
		{
			Warn(FString::Printf(
				TEXT("%s is built for game '%s', but this build declares no game id in the mod framework settings; the game identity check is skipped."),
				*Who, *Manifest.Game.GameId));
		}
		else if (!Manifest.Game.GameId.Equals(Environment.GameId, ESearchCase::IgnoreCase))
		{
			Reject(EModLoadFailureReason::IncompatibleGame, FString::Printf(
				TEXT("%s is built for game '%s', but this game is '%s'."),
				*Who, *Manifest.Game.GameId, *Environment.GameId));
		}

		// --- Game version --------------------------------------------------------------------------
		const FModVersionRange& GameRange = Manifest.Game.VersionRange;
		if (RangeIsMalformed(GameRange))
		{
			Warn(FString::Printf(
				TEXT("%s declares the game version range '%s', which is not valid range syntax; it is treated as 'any version'."),
				*Who, *GameRange.Expression));
		}
		else if (RangeConstrains(GameRange))
		{
			if (Environment.GameVersion.IsZero())
			{
				Warn(FString::Printf(
					TEXT("%s requires game version %s, but this build declares no game version in the mod framework settings; the game version check is skipped."),
					*Who, *GameRange.Expression));
			}
			else if (!GameRange.Satisfies(Environment.GameVersion))
			{
				Reject(EModLoadFailureReason::IncompatibleGame, FString::Printf(
					TEXT("%s requires game version %s, but this game is version %s."),
					*Who, *GameRange.Expression, *Environment.GameVersion.ToString()));
			}
		}

		// --- Framework version ---------------------------------------------------------------------
		const FModVersionRange& FrameworkRange = Manifest.FrameworkVersionRange;
		if (RangeIsMalformed(FrameworkRange))
		{
			Warn(FString::Printf(
				TEXT("%s declares the framework version range '%s', which is not valid range syntax; it is treated as 'any version'."),
				*Who, *FrameworkRange.Expression));
		}
		else if (RangeConstrains(FrameworkRange))
		{
			if (Environment.FrameworkVersion.IsZero())
			{
				Warn(FString::Printf(
					TEXT("%s requires mod framework version %s, but the environment carries no framework version; the framework version check is skipped."),
					*Who, *FrameworkRange.Expression));
			}
			else if (!FrameworkRange.Satisfies(Environment.FrameworkVersion))
			{
				Reject(EModLoadFailureReason::IncompatibleFramework, FString::Printf(
					TEXT("%s requires mod framework version %s, but this build provides framework version %s."),
					*Who, *FrameworkRange.Expression, *Environment.FrameworkVersion.ToString()));
			}
		}

		// --- Modding SDK ---------------------------------------------------------------------------
		// An empty SdkId means "this mod does not care which SDK is present", which is a legitimate
		// thing to say; only a version requirement with no id to hang it on is a manifest defect.
		const FModSdkRequirement& Sdk = Manifest.Sdk;
		if (Sdk.SdkId.IsEmpty())
		{
			if (RangeConstrains(Sdk.VersionRange))
			{
				Warn(FString::Printf(
					TEXT("%s requires SDK version %s but names no SDK id; the SDK requirement is ignored."),
					*Who, *Sdk.VersionRange.Expression));
			}

			return;
		}

		if (Environment.SdkId.IsEmpty())
		{
			Reject(EModLoadFailureReason::IncompatibleSdk, FString::Printf(
				TEXT("%s requires the modding SDK '%s', but this game does not provide a modding SDK."),
				*Who, *Sdk.SdkId));
			return;
		}

		if (!Sdk.SdkId.Equals(Environment.SdkId, ESearchCase::IgnoreCase))
		{
			Reject(EModLoadFailureReason::IncompatibleSdk, FString::Printf(
				TEXT("%s requires the modding SDK '%s', but this game provides '%s'."),
				*Who, *Sdk.SdkId, *Environment.SdkId));
			return;
		}

		if (RangeIsMalformed(Sdk.VersionRange))
		{
			Warn(FString::Printf(
				TEXT("%s declares the SDK version range '%s', which is not valid range syntax; it is treated as 'any version'."),
				*Who, *Sdk.VersionRange.Expression));
		}
		else if (RangeConstrains(Sdk.VersionRange))
		{
			if (Environment.SdkVersion.IsZero())
			{
				Warn(FString::Printf(
					TEXT("%s requires SDK version %s, but this build declares no SDK version in the mod framework settings; the SDK version check is skipped."),
					*Who, *Sdk.VersionRange.Expression));
			}
			else if (!Sdk.VersionRange.Satisfies(Environment.SdkVersion))
			{
				Reject(EModLoadFailureReason::IncompatibleSdk, FString::Printf(
					TEXT("%s requires SDK version %s, but the installed SDK '%s' is version %s."),
					*Who, *Sdk.VersionRange.Expression, *Environment.SdkId, *Environment.SdkVersion.ToString()));
			}
		}
	}

	//~ ---------------------------------------------------------------------------------------------
	//~ Graph construction.
	//~ ---------------------------------------------------------------------------------------------

	/**
	 * The one and only edge-rule implementation, so cycle detection and the topological sort can never
	 * disagree about what the graph is.
	 *
	 * Nodes must be sorted; it is both the node set and the iteration order. Manifests may contain
	 * entries that are NOT in Nodes - that is how the diagnostics can tell "never installed" apart
	 * from "installed but rejected". Every node becomes a key even when it has no edges, so the map
	 * doubles as the node list, and every adjacency list comes out sorted and duplicate-free.
	 *
	 * OutDiagnostics may be null. Pass it only on the final pass: the intermediate passes rebuild the
	 * graph several times and would otherwise repeat every Info message.
	 */
	void BuildEdges(const TArray<FModId>& Nodes, const TMap<FModId, const FModManifest*>& Manifests,
		TMap<FModId, TArray<FModId>>& OutEdges, TArray<FModDiagnostic>* OutDiagnostics)
	{
		OutEdges.Reset();
		OutEdges.Reserve(Nodes.Num());

		TSet<FModId> NodeSet;
		NodeSet.Reserve(Nodes.Num());
		for (const FModId& Id : Nodes)
		{
			NodeSet.Add(Id);
			OutEdges.Add(Id);
		}

		auto AddEdge = [&OutEdges](const FModId& From, const FModId& To)
		{
			OutEdges.FindOrAdd(From).AddUnique(To);
		};

		for (const FModId& Id : Nodes)
		{
			const FModManifest* const* ManifestPtr = Manifests.Find(Id);
			if (ManifestPtr == nullptr || *ManifestPtr == nullptr)
			{
				continue;
			}

			const FModManifest& Manifest = **ManifestPtr;

			for (const FModDependency& Dependency : Manifest.Dependencies)
			{
				if (!Dependency.Id.IsValid())
				{
					continue;
				}

				if (!NodeSet.Contains(Dependency.Id))
				{
					// A missing REQUIRED dependency has already been reported as a rejection; only the
					// optional case is news here.
					if (OutDiagnostics != nullptr && Dependency.bOptional)
					{
						const TCHAR* Fate = Manifests.Contains(Dependency.Id)
							? TEXT("is installed but will not load")
							: TEXT("is not installed");

						OutDiagnostics->Add(MakeDiagnostic(EModDiagnosticSeverity::Info,
							ResolveCodes::OptionalMissing, Id,
							FString::Printf(TEXT("%s has an optional dependency on %s, which %s; it will load without it.%s"),
								*DescribeMod(Manifest), *DescribeRequirement(Dependency.Id, Dependency.VersionRange),
								Fate, *DescribeDependencyReason(Dependency)),
							Dependency.Id.ToString()));
					}

					continue;
				}

				// Present means ordered: the dependency loads first even when its version is not the one
				// the dependent hoped for, because loading it later could only ever be worse.
				AddEdge(Dependency.Id, Id);

				if (OutDiagnostics != nullptr && Dependency.bOptional && RangeConstrains(Dependency.VersionRange))
				{
					const FModManifest* const* DependencyManifest = Manifests.Find(Dependency.Id);
					if (DependencyManifest != nullptr && *DependencyManifest != nullptr
						&& !Dependency.VersionRange.Satisfies((*DependencyManifest)->Version))
					{
						OutDiagnostics->Add(MakeDiagnostic(EModDiagnosticSeverity::Info,
							ResolveCodes::OptionalMissing, Id,
							FString::Printf(TEXT("%s has an optional dependency on %s, but version %s is installed; it will load without it.%s"),
								*DescribeMod(Manifest), *DescribeRequirement(Dependency.Id, Dependency.VersionRange),
								*(*DependencyManifest)->Version.ToString(), *DescribeDependencyReason(Dependency)),
							Dependency.Id.ToString()));
					}
				}
			}

			auto ProcessOrdering = [&](const TArray<FModId>& Targets, const TCHAR* Keyword, bool bTargetLoadsFirst)
			{
				for (const FModId& Target : Targets)
				{
					if (!Target.IsValid())
					{
						continue;
					}

					if (!NodeSet.Contains(Target))
					{
						if (OutDiagnostics != nullptr)
						{
							const TCHAR* Fate = Manifests.Contains(Target)
								? TEXT("is installed but is not in the resolved load order")
								: TEXT("is not installed");

							OutDiagnostics->Add(MakeDiagnostic(EModDiagnosticSeverity::Info,
								ResolveCodes::OrderingIgnored, Id,
								FString::Printf(TEXT("%s declares %s '%s', which %s; the ordering constraint is ignored."),
									*DescribeMod(Manifest), Keyword, *Target.ToString(), Fate),
								Target.ToString()));
						}

						continue;
					}

					if (bTargetLoadsFirst)
					{
						AddEdge(Target, Id);
					}
					else
					{
						AddEdge(Id, Target);
					}
				}
			};

			ProcessOrdering(Manifest.LoadAfter, TEXT("loadAfter"), /*bTargetLoadsFirst*/ true);
			ProcessOrdering(Manifest.LoadBefore, TEXT("loadBefore"), /*bTargetLoadsFirst*/ false);
		}

		for (TPair<FModId, TArray<FModId>>& Pair : OutEdges)
		{
			Pair.Value.Sort();
		}
	}

	//~ ---------------------------------------------------------------------------------------------
	//~ Cascading rejection.
	//~ ---------------------------------------------------------------------------------------------

	/**
	 * Rejects every mod whose required dependency is gone, repeatedly, until nothing changes.
	 *
	 * The fixed point matters: a -> b -> c means rejecting c has to take b down on one pass and a on
	 * the next. Each rejection carries the whole chain from the immediate dependency to the root
	 * cause, so the player is told "you are missing c", not "b failed, good luck".
	 */
	void RunCascade(FModResolveResult& Result, FResolveState& State)
	{
		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;

			for (const FModId& Id : State.AllIds)
			{
				if (State.Rejected.Contains(Id))
				{
					continue;
				}

				const FModManifest* Manifest = FindManifest(State, Id);
				if (Manifest == nullptr)
				{
					continue;
				}

				for (const FModDependency& Dependency : Manifest->Dependencies)
				{
					if (Dependency.bOptional || !Dependency.Id.IsValid())
					{
						continue;
					}

					const bool bDependencyGone = !State.Manifests.Contains(Dependency.Id)
						|| State.Rejected.Contains(Dependency.Id);
					if (!bDependencyGone)
					{
						continue;
					}

					// Walk to the root cause: the chain of a cascade rejection already ends at whatever
					// originally went wrong, so appending to it keeps the whole story in one array.
					TArray<FModId> Chain;
					Chain.Add(Dependency.Id);

					FString RootMessage;
					if (const int32* DependencyIndex = State.FirstRejection.Find(Dependency.Id))
					{
						if (Result.Rejections.IsValidIndex(*DependencyIndex))
						{
							const FModRejection& DependencyRejection = Result.Rejections[*DependencyIndex];
							RootMessage = DependencyRejection.Message;

							if (DependencyRejection.Reason == EModLoadFailureReason::DependencyFailed)
							{
								Chain.Append(DependencyRejection.RelatedMods);
							}
						}
					}

					if (const int32* RootIndex = State.FirstRejection.Find(Chain.Last()))
					{
						if (Result.Rejections.IsValidIndex(*RootIndex))
						{
							RootMessage = Result.Rejections[*RootIndex].Message;
						}
					}

					FString Message = FString::Printf(
						TEXT("%s cannot be loaded because its required dependency '%s' was rejected. Dependency chain: %s -> %s."),
						*DescribeMod(*Manifest), *Dependency.Id.ToString(), *Id.ToString(),
						*JoinIds(Chain, TEXT(" -> ")));

					if (!RootMessage.IsEmpty())
					{
						Message += FString::Printf(TEXT(" Root cause: %s"), *RootMessage);
					}

					RejectMod(Result, State, Id, EModLoadFailureReason::DependencyFailed,
						ResolveCodes::CascadeReject, Message, Chain);

					bChanged = true;
					break;
				}
			}
		}
	}
}

//~ -------------------------------------------------------------------------------------------------
//~ FModEnvironment
//~ -------------------------------------------------------------------------------------------------

FModEnvironment FModEnvironment::FromSettings()
{
	FModEnvironment Environment;
	Environment.FrameworkVersion = ModFrameworkVersion::Get();

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	if (Settings == nullptr)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("FModEnvironment::FromSettings: the mod framework settings are not available. Falling back to a permissive environment (framework version %s, no game id, no SDK): mods will not be checked against this game's identity."),
			*Environment.FrameworkVersion.ToString());
		return Environment;
	}

	Environment.GameId = Settings->GameId;
	Environment.SdkId = Settings->SdkId;

	if (Environment.GameId.IsEmpty())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("FModEnvironment::FromSettings: no GameId is configured under Project Settings > Plugins > Mod Framework. Every mod's game requirement will be accepted, whichever game it was built for."));
	}

	FString ParseError;
	if (Settings->GameVersion.IsEmpty())
	{
		if (!Environment.GameId.IsEmpty())
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("FModEnvironment::FromSettings: game '%s' declares no GameVersion. Mods that pin a game version range will be accepted without checking."),
				*Environment.GameId);
		}
	}
	else if (!FModVersion::Parse(Settings->GameVersion, Environment.GameVersion, &ParseError))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("FModEnvironment::FromSettings: GameVersion '%s' is not a valid semantic version (%s). Game version requirements will not be enforced."),
			*Settings->GameVersion, *ParseError);
		Environment.GameVersion = FModVersion();
	}

	if (!Environment.SdkId.IsEmpty())
	{
		if (Settings->SdkVersion.IsEmpty())
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("FModEnvironment::FromSettings: SDK '%s' declares no SdkVersion. Mods that pin an SDK version range will be accepted without checking."),
				*Environment.SdkId);
		}
		else if (!FModVersion::Parse(Settings->SdkVersion, Environment.SdkVersion, &ParseError))
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("FModEnvironment::FromSettings: SdkVersion '%s' is not a valid semantic version (%s). SDK version requirements will not be enforced."),
				*Settings->SdkVersion, *ParseError);
			Environment.SdkVersion = FModVersion();
		}
	}

	return Environment;
}

//~ -------------------------------------------------------------------------------------------------
//~ FModResolveResult
//~ -------------------------------------------------------------------------------------------------

const FModRejection* FModResolveResult::FindRejection(const FModId& ModId) const
{
	for (const FModRejection& Rejection : Rejections)
	{
		if (Rejection.ModId == ModId)
		{
			return &Rejection;
		}
	}

	return nullptr;
}

FString FModResolveResult::ToDebugString() const
{
	TSet<FModId> RejectedIds;
	RejectedIds.Reserve(Rejections.Num());
	for (const FModRejection& Rejection : Rejections)
	{
		RejectedIds.Add(Rejection.ModId);
	}

	FString Out = FString::Printf(
		TEXT("Mod dependency resolve: %s - %d mod(s) in the load order, %d rejection(s) across %d mod(s), %d diagnostic(s)."),
		bSuccess ? TEXT("SUCCESS") : TEXT("FAILED"), LoadOrder.Num(), Rejections.Num(), RejectedIds.Num(),
		Diagnostics.Num());

	Out += TEXT("\nLoad order:");
	if (LoadOrder.Num() == 0)
	{
		Out += TEXT("\n  (empty)");
	}
	else
	{
		for (int32 Index = 0; Index < LoadOrder.Num(); ++Index)
		{
			Out += FString::Printf(TEXT("\n  [%d] %s"), Index, *LoadOrder[Index].ToString());
		}
	}

	Out += TEXT("\nRejections:");
	if (Rejections.Num() == 0)
	{
		Out += TEXT("\n  (none)");
	}
	else
	{
		for (const FModRejection& Rejection : Rejections)
		{
			Out += FString::Printf(TEXT("\n  %s [%s] %s"), *Rejection.ModId.ToString(),
				*ModFrameworkEnums::ToString(Rejection.Reason), *Rejection.Message);

			if (Rejection.RelatedMods.Num() > 0)
			{
				Out += FString::Printf(TEXT("\n      related: %s"), *JoinIds(Rejection.RelatedMods, TEXT(", ")));
			}
		}
	}

	if (Diagnostics.Num() > 0)
	{
		Out += TEXT("\nDiagnostics:");
		for (const FModDiagnostic& Diagnostic : Diagnostics)
		{
			Out += TEXT("\n  ");
			Out += Diagnostic.ToString();
		}
	}

	return Out;
}

//~ -------------------------------------------------------------------------------------------------
//~ FModDependencyResolver
//~ -------------------------------------------------------------------------------------------------

TArray<FModRejection> FModDependencyResolver::CheckEnvironment(const FModManifest& Manifest, const FModEnvironment& Environment)
{
	TArray<FModRejection> Rejections;
	CheckEnvironmentInternal(Manifest, Environment, Rejections, nullptr);
	return Rejections;
}

void FModDependencyResolver::BuildGraph(const TArray<FModManifest>& Manifests, TMap<FModId, TArray<FModId>>& OutEdges)
{
	TMap<FModId, const FModManifest*> ById;
	ById.Reserve(Manifests.Num());

	for (const FModManifest& Manifest : Manifests)
	{
		if (!Manifest.Id.IsValid())
		{
			continue;
		}

		const FModManifest** Existing = ById.Find(Manifest.Id);
		if (Existing == nullptr)
		{
			ById.Add(Manifest.Id, &Manifest);
		}
		else if (Manifest.Version > (*Existing)->Version)
		{
			*Existing = &Manifest;
		}
	}

	TArray<FModId> Nodes;
	ById.GenerateKeyArray(Nodes);
	Nodes.Sort();

	BuildEdges(Nodes, ById, OutEdges, nullptr);
}

TArray<TArray<FModId>> FModDependencyResolver::FindCycles(const TMap<FModId, TArray<FModId>>& Edges)
{
	TArray<TArray<FModId>> Cycles;

	// Nodes are every key plus every edge target, so a hand-built map with implicit nodes still works.
	// Sorting them means index order is id order, which is what makes "smallest member" a cheap min.
	TArray<FModId> Nodes;
	{
		TSet<FModId> NodeSet;
		for (const TPair<FModId, TArray<FModId>>& Pair : Edges)
		{
			NodeSet.Add(Pair.Key);
			for (const FModId& Target : Pair.Value)
			{
				NodeSet.Add(Target);
			}
		}

		Nodes = NodeSet.Array();
		Nodes.Sort();
	}

	const int32 NumNodes = Nodes.Num();
	if (NumNodes == 0)
	{
		return Cycles;
	}

	TMap<FModId, int32> IndexOf;
	IndexOf.Reserve(NumNodes);
	for (int32 Index = 0; Index < NumNodes; ++Index)
	{
		IndexOf.Add(Nodes[Index], Index);
	}

	TArray<TArray<int32>> Adjacency;
	Adjacency.SetNum(NumNodes);
	for (int32 Index = 0; Index < NumNodes; ++Index)
	{
		const TArray<FModId>* Targets = Edges.Find(Nodes[Index]);
		if (Targets == nullptr)
		{
			continue;
		}

		for (const FModId& Target : *Targets)
		{
			if (const int32* TargetIndex = IndexOf.Find(Target))
			{
				Adjacency[Index].AddUnique(*TargetIndex);
			}
		}

		Adjacency[Index].Sort();
	}

	// Tarjan, iteratively: a deeply chained mod list must not be able to blow the stack.
	struct FFrame
	{
		int32 Node = 0;
		int32 ChildCursor = 0;
	};

	TArray<int32> DiscoveryIndex;
	TArray<int32> LowLink;
	TArray<bool> OnStack;
	DiscoveryIndex.Init(INDEX_NONE, NumNodes);
	LowLink.Init(INDEX_NONE, NumNodes);
	OnStack.Init(false, NumNodes);

	TArray<int32> ComponentStack;
	TArray<FFrame> Frames;
	TArray<TArray<int32>> Components;
	int32 NextIndex = 0;

	for (int32 Root = 0; Root < NumNodes; ++Root)
	{
		if (DiscoveryIndex[Root] != INDEX_NONE)
		{
			continue;
		}

		DiscoveryIndex[Root] = NextIndex;
		LowLink[Root] = NextIndex;
		++NextIndex;
		ComponentStack.Push(Root);
		OnStack[Root] = true;
		Frames.Push(FFrame{ Root, 0 });

		while (Frames.Num() > 0)
		{
			const int32 FrameIndex = Frames.Num() - 1;
			const int32 Node = Frames[FrameIndex].Node;
			const TArray<int32>& Children = Adjacency[Node];

			if (Frames[FrameIndex].ChildCursor < Children.Num())
			{
				const int32 Child = Children[Frames[FrameIndex].ChildCursor];
				++Frames[FrameIndex].ChildCursor;

				if (DiscoveryIndex[Child] == INDEX_NONE)
				{
					DiscoveryIndex[Child] = NextIndex;
					LowLink[Child] = NextIndex;
					++NextIndex;
					ComponentStack.Push(Child);
					OnStack[Child] = true;
					Frames.Push(FFrame{ Child, 0 });
				}
				else if (OnStack[Child])
				{
					LowLink[Node] = FMath::Min(LowLink[Node], DiscoveryIndex[Child]);
				}

				continue;
			}

			Frames.Pop();
			if (Frames.Num() > 0)
			{
				const int32 Parent = Frames.Last().Node;
				LowLink[Parent] = FMath::Min(LowLink[Parent], LowLink[Node]);
			}

			if (LowLink[Node] == DiscoveryIndex[Node])
			{
				TArray<int32> Component;
				while (ComponentStack.Num() > 0)
				{
					const int32 Member = ComponentStack.Pop();
					OnStack[Member] = false;
					Component.Add(Member);
					if (Member == Node)
					{
						break;
					}
				}

				Components.Add(MoveTemp(Component));
			}
		}
	}

	for (const TArray<int32>& Component : Components)
	{
		if (Component.Num() == 0)
		{
			continue;
		}

		// Index order is id order, so the smallest index is the lexicographically smallest member.
		int32 Start = Component[0];
		for (const int32 Member : Component)
		{
			Start = FMath::Min(Start, Member);
		}

		const bool bSelfEdge = Adjacency[Start].Contains(Start);
		if (Component.Num() == 1 && !bSelfEdge)
		{
			continue;
		}

		TArray<int32> Path;
		if (bSelfEdge)
		{
			Path.Add(Start);
		}
		else
		{
			// Shortest cycle back to Start, breadth first with children visited in id order: the same
			// component always yields the same path, on every machine and every run.
			TSet<int32> Members;
			Members.Reserve(Component.Num());
			for (const int32 Member : Component)
			{
				Members.Add(Member);
			}

			TMap<int32, int32> Parent;
			TSet<int32> Visited;
			TArray<int32> Queue;

			Queue.Add(Start);
			Visited.Add(Start);

			int32 Head = 0;
			int32 Closer = INDEX_NONE;
			while (Head < Queue.Num() && Closer == INDEX_NONE)
			{
				const int32 Node = Queue[Head];
				++Head;

				for (const int32 Child : Adjacency[Node])
				{
					if (!Members.Contains(Child))
					{
						continue;
					}

					if (Child == Start)
					{
						Closer = Node;
						break;
					}

					if (!Visited.Contains(Child))
					{
						Visited.Add(Child);
						Parent.Add(Child, Node);
						Queue.Add(Child);
					}
				}
			}

			if (Closer != INDEX_NONE)
			{
				int32 Walk = Closer;
				while (true)
				{
					Path.Add(Walk);
					const int32* Previous = Parent.Find(Walk);
					if (Previous == nullptr)
					{
						break;
					}

					Walk = *Previous;
				}

				Algo::Reverse(Path);
			}
			else
			{
				// Unreachable for a genuine strongly connected component, but a graph handed to the
				// public entry point is untrusted too: report the members rather than nothing.
				Path = Component;
				Path.Sort();
			}
		}

		TArray<FModId> Cycle;
		Cycle.Reserve(Path.Num());
		for (const int32 Member : Path)
		{
			Cycle.Add(Nodes[Member]);
		}

		Cycles.Add(MoveTemp(Cycle));
	}

	// Components are disjoint, so first elements are unique and this is a total order.
	Cycles.Sort([](const TArray<FModId>& A, const TArray<FModId>& B)
	{
		if (A.Num() == 0 || B.Num() == 0)
		{
			return A.Num() > B.Num();
		}

		return A[0] < B[0];
	});

	return Cycles;
}

FModResolveResult FModDependencyResolver::Resolve(const FModResolveRequest& Request)
{
	FModResolveResult Result;
	FResolveState State;

	// --- 1. Index the candidates, resolving duplicate ids ------------------------------------------
	// Discovery order decides the tie-break between identical versions, so this walks Candidates as
	// given. Pointers into Request.Candidates stay valid: Request outlives the call and is const.
	{
		TMap<FModId, const FModManifest*> Winners;
		Winners.Reserve(Request.Candidates.Num());

		for (const FModManifest& Candidate : Request.Candidates)
		{
			if (!Candidate.Id.IsValid())
			{
				FString Name = Candidate.DisplayName;
				if (Name.IsEmpty())
				{
					Name = TEXT("<unnamed>");
				}

				Result.Diagnostics.Add(MakeDiagnostic(EModDiagnosticSeverity::Error,
					ResolveCodes::InvalidManifest, FModId(),
					FString::Printf(TEXT("A discovered manifest ('%s') carries no mod id and cannot take part in dependency resolution."),
						*Name)));
				continue;
			}

			const FModManifest** ExistingPtr = Winners.Find(Candidate.Id);
			if (ExistingPtr == nullptr)
			{
				Winners.Add(Candidate.Id, &Candidate);
				continue;
			}

			const FModManifest* const Previous = *ExistingPtr;
			const bool bCandidateWins = Candidate.Version > Previous->Version;
			const FModManifest* const Kept = bCandidateWins ? &Candidate : Previous;
			const FModManifest* const Dropped = bCandidateWins ? Previous : &Candidate;

			if (bCandidateWins)
			{
				*ExistingPtr = &Candidate;
			}

			// Deliberately NOT marked rejected: the id itself survives through the copy that was kept.
			AddRejectionRecord(Result, Dropped->Id, EModLoadFailureReason::DuplicateModId,
				ResolveCodes::DuplicateId,
				FString::Printf(TEXT("%s version %s is a duplicate: another copy of '%s' at version %s was found and takes precedence."),
					*DescribeMod(*Dropped), *Dropped->Version.ToString(), *Dropped->Id.ToString(),
					*Kept->Version.ToString()),
				TArray<FModId>({ Kept->Id }));
		}

		State.Manifests = MoveTemp(Winners);
	}

	State.Manifests.GenerateKeyArray(State.AllIds);
	State.AllIds.Sort();
	State.Rejected.Reserve(State.AllIds.Num());

	// --- 2. Explicitly disabled mods ---------------------------------------------------------------
	{
		TSet<FModId> DisabledSet;
		DisabledSet.Reserve(Request.DisabledMods.Num());
		for (const FModId& Id : Request.DisabledMods)
		{
			DisabledSet.Add(Id);
		}

		for (const FModId& Id : State.AllIds)
		{
			if (!DisabledSet.Contains(Id))
			{
				continue;
			}

			const FModManifest* Manifest = FindManifest(State, Id);
			if (Manifest == nullptr)
			{
				continue;
			}

			// Info, not Error: the player asked for this, so it must not make the whole resolve look
			// like a failure.
			RejectMod(Result, State, Id, EModLoadFailureReason::Disabled, ResolveCodes::Disabled,
				FString::Printf(TEXT("%s is disabled and will not be loaded."), *DescribeMod(*Manifest)),
				TArray<FModId>(), EModDiagnosticSeverity::Info);
		}
	}

	// --- 3. Environment ----------------------------------------------------------------------------
	if (Request.bCheckEnvironment)
	{
		for (const FModId& Id : State.AllIds)
		{
			if (State.Rejected.Contains(Id))
			{
				continue;
			}

			const FModManifest* Manifest = FindManifest(State, Id);
			if (Manifest == nullptr)
			{
				continue;
			}

			TArray<FModRejection> EnvironmentRejections;
			CheckEnvironmentInternal(*Manifest, Request.Environment, EnvironmentRejections, &Result.Diagnostics);

			for (const FModRejection& Rejection : EnvironmentRejections)
			{
				RejectMod(Result, State, Id, Rejection.Reason, ResolveCodes::Environment, Rejection.Message,
					Rejection.RelatedMods);
			}
		}
	}

	// --- 4. Required dependencies ------------------------------------------------------------------
	// Every unsatisfied requirement gets its own rejection: an author fixing a mod wants the whole
	// list, not the first entry followed by another run.
	for (const FModId& Id : State.AllIds)
	{
		if (State.Rejected.Contains(Id))
		{
			continue;
		}

		const FModManifest* Manifest = FindManifest(State, Id);
		if (Manifest == nullptr)
		{
			continue;
		}

		for (const FModDependency& Dependency : Manifest->Dependencies)
		{
			if (Dependency.bOptional)
			{
				continue;
			}

			if (!Dependency.Id.IsValid())
			{
				Result.Diagnostics.Add(MakeDiagnostic(EModDiagnosticSeverity::Warning,
					ResolveCodes::MissingDependency, Id,
					FString::Printf(TEXT("%s declares a dependency with no id; the entry is ignored."),
						*DescribeMod(*Manifest))));
				continue;
			}

			const FModManifest* DependencyManifest = FindManifest(State, Dependency.Id);
			if (DependencyManifest == nullptr)
			{
				RejectMod(Result, State, Id, EModLoadFailureReason::MissingDependency,
					ResolveCodes::MissingDependency,
					FString::Printf(TEXT("%s requires %s, but it is not installed.%s"),
						*DescribeMod(*Manifest), *DescribeRequirement(Dependency.Id, Dependency.VersionRange),
						*DescribeDependencyReason(Dependency)),
					TArray<FModId>({ Dependency.Id }));
				continue;
			}

			if (RangeIsMalformed(Dependency.VersionRange))
			{
				Result.Diagnostics.Add(MakeDiagnostic(EModDiagnosticSeverity::Warning,
					ResolveCodes::VersionConflict, Id,
					FString::Printf(TEXT("%s declares the version range '%s' for dependency '%s', which is not valid range syntax; it is treated as 'any version'."),
						*DescribeMod(*Manifest), *Dependency.VersionRange.Expression, *Dependency.Id.ToString()),
					Dependency.Id.ToString()));
				continue;
			}

			if (RangeConstrains(Dependency.VersionRange)
				&& !Dependency.VersionRange.Satisfies(DependencyManifest->Version))
			{
				RejectMod(Result, State, Id, EModLoadFailureReason::IncompatibleDependencyVersion,
					ResolveCodes::VersionConflict,
					FString::Printf(TEXT("%s requires %s, but version %s is installed.%s"),
						*DescribeMod(*Manifest), *DescribeRequirement(Dependency.Id, Dependency.VersionRange),
						*DependencyManifest->Version.ToString(), *DescribeDependencyReason(Dependency)),
					TArray<FModId>({ Dependency.Id }));
			}
		}
	}

	// --- 5/6. Cascade to a fixed point, then strip cycles, then cascade again ----------------------
	// Removing a cycle can orphan a dependent, and it can also expose a smaller cycle that was hidden
	// inside a larger strongly connected component, so both steps repeat. Every pass rejects at least
	// one mod, which bounds the loop by the number of candidates.
	RunCascade(Result, State);

	{
		const int32 MaxPasses = State.AllIds.Num() + 1;
		for (int32 Pass = 0; Pass < MaxPasses; ++Pass)
		{
			TArray<FModId> Accepted;
			CollectAccepted(State, Accepted);

			TMap<FModId, TArray<FModId>> Edges;
			BuildEdges(Accepted, State.Manifests, Edges, nullptr);

			const TArray<TArray<FModId>> Cycles = FindCycles(Edges);
			if (Cycles.Num() == 0)
			{
				break;
			}

			for (const TArray<FModId>& Cycle : Cycles)
			{
				if (Cycle.Num() == 0)
				{
					continue;
				}

				const FString CyclePath = JoinIds(Cycle, TEXT(" -> ")) + TEXT(" -> ") + Cycle[0].ToString();

				for (const FModId& Member : Cycle)
				{
					if (State.Rejected.Contains(Member))
					{
						continue;
					}

					const FModManifest* Manifest = FindManifest(State, Member);
					const FString Who = Manifest != nullptr
						? DescribeMod(*Manifest)
						: FString::Printf(TEXT("Mod '%s'"), *Member.ToString());

					RejectMod(Result, State, Member, EModLoadFailureReason::CircularDependency,
						ResolveCodes::Cycle,
						FString::Printf(TEXT("%s is part of a dependency cycle and cannot be loaded. Dependency cycle: %s"),
							*Who, *CyclePath),
						Cycle);
				}
			}

			RunCascade(Result, State);
		}
	}

	// --- 7. Topological sort -----------------------------------------------------------------------
	TArray<FModId> Accepted;
	CollectAccepted(State, Accepted);

	TMap<FModId, TArray<FModId>> FinalEdges;
	BuildEdges(Accepted, State.Manifests, FinalEdges, &Result.Diagnostics);

	TMap<FModId, int32> InDegree;
	InDegree.Reserve(Accepted.Num());
	for (const FModId& Id : Accepted)
	{
		InDegree.Add(Id, 0);
	}

	for (const FModId& Id : Accepted)
	{
		const TArray<FModId>* Targets = FinalEdges.Find(Id);
		if (Targets == nullptr)
		{
			continue;
		}

		for (const FModId& Target : *Targets)
		{
			if (int32* Degree = InDegree.Find(Target))
			{
				++(*Degree);
			}
		}
	}

	auto PriorityOf = [&State](const FModId& Id) -> int32
	{
		const FModManifest* Manifest = FindManifest(State, Id);
		return Manifest != nullptr ? Manifest->Priority : 0;
	};

	// The whole point of the ready set: higher priority first, then id order. Both keys together are
	// a total order over distinct mods, so the sort needs no stability guarantee to be reproducible.
	auto ReadyOrder = [&PriorityOf](const FModId& A, const FModId& B)
	{
		const int32 PriorityA = PriorityOf(A);
		const int32 PriorityB = PriorityOf(B);
		if (PriorityA != PriorityB)
		{
			return PriorityA > PriorityB;
		}

		return A < B;
	};

	TArray<FModId> Ready;
	for (const FModId& Id : Accepted)
	{
		const int32* Degree = InDegree.Find(Id);
		if (Degree != nullptr && *Degree == 0)
		{
			Ready.Add(Id);
		}
	}

	Ready.Sort(ReadyOrder);

	Result.LoadOrder.Reserve(Accepted.Num());
	while (Ready.Num() > 0)
	{
		const FModId Next = Ready[0];
		Ready.RemoveAt(0);
		Result.LoadOrder.Add(Next);

		bool bReleasedAny = false;
		if (const TArray<FModId>* Targets = FinalEdges.Find(Next))
		{
			for (const FModId& Target : *Targets)
			{
				int32* Degree = InDegree.Find(Target);
				if (Degree == nullptr)
				{
					continue;
				}

				--(*Degree);
				if (*Degree == 0)
				{
					Ready.Add(Target);
					bReleasedAny = true;
				}
			}
		}

		if (bReleasedAny)
		{
			Ready.Sort(ReadyOrder);
		}
	}

	// Cycle removal above should make this unreachable. It is kept because an ordering bug must
	// surface as a diagnostic rather than as a silently truncated load order.
	if (Result.LoadOrder.Num() != Accepted.Num())
	{
		TArray<FModId> Unordered;
		for (const FModId& Id : Accepted)
		{
			if (!Result.LoadOrder.Contains(Id))
			{
				Unordered.Add(Id);
			}
		}

		const FString UnorderedText = JoinIds(Unordered, TEXT(", "));
		for (const FModId& Id : Unordered)
		{
			const FModManifest* Manifest = FindManifest(State, Id);
			const FString Who = Manifest != nullptr
				? DescribeMod(*Manifest)
				: FString::Printf(TEXT("Mod '%s'"), *Id.ToString());

			RejectMod(Result, State, Id, EModLoadFailureReason::CircularDependency, ResolveCodes::Cycle,
				FString::Printf(TEXT("%s could not be placed in the load order: it is still part of an unresolved ordering cycle involving %s."),
					*Who, *UnorderedText),
				Unordered);
		}
	}

	// --- 8. Verdict --------------------------------------------------------------------------------
	bool bHardRejection = false;
	for (const FModRejection& Rejection : Result.Rejections)
	{
		if (Rejection.Reason != EModLoadFailureReason::Disabled)
		{
			bHardRejection = true;
			break;
		}
	}

	Result.bSuccess = !bHardRejection && !ModDiagnostics::HasErrors(Result.Diagnostics);

	UE_LOG(LogModFramework, Verbose,
		TEXT("Dependency resolve finished: %d candidate(s) -> %d in the load order, %d rejection(s)."),
		Request.Candidates.Num(), Result.LoadOrder.Num(), Result.Rejections.Num());

	return Result;
}
