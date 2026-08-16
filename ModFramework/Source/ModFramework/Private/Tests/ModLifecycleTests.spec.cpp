// Copyright (c) 2026. Licensed for use in your own projects.

#if WITH_DEV_AUTOMATION_TESTS

#include "API/ModAPI.h"
#include "API/ModAPIRegistry.h"
#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreGlobals.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Extensions/ModExtension.h"
#include "Extensions/ModExtensionRegistry.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreMiscDefines.h"
#include "Permissions/ModPermissions.h"
#include "Registry/ModInfo.h"
#include "Registry/ModRegistry.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"
#include "UObject/GarbageCollection.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectBaseUtility.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

/**
 * Fixtures shared by every test below.
 *
 * Nothing here needs a world or a game instance: UModRegistry, UModAPIRegistry and
 * UModExtensionRegistry are plain UObjects that never resolve a world, and every object these tests
 * create is outered to the transient package. If a change to one of them makes a test here need a
 * world, that change broke the registries' independence from one, not the test.
 */
namespace ModLifecycleTestsPrivate
{
	/** Every EModState value, in declaration order. Kept in sync with Core/ModFrameworkTypes.h. */
	static const EModState AllStates[] =
	{
		EModState::Unknown,
		EModState::Discovered,
		EModState::Validated,
		EModState::DependenciesResolved,
		EModState::Mounted,
		EModState::Loading,
		EModState::Loaded,
		EModState::Activated,
		EModState::Deactivated,
		EModState::Unmounted,
		EModState::Failed,
		EModState::Disabled
	};

	/** One edge of the lifecycle graph. */
	struct FStateEdge
	{
		EModState From;
		EModState To;
	};

	/**
	 * The transition table exactly as UModRegistry::IsTransitionAllowed implements it, minus the
	 * universal "any state may move to Failed" rule, which is tested separately.
	 *
	 * This is a transcription of the switch in Private/Registry/ModRegistry.cpp, which agrees with
	 * the table documented on UModRegistry::IsTransitionAllowed in the header.
	 */
	static const FStateEdge AllowedEdges[] =
	{
		{ EModState::Unknown,              EModState::Discovered           },
		{ EModState::Discovered,           EModState::Validated            },
		{ EModState::Discovered,           EModState::Disabled             },
		{ EModState::Validated,            EModState::DependenciesResolved },
		{ EModState::Validated,            EModState::Disabled             },
		{ EModState::DependenciesResolved, EModState::Mounted              },
		{ EModState::Mounted,              EModState::Loading              },
		{ EModState::Mounted,              EModState::Unmounted            },
		{ EModState::Loading,              EModState::Loaded               },
		{ EModState::Loaded,               EModState::Activated            },
		{ EModState::Loaded,               EModState::Unmounted            },
		{ EModState::Activated,            EModState::Deactivated          },
		{ EModState::Deactivated,          EModState::Activated            },
		{ EModState::Deactivated,          EModState::Unmounted            },
		{ EModState::Unmounted,            EModState::Discovered           },
		{ EModState::Unmounted,            EModState::Mounted              },
		{ EModState::Failed,               EModState::Discovered           },
		{ EModState::Disabled,             EModState::Discovered           }
	};

	/** True when (From, To) is an edge the lifecycle graph is expected to accept. */
	bool IsExpectedEdge(EModState From, EModState To)
	{
		// Anything can fail, at any point, including a mod that has already failed.
		if (To == EModState::Failed)
		{
			return true;
		}

		for (const FStateEdge& Edge : AllowedEdges)
		{
			if (Edge.From == From && Edge.To == To)
			{
				return true;
			}
		}

		return false;
	}

	FModId MakeTestId(const TCHAR* InId)
	{
		return FModId(FName(InId));
	}

	/** A manifest with just enough filled in for UModRegistry to accept the record. */
	FModManifest MakeTestManifest(const TCHAR* InId, const TCHAR* InVersion = TEXT("1.0.0"))
	{
		FModManifest Manifest;
		Manifest.Id = MakeTestId(InId);
		Manifest.Version = FModVersion::FromString(InVersion);
		Manifest.Game.GameId = TEXT("test.game");
		return Manifest;
	}

	/** A discovered mod record as a provider would hand it to UModRegistry::RegisterMod. */
	FModInfo MakeTestModInfo(const TCHAR* InId, int32 InLoadOrder = INDEX_NONE)
	{
		FModInfo Info;
		Info.Manifest = MakeTestManifest(InId);
		Info.RootPath = FString::Printf(TEXT("/virtual/mods/%s"), InId);
		Info.ProviderId = FName(TEXT("test.provider"));
		Info.LoadOrder = InLoadOrder;
		return Info;
	}

	/** The point, resource and three mods the ResolveResource tests fight over. */
	const TCHAR* const ContestPoint = TEXT("game.contest");
	const TCHAR* const ContestResource = TEXT("contest.resource");
	const TCHAR* const ContestEarly = TEXT("contest.early");
	const TCHAR* const ContestMiddle = TEXT("contest.middle");
	const TCHAR* const ContestLate = TEXT("contest.late");

	/** An extension point descriptor with the framework defaults, ready to be tweaked by a test. */
	FModExtensionPointDescriptor MakeTestPoint(const TCHAR* InPointId)
	{
		FModExtensionPointDescriptor Point;
		Point.ExtensionPointId = FName(InPointId);
		Point.Version = FModVersion(1, 0, 0);
		return Point;
	}

	/** "a, b, c" - so a failed ordering assertion prints the whole order rather than a count. */
	FString JoinIds(const TArray<FModId>& Ids)
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

	/** The same, for the plural FModInfo accessors. */
	FString JoinModInfoIds(const TArray<FModInfo>& Mods)
	{
		FString Result;
		for (int32 Index = 0; Index < Mods.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += TEXT(", ");
			}

			Result += Mods[Index].GetId().ToString();
		}

		return Result;
	}

	/** The same, for an extension list. A null entry is spelled out rather than skipped. */
	FString JoinExtensionIds(const TArray<UModExtension*>& Extensions)
	{
		FString Result;
		for (int32 Index = 0; Index < Extensions.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += TEXT(", ");
			}

			Result += Extensions[Index] != nullptr
				? Extensions[Index]->GetResolvedExtensionId().ToString()
				: FString(TEXT("<null>"));
		}

		return Result;
	}

	/**
	 * Writes one of UModAPI's authored override properties through reflection.
	 *
	 * The overrides are protected so that nothing outside the class defaults can forge an API
	 * identity, and the editor sets them through reflection from those defaults. This spec has to do
	 * the same, because UnrealHeaderTool never parses a .cpp: a test-only UModAPI subclass cannot be
	 * declared in this file, and putting one in a header would ship it in the runtime module in every
	 * configuration. Writing the property is exactly what a details panel does, and the identity is
	 * only read afterwards, so nothing observes a half-configured API.
	 */
	template <typename PropertyValueType>
	bool WriteAuthoredProperty(UObject* Object, const TCHAR* PropertyName, const PropertyValueType& Value)
	{
		if (Object == nullptr || Object->GetClass() == nullptr)
		{
			return false;
		}

		FProperty* Property = Object->GetClass()->FindPropertyByName(FName(PropertyName));
		if (Property == nullptr)
		{
			return false;
		}

		*Property->ContainerPtrToValuePtr<PropertyValueType>(Object) = Value;
		return true;
	}
}

BEGIN_DEFINE_SPEC(FModLifecycleSpec, "ModFramework.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	/**
	 * The registry under test, rebuilt for every single It.
	 *
	 * A TStrongObjectPtr rather than a raw pointer because the spec runner executes BeforeEach, the
	 * test body and AfterEach as separate latent commands, i.e. potentially on different frames, and
	 * a garbage collection between two of them would otherwise take the registry with it.
	 */
	TStrongObjectPtr<UModRegistry> Registry;

	UModRegistry* GetModRegistry() const;
	UModAPIRegistry* GetApiRegistry() const;
	UModExtensionRegistry* GetExtensionRegistry() const;

	/** Case-sensitive string comparison with a message that prints both sides. */
	bool ExpectString(const FString& What, const FString& Actual, const FString& Expected);

	/** Asserts a diagnostic carries the expected code, an Error severity and a readable message. */
	bool ExpectDiagnostic(const FString& What, const FModDiagnostic& Diagnostic, const TCHAR* Code);

	/** Asserts nothing was written into a diagnostic, i.e. the call succeeded. */
	bool ExpectNoDiagnostic(const FString& What, const FModDiagnostic& Diagnostic);

	/** Registers a mod record, failing the test with the registry's own diagnostic if it is refused. */
	bool RegisterTestMod(const TCHAR* InId, int32 InLoadOrder = INDEX_NONE);

	/** Walks a freshly registered mod down the happy path until it reaches Target. */
	bool AdvanceModTo(const FModId& InId, EModState Target);

	/** Opens an extension point, failing the test with the registry's own diagnostic if refused. */
	bool RegisterTestPoint(const FModExtensionPointDescriptor& InPoint);

	/** An unregistered extension, configured but not yet handed to the registry. */
	UModExtension* MakeTestExtension(const TCHAR* InPointId, const TCHAR* InExtensionId,
		int32 InPriority = 0, const TArray<FName>& InClaims = TArray<FName>());

	/** MakeTestExtension plus a registration that the test expects to succeed. */
	UModExtension* RegisterTestExtension(const TCHAR* InPointId, const TCHAR* InModId, const TCHAR* InExtensionId,
		int32 InPriority = 0, const TArray<FName>& InClaims = TArray<FName>());

	/** An unregistered API with an authored id, version and permission list. */
	UModAPI* MakeTestApi(const TCHAR* InApiId, const TCHAR* InVersion = TEXT("1.0.0"),
		const TArray<FName>& InPermissions = TArray<FName>());

	/**
	 * Opens the contest point under Policy and gives three mods, in load order, an active claim on
	 * the same resource. The priorities are per mod so a Priority-policy test can rank them.
	 */
	bool SetUpResourceContest(EModConflictPolicy Policy,
		int32 EarlyPriority = 0, int32 MiddlePriority = 0, int32 LatePriority = 0);

	/** The resolved id of whoever wins the contested resource, or "<none>" when nobody does. */
	FString ResolveContestWinner();

	/**
	 * A bare UClass whose super is InBase, used only as a type token for IsChildOf / IsA checks.
	 *
	 * Two behaviours need a class *derived* from a framework base to test their negative path: an
	 * extension point's RequiredBaseClass and K2_RequestAPI's ApiClass. Both only ever ask "is this
	 * object's class a child of that class", and IsChildOf walks the SuperStruct chain, which
	 * UStruct::SetSuperStruct keeps consistent (it re-initialises the base chain array). The token is
	 * never instantiated and never given a class default object, so nothing else about it has to be
	 * real. See WriteAuthoredProperty above for why a genuine test-only UCLASS is not an option.
	 */
	UClass* MakeTypeToken(UClass* InBase, const TCHAR* InName);

	/**
	 * LogModFramework is deliberately not captured as test output.
	 *
	 * Almost every test below drives a rejection path on purpose, and the framework mirrors every
	 * rejection into LogModFramework at Error or Warning verbosity - by design, because a player's
	 * log is where a broken mod has to be diagnosable. The automation output device turns a captured
	 * Error into a test failure, so leaving the category captured would fail the suite for doing
	 * exactly what it is testing. Every assertion here is made on the returned FModDiagnostic or on
	 * the observable state instead, which is stricter than matching log text.
	 */
	virtual bool ShouldCaptureLogCategory(const FName& Category) const override
	{
		static const FName ModFrameworkCategory(TEXT("LogModFramework"));
		return Category != ModFrameworkCategory;
	}

END_DEFINE_SPEC(FModLifecycleSpec)

//////////////////////////////////////////////////////////////////////////
// Helpers

UModRegistry* FModLifecycleSpec::GetModRegistry() const
{
	return Registry.Get();
}

UModAPIRegistry* FModLifecycleSpec::GetApiRegistry() const
{
	return Registry.IsValid() ? Registry->GetAPIRegistry() : nullptr;
}

UModExtensionRegistry* FModLifecycleSpec::GetExtensionRegistry() const
{
	return Registry.IsValid() ? Registry->GetExtensionRegistry() : nullptr;
}

bool FModLifecycleSpec::ExpectString(const FString& What, const FString& Actual, const FString& Expected)
{
	if (!Actual.Equals(Expected, ESearchCase::CaseSensitive))
	{
		AddError(FString::Printf(TEXT("%s: expected '%s' but got '%s'."), *What, *Expected, *Actual));
		return false;
	}

	return true;
}

bool FModLifecycleSpec::ExpectDiagnostic(const FString& What, const FModDiagnostic& Diagnostic, const TCHAR* Code)
{
	bool bOk = ExpectString(FString::Printf(TEXT("%s: diagnostic code"), *What), Diagnostic.Code.ToString(), Code);

	if (Diagnostic.Severity != EModDiagnosticSeverity::Error)
	{
		AddError(FString::Printf(TEXT("%s: diagnostic '%s' has severity %s, expected Error."),
			*What, Code, *ModFrameworkEnums::ToString(Diagnostic.Severity)));
		bOk = false;
	}

	// A refusal a mod author cannot read is barely better than a silent one.
	if (Diagnostic.Message.IsEmpty())
	{
		AddError(FString::Printf(TEXT("%s: diagnostic '%s' carries no message."), *What, Code));
		bOk = false;
	}

	return bOk;
}

bool FModLifecycleSpec::ExpectNoDiagnostic(const FString& What, const FModDiagnostic& Diagnostic)
{
	if (!Diagnostic.Code.IsNone())
	{
		AddError(FString::Printf(TEXT("%s: expected no diagnostic but got %s"), *What, *Diagnostic.ToString()));
		return false;
	}

	return true;
}

bool FModLifecycleSpec::RegisterTestMod(const TCHAR* InId, int32 InLoadOrder)
{
	FModDiagnostic Error;

	if (!GetModRegistry()->RegisterMod(ModLifecycleTestsPrivate::MakeTestModInfo(InId, InLoadOrder), Error))
	{
		AddError(FString::Printf(TEXT("Fixture: registering mod '%s' failed: %s"), InId, *Error.ToString()));
		return false;
	}

	return true;
}

bool FModLifecycleSpec::AdvanceModTo(const FModId& InId, EModState Target)
{
	// The states a mod passes through on the way to being live, in order.
	static const EModState HappyPath[] =
	{
		EModState::Validated,
		EModState::DependenciesResolved,
		EModState::Mounted,
		EModState::Loading,
		EModState::Loaded,
		EModState::Activated
	};

	const FModInfo* Info = GetModRegistry()->FindMod(InId);
	if (Info == nullptr)
	{
		AddError(FString::Printf(TEXT("Fixture: cannot advance unregistered mod '%s'."), *InId.ToString()));
		return false;
	}

	if (Info->State == Target)
	{
		return true;
	}

	for (const EModState Step : HappyPath)
	{
		if (!GetModRegistry()->SetModState(InId, Step))
		{
			AddError(FString::Printf(TEXT("Fixture: mod '%s' could not move to %s."),
				*InId.ToString(), *ModFrameworkEnums::ToString(Step)));
			return false;
		}

		if (Step == Target)
		{
			return true;
		}
	}

	AddError(FString::Printf(TEXT("Fixture: %s is not on the happy path, so a mod cannot be driven to it."),
		*ModFrameworkEnums::ToString(Target)));
	return false;
}

bool FModLifecycleSpec::RegisterTestPoint(const FModExtensionPointDescriptor& InPoint)
{
	FModDiagnostic Error;

	if (!GetExtensionRegistry()->RegisterExtensionPoint(InPoint, Error))
	{
		AddError(FString::Printf(TEXT("Fixture: opening extension point '%s' failed: %s"),
			*InPoint.ExtensionPointId.ToString(), *Error.ToString()));
		return false;
	}

	return true;
}

UModExtension* FModLifecycleSpec::MakeTestExtension(const TCHAR* InPointId, const TCHAR* InExtensionId,
	int32 InPriority, const TArray<FName>& InClaims)
{
	// UModExtension is UCLASS(Abstract) because a game is expected to subclass it, but the C++ class
	// has no pure virtuals and the registry only ever treats it as data plus four callbacks. This
	// scope is the engine's supported way to allocate one anyway; see MakeTypeToken for why a real
	// test-only subclass is not available here.
	FScopedAllowAbstractClassAllocation AllowAbstract;

	UModExtension* Extension = NewObject<UModExtension>(GetTransientPackage());

	Extension->ExtensionPointId = InPointId != nullptr ? FName(InPointId) : NAME_None;
	Extension->ExtensionId = InExtensionId != nullptr ? FName(InExtensionId) : NAME_None;
	Extension->Priority = InPriority;
	Extension->ClaimedResourceIds = InClaims;

	return Extension;
}

UModExtension* FModLifecycleSpec::RegisterTestExtension(const TCHAR* InPointId, const TCHAR* InModId,
	const TCHAR* InExtensionId, int32 InPriority, const TArray<FName>& InClaims)
{
	UModExtension* Extension = MakeTestExtension(InPointId, InExtensionId, InPriority, InClaims);

	FModDiagnostic Error;
	if (!GetExtensionRegistry()->RegisterExtension(Extension, ModLifecycleTestsPrivate::MakeTestId(InModId), Error))
	{
		AddError(FString::Printf(TEXT("Fixture: registering extension '%s' for mod '%s' failed: %s"),
			InExtensionId != nullptr ? InExtensionId : TEXT("<unnamed>"), InModId, *Error.ToString()));
		return nullptr;
	}

	return Extension;
}

UModAPI* FModLifecycleSpec::MakeTestApi(const TCHAR* InApiId, const TCHAR* InVersion, const TArray<FName>& InPermissions)
{
	using namespace ModLifecycleTestsPrivate;

	FScopedAllowAbstractClassAllocation AllowAbstract;

	UModAPI* Api = NewObject<UModAPI>(GetTransientPackage());

	if (!WriteAuthoredProperty<FName>(Api, TEXT("ApiIdOverride"), FName(InApiId)))
	{
		AddError(TEXT("Fixture: UModAPI has no ApiIdOverride property; this spec configures APIs through it."));
		return nullptr;
	}

	if (!WriteAuthoredProperty<FModVersion>(Api, TEXT("ApiVersionOverride"), FModVersion::FromString(InVersion)))
	{
		AddError(TEXT("Fixture: UModAPI has no ApiVersionOverride property; this spec configures APIs through it."));
		return nullptr;
	}

	if (!WriteAuthoredProperty<TArray<FName>>(Api, TEXT("RequiredPermissionsOverride"), InPermissions))
	{
		AddError(TEXT("Fixture: UModAPI has no RequiredPermissionsOverride property; this spec configures APIs through it."));
		return nullptr;
	}

	// The identity is cached on first query; nothing has queried it yet, but say so explicitly so a
	// future reordering of this function cannot hand back a stale id.
	Api->InvalidateResolvedIdentity();

	return Api;
}

bool FModLifecycleSpec::SetUpResourceContest(EModConflictPolicy Policy,
	int32 EarlyPriority, int32 MiddlePriority, int32 LatePriority)
{
	using namespace ModLifecycleTestsPrivate;

	FModExtensionPointDescriptor Point = MakeTestPoint(ContestPoint);
	Point.DefaultConflictPolicy = Policy;

	if (!RegisterTestPoint(Point))
	{
		return false;
	}

	TArray<FModId> Order;
	Order.Add(MakeTestId(ContestEarly));
	Order.Add(MakeTestId(ContestMiddle));
	Order.Add(MakeTestId(ContestLate));
	GetExtensionRegistry()->SetModLoadOrder(Order);

	TArray<FName> Claims;
	Claims.Add(FName(ContestResource));

	const TCHAR* const ContestingMods[] = { ContestEarly, ContestMiddle, ContestLate };
	const int32 Priorities[] = { EarlyPriority, MiddlePriority, LatePriority };

	constexpr int32 ContestantCount = UE_ARRAY_COUNT(ContestingMods);

	for (int32 Index = 0; Index < ContestantCount; ++Index)
	{
		const FString ExtensionId = FString::Printf(TEXT("%s:claim"), ContestingMods[Index]);

		if (RegisterTestExtension(ContestPoint, ContestingMods[Index], *ExtensionId, Priorities[Index], Claims) == nullptr)
		{
			return false;
		}

		// Only activated extensions may win a resource: a mod that is merely loaded must not change
		// what the game sees.
		GetExtensionRegistry()->SetModExtensionsActive(MakeTestId(ContestingMods[Index]), true);
	}

	return true;
}

FString FModLifecycleSpec::ResolveContestWinner()
{
	using namespace ModLifecycleTestsPrivate;

	const UModExtension* Winner = GetExtensionRegistry()->ResolveResource(FName(ContestPoint), FName(ContestResource));
	return Winner != nullptr ? Winner->GetResolvedExtensionId().ToString() : FString(TEXT("<none>"));
}

UClass* FModLifecycleSpec::MakeTypeToken(UClass* InBase, const TCHAR* InName)
{
	UClass* Token = NewObject<UClass>(
		GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UClass::StaticClass(), FName(InName)),
		RF_Transient);

	Token->SetSuperStruct(InBase);
	return Token;
}

