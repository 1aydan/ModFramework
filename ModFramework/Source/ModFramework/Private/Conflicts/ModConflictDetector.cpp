// Copyright (c) 2026. Licensed for use in your own projects.

#include "Conflicts/ModConflictDetector.h"

#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Logging/LogMacros.h"
#include "Math/NumericLimits.h"
#include "Templates/TypeHash.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"

// ---------------------------------------------------------------------------------------------
// FModConflictPolicyTable
// ---------------------------------------------------------------------------------------------

FName FModConflictPolicyTable::MakeResourceKey(FName ExtensionPointId, FName ResourceId)
{
	// "<extensionPoint>:<resource>". The single definition of the ResourcePolicies key format;
	// NAME_None stringifies to "None", which keeps the key well formed even for malformed input.
	const FString Key = ExtensionPointId.ToString() + TEXT(":") + ResourceId.ToString();
	return FName(*Key);
}

EModConflictPolicy FModConflictPolicyTable::Resolve(FName ExtensionPointId, FName ResourceId) const
{
	if (const EModConflictPolicy* ResourceOverride = ResourcePolicies.Find(MakeResourceKey(ExtensionPointId, ResourceId)))
	{
		return *ResourceOverride;
	}

	if (const EModConflictPolicy* PointOverride = PointPolicies.Find(ExtensionPointId))
	{
		return *PointOverride;
	}

	return DefaultPolicy;
}

// ---------------------------------------------------------------------------------------------
// Implementation details
// ---------------------------------------------------------------------------------------------

namespace ModConflictDetectorPrivate
{
	/** Identity of one contested resource. Claims never cross extension points. */
	struct FResourceKey
	{
		FName ExtensionPointId;
		FName ResourceId;

		bool operator==(const FResourceKey& Other) const
		{
			return ExtensionPointId == Other.ExtensionPointId && ResourceId == Other.ResourceId;
		}

		friend uint32 GetTypeHash(const FResourceKey& In)
		{
			return HashCombine(GetTypeHash(In.ExtensionPointId), GetTypeHash(In.ResourceId));
		}
	};

	/** Everything one mod contributes to a single resource, folded across all of that mod's claims. */
	struct FContender
	{
		FModId ModId;
		int32 Priority = 0;
		int32 LoadOrder = INDEX_NONE;
	};

	/** All claims on one resource, accumulated in the order the claims were supplied. */
	struct FResourceGroup
	{
		FName ExtensionPointId;
		FName ResourceId;
		TArray<FContender> Contenders;
		TMap<FModId, int32> ContenderIndexByMod;

		/** The policy every claim so far asked for; only meaningful while bPreferenceUnanimous. */
		EModConflictPolicy PreferredPolicy = EModConflictPolicy::Error;
		bool bPreferenceUnanimous = true;
		bool bHasClaim = false;
	};

	/** Which of the four rules in FModConflictDetector::Detect chose the applied policy. */
	enum class EPolicySource : uint8
	{
		ResourceOverride,
		PointOverride,
		UnanimousPreference,
		TableDefault
	};

	/** One row of the BuildReport table. The last column is never padded. */
	struct FReportRow
	{
		FString Index;
		FString Point;
		FString Resource;
		FString Policy;
		FString Winner;
		FString Contenders;
	};

	/**
	 * Orders load orders so that an unassigned one (INDEX_NONE, or any other negative value that
	 * untrusted data may carry) sorts after every assigned one instead of before them - otherwise
	 * "FirstWins" would hand victory to a mod the resolver has not placed yet.
	 */
	int64 LoadOrderSortKey(int32 InLoadOrder)
	{
		return InLoadOrder >= 0 ? static_cast<int64>(InLoadOrder) : TNumericLimits<int64>::Max();
	}

	/** False for a value outside the enum, which deserializing a hand-edited config can produce. */
	bool IsKnownPolicy(EModConflictPolicy In)
	{
		switch (In)
		{
		case EModConflictPolicy::Error:
		case EModConflictPolicy::FirstWins:
		case EModConflictPolicy::LastWins:
		case EModConflictPolicy::Priority:
		case EModConflictPolicy::Merge:
			return true;
		}

		return false;
	}

