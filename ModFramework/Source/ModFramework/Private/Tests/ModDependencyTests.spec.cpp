// Copyright (c) 2026. Licensed for use in your own projects.

#if WITH_DEV_AUTOMATION_TESTS

#include "Conflicts/ModConflictDetector.h"
#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Dependencies/ModDependencyResolver.h"
#include "Dependencies/ModDependencyTypes.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreMiscDefines.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"

/**
 * Builders and small queries shared by every test below.
 *
 * Nothing in here touches a world, a game instance, the settings object or the file system: both
 * FModDependencyResolver and FModConflictDetector are pure functions over their arguments, and these
 * tests are written to keep it that way. If a change to either makes one of these tests need a
 * UObject, that change broke the purity guarantee, not the test.
 */
namespace ModDependencyTestsPrivate
{
	/** The game every manifest built here claims to target. */
	const TCHAR* const TestGameId = TEXT("test.game");

	/** The SDK the test environment ships. */
	const TCHAR* const TestSdkId = TEXT("test.game.sdk");

	/** The extension point most conflict tests claim resources on. */
	const TCHAR* const TestPoint = TEXT("game.weapon");

	/** The resource most conflict tests fight over. */
	const TCHAR* const TestResource = TEXT("weapon.longsword");

	FModId MakeTestId(const TCHAR* InId)
	{
		return FModId(FName(InId));
	}

	/**
	 * Parses a range expression the way a manifest parser would.
	 *
	 * A deliberately malformed expression is fine to pass here: Parse records the failure on the range
	 * itself, which is exactly the state the resolver has to cope with when a mod author mistypes a
	 * caret.
	 */
	FModVersionRange MakeTestRange(const TCHAR* Expression)
	{
		FModVersionRange Range;
		FModVersionRange::Parse(Expression, Range, nullptr);
		return Range;
	}

	/**
	 * A manifest that satisfies every check in MakeTestEnvironment().
	 *
	 * DisplayName is deliberately left empty so FModDependencyResolver's message formatter renders the
	 * mod as "Mod '<id>'" rather than "Mod '<name>' (<id>)". That keeps the exact-string assertions in
	 * this file readable, and the "with a display name" variant is covered by its own test.
	 */
	FModManifest MakeTestManifest(const TCHAR* InId, const TCHAR* InVersion = TEXT("1.0.0"))
	{
		FModManifest Manifest;
		Manifest.Id = MakeTestId(InId);
		Manifest.Version = FModVersion::FromString(InVersion);
		Manifest.Game.GameId = TestGameId;
		return Manifest;
	}

	/** Appends a required (or, when bOptional, an optional) dependency to Manifest. */
	void AddTestDependency(FModManifest& Manifest, const TCHAR* DependencyId,
		const TCHAR* RangeExpression = TEXT("*"), bool bOptional = false, const TCHAR* Reason = nullptr)
	{
		FModDependency Dependency;
		Dependency.Id = MakeTestId(DependencyId);
		Dependency.VersionRange = MakeTestRange(RangeExpression);
		Dependency.bOptional = bOptional;
		if (Reason != nullptr)
		{
			Dependency.Reason = Reason;
		}

		Manifest.Dependencies.Add(MoveTemp(Dependency));
	}

	/** The host the tests resolve against: game 1.5.0, framework 0.1.0, SDK 1.5.0. */
	FModEnvironment MakeTestEnvironment()
	{
		FModEnvironment Environment;
		Environment.GameId = TestGameId;
		Environment.GameVersion = FModVersion(1, 5, 0);
		Environment.FrameworkVersion = FModVersion(0, 1, 0);
		Environment.SdkId = TestSdkId;
		Environment.SdkVersion = FModVersion(1, 5, 0);
		return Environment;
	}

	FModResolveRequest MakeTestRequest(TArray<FModManifest> Candidates)
	{
		FModResolveRequest Request;
		Request.Candidates = MoveTemp(Candidates);
		Request.Environment = MakeTestEnvironment();
		Request.bCheckEnvironment = true;
		return Request;
	}

	/** "a, b, c" - used so a failed ordering assertion prints the whole order, not just a count. */
	FString JoinIdArray(const TArray<FModId>& Ids)
	{
		FString Result;
		for (int32 Index = 0; Index < Ids.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += TEXT(", ");
			}

			Result += Ids[Index].ToString();
		}

		return Result;
	}

	/** A one-line fingerprint of every rejection, for the "same rejections in the same order" test. */
	FString FingerprintRejections(const TArray<FModRejection>& Rejections)
	{
		FString Result;
		for (const FModRejection& Rejection : Rejections)
		{
			Result += FString::Printf(TEXT("%s|%s|%s|%s\n"),
				*Rejection.ModId.ToString(),
				*ModFrameworkEnums::ToString(Rejection.Reason),
				*JoinIdArray(Rejection.RelatedMods),
				*Rejection.Message);
		}

		return Result;
	}

	const FModDiagnostic* FindDiagnostic(const TArray<FModDiagnostic>& Diagnostics, const TCHAR* Code)
	{
		const FName Wanted(Code);
		for (const FModDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Code == Wanted)
			{
				return &Diagnostic;
			}
		}

		return nullptr;
	}

	int32 CountDiagnostics(const TArray<FModDiagnostic>& Diagnostics, const TCHAR* Code)
	{
		const FName Wanted(Code);
		int32 Count = 0;
		for (const FModDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Code == Wanted)
			{
				++Count;
			}
		}

		return Count;
	}

	int32 IndexOfMod(const TArray<FModId>& Order, const TCHAR* InId)
	{
		return Order.IndexOfByKey(MakeTestId(InId));
	}

	/** Factorial for the tiny candidate counts the determinism tests use. */
	int32 SmallFactorial(int32 N)
	{
		int32 Result = 1;
		for (int32 Index = 2; Index <= N; ++Index)
		{
			Result *= Index;
		}

		return Result;
	}

	/**
	 * The PermutationIndex-th permutation of [0, Count), via the factorial number system.
	 *
	 * Written out rather than pulled from a shuffle helper on purpose: a determinism test that depends
	 * on a random generator proves nothing, and one that only tries the reversal misses the orderings
	 * that a hash-order bug actually produces.
	 */
	TArray<int32> IndexPermutation(int32 Count, int32 PermutationIndex)
	{
		TArray<int32> Available;
		Available.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Available.Add(Index);
		}

		TArray<int32> Result;
		Result.Reserve(Count);

		int32 Remaining = PermutationIndex;
		for (int32 Slot = 0; Slot < Count; ++Slot)
		{
			const int32 BlockSize = SmallFactorial(Count - Slot - 1);
			const int32 RawPick = BlockSize > 0 ? Remaining / BlockSize : 0;
			const int32 Pick = FMath::Clamp(RawPick, 0, Available.Num() - 1);

			Result.Add(Available[Pick]);
			Available.RemoveAt(Pick);
			Remaining -= Pick * BlockSize;
		}

		return Result;
	}

	/** Reorders Candidates by the PermutationIndex-th permutation of its indices. */
	TArray<FModManifest> PermuteCandidates(const TArray<FModManifest>& Candidates, int32 PermutationIndex)
	{
		const TArray<int32> Order = IndexPermutation(Candidates.Num(), PermutationIndex);

		TArray<FModManifest> Result;
		Result.Reserve(Candidates.Num());
		for (const int32 Index : Order)
		{
			Result.Add(Candidates[Index]);
		}

		return Result;
	}

	FModResourceClaim MakeTestClaim(const TCHAR* InModId, int32 InLoadOrder = INDEX_NONE, int32 InPriority = 0,
		EModConflictPolicy InPreferred = EModConflictPolicy::Error,
		const TCHAR* InPoint = TestPoint, const TCHAR* InResource = TestResource)
	{
		FModResourceClaim Claim;
		Claim.ModId = MakeTestId(InModId);
		Claim.ExtensionPointId = FName(InPoint);
		Claim.ResourceId = FName(InResource);
		Claim.LoadOrder = InLoadOrder;
		Claim.Priority = InPriority;
		Claim.PreferredPolicy = InPreferred;
		return Claim;
	}

	/** A policy table whose default is deliberately distinct from every enum default. */
	FModConflictPolicyTable MakeTestPolicies(EModConflictPolicy DefaultPolicy)
	{
		FModConflictPolicyTable Policies;
		Policies.DefaultPolicy = DefaultPolicy;
		return Policies;
	}
}