//////////////////////////////////////////////////////////////////////////

void FModLifecycleSpec::Define()
{
	using namespace ModLifecycleTestsPrivate;

	BeforeEach([this]()
	{
		Registry.Reset(NewObject<UModRegistry>(GetTransientPackage()));
		Registry->Initialize();
	});

	AfterEach([this]()
	{
		if (Registry.IsValid())
		{
			Registry->Shutdown();
			Registry.Reset();
		}
	});

	//~ -------------------------------------------------------------------------------------------
	//~ State machine
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Transitions"), [this]()
	{
		It(TEXT("accepts every edge in the documented transition table"), [this]()
		{
			for (const FStateEdge& Edge : AllowedEdges)
			{
				const FString FailureMessage = FString::Printf(TEXT("%s -> %s is a documented edge"),
					*ModFrameworkEnums::ToString(Edge.From), *ModFrameworkEnums::ToString(Edge.To));

				TestTrue(*FailureMessage, UModRegistry::IsTransitionAllowed(Edge.From, Edge.To));
			}
		});

		// DOCUMENTED GUARANTEE: any state may move to Failed, including Failed itself. Loading a mod
		// is a long chain of steps that can each break, and every one of them has to be able to give
		// up without first proving which step it is on.
		It(TEXT("accepts a move to Failed from every state, Failed included"), [this]()
		{
			for (const EModState From : AllStates)
			{
				const FString FailureMessage = FString::Printf(TEXT("%s -> Failed"), *ModFrameworkEnums::ToString(From));
				TestTrue(*FailureMessage, UModRegistry::IsTransitionAllowed(From, EModState::Failed));
			}
		});

		// Exhaustive rather than a sample: every pair the table does not list has to be refused, and
		// a table that quietly gains an edge is exactly the regression worth catching.
		It(TEXT("rejects every pair the table does not list"), [this]()
		{
			int32 UnexpectedlyAllowed = 0;

			for (const EModState From : AllStates)
			{
				for (const EModState To : AllStates)
				{
					const bool bExpected = IsExpectedEdge(From, To);
					const bool bActual = UModRegistry::IsTransitionAllowed(From, To);

					if (bActual == bExpected)
					{
						continue;
					}

					++UnexpectedlyAllowed;
					AddError(FString::Printf(TEXT("%s -> %s: the state machine %s it, expected it to be %s."),
						*ModFrameworkEnums::ToString(From),
						*ModFrameworkEnums::ToString(To),
						bActual ? TEXT("allows") : TEXT("rejects"),
						bExpected ? TEXT("allowed") : TEXT("rejected")));
				}
			}

			TestEqual(TEXT("number of transitions that disagree with the documented table"), UnexpectedlyAllowed, 0);
		});

		It(TEXT("rejects re-entering the state a mod is already in, except Failed"), [this]()
		{
			for (const EModState State : AllStates)
			{
				const FString FailureMessage = FString::Printf(TEXT("%s -> %s"),
					*ModFrameworkEnums::ToString(State), *ModFrameworkEnums::ToString(State));

				if (State == EModState::Failed)
				{
					TestTrue(*FailureMessage, UModRegistry::IsTransitionAllowed(State, State));
				}
				else
				{
					TestFalse(*FailureMessage, UModRegistry::IsTransitionAllowed(State, State));
				}
			}
		});

		// A state value can arrive from deserialised, untrusted data, so an out-of-range enumerator
		// has to be refused rather than asserted on.
		It(TEXT("rejects a state value outside the enum"), [this]()
		{
			const EModState Garbage = static_cast<EModState>(200);

			TestFalse(TEXT("garbage -> Discovered"), UModRegistry::IsTransitionAllowed(Garbage, EModState::Discovered));
			TestFalse(TEXT("garbage -> Loaded"), UModRegistry::IsTransitionAllowed(Garbage, EModState::Loaded));
			TestFalse(TEXT("Discovered -> garbage"), UModRegistry::IsTransitionAllowed(EModState::Discovered, Garbage));
			TestFalse(TEXT("Loaded -> garbage"), UModRegistry::IsTransitionAllowed(EModState::Loaded, Garbage));
			TestFalse(TEXT("garbage -> garbage"), UModRegistry::IsTransitionAllowed(Garbage, Garbage));

			// "Anything can fail" is checked before the state is interpreted at all, so even a value
			// the framework cannot name is allowed to end up Failed.
			TestTrue(TEXT("garbage -> Failed"), UModRegistry::IsTransitionAllowed(Garbage, EModState::Failed));
		});

		It(TEXT("SetModState refuses an illegal transition and changes nothing"), [this]()
		{
			if (!RegisterTestMod(TEXT("state.mod")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("state.mod"));

			TestFalse(TEXT("Discovered -> Loaded is refused"), GetModRegistry()->SetModState(ModId, EModState::Loaded));

			const FModInfo* Info = GetModRegistry()->FindMod(ModId);
			if (TestNotNull(TEXT("the record survives a refused transition"), Info))
			{
				ExpectString(TEXT("state after a refused transition"),
					ModFrameworkEnums::ToString(Info->State), TEXT("Discovered"));
			}
		});

		It(TEXT("SetModState refuses an unknown mod id"), [this]()
		{
			TestFalse(TEXT("an unregistered mod cannot change state"),
				GetModRegistry()->SetModState(MakeTestId(TEXT("state.absent")), EModState::Validated));
		});

		It(TEXT("walks a mod all the way to Activated one legal edge at a time"), [this]()
		{
			if (!RegisterTestMod(TEXT("state.walk")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("state.walk"));

			TestTrue(TEXT("reaches Activated"), AdvanceModTo(ModId, EModState::Activated));

			const FModInfo* Info = GetModRegistry()->FindMod(ModId);
			if (TestNotNull(TEXT("the record still exists"), Info))
			{
				ExpectString(TEXT("final state"), ModFrameworkEnums::ToString(Info->State), TEXT("Activated"));
				TestTrue(TEXT("an activated mod counts as loaded"), Info->IsLoaded());
				TestTrue(TEXT("an activated mod counts as active"), Info->IsActive());
			}

			// Deactivated is still "loaded": the code is live, it is just idle.
			TestTrue(TEXT("Activated -> Deactivated"), GetModRegistry()->SetModState(ModId, EModState::Deactivated));

			Info = GetModRegistry()->FindMod(ModId);
			if (TestNotNull(TEXT("the record still exists after deactivating"), Info))
			{
				TestTrue(TEXT("a deactivated mod is still loaded"), Info->IsLoaded());
				TestFalse(TEXT("a deactivated mod is not active"), Info->IsActive());
			}

			TestTrue(TEXT("Deactivated -> Unmounted"), GetModRegistry()->SetModState(ModId, EModState::Unmounted));

			Info = GetModRegistry()->FindMod(ModId);
			if (TestNotNull(TEXT("the record still exists after unmounting"), Info))
			{
				TestFalse(TEXT("an unmounted mod is not loaded"), Info->IsLoaded());
			}
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Registration
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Registration"), [this]()
	{
		It(TEXT("files a discovered mod under its manifest id and moves it out of Unknown"), [this]()
		{
			FModDiagnostic Error;
			const FModInfo Incoming = MakeTestModInfo(TEXT("reg.alpha"));

			TestTrue(TEXT("registers"), GetModRegistry()->RegisterMod(Incoming, Error));

			// OutError is only written on failure, so it must still be untouched here.
			ExpectNoDiagnostic(TEXT("a successful registration"), Error);

			const FModInfo* Stored = GetModRegistry()->FindMod(MakeTestId(TEXT("reg.alpha")));
			if (!TestNotNull(TEXT("the record is retrievable"), Stored))
			{
				return;
			}

			// Registering a record IS the act of discovering it, and Discovered is the only edge out
			// of Unknown, so the registry applies it rather than leaving the record in Unknown.
			ExpectString(TEXT("state"), ModFrameworkEnums::ToString(Stored->State), TEXT("Discovered"));
			ExpectString(TEXT("root path survives"), Stored->RootPath, Incoming.RootPath);
			ExpectString(TEXT("provider id survives"), Stored->ProviderId.ToString(), TEXT("test.provider"));
			TestEqual(TEXT("mod count"), GetModRegistry()->GetModCount(), 1);
			TestTrue(TEXT("IsModRegistered"), GetModRegistry()->IsModRegistered(MakeTestId(TEXT("reg.alpha"))));
		});

		It(TEXT("does not broadcast a state change for a registration"), [this]()
		{
			int32 BroadcastCount = 0;
			const FDelegateHandle Handle = GetModRegistry()->OnModStateChanged.AddLambda(
				[&BroadcastCount](const FModId&, EModState, EModState)
				{
					++BroadcastCount;
				});

			RegisterTestMod(TEXT("reg.quiet"));

			GetModRegistry()->OnModStateChanged.Remove(Handle);

			// Registration is not a transition: the record arrives already in Discovered, and a
			// listener that treated registration as a transition would double-count every mod.
			TestEqual(TEXT("broadcasts during registration"), BroadcastCount, 0);
		});

		It(TEXT("keeps a state a provider already assigned"), [this]()
		{
			FModInfo Incoming = MakeTestModInfo(TEXT("reg.prestated"));
			Incoming.State = EModState::Disabled;

			FModDiagnostic Error;
			TestTrue(TEXT("registers"), GetModRegistry()->RegisterMod(Incoming, Error));

			const FModInfo* Stored = GetModRegistry()->FindMod(MakeTestId(TEXT("reg.prestated")));
			if (TestNotNull(TEXT("the record is retrievable"), Stored))
			{
				ExpectString(TEXT("state"), ModFrameworkEnums::ToString(Stored->State), TEXT("Disabled"));
			}
		});

		It(TEXT("rejects a record with no manifest id"), [this]()
		{
			FModInfo Incoming = MakeTestModInfo(TEXT("reg.nameless"));
			Incoming.Manifest.Id.Reset();

			FModDiagnostic Error;
			TestFalse(TEXT("does not register"), GetModRegistry()->RegisterMod(Incoming, Error));
			ExpectDiagnostic(TEXT("a record with no id"), Error, TEXT("Registry.InvalidModId"));
			TestEqual(TEXT("nothing was stored"), GetModRegistry()->GetModCount(), 0);
		});

		It(TEXT("rejects a duplicate id and keeps the first record"), [this]()
		{
			FModInfo First = MakeTestModInfo(TEXT("reg.dup"));
			First.RootPath = TEXT("/virtual/mods/first");

			FModInfo Second = MakeTestModInfo(TEXT("reg.dup"));
			Second.RootPath = TEXT("/virtual/mods/second");

			FModDiagnostic Error;
			TestTrue(TEXT("the first registers"), GetModRegistry()->RegisterMod(First, Error));
			TestFalse(TEXT("the second is refused"), GetModRegistry()->RegisterMod(Second, Error));

			ExpectDiagnostic(TEXT("a duplicate id"), Error, TEXT("Registry.DuplicateModId"));
			ExpectString(TEXT("the diagnostic is stamped with the mod id"), Error.ModId.ToString(), TEXT("reg.dup"));

			const FModInfo* Stored = GetModRegistry()->FindMod(MakeTestId(TEXT("reg.dup")));
			if (TestNotNull(TEXT("the record is retrievable"), Stored))
			{
				// The winner must be the mod that got there first; silently replacing it would make
				// which copy of a mod runs depend on directory scan order.
				ExpectString(TEXT("the first record survives"), Stored->RootPath, TEXT("/virtual/mods/first"));
			}

			TestEqual(TEXT("mod count"), GetModRegistry()->GetModCount(), 1);
		});

		It(TEXT("normalises any negative load order to INDEX_NONE"), [this]()
		{
			FModInfo Incoming = MakeTestModInfo(TEXT("reg.negative"));
			Incoming.LoadOrder = -17;

			FModDiagnostic Error;
			TestTrue(TEXT("registers"), GetModRegistry()->RegisterMod(Incoming, Error));

			const FModInfo* Stored = GetModRegistry()->FindMod(MakeTestId(TEXT("reg.negative")));
			if (TestNotNull(TEXT("the record is retrievable"), Stored))
			{
				// The sort comparator and SetLoadOrder both spell "unordered" as INDEX_NONE; a second
				// spelling would sort inconsistently.
				TestEqual(TEXT("load order"), Stored->LoadOrder, static_cast<int32>(INDEX_NONE));
			}
		});

		It(TEXT("unregisters a mod and refuses an unknown id"), [this]()
		{
			if (!RegisterTestMod(TEXT("reg.gone")))
			{
				return;
			}

			TestFalse(TEXT("an unknown id cannot be unregistered"),
				GetModRegistry()->UnregisterMod(MakeTestId(TEXT("reg.never"))));

			TestTrue(TEXT("a known id can"), GetModRegistry()->UnregisterMod(MakeTestId(TEXT("reg.gone"))));
			TestEqual(TEXT("mod count"), GetModRegistry()->GetModCount(), 0);
			TestNull(TEXT("the record is gone"), GetModRegistry()->FindMod(MakeTestId(TEXT("reg.gone"))));
		});

		It(TEXT("Reset forgets every mod"), [this]()
		{
			RegisterTestMod(TEXT("reg.one"));
			RegisterTestMod(TEXT("reg.two"));
			TestEqual(TEXT("mod count before"), GetModRegistry()->GetModCount(), 2);

			GetModRegistry()->Reset();

			TestEqual(TEXT("mod count after"), GetModRegistry()->GetModCount(), 0);
			TestEqual(TEXT("no ids remain"), GetModRegistry()->GetAllModIds().Num(), 0);
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Lookup and ordering
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Lookup"), [this]()
	{
		It(TEXT("GetModInfo copies the record out and leaves the output alone for an unknown id"), [this]()
		{
			if (!RegisterTestMod(TEXT("look.known")))
			{
				return;
			}

			FModInfo Copy;
			TestTrue(TEXT("a known id copies out"), GetModRegistry()->GetModInfo(MakeTestId(TEXT("look.known")), Copy));
			ExpectString(TEXT("the copy carries the id"), Copy.GetId().ToString(), TEXT("look.known"));

			FModInfo Sentinel;
			Sentinel.RootPath = TEXT("/sentinel");

			TestFalse(TEXT("an unknown id does not"), GetModRegistry()->GetModInfo(MakeTestId(TEXT("look.absent")), Sentinel));
			ExpectString(TEXT("the caller's value is untouched"), Sentinel.RootPath, TEXT("/sentinel"));
		});

		It(TEXT("GetAllModIds is sorted lexically"), [this]()
		{
			RegisterTestMod(TEXT("look.zulu"));
			RegisterTestMod(TEXT("look.alpha"));
			RegisterTestMod(TEXT("look.mike"));

			ExpectString(TEXT("sorted ids"), JoinIds(GetModRegistry()->GetAllModIds()),
				TEXT("look.alpha, look.mike, look.zulu"));
		});

		// DOCUMENTED GUARANTEE: load order ascending, unordered mods last, ties broken by id. Mod ids
		// are unique, so the order is total and reproduces exactly from run to run even though the
		// underlying TMap iterates in insertion and removal order.
		It(TEXT("GetAllMods sorts by load order with unordered mods last, then by id"), [this]()
		{
			RegisterTestMod(TEXT("order.unranked.b"));
			RegisterTestMod(TEXT("order.second"), 1);
			RegisterTestMod(TEXT("order.unranked.a"));
			RegisterTestMod(TEXT("order.first"), 0);
			RegisterTestMod(TEXT("order.third"), 2);

			ExpectString(TEXT("GetAllMods"), JoinModInfoIds(GetModRegistry()->GetAllMods()),
				TEXT("order.first, order.second, order.third, order.unranked.a, order.unranked.b"));
		});

		It(TEXT("GetModsInState, GetLoadedMods and GetActiveMods keep the GetAllMods order"), [this]()
		{
			RegisterTestMod(TEXT("filter.second"), 1);
			RegisterTestMod(TEXT("filter.first"), 0);
			RegisterTestMod(TEXT("filter.third"), 2);

			// first -> Activated, second -> Loaded, third stays Discovered.
			AdvanceModTo(MakeTestId(TEXT("filter.first")), EModState::Activated);
			AdvanceModTo(MakeTestId(TEXT("filter.second")), EModState::Loaded);

			ExpectString(TEXT("mods in Discovered"),
				JoinModInfoIds(GetModRegistry()->GetModsInState(EModState::Discovered)), TEXT("filter.third"));

			ExpectString(TEXT("loaded mods"),
				JoinModInfoIds(GetModRegistry()->GetLoadedMods()), TEXT("filter.first, filter.second"));

			ExpectString(TEXT("active mods"),
				JoinModInfoIds(GetModRegistry()->GetActiveMods()), TEXT("filter.first"));

			// Deactivated is still loaded, so it must stay in GetLoadedMods and leave GetActiveMods.
			GetModRegistry()->SetModState(MakeTestId(TEXT("filter.first")), EModState::Deactivated);

			ExpectString(TEXT("loaded mods after deactivating"),
				JoinModInfoIds(GetModRegistry()->GetLoadedMods()), TEXT("filter.first, filter.second"));
			ExpectString(TEXT("active mods after deactivating"),
				JoinModInfoIds(GetModRegistry()->GetActiveMods()), FString());
		});

		It(TEXT("SetLoadOrder numbers the listed mods and unranks everything else"), [this]()
		{
			RegisterTestMod(TEXT("setorder.a"), 7);
			RegisterTestMod(TEXT("setorder.b"), 8);
			RegisterTestMod(TEXT("setorder.c"), 9);

			TArray<FModId> Order;
			Order.Add(MakeTestId(TEXT("setorder.c")));
			Order.Add(MakeTestId(TEXT("setorder.a")));

			TestTrue(TEXT("a clean order returns true"), GetModRegistry()->SetLoadOrder(Order));

			const FModInfo* C = GetModRegistry()->FindMod(MakeTestId(TEXT("setorder.c")));
			const FModInfo* A = GetModRegistry()->FindMod(MakeTestId(TEXT("setorder.a")));
			const FModInfo* B = GetModRegistry()->FindMod(MakeTestId(TEXT("setorder.b")));

			if (TestNotNull(TEXT("c exists"), C))
			{
				TestEqual(TEXT("c is first"), C->LoadOrder, 0);
			}
			if (TestNotNull(TEXT("a exists"), A))
			{
				TestEqual(TEXT("a is second"), A->LoadOrder, 1);
			}
			if (TestNotNull(TEXT("b exists"), B))
			{
				// A mod the resolver left out of the order must be reset, not left carrying a stale
				// position from a previous resolve.
				TestEqual(TEXT("b is unranked"), B->LoadOrder, static_cast<int32>(INDEX_NONE));
			}

			ExpectString(TEXT("GetAllMods follows the new order"),
				JoinModInfoIds(GetModRegistry()->GetAllMods()), TEXT("setorder.c, setorder.a, setorder.b"));
		});

		It(TEXT("SetLoadOrder still applies the usable entries of a partially stale order"), [this]()
		{
			RegisterTestMod(TEXT("stale.a"));
			RegisterTestMod(TEXT("stale.b"));

			TArray<FModId> Order;
			Order.Add(MakeTestId(TEXT("stale.b")));
			Order.Add(MakeTestId(TEXT("stale.ghost")));
			Order.Add(MakeTestId(TEXT("stale.b")));
			Order.Add(FModId());
			Order.Add(MakeTestId(TEXT("stale.a")));

			// A stale order is a warning, not a reason to leave every mod unordered: an unordered
			// table would change which mod wins every conflict in the session.
			TestFalse(TEXT("a stale order reports itself"), GetModRegistry()->SetLoadOrder(Order));

			const FModInfo* B = GetModRegistry()->FindMod(MakeTestId(TEXT("stale.b")));
			const FModInfo* A = GetModRegistry()->FindMod(MakeTestId(TEXT("stale.a")));

			if (TestNotNull(TEXT("b exists"), B))
			{
				TestEqual(TEXT("the first occurrence of b wins"), B->LoadOrder, 0);
			}
			if (TestNotNull(TEXT("a exists"), A))
			{
				// The unknown id, the repeat and the invalid id all consumed no position.
				TestEqual(TEXT("a follows immediately"), A->LoadOrder, 1);
			}
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Enabling and failure
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Enabling"), [this]()
	{
		It(TEXT("disabling a discovered mod moves it to Disabled and re-enabling returns it to Discovered"), [this]()
		{
			if (!RegisterTestMod(TEXT("enable.simple")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("enable.simple"));

			TestTrue(TEXT("disables"), GetModRegistry()->SetModEnabled(ModId, false));

			const FModInfo* Info = GetModRegistry()->FindMod(ModId);
			if (TestNotNull(TEXT("the record exists"), Info))
			{
				TestFalse(TEXT("the flag is cleared"), Info->bEnabled);
				ExpectString(TEXT("state"), ModFrameworkEnums::ToString(Info->State), TEXT("Disabled"));
			}

			TestTrue(TEXT("re-enables"), GetModRegistry()->SetModEnabled(ModId, true));

			Info = GetModRegistry()->FindMod(ModId);
			if (TestNotNull(TEXT("the record still exists"), Info))
			{
				TestTrue(TEXT("the flag is set"), Info->bEnabled);
				ExpectString(TEXT("state"), ModFrameworkEnums::ToString(Info->State), TEXT("Discovered"));
			}
		});

		It(TEXT("disabling a loaded mod clears the flag but leaves the state alone"), [this]()
		{
			if (!RegisterTestMod(TEXT("enable.loaded")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("enable.loaded"));
			if (!AdvanceModTo(ModId, EModState::Loaded))
			{
				return;
			}

			TestTrue(TEXT("disables"), GetModRegistry()->SetModEnabled(ModId, false));

			const FModInfo* Info = GetModRegistry()->FindMod(ModId);
			if (TestNotNull(TEXT("the record exists"), Info))
			{
				TestFalse(TEXT("the flag is cleared"), Info->bEnabled);

				// Loaded -> Disabled is not an edge: tearing a live mod down is UModSubsystem's job,
				// and pretending it is already down would strand its objects and mounts.
				ExpectString(TEXT("state is unchanged"), ModFrameworkEnums::ToString(Info->State), TEXT("Loaded"));
			}
		});

		It(TEXT("setting the flag to the value it already has is a no-op that still succeeds"), [this]()
		{
			if (!RegisterTestMod(TEXT("enable.noop")))
			{
				return;
			}

			int32 BroadcastCount = 0;
			const FDelegateHandle Handle = GetModRegistry()->OnModStateChanged.AddLambda(
				[&BroadcastCount](const FModId&, EModState, EModState)
				{
					++BroadcastCount;
				});

			TestTrue(TEXT("enabling an enabled mod succeeds"),
				GetModRegistry()->SetModEnabled(MakeTestId(TEXT("enable.noop")), true));

			GetModRegistry()->OnModStateChanged.Remove(Handle);

			TestEqual(TEXT("no state change was broadcast"), BroadcastCount, 0);
		});

		It(TEXT("refuses an unknown mod id"), [this]()
		{
			TestFalse(TEXT("an unregistered mod has no flag to set"),
				GetModRegistry()->SetModEnabled(MakeTestId(TEXT("enable.absent")), false));
		});
	});

	Describe(TEXT("Failure"), [this]()
	{
		It(TEXT("SetModFailed records the reason, the message and a diagnostic"), [this]()
		{
			if (!RegisterTestMod(TEXT("fail.mod")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("fail.mod"));

			TestTrue(TEXT("fails the mod"), GetModRegistry()->SetModFailed(
				ModId, EModLoadFailureReason::MountFailed, TEXT("the pak refused to mount")));

			const FModInfo* Info = GetModRegistry()->FindMod(ModId);
			if (!TestNotNull(TEXT("the record exists"), Info))
			{
				return;
			}

			ExpectString(TEXT("state"), ModFrameworkEnums::ToString(Info->State), TEXT("Failed"));
			ExpectString(TEXT("reason"), ModFrameworkEnums::ToString(Info->FailureReason), TEXT("MountFailed"));
			ExpectString(TEXT("message"), Info->FailureMessage, TEXT("the pak refused to mount"));

			if (TestEqual(TEXT("one diagnostic was recorded"), Info->Diagnostics.Num(), 1))
			{
				ExpectString(TEXT("diagnostic code"), Info->Diagnostics[0].Code.ToString(), TEXT("Registry.ModFailed"));
				ExpectString(TEXT("diagnostic mod id"), Info->Diagnostics[0].ModId.ToString(), TEXT("fail.mod"));

				// The reason has to survive in text as well as in the enum: FModInfo is what a mod
				// browser shows, and "MountFailed" alone tells a player nothing.
				TestTrue(TEXT("the diagnostic names the reason"),
					Info->Diagnostics[0].Message.Contains(TEXT("MountFailed"), ESearchCase::CaseSensitive));
				TestTrue(TEXT("the diagnostic carries the message"),
					Info->Diagnostics[0].Message.Contains(TEXT("the pak refused to mount"), ESearchCase::CaseSensitive));
			}
		});

		It(TEXT("clears the failure reason when the mod leaves Failed"), [this]()
		{
			if (!RegisterTestMod(TEXT("fail.retry")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("fail.retry"));
			GetModRegistry()->SetModFailed(ModId, EModLoadFailureReason::ProviderError, TEXT("transient"));

			// Failed -> Discovered is the retry edge.
			TestTrue(TEXT("retries"), GetModRegistry()->SetModState(ModId, EModState::Discovered));

			const FModInfo* Info = GetModRegistry()->FindMod(ModId);
			if (TestNotNull(TEXT("the record exists"), Info))
			{
				ExpectString(TEXT("reason"), ModFrameworkEnums::ToString(Info->FailureReason), TEXT("None"));
				TestTrue(TEXT("the message is cleared"), Info->FailureMessage.IsEmpty());

				// The diagnostic history is deliberately NOT cleared: why a mod failed last time is
				// exactly what somebody debugging a retry needs.
				TestEqual(TEXT("the diagnostic history survives"), Info->Diagnostics.Num(), 1);
			}
		});

		It(TEXT("refuses to fail an unknown mod id"), [this]()
		{
			TestFalse(TEXT("an unregistered mod cannot fail"), GetModRegistry()->SetModFailed(
				MakeTestId(TEXT("fail.absent")), EModLoadFailureReason::Internal, TEXT("nobody home")));
		});

		It(TEXT("AddDiagnostic stamps the mod id and ignores an unknown one"), [this]()
		{
			if (!RegisterTestMod(TEXT("diag.mod")))
			{
				return;
			}

			GetModRegistry()->AddDiagnostic(MakeTestId(TEXT("diag.mod")),
				FModDiagnostic::Warning(FName(TEXT("Test.Unstamped")), TEXT("no mod id set"), TEXT("ctx")));

			// An entry that already names a mod keeps that name, so a diagnostic forwarded from one
			// mod's resolve is not re-attributed to whoever stored it.
			FModDiagnostic PreStamped = FModDiagnostic::Info(FName(TEXT("Test.Stamped")), TEXT("already stamped"));
			PreStamped.ModId = MakeTestId(TEXT("diag.other"));
			GetModRegistry()->AddDiagnostic(MakeTestId(TEXT("diag.mod")), PreStamped);

			const FModInfo* Info = GetModRegistry()->FindMod(MakeTestId(TEXT("diag.mod")));
			if (TestNotNull(TEXT("the record exists"), Info)
				&& TestEqual(TEXT("two diagnostics were recorded"), Info->Diagnostics.Num(), 2))
			{
				ExpectString(TEXT("the unstamped entry is stamped"), Info->Diagnostics[0].ModId.ToString(), TEXT("diag.mod"));
				ExpectString(TEXT("the pre-stamped entry keeps its id"), Info->Diagnostics[1].ModId.ToString(), TEXT("diag.other"));
			}

			// An unknown id must not create a record; it only logs.
			GetModRegistry()->AddDiagnostic(MakeTestId(TEXT("diag.absent")),
				FModDiagnostic::Error(FName(TEXT("Test.Dropped")), TEXT("nowhere to put this")));
			TestEqual(TEXT("no record was created"), GetModRegistry()->GetModCount(), 1);
		});

	});

	Describe(TEXT("Record fields"), [this]()
	{
		It(TEXT("SetGrantedPermissions drops duplicates and blanks"), [this]()
		{
			if (!RegisterTestMod(TEXT("perm.mod")))
			{
				return;
			}

			TArray<FName> Permissions;
			Permissions.Add(ModPermissions::GameplayModify);
			Permissions.Add(NAME_None);
			Permissions.Add(ModPermissions::GameplayModify);
			Permissions.Add(ModPermissions::SaveModify);

			GetModRegistry()->SetGrantedPermissions(MakeTestId(TEXT("perm.mod")), Permissions);

			const FModInfo* Info = GetModRegistry()->FindMod(MakeTestId(TEXT("perm.mod")));
			if (TestNotNull(TEXT("the record exists"), Info)
				&& TestEqual(TEXT("two permissions survive"), Info->GrantedPermissions.Num(), 2))
			{
				ExpectString(TEXT("first"), Info->GrantedPermissions[0].ToString(), ModPermissions::GameplayModify.ToString());
				ExpectString(TEXT("second"), Info->GrantedPermissions[1].ToString(), ModPermissions::SaveModify.ToString());
			}
		});

		It(TEXT("SetMountedPaths drops duplicates and empty entries"), [this]()
		{
			if (!RegisterTestMod(TEXT("mount.mod")))
			{
				return;
			}

			TArray<FString> Paths;
			Paths.Add(TEXT("/Alpha/"));
			Paths.Add(FString());
			Paths.Add(TEXT("/Alpha/"));
			Paths.Add(TEXT("/Beta/"));

			GetModRegistry()->SetMountedPaths(MakeTestId(TEXT("mount.mod")), Paths);

			const FModInfo* Info = GetModRegistry()->FindMod(MakeTestId(TEXT("mount.mod")));
			if (TestNotNull(TEXT("the record exists"), Info)
				&& TestEqual(TEXT("two paths survive"), Info->MountedPaths.Num(), 2))
			{
				ExpectString(TEXT("first"), Info->MountedPaths[0], TEXT("/Alpha/"));
				ExpectString(TEXT("second"), Info->MountedPaths[1], TEXT("/Beta/"));
			}
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ The state-changed delegate
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("State changes"), [this]()
	{
		// DOCUMENTED GUARANTEE: OnModStateChanged fires AFTER the new state has been committed, never
		// while it is changing. A handler is the natural place to react to a mod going live, and it
		// has to be able to read the registry back and see what it was just told about - anything
		// else makes every handler race the registry.
		It(TEXT("fires after the new state is committed, so a handler reads back what it was told"), [this]()
		{
			if (!RegisterTestMod(TEXT("event.mod")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("event.mod"));

			int32 CallCount = 0;
			bool bSawCommittedState = false;
			FString ObservedOld;
			FString ObservedNew;

			const FDelegateHandle Handle = GetModRegistry()->OnModStateChanged.AddLambda(
				[this, &CallCount, &bSawCommittedState, &ObservedOld, &ObservedNew]
				(const FModId& ChangedId, EModState OldState, EModState NewState)
				{
					++CallCount;
					ObservedOld = ModFrameworkEnums::ToString(OldState);
					ObservedNew = ModFrameworkEnums::ToString(NewState);

					const FModInfo* Live = GetModRegistry()->FindMod(ChangedId);
					bSawCommittedState = Live != nullptr && Live->State == NewState;
				});

			TestTrue(TEXT("Discovered -> Validated"), GetModRegistry()->SetModState(ModId, EModState::Validated));

			GetModRegistry()->OnModStateChanged.Remove(Handle);

			TestEqual(TEXT("the handler ran exactly once"), CallCount, 1);
			TestTrue(TEXT("the handler saw the committed state"), bSawCommittedState);
			ExpectString(TEXT("old state"), ObservedOld, TEXT("Discovered"));
			ExpectString(TEXT("new state"), ObservedNew, TEXT("Validated"));
		});

		It(TEXT("does not fire for a refused transition or an unknown mod"), [this]()
		{
			if (!RegisterTestMod(TEXT("event.quiet")))
			{
				return;
			}

			int32 CallCount = 0;
			const FDelegateHandle Handle = GetModRegistry()->OnModStateChanged.AddLambda(
				[&CallCount](const FModId&, EModState, EModState)
				{
					++CallCount;
				});

			GetModRegistry()->SetModState(MakeTestId(TEXT("event.quiet")), EModState::Loaded);
			GetModRegistry()->SetModState(MakeTestId(TEXT("event.absent")), EModState::Validated);

			GetModRegistry()->OnModStateChanged.Remove(Handle);

			TestEqual(TEXT("no broadcast"), CallCount, 0);
		});

		It(TEXT("fires once per step of a multi-step walk, in order"), [this]()
		{
			if (!RegisterTestMod(TEXT("event.walk")))
			{
				return;
			}

			TArray<FString> Seen;
			const FDelegateHandle Handle = GetModRegistry()->OnModStateChanged.AddLambda(
				[&Seen](const FModId&, EModState OldState, EModState NewState)
				{
					Seen.Add(FString::Printf(TEXT("%s->%s"),
						*ModFrameworkEnums::ToString(OldState), *ModFrameworkEnums::ToString(NewState)));
				});

			AdvanceModTo(MakeTestId(TEXT("event.walk")), EModState::Loaded);

			GetModRegistry()->OnModStateChanged.Remove(Handle);

			ExpectString(TEXT("the broadcast sequence"), FString::Join(Seen, TEXT(", ")),
				TEXT("Discovered->Validated, Validated->DependenciesResolved, DependenciesResolved->Mounted, ")
				TEXT("Mounted->Loading, Loading->Loaded"));
		});

		// SetModFailed records the diagnostic BEFORE it moves the mod, so a handler that reacts to
		// Failed by showing the player why already has something to show.
		It(TEXT("has the failure recorded before the Failed broadcast reaches a handler"), [this]()
		{
			if (!RegisterTestMod(TEXT("event.fail")))
			{
				return;
			}

			bool bReasonVisible = false;
			bool bDiagnosticVisible = false;

			const FDelegateHandle Handle = GetModRegistry()->OnModStateChanged.AddLambda(
				[this, &bReasonVisible, &bDiagnosticVisible](const FModId& ChangedId, EModState, EModState NewState)
				{
					if (NewState != EModState::Failed)
					{
						return;
					}

					const FModInfo* Live = GetModRegistry()->FindMod(ChangedId);
					bReasonVisible = Live != nullptr && Live->FailureReason == EModLoadFailureReason::EntryPointMissing;
					bDiagnosticVisible = Live != nullptr && Live->Diagnostics.Num() > 0;
				});

			GetModRegistry()->SetModFailed(MakeTestId(TEXT("event.fail")),
				EModLoadFailureReason::EntryPointMissing, TEXT("no entry point class"));

			GetModRegistry()->OnModStateChanged.Remove(Handle);

			TestTrue(TEXT("the handler saw the failure reason"), bReasonVisible);
			TestTrue(TEXT("the handler saw the diagnostic"), bDiagnosticVisible);
		});

		// A handler is explicitly allowed to mutate the registry, which is why SetModState copies the
		// id before broadcasting and never touches its FModInfo pointer afterwards.
		It(TEXT("survives a handler that unregisters the mod it is being told about"), [this]()
		{
			if (!RegisterTestMod(TEXT("event.suicide")))
			{
				return;
			}

			const FDelegateHandle Handle = GetModRegistry()->OnModStateChanged.AddLambda(
				[this](const FModId& ChangedId, EModState, EModState)
				{
					GetModRegistry()->UnregisterMod(ChangedId);
				});

			TestTrue(TEXT("the transition still reports success"),
				GetModRegistry()->SetModState(MakeTestId(TEXT("event.suicide")), EModState::Validated));

			GetModRegistry()->OnModStateChanged.Remove(Handle);

			TestEqual(TEXT("the handler's unregister took effect"), GetModRegistry()->GetModCount(), 0);
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Mod-owned object tracking
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Mod objects"), [this]()
	{
		It(TEXT("tracks an object and finds its owner again"), [this]()
		{
			if (!RegisterTestMod(TEXT("obj.owner")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("obj.owner"));
			UObject* Owned = NewObject<UModRegistry>(GetTransientPackage());

			GetModRegistry()->TrackModObject(ModId, Owned);

			const TArray<UObject*> Tracked = GetModRegistry()->GetModObjects(ModId);
			if (TestEqual(TEXT("one object is tracked"), Tracked.Num(), 1))
			{
				TestTrue(TEXT("it is the object that was handed over"), Tracked[0] == Owned);
			}

			ExpectString(TEXT("reverse lookup"), GetModRegistry()->FindOwningMod(Owned).ToString(), TEXT("obj.owner"));

			// Tracking the same object for the same mod twice must not duplicate it, or unloading
			// would try to release it twice.
			GetModRegistry()->TrackModObject(ModId, Owned);
			TestEqual(TEXT("tracking is idempotent"), GetModRegistry()->GetModObjects(ModId).Num(), 1);
		});

		It(TEXT("refuses a null object, a garbage object and an invalid mod id"), [this]()
		{
			if (!RegisterTestMod(TEXT("obj.guard")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("obj.guard"));

			GetModRegistry()->TrackModObject(ModId, nullptr);
			TestEqual(TEXT("a null object is not tracked"), GetModRegistry()->GetModObjects(ModId).Num(), 0);

			UObject* Dead = NewObject<UModRegistry>(GetTransientPackage());
			Dead->MarkAsGarbage();
			GetModRegistry()->TrackModObject(ModId, Dead);
			TestEqual(TEXT("a garbage object is not tracked"), GetModRegistry()->GetModObjects(ModId).Num(), 0);

			UObject* Live = NewObject<UModRegistry>(GetTransientPackage());
			GetModRegistry()->TrackModObject(FModId(), Live);
			TestFalse(TEXT("an invalid mod id owns nothing"), GetModRegistry()->FindOwningMod(Live).IsValid());
		});

		// Re-pointing ownership would let one mod's unload destroy another mod's object, so the first
		// claim wins and the second is only reported.
		It(TEXT("keeps the first owner when a second mod claims the same object"), [this]()
		{
			RegisterTestMod(TEXT("obj.first"));
			RegisterTestMod(TEXT("obj.second"));

			UObject* Contested = NewObject<UModRegistry>(GetTransientPackage());

			GetModRegistry()->TrackModObject(MakeTestId(TEXT("obj.first")), Contested);
			GetModRegistry()->TrackModObject(MakeTestId(TEXT("obj.second")), Contested);

			ExpectString(TEXT("owner"), GetModRegistry()->FindOwningMod(Contested).ToString(), TEXT("obj.first"));
			TestEqual(TEXT("the first mod still tracks it"),
				GetModRegistry()->GetModObjects(MakeTestId(TEXT("obj.first"))).Num(), 1);
			TestEqual(TEXT("the second mod tracks nothing"),
				GetModRegistry()->GetModObjects(MakeTestId(TEXT("obj.second"))).Num(), 0);
		});

		It(TEXT("untracks an object without destroying it, and only for its recorded owner"), [this]()
		{
			RegisterTestMod(TEXT("obj.keeper"));
			RegisterTestMod(TEXT("obj.meddler"));

			const FModId Keeper = MakeTestId(TEXT("obj.keeper"));
			UObject* Owned = NewObject<UModRegistry>(GetTransientPackage());
			GetModRegistry()->TrackModObject(Keeper, Owned);

			// An untrack naming the wrong mod must not orphan another mod's object.
			GetModRegistry()->UntrackModObject(MakeTestId(TEXT("obj.meddler")), Owned);
			ExpectString(TEXT("owner after the wrong mod untracks"),
				GetModRegistry()->FindOwningMod(Owned).ToString(), TEXT("obj.keeper"));
			TestEqual(TEXT("still tracked"), GetModRegistry()->GetModObjects(Keeper).Num(), 1);

			GetModRegistry()->UntrackModObject(Keeper, Owned);
			TestEqual(TEXT("no longer tracked"), GetModRegistry()->GetModObjects(Keeper).Num(), 0);
			TestFalse(TEXT("no longer owned"), GetModRegistry()->FindOwningMod(Owned).IsValid());

			// Untracking forgets, it does not destroy: the caller may still be using the object.
			TestTrue(TEXT("the object is still alive"), ::IsValid(Owned));
		});

		It(TEXT("ReleaseModObjects marks every tracked object garbage and forgets it"), [this]()
		{
			if (!RegisterTestMod(TEXT("obj.release")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("obj.release"));

			UObject* First = NewObject<UModRegistry>(GetTransientPackage());
			UObject* Second = NewObject<UModRegistry>(GetTransientPackage());
			GetModRegistry()->TrackModObject(ModId, First);
			GetModRegistry()->TrackModObject(ModId, Second);

			GetModRegistry()->ReleaseModObjects(ModId);

			TestEqual(TEXT("nothing is tracked afterwards"), GetModRegistry()->GetModObjects(ModId).Num(), 0);
			TestFalse(TEXT("the first is no longer owned"), GetModRegistry()->FindOwningMod(First).IsValid());
			TestFalse(TEXT("the second is no longer owned"), GetModRegistry()->FindOwningMod(Second).IsValid());
			TestFalse(TEXT("the first is garbage"), ::IsValid(First));
			TestFalse(TEXT("the second is garbage"), ::IsValid(Second));
		});

		It(TEXT("leaves a rooted object alive but still forgets it"), [this]()
		{
			if (!RegisterTestMod(TEXT("obj.rooted")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("obj.rooted"));

			UObject* Rooted = NewObject<UModRegistry>(GetTransientPackage());
			Rooted->AddToRoot();
			GetModRegistry()->TrackModObject(ModId, Rooted);

			// Marking a rooted object garbage is fatal in the engine, and force-unrooting something
			// somebody deliberately rooted would be worse than leaking it.
			GetModRegistry()->ReleaseModObjects(ModId);

			TestTrue(TEXT("the rooted object survives"), ::IsValid(Rooted));
			TestEqual(TEXT("but the registry forgot it"), GetModRegistry()->GetModObjects(ModId).Num(), 0);
			TestFalse(TEXT("and it has no owner"), GetModRegistry()->FindOwningMod(Rooted).IsValid());

			Rooted->RemoveFromRoot();
		});

		It(TEXT("GetModObjects skips an object that died behind the registry's back"), [this]()
		{
			if (!RegisterTestMod(TEXT("obj.stale")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("obj.stale"));

			UObject* Doomed = NewObject<UModRegistry>(GetTransientPackage());
			UObject* Survivor = NewObject<UModRegistry>(GetTransientPackage());
			GetModRegistry()->TrackModObject(ModId, Doomed);
			GetModRegistry()->TrackModObject(ModId, Survivor);

			Doomed->MarkAsGarbage();

			const TArray<UObject*> Tracked = GetModRegistry()->GetModObjects(ModId);
			if (TestEqual(TEXT("only the live object comes back"), Tracked.Num(), 1))
			{
				TestTrue(TEXT("and it is the survivor"), Tracked[0] == Survivor);
			}
		});

		// The reverse index is swept amortised - once per doubling, starting at the 64 entries the
		// header initialises ObjectOwnersSweepThreshold to - so tracking and lookup both stay O(1).
		// The observable consequence is that entries for dead objects stop answering FindOwningMod.
		It(TEXT("prunes reverse-lookup entries whose object has died"), [this]()
		{
			if (!RegisterTestMod(TEXT("obj.prune")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("obj.prune"));

			TArray<UObject*> Doomed;
			for (int32 Index = 0; Index < 8; ++Index)
			{
				UObject* Object = NewObject<UModRegistry>(GetTransientPackage());
				GetModRegistry()->TrackModObject(ModId, Object);
				Doomed.Add(Object);
			}

			for (UObject* Object : Doomed)
			{
				Object->MarkAsGarbage();
			}

			// Before a sweep the entry is still in the index, so the dead object still resolves.
			ExpectString(TEXT("a dead object still resolves before the sweep"),
				GetModRegistry()->FindOwningMod(Doomed[0]).ToString(), TEXT("obj.prune"));

			// Comfortably past the initial threshold of 64, so at least one sweep has to have run.
			UObject* LastLive = nullptr;
			for (int32 Index = 0; Index < 96; ++Index)
			{
				LastLive = NewObject<UModRegistry>(GetTransientPackage());
				GetModRegistry()->TrackModObject(ModId, LastLive);
			}

			for (int32 Index = 0; Index < Doomed.Num(); ++Index)
			{
				const FString FailureMessage = FString::Printf(
					TEXT("dead object %d was pruned out of the reverse index"), Index);
				TestFalse(*FailureMessage, GetModRegistry()->FindOwningMod(Doomed[Index]).IsValid());
			}

			// Live entries are untouched by the sweep.
			ExpectString(TEXT("a live object still resolves after the sweep"),
				GetModRegistry()->FindOwningMod(LastLive).ToString(), TEXT("obj.prune"));
			TestEqual(TEXT("only the live objects are still tracked"), GetModRegistry()->GetModObjects(ModId).Num(), 96);
		});

		It(TEXT("FindOwningMod returns an invalid id for null and for an untracked object"), [this]()
		{
			UObject* Stranger = NewObject<UModRegistry>(GetTransientPackage());

			TestFalse(TEXT("null has no owner"), GetModRegistry()->FindOwningMod(nullptr).IsValid());
			TestFalse(TEXT("an untracked object has no owner"), GetModRegistry()->FindOwningMod(Stranger).IsValid());
		});

		It(TEXT("unregistering a mod releases the objects it owned"), [this]()
		{
			if (!RegisterTestMod(TEXT("obj.unreg")))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("obj.unreg"));
			UObject* Owned = NewObject<UModRegistry>(GetTransientPackage());
			GetModRegistry()->TrackModObject(ModId, Owned);

			TestTrue(TEXT("unregisters"), GetModRegistry()->UnregisterMod(ModId));

			TestFalse(TEXT("the object was released"), ::IsValid(Owned));
			TestFalse(TEXT("and has no owner"), GetModRegistry()->FindOwningMod(Owned).IsValid());
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ API registry
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("APIs"), [this]()
	{
		It(TEXT("registers a game API and publishes a descriptor that matches it"), [this]()
		{
			UModAPI* Api = MakeTestApi(TEXT("game.combat"), TEXT("2.1.0"));
			if (Api == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			TestTrue(TEXT("registers"), GetApiRegistry()->RegisterAPI(Api, Error));
			ExpectNoDiagnostic(TEXT("a successful registration"), Error);

			TestTrue(TEXT("HasAPI"), GetApiRegistry()->HasAPI(FName(TEXT("game.combat"))));
			TestTrue(TEXT("FindAPI returns the object"), GetApiRegistry()->FindAPI(FName(TEXT("game.combat"))) == Api);

			FModAPIDescriptor Descriptor;
			if (TestTrue(TEXT("GetAPIDescriptor"), GetApiRegistry()->GetAPIDescriptor(FName(TEXT("game.combat")), Descriptor)))
			{
				ExpectString(TEXT("descriptor id"), Descriptor.ApiId.ToString(), TEXT("game.combat"));
				ExpectString(TEXT("descriptor version"), Descriptor.Version.ToString(), TEXT("2.1.0"));
				ExpectString(TEXT("descriptor class path"), Descriptor.ClassPath, Api->GetClass()->GetPathName());

				// An API the game itself provides has no provider mod, which is what stops
				// UnregisterAllForMod from ever taking it away.
				TestFalse(TEXT("no provider mod"), Descriptor.ProviderModId.IsValid());
			}
		});

		It(TEXT("GetAPIDescriptor resets the output for an unknown id"), [this]()
		{
			FModAPIDescriptor Descriptor;
			Descriptor.ApiId = FName(TEXT("stale"));

			TestFalse(TEXT("unknown id"), GetApiRegistry()->GetAPIDescriptor(FName(TEXT("game.absent")), Descriptor));
			TestTrue(TEXT("the stale value is cleared"), Descriptor.ApiId.IsNone());
		});

		It(TEXT("rejects a null API and a garbage one"), [this]()
		{
			FModDiagnostic Error;
			TestFalse(TEXT("a null API"), GetApiRegistry()->RegisterAPI(nullptr, Error));
			ExpectDiagnostic(TEXT("a null API"), Error, TEXT("API.NullApi"));

			UModAPI* Dead = MakeTestApi(TEXT("game.dead"));
			if (Dead == nullptr)
			{
				return;
			}
			Dead->MarkAsGarbage();

			TestFalse(TEXT("a garbage API"), GetApiRegistry()->RegisterAPI(Dead, Error));
			ExpectDiagnostic(TEXT("a garbage API"), Error, TEXT("API.NullApi"));
			TestEqual(TEXT("nothing was registered"), GetApiRegistry()->GetAllAPIs().Num(), 0);
		});

		It(TEXT("rejects a second API claiming an id that is already taken"), [this]()
		{
			UModAPI* First = MakeTestApi(TEXT("game.dup"), TEXT("1.0.0"));
			UModAPI* Second = MakeTestApi(TEXT("game.dup"), TEXT("9.0.0"));
			if (First == nullptr || Second == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			TestTrue(TEXT("the first registers"), GetApiRegistry()->RegisterAPI(First, Error));
			TestFalse(TEXT("the second is refused"), GetApiRegistry()->RegisterAPI(Second, Error));
			ExpectDiagnostic(TEXT("a duplicate id"), Error, TEXT("API.Duplicate"));

			// The incumbent must survive: swapping it out would hand every mod that already holds a
			// pointer to it an object the registry no longer knows about.
			TestTrue(TEXT("the first API is still the one on file"),
				GetApiRegistry()->FindAPI(FName(TEXT("game.dup"))) == First);
			TestEqual(TEXT("one registration"), GetApiRegistry()->GetAllAPIs().Num(), 1);
		});

		It(TEXT("rejects registering one API object under two ids"), [this]()
		{
			using namespace ModLifecycleTestsPrivate;

			UModAPI* Api = MakeTestApi(TEXT("game.once"));
			if (Api == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			TestTrue(TEXT("registers"), GetApiRegistry()->RegisterAPI(Api, Error));

			// Re-identify the same object and try again. Registering it twice would notify - and tear
			// down - the same object twice.
			WriteAuthoredProperty<FName>(Api, TEXT("ApiIdOverride"), FName(TEXT("game.twice")));
			Api->InvalidateResolvedIdentity();

			TestFalse(TEXT("the same object under a second id is refused"), GetApiRegistry()->RegisterAPI(Api, Error));
			ExpectDiagnostic(TEXT("a duplicate object"), Error, TEXT("API.Duplicate"));
			TestEqual(TEXT("one registration"), GetApiRegistry()->GetAllAPIs().Num(), 1);
		});

		It(TEXT("GetAllAPIs is sorted by id"), [this]()
		{
			FModDiagnostic Error;
			GetApiRegistry()->RegisterAPI(MakeTestApi(TEXT("game.zulu")), Error);
			GetApiRegistry()->RegisterAPI(MakeTestApi(TEXT("game.alpha")), Error);
			GetApiRegistry()->RegisterAPI(MakeTestApi(TEXT("game.mike")), Error);

			const TArray<FModAPIDescriptor> All = GetApiRegistry()->GetAllAPIs();
			if (TestEqual(TEXT("three APIs"), All.Num(), 3))
			{
				ExpectString(TEXT("first"), All[0].ApiId.ToString(), TEXT("game.alpha"));
				ExpectString(TEXT("second"), All[1].ApiId.ToString(), TEXT("game.mike"));
				ExpectString(TEXT("third"), All[2].ApiId.ToString(), TEXT("game.zulu"));
			}
		});

		It(TEXT("RequestAPI reports API.NotFound for an unknown id"), [this]()
		{
			FModDiagnostic Error;
			UModAPI* Result = GetApiRegistry()->RequestAPI(FName(TEXT("game.absent")), FModVersionRange::Any(),
				MakeTestId(TEXT("api.consumer")), Error);

			TestNull(TEXT("nothing comes back"), Result);
			ExpectDiagnostic(TEXT("an unknown id"), Error, TEXT("API.NotFound"));
			ExpectString(TEXT("the diagnostic names the requesting mod"), Error.ModId.ToString(), TEXT("api.consumer"));
		});

		It(TEXT("RequestAPI accepts a version inside the requested range and refuses one outside it"), [this]()
		{
			UModAPI* Api = MakeTestApi(TEXT("game.versioned"), TEXT("2.1.0"));
			if (Api == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			if (!TestTrue(TEXT("registers"), GetApiRegistry()->RegisterAPI(Api, Error)))
			{
				return;
			}

			const FModId Consumer = MakeTestId(TEXT("api.consumer"));

			FModVersionRange Compatible;
			FModVersionRange TooNew;
			FModVersionRange TooOld;
			TestTrue(TEXT("^2.0.0 parses"), FModVersionRange::Parse(TEXT("^2.0.0"), Compatible));
			TestTrue(TEXT(">=3.0.0 parses"), FModVersionRange::Parse(TEXT(">=3.0.0"), TooNew));
			TestTrue(TEXT("<2.0.0 parses"), FModVersionRange::Parse(TEXT("<2.0.0"), TooOld));

			TestTrue(TEXT("^2.0.0 is satisfied by 2.1.0"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.versioned")), Compatible, Consumer, Error) == Api);
			ExpectNoDiagnostic(TEXT("an accepted request"), Error);

			TestNull(TEXT(">=3.0.0 is not"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.versioned")), TooNew, Consumer, Error));
			ExpectDiagnostic(TEXT("a version that is too old for the request"), Error, TEXT("API.VersionMismatch"));

			// The refusal has to say what it found as well as what it wanted, or a mod author cannot
			// tell a typo from a genuinely incompatible game build.
			TestTrue(TEXT("the diagnostic names the registered version"),
				Error.Message.Contains(TEXT("2.1.0"), ESearchCase::CaseSensitive));

			TestNull(TEXT("<2.0.0 is not either"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.versioned")), TooOld, Consumer, Error));
			ExpectDiagnostic(TEXT("a version that is too new for the request"), Error, TEXT("API.VersionMismatch"));

			// A range that failed to parse must accept nothing, rather than being silently widened.
			FModVersionRange Malformed;
			TestFalse(TEXT("the malformed range does not parse"), FModVersionRange::Parse(TEXT("!=2.1"), Malformed));
			TestNull(TEXT("a malformed range accepts nothing"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.versioned")), Malformed, Consumer, Error));
			ExpectDiagnostic(TEXT("a malformed range"), Error, TEXT("API.VersionMismatch"));

			// NOTE: the comment on UModAPIRegistry::RequestAPI says an "empty or unparsed" range
			// accepts nothing. That holds for an unparsed *malformed* range, tested above, but a
			// default-constructed range has an empty Expression, and FModVersionRange treats an empty
			// expression as "any" (see FModVersionRange::Satisfies and the Manifest spec's list of
			// any-forms). This asserts what the code actually does.
			const FModVersionRange DefaultConstructed;
			TestTrue(TEXT("a default-constructed range behaves as any"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.versioned")), DefaultConstructed, Consumer, Error) == Api);
		});

		It(TEXT("refuses a mod that does not hold a required permission"), [this]()
		{
			TArray<FName> Required;
			Required.Add(ModPermissions::GameplayModify);

			UModAPI* Api = MakeTestApi(TEXT("game.gated"), TEXT("1.0.0"), Required);
			if (Api == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			if (!TestTrue(TEXT("registers"), GetApiRegistry()->RegisterAPI(Api, Error)))
			{
				return;
			}

			const FModId Allowed = MakeTestId(TEXT("api.allowed"));
			const FModId Denied = MakeTestId(TEXT("api.denied"));

			GetApiRegistry()->SetPermissionCheck(UModAPIRegistry::FModPermissionCheck::CreateLambda(
				[Allowed](const FModId& ModId, FName Permission)
				{
					return ModId == Allowed && Permission == ModPermissions::GameplayModify;
				}));

			TestTrue(TEXT("the permitted mod gets the API"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.gated")), FModVersionRange::Any(), Allowed, Error) == Api);

			TestNull(TEXT("the unpermitted mod does not"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.gated")), FModVersionRange::Any(), Denied, Error));
			ExpectDiagnostic(TEXT("a denied permission"), Error, TEXT("API.PermissionDenied"));
			TestTrue(TEXT("the diagnostic names the permission"),
				Error.Message.Contains(ModPermissions::GameplayModify.ToString(), ESearchCase::CaseSensitive));

			// An invalid mod id is checked like any other, so passing a blank id is not a way past
			// the gate.
			TestNull(TEXT("a blank mod id is not a way around the gate"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.gated")), FModVersionRange::Any(), FModId(), Error));
			ExpectDiagnostic(TEXT("a blank requesting mod"), Error, TEXT("API.PermissionDenied"));
		});

		// DOCUMENTED GUARANTEE: the permission gate fails CLOSED. An API that guards itself with a
		// permission must never be handed out merely because nothing has wired the permission
		// registry up yet - that would turn a missing subsystem into a silent grant.
		It(TEXT("refuses a gated API while no permission check is installed"), [this]()
		{
			TArray<FName> Required;
			Required.Add(ModPermissions::SaveModify);

			UModAPI* Gated = MakeTestApi(TEXT("game.closed"), TEXT("1.0.0"), Required);
			UModAPI* Ungated = MakeTestApi(TEXT("game.open"), TEXT("1.0.0"));
			if (Gated == nullptr || Ungated == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			GetApiRegistry()->RegisterAPI(Gated, Error);
			GetApiRegistry()->RegisterAPI(Ungated, Error);

			const FModId Consumer = MakeTestId(TEXT("api.consumer"));

			TestNull(TEXT("the gated API is refused"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.closed")), FModVersionRange::Any(), Consumer, Error));
			ExpectDiagnostic(TEXT("no check installed"), Error, TEXT("API.PermissionDenied"));

			// The message has to distinguish "you lack this permission" from "nothing can answer that
			// question yet", because the fix for each is completely different.
			TestTrue(TEXT("the diagnostic says why"),
				Error.Message.Contains(TEXT("no permission check is installed"), ESearchCase::CaseSensitive));

			// An API that declares no permissions is unaffected, which is what keeps editor tooling
			// and this very spec working without wiring anything up.
			TestTrue(TEXT("an ungated API is still handed out"),
				GetApiRegistry()->RequestAPI(FName(TEXT("game.open")), FModVersionRange::Any(), Consumer, Error) == Ungated);

			// An all-blank permission list counts as ungated too.
			TArray<FName> Blank;
			Blank.Add(NAME_None);
			UModAPI* BlankGated = MakeTestApi(TEXT("game.blank"), TEXT("1.0.0"), Blank);
			if (BlankGated != nullptr && GetApiRegistry()->RegisterAPI(BlankGated, Error))
			{
				TestTrue(TEXT("a blank permission list does not gate anything"),
					GetApiRegistry()->RequestAPI(FName(TEXT("game.blank")), FModVersionRange::Any(), Consumer, Error) == BlankGated);
			}
		});

		It(TEXT("K2_RequestAPI refuses a class the registered API is not"), [this]()
		{
			UModAPI* Api = MakeTestApi(TEXT("game.typed"), TEXT("1.0.0"));
			if (Api == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			if (!TestTrue(TEXT("registers"), GetApiRegistry()->RegisterAPI(Api, Error)))
			{
				return;
			}

			const FModId Consumer = MakeTestId(TEXT("api.consumer"));

			// A Blueprint treats the result as an ApiClass because of DeterminesOutputType, so the
			// cast has to be real - handing back a wrongly typed object would be undetectable there.
			UClass* Unrelated = MakeTypeToken(UModAPI::StaticClass(), TEXT("ModLifecycleSpec_OtherApi"));

			TestNull(TEXT("a mismatched class is refused"),
				GetApiRegistry()->K2_RequestAPI(FName(TEXT("game.typed")), Unrelated, FString(), Consumer, Error));
			ExpectDiagnostic(TEXT("a class mismatch"), Error, TEXT("API.ClassMismatch"));

			TestTrue(TEXT("the matching class is accepted"),
				GetApiRegistry()->K2_RequestAPI(FName(TEXT("game.typed")), UModAPI::StaticClass(), FString(), Consumer, Error) == Api);

			// A null class means "do not type-check".
			TestTrue(TEXT("a null class skips the check"),
				GetApiRegistry()->K2_RequestAPI(FName(TEXT("game.typed")), nullptr, FString(), Consumer, Error) == Api);
		});

		It(TEXT("K2_RequestAPI treats an empty range as any and refuses a malformed one"), [this]()
		{
			UModAPI* Api = MakeTestApi(TEXT("game.text"), TEXT("1.4.2"));
			if (Api == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			if (!TestTrue(TEXT("registers"), GetApiRegistry()->RegisterAPI(Api, Error)))
			{
				return;
			}

			const FModId Consumer = MakeTestId(TEXT("api.consumer"));

			TestTrue(TEXT("an empty range means any version"),
				GetApiRegistry()->K2_RequestAPI(FName(TEXT("game.text")), nullptr, FString(), Consumer, Error) == Api);

			TestTrue(TEXT("a satisfied range is accepted"),
				GetApiRegistry()->K2_RequestAPI(FName(TEXT("game.text")), nullptr, TEXT("^1.4.0"), Consumer, Error) == Api);

			TestNull(TEXT("an unsatisfied range is refused"),
				GetApiRegistry()->K2_RequestAPI(FName(TEXT("game.text")), nullptr, TEXT("^2.0.0"), Consumer, Error));
			ExpectDiagnostic(TEXT("an unsatisfied range"), Error, TEXT("API.VersionMismatch"));

			// A mod that asked for "!=1.4.2" and typo'd it must not be handed the API anyway.
			TestNull(TEXT("a malformed range is refused rather than widened"),
				GetApiRegistry()->K2_RequestAPI(FName(TEXT("game.text")), nullptr, TEXT("!=1.4"), Consumer, Error));
			ExpectDiagnostic(TEXT("a malformed range"), Error, TEXT("API.VersionMismatch"));
		});

		It(TEXT("UnregisterAPI removes one API and reports an unknown id"), [this]()
		{
			UModAPI* Api = MakeTestApi(TEXT("game.temp"));
			if (Api == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			GetApiRegistry()->RegisterAPI(Api, Error);

			TArray<FString> Unregistered;
			const FDelegateHandle Handle = GetApiRegistry()->OnAPIUnregistered.AddLambda(
				[&Unregistered](FName ApiId)
				{
					Unregistered.Add(ApiId.ToString());
				});

			TestFalse(TEXT("an unknown id"), GetApiRegistry()->UnregisterAPI(FName(TEXT("game.absent"))));
			TestTrue(TEXT("a known id"), GetApiRegistry()->UnregisterAPI(FName(TEXT("game.temp"))));

			GetApiRegistry()->OnAPIUnregistered.Remove(Handle);

			TestFalse(TEXT("HasAPI afterwards"), GetApiRegistry()->HasAPI(FName(TEXT("game.temp"))));
			ExpectString(TEXT("the change delegate fired once, for the right id"),
				FString::Join(Unregistered, TEXT(", ")), TEXT("game.temp"));
		});

		It(TEXT("UnregisterAllForMod takes away one mod's APIs and leaves the game's alone"), [this]()
		{
			UModAPI* GameApi = MakeTestApi(TEXT("game.core"));
			UModAPI* ModApiOne = MakeTestApi(TEXT("mod.one.first"));
			UModAPI* ModApiTwo = MakeTestApi(TEXT("mod.one.second"));
			UModAPI* OtherModApi = MakeTestApi(TEXT("mod.two.only"));
			if (GameApi == nullptr || ModApiOne == nullptr || ModApiTwo == nullptr || OtherModApi == nullptr)
			{
				return;
			}

			const FModId ModOne = MakeTestId(TEXT("api.modone"));
			const FModId ModTwo = MakeTestId(TEXT("api.modtwo"));

			FModDiagnostic Error;
			GetApiRegistry()->RegisterAPI(GameApi, Error);
			GetApiRegistry()->RegisterAPIForMod(ModApiOne, ModOne, Error);
			GetApiRegistry()->RegisterAPIForMod(ModApiTwo, ModOne, Error);
			GetApiRegistry()->RegisterAPIForMod(OtherModApi, ModTwo, Error);
			TestEqual(TEXT("four APIs are registered"), GetApiRegistry()->GetAllAPIs().Num(), 4);

			FModAPIDescriptor Descriptor;
			if (GetApiRegistry()->GetAPIDescriptor(FName(TEXT("mod.one.first")), Descriptor))
			{
				ExpectString(TEXT("a mod-provided API records its provider"),
					Descriptor.ProviderModId.ToString(), TEXT("api.modone"));
			}

			GetApiRegistry()->UnregisterAllForMod(ModOne);

			TestFalse(TEXT("the first mod API is gone"), GetApiRegistry()->HasAPI(FName(TEXT("mod.one.first"))));
			TestFalse(TEXT("the second mod API is gone"), GetApiRegistry()->HasAPI(FName(TEXT("mod.one.second"))));
			TestTrue(TEXT("the other mod's API survives"), GetApiRegistry()->HasAPI(FName(TEXT("mod.two.only"))));
			TestTrue(TEXT("the game's API survives"), GetApiRegistry()->HasAPI(FName(TEXT("game.core"))));

			// An invalid provider id marks a game-provided API, which no mod may ever take away.
			GetApiRegistry()->UnregisterAllForMod(FModId());
			TestTrue(TEXT("an invalid mod id removes nothing"), GetApiRegistry()->HasAPI(FName(TEXT("game.core"))));
		});

		It(TEXT("Reset removes every API and notifies each one once"), [this]()
		{
			FModDiagnostic Error;
			GetApiRegistry()->RegisterAPI(MakeTestApi(TEXT("game.reset.b")), Error);
			GetApiRegistry()->RegisterAPI(MakeTestApi(TEXT("game.reset.a")), Error);

			TArray<FString> Unregistered;
			const FDelegateHandle Handle = GetApiRegistry()->OnAPIUnregistered.AddLambda(
				[&Unregistered](FName ApiId)
				{
					Unregistered.Add(ApiId.ToString());
				});

			GetApiRegistry()->Reset();

			GetApiRegistry()->OnAPIUnregistered.Remove(Handle);

			TestEqual(TEXT("nothing is registered afterwards"), GetApiRegistry()->GetAllAPIs().Num(), 0);

			// Sorted, so console output and tests read the same on every machine.
			ExpectString(TEXT("each API was notified exactly once, in id order"),
				FString::Join(Unregistered, TEXT(", ")), TEXT("game.reset.a, game.reset.b"));
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Extension points
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Extension points"), [this]()
	{
		It(TEXT("opens a point and hands the descriptor back"), [this]()
		{
			FModExtensionPointDescriptor Point = MakeTestPoint(TEXT("game.weapon"));
			Point.DefaultConflictPolicy = EModConflictPolicy::Priority;
			Point.bAllowMultiplePerMod = false;

			if (!RegisterTestPoint(Point))
			{
				return;
			}

			FModExtensionPointDescriptor Stored;
			if (TestTrue(TEXT("GetExtensionPoint"), GetExtensionRegistry()->GetExtensionPoint(FName(TEXT("game.weapon")), Stored)))
			{
				ExpectString(TEXT("id"), Stored.ExtensionPointId.ToString(), TEXT("game.weapon"));
				ExpectString(TEXT("policy"), ModFrameworkEnums::ToString(Stored.DefaultConflictPolicy), TEXT("Priority"));
				TestFalse(TEXT("quota"), Stored.bAllowMultiplePerMod);
			}

			// The bucket is created up front, so every later lookup can assume the two maps agree.
			TestEqual(TEXT("a new point starts empty"), GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.weapon"))).Num(), 0);
		});

		It(TEXT("GetExtensionPoint resets the output for an unknown id"), [this]()
		{
			FModExtensionPointDescriptor Stored = MakeTestPoint(TEXT("stale"));

			TestFalse(TEXT("unknown id"), GetExtensionRegistry()->GetExtensionPoint(FName(TEXT("game.absent")), Stored));
			TestTrue(TEXT("the stale descriptor is cleared"), Stored.ExtensionPointId.IsNone());
		});

		It(TEXT("rejects a point with no id"), [this]()
		{
			FModExtensionPointDescriptor Point;

			FModDiagnostic Error;
			TestFalse(TEXT("does not register"), GetExtensionRegistry()->RegisterExtensionPoint(Point, Error));
			ExpectDiagnostic(TEXT("a point with no id"), Error, TEXT("ExtensionPoint.InvalidId"));
			TestEqual(TEXT("nothing was opened"), GetExtensionRegistry()->GetExtensionPoints().Num(), 0);
		});

		It(TEXT("rejects a duplicate point and keeps the original descriptor"), [this]()
		{
			FModExtensionPointDescriptor First = MakeTestPoint(TEXT("game.dup"));
			First.DefaultConflictPolicy = EModConflictPolicy::FirstWins;

			FModExtensionPointDescriptor Second = MakeTestPoint(TEXT("game.dup"));
			Second.DefaultConflictPolicy = EModConflictPolicy::Merge;

			if (!RegisterTestPoint(First))
			{
				return;
			}

			FModDiagnostic Error;
			TestFalse(TEXT("the second is refused"), GetExtensionRegistry()->RegisterExtensionPoint(Second, Error));
			ExpectDiagnostic(TEXT("a duplicate point"), Error, TEXT("ExtensionPoint.Duplicate"));

			// Registering a point never replaces an existing one: the extensions already filed under
			// it were validated against the old descriptor.
			FModExtensionPointDescriptor Stored;
			if (TestTrue(TEXT("GetExtensionPoint"), GetExtensionRegistry()->GetExtensionPoint(FName(TEXT("game.dup")), Stored)))
			{
				ExpectString(TEXT("the original policy survives"),
					ModFrameworkEnums::ToString(Stored.DefaultConflictPolicy), TEXT("FirstWins"));
			}
		});

		It(TEXT("GetExtensionPoints is sorted by id"), [this]()
		{
			RegisterTestPoint(MakeTestPoint(TEXT("game.zulu")));
			RegisterTestPoint(MakeTestPoint(TEXT("game.alpha")));
			RegisterTestPoint(MakeTestPoint(TEXT("game.mike")));

			const TArray<FModExtensionPointDescriptor> Points = GetExtensionRegistry()->GetExtensionPoints();
			if (TestEqual(TEXT("three points"), Points.Num(), 3))
			{
				ExpectString(TEXT("first"), Points[0].ExtensionPointId.ToString(), TEXT("game.alpha"));
				ExpectString(TEXT("second"), Points[1].ExtensionPointId.ToString(), TEXT("game.mike"));
				ExpectString(TEXT("third"), Points[2].ExtensionPointId.ToString(), TEXT("game.zulu"));
			}
		});

		It(TEXT("closes an empty point and reports an unknown one"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.transient"))))
			{
				return;
			}

			TestFalse(TEXT("an unknown point"), GetExtensionRegistry()->UnregisterExtensionPoint(FName(TEXT("game.absent"))));
			TestTrue(TEXT("an empty point"), GetExtensionRegistry()->UnregisterExtensionPoint(FName(TEXT("game.transient"))));
			TestEqual(TEXT("nothing is left"), GetExtensionRegistry()->GetExtensionPoints().Num(), 0);
		});

		// Closing a point out from under live extensions would leave them orphaned but still
		// referenced, so the registry refuses until they are gone.
		It(TEXT("refuses to close a point that still has extensions"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.busy"))))
			{
				return;
			}

			if (RegisterTestExtension(TEXT("game.busy"), TEXT("ext.mod"), TEXT("ext.busy")) == nullptr)
			{
				return;
			}

			TestFalse(TEXT("the point cannot be closed"),
				GetExtensionRegistry()->UnregisterExtensionPoint(FName(TEXT("game.busy"))));

			// Both the point and its extension have to survive the refusal untouched.
			TestEqual(TEXT("the point is still open"), GetExtensionRegistry()->GetExtensionPoints().Num(), 1);
			TestEqual(TEXT("the extension is still registered"),
				GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.busy"))).Num(), 1);

			TestTrue(TEXT("the extension can be removed"),
				GetExtensionRegistry()->UnregisterExtension(FName(TEXT("game.busy")), FName(TEXT("ext.busy"))));
			TestTrue(TEXT("and then the point closes"),
				GetExtensionRegistry()->UnregisterExtensionPoint(FName(TEXT("game.busy"))));
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Extensions
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Extensions"), [this]()
	{
		It(TEXT("files an extension under its point, assigns the owner and starts it inactive"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.point"))))
			{
				return;
			}

			UModExtension* Extension = MakeTestExtension(TEXT("game.point"), TEXT("ext.first"));

			FModDiagnostic Error;
			if (!TestTrue(TEXT("registers"), GetExtensionRegistry()->RegisterExtension(
				Extension, MakeTestId(TEXT("ext.mod")), Error)))
			{
				return;
			}

			ExpectNoDiagnostic(TEXT("a successful registration"), Error);
			ExpectString(TEXT("the owner was assigned"), Extension->OwningModId.ToString(), TEXT("ext.mod"));

			// An extension is registered inactive; UModSubsystem activates it with its owning mod, so
			// game code that walks a point never sees a mod that is merely loaded.
			TestFalse(TEXT("it starts inactive"), Extension->IsExtensionActive());
			TestEqual(TEXT("GetExtensions skips it"), GetExtensionRegistry()->GetExtensions(FName(TEXT("game.point"))).Num(), 0);
			TestEqual(TEXT("GetAllExtensions includes it"),
				GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.point"))).Num(), 1);

			TestTrue(TEXT("FindExtension by its authored id"),
				GetExtensionRegistry()->FindExtension(FName(TEXT("game.point")), FName(TEXT("ext.first"))) == Extension);
		});

		It(TEXT("falls back to <modid>:<classname> when the author left the id empty"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.point"))))
			{
				return;
			}

			UModExtension* Extension = MakeTestExtension(TEXT("game.point"), nullptr);

			FModDiagnostic Error;
			if (!TestTrue(TEXT("registers"), GetExtensionRegistry()->RegisterExtension(
				Extension, MakeTestId(TEXT("ext.mod")), Error)))
			{
				return;
			}

			// The fallback is only stable once the owner is assigned, which the registry does before
			// it computes the id - otherwise two mods shipping the same class would collide.
			const FString Expected = FString::Printf(TEXT("ext.mod:%s"), *Extension->GetClass()->GetName());
			ExpectString(TEXT("resolved id"), Extension->GetResolvedExtensionId().ToString(), Expected);

			TestTrue(TEXT("FindExtension matches the resolved id"),
				GetExtensionRegistry()->FindExtension(FName(TEXT("game.point")), FName(*Expected)) == Extension);
		});

		It(TEXT("rejects a null extension, a garbage one and one with no point id"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.point"))))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("ext.mod"));
			FModDiagnostic Error;

			TestFalse(TEXT("a null extension"), GetExtensionRegistry()->RegisterExtension(nullptr, ModId, Error));
			ExpectDiagnostic(TEXT("a null extension"), Error, TEXT("Extension.Invalid"));

			UModExtension* Dead = MakeTestExtension(TEXT("game.point"), TEXT("ext.dead"));
			Dead->MarkAsGarbage();
			TestFalse(TEXT("a garbage extension"), GetExtensionRegistry()->RegisterExtension(Dead, ModId, Error));
			ExpectDiagnostic(TEXT("a garbage extension"), Error, TEXT("Extension.Invalid"));

			UModExtension* Unaddressed = MakeTestExtension(nullptr, TEXT("ext.nowhere"));
			TestFalse(TEXT("no point id"), GetExtensionRegistry()->RegisterExtension(Unaddressed, ModId, Error));
			ExpectDiagnostic(TEXT("no point id"), Error, TEXT("Extension.Invalid"));

			TestEqual(TEXT("nothing was filed"), GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.point"))).Num(), 0);
		});

		// Extensions are never queued for a point that may open later: a mod would then silently do
		// nothing, which is far harder to diagnose than an error.
		It(TEXT("rejects an extension whose point nobody opened"), [this]()
		{
			UModExtension* Extension = MakeTestExtension(TEXT("game.imaginary"), TEXT("ext.orphan"));

			FModDiagnostic Error;
			TestFalse(TEXT("does not register"), GetExtensionRegistry()->RegisterExtension(
				Extension, MakeTestId(TEXT("ext.mod")), Error));

			ExpectDiagnostic(TEXT("an unknown point"), Error, TEXT("Extension.PointNotFound"));
			ExpectString(TEXT("the diagnostic names the mod"), Error.ModId.ToString(), TEXT("ext.mod"));
			TestFalse(TEXT("the extension was not adopted"), Extension->OwningModId.IsValid());
		});

		It(TEXT("enforces the point's required base class"), [this]()
		{
			// A class derived from UModExtension that nothing in this spec is an instance of. See
			// MakeTypeToken for why the class is synthesised rather than declared.
			UClass* RequiredBase = MakeTypeToken(UModExtension::StaticClass(), TEXT("ModLifecycleSpec_WeaponExtension"));

			FModExtensionPointDescriptor Strict = MakeTestPoint(TEXT("game.strict"));
			Strict.RequiredBaseClass = RequiredBase;

			FModExtensionPointDescriptor Loose = MakeTestPoint(TEXT("game.loose"));
			Loose.RequiredBaseClass = UModExtension::StaticClass();

			if (!RegisterTestPoint(Strict) || !RegisterTestPoint(Loose))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("ext.mod"));
			FModDiagnostic Error;

			UModExtension* WrongType = MakeTestExtension(TEXT("game.strict"), TEXT("ext.wrong"));
			TestFalse(TEXT("a class that does not derive from the required base"),
				GetExtensionRegistry()->RegisterExtension(WrongType, ModId, Error));
			ExpectDiagnostic(TEXT("a base class mismatch"), Error, TEXT("Extension.BaseClassMismatch"));
			TestTrue(TEXT("the diagnostic names the required class"),
				Error.Message.Contains(RequiredBase->GetName(), ESearchCase::CaseSensitive));

			// The rejected extension must come out exactly as it went in.
			TestFalse(TEXT("the rejected extension was not adopted"), WrongType->OwningModId.IsValid());
			TestEqual(TEXT("nothing was filed"), GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.strict"))).Num(), 0);

			UModExtension* RightType = MakeTestExtension(TEXT("game.loose"), TEXT("ext.right"));
			TestTrue(TEXT("a class that does derive from the required base"),
				GetExtensionRegistry()->RegisterExtension(RightType, ModId, Error));
		});

		It(TEXT("rejects a second extension using an id that is already taken at that point"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.point"))) || !RegisterTestPoint(MakeTestPoint(TEXT("game.other"))))
			{
				return;
			}

			if (RegisterTestExtension(TEXT("game.point"), TEXT("ext.first"), TEXT("ext.shared")) == nullptr)
			{
				return;
			}

			UModExtension* Clash = MakeTestExtension(TEXT("game.point"), TEXT("ext.shared"));

			FModDiagnostic Error;
			TestFalse(TEXT("a duplicate id at the same point"), GetExtensionRegistry()->RegisterExtension(
				Clash, MakeTestId(TEXT("ext.second")), Error));

			ExpectDiagnostic(TEXT("a duplicate extension id"), Error, TEXT("Extension.DuplicateId"));

			// A rejected registration restores the previous owner, so a mod cannot end up marked as
			// the owner of something the registry never accepted.
			TestFalse(TEXT("the rejected extension was not adopted"), Clash->OwningModId.IsValid());
			TestEqual(TEXT("one extension is filed"),
				GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.point"))).Num(), 1);

			// Ids are unique within a point, not across the whole registry.
			UModExtension* SameIdElsewhere = MakeTestExtension(TEXT("game.other"), TEXT("ext.shared"));
			TestTrue(TEXT("the same id at a different point is fine"), GetExtensionRegistry()->RegisterExtension(
				SameIdElsewhere, MakeTestId(TEXT("ext.second")), Error));
		});

		It(TEXT("honours bAllowMultiplePerMod in both directions"), [this]()
		{
			FModExtensionPointDescriptor Single = MakeTestPoint(TEXT("game.single"));
			Single.bAllowMultiplePerMod = false;

			FModExtensionPointDescriptor Many = MakeTestPoint(TEXT("game.many"));
			Many.bAllowMultiplePerMod = true;

			if (!RegisterTestPoint(Single) || !RegisterTestPoint(Many))
			{
				return;
			}

			const FModId ModOne = MakeTestId(TEXT("ext.one"));
			const FModId ModTwo = MakeTestId(TEXT("ext.two"));
			FModDiagnostic Error;

			TestTrue(TEXT("the first contribution to the single-slot point"), GetExtensionRegistry()->RegisterExtension(
				MakeTestExtension(TEXT("game.single"), TEXT("ext.single.a")), ModOne, Error));

			UModExtension* Second = MakeTestExtension(TEXT("game.single"), TEXT("ext.single.b"));
			TestFalse(TEXT("a second contribution from the same mod"),
				GetExtensionRegistry()->RegisterExtension(Second, ModOne, Error));
			ExpectDiagnostic(TEXT("a second contribution"), Error, TEXT("Extension.MultipleNotAllowed"));
			TestFalse(TEXT("the rejected extension was not adopted"), Second->OwningModId.IsValid());

			// The quota is per mod, not per point.
			TestTrue(TEXT("a different mod may still contribute"), GetExtensionRegistry()->RegisterExtension(
				MakeTestExtension(TEXT("game.single"), TEXT("ext.single.c")), ModTwo, Error));

			TestTrue(TEXT("the first contribution to the multi-slot point"), GetExtensionRegistry()->RegisterExtension(
				MakeTestExtension(TEXT("game.many"), TEXT("ext.many.a")), ModOne, Error));
			TestTrue(TEXT("and the second, from the same mod"), GetExtensionRegistry()->RegisterExtension(
				MakeTestExtension(TEXT("game.many"), TEXT("ext.many.b")), ModOne, Error));

			TestEqual(TEXT("the single-slot point holds two, one per mod"),
				GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.single"))).Num(), 2);
			TestEqual(TEXT("the multi-slot point holds two from one mod"),
				GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.many"))).Num(), 2);
		});

		It(TEXT("refuses a mod that lacks a permission the point requires"), [this]()
		{
			FModExtensionPointDescriptor Gated = MakeTestPoint(TEXT("game.gated"));
			Gated.RequiredPermissions.Add(ModPermissions::GameplayModify);
			Gated.RequiredPermissions.Add(NAME_None);

			if (!RegisterTestPoint(Gated))
			{
				return;
			}

			const FModId Allowed = MakeTestId(TEXT("ext.allowed"));
			const FModId Denied = MakeTestId(TEXT("ext.denied"));

			GetExtensionRegistry()->SetPermissionCheck(UModAPIRegistry::FModPermissionCheck::CreateLambda(
				[Allowed](const FModId& ModId, FName Permission)
				{
					return ModId == Allowed && Permission == ModPermissions::GameplayModify;
				}));

			FModDiagnostic Error;

			TestTrue(TEXT("the permitted mod contributes"), GetExtensionRegistry()->RegisterExtension(
				MakeTestExtension(TEXT("game.gated"), TEXT("ext.ok")), Allowed, Error));

			UModExtension* Refused = MakeTestExtension(TEXT("game.gated"), TEXT("ext.no"));
			TestFalse(TEXT("the unpermitted mod does not"),
				GetExtensionRegistry()->RegisterExtension(Refused, Denied, Error));
			ExpectDiagnostic(TEXT("a denied permission"), Error, TEXT("Extension.PermissionDenied"));
			TestTrue(TEXT("the diagnostic names the permission"),
				Error.Message.Contains(ModPermissions::GameplayModify.ToString(), ESearchCase::CaseSensitive));
			TestFalse(TEXT("the rejected extension was not adopted"), Refused->OwningModId.IsValid());
		});

		// DOCUMENTED GUARANTEE, and deliberately the opposite of the API registry: with no check
		// injected every permission counts as held, so a registry used standalone - tests, editor
		// tooling - is not crippled by a policy that does not exist there. The API registry fails
		// closed instead because handing out a gated API is a capability grant; filing an extension
		// only makes it visible to the game, which decides what to do with it.
		It(TEXT("accepts every extension while no permission check is installed"), [this]()
		{
			FModExtensionPointDescriptor Gated = MakeTestPoint(TEXT("game.ungoverned"));
			Gated.RequiredPermissions.Add(ModPermissions::NativeCode);

			if (!RegisterTestPoint(Gated))
			{
				return;
			}

			FModDiagnostic Error;
			TestTrue(TEXT("registers without a check installed"), GetExtensionRegistry()->RegisterExtension(
				MakeTestExtension(TEXT("game.ungoverned"), TEXT("ext.free")), MakeTestId(TEXT("ext.mod")), Error));
		});

		It(TEXT("SetModExtensionsActive flips only the named mod's extensions"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.point"))))
			{
				return;
			}

			UModExtension* Mine = RegisterTestExtension(TEXT("game.point"), TEXT("ext.mine"), TEXT("ext.a"));
			UModExtension* Theirs = RegisterTestExtension(TEXT("game.point"), TEXT("ext.theirs"), TEXT("ext.b"));
			if (Mine == nullptr || Theirs == nullptr)
			{
				return;
			}

			GetExtensionRegistry()->SetModExtensionsActive(MakeTestId(TEXT("ext.mine")), true);

			TestTrue(TEXT("mine is active"), Mine->IsExtensionActive());
			TestFalse(TEXT("theirs is not"), Theirs->IsExtensionActive());
			ExpectString(TEXT("GetExtensions returns only the active one"),
				JoinExtensionIds(GetExtensionRegistry()->GetExtensions(FName(TEXT("game.point")))), TEXT("ext.a"));
			TestEqual(TEXT("GetAllExtensions still returns both"),
				GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.point"))).Num(), 2);

			GetExtensionRegistry()->SetModExtensionsActive(MakeTestId(TEXT("ext.mine")), false);
			TestFalse(TEXT("mine is inactive again"), Mine->IsExtensionActive());
			TestEqual(TEXT("nothing is active"), GetExtensionRegistry()->GetExtensions(FName(TEXT("game.point"))).Num(), 0);
		});

		It(TEXT("UnregisterExtension removes one entry and reports an unknown one"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.point"))))
			{
				return;
			}

			if (RegisterTestExtension(TEXT("game.point"), TEXT("ext.mod"), TEXT("ext.a")) == nullptr
				|| RegisterTestExtension(TEXT("game.point"), TEXT("ext.mod"), TEXT("ext.b")) == nullptr)
			{
				return;
			}

			TestFalse(TEXT("an unknown point"),
				GetExtensionRegistry()->UnregisterExtension(FName(TEXT("game.absent")), FName(TEXT("ext.a"))));
			TestFalse(TEXT("an unknown extension id"),
				GetExtensionRegistry()->UnregisterExtension(FName(TEXT("game.point")), FName(TEXT("ext.absent"))));

			TestTrue(TEXT("a known extension"),
				GetExtensionRegistry()->UnregisterExtension(FName(TEXT("game.point")), FName(TEXT("ext.a"))));

			ExpectString(TEXT("what is left"),
				JoinExtensionIds(GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.point")))), TEXT("ext.b"));
		});

		It(TEXT("GetExtensionsForMod walks every point in a stable order"), [this]()
		{
			RegisterTestPoint(MakeTestPoint(TEXT("game.zulu")));
			RegisterTestPoint(MakeTestPoint(TEXT("game.alpha")));

			RegisterTestExtension(TEXT("game.zulu"), TEXT("ext.mine"), TEXT("ext.z"));
			RegisterTestExtension(TEXT("game.alpha"), TEXT("ext.mine"), TEXT("ext.a"));
			RegisterTestExtension(TEXT("game.alpha"), TEXT("ext.theirs"), TEXT("ext.other"));

			// A result that feeds diagnostics has to read the same way on every machine, so the point
			// ids are sorted rather than taken in TMap order.
			ExpectString(TEXT("one mod's extensions"),
				JoinExtensionIds(GetExtensionRegistry()->GetExtensionsForMod(MakeTestId(TEXT("ext.mine")))),
				TEXT("ext.a, ext.z"));

			TestEqual(TEXT("a mod that contributed nothing"),
				GetExtensionRegistry()->GetExtensionsForMod(MakeTestId(TEXT("ext.nobody"))).Num(), 0);
		});

		It(TEXT("UnregisterAllForMod clears one mod out of every point and leaves the rest"), [this]()
		{
			RegisterTestPoint(MakeTestPoint(TEXT("game.one")));
			RegisterTestPoint(MakeTestPoint(TEXT("game.two")));

			RegisterTestExtension(TEXT("game.one"), TEXT("ext.leaver"), TEXT("ext.leaver.a"));
			RegisterTestExtension(TEXT("game.two"), TEXT("ext.leaver"), TEXT("ext.leaver.b"));
			RegisterTestExtension(TEXT("game.one"), TEXT("ext.stayer"), TEXT("ext.stayer.a"));

			GetExtensionRegistry()->UnregisterAllForMod(MakeTestId(TEXT("ext.leaver")));

			TestEqual(TEXT("the leaver has nothing left"),
				GetExtensionRegistry()->GetExtensionsForMod(MakeTestId(TEXT("ext.leaver"))).Num(), 0);
			ExpectString(TEXT("the stayer is untouched"),
				JoinExtensionIds(GetExtensionRegistry()->GetExtensionsForMod(MakeTestId(TEXT("ext.stayer")))),
				TEXT("ext.stayer.a"));

			// The points themselves belong to the game and survive a mod unloading.
			TestEqual(TEXT("both points are still open"), GetExtensionRegistry()->GetExtensionPoints().Num(), 2);
			TestEqual(TEXT("the point the leaver alone used is now empty"),
				GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.two"))).Num(), 0);

			// Removing a mod that contributed nothing is a no-op, not an error.
			GetExtensionRegistry()->UnregisterAllForMod(MakeTestId(TEXT("ext.nobody")));
			TestEqual(TEXT("the stayer is still there"),
				GetExtensionRegistry()->GetExtensionsForMod(MakeTestId(TEXT("ext.stayer"))).Num(), 1);
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Extension ordering
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Extension ordering"), [this]()
	{
		// DOCUMENTED GUARANTEE, most significant first:
		//   1. Priority, descending - the mod author's own weight.
		//   2. The owning mod's load order, ascending - mods the resolver put first extend first.
		//   3. The resolved extension id, ascending - unique within a point, so no ties remain.
		// Because rule 3 always breaks the tie, the comparison is a strict total order and the sorted
		// result cannot depend on the order the extensions arrived in. That is what makes a session
		// reproducible from the manifest set alone.
		It(TEXT("sorts by priority, then load order, then id"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.sorted"))))
			{
				return;
			}

			TArray<FModId> Order;
			Order.Add(MakeTestId(TEXT("sort.first")));
			Order.Add(MakeTestId(TEXT("sort.second")));
			GetExtensionRegistry()->SetModLoadOrder(Order);

			RegisterTestExtension(TEXT("game.sorted"), TEXT("sort.second"), TEXT("ext.f"), -5);
			RegisterTestExtension(TEXT("game.sorted"), TEXT("sort.first"), TEXT("ext.c"), 0);
			RegisterTestExtension(TEXT("game.sorted"), TEXT("sort.unranked"), TEXT("ext.d"), 10);
			RegisterTestExtension(TEXT("game.sorted"), TEXT("sort.second"), TEXT("ext.b"), 10);
			RegisterTestExtension(TEXT("game.sorted"), TEXT("sort.first"), TEXT("ext.aa"), 10);
			RegisterTestExtension(TEXT("game.sorted"), TEXT("sort.first"), TEXT("ext.a"), 10);

			// ext.a and ext.aa share a mod and a priority, so only the id separates them; ext.d
			// belongs to a mod the resolver never ordered, so it sorts after every ranked mod but
			// still ahead of the lower priorities.
			ExpectString(TEXT("sorted extensions"),
				JoinExtensionIds(GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.sorted")))),
				TEXT("ext.a, ext.aa, ext.b, ext.d, ext.c, ext.f"));
		});

		It(TEXT("produces the same order whatever order the extensions were registered in"), [this]()
		{
			// Three registration orders of the same six extensions. A hash-order or insertion-order
			// bug shows up as one of these disagreeing with the others.
			const TCHAR* const RegistrationOrders[3][6] =
			{
				{ TEXT("ext.a"), TEXT("ext.aa"), TEXT("ext.b"), TEXT("ext.c"), TEXT("ext.d"), TEXT("ext.f") },
				{ TEXT("ext.f"), TEXT("ext.d"), TEXT("ext.c"), TEXT("ext.b"), TEXT("ext.aa"), TEXT("ext.a") },
				{ TEXT("ext.c"), TEXT("ext.a"), TEXT("ext.f"), TEXT("ext.aa"), TEXT("ext.d"), TEXT("ext.b") }
			};

			for (int32 Attempt = 0; Attempt < 3; ++Attempt)
			{
				const FString PointName = FString::Printf(TEXT("game.deterministic.%d"), Attempt);
				if (!RegisterTestPoint(MakeTestPoint(*PointName)))
				{
					return;
				}

				TArray<FModId> Order;
				Order.Add(MakeTestId(TEXT("sort.first")));
				Order.Add(MakeTestId(TEXT("sort.second")));
				GetExtensionRegistry()->SetModLoadOrder(Order);

				for (int32 Index = 0; Index < 6; ++Index)
				{
					const FString ExtensionId = RegistrationOrders[Attempt][Index];

					// Same (mod, priority) assignment as the ordering test above.
					const TCHAR* ModId = TEXT("sort.first");
					int32 Priority = 10;

					if (ExtensionId == TEXT("ext.b"))
					{
						ModId = TEXT("sort.second");
					}
					else if (ExtensionId == TEXT("ext.d"))
					{
						ModId = TEXT("sort.unranked");
					}
					else if (ExtensionId == TEXT("ext.c"))
					{
						Priority = 0;
					}
					else if (ExtensionId == TEXT("ext.f"))
					{
						ModId = TEXT("sort.second");
						Priority = -5;
					}

					RegisterTestExtension(*PointName, ModId, *ExtensionId, Priority);
				}

				const FString FailureMessage = FString::Printf(TEXT("registration order %d"), Attempt);
				ExpectString(FailureMessage,
					JoinExtensionIds(GetExtensionRegistry()->GetAllExtensions(FName(*PointName))),
					TEXT("ext.a, ext.aa, ext.b, ext.d, ext.c, ext.f"));
			}
		});

		It(TEXT("re-sorts every point when the load order changes"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.reorder"))))
			{
				return;
			}

			RegisterTestExtension(TEXT("game.reorder"), TEXT("sort.alpha"), TEXT("ext.alpha"));
			RegisterTestExtension(TEXT("game.reorder"), TEXT("sort.beta"), TEXT("ext.beta"));

			// With no load order at all both mods sort last-equal, so only the id separates them.
			ExpectString(TEXT("before any load order"),
				JoinExtensionIds(GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.reorder")))),
				TEXT("ext.alpha, ext.beta"));

			TArray<FModId> Order;
			Order.Add(MakeTestId(TEXT("sort.beta")));
			Order.Add(MakeTestId(TEXT("sort.alpha")));
			GetExtensionRegistry()->SetModLoadOrder(Order);

			ExpectString(TEXT("after the load order is published"),
				JoinExtensionIds(GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.reorder")))),
				TEXT("ext.beta, ext.alpha"));
		});

		// REGRESSION TEST - this was a live crash, not a hypothetical one.
		//
		// TArray<TObjectPtr<T>>::Sort routes through TDereferenceWrapper (UObject/ObjectPtr.h), which
		// calls Predicate(*A, *B): it dereferences every element BEFORE the predicate ever sees it.
		// So the predicate takes const T&, a null check inside it is unreachable dead code, and a
		// null slot crashes inside the engine wrapper rather than in framework code. Garbage
		// collection can null a UPROPERTY TObjectPtr slot at any time, which is exactly the state
		// this test creates: UModExtensionRegistry::SortExtensionList has to drop invalid entries
		// BEFORE it sorts, never sort them to the end.
		It(TEXT("survives a dead entry in an extension list when the list is re-sorted"), [this]()
		{
			if (!RegisterTestPoint(MakeTestPoint(TEXT("game.sortcrash"))))
			{
				return;
			}

			TArray<FModId> Order;
			Order.Add(MakeTestId(TEXT("crash.first")));
			Order.Add(MakeTestId(TEXT("crash.second")));
			GetExtensionRegistry()->SetModLoadOrder(Order);

			UModExtension* Doomed = RegisterTestExtension(TEXT("game.sortcrash"), TEXT("crash.first"), TEXT("ext.doomed"));
			if (Doomed == nullptr
				|| RegisterTestExtension(TEXT("game.sortcrash"), TEXT("crash.first"), TEXT("ext.survivor.a")) == nullptr
				|| RegisterTestExtension(TEXT("game.sortcrash"), TEXT("crash.second"), TEXT("ext.survivor.b")) == nullptr)
			{
				return;
			}

			TestEqual(TEXT("three extensions before the collection"),
				GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.sortcrash"))).Num(), 3);

			// Kill one behind the registry's back and let the collector clear the slot. Nothing may
			// touch Doomed after this point.
			Doomed->MarkAsGarbage();
			Doomed = nullptr;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

			// SetModLoadOrder re-sorts every point holding more than one extension, which is the path
			// that used to crash. The list still has three slots here, one of them dead.
			TArray<FModId> ReversedOrder;
			ReversedOrder.Add(MakeTestId(TEXT("crash.second")));
			ReversedOrder.Add(MakeTestId(TEXT("crash.first")));
			GetExtensionRegistry()->SetModLoadOrder(ReversedOrder);

			// The dead entry is dropped rather than sorted, and the survivors come back in the new
			// order - so the sort really ran instead of being skipped.
			ExpectString(TEXT("what survived the re-sort"),
				JoinExtensionIds(GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.sortcrash")))),
				TEXT("ext.survivor.b, ext.survivor.a"));

			TestNull(TEXT("the dead extension is no longer findable"),
				GetExtensionRegistry()->FindExtension(FName(TEXT("game.sortcrash")), FName(TEXT("ext.doomed"))));

			// Registering into the same list afterwards has to keep working: RegisterExtension sorts
			// too, and would have hit the same crash on the next contribution.
			FModDiagnostic Error;
			TestTrue(TEXT("the point still accepts new extensions"), GetExtensionRegistry()->RegisterExtension(
				MakeTestExtension(TEXT("game.sortcrash"), TEXT("ext.newcomer"), 10),
				MakeTestId(TEXT("crash.second")), Error));

			ExpectString(TEXT("the newcomer sorts by priority"),
				JoinExtensionIds(GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.sortcrash")))),
				TEXT("ext.newcomer, ext.survivor.b, ext.survivor.a"));
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Resource resolution
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Resource resolution"), [this]()
	{
		It(TEXT("FirstWins picks the earliest load order"), [this]()
		{
			if (!SetUpResourceContest(EModConflictPolicy::FirstWins))
			{
				return;
			}

			ExpectString(TEXT("winner"), ResolveContestWinner(), TEXT("contest.early:claim"));
		});

		It(TEXT("LastWins picks the latest load order"), [this]()
		{
			if (!SetUpResourceContest(EModConflictPolicy::LastWins))
			{
				return;
			}

			ExpectString(TEXT("winner"), ResolveContestWinner(), TEXT("contest.late:claim"));
		});

		It(TEXT("Priority picks the highest priority and breaks a tie on the latest load order"), [this]()
		{
			if (!SetUpResourceContest(EModConflictPolicy::Priority, /*Early=*/1, /*Middle=*/9, /*Late=*/3))
			{
				return;
			}

			ExpectString(TEXT("the highest priority wins regardless of load order"),
				ResolveContestWinner(), TEXT("contest.middle:claim"));
		});

		It(TEXT("Priority breaks an exact tie on the latest load order"), [this]()
		{
			if (!SetUpResourceContest(EModConflictPolicy::Priority, /*Early=*/7, /*Middle=*/7, /*Late=*/7))
			{
				return;
			}

			ExpectString(TEXT("winner"), ResolveContestWinner(), TEXT("contest.late:claim"));
		});

		// Error means "a human has to decide" and Merge means "there is no single winner"; handing
		// back one of the contenders under either policy would be a lie.
		It(TEXT("Error produces no winner while the resource is contested"), [this]()
		{
			if (!SetUpResourceContest(EModConflictPolicy::Error))
			{
				return;
			}

			ExpectString(TEXT("Error"), ResolveContestWinner(), TEXT("<none>"));
		});

		It(TEXT("Merge produces no winner while the resource is contested"), [this]()
		{
			if (!SetUpResourceContest(EModConflictPolicy::Merge))
			{
				return;
			}

			ExpectString(TEXT("Merge"), ResolveContestWinner(), TEXT("<none>"));
		});

		// An uncontested claim wins under every policy, Error and Merge included: there is nothing
		// to reconcile.
		It(TEXT("a single contender wins under every policy"), [this]()
		{
			const EModConflictPolicy Policies[] =
			{
				EModConflictPolicy::Error,
				EModConflictPolicy::FirstWins,
				EModConflictPolicy::LastWins,
				EModConflictPolicy::Priority,
				EModConflictPolicy::Merge
			};

			TArray<FName> Claims;
			Claims.Add(FName(TEXT("solo.resource")));

			for (const EModConflictPolicy Policy : Policies)
			{
				const FString PointName = FString::Printf(TEXT("game.solo.%s"), *ModFrameworkEnums::ToString(Policy));

				FModExtensionPointDescriptor Point = MakeTestPoint(*PointName);
				Point.DefaultConflictPolicy = Policy;
				if (!RegisterTestPoint(Point))
				{
					continue;
				}

				if (RegisterTestExtension(*PointName, TEXT("solo.mod"), TEXT("ext.solo"), 0, Claims) == nullptr)
				{
					continue;
				}
				GetExtensionRegistry()->SetModExtensionsActive(MakeTestId(TEXT("solo.mod")), true);

				const UModExtension* Winner = GetExtensionRegistry()->ResolveResource(
					FName(*PointName), FName(TEXT("solo.resource")));

				const FString FailureMessage = FString::Printf(TEXT("the sole claimant wins under %s"),
					*ModFrameworkEnums::ToString(Policy));
				TestNotNull(*FailureMessage, Winner);
			}
		});

		It(TEXT("ignores a claimant whose mod is loaded but not activated"), [this]()
		{
			if (!SetUpResourceContest(EModConflictPolicy::LastWins))
			{
				return;
			}

			// Deactivate the mod LastWins would otherwise pick; the resource has to fall to the next
			// active claimant rather than staying with an idle mod.
			GetExtensionRegistry()->SetModExtensionsActive(MakeTestId(ContestLate), false);
			ExpectString(TEXT("winner after the last mod deactivates"),
				ResolveContestWinner(), TEXT("contest.middle:claim"));

			GetExtensionRegistry()->SetModExtensionsActive(MakeTestId(ContestMiddle), false);
			GetExtensionRegistry()->SetModExtensionsActive(MakeTestId(ContestEarly), false);
			ExpectString(TEXT("winner once nothing is active"), ResolveContestWinner(), TEXT("<none>"));
		});

		It(TEXT("returns nothing for an unknown point or an unclaimed resource"), [this]()
		{
			if (!SetUpResourceContest(EModConflictPolicy::LastWins))
			{
				return;
			}

			TestNull(TEXT("an unknown point"),
				GetExtensionRegistry()->ResolveResource(FName(TEXT("game.absent")), FName(ContestResource)));
			TestNull(TEXT("an unclaimed resource"),
				GetExtensionRegistry()->ResolveResource(FName(ContestPoint), FName(TEXT("nobody.wants.this"))));
		});

		It(TEXT("CollectResourceClaims reports one claim per extension and resource"), [this]()
		{
			FModExtensionPointDescriptor Point = MakeTestPoint(TEXT("game.claims"));
			Point.DefaultConflictPolicy = EModConflictPolicy::Priority;
			if (!RegisterTestPoint(Point))
			{
				return;
			}

			TArray<FModId> Order;
			Order.Add(MakeTestId(TEXT("claim.ranked")));
			GetExtensionRegistry()->SetModLoadOrder(Order);

			TArray<FName> RankedClaims;
			RankedClaims.Add(FName(TEXT("res.x")));
			RankedClaims.Add(NAME_None);
			RankedClaims.Add(FName(TEXT("res.y")));

			TArray<FName> UnrankedClaims;
			UnrankedClaims.Add(FName(TEXT("res.x")));

			if (RegisterTestExtension(TEXT("game.claims"), TEXT("claim.ranked"), TEXT("ext.ranked"), 5, RankedClaims) == nullptr
				|| RegisterTestExtension(TEXT("game.claims"), TEXT("claim.unranked"), TEXT("ext.unranked"), 5, UnrankedClaims) == nullptr)
			{
				return;
			}

			// Deliberately NOT activated: conflicts have to surface before anything is activated, or
			// a player only learns two mods fight after both are already running.
			const TArray<FModResourceClaim> Claims = GetExtensionRegistry()->CollectResourceClaims();

			if (!TestEqual(TEXT("three claims: two from the ranked mod, one from the other"), Claims.Num(), 3))
			{
				return;
			}

			ExpectString(TEXT("claim 0 mod"), Claims[0].ModId.ToString(), TEXT("claim.ranked"));
			ExpectString(TEXT("claim 0 point"), Claims[0].ExtensionPointId.ToString(), TEXT("game.claims"));
			ExpectString(TEXT("claim 0 resource"), Claims[0].ResourceId.ToString(), TEXT("res.x"));
			ExpectString(TEXT("claim 0 extension"), Claims[0].ExtensionId.ToString(), TEXT("ext.ranked"));
			TestEqual(TEXT("claim 0 priority"), Claims[0].Priority, 5);
			TestEqual(TEXT("claim 0 load order"), Claims[0].LoadOrder, 0);
			ExpectString(TEXT("claim 0 policy"),
				ModFrameworkEnums::ToString(Claims[0].PreferredPolicy), TEXT("Priority"));

			// The None entry in the middle of the array claims nothing and is skipped.
			ExpectString(TEXT("claim 1 resource"), Claims[1].ResourceId.ToString(), TEXT("res.y"));

			ExpectString(TEXT("claim 2 mod"), Claims[2].ModId.ToString(), TEXT("claim.unranked"));
			TestEqual(TEXT("claim 2 has no load order"), Claims[2].LoadOrder, static_cast<int32>(INDEX_NONE));
		});
	});

	//~ -------------------------------------------------------------------------------------------
	//~ Teardown
	//~ -------------------------------------------------------------------------------------------

	Describe(TEXT("Teardown"), [this]()
	{
		It(TEXT("unregistering a mod takes its extensions and APIs with it"), [this]()
		{
			if (!RegisterTestMod(TEXT("down.mod")) || !RegisterTestPoint(MakeTestPoint(TEXT("game.point"))))
			{
				return;
			}

			const FModId ModId = MakeTestId(TEXT("down.mod"));

			UModAPI* ModApi = MakeTestApi(TEXT("mod.provided"));
			UModAPI* GameApi = MakeTestApi(TEXT("game.provided"));
			if (ModApi == nullptr || GameApi == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			GetApiRegistry()->RegisterAPIForMod(ModApi, ModId, Error);
			GetApiRegistry()->RegisterAPI(GameApi, Error);
			RegisterTestExtension(TEXT("game.point"), TEXT("down.mod"), TEXT("ext.down"));

			UObject* Owned = NewObject<UModRegistry>(GetTransientPackage());
			GetModRegistry()->TrackModObject(ModId, Owned);

			TestTrue(TEXT("unregisters"), GetModRegistry()->UnregisterMod(ModId));

			TestFalse(TEXT("the mod's API is gone"), GetApiRegistry()->HasAPI(FName(TEXT("mod.provided"))));
			TestTrue(TEXT("the game's API survives"), GetApiRegistry()->HasAPI(FName(TEXT("game.provided"))));
			TestEqual(TEXT("the mod's extension is gone"),
				GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.point"))).Num(), 0);
			TestEqual(TEXT("the point survives"), GetExtensionRegistry()->GetExtensionPoints().Num(), 1);
			TestFalse(TEXT("the mod's object was released"), ::IsValid(Owned));
		});

		// Reset is the "reload every mod" tool: what the GAME registered has to survive it, or a full
		// mod reload would need the game to re-announce its own modding surface.
		It(TEXT("Reset drops the mods but keeps what the game registered"), [this]()
		{
			if (!RegisterTestMod(TEXT("down.a")) || !RegisterTestMod(TEXT("down.b"))
				|| !RegisterTestPoint(MakeTestPoint(TEXT("game.point"))))
			{
				return;
			}

			UModAPI* GameApi = MakeTestApi(TEXT("game.provided"));
			UModAPI* ModApi = MakeTestApi(TEXT("mod.provided"));
			if (GameApi == nullptr || ModApi == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			GetApiRegistry()->RegisterAPI(GameApi, Error);
			GetApiRegistry()->RegisterAPIForMod(ModApi, MakeTestId(TEXT("down.a")), Error);
			RegisterTestExtension(TEXT("game.point"), TEXT("down.a"), TEXT("ext.a"));
			RegisterTestExtension(TEXT("game.point"), TEXT("down.b"), TEXT("ext.b"));

			GetModRegistry()->Reset();

			TestEqual(TEXT("no mods remain"), GetModRegistry()->GetModCount(), 0);
			TestTrue(TEXT("the game's API survives"), GetApiRegistry()->HasAPI(FName(TEXT("game.provided"))));
			TestFalse(TEXT("the mod's API is gone"), GetApiRegistry()->HasAPI(FName(TEXT("mod.provided"))));
			TestEqual(TEXT("the game's extension point survives"), GetExtensionRegistry()->GetExtensionPoints().Num(), 1);
			TestEqual(TEXT("but holds nothing"), GetExtensionRegistry()->GetAllExtensions(FName(TEXT("game.point"))).Num(), 0);
		});

		It(TEXT("Shutdown empties both sub-registries and unbinds the state delegate"), [this]()
		{
			if (!RegisterTestMod(TEXT("down.mod")) || !RegisterTestPoint(MakeTestPoint(TEXT("game.point"))))
			{
				return;
			}

			UModAPI* GameApi = MakeTestApi(TEXT("game.provided"));
			if (GameApi == nullptr)
			{
				return;
			}

			FModDiagnostic Error;
			GetApiRegistry()->RegisterAPI(GameApi, Error);
			RegisterTestExtension(TEXT("game.point"), TEXT("down.mod"), TEXT("ext.down"));

			int32 BroadcastCount = 0;
			GetModRegistry()->OnModStateChanged.AddLambda([&BroadcastCount](const FModId&, EModState, EModState)
			{
				++BroadcastCount;
			});

			// Cached before Shutdown detaches them from the registry.
			UModAPIRegistry* Apis = GetApiRegistry();
			UModExtensionRegistry* ExtensionsRegistry = GetExtensionRegistry();

			GetModRegistry()->Shutdown();

			TestEqual(TEXT("no mods remain"), GetModRegistry()->GetModCount(), 0);
			TestNull(TEXT("the API sub-registry is detached"), GetModRegistry()->GetAPIRegistry());
			TestNull(TEXT("the extension sub-registry is detached"), GetModRegistry()->GetExtensionRegistry());

			// Shutdown is the end of the session, so even what the game registered goes.
			TestEqual(TEXT("every API was dropped"), Apis->GetAllAPIs().Num(), 0);
			TestEqual(TEXT("every extension point was closed"), ExtensionsRegistry->GetExtensionPoints().Num(), 0);

			// Initialize is idempotent, so a registry can be brought back up after a shutdown.
			GetModRegistry()->Initialize();
			TestNotNull(TEXT("Initialize rebuilds the API sub-registry"), GetModRegistry()->GetAPIRegistry());
			TestNotNull(TEXT("Initialize rebuilds the extension sub-registry"), GetModRegistry()->GetExtensionRegistry());

			RegisterTestMod(TEXT("down.after"));
			GetModRegistry()->SetModState(MakeTestId(TEXT("down.after")), EModState::Validated);
			TestEqual(TEXT("the listener bound before Shutdown was unbound"), BroadcastCount, 0);
		});

		It(TEXT("Initialize keeps the sub-registries it already created"), [this]()
		{
			UModAPIRegistry* Apis = GetApiRegistry();
			UModExtensionRegistry* ExtensionsRegistry = GetExtensionRegistry();

			if (!TestNotNull(TEXT("the API sub-registry exists"), Apis)
				|| !TestNotNull(TEXT("the extension sub-registry exists"), ExtensionsRegistry))
			{
				return;
			}

			FModDiagnostic Error;
			Apis->RegisterAPI(MakeTestApi(TEXT("game.kept")), Error);
			RegisterTestPoint(MakeTestPoint(TEXT("game.kept.point")));

			GetModRegistry()->Initialize();

			TestTrue(TEXT("the same API sub-registry"), GetApiRegistry() == Apis);
			TestTrue(TEXT("the same extension sub-registry"), GetExtensionRegistry() == ExtensionsRegistry);
			TestTrue(TEXT("its contents survived"), GetApiRegistry()->HasAPI(FName(TEXT("game.kept"))));
			TestEqual(TEXT("and so did the point"), GetExtensionRegistry()->GetExtensionPoints().Num(), 1);
		});

		It(TEXT("Shutdown is safe without a prior Initialize"), [this]()
		{
			UModRegistry* Bare = NewObject<UModRegistry>(GetTransientPackage());

			Bare->Shutdown();

			TestEqual(TEXT("nothing is registered"), Bare->GetModCount(), 0);
			TestNull(TEXT("no API sub-registry was created"), Bare->GetAPIRegistry());
			TestNull(TEXT("no extension sub-registry was created"), Bare->GetExtensionRegistry());
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