	/** "com.a, com.b" or "'com.a', 'com.b'". Returns an empty string for an empty array. */
	FString JoinModIds(const TArray<FModId>& In, bool bQuote)
	{
		FString Result;

		for (int32 Index = 0; Index < In.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result.Append(TEXT(", "));
			}

			if (bQuote)
			{
				Result.Append(TEXT("'"));
				Result.Append(In[Index].ToString());
				Result.Append(TEXT("'"));
			}
			else
			{
				Result.Append(In[Index].ToString());
			}
		}

		return Result;
	}

	/** "both contenders" / "all 3 contenders" - English that reads properly at every count. */
	FString DescribeAllContenders(int32 NumContenders)
	{
		if (NumContenders == 2)
		{
			return TEXT("both contenders");
		}

		return FString::Printf(TEXT("all %d contenders"), NumContenders);
	}

	/** The trailing "... are overridden" half of a winner sentence. */
	FString DescribeLosers(const TArray<FModId>& Losers)
	{
		if (Losers.Num() == 0)
		{
			return TEXT("nothing else is overridden");
		}

		if (Losers.Num() == 1)
		{
			return FString::Printf(TEXT("'%s' is overridden"), *Losers[0].ToString());
		}

		return FString::Printf(TEXT("%s are overridden"), *JoinModIds(Losers, /*bQuote*/ true));
	}

	/** Applies the four-rule precedence and reports which rule fired. */
	EModConflictPolicy ResolvePolicy(const FModConflictPolicyTable& Policies, const FResourceGroup& Group, EPolicySource& OutSource)
	{
		const FName ResourceKey = FModConflictPolicyTable::MakeResourceKey(Group.ExtensionPointId, Group.ResourceId);

		if (const EModConflictPolicy* ResourceOverride = Policies.ResourcePolicies.Find(ResourceKey))
		{
			OutSource = EPolicySource::ResourceOverride;
			return *ResourceOverride;
		}

		if (const EModConflictPolicy* PointOverride = Policies.PointPolicies.Find(Group.ExtensionPointId))
		{
			OutSource = EPolicySource::PointOverride;
			return *PointOverride;
		}

		if (Group.bHasClaim && Group.bPreferenceUnanimous)
		{
			OutSource = EPolicySource::UnanimousPreference;
			return Group.PreferredPolicy;
		}

		OutSource = EPolicySource::TableDefault;
		return Policies.DefaultPolicy;
	}

	/** The middle clause of the explanation: how the applied policy was arrived at. */
	FString DescribePolicyRule(EPolicySource Source, const FResourceGroup& Group, EModConflictPolicy Policy)
	{
		const FString PolicyName = ModFrameworkEnums::ToString(Policy);

		switch (Source)
		{
		case EPolicySource::ResourceOverride:
			return FString::Printf(
				TEXT("the conflict policy table overrides this exact resource with %s"), *PolicyName);

		case EPolicySource::PointOverride:
			return FString::Printf(
				TEXT("the conflict policy table overrides extension point '%s' with %s"),
				*Group.ExtensionPointId.ToString(), *PolicyName);

		case EPolicySource::UnanimousPreference:
			return FString::Printf(
				TEXT("the conflict policy table has no override for it and every claim asked for %s"), *PolicyName);

		default:
			return FString::Printf(
				TEXT("the conflict policy table has no override for it and the claims disagree about what they want, ")
				TEXT("so the table default of %s applies"), *PolicyName);
		}
	}

	/**
	 * Picks the winning contender, or nullptr when the policy names none.
	 * Sorted must already be ordered by (load order ascending, mod id ascending), which is what makes
	 * every tie-break below resolve to the lowest mod id.
	 */
	const FContender* SelectWinner(const TArray<FContender>& Sorted, EModConflictPolicy Policy)
	{
		if (Sorted.Num() == 0)
		{
			return nullptr;
		}

		switch (Policy)
		{
		case EModConflictPolicy::FirstWins:
			return &Sorted[0];

		case EModConflictPolicy::LastWins:
		{
			const FContender* Best = &Sorted[0];
			for (int32 Index = 1; Index < Sorted.Num(); ++Index)
			{
				if (LoadOrderSortKey(Sorted[Index].LoadOrder) > LoadOrderSortKey(Best->LoadOrder))
				{
					Best = &Sorted[Index];
				}
			}
			return Best;
		}

		case EModConflictPolicy::Priority:
		{
			const FContender* Best = &Sorted[0];
			for (int32 Index = 1; Index < Sorted.Num(); ++Index)
			{
				const FContender& Candidate = Sorted[Index];
				if (Candidate.Priority > Best->Priority)
				{
					Best = &Candidate;
				}
				else if (Candidate.Priority == Best->Priority
					&& LoadOrderSortKey(Candidate.LoadOrder) > LoadOrderSortKey(Best->LoadOrder))
				{
					Best = &Candidate;
				}
			}
			return Best;
		}

		case EModConflictPolicy::Merge:
		case EModConflictPolicy::Error:
		default:
			return nullptr;
		}
	}

	/** The final clause of the explanation: what actually happens to the contenders. */
	FString DescribeOutcome(EModConflictPolicy Policy, const FContender* Winner, const TArray<FModId>& Losers, int32 NumContenders)
	{
		if (Winner == nullptr)
		{
			switch (Policy)
			{
			case EModConflictPolicy::Merge:
				return FString::Printf(
					TEXT("%s are kept and merged, so there is no single winner"), *DescribeAllContenders(NumContenders));

			case EModConflictPolicy::Error:
				return FString::Printf(
					TEXT("no mod may claim it and %s are blocked"), *DescribeAllContenders(NumContenders));

			default:
				return FString::Printf(
					TEXT("the policy value %d is not a known conflict policy, so the conflict is treated as blocking ")
					TEXT("and %s are blocked"),
					static_cast<int32>(Policy), *DescribeAllContenders(NumContenders));
			}
		}

		const FString WinnerId = Winner->ModId.ToString();
		const FString LoserClause = DescribeLosers(Losers);

		switch (Policy)
		{
		case EModConflictPolicy::FirstWins:
			if (Winner->LoadOrder >= 0)
			{
				return FString::Printf(TEXT("'%s' wins because it loads first (load order %d) and %s"),
					*WinnerId, Winner->LoadOrder, *LoserClause);
			}
			return FString::Printf(
				TEXT("'%s' wins because no load order has been assigned yet, so the lowest mod id is taken, and %s"),
				*WinnerId, *LoserClause);

		case EModConflictPolicy::LastWins:
			if (Winner->LoadOrder >= 0)
			{
				return FString::Printf(TEXT("'%s' wins because it loads last (load order %d) and %s"),
					*WinnerId, Winner->LoadOrder, *LoserClause);
			}
			return FString::Printf(
				TEXT("'%s' wins because it has no assigned load order and therefore sorts last, and %s"),
				*WinnerId, *LoserClause);

		case EModConflictPolicy::Priority:
			return FString::Printf(TEXT("'%s' wins because it declares the highest priority (%d) and %s"),
				*WinnerId, Winner->Priority, *LoserClause);

		default:
			// Unreachable: SelectWinner only returns a contender for the three policies above.
			return FString::Printf(TEXT("'%s' wins and %s"), *WinnerId, *LoserClause);
		}
	}

	/** Widens Width to fit Cell. */
	void Widen(int32& Width, const FString& Cell)
	{
		if (Cell.Len() > Width)
		{
			Width = Cell.Len();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// FModConflictDetector
// ---------------------------------------------------------------------------------------------

TArray<FModConflict> FModConflictDetector::Detect(const TArray<FModResourceClaim>& Claims, const FModConflictPolicyTable& Policies)
{
	using namespace ModConflictDetectorPrivate;

	TArray<FModConflict> Conflicts;

	// --- Group the claims by (extension point, resource) --------------------------------------
	//
	// Groups are kept in a plain array so that the traversal order below does not depend on hash
	// table layout; the map only maps a key to its index. The final output is sorted anyway, but
	// keeping the intermediate stage deterministic makes the whole function reproducible.

	TArray<FResourceGroup> Groups;
	TMap<FResourceKey, int32> GroupIndexByKey;

	for (const FModResourceClaim& Claim : Claims)
	{
		// Untrusted input: a claim that names no mod, no extension point or no resource cannot be
		// reconciled against anything, so it is dropped rather than fabricating a conflict.
		if (!Claim.ModId.IsValid() || Claim.ExtensionPointId.IsNone() || Claim.ResourceId.IsNone())
		{
			UE_LOG(LogModFramework, Verbose,
				TEXT("Conflict detection skipped an incomplete resource claim (mod '%s', point '%s', resource '%s')."),
				*Claim.ModId.ToString(), *Claim.ExtensionPointId.ToString(), *Claim.ResourceId.ToString());
			continue;
		}

		const FResourceKey Key{ Claim.ExtensionPointId, Claim.ResourceId };

		int32* ExistingGroupIndex = GroupIndexByKey.Find(Key);
		if (ExistingGroupIndex == nullptr)
		{
			FResourceGroup NewGroup;
			NewGroup.ExtensionPointId = Claim.ExtensionPointId;
			NewGroup.ResourceId = Claim.ResourceId;

			const int32 NewIndex = Groups.Add(MoveTemp(NewGroup));
			ExistingGroupIndex = &GroupIndexByKey.Add(Key, NewIndex);
		}

		FResourceGroup& Group = Groups[*ExistingGroupIndex];

		// Unanimity is a property of every claim on the resource, not of every mod: a mod that
		// contradicts itself across two extensions cannot impose a preference either.
		if (!Group.bHasClaim)
		{
			Group.PreferredPolicy = Claim.PreferredPolicy;
			Group.bHasClaim = true;
		}
		else if (Group.PreferredPolicy != Claim.PreferredPolicy)
		{
			Group.bPreferenceUnanimous = false;
		}

		// Fold repeated claims from one mod into a single contender. Two claims by the same mod on
		// the same resource are explicitly not a conflict.
		if (const int32* ExistingContenderIndex = Group.ContenderIndexByMod.Find(Claim.ModId))
		{
			FContender& Contender = Group.Contenders[*ExistingContenderIndex];

			if (Claim.Priority > Contender.Priority)
			{
				Contender.Priority = Claim.Priority;
			}

			if (LoadOrderSortKey(Claim.LoadOrder) < LoadOrderSortKey(Contender.LoadOrder))
			{
				Contender.LoadOrder = Claim.LoadOrder;
			}
		}
		else
		{
			FContender Contender;
			Contender.ModId = Claim.ModId;
			Contender.Priority = Claim.Priority;
			Contender.LoadOrder = Claim.LoadOrder;

			const int32 ContenderIndex = Group.Contenders.Add(Contender);
			Group.ContenderIndexByMod.Add(Claim.ModId, ContenderIndex);
		}
	}

	// --- Turn every contested group into a conflict --------------------------------------------

	for (FResourceGroup& Group : Groups)
	{
		if (Group.Contenders.Num() < 2)
		{
			continue;
		}

		// Load order first, mod id second. Every winner tie-break below relies on this ordering.
		Group.Contenders.Sort([](const ModConflictDetectorPrivate::FContender& A, const ModConflictDetectorPrivate::FContender& B)
		{
			const int64 KeyA = ModConflictDetectorPrivate::LoadOrderSortKey(A.LoadOrder);
			const int64 KeyB = ModConflictDetectorPrivate::LoadOrderSortKey(B.LoadOrder);

			if (KeyA != KeyB)
			{
				return KeyA < KeyB;
			}

			return A.ModId < B.ModId;
		});

		EPolicySource PolicySource = EPolicySource::TableDefault;
		const EModConflictPolicy AppliedPolicy = ResolvePolicy(Policies, Group, PolicySource);
		const FContender* Winner = SelectWinner(Group.Contenders, AppliedPolicy);

		FModConflict Conflict;
		Conflict.ExtensionPointId = Group.ExtensionPointId;
		Conflict.ResourceId = Group.ResourceId;
		Conflict.AppliedPolicy = AppliedPolicy;

		Conflict.Contenders.Reserve(Group.Contenders.Num());
		for (const FContender& Contender : Group.Contenders)
		{
			Conflict.Contenders.Add(Contender.ModId);
		}

		if (Winner != nullptr)
		{
			Conflict.Winner = Winner->ModId;
			Conflict.Losers.Reserve(Group.Contenders.Num() - 1);

			for (const FContender& Contender : Group.Contenders)
			{
				if (Contender.ModId != Winner->ModId)
				{
					Conflict.Losers.Add(Contender.ModId);
				}
			}
		}

		// A policy value the framework does not recognise is treated as Error: refusing to load is
		// the only safe reading of "the game asked for something this build cannot honour".
		Conflict.bBlocking = (AppliedPolicy == EModConflictPolicy::Error) || !IsKnownPolicy(AppliedPolicy);

		Conflict.Explanation = FString::Printf(
			TEXT("%d mods claim resource '%s' on extension point '%s' (in load order: %s); %s, so %s."),
			Conflict.Contenders.Num(),
			*Group.ResourceId.ToString(),
			*Group.ExtensionPointId.ToString(),
			*JoinModIds(Conflict.Contenders, /*bQuote*/ false),
			*DescribePolicyRule(PolicySource, Group, AppliedPolicy),
			*DescribeOutcome(AppliedPolicy, Winner, Conflict.Losers, Conflict.Contenders.Num()));

		UE_LOG(LogModFramework, Verbose, TEXT("Mod conflict: %s"), *Conflict.Explanation);

		Conflicts.Add(MoveTemp(Conflict));
	}

	// --- Deterministic output order -------------------------------------------------------------
	//
	// (ExtensionPointId, ResourceId) is unique across conflicts, so this is a total order and the
	// result does not depend on the order the claims arrived in. FName::Compare is the alphabetical
	// comparison that is stable across process runs (FastLess deliberately is not).

	Conflicts.StableSort([](const FModConflict& A, const FModConflict& B)
	{
		const int32 PointCompare = A.ExtensionPointId.Compare(B.ExtensionPointId);
		if (PointCompare != 0)
		{
			return PointCompare < 0;
		}

		return A.ResourceId.Compare(B.ResourceId) < 0;
	});

	return Conflicts;
}

TArray<FModId> FModConflictDetector::GetBlockedMods(const TArray<FModConflict>& Conflicts)
{
	TSet<FModId> Unique;

	for (const FModConflict& Conflict : Conflicts)
	{
		if (!Conflict.bBlocking)
		{
			continue;
		}

		for (const FModId& Contender : Conflict.Contenders)
		{
			if (Contender.IsValid())
			{
				Unique.Add(Contender);
			}
		}
	}

	TArray<FModId> Blocked = Unique.Array();
	Blocked.Sort([](const FModId& A, const FModId& B)
	{
		return A < B;
	});

	return Blocked;
}

FString FModConflictDetector::BuildReport(const TArray<FModConflict>& Conflicts)
{
	using namespace ModConflictDetectorPrivate;

	if (Conflicts.Num() == 0)
	{
		return TEXT("No mod conflicts detected.");
	}

	int32 BlockingCount = 0;
	for (const FModConflict& Conflict : Conflicts)
	{
		if (Conflict.bBlocking)
		{
			++BlockingCount;
		}
	}

	// --- Cells ----------------------------------------------------------------------------------

	FReportRow Header;
	Header.Index = TEXT("#");
	Header.Point = TEXT("EXTENSION POINT");
	Header.Resource = TEXT("RESOURCE");
	Header.Policy = TEXT("POLICY");
	Header.Winner = TEXT("WINNER");
	Header.Contenders = TEXT("CONTENDERS");

	TArray<FReportRow> Rows;
	Rows.Reserve(Conflicts.Num());

	for (int32 Index = 0; Index < Conflicts.Num(); ++Index)
	{
		const FModConflict& Conflict = Conflicts[Index];

		FReportRow Row;
		Row.Index = FString::Printf(TEXT("%d"), Index + 1);
		Row.Point = Conflict.ExtensionPointId.ToString();
		Row.Resource = Conflict.ResourceId.ToString();
		Row.Policy = ModFrameworkEnums::ToString(Conflict.AppliedPolicy);

		if (Conflict.Winner.IsValid())
		{
			Row.Winner = Conflict.Winner.ToString();
		}
		else if (Conflict.bBlocking)
		{
			Row.Winner = TEXT("(blocked)");
		}
		else if (Conflict.AppliedPolicy == EModConflictPolicy::Merge)
		{
			Row.Winner = TEXT("(merged)");
		}
		else
		{
			Row.Winner = TEXT("(none)");
		}

		Row.Contenders = JoinModIds(Conflict.Contenders, /*bQuote*/ false);

		Rows.Add(MoveTemp(Row));
	}

	// --- Column widths --------------------------------------------------------------------------

	int32 IndexWidth = Header.Index.Len();
	int32 PointWidth = Header.Point.Len();
	int32 ResourceWidth = Header.Resource.Len();
	int32 PolicyWidth = Header.Policy.Len();
	int32 WinnerWidth = Header.Winner.Len();
	int32 ContenderWidth = Header.Contenders.Len();

	for (const FReportRow& Row : Rows)
	{
		Widen(IndexWidth, Row.Index);
		Widen(PointWidth, Row.Point);
		Widen(ResourceWidth, Row.Resource);
		Widen(PolicyWidth, Row.Policy);
		Widen(WinnerWidth, Row.Winner);
		Widen(ContenderWidth, Row.Contenders);
	}

	// The trailing column is never padded, so no line carries invisible whitespace.
	auto FormatRow = [&](const ModConflictDetectorPrivate::FReportRow& Row)
	{
		return FString::Printf(TEXT("  %s  %s  %s  %s  %s  %s"),
			*Row.Index.RightPad(IndexWidth),
			*Row.Point.RightPad(PointWidth),
			*Row.Resource.RightPad(ResourceWidth),
			*Row.Policy.RightPad(PolicyWidth),
			*Row.Winner.RightPad(WinnerWidth),
			*Row.Contenders);
	};

	FReportRow Separator;
	Separator.Index = FString::ChrN(IndexWidth, TEXT('-'));
	Separator.Point = FString::ChrN(PointWidth, TEXT('-'));
	Separator.Resource = FString::ChrN(ResourceWidth, TEXT('-'));
	Separator.Policy = FString::ChrN(PolicyWidth, TEXT('-'));
	Separator.Winner = FString::ChrN(WinnerWidth, TEXT('-'));
	Separator.Contenders = FString::ChrN(ContenderWidth, TEXT('-'));

	// --- Assemble ---------------------------------------------------------------------------------

	TArray<FString> Lines;
	Lines.Reserve(Rows.Num() * 3 + 8);

	Lines.Add(FString::Printf(TEXT("Mod conflicts: %d detected, %d blocking, %d resolved."),
		Conflicts.Num(), BlockingCount, Conflicts.Num() - BlockingCount));
	Lines.Add(FString());
	Lines.Add(FormatRow(Header));
	Lines.Add(FormatRow(Separator));

	for (const FReportRow& Row : Rows)
	{
		Lines.Add(FormatRow(Row));
	}

	Lines.Add(FString());
	Lines.Add(TEXT("Explanations:"));

	for (int32 Index = 0; Index < Conflicts.Num(); ++Index)
	{
		const FModConflict& Conflict = Conflicts[Index];
		const FName ResourceKey = FModConflictPolicyTable::MakeResourceKey(Conflict.ExtensionPointId, Conflict.ResourceId);

		Lines.Add(FString::Printf(TEXT("  [%d] %s"), Index + 1, *ResourceKey.ToString()));
		Lines.Add(FString::Printf(TEXT("      %s"), *Conflict.Explanation));
	}

	return FString::Join(Lines, TEXT("\n"));
}