BEGIN_DEFINE_SPEC(FModDependencySpec, "ModFramework.Dependencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	/** TestTrue with the whole subject string in the failure text, so a regression shows what happened. */
	void ExpectContains(const TCHAR* What, const FString& Actual, const TCHAR* Needle);

	/** Compares a load order against a comma separated expectation, printing both sides on failure. */
	void ExpectOrder(const TCHAR* What, const TArray<FModId>& Actual, const FString& Expected);

	/** Asserts a mod was rejected for exactly the given reason and hands the rejection back. */
	const FModRejection* ExpectRejection(const TCHAR* What, const FModResolveResult& Result, const TCHAR* ModId,
		EModLoadFailureReason ExpectedReason);

	/** Asserts a diagnostic with the given code exists at the given severity and hands it back. */
	const FModDiagnostic* ExpectDiagnostic(const TCHAR* What, const FModResolveResult& Result, const TCHAR* Code,
		EModDiagnosticSeverity ExpectedSeverity);

	/** Resolves every permutation of Candidates and asserts each one produces Fingerprint. */
	void ExpectSameForEveryCandidateOrder(const TCHAR* What, const TArray<FModManifest>& Candidates,
		const TArray<FModId>& DisabledMods, TFunction<FString(const FModResolveResult&)> Fingerprint);

END_DEFINE_SPEC(FModDependencySpec)

void FModDependencySpec::ExpectContains(const TCHAR* What, const FString& Actual, const TCHAR* Needle)
{
	const FString FailureMessage = FString::Printf(
		TEXT("%s: expected the text to contain \"%s\", but it was \"%s\""), What, Needle, *Actual);

	TestTrue(*FailureMessage, Actual.Contains(Needle));
}

void FModDependencySpec::ExpectOrder(const TCHAR* What, const TArray<FModId>& Actual, const FString& Expected)
{
	const FString FailureMessage = FString::Printf(TEXT("%s: load order"), What);
	TestEqual(*FailureMessage, ModDependencyTestsPrivate::JoinIdArray(Actual), Expected);
}

const FModRejection* FModDependencySpec::ExpectRejection(const TCHAR* What, const FModResolveResult& Result,
	const TCHAR* ModId, EModLoadFailureReason ExpectedReason)
{
	const FModRejection* Rejection = Result.FindRejection(ModDependencyTestsPrivate::MakeTestId(ModId));

	const FString FailureMessage = FString::Printf(TEXT("%s: '%s' was rejected"), What, ModId);
	if (!TestTrue(*FailureMessage, Rejection != nullptr))
	{
		return nullptr;
	}

	const FString ReasonDescription = FString::Printf(TEXT("%s: rejection reason for '%s'"), What, ModId);
	TestEqual(*ReasonDescription,
		ModFrameworkEnums::ToString(Rejection->Reason), ModFrameworkEnums::ToString(ExpectedReason));

	return Rejection;
}

const FModDiagnostic* FModDependencySpec::ExpectDiagnostic(const TCHAR* What, const FModResolveResult& Result,
	const TCHAR* Code, EModDiagnosticSeverity ExpectedSeverity)
{
	const FModDiagnostic* Diagnostic = ModDependencyTestsPrivate::FindDiagnostic(Result.Diagnostics, Code);

	const FString FailureMessage = FString::Printf(TEXT("%s: a '%s' diagnostic was emitted"), What, Code);
	if (!TestTrue(*FailureMessage, Diagnostic != nullptr))
	{
		return nullptr;
	}

	const FString SeverityDescription = FString::Printf(TEXT("%s: severity of '%s'"), What, Code);
	TestEqual(*SeverityDescription,
		ModFrameworkEnums::ToString(Diagnostic->Severity), ModFrameworkEnums::ToString(ExpectedSeverity));

	return Diagnostic;
}

void FModDependencySpec::ExpectSameForEveryCandidateOrder(const TCHAR* What, const TArray<FModManifest>& Candidates,
	const TArray<FModId>& DisabledMods, TFunction<FString(const FModResolveResult&)> Fingerprint)
{
	using namespace ModDependencyTestsPrivate;

	FModResolveRequest BaselineRequest = MakeTestRequest(Candidates);
	BaselineRequest.DisabledMods = DisabledMods;

	const FString Baseline = Fingerprint(FModDependencyResolver::Resolve(BaselineRequest));

	const int32 NumPermutations = SmallFactorial(Candidates.Num());

	int32 MismatchCount = 0;
	int32 FirstMismatchIndex = INDEX_NONE;
	FString FirstMismatch;

	for (int32 PermutationIndex = 0; PermutationIndex < NumPermutations; ++PermutationIndex)
	{
		FModResolveRequest Request = MakeTestRequest(PermuteCandidates(Candidates, PermutationIndex));
		Request.DisabledMods = DisabledMods;

		const FString Actual = Fingerprint(FModDependencyResolver::Resolve(Request));
		if (Actual != Baseline)
		{
			++MismatchCount;
			if (FirstMismatchIndex == INDEX_NONE)
			{
				FirstMismatchIndex = PermutationIndex;
				FirstMismatch = Actual;
			}
		}
	}

	const FString FailureMessage = FString::Printf(
		TEXT("%s: number of candidate orderings (out of %d) that produced a different result"),
		What, NumPermutations);

	TestEqual(*FailureMessage, MismatchCount, 0);

	if (MismatchCount > 0)
	{
		AddError(FString::Printf(
			TEXT("%s: permutation %d produced\n%s\nbut the unpermuted input produced\n%s"),
			What, FirstMismatchIndex, *FirstMismatch, *Baseline));
	}
}

void FModDependencySpec::Define()
{
	using namespace ModDependencyTestsPrivate;

	//~ -------------------------------------------------------------------------------------------
	//~ Load order
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Load order"), [this]()
	{
		It(TEXT("resolves a linear chain so that every dependency precedes its dependent"), [this]()
		{
			FModManifest Base = MakeTestManifest(TEXT("chain.base"));

			FModManifest Middle = MakeTestManifest(TEXT("chain.middle"));
			AddTestDependency(Middle, TEXT("chain.base"));

			FModManifest Top = MakeTestManifest(TEXT("chain.top"));
			AddTestDependency(Top, TEXT("chain.middle"));

			// Handed to the resolver in the exact reverse of the required order.
			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Top, Middle, Base }));

			TestTrue(TEXT("linear chain: the resolve succeeded"), Result.bSuccess);
			TestEqual(TEXT("linear chain: nothing was rejected"), Result.Rejections.Num(), 0);
			ExpectOrder(TEXT("linear chain"), Result.LoadOrder,
				TEXT("chain.base, chain.middle, chain.top"));
		});

		It(TEXT("resolves a diamond so that the shared base loads first and the tip loads last"), [this]()
		{
			FModManifest Base = MakeTestManifest(TEXT("diamond.base"));

			FModManifest Left = MakeTestManifest(TEXT("diamond.left"));
			AddTestDependency(Left, TEXT("diamond.base"));

			FModManifest Right = MakeTestManifest(TEXT("diamond.right"));
			AddTestDependency(Right, TEXT("diamond.base"));

			FModManifest Top = MakeTestManifest(TEXT("diamond.top"));
			AddTestDependency(Top, TEXT("diamond.left"));
			AddTestDependency(Top, TEXT("diamond.right"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Top, Right, Left, Base }));

			TestTrue(TEXT("diamond: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("diamond"), Result.LoadOrder,
				TEXT("diamond.base, diamond.left, diamond.right, diamond.top"));

			// The topological property itself, independently of the exact tie-break above.
			const int32 BaseIndex = IndexOfMod(Result.LoadOrder, TEXT("diamond.base"));
			const int32 LeftIndex = IndexOfMod(Result.LoadOrder, TEXT("diamond.left"));
			const int32 RightIndex = IndexOfMod(Result.LoadOrder, TEXT("diamond.right"));
			const int32 TopIndex = IndexOfMod(Result.LoadOrder, TEXT("diamond.top"));

			TestTrue(TEXT("diamond: base loads before left"), BaseIndex >= 0 && BaseIndex < LeftIndex);
			TestTrue(TEXT("diamond: base loads before right"), BaseIndex >= 0 && BaseIndex < RightIndex);
			TestTrue(TEXT("diamond: left loads before top"), LeftIndex >= 0 && LeftIndex < TopIndex);
			TestTrue(TEXT("diamond: right loads before top"), RightIndex >= 0 && RightIndex < TopIndex);
		});

		It(TEXT("puts a higher priority first among mods the graph leaves unordered"), [this]()
		{
			FModManifest High = MakeTestManifest(TEXT("prio.high"));
			High.Priority = 10;

			FModManifest Low = MakeTestManifest(TEXT("prio.low"));
			Low.Priority = -5;

			FModManifest Mid = MakeTestManifest(TEXT("prio.mid"));
			Mid.Priority = 0;

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Low, Mid, High }));

			// Alphabetically these sort high, low, mid - so this genuinely tests the priority key and
			// not the id fallback underneath it.
			ExpectOrder(TEXT("priority tie-break"), Result.LoadOrder,
				TEXT("prio.high, prio.mid, prio.low"));
		});

		It(TEXT("falls back to ascending mod id when priorities are equal"), [this]()
		{
			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({
				MakeTestManifest(TEXT("tie.c")),
				MakeTestManifest(TEXT("tie.a")),
				MakeTestManifest(TEXT("tie.b")) }));

			ExpectOrder(TEXT("equal priority"), Result.LoadOrder, TEXT("tie.a, tie.b, tie.c"));
		});

		It(TEXT("lets priority lose to a dependency edge"), [this]()
		{
			// A dependency is a hard constraint; priority only orders what the graph does not.
			FModManifest Library = MakeTestManifest(TEXT("hard.library"));
			Library.Priority = -1000;

			FModManifest Consumer = MakeTestManifest(TEXT("hard.consumer"));
			Consumer.Priority = 1000;
			AddTestDependency(Consumer, TEXT("hard.library"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Consumer, Library }));

			ExpectOrder(TEXT("dependency beats priority"), Result.LoadOrder,
				TEXT("hard.library, hard.consumer"));
		});

		It(TEXT("honours loadBefore and loadAfter between mods with no dependency edge"), [this]()
		{
			FModManifest First = MakeTestManifest(TEXT("order.first"));
			First.LoadBefore.Add(MakeTestId(TEXT("order.last")));

			FModManifest Middle = MakeTestManifest(TEXT("order.middle"));
			Middle.LoadAfter.Add(MakeTestId(TEXT("order.first")));
			Middle.LoadBefore.Add(MakeTestId(TEXT("order.last")));

			FModManifest Last = MakeTestManifest(TEXT("order.last"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Last, Middle, First }));

			TestTrue(TEXT("ordering edges: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("ordering edges"), Result.LoadOrder,
				TEXT("order.first, order.middle, order.last"));
		});

		It(TEXT("drops an ordering entry that names a mod nobody installed and says so"), [this]()
		{
			FModManifest Only = MakeTestManifest(TEXT("orphan.only"));
			Only.LoadAfter.Add(MakeTestId(TEXT("orphan.ghost")));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Only }));

			TestTrue(TEXT("ignored ordering: the resolve still succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("ignored ordering"), Result.LoadOrder, TEXT("orphan.only"));

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("ignored ordering"), Result,
				TEXT("Resolve.OrderingIgnored"), EModDiagnosticSeverity::Info);
			if (Diagnostic != nullptr)
			{
				TestEqual(TEXT("ignored ordering: diagnostic context names the absent mod"),
					Diagnostic->Context, FString(TEXT("orphan.ghost")));
				TestEqual(TEXT("ignored ordering: diagnostic is attributed to the declaring mod"),
					Diagnostic->ModId.ToString(), FString(TEXT("orphan.only")));
				ExpectContains(TEXT("ignored ordering"), Diagnostic->Message, TEXT("loadAfter 'orphan.ghost'"));
				ExpectContains(TEXT("ignored ordering"), Diagnostic->Message, TEXT("is not installed"));
			}
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Determinism
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Determinism"), [this]()
	{
		// DOCUMENTED GUARANTEE - do not "simplify" these tests down to a single ordering.
		//
		// FModDependencyResolver promises that the resolved load order depends only on the SET of
		// candidates, never on the order discovery happened to hand them over. That is what makes a
		// mod list reproducible across machines and what a player's save relies on. It is also the
		// single easiest property to break: one TMap iterated instead of the sorted id array, one
		// unstable sort, and the order silently starts depending on hash layout. These tests resolve
		// EVERY permutation of the candidate array and demand byte-identical output.

		It(TEXT("produces the identical load order for all 24 orderings of a diamond graph"), [this]()
		{
			FModManifest Base = MakeTestManifest(TEXT("diamond.base"));

			FModManifest Left = MakeTestManifest(TEXT("diamond.left"));
			AddTestDependency(Left, TEXT("diamond.base"));

			FModManifest Right = MakeTestManifest(TEXT("diamond.right"));
			AddTestDependency(Right, TEXT("diamond.base"));

			FModManifest Top = MakeTestManifest(TEXT("diamond.top"));
			AddTestDependency(Top, TEXT("diamond.left"));
			AddTestDependency(Top, TEXT("diamond.right"));

			ExpectSameForEveryCandidateOrder(TEXT("diamond determinism"), { Base, Left, Right, Top },
				TArray<FModId>(), [](const FModResolveResult& Result)
				{
					return JoinIdArray(Result.LoadOrder);
				});
		});

		It(TEXT("produces the identical load order for all 120 orderings of a mixed graph"), [this]()
		{
			// Required dependencies, an optional dependency, an ordering edge and two priorities all
			// at once: every input the ready-set comparison looks at.
			FModManifest Core = MakeTestManifest(TEXT("det.core"));
			Core.Priority = 10;

			FModManifest Alpha = MakeTestManifest(TEXT("det.alpha"));
			AddTestDependency(Alpha, TEXT("det.core"));

			FModManifest Beta = MakeTestManifest(TEXT("det.beta"));
			AddTestDependency(Beta, TEXT("det.core"));
			Beta.LoadAfter.Add(MakeTestId(TEXT("det.alpha")));

			FModManifest Gamma = MakeTestManifest(TEXT("det.gamma"));
			AddTestDependency(Gamma, TEXT("det.beta"), TEXT("*"), /*bOptional*/ true);

			FModManifest Delta = MakeTestManifest(TEXT("det.delta"));
			Delta.Priority = 5;
			Delta.LoadBefore.Add(MakeTestId(TEXT("det.gamma")));

			const TArray<FModManifest> Candidates = { Core, Alpha, Beta, Gamma, Delta };

			const FModResolveResult Baseline = FModDependencyResolver::Resolve(MakeTestRequest(Candidates));
			TestTrue(TEXT("mixed determinism: the baseline resolve succeeded"), Baseline.bSuccess);
			ExpectOrder(TEXT("mixed determinism baseline"), Baseline.LoadOrder,
				TEXT("det.core, det.delta, det.alpha, det.beta, det.gamma"));

			ExpectSameForEveryCandidateOrder(TEXT("mixed determinism"), Candidates, TArray<FModId>(),
				[](const FModResolveResult& Result)
				{
					return JoinIdArray(Result.LoadOrder);
				});
		});

		It(TEXT("reports the same rejections, in the same order, for every candidate ordering"), [this]()
		{
			// Rejections are shown to players and diffed between sessions, so their order and their
			// wording are part of the guarantee too, not just the load order.
			FModManifest Ok = MakeTestManifest(TEXT("stable.ok"));

			FModManifest Needy = MakeTestManifest(TEXT("stable.needy"));
			AddTestDependency(Needy, TEXT("stable.absent"), TEXT(">=1.0.0"));

			FModManifest CycleOne = MakeTestManifest(TEXT("stable.cyc1"));
			AddTestDependency(CycleOne, TEXT("stable.cyc2"));

			FModManifest CycleTwo = MakeTestManifest(TEXT("stable.cyc2"));
			AddTestDependency(CycleTwo, TEXT("stable.cyc1"));

			const TArray<FModManifest> Candidates = { Ok, Needy, CycleOne, CycleTwo };

			const FModResolveResult Baseline = FModDependencyResolver::Resolve(MakeTestRequest(Candidates));
			TestEqual(TEXT("rejection determinism: baseline rejection count"), Baseline.Rejections.Num(), 3);
			ExpectOrder(TEXT("rejection determinism baseline"), Baseline.LoadOrder, TEXT("stable.ok"));

			ExpectSameForEveryCandidateOrder(TEXT("rejection determinism"), Candidates, TArray<FModId>(),
				[](const FModResolveResult& Result)
				{
					return JoinIdArray(Result.LoadOrder) + TEXT("\n") + FingerprintRejections(Result.Rejections);
				});
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Dependency rejection
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Dependencies"), [this]()
	{
		It(TEXT("rejects a mod whose required dependency is not installed"), [this]()
		{
			FModManifest Needy = MakeTestManifest(TEXT("req.needy"));
			AddTestDependency(Needy, TEXT("req.missing"), TEXT(">=2.0.0"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Needy }));

			TestFalse(TEXT("missing dependency: the resolve failed"), Result.bSuccess);
			ExpectOrder(TEXT("missing dependency"), Result.LoadOrder, FString());

			const FModRejection* Rejection = ExpectRejection(TEXT("missing dependency"), Result,
				TEXT("req.needy"), EModLoadFailureReason::MissingDependency);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("missing dependency: message"), Rejection->Message,
					FString(TEXT("Mod 'req.needy' requires 'req.missing' >=2.0.0, but it is not installed.")));
				TestEqual(TEXT("missing dependency: related mods"),
					JoinIdArray(Rejection->RelatedMods), FString(TEXT("req.missing")));
			}

			ExpectDiagnostic(TEXT("missing dependency"), Result,
				TEXT("Resolve.MissingDependency"), EModDiagnosticSeverity::Error);
		});

		It(TEXT("rejects a version-incompatible dependency and names both the wanted range and the installed version"), [this]()
		{
			FModManifest Library = MakeTestManifest(TEXT("ver.lib"), TEXT("1.0.0"));

			FModManifest App = MakeTestManifest(TEXT("ver.app"));
			AddTestDependency(App, TEXT("ver.lib"), TEXT(">=2.0.0"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Library, App }));

			TestFalse(TEXT("version conflict: the resolve failed"), Result.bSuccess);

			// The library itself is blameless and still loads.
			ExpectOrder(TEXT("version conflict"), Result.LoadOrder, TEXT("ver.lib"));

			const FModRejection* Rejection = ExpectRejection(TEXT("version conflict"), Result,
				TEXT("ver.app"), EModLoadFailureReason::IncompatibleDependencyVersion);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("version conflict: message"), Rejection->Message,
					FString(TEXT("Mod 'ver.app' requires 'ver.lib' >=2.0.0, but version 1.0.0 is installed.")));

				// Both versions have to be in the sentence: a player cannot act on "incompatible".
				ExpectContains(TEXT("version conflict"), Rejection->Message, TEXT(">=2.0.0"));
				ExpectContains(TEXT("version conflict"), Rejection->Message, TEXT("1.0.0"));
			}

			ExpectDiagnostic(TEXT("version conflict"), Result,
				TEXT("Resolve.VersionConflict"), EModDiagnosticSeverity::Error);
		});

		It(TEXT("appends the author's reason to an unsatisfiable dependency"), [this]()
		{
			FModManifest App = MakeTestManifest(TEXT("reason.app"));
			AddTestDependency(App, TEXT("reason.missing"), TEXT("*"), /*bOptional*/ false,
				TEXT("Adds the settings page."));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ App }));

			const FModRejection* Rejection = ExpectRejection(TEXT("dependency reason"), Result,
				TEXT("reason.app"), EModLoadFailureReason::MissingDependency);
			if (Rejection != nullptr)
			{
				ExpectContains(TEXT("dependency reason"), Rejection->Message,
					TEXT("The mod author notes: \"Adds the settings page.\""));

				// An unconstrained range must not be printed as an empty string.
				ExpectContains(TEXT("dependency reason"), Rejection->Message,
					TEXT("'reason.missing' (any version)"));
			}
		});

		It(TEXT("names a mod by its display name and its id when it has one"), [this]()
		{
			FModManifest App = MakeTestManifest(TEXT("named.app"));
			App.DisplayName = TEXT("Better Combat");
			AddTestDependency(App, TEXT("named.missing"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ App }));

			const FModRejection* Rejection = ExpectRejection(TEXT("display name"), Result,
				TEXT("named.app"), EModLoadFailureReason::MissingDependency);
			if (Rejection != nullptr)
			{
				ExpectContains(TEXT("display name"), Rejection->Message, TEXT("Mod 'Better Combat' (named.app)"));
			}
		});

		It(TEXT("ignores a dependency entry with no id and warns instead of rejecting"), [this]()
		{
			// Untrusted input: an empty "id" in mod.json must never take the whole mod down.
			FModManifest App = MakeTestManifest(TEXT("noid.app"));
			App.Dependencies.AddDefaulted();

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ App }));

			ExpectOrder(TEXT("empty dependency id"), Result.LoadOrder, TEXT("noid.app"));
			TestEqual(TEXT("empty dependency id: nothing was rejected"), Result.Rejections.Num(), 0);

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("empty dependency id"), Result,
				TEXT("Resolve.MissingDependency"), EModDiagnosticSeverity::Warning);
			if (Diagnostic != nullptr)
			{
				ExpectContains(TEXT("empty dependency id"), Diagnostic->Message,
					TEXT("declares a dependency with no id"));
			}
		});

		It(TEXT("treats a malformed dependency range as any version and warns"), [this]()
		{
			FModManifest Library = MakeTestManifest(TEXT("bad.lib"), TEXT("1.0.0"));

			FModManifest App = MakeTestManifest(TEXT("bad.app"));
			AddTestDependency(App, TEXT("bad.lib"), TEXT("totally not a range"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Library, App }));

			// Punishing a player for a mod author's typo helps nobody: both mods still load.
			ExpectOrder(TEXT("malformed range"), Result.LoadOrder, TEXT("bad.lib, bad.app"));
			TestEqual(TEXT("malformed range: nothing was rejected"), Result.Rejections.Num(), 0);

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("malformed range"), Result,
				TEXT("Resolve.VersionConflict"), EModDiagnosticSeverity::Warning);
			if (Diagnostic != nullptr)
			{
				ExpectContains(TEXT("malformed range"), Diagnostic->Message, TEXT("not valid range syntax"));
			}
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Optional dependencies
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Optional dependencies"), [this]()
	{
		// DOCUMENTED GUARANTEE: an unsatisfied optional dependency never rejects anything and never
		// downgrades the resolve to a failure. It exists so a mod can light up extra behaviour when a
		// companion mod happens to be present; turning it into an error would make it useless.

		It(TEXT("emits an info diagnostic and still loads the mod when an optional dependency is absent"), [this]()
		{
			FModManifest App = MakeTestManifest(TEXT("opt.app"));
			AddTestDependency(App, TEXT("opt.absent"), TEXT("*"), /*bOptional*/ true,
				TEXT("Adds a settings page."));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ App }));

			TestTrue(TEXT("optional absent: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("optional absent"), Result.LoadOrder, TEXT("opt.app"));
			TestEqual(TEXT("optional absent: nothing was rejected"), Result.Rejections.Num(), 0);
			TestEqual(TEXT("optional absent: exactly one diagnostic"), Result.Diagnostics.Num(), 1);

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("optional absent"), Result,
				TEXT("Resolve.OptionalMissing"), EModDiagnosticSeverity::Info);
			if (Diagnostic != nullptr)
			{
				TestEqual(TEXT("optional absent: diagnostic context names the dependency"),
					Diagnostic->Context, FString(TEXT("opt.absent")));
				ExpectContains(TEXT("optional absent"), Diagnostic->Message, TEXT("is not installed"));
				ExpectContains(TEXT("optional absent"), Diagnostic->Message, TEXT("it will load without it"));
				ExpectContains(TEXT("optional absent"), Diagnostic->Message,
					TEXT("The mod author notes: \"Adds a settings page.\""));
			}
		});

		It(TEXT("loads a mod whose optional dependency is installed at an unwanted version"), [this]()
		{
			FModManifest Library = MakeTestManifest(TEXT("optver.lib"), TEXT("1.0.0"));

			FModManifest App = MakeTestManifest(TEXT("optver.app"));
			AddTestDependency(App, TEXT("optver.lib"), TEXT(">=2.0.0"), /*bOptional*/ true);

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ App, Library }));

			TestTrue(TEXT("optional wrong version: the resolve succeeded"), Result.bSuccess);

			// Present means ordered even when the version is not the one the dependent hoped for.
			ExpectOrder(TEXT("optional wrong version"), Result.LoadOrder, TEXT("optver.lib, optver.app"));

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("optional wrong version"), Result,
				TEXT("Resolve.OptionalMissing"), EModDiagnosticSeverity::Info);
			if (Diagnostic != nullptr)
			{
				ExpectContains(TEXT("optional wrong version"), Diagnostic->Message,
					TEXT("but version 1.0.0 is installed"));
			}
		});

		It(TEXT("does not cascade a rejection through an optional dependency"), [this]()
		{
			FModManifest Broken = MakeTestManifest(TEXT("optcascade.broken"));
			Broken.Game.GameId = TEXT("some.other.game");

			FModManifest App = MakeTestManifest(TEXT("optcascade.app"));
			AddTestDependency(App, TEXT("optcascade.broken"), TEXT("*"), /*bOptional*/ true);

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Broken, App }));

			ExpectOrder(TEXT("optional cascade"), Result.LoadOrder, TEXT("optcascade.app"));
			TestEqual(TEXT("optional cascade: only the broken mod was rejected"), Result.Rejections.Num(), 1);
			ExpectRejection(TEXT("optional cascade"), Result,
				TEXT("optcascade.broken"), EModLoadFailureReason::IncompatibleGame);

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("optional cascade"), Result,
				TEXT("Resolve.OptionalMissing"), EModDiagnosticSeverity::Info);
			if (Diagnostic != nullptr)
			{
				ExpectContains(TEXT("optional cascade"), Diagnostic->Message,
					TEXT("is installed but will not load"));
			}
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Cascading rejection
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Cascading rejection"), [this]()
	{
		It(TEXT("carries a rejection transitively down the whole dependency chain"), [this]()
		{
			// a -> b -> c -> (absent). Rejecting c has to take b down on one pass and a on the next,
			// and every message has to end at the ROOT cause rather than at the mod next door.
			FModManifest A = MakeTestManifest(TEXT("cascade.a"));
			AddTestDependency(A, TEXT("cascade.b"));

			FModManifest B = MakeTestManifest(TEXT("cascade.b"));
			AddTestDependency(B, TEXT("cascade.c"));

			FModManifest C = MakeTestManifest(TEXT("cascade.c"));
			AddTestDependency(C, TEXT("cascade.absent"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ A, B, C }));

			TestFalse(TEXT("cascade: the resolve failed"), Result.bSuccess);
			ExpectOrder(TEXT("cascade"), Result.LoadOrder, FString());
			TestEqual(TEXT("cascade: every mod in the chain was rejected"), Result.Rejections.Num(), 3);

			ExpectRejection(TEXT("cascade root"), Result,
				TEXT("cascade.c"), EModLoadFailureReason::MissingDependency);

			const FModRejection* MiddleRejection = ExpectRejection(TEXT("cascade middle"), Result,
				TEXT("cascade.b"), EModLoadFailureReason::DependencyFailed);
			if (MiddleRejection != nullptr)
			{
				TestEqual(TEXT("cascade middle: related mods"),
					JoinIdArray(MiddleRejection->RelatedMods), FString(TEXT("cascade.c")));
				ExpectContains(TEXT("cascade middle"), MiddleRejection->Message,
					TEXT("Dependency chain: cascade.b -> cascade.c."));
			}

			const FModRejection* TopRejection = ExpectRejection(TEXT("cascade top"), Result,
				TEXT("cascade.a"), EModLoadFailureReason::DependencyFailed);
			if (TopRejection != nullptr)
			{
				// RelatedMods runs from the immediate dependency down to the root cause, which is
				// always the last element - a mod browser links the whole chain from this.
				TestEqual(TEXT("cascade top: related mods"),
					JoinIdArray(TopRejection->RelatedMods), FString(TEXT("cascade.b, cascade.c")));
				ExpectContains(TEXT("cascade top"), TopRejection->Message,
					TEXT("Dependency chain: cascade.a -> cascade.b -> cascade.c."));
				ExpectContains(TEXT("cascade top"), TopRejection->Message, TEXT("Root cause:"));
				ExpectContains(TEXT("cascade top"), TopRejection->Message, TEXT("cascade.absent"));
			}

			TestEqual(TEXT("cascade: two cascade diagnostics were emitted"),
				CountDiagnostics(Result.Diagnostics, TEXT("Resolve.CascadeReject")), 2);
		});

		It(TEXT("cascades through a mod the player disabled"), [this]()
		{
			FModManifest Library = MakeTestManifest(TEXT("dis.lib"));

			FModManifest App = MakeTestManifest(TEXT("dis.app"));
			AddTestDependency(App, TEXT("dis.lib"));

			FModResolveRequest Request = MakeTestRequest({ Library, App });
			Request.DisabledMods.Add(MakeTestId(TEXT("dis.lib")));

			const FModResolveResult Result = FModDependencyResolver::Resolve(Request);

			ExpectOrder(TEXT("disabled cascade"), Result.LoadOrder, FString());

			// Switching a mod off is not a failure; losing a mod because of it is.
			TestFalse(TEXT("disabled cascade: the resolve failed because a dependent was orphaned"),
				Result.bSuccess);

			ExpectRejection(TEXT("disabled cascade"), Result, TEXT("dis.lib"), EModLoadFailureReason::Disabled);

			const FModRejection* AppRejection = ExpectRejection(TEXT("disabled cascade"), Result,
				TEXT("dis.app"), EModLoadFailureReason::DependencyFailed);
			if (AppRejection != nullptr)
			{
				ExpectContains(TEXT("disabled cascade"), AppRejection->Message, TEXT("Root cause:"));
				ExpectContains(TEXT("disabled cascade"), AppRejection->Message, TEXT("is disabled"));
			}
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Cycles
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Cycles"), [this]()
	{
		// Graph edges point "loads earlier -> loads later", so a mod that DEPENDS on another produces
		// an edge FROM the dependency TO itself. The cycle strings below follow those edges, which is
		// why they read in the opposite direction from the dependency declarations. Each cycle is
		// reported in canonical rotation: it starts at the lexicographically smallest member and does
		// not repeat it at the end. Do not "fix" the expected strings to match declaration order.

		It(TEXT("rejects both members of a two-node cycle and names the cycle path"), [this]()
		{
			FModManifest A = MakeTestManifest(TEXT("cycle2.a"));
			AddTestDependency(A, TEXT("cycle2.b"));

			FModManifest B = MakeTestManifest(TEXT("cycle2.b"));
			AddTestDependency(B, TEXT("cycle2.a"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ A, B }));

			TestFalse(TEXT("two-node cycle: the resolve failed"), Result.bSuccess);
			ExpectOrder(TEXT("two-node cycle"), Result.LoadOrder, FString());
			TestEqual(TEXT("two-node cycle: both members were rejected"), Result.Rejections.Num(), 2);

			const FModRejection* First = ExpectRejection(TEXT("two-node cycle"), Result,
				TEXT("cycle2.a"), EModLoadFailureReason::CircularDependency);
			if (First != nullptr)
			{
				TestEqual(TEXT("two-node cycle: message"), First->Message, FString(
					TEXT("Mod 'cycle2.a' is part of a dependency cycle and cannot be loaded. ")
					TEXT("Dependency cycle: cycle2.a -> cycle2.b -> cycle2.a")));
				TestEqual(TEXT("two-node cycle: related mods hold the cycle"),
					JoinIdArray(First->RelatedMods), FString(TEXT("cycle2.a, cycle2.b")));
			}

			const FModRejection* Second = ExpectRejection(TEXT("two-node cycle"), Result,
				TEXT("cycle2.b"), EModLoadFailureReason::CircularDependency);
			if (Second != nullptr)
			{
				ExpectContains(TEXT("two-node cycle"), Second->Message,
					TEXT("Dependency cycle: cycle2.a -> cycle2.b -> cycle2.a"));
			}

			TestEqual(TEXT("two-node cycle: two cycle diagnostics"),
				CountDiagnostics(Result.Diagnostics, TEXT("Resolve.Cycle")), 2);
		});

		It(TEXT("rejects every member of a three-node cycle and names the cycle path"), [this]()
		{
			// a depends on b, b depends on c, c depends on a. Following the load edges from the
			// smallest member gives a -> c -> b -> a.
			FModManifest A = MakeTestManifest(TEXT("cycle3.a"));
			AddTestDependency(A, TEXT("cycle3.b"));

			FModManifest B = MakeTestManifest(TEXT("cycle3.b"));
			AddTestDependency(B, TEXT("cycle3.c"));

			FModManifest C = MakeTestManifest(TEXT("cycle3.c"));
			AddTestDependency(C, TEXT("cycle3.a"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ C, A, B }));

			TestFalse(TEXT("three-node cycle: the resolve failed"), Result.bSuccess);
			ExpectOrder(TEXT("three-node cycle"), Result.LoadOrder, FString());
			TestEqual(TEXT("three-node cycle: all three members were rejected"), Result.Rejections.Num(), 3);

			for (const TCHAR* Member : { TEXT("cycle3.a"), TEXT("cycle3.b"), TEXT("cycle3.c") })
			{
				const FModRejection* Rejection = ExpectRejection(TEXT("three-node cycle"), Result, Member,
					EModLoadFailureReason::CircularDependency);
				if (Rejection != nullptr)
				{
					ExpectContains(TEXT("three-node cycle"), Rejection->Message,
						TEXT("Dependency cycle: cycle3.a -> cycle3.c -> cycle3.b -> cycle3.a"));
					TestEqual(TEXT("three-node cycle: related mods hold the canonical rotation"),
						JoinIdArray(Rejection->RelatedMods), FString(TEXT("cycle3.a, cycle3.c, cycle3.b")));
				}
			}
		});

		It(TEXT("rejects a mod that declares a dependency on itself"), [this]()
		{
			FModManifest Solo = MakeTestManifest(TEXT("self.mod"));
			AddTestDependency(Solo, TEXT("self.mod"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Solo }));

			TestFalse(TEXT("self dependency: the resolve failed"), Result.bSuccess);
			ExpectOrder(TEXT("self dependency"), Result.LoadOrder, FString());

			const FModRejection* Rejection = ExpectRejection(TEXT("self dependency"), Result,
				TEXT("self.mod"), EModLoadFailureReason::CircularDependency);
			if (Rejection != nullptr)
			{
				ExpectContains(TEXT("self dependency"), Rejection->Message,
					TEXT("Dependency cycle: self.mod -> self.mod"));
				TestEqual(TEXT("self dependency: related mods"),
					JoinIdArray(Rejection->RelatedMods), FString(TEXT("self.mod")));
			}
		});

		It(TEXT("rejects an ordering cycle built only from loadBefore"), [this]()
		{
			FModManifest A = MakeTestManifest(TEXT("ordcycle.a"));
			A.LoadBefore.Add(MakeTestId(TEXT("ordcycle.b")));

			FModManifest B = MakeTestManifest(TEXT("ordcycle.b"));
			B.LoadBefore.Add(MakeTestId(TEXT("ordcycle.a")));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ A, B }));

			TestFalse(TEXT("loadBefore cycle: the resolve failed"), Result.bSuccess);
			ExpectOrder(TEXT("loadBefore cycle"), Result.LoadOrder, FString());
			TestEqual(TEXT("loadBefore cycle: both members were rejected"), Result.Rejections.Num(), 2);

			const FModRejection* Rejection = ExpectRejection(TEXT("loadBefore cycle"), Result,
				TEXT("ordcycle.a"), EModLoadFailureReason::CircularDependency);
			if (Rejection != nullptr)
			{
				ExpectContains(TEXT("loadBefore cycle"), Rejection->Message,
					TEXT("Dependency cycle: ordcycle.a -> ordcycle.b -> ordcycle.a"));
			}
		});

		It(TEXT("rejects an ordering cycle a single mod creates with contradictory loadBefore and loadAfter"), [this]()
		{
			// The innocent party is rejected too: the framework has no way to know which half of the
			// contradiction the author meant.
			FModManifest X = MakeTestManifest(TEXT("contra.x"));
			X.LoadBefore.Add(MakeTestId(TEXT("contra.y")));
			X.LoadAfter.Add(MakeTestId(TEXT("contra.y")));

			FModManifest Y = MakeTestManifest(TEXT("contra.y"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ X, Y }));

			ExpectOrder(TEXT("contradictory ordering"), Result.LoadOrder, FString());
			ExpectRejection(TEXT("contradictory ordering"), Result,
				TEXT("contra.x"), EModLoadFailureReason::CircularDependency);
			ExpectRejection(TEXT("contradictory ordering"), Result,
				TEXT("contra.y"), EModLoadFailureReason::CircularDependency);
		});

		It(TEXT("cascades to a mod that depended on a cycle member"), [this]()
		{
			FModManifest A = MakeTestManifest(TEXT("cyc.a"));
			AddTestDependency(A, TEXT("cyc.b"));

			FModManifest B = MakeTestManifest(TEXT("cyc.b"));
			AddTestDependency(B, TEXT("cyc.a"));

			FModManifest User = MakeTestManifest(TEXT("cyc.user"));
			AddTestDependency(User, TEXT("cyc.a"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ A, B, User }));

			ExpectOrder(TEXT("cycle cascade"), Result.LoadOrder, FString());
			TestEqual(TEXT("cycle cascade: three rejections"), Result.Rejections.Num(), 3);

			ExpectRejection(TEXT("cycle cascade"), Result, TEXT("cyc.a"), EModLoadFailureReason::CircularDependency);
			ExpectRejection(TEXT("cycle cascade"), Result, TEXT("cyc.b"), EModLoadFailureReason::CircularDependency);

			const FModRejection* UserRejection = ExpectRejection(TEXT("cycle cascade"), Result,
				TEXT("cyc.user"), EModLoadFailureReason::DependencyFailed);
			if (UserRejection != nullptr)
			{
				TestEqual(TEXT("cycle cascade: related mods"),
					JoinIdArray(UserRejection->RelatedMods), FString(TEXT("cyc.a")));
				ExpectContains(TEXT("cycle cascade"), UserRejection->Message, TEXT("Root cause:"));
			}
		});

		It(TEXT("leaves a mod outside the cycle untouched"), [this]()
		{
			FModManifest A = MakeTestManifest(TEXT("bystand.a"));
			AddTestDependency(A, TEXT("bystand.b"));

			FModManifest B = MakeTestManifest(TEXT("bystand.b"));
			AddTestDependency(B, TEXT("bystand.a"));

			FModManifest Innocent = MakeTestManifest(TEXT("bystand.innocent"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ A, Innocent, B }));

			ExpectOrder(TEXT("bystander"), Result.LoadOrder, TEXT("bystand.innocent"));
			TestEqual(TEXT("bystander: only the cycle members were rejected"), Result.Rejections.Num(), 2);
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Duplicates and disabled mods
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Duplicates"), [this]()
	{
		// DOCUMENTED GUARANTEE: when the same id is discovered more than once the HIGHEST version
		// wins, whatever order discovery produced. A player who drops a newer copy of a mod into a
		// second search directory must get the newer copy, not whichever folder was scanned first.

		It(TEXT("keeps the highest version when the newer copy is discovered second"), [this]()
		{
			FModManifest Old = MakeTestManifest(TEXT("dup.lib"), TEXT("1.0.0"));
			FModManifest New = MakeTestManifest(TEXT("dup.lib"), TEXT("2.0.0"));

			FModManifest App = MakeTestManifest(TEXT("dup.app"));
			AddTestDependency(App, TEXT("dup.lib"), TEXT(">=2.0.0"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Old, New, App }));

			// The dependent loading at all is the proof that 2.0.0 is the copy that was kept.
			ExpectOrder(TEXT("duplicate ids"), Result.LoadOrder, TEXT("dup.lib, dup.app"));

			const FModRejection* Rejection = ExpectRejection(TEXT("duplicate ids"), Result,
				TEXT("dup.lib"), EModLoadFailureReason::DuplicateModId);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("duplicate ids: message"), Rejection->Message, FString(
					TEXT("Mod 'dup.lib' version 1.0.0 is a duplicate: another copy of 'dup.lib' ")
					TEXT("at version 2.0.0 was found and takes precedence.")));
			}

			// A duplicate is a hard rejection, so the resolve reports failure even though the id is
			// still in the load order. Anything that wants to know whether a mod will load must look
			// at LoadOrder, not at the rejection list.
			TestFalse(TEXT("duplicate ids: the resolve reports failure"), Result.bSuccess);
		});

		It(TEXT("keeps the highest version when the newer copy is discovered first"), [this]()
		{
			FModManifest Old = MakeTestManifest(TEXT("dup.lib"), TEXT("1.0.0"));
			FModManifest New = MakeTestManifest(TEXT("dup.lib"), TEXT("2.0.0"));

			FModManifest App = MakeTestManifest(TEXT("dup.app"));
			AddTestDependency(App, TEXT("dup.lib"), TEXT(">=2.0.0"));

			const FModResolveResult Forward = FModDependencyResolver::Resolve(
				MakeTestRequest({ Old, New, App }));
			const FModResolveResult Reversed = FModDependencyResolver::Resolve(
				MakeTestRequest({ New, Old, App }));

			ExpectOrder(TEXT("duplicate ids reversed"), Reversed.LoadOrder, TEXT("dup.lib, dup.app"));

			const FModRejection* ForwardRejection = Forward.FindRejection(MakeTestId(TEXT("dup.lib")));
			const FModRejection* ReversedRejection = Reversed.FindRejection(MakeTestId(TEXT("dup.lib")));

			TestTrue(TEXT("duplicate ids reversed: both orders rejected a copy"),
				ForwardRejection != nullptr && ReversedRejection != nullptr);

			if (ForwardRejection != nullptr && ReversedRejection != nullptr)
			{
				TestEqual(TEXT("duplicate ids reversed: the same copy loses either way"),
					ReversedRejection->Message, ForwardRejection->Message);
			}
		});

		It(TEXT("prefers a release over a pre-release of the same version"), [this]()
		{
			// Semver precedence, not string order: 2.0.0 beats 2.0.0-rc.1.
			FModManifest Release = MakeTestManifest(TEXT("pre.lib"), TEXT("2.0.0"));
			FModManifest Candidate = MakeTestManifest(TEXT("pre.lib"), TEXT("2.0.0-rc.1"));

			FModManifest App = MakeTestManifest(TEXT("pre.app"));
			AddTestDependency(App, TEXT("pre.lib"), TEXT("2.0.0"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(
				MakeTestRequest({ Candidate, Release, App }));

			ExpectOrder(TEXT("pre-release duplicate"), Result.LoadOrder, TEXT("pre.lib, pre.app"));

			const FModRejection* Rejection = ExpectRejection(TEXT("pre-release duplicate"), Result,
				TEXT("pre.lib"), EModLoadFailureReason::DuplicateModId);
			if (Rejection != nullptr)
			{
				ExpectContains(TEXT("pre-release duplicate"), Rejection->Message, TEXT("version 2.0.0-rc.1 is a duplicate"));
			}
		});

		It(TEXT("reports a manifest with no id instead of letting it take part"), [this]()
		{
			FModManifest Anonymous;
			Anonymous.DisplayName = TEXT("Nameless");
			Anonymous.Version = FModVersion(1, 0, 0);

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({
				MakeTestManifest(TEXT("valid.mod")), Anonymous }));

			ExpectOrder(TEXT("missing id"), Result.LoadOrder, TEXT("valid.mod"));

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("missing id"), Result,
				TEXT("Resolve.InvalidManifest"), EModDiagnosticSeverity::Error);
			if (Diagnostic != nullptr)
			{
				ExpectContains(TEXT("missing id"), Diagnostic->Message, TEXT("Nameless"));
			}

			TestFalse(TEXT("missing id: the resolve reports failure"), Result.bSuccess);
		});
	});

	Describe(TEXT("Disabled mods"), [this]()
	{
		It(TEXT("excludes a disabled mod without turning the resolve into a failure"), [this]()
		{
			FModResolveRequest Request = MakeTestRequest({
				MakeTestManifest(TEXT("dis.solo")),
				MakeTestManifest(TEXT("dis.other")) });
			Request.DisabledMods.Add(MakeTestId(TEXT("dis.solo")));

			const FModResolveResult Result = FModDependencyResolver::Resolve(Request);

			// The player asked for this, so it must not look like something went wrong.
			TestTrue(TEXT("disabled: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("disabled"), Result.LoadOrder, TEXT("dis.other"));

			const FModRejection* Rejection = ExpectRejection(TEXT("disabled"), Result,
				TEXT("dis.solo"), EModLoadFailureReason::Disabled);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("disabled: message"), Rejection->Message,
					FString(TEXT("Mod 'dis.solo' is disabled and will not be loaded.")));
			}

			ExpectDiagnostic(TEXT("disabled"), Result, TEXT("Resolve.Disabled"), EModDiagnosticSeverity::Info);
		});

		It(TEXT("ignores a disabled id that matches no candidate"), [this]()
		{
			FModResolveRequest Request = MakeTestRequest({ MakeTestManifest(TEXT("dis.present")) });
			Request.DisabledMods.Add(MakeTestId(TEXT("dis.ghost")));

			const FModResolveResult Result = FModDependencyResolver::Resolve(Request);

			TestTrue(TEXT("unknown disabled id: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("unknown disabled id"), Result.LoadOrder, TEXT("dis.present"));
			TestEqual(TEXT("unknown disabled id: nothing was rejected"), Result.Rejections.Num(), 0);
			TestEqual(TEXT("unknown disabled id: no diagnostics"), Result.Diagnostics.Num(), 0);
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Environment
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Environment"), [this]()
	{
		It(TEXT("rejects a mod built for a different game"), [this]()
		{
			FModManifest Foreign = MakeTestManifest(TEXT("env.foreign"));
			Foreign.Game.GameId = TEXT("some.other.game");

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Foreign }));

			const FModRejection* Rejection = ExpectRejection(TEXT("wrong game"), Result,
				TEXT("env.foreign"), EModLoadFailureReason::IncompatibleGame);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("wrong game: message"), Rejection->Message, FString(
					TEXT("Mod 'env.foreign' is built for game 'some.other.game', but this game is 'test.game'.")));
			}

			ExpectDiagnostic(TEXT("wrong game"), Result, TEXT("Resolve.Environment"), EModDiagnosticSeverity::Error);
			ExpectOrder(TEXT("wrong game"), Result.LoadOrder, FString());
		});

		It(TEXT("matches the game id case insensitively"), [this]()
		{
			FModManifest Shouty = MakeTestManifest(TEXT("env.shouty"));
			Shouty.Game.GameId = TEXT("TEST.GAME");

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Shouty }));

			TestTrue(TEXT("game id case: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("game id case"), Result.LoadOrder, TEXT("env.shouty"));
		});

		It(TEXT("rejects a mod that requires a newer game version and names both versions"), [this]()
		{
			FModManifest Future = MakeTestManifest(TEXT("env.future"));
			Future.Game.VersionRange = MakeTestRange(TEXT(">=2.0.0"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Future }));

			const FModRejection* Rejection = ExpectRejection(TEXT("game version"), Result,
				TEXT("env.future"), EModLoadFailureReason::IncompatibleGame);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("game version: message"), Rejection->Message, FString(
					TEXT("Mod 'env.future' requires game version >=2.0.0, but this game is version 1.5.0.")));
			}
		});

		It(TEXT("accepts a mod whose game version range covers the running game"), [this]()
		{
			FModManifest Fine = MakeTestManifest(TEXT("env.fine"));
			Fine.Game.VersionRange = MakeTestRange(TEXT(">=1.5.0 <2.0.0"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Fine }));

			TestTrue(TEXT("game version in range: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("game version in range"), Result.LoadOrder, TEXT("env.fine"));
		});

		It(TEXT("rejects a mod outside the framework version range and names both versions"), [this]()
		{
			FModManifest Newer = MakeTestManifest(TEXT("env.newerframework"));
			Newer.FrameworkVersionRange = MakeTestRange(TEXT("^1.0.0"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Newer }));

			const FModRejection* Rejection = ExpectRejection(TEXT("framework version"), Result,
				TEXT("env.newerframework"), EModLoadFailureReason::IncompatibleFramework);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("framework version: message"), Rejection->Message, FString(
					TEXT("Mod 'env.newerframework' requires mod framework version ^1.0.0, ")
					TEXT("but this build provides framework version 0.1.0.")));
			}
		});

		It(TEXT("rejects a mod that needs an SDK this game does not ship"), [this]()
		{
			FModManifest NeedsSdk = MakeTestManifest(TEXT("env.nosdk"));
			NeedsSdk.Sdk.SdkId = TEXT("test.game.sdk");

			FModResolveRequest Request = MakeTestRequest({ NeedsSdk });
			Request.Environment.SdkId = FString();
			Request.Environment.SdkVersion = FModVersion();

			const FModResolveResult Result = FModDependencyResolver::Resolve(Request);

			const FModRejection* Rejection = ExpectRejection(TEXT("absent SDK"), Result,
				TEXT("env.nosdk"), EModLoadFailureReason::IncompatibleSdk);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("absent SDK: message"), Rejection->Message, FString(
					TEXT("Mod 'env.nosdk' requires the modding SDK 'test.game.sdk', ")
					TEXT("but this game does not provide a modding SDK.")));
			}
		});

		It(TEXT("rejects a mod that needs a different SDK"), [this]()
		{
			FModManifest WrongSdk = MakeTestManifest(TEXT("env.wrongsdk"));
			WrongSdk.Sdk.SdkId = TEXT("some.other.sdk");

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ WrongSdk }));

			const FModRejection* Rejection = ExpectRejection(TEXT("wrong SDK"), Result,
				TEXT("env.wrongsdk"), EModLoadFailureReason::IncompatibleSdk);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("wrong SDK: message"), Rejection->Message, FString(
					TEXT("Mod 'env.wrongsdk' requires the modding SDK 'some.other.sdk', ")
					TEXT("but this game provides 'test.game.sdk'.")));
			}
		});

		It(TEXT("rejects a mod outside the SDK version range and names both versions"), [this]()
		{
			FModManifest NewerSdk = MakeTestManifest(TEXT("env.newersdk"));
			NewerSdk.Sdk.SdkId = TEXT("test.game.sdk");
			NewerSdk.Sdk.VersionRange = MakeTestRange(TEXT(">=2.0.0"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ NewerSdk }));

			const FModRejection* Rejection = ExpectRejection(TEXT("SDK version"), Result,
				TEXT("env.newersdk"), EModLoadFailureReason::IncompatibleSdk);
			if (Rejection != nullptr)
			{
				TestEqual(TEXT("SDK version: message"), Rejection->Message, FString(
					TEXT("Mod 'env.newersdk' requires SDK version >=2.0.0, ")
					TEXT("but the installed SDK 'test.game.sdk' is version 1.5.0.")));
			}
		});

		It(TEXT("ignores an SDK version requirement that names no SDK id"), [this]()
		{
			FModManifest Vague = MakeTestManifest(TEXT("env.vaguesdk"));
			Vague.Sdk.VersionRange = MakeTestRange(TEXT(">=99.0.0"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Vague }));

			TestTrue(TEXT("SDK with no id: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("SDK with no id"), Result.LoadOrder, TEXT("env.vaguesdk"));

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("SDK with no id"), Result,
				TEXT("Resolve.Environment"), EModDiagnosticSeverity::Warning);
			if (Diagnostic != nullptr)
			{
				ExpectContains(TEXT("SDK with no id"), Diagnostic->Message, TEXT("names no SDK id"));
			}
		});

		It(TEXT("skips a version check the host cannot answer and warns instead of rejecting"), [this]()
		{
			// A game that never configured a GameVersion must not lose every mod that pins one.
			FModManifest Pinned = MakeTestManifest(TEXT("env.pinned"));
			Pinned.Game.VersionRange = MakeTestRange(TEXT(">=2.0.0"));

			FModResolveRequest Request = MakeTestRequest({ Pinned });
			Request.Environment.GameVersion = FModVersion();

			const FModResolveResult Result = FModDependencyResolver::Resolve(Request);

			TestTrue(TEXT("unknown game version: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("unknown game version"), Result.LoadOrder, TEXT("env.pinned"));
			TestEqual(TEXT("unknown game version: nothing was rejected"), Result.Rejections.Num(), 0);

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("unknown game version"), Result,
				TEXT("Resolve.Environment"), EModDiagnosticSeverity::Warning);
			if (Diagnostic != nullptr)
			{
				ExpectContains(TEXT("unknown game version"), Diagnostic->Message,
					TEXT("the game version check is skipped"));
			}
		});

		It(TEXT("treats a malformed environment range as any version and warns"), [this]()
		{
			FModManifest Typo = MakeTestManifest(TEXT("env.typo"));
			Typo.Game.VersionRange = MakeTestRange(TEXT("totally not a range"));

			const FModResolveResult Result = FModDependencyResolver::Resolve(MakeTestRequest({ Typo }));

			TestTrue(TEXT("malformed game range: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("malformed game range"), Result.LoadOrder, TEXT("env.typo"));

			const FModDiagnostic* Diagnostic = ExpectDiagnostic(TEXT("malformed game range"), Result,
				TEXT("Resolve.Environment"), EModDiagnosticSeverity::Warning);
			if (Diagnostic != nullptr)
			{
				ExpectContains(TEXT("malformed game range"), Diagnostic->Message,
					TEXT("not valid range syntax"));
			}
		});

		It(TEXT("skips every environment check when the request asks it to"), [this]()
		{
			FModManifest Foreign = MakeTestManifest(TEXT("env.skipped"));
			Foreign.Game.GameId = TEXT("some.other.game");
			Foreign.FrameworkVersionRange = MakeTestRange(TEXT("^9.0.0"));
			Foreign.Sdk.SdkId = TEXT("some.other.sdk");

			FModResolveRequest Request = MakeTestRequest({ Foreign });
			Request.bCheckEnvironment = false;

			const FModResolveResult Result = FModDependencyResolver::Resolve(Request);

			TestTrue(TEXT("environment skipped: the resolve succeeded"), Result.bSuccess);
			ExpectOrder(TEXT("environment skipped"), Result.LoadOrder, TEXT("env.skipped"));
			TestEqual(TEXT("environment skipped: nothing was rejected"), Result.Rejections.Num(), 0);
		});

		It(TEXT("CheckEnvironment reports the same verdicts standalone"), [this]()
		{
			const FModEnvironment Environment = MakeTestEnvironment();

			const TArray<FModRejection> Good = FModDependencyResolver::CheckEnvironment(
				MakeTestManifest(TEXT("check.good")), Environment);
			TestEqual(TEXT("CheckEnvironment: a compatible manifest yields no rejections"), Good.Num(), 0);

			FModManifest Bad = MakeTestManifest(TEXT("check.bad"));
			Bad.Game.GameId = TEXT("some.other.game");

			const TArray<FModRejection> Rejections = FModDependencyResolver::CheckEnvironment(Bad, Environment);
			TestEqual(TEXT("CheckEnvironment: an incompatible manifest yields one rejection"), Rejections.Num(), 1);

			if (Rejections.Num() == 1)
			{
				TestEqual(TEXT("CheckEnvironment: rejection reason"),
					ModFrameworkEnums::ToString(Rejections[0].Reason),
					ModFrameworkEnums::ToString(EModLoadFailureReason::IncompatibleGame));
				TestEqual(TEXT("CheckEnvironment: rejection names the mod"),
					Rejections[0].ModId.ToString(), FString(TEXT("check.bad")));
			}
		});

		It(TEXT("CheckEnvironment reports every independent problem at once"), [this]()
		{
			FModManifest Bad = MakeTestManifest(TEXT("check.multi"));
			Bad.Game.GameId = TEXT("some.other.game");
			Bad.FrameworkVersionRange = MakeTestRange(TEXT("^9.0.0"));

			const TArray<FModRejection> Rejections = FModDependencyResolver::CheckEnvironment(
				Bad, MakeTestEnvironment());

			// An author fixing a mod wants the whole list, not the first entry followed by another run.
			TestEqual(TEXT("CheckEnvironment: both problems were reported"), Rejections.Num(), 2);
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Graph and cycle utilities
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Graph"), [this]()
	{
		It(TEXT("BuildGraph makes every manifest a node and points edges from dependency to dependent"), [this]()
		{
			FModManifest Base = MakeTestManifest(TEXT("graph.base"));

			FModManifest Mid = MakeTestManifest(TEXT("graph.mid"));
			AddTestDependency(Mid, TEXT("graph.base"));

			FModManifest Top = MakeTestManifest(TEXT("graph.top"));
			Top.LoadAfter.Add(MakeTestId(TEXT("graph.mid")));

			TMap<FModId, TArray<FModId>> Edges;
			FModDependencyResolver::BuildGraph({ Top, Mid, Base }, Edges);

			TestEqual(TEXT("BuildGraph: every manifest is a node"), Edges.Num(), 3);

			const TArray<FModId>* FromBase = Edges.Find(MakeTestId(TEXT("graph.base")));
			const TArray<FModId>* FromMid = Edges.Find(MakeTestId(TEXT("graph.mid")));
			const TArray<FModId>* FromTop = Edges.Find(MakeTestId(TEXT("graph.top")));

			TestTrue(TEXT("BuildGraph: all three nodes are present"),
				FromBase != nullptr && FromMid != nullptr && FromTop != nullptr);

			if (FromBase != nullptr && FromMid != nullptr && FromTop != nullptr)
			{
				TestEqual(TEXT("BuildGraph: the dependency points at its dependent"),
					JoinIdArray(*FromBase), FString(TEXT("graph.mid")));
				TestEqual(TEXT("BuildGraph: loadAfter points at the declaring mod"),
					JoinIdArray(*FromMid), FString(TEXT("graph.top")));
				TestEqual(TEXT("BuildGraph: a leaf has no outgoing edges"),
					JoinIdArray(*FromTop), FString());
			}
		});

		It(TEXT("BuildGraph drops edges naming an absent mod but keeps a self edge"), [this]()
		{
			FModManifest Solo = MakeTestManifest(TEXT("graph.solo"));
			AddTestDependency(Solo, TEXT("graph.solo"));
			AddTestDependency(Solo, TEXT("graph.absent"));
			Solo.LoadBefore.Add(MakeTestId(TEXT("graph.alsoabsent")));

			TMap<FModId, TArray<FModId>> Edges;
			FModDependencyResolver::BuildGraph({ Solo }, Edges);

			TestEqual(TEXT("BuildGraph self edge: one node"), Edges.Num(), 1);

			const TArray<FModId>* FromSolo = Edges.Find(MakeTestId(TEXT("graph.solo")));
			TestTrue(TEXT("BuildGraph self edge: the node exists"), FromSolo != nullptr);

			if (FromSolo != nullptr)
			{
				// The self edge has to survive: FindCycles is what turns it into a rejection.
				TestEqual(TEXT("BuildGraph self edge: only the self edge remains"),
					JoinIdArray(*FromSolo), FString(TEXT("graph.solo")));
			}
		});

		It(TEXT("BuildGraph keeps only the highest version of a duplicated id"), [this]()
		{
			FModManifest Old = MakeTestManifest(TEXT("graph.dup"), TEXT("1.0.0"));

			FModManifest New = MakeTestManifest(TEXT("graph.dup"), TEXT("2.0.0"));
			New.LoadBefore.Add(MakeTestId(TEXT("graph.other")));

			FModManifest Other = MakeTestManifest(TEXT("graph.other"));

			TMap<FModId, TArray<FModId>> Edges;
			FModDependencyResolver::BuildGraph({ Old, New, Other }, Edges);

			TestEqual(TEXT("BuildGraph duplicates: one node per id"), Edges.Num(), 2);

			const TArray<FModId>* FromDup = Edges.Find(MakeTestId(TEXT("graph.dup")));
			TestTrue(TEXT("BuildGraph duplicates: the id is a node"), FromDup != nullptr);

			if (FromDup != nullptr)
			{
				// Only the 2.0.0 copy declares this edge, so seeing it proves 2.0.0 won.
				TestEqual(TEXT("BuildGraph duplicates: the highest version supplied the edges"),
					JoinIdArray(*FromDup), FString(TEXT("graph.other")));
			}
		});

		It(TEXT("FindCycles returns nothing for an acyclic graph"), [this]()
		{
			TMap<FModId, TArray<FModId>> Edges;
			Edges.Add(MakeTestId(TEXT("acyclic.a")), { MakeTestId(TEXT("acyclic.b")) });
			Edges.Add(MakeTestId(TEXT("acyclic.b")), { MakeTestId(TEXT("acyclic.c")) });
			Edges.Add(MakeTestId(TEXT("acyclic.c")), TArray<FModId>());

			TestEqual(TEXT("FindCycles: acyclic graph"), FModDependencyResolver::FindCycles(Edges).Num(), 0);
		});

		It(TEXT("FindCycles returns each cycle once, rotated to start at its smallest member"), [this]()
		{
			// b -> c -> a -> b is documented to come back as {a, b, c}.
			TMap<FModId, TArray<FModId>> Edges;
			Edges.Add(MakeTestId(TEXT("rot.b")), { MakeTestId(TEXT("rot.c")) });
			Edges.Add(MakeTestId(TEXT("rot.c")), { MakeTestId(TEXT("rot.a")) });
			Edges.Add(MakeTestId(TEXT("rot.a")), { MakeTestId(TEXT("rot.b")) });

			const TArray<TArray<FModId>> Cycles = FModDependencyResolver::FindCycles(Edges);

			TestEqual(TEXT("FindCycles: one cycle"), Cycles.Num(), 1);
			if (Cycles.Num() == 1)
			{
				TestEqual(TEXT("FindCycles: canonical rotation"),
					JoinIdArray(Cycles[0]), FString(TEXT("rot.a, rot.b, rot.c")));
			}
		});

		It(TEXT("FindCycles reports a self edge as a one-element cycle"), [this]()
		{
			TMap<FModId, TArray<FModId>> Edges;
			Edges.Add(MakeTestId(TEXT("loop.self")), { MakeTestId(TEXT("loop.self")) });

			const TArray<TArray<FModId>> Cycles = FModDependencyResolver::FindCycles(Edges);

			TestEqual(TEXT("FindCycles self edge: one cycle"), Cycles.Num(), 1);
			if (Cycles.Num() == 1)
			{
				TestEqual(TEXT("FindCycles self edge: the cycle is the node itself"),
					JoinIdArray(Cycles[0]), FString(TEXT("loop.self")));
			}
		});

		It(TEXT("FindCycles sorts disjoint cycles by their first member"), [this]()
		{
			TMap<FModId, TArray<FModId>> Edges;
			Edges.Add(MakeTestId(TEXT("two.x")), { MakeTestId(TEXT("two.y")) });
			Edges.Add(MakeTestId(TEXT("two.y")), { MakeTestId(TEXT("two.x")) });
			Edges.Add(MakeTestId(TEXT("one.a")), { MakeTestId(TEXT("one.b")) });
			Edges.Add(MakeTestId(TEXT("one.b")), { MakeTestId(TEXT("one.a")) });

			const TArray<TArray<FModId>> Cycles = FModDependencyResolver::FindCycles(Edges);

			TestEqual(TEXT("FindCycles disjoint: two cycles"), Cycles.Num(), 2);
			if (Cycles.Num() == 2)
			{
				TestEqual(TEXT("FindCycles disjoint: first cycle"),
					JoinIdArray(Cycles[0]), FString(TEXT("one.a, one.b")));
				TestEqual(TEXT("FindCycles disjoint: second cycle"),
					JoinIdArray(Cycles[1]), FString(TEXT("two.x, two.y")));
			}
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Conflicts
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Conflicts"), [this]()
	{
		It(TEXT("does not report a conflict for a single claim"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{ MakeTestClaim(TEXT("solo.mod"), 0) },
				MakeTestPolicies(EModConflictPolicy::Error));

			TestEqual(TEXT("single claim: no conflict"), Conflicts.Num(), 0);
		});

		It(TEXT("does not report a conflict for two claims from the same mod"), [this]()
		{
			// DOCUMENTED GUARANTEE: a mod is allowed to modify its own resource from several
			// extensions. Folding those claims into one contender is what stops the conflict report
			// from filling up with a mod arguing with itself.
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("same.mod"), 0, 1),
					MakeTestClaim(TEXT("same.mod"), 3, 9)
				},
				MakeTestPolicies(EModConflictPolicy::Error));

			TestEqual(TEXT("same-mod claims: no conflict"), Conflicts.Num(), 0);
		});

		It(TEXT("does report a conflict once a second mod claims the same resource"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("pair.b"), 1),
					MakeTestClaim(TEXT("pair.a"), 0),
					MakeTestClaim(TEXT("pair.a"), 4)
				},
				MakeTestPolicies(EModConflictPolicy::Merge));

			TestEqual(TEXT("two mods: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				// Contenders come back in load order then id, deduplicated by mod.
				TestEqual(TEXT("two mods: contenders"),
					JoinIdArray(Conflicts[0].Contenders), FString(TEXT("pair.a, pair.b")));
				TestEqual(TEXT("two mods: extension point"),
					Conflicts[0].ExtensionPointId.ToString(), FString(TestPoint));
				TestEqual(TEXT("two mods: resource"),
					Conflicts[0].ResourceId.ToString(), FString(TestResource));
			}
		});

		It(TEXT("ignores a claim that names no mod, no extension point or no resource"), [this]()
		{
			// Untrusted input from a hand-edited manifest must be dropped, not turned into a conflict.
			FModResourceClaim NoMod = MakeTestClaim(TEXT("ignored.a"), 0);
			NoMod.ModId.Reset();

			FModResourceClaim NoPoint = MakeTestClaim(TEXT("ignored.b"), 1);
			NoPoint.ExtensionPointId = NAME_None;

			FModResourceClaim NoResource = MakeTestClaim(TEXT("ignored.c"), 2);
			NoResource.ResourceId = NAME_None;

			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{ NoMod, NoPoint, NoResource, MakeTestClaim(TEXT("ignored.d"), 3) },
				MakeTestPolicies(EModConflictPolicy::Error));

			TestEqual(TEXT("incomplete claims: nothing is contested"), Conflicts.Num(), 0);
		});

		It(TEXT("FirstWins hands the resource to the lowest load order"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("first.late"), 7),
					MakeTestClaim(TEXT("first.early"), 2),
					MakeTestClaim(TEXT("first.middle"), 5)
				},
				MakeTestPolicies(EModConflictPolicy::FirstWins));

			TestEqual(TEXT("FirstWins: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestEqual(TEXT("FirstWins: winner"),
					Conflicts[0].Winner.ToString(), FString(TEXT("first.early")));
				TestEqual(TEXT("FirstWins: losers keep contender order"),
					JoinIdArray(Conflicts[0].Losers), FString(TEXT("first.middle, first.late")));
				TestFalse(TEXT("FirstWins: not blocking"), Conflicts[0].bBlocking);
				ExpectContains(TEXT("FirstWins"), Conflicts[0].Explanation,
					TEXT("'first.early' wins because it loads first (load order 2)"));
			}
		});

		It(TEXT("LastWins hands the resource to the highest load order"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("last.early"), 2),
					MakeTestClaim(TEXT("last.late"), 7),
					MakeTestClaim(TEXT("last.middle"), 5)
				},
				MakeTestPolicies(EModConflictPolicy::LastWins));

			TestEqual(TEXT("LastWins: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestEqual(TEXT("LastWins: winner"),
					Conflicts[0].Winner.ToString(), FString(TEXT("last.late")));
				TestEqual(TEXT("LastWins: losers"),
					JoinIdArray(Conflicts[0].Losers), FString(TEXT("last.early, last.middle")));
				TestFalse(TEXT("LastWins: not blocking"), Conflicts[0].bBlocking);
			}
		});

		It(TEXT("Priority picks the highest priority and breaks ties by the highest load order"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("prio.first"), 0, 5),
					MakeTestClaim(TEXT("prio.second"), 1, 5),
					MakeTestClaim(TEXT("prio.third"), 2, 1)
				},
				MakeTestPolicies(EModConflictPolicy::Priority));

			TestEqual(TEXT("Priority: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestEqual(TEXT("Priority: the later of the two highest priorities wins"),
					Conflicts[0].Winner.ToString(), FString(TEXT("prio.second")));
				ExpectContains(TEXT("Priority"), Conflicts[0].Explanation,
					TEXT("declares the highest priority (5)"));
			}
		});

		It(TEXT("Priority falls back to the lowest mod id when priority and load order both tie"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("tie.zulu"), 3, 5),
					MakeTestClaim(TEXT("tie.alpha"), 3, 5)
				},
				MakeTestPolicies(EModConflictPolicy::Priority));

			TestEqual(TEXT("Priority tie: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestEqual(TEXT("Priority tie: winner is the lowest id"),
					Conflicts[0].Winner.ToString(), FString(TEXT("tie.alpha")));
			}
		});

		It(TEXT("Error blocks every contender and picks no winner"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("err.a"), 0),
					MakeTestClaim(TEXT("err.b"), 1)
				},
				MakeTestPolicies(EModConflictPolicy::Error));

			TestEqual(TEXT("Error: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestTrue(TEXT("Error: the conflict is blocking"), Conflicts[0].bBlocking);
				TestFalse(TEXT("Error: no winner"), Conflicts[0].Winner.IsValid());

				// Nobody lost to anybody, so nobody is a loser.
				TestEqual(TEXT("Error: no losers"), Conflicts[0].Losers.Num(), 0);
				ExpectContains(TEXT("Error"), Conflicts[0].Explanation,
					TEXT("no mod may claim it and both contenders are blocked"));
			}

			// Every contender is blocked, not just the later ones: an Error policy says the game has
			// no way to arbitrate, so the framework refuses to guess which one the player meant.
			TestEqual(TEXT("Error: blocked mods"),
				JoinIdArray(FModConflictDetector::GetBlockedMods(Conflicts)), FString(TEXT("err.a, err.b")));
		});

		It(TEXT("Merge keeps every contender and does not block"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("merge.a"), 0),
					MakeTestClaim(TEXT("merge.b"), 1),
					MakeTestClaim(TEXT("merge.c"), 2)
				},
				MakeTestPolicies(EModConflictPolicy::Merge));

			TestEqual(TEXT("Merge: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestFalse(TEXT("Merge: not blocking"), Conflicts[0].bBlocking);
				TestFalse(TEXT("Merge: no winner"), Conflicts[0].Winner.IsValid());
				TestEqual(TEXT("Merge: no losers"), Conflicts[0].Losers.Num(), 0);
				ExpectContains(TEXT("Merge"), Conflicts[0].Explanation,
					TEXT("all 3 contenders are kept and merged"));
			}

			TestEqual(TEXT("Merge: nothing is blocked"),
				FModConflictDetector::GetBlockedMods(Conflicts).Num(), 0);
		});

		It(TEXT("sorts an unassigned load order after every assigned one"), [this]()
		{
			// Conflict detection can run before the resolver has produced a load order. A claim with
			// no position must not win FirstWins by accident.
			const TArray<FModResourceClaim> Claims = {
				MakeTestClaim(TEXT("noorder.unplaced"), INDEX_NONE),
				MakeTestClaim(TEXT("noorder.placed"), 0)
			};

			const TArray<FModConflict> FirstResult = FModConflictDetector::Detect(
				Claims, MakeTestPolicies(EModConflictPolicy::FirstWins));

			TestEqual(TEXT("unassigned load order: one FirstWins conflict"), FirstResult.Num(), 1);
			if (FirstResult.Num() == 1)
			{
				TestEqual(TEXT("unassigned load order: contenders put the placed mod first"),
					JoinIdArray(FirstResult[0].Contenders), FString(TEXT("noorder.placed, noorder.unplaced")));
				TestEqual(TEXT("unassigned load order: FirstWins picks the placed mod"),
					FirstResult[0].Winner.ToString(), FString(TEXT("noorder.placed")));
			}

			const TArray<FModConflict> LastResult = FModConflictDetector::Detect(
				Claims, MakeTestPolicies(EModConflictPolicy::LastWins));

			TestEqual(TEXT("unassigned load order: one LastWins conflict"), LastResult.Num(), 1);
			if (LastResult.Num() == 1)
			{
				TestEqual(TEXT("unassigned load order: LastWins picks the unplaced mod"),
					LastResult[0].Winner.ToString(), FString(TEXT("noorder.unplaced")));
				ExpectContains(TEXT("unassigned load order"), LastResult[0].Explanation,
					TEXT("no assigned load order and therefore sorts last"));
			}
		});

		It(TEXT("returns conflicts sorted by extension point then resource"), [this]()
		{
			TArray<FModResourceClaim> Claims;
			Claims.Add(MakeTestClaim(TEXT("sort.a"), 0, 0, EModConflictPolicy::Error, TEXT("z.point"), TEXT("b.resource")));
			Claims.Add(MakeTestClaim(TEXT("sort.b"), 1, 0, EModConflictPolicy::Error, TEXT("z.point"), TEXT("b.resource")));
			Claims.Add(MakeTestClaim(TEXT("sort.a"), 0, 0, EModConflictPolicy::Error, TEXT("a.point"), TEXT("q.resource")));
			Claims.Add(MakeTestClaim(TEXT("sort.b"), 1, 0, EModConflictPolicy::Error, TEXT("a.point"), TEXT("q.resource")));
			Claims.Add(MakeTestClaim(TEXT("sort.a"), 0, 0, EModConflictPolicy::Error, TEXT("z.point"), TEXT("a.resource")));
			Claims.Add(MakeTestClaim(TEXT("sort.b"), 1, 0, EModConflictPolicy::Error, TEXT("z.point"), TEXT("a.resource")));

			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				Claims, MakeTestPolicies(EModConflictPolicy::Merge));

			TestEqual(TEXT("conflict sorting: three conflicts"), Conflicts.Num(), 3);
			if (Conflicts.Num() == 3)
			{
				FString Actual;
				for (const FModConflict& Conflict : Conflicts)
				{
					if (!Actual.IsEmpty())
					{
						Actual += TEXT(", ");
					}

					Actual += FString::Printf(TEXT("%s:%s"),
						*Conflict.ExtensionPointId.ToString(), *Conflict.ResourceId.ToString());
				}

				TestEqual(TEXT("conflict sorting: order"), Actual,
					FString(TEXT("a.point:q.resource, z.point:a.resource, z.point:b.resource")));
			}
		});

		It(TEXT("collects every contender of every blocking conflict, sorted and deduplicated"), [this]()
		{
			TArray<FModResourceClaim> Claims;
			Claims.Add(MakeTestClaim(TEXT("block.b"), 1, 0, EModConflictPolicy::Error, TestPoint, TEXT("res.one")));
			Claims.Add(MakeTestClaim(TEXT("block.a"), 0, 0, EModConflictPolicy::Error, TestPoint, TEXT("res.one")));
			Claims.Add(MakeTestClaim(TEXT("block.a"), 0, 0, EModConflictPolicy::Error, TestPoint, TEXT("res.two")));
			Claims.Add(MakeTestClaim(TEXT("block.c"), 2, 0, EModConflictPolicy::Error, TestPoint, TEXT("res.two")));
			Claims.Add(MakeTestClaim(TEXT("free.a"), 0, 0, EModConflictPolicy::Merge, TestPoint, TEXT("res.merged")));
			Claims.Add(MakeTestClaim(TEXT("free.b"), 1, 0, EModConflictPolicy::Merge, TestPoint, TEXT("res.merged")));

			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				Claims, MakeTestPolicies(EModConflictPolicy::Error));

			TestEqual(TEXT("blocked mods: three conflicts"), Conflicts.Num(), 3);
			TestEqual(TEXT("blocked mods: only the blocking contenders, sorted and unique"),
				JoinIdArray(FModConflictDetector::GetBlockedMods(Conflicts)),
				FString(TEXT("block.a, block.b, block.c")));
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Conflict policy precedence
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Conflict policy precedence"), [this]()
	{
		// DOCUMENTED GUARANTEE, most specific first:
		//   1. a per-resource override in the policy table;
		//   2. a per-extension-point override in the policy table;
		//   3. a preference every claim on the resource agreed on;
		//   4. the table default.
		// One mod can never impose a policy on another, which is why (3) needs unanimity.

		It(TEXT("prefers a resource override over everything else"), [this]()
		{
			FModConflictPolicyTable Policies = MakeTestPolicies(EModConflictPolicy::Error);
			Policies.PointPolicies.Add(FName(TestPoint), EModConflictPolicy::LastWins);
			Policies.ResourcePolicies.Add(
				FModConflictPolicyTable::MakeResourceKey(FName(TestPoint), FName(TestResource)),
				EModConflictPolicy::FirstWins);

			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("prec.a"), 0, 0, EModConflictPolicy::Merge),
					MakeTestClaim(TEXT("prec.b"), 1, 0, EModConflictPolicy::Merge)
				},
				Policies);

			TestEqual(TEXT("resource override: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestEqual(TEXT("resource override: applied policy"),
					ModFrameworkEnums::ToString(Conflicts[0].AppliedPolicy),
					ModFrameworkEnums::ToString(EModConflictPolicy::FirstWins));
				TestEqual(TEXT("resource override: winner"),
					Conflicts[0].Winner.ToString(), FString(TEXT("prec.a")));
				ExpectContains(TEXT("resource override"), Conflicts[0].Explanation,
					TEXT("overrides this exact resource with FirstWins"));
			}
		});

		It(TEXT("prefers a point override over a unanimous preference"), [this]()
		{
			FModConflictPolicyTable Policies = MakeTestPolicies(EModConflictPolicy::Error);
			Policies.PointPolicies.Add(FName(TestPoint), EModConflictPolicy::LastWins);

			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("prec.a"), 0, 0, EModConflictPolicy::Merge),
					MakeTestClaim(TEXT("prec.b"), 1, 0, EModConflictPolicy::Merge)
				},
				Policies);

			TestEqual(TEXT("point override: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestEqual(TEXT("point override: applied policy"),
					ModFrameworkEnums::ToString(Conflicts[0].AppliedPolicy),
					ModFrameworkEnums::ToString(EModConflictPolicy::LastWins));
				TestEqual(TEXT("point override: winner"),
					Conflicts[0].Winner.ToString(), FString(TEXT("prec.b")));
				ExpectContains(TEXT("point override"), Conflicts[0].Explanation,
					TEXT("overrides extension point 'game.weapon' with LastWins"));
			}
		});

		It(TEXT("prefers a unanimous preference over the table default"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("prec.a"), 0, 0, EModConflictPolicy::Merge),
					MakeTestClaim(TEXT("prec.b"), 1, 0, EModConflictPolicy::Merge)
				},
				MakeTestPolicies(EModConflictPolicy::Error));

			TestEqual(TEXT("unanimous preference: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestEqual(TEXT("unanimous preference: applied policy"),
					ModFrameworkEnums::ToString(Conflicts[0].AppliedPolicy),
					ModFrameworkEnums::ToString(EModConflictPolicy::Merge));
				TestFalse(TEXT("unanimous preference: Merge is not blocking"), Conflicts[0].bBlocking);
				ExpectContains(TEXT("unanimous preference"), Conflicts[0].Explanation,
					TEXT("no override for it and every claim asked for Merge"));
			}
		});

		It(TEXT("falls back to the table default when the claims disagree"), [this]()
		{
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("prec.a"), 0, 0, EModConflictPolicy::Merge),
					MakeTestClaim(TEXT("prec.b"), 1, 0, EModConflictPolicy::FirstWins)
				},
				MakeTestPolicies(EModConflictPolicy::Error));

			TestEqual(TEXT("disagreement: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestEqual(TEXT("disagreement: applied policy"),
					ModFrameworkEnums::ToString(Conflicts[0].AppliedPolicy),
					ModFrameworkEnums::ToString(EModConflictPolicy::Error));
				TestTrue(TEXT("disagreement: Error is blocking"), Conflicts[0].bBlocking);
				ExpectContains(TEXT("disagreement"), Conflicts[0].Explanation,
					TEXT("the claims disagree about what they want, so the table default of Error applies"));
			}
		});

		It(TEXT("denies unanimity to a mod that contradicts itself across two claims"), [this]()
		{
			// Unanimity is a property of every CLAIM on the resource, not of every mod.
			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				{
					MakeTestClaim(TEXT("split.a"), 0, 0, EModConflictPolicy::Merge),
					MakeTestClaim(TEXT("split.a"), 1, 0, EModConflictPolicy::FirstWins),
					MakeTestClaim(TEXT("split.b"), 2, 0, EModConflictPolicy::Merge)
				},
				MakeTestPolicies(EModConflictPolicy::LastWins));

			TestEqual(TEXT("self-contradiction: one conflict"), Conflicts.Num(), 1);
			if (Conflicts.Num() == 1)
			{
				TestEqual(TEXT("self-contradiction: the table default applies"),
					ModFrameworkEnums::ToString(Conflicts[0].AppliedPolicy),
					ModFrameworkEnums::ToString(EModConflictPolicy::LastWins));
			}
		});

		It(TEXT("FModConflictPolicyTable::Resolve applies resource, then point, then default"), [this]()
		{
			const FName Point(TestPoint);
			const FName Resource(TestResource);

			FModConflictPolicyTable Policies = MakeTestPolicies(EModConflictPolicy::Merge);

			TestEqual(TEXT("table Resolve: default"),
				ModFrameworkEnums::ToString(Policies.Resolve(Point, Resource)),
				ModFrameworkEnums::ToString(EModConflictPolicy::Merge));

			Policies.PointPolicies.Add(Point, EModConflictPolicy::LastWins);
			TestEqual(TEXT("table Resolve: point override"),
				ModFrameworkEnums::ToString(Policies.Resolve(Point, Resource)),
				ModFrameworkEnums::ToString(EModConflictPolicy::LastWins));

			Policies.ResourcePolicies.Add(
				FModConflictPolicyTable::MakeResourceKey(Point, Resource), EModConflictPolicy::FirstWins);
			TestEqual(TEXT("table Resolve: resource override"),
				ModFrameworkEnums::ToString(Policies.Resolve(Point, Resource)),
				ModFrameworkEnums::ToString(EModConflictPolicy::FirstWins));

			// A different resource on the same point still sees only the point override.
			TestEqual(TEXT("table Resolve: another resource on the same point"),
				ModFrameworkEnums::ToString(Policies.Resolve(Point, FName(TEXT("weapon.dagger")))),
				ModFrameworkEnums::ToString(EModConflictPolicy::LastWins));
		});

		It(TEXT("MakeResourceKey joins the extension point and the resource with a single colon"), [this]()
		{
			TestEqual(TEXT("MakeResourceKey: format"),
				FModConflictPolicyTable::MakeResourceKey(FName(TestPoint), FName(TestResource)).ToString(),
				FString(TEXT("game.weapon:weapon.longsword")));

			// NAME_None still produces a well formed key rather than an empty one.
			TestEqual(TEXT("MakeResourceKey: none"),
				FModConflictPolicyTable::MakeResourceKey(NAME_None, NAME_None).ToString(),
				FString(TEXT("None:None")));
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Conflict report
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Conflict report"), [this]()
	{
		It(TEXT("says so in words when there is nothing to report"), [this]()
		{
			TestEqual(TEXT("empty report"), FModConflictDetector::BuildReport(TArray<FModConflict>()),
				FString(TEXT("No mod conflicts detected.")));
		});

		It(TEXT("summarises how many conflicts are blocking and how many were resolved"), [this]()
		{
			TArray<FModResourceClaim> Claims;
			Claims.Add(MakeTestClaim(TEXT("rep.a"), 0, 0, EModConflictPolicy::Error, TestPoint, TEXT("res.blocked")));
			Claims.Add(MakeTestClaim(TEXT("rep.b"), 1, 0, EModConflictPolicy::Error, TestPoint, TEXT("res.blocked")));
			Claims.Add(MakeTestClaim(TEXT("rep.a"), 0, 0, EModConflictPolicy::LastWins, TestPoint, TEXT("res.resolved")));
			Claims.Add(MakeTestClaim(TEXT("rep.b"), 1, 0, EModConflictPolicy::LastWins, TestPoint, TEXT("res.resolved")));

			const TArray<FModConflict> Conflicts = FModConflictDetector::Detect(
				Claims, MakeTestPolicies(EModConflictPolicy::Error));

			TestEqual(TEXT("report: two conflicts"), Conflicts.Num(), 2);

			const FString Report = FModConflictDetector::BuildReport(Conflicts);

			ExpectContains(TEXT("report"), Report, TEXT("Mod conflicts: 2 detected, 1 blocking, 1 resolved."));
			ExpectContains(TEXT("report"), Report, TEXT("EXTENSION POINT"));
			ExpectContains(TEXT("report"), Report, TEXT("(blocked)"));
			ExpectContains(TEXT("report"), Report, TEXT("game.weapon:res.blocked"));
			ExpectContains(TEXT("report"), Report, TEXT("game.weapon:res.resolved"));
			TestFalse(TEXT("report: no trailing newline"), Report.EndsWith(TEXT("\n")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
