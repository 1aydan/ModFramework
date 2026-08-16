// Copyright (c) 2026. Licensed for use in your own projects.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "Core/ModFrameworkVersion.h"
#include "CoreTypes.h"
#include "HAL/FileManager.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModManifestParser.h"
#include "Manifest/ModVersion.h"
#include "Math/NumericLimits.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Templates/TypeHash.h"
#include "UObject/NameTypes.h"
#include "UObject/SoftObjectPath.h"

/**
 * Manifest, version and version-range tests.
 *
 * Everything here runs against pure data: no world, no game instance and no mods on disk. The one
 * group that touches the filesystem creates its own directory under FPaths::AutomationTransientDir()
 * and removes it again in AfterEach.
 */
BEGIN_DEFINE_SPEC(FModManifestSpec, "ModFramework.Manifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	/** Per-test scratch directory; only the "Files" group creates one. */
	FString TempDirectory;

	/** A manifest whose four required fields are present and valid, plus ExtraJson (starts with a comma). */
	static FString MakeManifestJson(const FString& ExtraJson = FString());

	/** The kitchen-sink manifest used by the round-trip and full-read tests. */
	static const TCHAR* GetFullManifestJson();

	/** A manifest built in code that passes validation, for the tests that never touch JSON. */
	static FModManifest MakeValidManifest();

	const FModDiagnostic* FindDiagnostic(const TArray<FModDiagnostic>& In, const TCHAR* Code, const TCHAR* Context) const;
	int32 CountDiagnostics(const TArray<FModDiagnostic>& In, const TCHAR* Code) const;

	bool ExpectString(const FString& What, const FString& Actual, const FString& Expected);
	bool ExpectDiagnostic(const FString& What, const TArray<FModDiagnostic>& In, EModDiagnosticSeverity Severity, const TCHAR* Code, const TCHAR* Context);
	bool ExpectNoDiagnostics(const FString& What, const FModManifestParseResult& Result);
	bool ExpectParseFailure(const FString& What, const FModManifestParseResult& Result, const TCHAR* Code, const TCHAR* Context);

	bool ExpectVersionParses(const TCHAR* Text, const TCHAR* ExpectedCanonicalForm, bool bAllowPartial = true);
	bool ExpectVersionRejected(const TCHAR* Text, bool bAllowPartial = true);
	bool ExpectVersionOrder(const TCHAR* Lower, const TCHAR* Higher);

	bool ExpectRangeSatisfied(const TCHAR* RangeText, const TCHAR* VersionText, bool bExpected);
	bool ExpectRangeRejected(const TCHAR* RangeText);

	void CompareManifests(const FString& What, const FModManifest& Expected, const FModManifest& Actual);

END_DEFINE_SPEC(FModManifestSpec)

//////////////////////////////////////////////////////////////////////////
// Fixtures

FString FModManifestSpec::MakeManifestJson(const FString& ExtraJson)
{
	return FString::Printf(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "Example Mod",
	"version": "1.0.0",
	"game": { "id": "com.example.game" }%s
})json"), *ExtraJson);
}

const TCHAR* FModManifestSpec::GetFullManifestJson()
{
	return TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.bettercombat",
	"name": "Better Combat",
	"description": "Rebalances melee combat.",
	"author": "Example Author",
	"homepage": "https://example.com",
	"license": "MIT",
	"icon": "Art/Icon.png",
	"version": "2.1.0-rc.1+build.7",
	"game": { "id": "com.example.game", "version": ">=1.5.0 <2.0.0" },
	"framework": { "version": "^0.1.0" },
	"sdk": { "id": "com.example.game.sdk", "version": "^1.5.0" },
	"dependencies": [
		{ "id": "com.example.corelib", "version": ">=2.0.0" },
		{ "id": "com.example.ui", "version": "*", "optional": true, "reason": "Adds a settings page." }
	],
	"loadBefore": ["com.example.late"],
	"loadAfter": ["com.example.early"],
	"priority": 10,
	"network": { "scope": "ServerOnly", "requiredForNetworkPlay": true },
	"permissions": ["gameplay.modify", "save.modify"],
	"entryPoint": {
		"nativeModule": "BetterCombatRuntime",
		"class": "/BetterCombat/BP_BetterCombatEntry.BP_BetterCombatEntry_C",
		"contentBundles": ["/BetterCombat/DA_BetterCombatBundle.DA_BetterCombatBundle"]
	},
	"content": [
		{ "path": "Content/BetterCombat.pak", "type": "Pak", "mountPoint": "/BetterCombat/", "mountOrder": 0 },
		{ "path": "Content/Loose", "type": "LooseDirectory", "mountPoint": "/BetterCombatLoose/", "mountOrder": 7 }
	],
	"claims": [
		{ "extensionPoint": "game.weapon", "resource": "weapon.longsword", "policy": "Priority" }
	],
	"metadata": { "category": "Gameplay", "tags": "combat,melee" }
})json");
}

FModManifest FModManifestSpec::MakeValidManifest()
{
	FModManifest Manifest;
	Manifest.ManifestVersion = MODFRAMEWORK_MANIFEST_VERSION;
	Manifest.Id = FModId(FName(TEXT("com.example.mod")));
	Manifest.DisplayName = TEXT("Example Mod");
	Manifest.Version = FModVersion(1, 0, 0);
	Manifest.Game.GameId = TEXT("com.example.game");
	return Manifest;
}

//////////////////////////////////////////////////////////////////////////
// Assertion helpers

const FModDiagnostic* FModManifestSpec::FindDiagnostic(const TArray<FModDiagnostic>& In, const TCHAR* Code, const TCHAR* Context) const
{
	const FName WantedCode(Code);

	for (const FModDiagnostic& Diagnostic : In)
	{
		if (Diagnostic.Code != WantedCode)
		{
			continue;
		}

		if (Context != nullptr && !Diagnostic.Context.Equals(Context, ESearchCase::CaseSensitive))
		{
			continue;
		}

		return &Diagnostic;
	}

	return nullptr;
}

int32 FModManifestSpec::CountDiagnostics(const TArray<FModDiagnostic>& In, const TCHAR* Code) const
{
	const FName WantedCode(Code);

	int32 Count = 0;
	for (const FModDiagnostic& Diagnostic : In)
	{
		if (Diagnostic.Code == WantedCode)
		{
			++Count;
		}
	}
	return Count;
}

/** Case-sensitive string comparison with a message that prints both sides. */
bool FModManifestSpec::ExpectString(const FString& What, const FString& Actual, const FString& Expected)
{
	if (!Actual.Equals(Expected, ESearchCase::CaseSensitive))
	{
		AddError(FString::Printf(TEXT("%s: expected '%s' but got '%s'."), *What, *Expected, *Actual));
		return false;
	}

	return true;
}

bool FModManifestSpec::ExpectDiagnostic(const FString& What, const TArray<FModDiagnostic>& In,
	EModDiagnosticSeverity Severity, const TCHAR* Code, const TCHAR* Context)
{
	const FModDiagnostic* Found = FindDiagnostic(In, Code, Context);
	if (Found == nullptr)
	{
		AddError(FString::Printf(TEXT("%s: expected diagnostic '%s' on '%s'. Got:\n%s"),
			*What, Code, Context != nullptr ? Context : TEXT("<any>"), *ModDiagnostics::Join(In)));
		return false;
	}

	if (Found->Severity != Severity)
	{
		AddError(FString::Printf(TEXT("%s: diagnostic '%s' on '%s' has severity %s, expected %s."),
			*What, Code, Context != nullptr ? Context : TEXT("<any>"),
			*ModFrameworkEnums::ToString(Found->Severity), *ModFrameworkEnums::ToString(Severity)));
		return false;
	}

	if (Found->Message.IsEmpty())
	{
		AddError(FString::Printf(TEXT("%s: diagnostic '%s' on '%s' carries no message; every diagnostic must be readable by a mod author."),
			*What, Code, Context != nullptr ? Context : TEXT("<any>")));
		return false;
	}

	return true;
}

bool FModManifestSpec::ExpectNoDiagnostics(const FString& What, const FModManifestParseResult& Result)
{
	if (!Result.bSuccess || Result.Diagnostics.Num() > 0)
	{
		AddError(FString::Printf(TEXT("%s: expected a clean parse but bSuccess=%s and %d diagnostic(s) were raised:\n%s"),
			*What, Result.bSuccess ? TEXT("true") : TEXT("false"), Result.Diagnostics.Num(),
			*ModDiagnostics::Join(Result.Diagnostics)));
		return false;
	}

	return true;
}

/** A failing parse must report the expected code AND come back with bSuccess false. */
bool FModManifestSpec::ExpectParseFailure(const FString& What, const FModManifestParseResult& Result,
	const TCHAR* Code, const TCHAR* Context)
{
	bool bOk = true;

	if (Result.bSuccess)
	{
		AddError(FString::Printf(TEXT("%s: expected bSuccess to be false because an Error diagnostic was raised."), *What));
		bOk = false;
	}

	bOk &= ExpectDiagnostic(What, Result.Diagnostics, EModDiagnosticSeverity::Error, Code, Context);
	return bOk;
}

bool FModManifestSpec::ExpectVersionParses(const TCHAR* Text, const TCHAR* ExpectedCanonicalForm, bool bAllowPartial)
{
	FModVersion Version;
	FString Error;

	if (!FModVersion::Parse(Text, Version, &Error, bAllowPartial))
	{
		AddError(FString::Printf(TEXT("Expected '%s' to parse as a version but it was rejected: %s"), Text, *Error));
		return false;
	}

	return ExpectString(FString::Printf(TEXT("'%s' parsed"), Text), Version.ToString(), ExpectedCanonicalForm);
}

bool FModManifestSpec::ExpectVersionRejected(const TCHAR* Text, bool bAllowPartial)
{
	FModVersion Version(9, 9, 9);
	FString Error;

	if (FModVersion::Parse(Text, Version, &Error, bAllowPartial))
	{
		AddError(FString::Printf(TEXT("Expected '%s' to be rejected but it parsed as %s."), Text, *Version.ToString()));
		return false;
	}

	bool bOk = true;

	// A rejected parse must leave the output at 0.0.0 rather than half-filled, so a caller that
	// ignores the return value cannot end up with a plausible-looking wrong version.
	if (!Version.IsZero() || !Version.BuildMetadata.IsEmpty())
	{
		AddError(FString::Printf(TEXT("Rejecting '%s' left the output at %s instead of resetting it to 0.0.0."),
			Text, *Version.ToString()));
		bOk = false;
	}

	if (Error.IsEmpty())
	{
		AddError(FString::Printf(TEXT("Rejecting '%s' produced no error message."), Text));
		bOk = false;
	}

	return bOk;
}

/** Asserts Lower < Higher under every comparison operator, and that the reverse holds too. */
bool FModManifestSpec::ExpectVersionOrder(const TCHAR* Lower, const TCHAR* Higher)
{
	FModVersion A;
	FModVersion B;

	if (!FModVersion::Parse(Lower, A) || !FModVersion::Parse(Higher, B))
	{
		AddError(FString::Printf(TEXT("Could not parse the ordering pair '%s' / '%s'."), Lower, Higher));
		return false;
	}

	bool bOk = true;

	if (A.Compare(B) >= 0)
	{
		AddError(FString::Printf(TEXT("Expected '%s' to sort below '%s' but Compare returned %d."), Lower, Higher, A.Compare(B)));
		bOk = false;
	}

	if (B.Compare(A) <= 0)
	{
		AddError(FString::Printf(TEXT("Expected '%s' to sort above '%s' but Compare returned %d."), Higher, Lower, B.Compare(A)));
		bOk = false;
	}

	if (!(A < B) || !(A <= B) || (A > B) || (A >= B) || (A == B) || !(A != B))
	{
		AddError(FString::Printf(TEXT("The relational operators disagree with Compare for '%s' vs '%s'."), Lower, Higher));
		bOk = false;
	}

	return bOk;
}

bool FModManifestSpec::ExpectRangeSatisfied(const TCHAR* RangeText, const TCHAR* VersionText, bool bExpected)
{
	FModVersionRange Range;
	FString RangeError;
	if (!FModVersionRange::Parse(RangeText, Range, &RangeError))
	{
		AddError(FString::Printf(TEXT("Version range '%s' failed to parse: %s"), RangeText, *RangeError));
		return false;
	}

	FModVersion Version;
	FString VersionError;
	if (!FModVersion::Parse(VersionText, Version, &VersionError))
	{
		AddError(FString::Printf(TEXT("Version '%s' failed to parse: %s"), VersionText, *VersionError));
		return false;
	}

	const bool bActual = Range.Satisfies(Version);
	if (bActual != bExpected)
	{
		AddError(FString::Printf(TEXT("Expected '%s' %s satisfy '%s' (%s), but it %s."),
			VersionText,
			bExpected ? TEXT("to") : TEXT("not to"),
			RangeText,
			*Range.Describe(),
			bActual ? TEXT("did") : TEXT("did not")));
		return false;
	}

	return true;
}

bool FModManifestSpec::ExpectRangeRejected(const TCHAR* RangeText)
{
	FModVersionRange Range;
	FString Error;

	if (FModVersionRange::Parse(RangeText, Range, &Error))
	{
		AddError(FString::Printf(TEXT("Expected the version range '%s' to be rejected but it compiled to %s."),
			RangeText, *Range.Describe()));
		return false;
	}

	bool bOk = true;

	if (Error.IsEmpty())
	{
		AddError(FString::Printf(TEXT("Rejecting the version range '%s' produced no error message."), RangeText));
		bOk = false;
	}

	// A malformed range must never accept anything: silently matching everything would let a mod
	// with a typo in its constraint load against an incompatible dependency.
	if (Range.IsValid() || Range.IsAny() || Range.Satisfies(FModVersion(1, 0, 0)) || Range.Satisfies(FModVersion(0, 0, 0)))
	{
		AddError(FString::Printf(TEXT("The malformed version range '%s' still reports itself valid or satisfiable."), RangeText));
		bOk = false;
	}

	return bOk;
}

void FModManifestSpec::CompareManifests(const FString& What, const FModManifest& Expected, const FModManifest& Actual)
{
	TestEqual(*FString::Printf(TEXT("%s: manifestVersion"), *What), Actual.ManifestVersion, Expected.ManifestVersion);
	ExpectString(FString::Printf(TEXT("%s: id"), *What), Actual.Id.ToString(), Expected.Id.ToString());
	ExpectString(FString::Printf(TEXT("%s: name"), *What), Actual.DisplayName, Expected.DisplayName);
	ExpectString(FString::Printf(TEXT("%s: description"), *What), Actual.Description, Expected.Description);
	ExpectString(FString::Printf(TEXT("%s: author"), *What), Actual.Author, Expected.Author);
	ExpectString(FString::Printf(TEXT("%s: homepage"), *What), Actual.Homepage, Expected.Homepage);
	ExpectString(FString::Printf(TEXT("%s: license"), *What), Actual.License, Expected.License);
	ExpectString(FString::Printf(TEXT("%s: icon"), *What), Actual.IconPath, Expected.IconPath);

	// ToString rather than operator==, because operator== deliberately ignores build metadata and
	// this comparison has to prove the metadata survived the round trip.
	ExpectString(FString::Printf(TEXT("%s: version"), *What), Actual.Version.ToString(), Expected.Version.ToString());

	ExpectString(FString::Printf(TEXT("%s: game.id"), *What), Actual.Game.GameId, Expected.Game.GameId);
	ExpectString(FString::Printf(TEXT("%s: game.version"), *What), Actual.Game.VersionRange.Expression, Expected.Game.VersionRange.Expression);
	ExpectString(FString::Printf(TEXT("%s: framework.version"), *What), Actual.FrameworkVersionRange.Expression, Expected.FrameworkVersionRange.Expression);
	ExpectString(FString::Printf(TEXT("%s: sdk.id"), *What), Actual.Sdk.SdkId, Expected.Sdk.SdkId);
	ExpectString(FString::Printf(TEXT("%s: sdk.version"), *What), Actual.Sdk.VersionRange.Expression, Expected.Sdk.VersionRange.Expression);

	if (TestEqual(*FString::Printf(TEXT("%s: dependency count"), *What), Actual.Dependencies.Num(), Expected.Dependencies.Num()))
	{
		for (int32 Index = 0; Index < Expected.Dependencies.Num(); ++Index)
		{
			const FModDependency& E = Expected.Dependencies[Index];
			const FModDependency& A = Actual.Dependencies[Index];

			ExpectString(FString::Printf(TEXT("%s: dependencies[%d].id"), *What, Index), A.Id.ToString(), E.Id.ToString());
			ExpectString(FString::Printf(TEXT("%s: dependencies[%d].version"), *What, Index), A.VersionRange.Expression, E.VersionRange.Expression);
			TestEqual(*FString::Printf(TEXT("%s: dependencies[%d].optional"), *What, Index), A.bOptional ? 1 : 0, E.bOptional ? 1 : 0);
			ExpectString(FString::Printf(TEXT("%s: dependencies[%d].reason"), *What, Index), A.Reason, E.Reason);
		}
	}

	if (TestEqual(*FString::Printf(TEXT("%s: loadBefore count"), *What), Actual.LoadBefore.Num(), Expected.LoadBefore.Num()))
	{
		for (int32 Index = 0; Index < Expected.LoadBefore.Num(); ++Index)
		{
			ExpectString(FString::Printf(TEXT("%s: loadBefore[%d]"), *What, Index), Actual.LoadBefore[Index].ToString(), Expected.LoadBefore[Index].ToString());
		}
	}

	if (TestEqual(*FString::Printf(TEXT("%s: loadAfter count"), *What), Actual.LoadAfter.Num(), Expected.LoadAfter.Num()))
	{
		for (int32 Index = 0; Index < Expected.LoadAfter.Num(); ++Index)
		{
			ExpectString(FString::Printf(TEXT("%s: loadAfter[%d]"), *What, Index), Actual.LoadAfter[Index].ToString(), Expected.LoadAfter[Index].ToString());
		}
	}

	TestEqual(*FString::Printf(TEXT("%s: priority"), *What), Actual.Priority, Expected.Priority);
	ExpectString(FString::Printf(TEXT("%s: network.scope"), *What),
		ModFrameworkEnums::ToString(Actual.NetworkScope), ModFrameworkEnums::ToString(Expected.NetworkScope));
	TestEqual(*FString::Printf(TEXT("%s: network.requiredForNetworkPlay"), *What),
		Actual.bRequiredForNetworkPlay ? 1 : 0, Expected.bRequiredForNetworkPlay ? 1 : 0);

	if (TestEqual(*FString::Printf(TEXT("%s: permission count"), *What), Actual.RequestedPermissions.Num(), Expected.RequestedPermissions.Num()))
	{
		for (int32 Index = 0; Index < Expected.RequestedPermissions.Num(); ++Index)
		{
			ExpectString(FString::Printf(TEXT("%s: permissions[%d]"), *What, Index),
				Actual.RequestedPermissions[Index].ToString(), Expected.RequestedPermissions[Index].ToString());
		}
	}

	ExpectString(FString::Printf(TEXT("%s: entryPoint.nativeModule"), *What), Actual.EntryPoint.NativeModule, Expected.EntryPoint.NativeModule);
	ExpectString(FString::Printf(TEXT("%s: entryPoint.class"), *What), Actual.EntryPoint.EntryClass.ToString(), Expected.EntryPoint.EntryClass.ToString());

	if (TestEqual(*FString::Printf(TEXT("%s: content bundle count"), *What),
		Actual.EntryPoint.ContentBundles.Num(), Expected.EntryPoint.ContentBundles.Num()))
	{
		for (int32 Index = 0; Index < Expected.EntryPoint.ContentBundles.Num(); ++Index)
		{
			ExpectString(FString::Printf(TEXT("%s: entryPoint.contentBundles[%d]"), *What, Index),
				Actual.EntryPoint.ContentBundles[Index].ToString(), Expected.EntryPoint.ContentBundles[Index].ToString());
		}
	}

	if (TestEqual(*FString::Printf(TEXT("%s: content root count"), *What), Actual.ContentRoots.Num(), Expected.ContentRoots.Num()))
	{
		for (int32 Index = 0; Index < Expected.ContentRoots.Num(); ++Index)
		{
			const FModContentRoot& E = Expected.ContentRoots[Index];
			const FModContentRoot& A = Actual.ContentRoots[Index];

			ExpectString(FString::Printf(TEXT("%s: content[%d].path"), *What, Index), A.RelativePath, E.RelativePath);
			ExpectString(FString::Printf(TEXT("%s: content[%d].type"), *What, Index),
				ModFrameworkEnums::ToString(A.Type), ModFrameworkEnums::ToString(E.Type));
			ExpectString(FString::Printf(TEXT("%s: content[%d].mountPoint"), *What, Index), A.MountPoint, E.MountPoint);
			TestEqual(*FString::Printf(TEXT("%s: content[%d].mountOrder"), *What, Index), A.MountOrder, E.MountOrder);
		}
	}

	if (TestEqual(*FString::Printf(TEXT("%s: claim count"), *What), Actual.Claims.Num(), Expected.Claims.Num()))
	{
		for (int32 Index = 0; Index < Expected.Claims.Num(); ++Index)
		{
			const FModResourceClaimDeclaration& E = Expected.Claims[Index];
			const FModResourceClaimDeclaration& A = Actual.Claims[Index];

			ExpectString(FString::Printf(TEXT("%s: claims[%d].extensionPoint"), *What, Index), A.ExtensionPointId.ToString(), E.ExtensionPointId.ToString());
			ExpectString(FString::Printf(TEXT("%s: claims[%d].resource"), *What, Index), A.ResourceId.ToString(), E.ResourceId.ToString());
			ExpectString(FString::Printf(TEXT("%s: claims[%d].policy"), *What, Index),
				ModFrameworkEnums::ToString(A.PreferredPolicy), ModFrameworkEnums::ToString(E.PreferredPolicy));
		}
	}

	if (TestEqual(*FString::Printf(TEXT("%s: metadata count"), *What), Actual.Metadata.Num(), Expected.Metadata.Num()))
	{
		for (const TPair<FString, FString>& Pair : Expected.Metadata)
		{
			const FString* Found = Actual.Metadata.Find(Pair.Key);
			if (Found == nullptr)
			{
				AddError(FString::Printf(TEXT("%s: metadata key '%s' is missing."), *What, *Pair.Key));
				continue;
			}

			ExpectString(FString::Printf(TEXT("%s: metadata['%s']"), *What, *Pair.Key), *Found, Pair.Value);
		}
	}
}

//////////////////////////////////////////////////////////////////////////

void FModManifestSpec::Define()
{
	Describe("Version", [this]()
	{
		Describe("Parse", [this]()
		{
			It("accepts a full semantic version", [this]()
			{
				FModVersion Version;
				FString Error;

				TestTrue(TEXT("1.2.3 parses"), FModVersion::Parse(TEXT("1.2.3"), Version, &Error));
				TestEqual(TEXT("major"), Version.Major, 1);
				TestEqual(TEXT("minor"), Version.Minor, 2);
				TestEqual(TEXT("patch"), Version.Patch, 3);
				TestTrue(TEXT("no pre-release"), Version.PreRelease.IsEmpty());
				TestTrue(TEXT("no build metadata"), Version.BuildMetadata.IsEmpty());
				TestFalse(TEXT("1.2.3 is not a pre-release"), Version.IsPreRelease());
				TestFalse(TEXT("1.2.3 is not zero"), Version.IsZero());
				TestTrue(TEXT("the error string is left empty on success"), Error.IsEmpty());
			});

			It("accepts a pre-release and build metadata", [this]()
			{
				FModVersion Version;
				TestTrue(TEXT("parses"), FModVersion::Parse(TEXT("1.2.3-rc.1+build.7"), Version));
				ExpectString(TEXT("pre-release"), Version.PreRelease, TEXT("rc.1"));
				ExpectString(TEXT("build metadata"), Version.BuildMetadata, TEXT("build.7"));
				TestTrue(TEXT("is a pre-release"), Version.IsPreRelease());
				ExpectString(TEXT("round-trips"), Version.ToString(), TEXT("1.2.3-rc.1+build.7"));
			});

			// Build metadata is split off BEFORE the pre-release, so a '-' inside the metadata must
			// not be mistaken for the pre-release marker. Regressing this silently truncates versions.
			It("splits build metadata off before the pre-release", [this]()
			{
				FModVersion Version;
				TestTrue(TEXT("parses"), FModVersion::Parse(TEXT("1.2.3+build.7-extra"), Version));
				TestTrue(TEXT("no pre-release"), Version.PreRelease.IsEmpty());
				ExpectString(TEXT("build metadata keeps its hyphen"), Version.BuildMetadata, TEXT("build.7-extra"));
			});

			It("accepts a single leading v", [this]()
			{
				ExpectVersionParses(TEXT("v1.2.3"), TEXT("1.2.3"));
				ExpectVersionParses(TEXT("V1.2.3"), TEXT("1.2.3"));
				ExpectVersionParses(TEXT("v1.2.3-rc.1"), TEXT("1.2.3-rc.1"));
				ExpectVersionRejected(TEXT("v"));
				ExpectVersionRejected(TEXT("vv1.2.3"));
			});

			It("trims surrounding whitespace", [this]()
			{
				ExpectVersionParses(TEXT("  1.2.3  "), TEXT("1.2.3"));
			});

			It("normalises partial versions when partials are allowed", [this]()
			{
				ExpectVersionParses(TEXT("1"), TEXT("1.0.0"));
				ExpectVersionParses(TEXT("1.2"), TEXT("1.2.0"));
			});

			It("rejects partial versions when partials are not allowed", [this]()
			{
				ExpectVersionRejected(TEXT("1"), /*bAllowPartial=*/false);
				ExpectVersionRejected(TEXT("1.2"), /*bAllowPartial=*/false);
				ExpectVersionParses(TEXT("1.2.0"), TEXT("1.2.0"), /*bAllowPartial=*/false);
			});

			It("rejects leading zeroes in the core numbers", [this]()
			{
				ExpectVersionRejected(TEXT("01.2.3"));
				ExpectVersionRejected(TEXT("1.02.3"));
				ExpectVersionRejected(TEXT("1.2.03"));
				ExpectVersionParses(TEXT("0.0.0"), TEXT("0.0.0"));
				ExpectVersionParses(TEXT("1.0.0"), TEXT("1.0.0"));
			});

			It("rejects leading zeroes in numeric pre-release identifiers but allows them in build metadata", [this]()
			{
				ExpectVersionRejected(TEXT("1.2.3-00"));
				ExpectVersionRejected(TEXT("1.2.3-alpha.007"));
				ExpectVersionParses(TEXT("1.2.3-0"), TEXT("1.2.3-0"));
				// "0a" is alphanumeric, not numeric, so the leading zero rule does not apply.
				ExpectVersionParses(TEXT("1.2.3-0a"), TEXT("1.2.3-0a"));
				// Semver 2.0.0 explicitly allows leading zeroes in build metadata.
				ExpectVersionParses(TEXT("1.2.3+007"), TEXT("1.2.3+007"));
			});

			It("rejects negative numbers", [this]()
			{
				ExpectVersionRejected(TEXT("-1.2.3"));
				ExpectVersionRejected(TEXT("1.-2.3"));
				ExpectVersionRejected(TEXT("1.2.-3"));
			});

			It("rejects empty identifiers", [this]()
			{
				ExpectVersionRejected(TEXT("1.2.3-"));
				ExpectVersionRejected(TEXT("1.2.3+"));
				ExpectVersionRejected(TEXT("1.2.3-alpha..1"));
				ExpectVersionRejected(TEXT("1.2.3-.alpha"));
				ExpectVersionRejected(TEXT("1..3"));
				ExpectVersionRejected(TEXT("1.2."));
			});

			It("rejects more than three core numbers", [this]()
			{
				ExpectVersionRejected(TEXT("1.2.3.4"));
				ExpectVersionRejected(TEXT("1.2.3.4.5"));
			});

			It("rejects a pre-release or build suffix on a partial core", [this]()
			{
				ExpectVersionRejected(TEXT("1.2-rc"));
				ExpectVersionRejected(TEXT("1-rc"));
				ExpectVersionRejected(TEXT("1.2+build"));
			});

			It("rejects characters outside the identifier alphabet", [this]()
			{
				ExpectVersionRejected(TEXT("1.2.3-rc$1"));
				ExpectVersionRejected(TEXT("1.2.3+build/7"));
				ExpectVersionRejected(TEXT("1.2.x"));
				ExpectVersionRejected(TEXT("not-a-version"));
				// '-' is a legal identifier character, unlike '$' and '/'.
				ExpectVersionParses(TEXT("1.2.3-rc-1"), TEXT("1.2.3-rc-1"));
			});

			It("rejects numbers that do not fit in an int32", [this]()
			{
				ExpectVersionRejected(TEXT("99999999999.0.0"));
				ExpectVersionRejected(TEXT("2147483648.0.0"));
				ExpectVersionParses(TEXT("2147483647.0.0"), TEXT("2147483647.0.0"));
			});

			It("rejects the empty string", [this]()
			{
				ExpectVersionRejected(TEXT(""));
				ExpectVersionRejected(TEXT("   "));
			});

			It("FromString yields 0.0.0 for anything it cannot parse", [this]()
			{
				TestTrue(TEXT("garbage becomes 0.0.0"), FModVersion::FromString(TEXT("garbage")).IsZero());
				TestTrue(TEXT("an empty string becomes 0.0.0"), FModVersion::FromString(TEXT("")).IsZero());
				ExpectString(TEXT("a good version survives"), FModVersion::FromString(TEXT("3.2.1")).ToString(), TEXT("3.2.1"));
			});

			It("round-trips every canonical form through ToString", [this]()
			{
				const TCHAR* const Forms[] =
				{
					TEXT("0.0.0"),
					TEXT("1.2.3"),
					TEXT("1.2.3-rc.1"),
					TEXT("1.2.3+build.7"),
					TEXT("1.2.3-rc.1+build.7"),
					TEXT("10.20.30-alpha-1.2+x.y-z")
				};

				for (const TCHAR* Form : Forms)
				{
					ExpectVersionParses(Form, Form);
				}
			});
		});

		Describe("Precedence", [this]()
		{
			It("orders the core numbers numerically", [this]()
			{
				ExpectVersionOrder(TEXT("1.0.0"), TEXT("1.0.1"));
				ExpectVersionOrder(TEXT("1.0.9"), TEXT("1.1.0"));
				ExpectVersionOrder(TEXT("1.9.9"), TEXT("2.0.0"));
				// A plain string comparison would put 10 before 9 here.
				ExpectVersionOrder(TEXT("1.9.0"), TEXT("1.10.0"));
			});

			It("orders a pre-release below its release", [this]()
			{
				ExpectVersionOrder(TEXT("1.0.0-rc.1"), TEXT("1.0.0"));
				ExpectVersionOrder(TEXT("1.0.0-rc.1"), TEXT("1.0.1-rc.1"));
			});

			// The example ordering from the Semantic Versioning 2.0.0 specification, section 11.
			It("follows the semver 11.4 example ordering", [this]()
			{
				const TCHAR* const Ordered[] =
				{
					TEXT("1.0.0-alpha"),
					TEXT("1.0.0-alpha.1"),
					TEXT("1.0.0-alpha.beta"),
					TEXT("1.0.0-beta"),
					TEXT("1.0.0-beta.2"),
					TEXT("1.0.0-beta.11"),
					TEXT("1.0.0-rc.1"),
					TEXT("1.0.0")
				};

				constexpr int32 Count = UE_ARRAY_COUNT(Ordered);
				for (int32 Index = 0; Index + 1 < Count; ++Index)
				{
					ExpectVersionOrder(Ordered[Index], Ordered[Index + 1]);
				}

				// Transitivity: every earlier entry must sort below every later one, not just its
				// immediate neighbour.
				for (int32 Low = 0; Low < Count; ++Low)
				{
					for (int32 High = Low + 1; High < Count; ++High)
					{
						ExpectVersionOrder(Ordered[Low], Ordered[High]);
					}
				}
			});

			It("compares numeric identifiers numerically, not lexically", [this]()
			{
				ExpectVersionOrder(TEXT("1.0.0-2"), TEXT("1.0.0-10"));
				ExpectVersionOrder(TEXT("1.0.0-beta.9"), TEXT("1.0.0-beta.10"));
				ExpectVersionOrder(TEXT("1.0.0-rc.2"), TEXT("1.0.0-rc.11"));
			});

			It("sorts numeric identifiers below alphanumeric ones", [this]()
			{
				ExpectVersionOrder(TEXT("1.0.0-1"), TEXT("1.0.0-alpha"));
				ExpectVersionOrder(TEXT("1.0.0-999"), TEXT("1.0.0-a"));
				ExpectVersionOrder(TEXT("1.0.0-x.1"), TEXT("1.0.0-x.a"));
			});

			It("gives the longer identifier list the higher precedence when the shared identifiers match", [this]()
			{
				ExpectVersionOrder(TEXT("1.0.0-alpha"), TEXT("1.0.0-alpha.1"));
				ExpectVersionOrder(TEXT("1.0.0-alpha.1"), TEXT("1.0.0-alpha.1.0"));
			});

			It("ignores build metadata for ordering and equality", [this]()
			{
				FModVersion A;
				FModVersion B;
				TestTrue(TEXT("parses A"), FModVersion::Parse(TEXT("1.0.0+build.1"), A));
				TestTrue(TEXT("parses B"), FModVersion::Parse(TEXT("1.0.0+build.2"), B));

				TestEqual(TEXT("Compare ignores build metadata"), A.Compare(B), 0);
				TestTrue(TEXT("operator== ignores build metadata"), A == B);
				TestFalse(TEXT("operator!= ignores build metadata"), A != B);
				TestFalse(TEXT("neither sorts below the other"), A < B);
				TestFalse(TEXT("neither sorts above the other"), A > B);

				// ...but the text is preserved, which is what makes it worth parsing at all.
				ExpectString(TEXT("A keeps its metadata"), A.ToString(), TEXT("1.0.0+build.1"));
				ExpectString(TEXT("B keeps its metadata"), B.ToString(), TEXT("1.0.0+build.2"));

				FModVersion PreA;
				FModVersion PreB;
				TestTrue(TEXT("parses pre-release A"), FModVersion::Parse(TEXT("1.0.0-rc.1+a"), PreA));
				TestTrue(TEXT("parses pre-release B"), FModVersion::Parse(TEXT("1.0.0-rc.1+b"), PreB));
				TestEqual(TEXT("pre-release comparison also ignores build metadata"), PreA.Compare(PreB), 0);
			});

			It("hashes versions that compare equal to the same value", [this]()
			{
				const FModVersion A = FModVersion::FromString(TEXT("1.0.0+build.1"));
				const FModVersion B = FModVersion::FromString(TEXT("1.0.0+build.2"));
				TestEqual(TEXT("equal versions hash equally"), static_cast<int32>(GetTypeHash(A)), static_cast<int32>(GetTypeHash(B)));
			});

			// Sorting is the operation the dependency resolver relies on to pick a "best" candidate,
			// so the whole ordering is exercised through TArray::Sort as well as through Compare.
			It("sorts an array into the documented order", [this]()
			{
				TArray<FModVersion> Versions;
				Versions.Add(FModVersion::FromString(TEXT("1.0.0")));
				Versions.Add(FModVersion::FromString(TEXT("1.0.0-beta.11")));
				Versions.Add(FModVersion::FromString(TEXT("2.0.0")));
				Versions.Add(FModVersion::FromString(TEXT("1.0.0-alpha")));
				Versions.Add(FModVersion::FromString(TEXT("1.0.0-beta.2")));
				Versions.Add(FModVersion::FromString(TEXT("0.9.9")));
				Versions.Add(FModVersion::FromString(TEXT("1.0.0-alpha.1")));

				Versions.Sort();

				const TCHAR* const Expected[] =
				{
					TEXT("0.9.9"),
					TEXT("1.0.0-alpha"),
					TEXT("1.0.0-alpha.1"),
					TEXT("1.0.0-beta.2"),
					TEXT("1.0.0-beta.11"),
					TEXT("1.0.0"),
					TEXT("2.0.0")
				};

				if (TestEqual(TEXT("sorted count"), Versions.Num(), static_cast<int32>(UE_ARRAY_COUNT(Expected))))
				{
					for (int32 Index = 0; Index < Versions.Num(); ++Index)
					{
						ExpectString(FString::Printf(TEXT("sorted[%d]"), Index), Versions[Index].ToString(), Expected[Index]);
					}
				}
			});
		});
	});

	Describe("VersionRange", [this]()
	{
		Describe("Parse", [this]()
		{
			It("treats *, x, X, any and the empty string as any version", [this]()
			{
				const TCHAR* const AnyForms[] = { TEXT("*"), TEXT("x"), TEXT("X"), TEXT("any"), TEXT("ANY"), TEXT(""), TEXT("   ") };

				for (const TCHAR* Form : AnyForms)
				{
					FModVersionRange Range;
					if (!TestTrue(*FString::Printf(TEXT("'%s' parses"), Form), FModVersionRange::Parse(Form, Range)))
					{
						continue;
					}

					TestTrue(*FString::Printf(TEXT("'%s' is any"), Form), Range.IsAny());
					TestTrue(*FString::Printf(TEXT("'%s' is valid"), Form), Range.IsValid());
					TestTrue(*FString::Printf(TEXT("'%s' accepts 0.0.0"), Form), Range.Satisfies(FModVersion(0, 0, 0)));
					TestTrue(*FString::Printf(TEXT("'%s' accepts 9.9.9"), Form), Range.Satisfies(FModVersion(9, 9, 9)));

					// A clause with no comparators has nothing for the pre-release gate to key off,
					// so "any" really does mean any, pre-releases included.
					TestTrue(*FString::Printf(TEXT("'%s' accepts a pre-release"), Form),
						Range.Satisfies(FModVersion::FromString(TEXT("2.0.0-rc.1"))));
				}

				TestTrue(TEXT("Any() is any"), FModVersionRange::Any().IsAny());
				ExpectString(TEXT("Any() spells itself '*'"), FModVersionRange::Any().Expression, TEXT("*"));
			});

			It("preserves the source expression verbatim", [this]()
			{
				FModVersionRange Range;
				TestTrue(TEXT("parses"), FModVersionRange::Parse(TEXT("^1.2.3"), Range));
				ExpectString(TEXT("Expression"), Range.Expression, TEXT("^1.2.3"));
				ExpectString(TEXT("ToString"), Range.ToString(), TEXT("^1.2.3"));
				TestTrue(TEXT("bParsed is set by Parse"), Range.bParsed);
				TestFalse(TEXT("bParseFailed is clear"), Range.bParseFailed);
			});

			It("compiles a caret range into an inclusive lower and an exclusive upper bound", [this]()
			{
				FModVersionRange Range;
				TestTrue(TEXT("parses"), FModVersionRange::Parse(TEXT("^1.2.3"), Range));

				if (TestEqual(TEXT("one clause"), Range.Clauses.Num(), 1))
				{
					ExpectString(TEXT("clause"), Range.Clauses[0].ToString(), TEXT(">=1.2.3 <2.0.0"));

					if (TestEqual(TEXT("two comparators"), Range.Clauses[0].Comparators.Num(), 2))
					{
						ExpectString(TEXT("lower bound"), Range.Clauses[0].Comparators[0].ToString(), TEXT(">=1.2.3"));
						ExpectString(TEXT("upper bound"), Range.Clauses[0].Comparators[1].ToString(), TEXT("<2.0.0"));
					}
				}
			});

			It("spells an empty clause as *", [this]()
			{
				const FModVersionClause Empty;
				ExpectString(TEXT("empty clause"), Empty.ToString(), TEXT("*"));
				TestTrue(TEXT("an empty clause matches everything"), Empty.Satisfies(FModVersion(1, 2, 3)));
			});

			It("builds an exact range from Exactly", [this]()
			{
				const FModVersionRange Range = FModVersionRange::Exactly(FModVersion(1, 2, 3));

				ExpectString(TEXT("Expression"), Range.Expression, TEXT("1.2.3"));
				TestTrue(TEXT("valid"), Range.IsValid());
				TestFalse(TEXT("not any"), Range.IsAny());
				TestTrue(TEXT("accepts 1.2.3"), Range.Satisfies(FModVersion(1, 2, 3)));
				TestFalse(TEXT("rejects 1.2.4"), Range.Satisfies(FModVersion(1, 2, 4)));

				if (TestEqual(TEXT("one clause"), Range.Clauses.Num(), 1)
					&& TestEqual(TEXT("one comparator"), Range.Clauses[0].Comparators.Num(), 1))
				{
					ExpectString(TEXT("comparator"), Range.Clauses[0].Comparators[0].ToString(), TEXT("=1.2.3"));
				}
			});

			It("glues a detached operator onto the version that follows it", [this]()
			{
				ExpectRangeSatisfied(TEXT(">= 1.2.3"), TEXT("1.2.3"), true);
				ExpectRangeSatisfied(TEXT(">= 1.2.3"), TEXT("1.2.2"), false);
				ExpectRangeSatisfied(TEXT("^ 1.2.3"), TEXT("1.9.9"), true);
			});

			It("rejects malformed expressions", [this]()
			{
				// A concrete component may never follow a wildcard.
				ExpectRangeRejected(TEXT("1.x.3"));
				// An operator with nothing after it.
				ExpectRangeRejected(TEXT(">="));
				ExpectRangeRejected(TEXT(">= <="));
				// '!=' needs an exact major.minor.patch.
				ExpectRangeRejected(TEXT("!=1.2"));
				// '>' and '<' cannot be applied to a wildcard.
				ExpectRangeRejected(TEXT(">*"));
				ExpectRangeRejected(TEXT("<x"));
				// Empty '||' clauses.
				ExpectRangeRejected(TEXT("^1.0.0 ||"));
				ExpectRangeRejected(TEXT("|| ^1.0.0"));
				ExpectRangeRejected(TEXT("^1.0.0 || || ^2.0.0"));
				// A hyphen range needs exactly "<from> - <to>".
				ExpectRangeRejected(TEXT("1.0.0 - 2.0.0 - 3.0.0"));
				ExpectRangeRejected(TEXT("- 2.0.0"));
				// Plain nonsense.
				ExpectRangeRejected(TEXT("not a version"));
				ExpectRangeRejected(TEXT("1.2.3.4"));
				ExpectRangeRejected(TEXT("^01.2.3"));
			});

			It("reports a failed parse through bParseFailed and refuses every version", [this]()
			{
				FModVersionRange Range;
				TestFalse(TEXT("parse fails"), FModVersionRange::Parse(TEXT("garbage"), Range));
				TestTrue(TEXT("bParsed is set even on failure"), Range.bParsed);
				TestTrue(TEXT("bParseFailed is set"), Range.bParseFailed);
				TestEqual(TEXT("no clauses are left behind"), Range.Clauses.Num(), 0);
				TestFalse(TEXT("IsValid is false"), Range.IsValid());
				TestFalse(TEXT("IsAny is false"), Range.IsAny());
				TestFalse(TEXT("nothing satisfies it"), Range.Satisfies(FModVersion(1, 0, 0)));
				ExpectString(TEXT("the author's text is preserved for diagnostics"), Range.Expression, TEXT("garbage"));
			});

			// Documented thread-safety guarantee: the const query functions compile the expression
			// into a LOCAL array, so a shared const range can be queried from several threads.
			// Do not "optimise" this into a cached mutable parse.
			It("never mutates a range when a const query has to compile the expression", [this]()
			{
				FModVersionRange Range;
				Range.Expression = TEXT("^1.2.3");

				const FModVersionRange& ConstRange = Range;
				TestTrue(TEXT("an unparsed range still answers Satisfies"), ConstRange.Satisfies(FModVersion(1, 2, 4)));
				TestFalse(TEXT("an unparsed range still rejects out-of-range versions"), ConstRange.Satisfies(FModVersion(2, 0, 0)));
				TestTrue(TEXT("an unparsed range still answers IsValid"), ConstRange.IsValid());
				TestFalse(TEXT("an unparsed range still answers IsAny"), ConstRange.IsAny());
				ExpectString(TEXT("Describe works on an unparsed range"), ConstRange.Describe(), TEXT(">= 1.2.3 and < 2.0.0"));

				TestFalse(TEXT("bParsed was not flipped by a const query"), Range.bParsed);
				TestEqual(TEXT("no clauses were cached by a const query"), Range.Clauses.Num(), 0);

				FModVersionRange Bad;
				Bad.Expression = TEXT("!=1.2");
				const FModVersionRange& ConstBad = Bad;
				TestFalse(TEXT("an unparsed malformed range is not valid"), ConstBad.IsValid());
				TestFalse(TEXT("an unparsed malformed range satisfies nothing"), ConstBad.Satisfies(FModVersion(1, 2, 0)));
				TestFalse(TEXT("an unparsed malformed range is not any"), ConstBad.IsAny());
			});

			It("compares and hashes ranges on their expression", [this]()
			{
				FModVersionRange A;
				FModVersionRange B;
				FModVersionRange C;
				TestTrue(TEXT("parses A"), FModVersionRange::Parse(TEXT("^1.0.0"), A));
				TestTrue(TEXT("parses B"), FModVersionRange::Parse(TEXT("^1.0.0"), B));
				TestTrue(TEXT("parses C"), FModVersionRange::Parse(TEXT(">=1.0.0 <2.0.0"), C));

				TestTrue(TEXT("identical expressions are equal"), A == B);
				TestFalse(TEXT("identical expressions are not unequal"), A != B);
				TestEqual(TEXT("identical expressions hash equally"), static_cast<int32>(GetTypeHash(A)), static_cast<int32>(GetTypeHash(B)));
				TestTrue(TEXT("different spellings of the same constraint are not equal"), A != C);
			});
		});

		Describe("Operators", [this]()
		{
			It("handles an exact version", [this]()
			{
				ExpectRangeSatisfied(TEXT("1.2.3"), TEXT("1.2.3"), true);
				ExpectRangeSatisfied(TEXT("1.2.3"), TEXT("1.2.4"), false);
				ExpectRangeSatisfied(TEXT("1.2.3"), TEXT("1.2.2"), false);
				ExpectRangeSatisfied(TEXT("=1.2.3"), TEXT("1.2.3"), true);
				ExpectRangeSatisfied(TEXT("==1.2.3"), TEXT("1.2.3"), true);
				ExpectRangeSatisfied(TEXT("=1.2.3"), TEXT("1.2.4"), false);
			});

			It("handles !=", [this]()
			{
				ExpectRangeSatisfied(TEXT("!=1.5.0"), TEXT("1.5.0"), false);
				ExpectRangeSatisfied(TEXT("!=1.5.0"), TEXT("1.5.1"), true);
				ExpectRangeSatisfied(TEXT("!=1.5.0"), TEXT("0.0.1"), true);
			});

			It("handles > and >=", [this]()
			{
				ExpectRangeSatisfied(TEXT(">1.2.3"), TEXT("1.2.4"), true);
				ExpectRangeSatisfied(TEXT(">1.2.3"), TEXT("1.2.3"), false);
				ExpectRangeSatisfied(TEXT(">1.2.3"), TEXT("1.2.2"), false);
				ExpectRangeSatisfied(TEXT(">=1.2.3"), TEXT("1.2.3"), true);
				ExpectRangeSatisfied(TEXT(">=1.2.3"), TEXT("1.2.2"), false);
				ExpectRangeSatisfied(TEXT(">=1.2.3"), TEXT("9.0.0"), true);
			});

			It("handles < and <=", [this]()
			{
				ExpectRangeSatisfied(TEXT("<2.0.0"), TEXT("1.9.9"), true);
				ExpectRangeSatisfied(TEXT("<2.0.0"), TEXT("2.0.0"), false);
				ExpectRangeSatisfied(TEXT("<=2.0.0"), TEXT("2.0.0"), true);
				ExpectRangeSatisfied(TEXT("<=2.0.0"), TEXT("2.0.1"), false);
			});

			// A partial version widens the bound to the whole line it names: ">1.2" means "newer than
			// everything in the 1.2 line", i.e. >=1.3.0 - not >1.2.0.
			It("widens an operator applied to a partial version to the whole line", [this]()
			{
				ExpectRangeSatisfied(TEXT(">1.2"), TEXT("1.2.9"), false);
				ExpectRangeSatisfied(TEXT(">1.2"), TEXT("1.3.0"), true);
				ExpectRangeSatisfied(TEXT(">1"), TEXT("1.9.9"), false);
				ExpectRangeSatisfied(TEXT(">1"), TEXT("2.0.0"), true);

				ExpectRangeSatisfied(TEXT("<=2"), TEXT("2.9.9"), true);
				ExpectRangeSatisfied(TEXT("<=2"), TEXT("3.0.0"), false);
				ExpectRangeSatisfied(TEXT("<=1.2"), TEXT("1.2.9"), true);
				ExpectRangeSatisfied(TEXT("<=1.2"), TEXT("1.3.0"), false);

				ExpectRangeSatisfied(TEXT(">=1.2"), TEXT("1.2.0"), true);
				ExpectRangeSatisfied(TEXT(">=1.2"), TEXT("1.1.9"), false);
				ExpectRangeSatisfied(TEXT("<1.2"), TEXT("1.1.9"), true);
				ExpectRangeSatisfied(TEXT("<1.2"), TEXT("1.2.0"), false);

				ExpectRangeSatisfied(TEXT("=1.2"), TEXT("1.2.9"), true);
				ExpectRangeSatisfied(TEXT("=1.2"), TEXT("1.3.0"), false);
			});

			It("treats >=* and <=* as any", [this]()
			{
				ExpectRangeSatisfied(TEXT(">=*"), TEXT("0.0.1"), true);
				ExpectRangeSatisfied(TEXT("<=*"), TEXT("9.9.9"), true);
				ExpectRangeSatisfied(TEXT("=*"), TEXT("4.5.6"), true);
			});
		});

		Describe("Caret", [this]()
		{
			It("keeps the major version for 1.x", [this]()
			{
				ExpectRangeSatisfied(TEXT("^1.2.3"), TEXT("1.2.3"), true);
				ExpectRangeSatisfied(TEXT("^1.2.3"), TEXT("1.2.4"), true);
				ExpectRangeSatisfied(TEXT("^1.2.3"), TEXT("1.9.9"), true);
				ExpectRangeSatisfied(TEXT("^1.2.3"), TEXT("1.2.2"), false);
				ExpectRangeSatisfied(TEXT("^1.2.3"), TEXT("2.0.0"), false);
				ExpectRangeSatisfied(TEXT("^1.2.3"), TEXT("0.9.9"), false);
			});

			It("keeps the minor version for 0.x", [this]()
			{
				ExpectRangeSatisfied(TEXT("^0.2.3"), TEXT("0.2.3"), true);
				ExpectRangeSatisfied(TEXT("^0.2.3"), TEXT("0.2.9"), true);
				ExpectRangeSatisfied(TEXT("^0.2.3"), TEXT("0.3.0"), false);
				ExpectRangeSatisfied(TEXT("^0.2.3"), TEXT("0.2.2"), false);
			});

			It("keeps the patch version for 0.0.x", [this]()
			{
				ExpectRangeSatisfied(TEXT("^0.0.3"), TEXT("0.0.3"), true);
				ExpectRangeSatisfied(TEXT("^0.0.3"), TEXT("0.0.4"), false);
				ExpectRangeSatisfied(TEXT("^0.0.3"), TEXT("0.0.2"), false);
			});

			It("handles partial caret ranges", [this]()
			{
				ExpectRangeSatisfied(TEXT("^1"), TEXT("1.9.9"), true);
				ExpectRangeSatisfied(TEXT("^1"), TEXT("2.0.0"), false);
				ExpectRangeSatisfied(TEXT("^1.2"), TEXT("1.9.9"), true);
				ExpectRangeSatisfied(TEXT("^1.2"), TEXT("1.1.9"), false);
				ExpectRangeSatisfied(TEXT("^1.2"), TEXT("2.0.0"), false);

				// ^0 -> >=0.0.0 <1.0.0 and ^0.0 -> >=0.0.0 <0.1.0.
				ExpectRangeSatisfied(TEXT("^0"), TEXT("0.9.9"), true);
				ExpectRangeSatisfied(TEXT("^0"), TEXT("1.0.0"), false);
				ExpectRangeSatisfied(TEXT("^0.0"), TEXT("0.0.9"), true);
				ExpectRangeSatisfied(TEXT("^0.0"), TEXT("0.1.0"), false);

				// "^*" degrades to "*".
				ExpectRangeSatisfied(TEXT("^*"), TEXT("7.7.7"), true);
			});
		});

		Describe("Tilde", [this]()
		{
			It("pins the minor version", [this]()
			{
				ExpectRangeSatisfied(TEXT("~1.2.3"), TEXT("1.2.3"), true);
				ExpectRangeSatisfied(TEXT("~1.2.3"), TEXT("1.2.9"), true);
				ExpectRangeSatisfied(TEXT("~1.2.3"), TEXT("1.3.0"), false);
				ExpectRangeSatisfied(TEXT("~1.2.3"), TEXT("1.2.2"), false);
			});

			It("handles partial tilde ranges", [this]()
			{
				ExpectRangeSatisfied(TEXT("~1.2"), TEXT("1.2.0"), true);
				ExpectRangeSatisfied(TEXT("~1.2"), TEXT("1.2.9"), true);
				ExpectRangeSatisfied(TEXT("~1.2"), TEXT("1.3.0"), false);
				ExpectRangeSatisfied(TEXT("~1"), TEXT("1.9.9"), true);
				ExpectRangeSatisfied(TEXT("~1"), TEXT("2.0.0"), false);
			});

			It("treats ~> the same as ~", [this]()
			{
				ExpectRangeSatisfied(TEXT("~>1.2.3"), TEXT("1.2.9"), true);
				ExpectRangeSatisfied(TEXT("~>1.2.3"), TEXT("1.3.0"), false);
			});
		});

		Describe("Hyphen", [this]()
		{
			It("is inclusive at both ends", [this]()
			{
				ExpectRangeSatisfied(TEXT("1.2.3 - 2.0.0"), TEXT("1.2.3"), true);
				ExpectRangeSatisfied(TEXT("1.2.3 - 2.0.0"), TEXT("1.5.0"), true);
				ExpectRangeSatisfied(TEXT("1.2.3 - 2.0.0"), TEXT("2.0.0"), true);
				ExpectRangeSatisfied(TEXT("1.2.3 - 2.0.0"), TEXT("2.0.1"), false);
				ExpectRangeSatisfied(TEXT("1.2.3 - 2.0.0"), TEXT("1.2.2"), false);
			});

			It("extends a partial upper bound to the whole line it names", [this]()
			{
				ExpectRangeSatisfied(TEXT("1.2.3 - 2.3"), TEXT("2.3.9"), true);
				ExpectRangeSatisfied(TEXT("1.2.3 - 2.3"), TEXT("2.4.0"), false);
				ExpectRangeSatisfied(TEXT("1.2.3 - 2"), TEXT("2.9.9"), true);
				ExpectRangeSatisfied(TEXT("1.2.3 - 2"), TEXT("3.0.0"), false);
			});
		});

		Describe("Wildcards", [this]()
		{
			It("expands a trailing wildcard to the line it names", [this]()
			{
				ExpectRangeSatisfied(TEXT("1.2.x"), TEXT("1.2.0"), true);
				ExpectRangeSatisfied(TEXT("1.2.x"), TEXT("1.2.9"), true);
				ExpectRangeSatisfied(TEXT("1.2.x"), TEXT("1.3.0"), false);
				ExpectRangeSatisfied(TEXT("1.2.x"), TEXT("1.1.9"), false);

				ExpectRangeSatisfied(TEXT("1.x"), TEXT("1.0.0"), true);
				ExpectRangeSatisfied(TEXT("1.x"), TEXT("1.9.9"), true);
				ExpectRangeSatisfied(TEXT("1.x"), TEXT("2.0.0"), false);
				ExpectRangeSatisfied(TEXT("1.x"), TEXT("0.9.9"), false);

				ExpectRangeSatisfied(TEXT("1"), TEXT("1.9.9"), true);
				ExpectRangeSatisfied(TEXT("1"), TEXT("2.0.0"), false);

				// '*' and 'X' work in the same positions as 'x'.
				ExpectRangeSatisfied(TEXT("1.2.*"), TEXT("1.2.5"), true);
				ExpectRangeSatisfied(TEXT("1.2.*"), TEXT("1.3.0"), false);
				ExpectRangeSatisfied(TEXT("1.X"), TEXT("1.4.0"), true);
			});
		});

		Describe("Logic", [this]()
		{
			It("ANDs space and comma separated comparators", [this]()
			{
				ExpectRangeSatisfied(TEXT(">=1.2.3 <2.0.0"), TEXT("1.5.0"), true);
				ExpectRangeSatisfied(TEXT(">=1.2.3 <2.0.0"), TEXT("1.2.3"), true);
				ExpectRangeSatisfied(TEXT(">=1.2.3 <2.0.0"), TEXT("2.0.0"), false);
				ExpectRangeSatisfied(TEXT(">=1.2.3 <2.0.0"), TEXT("1.2.2"), false);

				ExpectRangeSatisfied(TEXT(">=1.2.3, <2.0.0"), TEXT("1.5.0"), true);
				ExpectRangeSatisfied(TEXT(">=1.2.3, <2.0.0"), TEXT("2.0.0"), false);

				ExpectRangeSatisfied(TEXT(">=1.0.0 <2.0.0 !=1.5.0"), TEXT("1.5.0"), false);
				ExpectRangeSatisfied(TEXT(">=1.0.0 <2.0.0 !=1.5.0"), TEXT("1.5.1"), true);
			});

			It("ORs || separated clauses", [this]()
			{
				ExpectRangeSatisfied(TEXT("^1.0.0 || ^2.0.0"), TEXT("1.5.0"), true);
				ExpectRangeSatisfied(TEXT("^1.0.0 || ^2.0.0"), TEXT("2.5.0"), true);
				ExpectRangeSatisfied(TEXT("^1.0.0 || ^2.0.0"), TEXT("3.0.0"), false);
				ExpectRangeSatisfied(TEXT("^1.0.0 || ^2.0.0"), TEXT("0.9.0"), false);

				ExpectRangeSatisfied(TEXT("1.0.0 || 2.0.0"), TEXT("1.0.0"), true);
				ExpectRangeSatisfied(TEXT("1.0.0 || 2.0.0"), TEXT("2.0.0"), true);
				ExpectRangeSatisfied(TEXT("1.0.0 || 2.0.0"), TEXT("1.5.0"), false);

				// A single "any" clause makes the whole disjunction "any".
				ExpectRangeSatisfied(TEXT("^1.0.0 || *"), TEXT("9.9.9"), true);
			});

			It("describes a range in human words", [this]()
			{
				FModVersionRange Range;

				TestTrue(TEXT("parses *"), FModVersionRange::Parse(TEXT("*"), Range));
				ExpectString(TEXT("* description"), Range.Describe(), TEXT("any version"));

				TestTrue(TEXT("parses 1.2.3"), FModVersionRange::Parse(TEXT("1.2.3"), Range));
				ExpectString(TEXT("exact description"), Range.Describe(), TEXT("exactly 1.2.3"));

				TestTrue(TEXT("parses >=1.2.3"), FModVersionRange::Parse(TEXT(">=1.2.3"), Range));
				ExpectString(TEXT(">= description"), Range.Describe(), TEXT("1.2.3 or newer"));

				TestTrue(TEXT("parses <=1.2.3"), FModVersionRange::Parse(TEXT("<=1.2.3"), Range));
				ExpectString(TEXT("<= description"), Range.Describe(), TEXT("1.2.3 or older"));

				TestTrue(TEXT("parses !=1.2.3"), FModVersionRange::Parse(TEXT("!=1.2.3"), Range));
				ExpectString(TEXT("!= description"), Range.Describe(), TEXT("any version other than 1.2.3"));

				TestTrue(TEXT("parses the AND form"), FModVersionRange::Parse(TEXT(">=1.2.3 <2.0.0"), Range));
				ExpectString(TEXT("AND description"), Range.Describe(), TEXT(">= 1.2.3 and < 2.0.0"));

				TestTrue(TEXT("parses the OR form"), FModVersionRange::Parse(TEXT("^1.0.0 || ^2.0.0"), Range));
				ExpectString(TEXT("OR description"), Range.Describe(), TEXT(">= 1.0.0 and < 2.0.0, or >= 2.0.0 and < 3.0.0"));

				FModVersionRange Bad;
				FModVersionRange::Parse(TEXT("garbage"), Bad);
				TestTrue(TEXT("a malformed range says so"), Bad.Describe().Contains(TEXT("invalid version range")));
			});
		});

		// The npm pre-release gate. Without it a mod asking for ">=1.0.0" would silently accept
		// 2.0.0-rc.1, i.e. an unreleased build of the next major version. Every one of these
		// assertions is load bearing; do not relax them.
		Describe("PreReleaseGate", [this]()
		{
			It("keeps a pre-release out of a range that does not name one", [this]()
			{
				ExpectRangeSatisfied(TEXT(">=1.0.0"), TEXT("2.0.0-rc.1"), false);
				ExpectRangeSatisfied(TEXT(">=2.0.0"), TEXT("2.0.0-rc.1"), false);
				ExpectRangeSatisfied(TEXT("<3.0.0"), TEXT("2.0.0-rc.1"), false);
				ExpectRangeSatisfied(TEXT("^1.2.3"), TEXT("1.2.4-rc.1"), false);
				ExpectRangeSatisfied(TEXT("~1.2.3"), TEXT("1.2.4-rc.1"), false);
				ExpectRangeSatisfied(TEXT("1.0.0 - 3.0.0"), TEXT("2.0.0-rc.1"), false);
				ExpectRangeSatisfied(TEXT("1.x"), TEXT("1.5.0-rc.1"), false);
			});

			It("lets a pre-release in when the range names one on the same major.minor.patch", [this]()
			{
				ExpectRangeSatisfied(TEXT(">=2.0.0-rc.1"), TEXT("2.0.0-rc.1"), true);
				ExpectRangeSatisfied(TEXT(">=2.0.0-rc.1"), TEXT("2.0.0-rc.2"), true);
				ExpectRangeSatisfied(TEXT(">=2.0.0-rc.2"), TEXT("2.0.0-rc.1"), false);
				ExpectRangeSatisfied(TEXT(">=1.2.3-rc.1 <2.0.0"), TEXT("1.2.3-rc.2"), true);
				ExpectRangeSatisfied(TEXT(">=1.2.3-alpha"), TEXT("1.2.3-rc.1"), true);
			});

			// The gate keys on the exact major.minor.patch of a comparator, so a pre-release of a
			// *different* patch is still shut out even though the comparator mentions a pre-release.
			It("only opens for the exact major.minor.patch the comparator names", [this]()
			{
				ExpectRangeSatisfied(TEXT(">=1.2.3-alpha"), TEXT("1.2.4-rc.1"), false);
				ExpectRangeSatisfied(TEXT(">=1.2.3-alpha <2.0.0"), TEXT("1.9.0-rc.1"), false);
			});

			It("applies the gate per clause, not across the whole range", [this]()
			{
				ExpectRangeSatisfied(TEXT("^1.0.0 || >=2.0.0-rc.1"), TEXT("2.0.0-rc.1"), true);
				// The pre-release comparator lives in the other clause, so it must not open the gate
				// for this one.
				ExpectRangeSatisfied(TEXT(">=2.0.0-rc.1 || ^3.0.0"), TEXT("3.5.0-beta.1"), false);
			});

			It("never gates a release version", [this]()
			{
				ExpectRangeSatisfied(TEXT(">=2.0.0-rc.1"), TEXT("2.0.0"), true);
				ExpectRangeSatisfied(TEXT(">=2.0.0-rc.1"), TEXT("9.9.9"), true);
			});

			It("lets any pre-release through a wildcard range", [this]()
			{
				ExpectRangeSatisfied(TEXT("*"), TEXT("2.0.0-rc.1"), true);
				ExpectRangeSatisfied(TEXT("any"), TEXT("0.1.0-alpha.1"), true);
			});
		});
	});

	Describe("Parsing", [this]()
	{
		It("reads every field of a full manifest", [this]()
		{
			const FModManifestParseResult Result = FModManifestParser::ParseFromString(GetFullManifestJson());

			if (!ExpectNoDiagnostics(TEXT("the full manifest"), Result))
			{
				return;
			}

			const FModManifest& M = Result.Manifest;

			TestEqual(TEXT("manifestVersion"), M.ManifestVersion, 1);
			ExpectString(TEXT("id"), M.Id.ToString(), TEXT("com.example.bettercombat"));
			ExpectString(TEXT("name"), M.DisplayName, TEXT("Better Combat"));
			ExpectString(TEXT("description"), M.Description, TEXT("Rebalances melee combat."));
			ExpectString(TEXT("author"), M.Author, TEXT("Example Author"));
			ExpectString(TEXT("homepage"), M.Homepage, TEXT("https://example.com"));
			ExpectString(TEXT("license"), M.License, TEXT("MIT"));
			ExpectString(TEXT("icon"), M.IconPath, TEXT("Art/Icon.png"));

			ExpectString(TEXT("version"), M.Version.ToString(), TEXT("2.1.0-rc.1+build.7"));
			TestEqual(TEXT("version major"), M.Version.Major, 2);
			TestEqual(TEXT("version minor"), M.Version.Minor, 1);
			TestEqual(TEXT("version patch"), M.Version.Patch, 0);
			ExpectString(TEXT("version pre-release"), M.Version.PreRelease, TEXT("rc.1"));
			ExpectString(TEXT("version build metadata"), M.Version.BuildMetadata, TEXT("build.7"));

			ExpectString(TEXT("game.id"), M.Game.GameId, TEXT("com.example.game"));
			ExpectString(TEXT("game.version"), M.Game.VersionRange.Expression, TEXT(">=1.5.0 <2.0.0"));
			TestTrue(TEXT("game.version accepts 1.6.0"), M.Game.VersionRange.Satisfies(FModVersion(1, 6, 0)));
			TestFalse(TEXT("game.version rejects 2.0.0"), M.Game.VersionRange.Satisfies(FModVersion(2, 0, 0)));

			ExpectString(TEXT("framework.version"), M.FrameworkVersionRange.Expression, TEXT("^0.1.0"));
			ExpectString(TEXT("sdk.id"), M.Sdk.SdkId, TEXT("com.example.game.sdk"));
			ExpectString(TEXT("sdk.version"), M.Sdk.VersionRange.Expression, TEXT("^1.5.0"));

			if (TestEqual(TEXT("dependency count"), M.Dependencies.Num(), 2))
			{
				ExpectString(TEXT("dependencies[0].id"), M.Dependencies[0].Id.ToString(), TEXT("com.example.corelib"));
				ExpectString(TEXT("dependencies[0].version"), M.Dependencies[0].VersionRange.Expression, TEXT(">=2.0.0"));
				TestFalse(TEXT("dependencies[0] is required"), M.Dependencies[0].bOptional);
				TestTrue(TEXT("dependencies[0] has no reason"), M.Dependencies[0].Reason.IsEmpty());

				ExpectString(TEXT("dependencies[1].id"), M.Dependencies[1].Id.ToString(), TEXT("com.example.ui"));
				ExpectString(TEXT("dependencies[1].version"), M.Dependencies[1].VersionRange.Expression, TEXT("*"));
				TestTrue(TEXT("dependencies[1] is optional"), M.Dependencies[1].bOptional);
				ExpectString(TEXT("dependencies[1].reason"), M.Dependencies[1].Reason, TEXT("Adds a settings page."));
			}

			if (TestEqual(TEXT("loadBefore count"), M.LoadBefore.Num(), 1))
			{
				ExpectString(TEXT("loadBefore[0]"), M.LoadBefore[0].ToString(), TEXT("com.example.late"));
			}
			if (TestEqual(TEXT("loadAfter count"), M.LoadAfter.Num(), 1))
			{
				ExpectString(TEXT("loadAfter[0]"), M.LoadAfter[0].ToString(), TEXT("com.example.early"));
			}

			TestEqual(TEXT("priority"), M.Priority, 10);
			ExpectString(TEXT("network.scope"), ModFrameworkEnums::ToString(M.NetworkScope), TEXT("ServerOnly"));
			TestTrue(TEXT("network.requiredForNetworkPlay"), M.bRequiredForNetworkPlay);

			if (TestEqual(TEXT("permission count"), M.RequestedPermissions.Num(), 2))
			{
				ExpectString(TEXT("permissions[0]"), M.RequestedPermissions[0].ToString(), TEXT("gameplay.modify"));
				ExpectString(TEXT("permissions[1]"), M.RequestedPermissions[1].ToString(), TEXT("save.modify"));
			}

			ExpectString(TEXT("entryPoint.nativeModule"), M.EntryPoint.NativeModule, TEXT("BetterCombatRuntime"));
			ExpectString(TEXT("entryPoint.class"), M.EntryPoint.EntryClass.ToString(),
				TEXT("/BetterCombat/BP_BetterCombatEntry.BP_BetterCombatEntry_C"));
			if (TestEqual(TEXT("content bundle count"), M.EntryPoint.ContentBundles.Num(), 1))
			{
				ExpectString(TEXT("entryPoint.contentBundles[0]"), M.EntryPoint.ContentBundles[0].ToString(),
					TEXT("/BetterCombat/DA_BetterCombatBundle.DA_BetterCombatBundle"));
			}

			if (TestEqual(TEXT("content root count"), M.ContentRoots.Num(), 2))
			{
				ExpectString(TEXT("content[0].path"), M.ContentRoots[0].RelativePath, TEXT("Content/BetterCombat.pak"));
				ExpectString(TEXT("content[0].type"), ModFrameworkEnums::ToString(M.ContentRoots[0].Type), TEXT("Pak"));
				ExpectString(TEXT("content[0].mountPoint"), M.ContentRoots[0].MountPoint, TEXT("/BetterCombat/"));
				TestEqual(TEXT("content[0].mountOrder"), M.ContentRoots[0].MountOrder, 0);

				ExpectString(TEXT("content[1].path"), M.ContentRoots[1].RelativePath, TEXT("Content/Loose"));
				ExpectString(TEXT("content[1].type"), ModFrameworkEnums::ToString(M.ContentRoots[1].Type), TEXT("LooseDirectory"));
				ExpectString(TEXT("content[1].mountPoint"), M.ContentRoots[1].MountPoint, TEXT("/BetterCombatLoose/"));
				TestEqual(TEXT("content[1].mountOrder"), M.ContentRoots[1].MountOrder, 7);
			}

			if (TestEqual(TEXT("claim count"), M.Claims.Num(), 1))
			{
				ExpectString(TEXT("claims[0].extensionPoint"), M.Claims[0].ExtensionPointId.ToString(), TEXT("game.weapon"));
				ExpectString(TEXT("claims[0].resource"), M.Claims[0].ResourceId.ToString(), TEXT("weapon.longsword"));
				ExpectString(TEXT("claims[0].policy"), ModFrameworkEnums::ToString(M.Claims[0].PreferredPolicy), TEXT("Priority"));
			}

			if (TestEqual(TEXT("metadata count"), M.Metadata.Num(), 2))
			{
				const FString* Category = M.Metadata.Find(TEXT("category"));
				const FString* Tags = M.Metadata.Find(TEXT("tags"));
				if (TestTrue(TEXT("metadata['category'] exists"), Category != nullptr))
				{
					ExpectString(TEXT("metadata['category']"), *Category, TEXT("Gameplay"));
				}
				if (TestTrue(TEXT("metadata['tags'] exists"), Tags != nullptr))
				{
					ExpectString(TEXT("metadata['tags']"), *Tags, TEXT("combat,melee"));
				}
			}
		});

		// Documented guarantee: ParseFromString(SerializeToString(M)) reproduces every field of any
		// M that itself came out of ParseFromString. Fields at their default value are omitted by the
		// writer, which is exactly where a round trip usually breaks.
		It("round-trips a full manifest through serialise and parse", [this]()
		{
			const FModManifestParseResult First = FModManifestParser::ParseFromString(GetFullManifestJson());
			if (!ExpectNoDiagnostics(TEXT("the first parse"), First))
			{
				return;
			}

			FString Serialized;
			if (!TestTrue(TEXT("serialises"), FModManifestParser::SerializeToString(First.Manifest, Serialized)))
			{
				return;
			}
			TestFalse(TEXT("the serialised manifest is not empty"), Serialized.IsEmpty());

			const FModManifestParseResult Second = FModManifestParser::ParseFromString(Serialized);
			if (!ExpectNoDiagnostics(TEXT("the re-parse"), Second))
			{
				return;
			}

			CompareManifests(TEXT("round trip"), First.Manifest, Second.Manifest);

			// A second pass must be a fixed point, otherwise the format drifts every time a tool
			// rewrites a manifest.
			FString Reserialized;
			TestTrue(TEXT("re-serialises"), FModManifestParser::SerializeToString(Second.Manifest, Reserialized));
			ExpectString(TEXT("serialisation is a fixed point"), Reserialized, Serialized);
		});

		// Documented guarantee: two runs over the same manifest produce byte-identical output. That
		// relies on the metadata keys and the unknown-field warnings being sorted; if either sort is
		// removed this test starts flapping rather than failing outright, so keep it.
		It("serialises deterministically with sorted metadata", [this]()
		{
			FModManifest Manifest = MakeValidManifest();
			Manifest.Metadata.Add(TEXT("zeta"), TEXT("3"));
			Manifest.Metadata.Add(TEXT("alpha"), TEXT("1"));
			Manifest.Metadata.Add(TEXT("middle"), TEXT("2"));

			FString First;
			FString Second;
			TestTrue(TEXT("serialises once"), FModManifestParser::SerializeToString(Manifest, First, /*bPrettyPrint=*/false));
			TestTrue(TEXT("serialises twice"), FModManifestParser::SerializeToString(Manifest, Second, /*bPrettyPrint=*/false));
			ExpectString(TEXT("both runs agree"), Second, First);

			const int32 AlphaIndex = First.Find(TEXT("\"alpha\""), ESearchCase::CaseSensitive);
			const int32 MiddleIndex = First.Find(TEXT("\"middle\""), ESearchCase::CaseSensitive);
			const int32 ZetaIndex = First.Find(TEXT("\"zeta\""), ESearchCase::CaseSensitive);

			TestTrue(TEXT("every metadata key was written"),
				AlphaIndex != INDEX_NONE && MiddleIndex != INDEX_NONE && ZetaIndex != INDEX_NONE);
			TestTrue(TEXT("metadata keys are written in sorted order"),
				AlphaIndex < MiddleIndex && MiddleIndex < ZetaIndex);
		});

		It("reports malformed JSON", [this]()
		{
			const FModManifestParseResult Result = FModManifestParser::ParseFromString(TEXT("{ this is not json"));
			ExpectParseFailure(TEXT("malformed JSON"), Result, TEXT("Manifest.InvalidJson"), TEXT(""));

			// The parser must not have guessed at any content.
			TestFalse(TEXT("no id was invented"), Result.Manifest.Id.IsValid());
			TestEqual(TEXT("exactly one diagnostic"), Result.Diagnostics.Num(), 1);
		});

		It("reports an empty document", [this]()
		{
			ExpectParseFailure(TEXT("an empty string"),
				FModManifestParser::ParseFromString(TEXT("")), TEXT("Manifest.InvalidJson"), TEXT(""));
			ExpectParseFailure(TEXT("whitespace only"),
				FModManifestParser::ParseFromString(TEXT("  \r\n\t ")), TEXT("Manifest.InvalidJson"), TEXT(""));
		});

		It("reports a JSON document that is not an object", [this]()
		{
			ExpectParseFailure(TEXT("an array"),
				FModManifestParser::ParseFromString(TEXT("[1, 2, 3]")), TEXT("Manifest.InvalidJson"), TEXT(""));
			ExpectParseFailure(TEXT("a bare string"),
				FModManifestParser::ParseFromString(TEXT("\"hello\"")), TEXT("Manifest.InvalidJson"), TEXT(""));
		});

		It("treats an explicit null exactly like an absent key", [this]()
		{
			const FModManifestParseResult Result = FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "Example Mod",
	"description": null,
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json"));

			ExpectNoDiagnostics(TEXT("a null optional field"), Result);
			TestTrue(TEXT("description is empty"), Result.Manifest.Description.IsEmpty());

			// A null on a required field is still a missing required field, not a type error.
			const FModManifestParseResult Missing = FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": null,
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json"));

			ExpectParseFailure(TEXT("a null name"), Missing, TEXT("Manifest.MissingField"), TEXT("name"));
			TestEqual(TEXT("a null is not also reported as a type error"),
				CountDiagnostics(Missing.Diagnostics, TEXT("Manifest.InvalidField")), 0);
		});

		It("warns about unknown fields in a stable sorted order", [this]()
		{
			const FModManifestParseResult Result = FModManifestParser::ParseFromString(
				MakeManifestJson(TEXT(",\n\t\"zebra\": 1,\n\t\"alpha\": 2")));

			TestTrue(TEXT("unknown fields are only a warning, so the manifest still loads"), Result.bSuccess);

			if (TestEqual(TEXT("exactly two warnings"), Result.Diagnostics.Num(), 2))
			{
				ExpectString(TEXT("first warning"), Result.Diagnostics[0].Context, TEXT("alpha"));
				ExpectString(TEXT("second warning"), Result.Diagnostics[1].Context, TEXT("zebra"));
				ExpectString(TEXT("first warning code"), Result.Diagnostics[0].Code.ToString(), TEXT("Manifest.UnknownField"));
				ExpectString(TEXT("severity"), ModFrameworkEnums::ToString(Result.Diagnostics[0].Severity), TEXT("Warning"));
			}
		});

		It("warns about unknown fields inside nested objects", [this]()
		{
			const FModManifestParseResult Result = FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "Example Mod",
	"version": "1.0.0",
	"game": { "id": "com.example.game", "bogus": 1 },
	"dependencies": [ { "id": "com.example.other", "version": "*", "surprise": true } ],
	"entryPoint": { "class": "/Mod/BP_Entry.BP_Entry_C", "extra": 1 }
})json"));

			TestTrue(TEXT("still succeeds"), Result.bSuccess);
			ExpectDiagnostic(TEXT("game"), Result.Diagnostics, EModDiagnosticSeverity::Warning, TEXT("Manifest.UnknownField"), TEXT("game.bogus"));
			ExpectDiagnostic(TEXT("dependency"), Result.Diagnostics, EModDiagnosticSeverity::Warning, TEXT("Manifest.UnknownField"), TEXT("dependencies[0].surprise"));
			ExpectDiagnostic(TEXT("entryPoint"), Result.Diagnostics, EModDiagnosticSeverity::Warning, TEXT("Manifest.UnknownField"), TEXT("entryPoint.extra"));
		});

		It("reports fields of the wrong JSON type", [this]()
		{
			ExpectParseFailure(TEXT("a string priority"),
				FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"priority\": \"high\""))),
				TEXT("Manifest.InvalidField"), TEXT("priority"));

			ExpectParseFailure(TEXT("a fractional priority"),
				FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"priority\": 1.5"))),
				TEXT("Manifest.InvalidField"), TEXT("priority"));

			ExpectParseFailure(TEXT("a numeric name"),
				FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"description\": 7"))),
				TEXT("Manifest.InvalidField"), TEXT("description"));

			ExpectParseFailure(TEXT("a string dependencies array"),
				FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"dependencies\": \"nope\""))),
				TEXT("Manifest.InvalidField"), TEXT("dependencies"));

			ExpectParseFailure(TEXT("a non-object dependency"),
				FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"dependencies\": [ \"nope\" ]"))),
				TEXT("Manifest.InvalidField"), TEXT("dependencies[0]"));

			ExpectParseFailure(TEXT("a non-string metadata value"),
				FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"metadata\": { \"k\": 5 }"))),
				TEXT("Manifest.InvalidField"), TEXT("metadata.k"));

			ExpectParseFailure(TEXT("a non-object game"),
				FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "Example Mod",
	"version": "1.0.0",
	"game": "com.example.game"
})json")), TEXT("Manifest.InvalidField"), TEXT("game"));
		});

		It("lowercases mod ids everywhere they appear", [this]()
		{
			const FModManifestParseResult Result = FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "COM.Example.Mod",
	"name": "Example Mod",
	"version": "1.0.0",
	"game": { "id": "com.example.game" },
	"dependencies": [ { "id": "COM.Example.Other", "version": "*" } ],
	"loadAfter": ["COM.Example.Early"]
})json"));

			ExpectNoDiagnostics(TEXT("a manifest written in mixed case"), Result);
			ExpectString(TEXT("id"), Result.Manifest.Id.ToString(), TEXT("com.example.mod"));

			if (TestEqual(TEXT("dependency count"), Result.Manifest.Dependencies.Num(), 1))
			{
				ExpectString(TEXT("dependency id"), Result.Manifest.Dependencies[0].Id.ToString(), TEXT("com.example.other"));
			}
			if (TestEqual(TEXT("loadAfter count"), Result.Manifest.LoadAfter.Num(), 1))
			{
				ExpectString(TEXT("loadAfter id"), Result.Manifest.LoadAfter[0].ToString(), TEXT("com.example.early"));
			}
		});

		It("stamps the mod id onto every diagnostic it raises", [this]()
		{
			const FModManifestParseResult Result = FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "",
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json"));

			if (TestEqual(TEXT("one diagnostic"), Result.Diagnostics.Num(), 1))
			{
				ExpectString(TEXT("code"), Result.Diagnostics[0].Code.ToString(), TEXT("Manifest.MissingField"));
				ExpectString(TEXT("stamped mod id"), Result.Diagnostics[0].ModId.ToString(), TEXT("com.example.mod"));
			}
		});

		It("prefixes diagnostics with the caller supplied context path", [this]()
		{
			const FModManifestParseResult Result = FModManifestParser::ParseFromString(
				MakeManifestJson(TEXT(",\n\t\"name\": \"\"")), TEXT("C:/Mods/Example/mod.json"));

			// MakeManifestJson already wrote "name", so the duplicate key wins or loses depending on
			// the JSON reader; either way the context prefix is what is under test here.
			if (Result.Diagnostics.Num() > 0)
			{
				TestTrue(TEXT("the context names the file"),
					Result.Diagnostics[0].Context.StartsWith(TEXT("C:/Mods/Example/mod.json"), ESearchCase::CaseSensitive));
			}

			const FModManifestParseResult Bad = FModManifestParser::ParseFromString(TEXT("{ broken"), TEXT("C:/Mods/Example/mod.json"));
			if (TestEqual(TEXT("one diagnostic"), Bad.Diagnostics.Num(), 1))
			{
				ExpectString(TEXT("context is exactly the path when there is no field"),
					Bad.Diagnostics[0].Context, TEXT("C:/Mods/Example/mod.json"));
			}

			const FModManifestParseResult Field = FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json"), TEXT("C:/Mods/Example/mod.json"));

			ExpectDiagnostic(TEXT("the field context is joined with ' : '"), Field.Diagnostics,
				EModDiagnosticSeverity::Error, TEXT("Manifest.MissingField"), TEXT("C:/Mods/Example/mod.json : name"));
		});
	});

	Describe("Validation", [this]()
	{
		Describe("RequiredFields", [this]()
		{
			It("reports a missing id", [this]()
			{
				ExpectParseFailure(TEXT("no id"), FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"name": "Example Mod",
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json")), TEXT("Manifest.MissingField"), TEXT("id"));
			});

			It("reports a missing name", [this]()
			{
				ExpectParseFailure(TEXT("no name"), FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json")), TEXT("Manifest.MissingField"), TEXT("name"));

				// Whitespace is not a name.
				ExpectParseFailure(TEXT("a blank name"),
					FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "   ",
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json")), TEXT("Manifest.MissingField"), TEXT("name"));
			});

			// A missing version lands on Manifest.InvalidVersion rather than MissingField, because
			// 0.0.0 and "absent" are the same state once the struct is built.
			It("reports a missing version", [this]()
			{
				ExpectParseFailure(TEXT("no version"), FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "Example Mod",
	"game": { "id": "com.example.game" }
})json")), TEXT("Manifest.InvalidVersion"), TEXT("version"));

				ExpectParseFailure(TEXT("an explicit 0.0.0"),
					FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "Example Mod",
	"version": "0.0.0",
	"game": { "id": "com.example.game" }
})json")), TEXT("Manifest.InvalidVersion"), TEXT("version"));
			});

			It("reports a missing game id", [this]()
			{
				ExpectParseFailure(TEXT("no game object"), FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "Example Mod",
	"version": "1.0.0"
})json")), TEXT("Manifest.MissingField"), TEXT("game.id"));

				ExpectParseFailure(TEXT("an empty game object"), FModManifestParser::ParseFromString(TEXT(R"json({
	"manifestVersion": 1,
	"id": "com.example.mod",
	"name": "Example Mod",
	"version": "1.0.0",
	"game": { }
})json")), TEXT("Manifest.MissingField"), TEXT("game.id"));
			});

			It("parses the mod version strictly but version constraints loosely", [this]()
			{
				// "2.1" is a valid *constraint* but not a valid published version number.
				ExpectParseFailure(TEXT("a partial mod version"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"description\": \"x\"")).Replace(TEXT("\"1.0.0\""), TEXT("\"2.1\""))),
					TEXT("Manifest.InvalidVersion"), TEXT("version"));

				// ...while the constraint fields still accept it.
				const FModManifestParseResult Loose = FModManifestParser::ParseFromString(
					MakeManifestJson(TEXT(",\n\t\"framework\": { \"version\": \"^0.1\" }")));
				ExpectNoDiagnostics(TEXT("a partial framework constraint"), Loose);
				ExpectString(TEXT("the constraint text is kept"), Loose.Manifest.FrameworkVersionRange.Expression, TEXT("^0.1"));
			});

			It("reports malformed version constraints", [this]()
			{
				ExpectParseFailure(TEXT("game.version"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"game\": { \"id\": \"g\", \"version\": \"not a range\" }"))
						.Replace(TEXT("\"game\": { \"id\": \"com.example.game\" },"), TEXT(""))),
					TEXT("Manifest.InvalidVersionRange"), TEXT("game.version"));

				ExpectParseFailure(TEXT("framework.version"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"framework\": { \"version\": \"!=1.2\" }"))),
					TEXT("Manifest.InvalidVersionRange"), TEXT("framework.version"));

				ExpectParseFailure(TEXT("sdk.version"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"sdk\": { \"id\": \"s\", \"version\": \">*\" }"))),
					TEXT("Manifest.InvalidVersionRange"), TEXT("sdk.version"));

				ExpectParseFailure(TEXT("dependencies[0].version"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"dependencies\": [ { \"id\": \"com.example.other\", \"version\": \"1.x.3\" } ]"))),
					TEXT("Manifest.InvalidVersionRange"), TEXT("dependencies[0].version"));
			});

			It("rejects a manifest version newer than this framework understands", [this]()
			{
				const FString TooNew = FString::Printf(TEXT(R"json({
	"manifestVersion": %d,
	"id": "com.example.mod",
	"name": "Example Mod",
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json"), MODFRAMEWORK_MANIFEST_VERSION + 1);

				ExpectParseFailure(TEXT("a future manifest version"), FModManifestParser::ParseFromString(TooNew),
					TEXT("Manifest.UnsupportedManifestVersion"), TEXT("manifestVersion"));

				// The current version is of course fine.
				const FString Current = FString::Printf(TEXT(R"json({
	"manifestVersion": %d,
	"id": "com.example.mod",
	"name": "Example Mod",
	"version": "1.0.0",
	"game": { "id": "com.example.game" }
})json"), MODFRAMEWORK_MANIFEST_VERSION);
				ExpectNoDiagnostics(TEXT("the current manifest version"), FModManifestParser::ParseFromString(Current));

				// A zero or negative schema version is a different, plainer error.
				ExpectParseFailure(TEXT("manifestVersion 0"),
					FModManifestParser::ParseFromString(MakeManifestJson().Replace(TEXT("\"manifestVersion\": 1"), TEXT("\"manifestVersion\": 0"))),
					TEXT("Manifest.InvalidField"), TEXT("manifestVersion"));
			});

			It("bounds the load order priority", [this]()
			{
				ExpectParseFailure(TEXT("too high"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"priority\": 1001"))),
					TEXT("Manifest.InvalidField"), TEXT("priority"));

				ExpectParseFailure(TEXT("too low"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"priority\": -1001"))),
					TEXT("Manifest.InvalidField"), TEXT("priority"));

				ExpectNoDiagnostics(TEXT("at the upper bound"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"priority\": 1000"))));
				ExpectNoDiagnostics(TEXT("at the lower bound"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"priority\": -1000"))));
			});
		});

		Describe("ModIds", [this]()
		{
			It("rejects an id that is too short", [this]()
			{
				ExpectParseFailure(TEXT("two characters"),
					FModManifestParser::ParseFromString(MakeManifestJson().Replace(TEXT("com.example.mod"), TEXT("ab"))),
					TEXT("Manifest.InvalidId"), TEXT("id"));

				// Three characters is the documented minimum and must still be accepted.
				ExpectNoDiagnostics(TEXT("three characters"),
					FModManifestParser::ParseFromString(MakeManifestJson().Replace(TEXT("\"id\": \"com.example.mod\""), TEXT("\"id\": \"abc\""))));
			});

			It("rejects an id that is too long", [this]()
			{
				const FString LongId = FString::ChrN(200, TEXT('a'));
				const FModManifestParseResult Result = FModManifestParser::ParseFromString(
					MakeManifestJson().Replace(TEXT("\"id\": \"com.example.mod\""), *FString::Printf(TEXT("\"id\": \"%s\""), *LongId)));

				// The length guard fires before any FName is built, so the id never reaches the
				// struct and validation then also reports it as missing.
				ExpectParseFailure(TEXT("a 200 character id"), Result, TEXT("Manifest.InvalidId"), TEXT("id"));
				ExpectDiagnostic(TEXT("and is then missing"), Result.Diagnostics, EModDiagnosticSeverity::Error, TEXT("Manifest.MissingField"), TEXT("id"));
			});

			It("rejects illegal characters", [this]()
			{
				const TCHAR* const BadIds[] =
				{
					TEXT("com.example.mod!"),
					TEXT("com example.mod"),
					TEXT("com/example/mod"),
					TEXT("com:example"),
					TEXT("com.example.m d"),
					TEXT(" com.example.mod")
				};

				for (const TCHAR* BadId : BadIds)
				{
					ExpectParseFailure(FString::Printf(TEXT("id '%s'"), BadId),
						FModManifestParser::ParseFromString(
							MakeManifestJson().Replace(TEXT("\"id\": \"com.example.mod\""), *FString::Printf(TEXT("\"id\": \"%s\""), BadId))),
						TEXT("Manifest.InvalidId"), TEXT("id"));
				}
			});

			It("rejects '..' and leading or trailing separators", [this]()
			{
				const TCHAR* const BadIds[] =
				{
					TEXT("com..example"),
					TEXT("..example"),
					TEXT(".com.example"),
					TEXT("-com.example"),
					TEXT("_com.example"),
					TEXT("com.example."),
					TEXT("com.example-"),
					TEXT("com.example_")
				};

				for (const TCHAR* BadId : BadIds)
				{
					ExpectParseFailure(FString::Printf(TEXT("id '%s'"), BadId),
						FModManifestParser::ParseFromString(
							MakeManifestJson().Replace(TEXT("\"id\": \"com.example.mod\""), *FString::Printf(TEXT("\"id\": \"%s\""), BadId))),
						TEXT("Manifest.InvalidId"), TEXT("id"));
				}
			});

			It("accepts the documented id alphabet", [this]()
			{
				const TCHAR* const GoodIds[] =
				{
					TEXT("abc"),
					TEXT("com.example.mod"),
					TEXT("com_example-mod.2"),
					TEXT("0mod"),
					TEXT("mod-1_2.3")
				};

				for (const TCHAR* GoodId : GoodIds)
				{
					ExpectNoDiagnostics(FString::Printf(TEXT("id '%s'"), GoodId),
						FModManifestParser::ParseFromString(
							MakeManifestJson().Replace(TEXT("\"id\": \"com.example.mod\""), *FString::Printf(TEXT("\"id\": \"%s\""), GoodId))));
				}
			});

			It("validates the ids inside loadBefore and loadAfter", [this]()
			{
				ExpectParseFailure(TEXT("a malformed loadBefore id"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"loadBefore\": [\"BAD ID\"]"))),
					TEXT("Manifest.InvalidId"), TEXT("loadBefore[0]"));

				ExpectParseFailure(TEXT("an empty loadAfter id"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"loadAfter\": [\"\"]"))),
					TEXT("Manifest.InvalidId"), TEXT("loadAfter[0]"));
			});
		});

		Describe("Dependencies", [this]()
		{
			It("reports a dependency with no id", [this]()
			{
				ExpectParseFailure(TEXT("no id key"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"dependencies\": [ { \"version\": \"*\" } ]"))),
					TEXT("Manifest.MissingField"), TEXT("dependencies[0].id"));
			});

			It("reports a malformed dependency id", [this]()
			{
				ExpectParseFailure(TEXT("too short"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"dependencies\": [ { \"id\": \"ab\", \"version\": \"*\" } ]"))),
					TEXT("Manifest.InvalidId"), TEXT("dependencies[0].id"));
			});

			It("rejects a self dependency", [this]()
			{
				const FModManifestParseResult Result = FModManifestParser::ParseFromString(
					MakeManifestJson(TEXT(",\n\t\"dependencies\": [ { \"id\": \"com.example.mod\", \"version\": \"*\" } ]")));

				ExpectParseFailure(TEXT("depending on itself"), Result, TEXT("Manifest.SelfDependency"), TEXT("dependencies[0].id"));
			});

			It("rejects ordering a mod against itself", [this]()
			{
				ExpectParseFailure(TEXT("loadBefore self"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"loadBefore\": [\"com.example.mod\"]"))),
					TEXT("Manifest.SelfDependency"), TEXT("loadBefore[0]"));

				ExpectParseFailure(TEXT("loadAfter self"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"loadAfter\": [\"com.example.mod\"]"))),
					TEXT("Manifest.SelfDependency"), TEXT("loadAfter[0]"));
			});

			It("rejects a duplicate dependency and names the second occurrence", [this]()
			{
				const FModManifestParseResult Result = FModManifestParser::ParseFromString(
					MakeManifestJson(TEXT(",\n\t\"dependencies\": [ { \"id\": \"com.example.other\", \"version\": \"^1.0.0\" }, { \"id\": \"com.example.other\", \"version\": \"^2.0.0\" } ]")));

				ExpectParseFailure(TEXT("a duplicate dependency"), Result, TEXT("Manifest.DuplicateDependency"), TEXT("dependencies[1].id"));

				// Exactly one duplicate report: the first occurrence is legitimate.
				TestEqual(TEXT("only the second occurrence is reported"),
					CountDiagnostics(Result.Diagnostics, TEXT("Manifest.DuplicateDependency")), 1);
			});

			It("keeps the declared order and the optional flag", [this]()
			{
				const FModManifestParseResult Result = FModManifestParser::ParseFromString(
					MakeManifestJson(TEXT(",\n\t\"dependencies\": [ { \"id\": \"com.example.b\", \"version\": \"^1.0.0\" }, { \"id\": \"com.example.a\", \"version\": \"*\", \"optional\": true } ]")));

				if (!ExpectNoDiagnostics(TEXT("two dependencies"), Result))
				{
					return;
				}

				if (TestEqual(TEXT("dependency count"), Result.Manifest.Dependencies.Num(), 2))
				{
					// Declaration order is preserved; the parser must never sort dependencies, because
					// the resolver derives tie-break ordering from it.
					ExpectString(TEXT("dependencies[0]"), Result.Manifest.Dependencies[0].Id.ToString(), TEXT("com.example.b"));
					ExpectString(TEXT("dependencies[1]"), Result.Manifest.Dependencies[1].Id.ToString(), TEXT("com.example.a"));
					TestFalse(TEXT("dependencies[0] is required"), Result.Manifest.Dependencies[0].bOptional);
					TestTrue(TEXT("dependencies[1] is optional"), Result.Manifest.Dependencies[1].bOptional);
				}
			});
		});

		// SECURITY. A content root names a file the framework will open on the player's machine, so
		// both of these have to stay rejected: an absolute path escapes the mod directory outright,
		// and a ".." segment walks out of it. Backslashes are folded to '/' first so a manifest
		// authored on Windows cannot smuggle either past the check.
		Describe("ContentRoots", [this]()
		{
			It("rejects an absolute content path", [this]()
			{
				ExpectParseFailure(TEXT("a rooted path"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"/etc/passwd\" } ]"))),
					TEXT("Manifest.InvalidContentRoot"), TEXT("content[0].path"));

				ExpectParseFailure(TEXT("a drive letter"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"C:/Windows/System32\" } ]"))),
					TEXT("Manifest.InvalidContentRoot"), TEXT("content[0].path"));

				ExpectParseFailure(TEXT("a home directory path"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"~/secrets\" } ]"))),
					TEXT("Manifest.InvalidContentRoot"), TEXT("content[0].path"));
			});

			It("rejects a content path that escapes the mod directory with ..", [this]()
			{
				ExpectParseFailure(TEXT("a leading .."),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"../../Other/secret.pak\" } ]"))),
					TEXT("Manifest.InvalidContentRoot"), TEXT("content[0].path"));

				ExpectParseFailure(TEXT("a buried .."),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"Content/../../Other/secret.pak\" } ]"))),
					TEXT("Manifest.InvalidContentRoot"), TEXT("content[0].path"));

				// Backslashes must be normalised before the ".." scan, or this one slips through.
				ExpectParseFailure(TEXT("a backslash spelled .."),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"..\\\\..\\\\Other\" } ]"))),
					TEXT("Manifest.InvalidContentRoot"), TEXT("content[0].path"));
			});

			It("accepts a contained relative path, including one that merely starts with two dots", [this]()
			{
				ExpectNoDiagnostics(TEXT("a normal path"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"Content/Mod.pak\" } ]"))));

				// "..data" is a file name, not a parent directory reference; only a whole ".."
				// segment escapes.
				ExpectNoDiagnostics(TEXT("a file name starting with dots"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"Content/..data\" } ]"))));
			});

			It("reports an empty content path", [this]()
			{
				ExpectParseFailure(TEXT("an empty path"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"\" } ]"))),
					TEXT("Manifest.EmptyContent"), TEXT("content[0].path"));

				ExpectParseFailure(TEXT("a missing path key"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"mountPoint\": \"/Mod/\" } ]"))),
					TEXT("Manifest.MissingField"), TEXT("content[0].path"));
			});

			It("rejects malformed mount points", [this]()
			{
				const TCHAR* const BadMountPoints[] =
				{
					TEXT("Mod"),
					TEXT("/Mod"),
					TEXT("Mod/"),
					TEXT("//"),
					TEXT("/ /"),
					TEXT("/My Mod/"),
					TEXT("/1Mod/"),
					TEXT("/Mod-1/"),
					TEXT("/Mod/Sub/")
				};

				for (const TCHAR* MountPoint : BadMountPoints)
				{
					ExpectParseFailure(FString::Printf(TEXT("mount point '%s'"), MountPoint),
						FModManifestParser::ParseFromString(MakeManifestJson(
							*FString::Printf(TEXT(",\n\t\"content\": [ { \"path\": \"Content/Mod.pak\", \"mountPoint\": \"%s\" } ]"), MountPoint))),
						TEXT("Manifest.InvalidContentRoot"), TEXT("content[0].mountPoint"));
				}
			});

			It("accepts a well formed mount point", [this]()
			{
				const TCHAR* const GoodMountPoints[] = { TEXT("/Mod/"), TEXT("/_Mod/"), TEXT("/Mod2/"), TEXT("/M/") };

				for (const TCHAR* MountPoint : GoodMountPoints)
				{
					ExpectNoDiagnostics(FString::Printf(TEXT("mount point '%s'"), MountPoint),
						FModManifestParser::ParseFromString(MakeManifestJson(
							*FString::Printf(TEXT(",\n\t\"content\": [ { \"path\": \"Content/Mod.pak\", \"mountPoint\": \"%s\" } ]"), MountPoint))));
				}

				// An omitted mount point is fine: the framework derives one from the mod folder.
				ExpectNoDiagnostics(TEXT("no mount point"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"Content/Mod.pak\" } ]"))));
			});

			It("reports an unknown content root type", [this]()
			{
				ExpectParseFailure(TEXT("a bogus type"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"content\": [ { \"path\": \"Content/Mod.pak\", \"type\": \"Zip\" } ]"))),
					TEXT("Manifest.InvalidField"), TEXT("content[0].type"));
			});

			It("parses the documented content root type aliases case insensitively", [this]()
			{
				const TCHAR* const Spellings[] = { TEXT("pak"), TEXT("PAK"), TEXT("IoStore"), TEXT("iostore"), TEXT("Loose"), TEXT("Directory"), TEXT("LOOSEDIRECTORY") };
				const TCHAR* const Expected[] = { TEXT("Pak"), TEXT("Pak"), TEXT("IoStore"), TEXT("IoStore"), TEXT("LooseDirectory"), TEXT("LooseDirectory"), TEXT("LooseDirectory") };

				for (int32 Index = 0; Index < UE_ARRAY_COUNT(Spellings); ++Index)
				{
					const FModManifestParseResult Result = FModManifestParser::ParseFromString(MakeManifestJson(
						*FString::Printf(TEXT(",\n\t\"content\": [ { \"path\": \"Content/Mod.pak\", \"type\": \"%s\" } ]"), Spellings[Index])));

					if (ExpectNoDiagnostics(FString::Printf(TEXT("type '%s'"), Spellings[Index]), Result)
						&& TestEqual(TEXT("content root count"), Result.Manifest.ContentRoots.Num(), 1))
					{
						ExpectString(FString::Printf(TEXT("type '%s'"), Spellings[Index]),
							ModFrameworkEnums::ToString(Result.Manifest.ContentRoots[0].Type), Expected[Index]);
					}
				}
			});
		});

		// SECURITY. The icon is loaded before the mod is mounted, straight off disk, so it gets the
		// same containment rules as a content root plus a format allow-list.
		Describe("Icon", [this]()
		{
			It("rejects an icon that escapes the mod directory", [this]()
			{
				const TCHAR* const BadIcons[] =
				{
					TEXT("../Icon.png"),
					TEXT("Art/../../Icon.png"),
					TEXT("/abs/Icon.png"),
					TEXT("C:/Icon.png"),
					TEXT("~/Icon.png")
				};

				for (const TCHAR* Icon : BadIcons)
				{
					ExpectParseFailure(FString::Printf(TEXT("icon '%s'"), Icon),
						FModManifestParser::ParseFromString(MakeManifestJson(*FString::Printf(TEXT(",\n\t\"icon\": \"%s\""), Icon))),
						TEXT("Manifest.InvalidIcon"), TEXT("icon"));
				}
			});

			// The icon path doubles as a lookup key inside a .mod package, so the two spellings must
			// not be allowed to diverge: a backslash is refused rather than rewritten.
			It("rejects a backslash spelled icon path", [this]()
			{
				ExpectParseFailure(TEXT("a backslash"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"icon\": \"Art\\\\Icon.png\""))),
					TEXT("Manifest.InvalidIcon"), TEXT("icon"));
			});

			It("rejects an unsupported image format", [this]()
			{
				const TCHAR* const BadIcons[] = { TEXT("Icon.bmp"), TEXT("Icon.tga"), TEXT("Icon.uasset"), TEXT("Icon"), TEXT("Art/Icon") };

				for (const TCHAR* Icon : BadIcons)
				{
					ExpectParseFailure(FString::Printf(TEXT("icon '%s'"), Icon),
						FModManifestParser::ParseFromString(MakeManifestJson(*FString::Printf(TEXT(",\n\t\"icon\": \"%s\""), Icon))),
						TEXT("Manifest.InvalidIcon"), TEXT("icon"));
				}
			});

			It("rejects a blank icon field", [this]()
			{
				ExpectParseFailure(TEXT("whitespace"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"icon\": \"   \""))),
					TEXT("Manifest.InvalidIcon"), TEXT("icon"));
			});

			It("accepts png, jpg and jpeg in any case", [this]()
			{
				const TCHAR* const GoodIcons[] = { TEXT("Icon.png"), TEXT("Art/Icon.jpg"), TEXT("Art/Sub/Icon.jpeg"), TEXT("Art/Icon.PNG") };

				for (const TCHAR* Icon : GoodIcons)
				{
					ExpectNoDiagnostics(FString::Printf(TEXT("icon '%s'"), Icon),
						FModManifestParser::ParseFromString(MakeManifestJson(*FString::Printf(TEXT(",\n\t\"icon\": \"%s\""), Icon))));
				}

				// No icon at all is the normal case and must stay silent.
				ExpectNoDiagnostics(TEXT("no icon"), FModManifestParser::ParseFromString(MakeManifestJson()));
			});
		});

		Describe("EntryPoint", [this]()
		{
			It("rejects a malformed native module name", [this]()
			{
				ExpectParseFailure(TEXT("a module name with a space"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"entryPoint\": { \"nativeModule\": \"My Module\" }"))),
					TEXT("Manifest.InvalidEntryPoint"), TEXT("entryPoint.nativeModule"));

				ExpectParseFailure(TEXT("a module name starting with a digit"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"entryPoint\": { \"nativeModule\": \"1Module\" }"))),
					TEXT("Manifest.InvalidEntryPoint"), TEXT("entryPoint.nativeModule"));

				ExpectNoDiagnostics(TEXT("a valid module name"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"entryPoint\": { \"nativeModule\": \"_My2Module\" }"))));
			});

			It("rejects a malformed entry class path", [this]()
			{
				ExpectParseFailure(TEXT("not a path at all"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"entryPoint\": { \"class\": \"not a path\" }"))),
					TEXT("Manifest.InvalidEntryPoint"), TEXT("entryPoint.class"));

				ExpectNoDiagnostics(TEXT("a well formed class path"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"entryPoint\": { \"class\": \"/Mod/BP_Entry.BP_Entry_C\" }"))));
			});

			It("rejects an empty or malformed content bundle path", [this]()
			{
				ExpectParseFailure(TEXT("an empty bundle"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"entryPoint\": { \"contentBundles\": [\"\"] }"))),
					TEXT("Manifest.InvalidEntryPoint"), TEXT("entryPoint.contentBundles[0]"));

				ExpectParseFailure(TEXT("a malformed bundle"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"entryPoint\": { \"contentBundles\": [\"nope nope\"] }"))),
					TEXT("Manifest.InvalidEntryPoint"), TEXT("entryPoint.contentBundles[0]"));
			});
		});

		Describe("PermissionsAndClaims", [this]()
		{
			It("rejects a permission containing whitespace", [this]()
			{
				ExpectParseFailure(TEXT("a spaced permission"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"permissions\": [\"gameplay modify\"]"))),
					TEXT("Manifest.InvalidPermission"), TEXT("permissions[0]"));

				ExpectParseFailure(TEXT("an empty permission"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"permissions\": [\"\"]"))),
					TEXT("Manifest.InvalidPermission"), TEXT("permissions[0]"));

				ExpectNoDiagnostics(TEXT("a dotted permission"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"permissions\": [\"gameplay.modify\", \"save.modify\"]"))));
			});

			It("rejects an incomplete claim", [this]()
			{
				ExpectParseFailure(TEXT("no extension point"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"claims\": [ { \"resource\": \"weapon.longsword\" } ]"))),
					TEXT("Manifest.MissingField"), TEXT("claims[0].extensionPoint"));

				ExpectParseFailure(TEXT("no resource"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"claims\": [ { \"extensionPoint\": \"game.weapon\" } ]"))),
					TEXT("Manifest.MissingField"), TEXT("claims[0].resource"));

				ExpectParseFailure(TEXT("an empty extension point"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"claims\": [ { \"extensionPoint\": \"\", \"resource\": \"weapon.longsword\" } ]"))),
					TEXT("Manifest.InvalidClaim"), TEXT("claims[0].extensionPoint"));

				ExpectParseFailure(TEXT("a spaced resource"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"claims\": [ { \"extensionPoint\": \"game.weapon\", \"resource\": \"weapon long sword\" } ]"))),
					TEXT("Manifest.InvalidClaim"), TEXT("claims[0].resource"));
			});

			It("reports an unknown conflict policy", [this]()
			{
				ExpectParseFailure(TEXT("a bogus policy"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"claims\": [ { \"extensionPoint\": \"game.weapon\", \"resource\": \"weapon.longsword\", \"policy\": \"Whatever\" } ]"))),
					TEXT("Manifest.InvalidField"), TEXT("claims[0].policy"));
			});

			It("reports an unknown network scope", [this]()
			{
				ExpectParseFailure(TEXT("a bogus scope"),
					FModManifestParser::ParseFromString(MakeManifestJson(TEXT(",\n\t\"network\": { \"scope\": \"Sideways\" }"))),
					TEXT("Manifest.InvalidField"), TEXT("network.scope"));

				// The documented aliases must keep working.
				const FModManifestParseResult Client = FModManifestParser::ParseFromString(
					MakeManifestJson(TEXT(",\n\t\"network\": { \"scope\": \"client\" }")));
				if (ExpectNoDiagnostics(TEXT("the 'client' alias"), Client))
				{
					ExpectString(TEXT("scope"), ModFrameworkEnums::ToString(Client.Manifest.NetworkScope), TEXT("ClientOnly"));
				}
			});
		});

		// ValidateManifest is the path tools use on a manifest that was built in code or read out of
		// a package, so it must work on a struct that never saw JSON, and it must APPEND rather than
		// replace: the caller's own diagnostics are preserved, and only the ones this call adds get
		// the mod id stamped onto them.
		It("appends to the caller's diagnostics without clearing or restamping them", [this]()
		{
			TArray<FModDiagnostic> Diagnostics;
			Diagnostics.Add(FModDiagnostic::Info(FName(TEXT("Caller.Sentinel")), TEXT("kept"), TEXT("caller")));

			FModManifest Manifest = MakeValidManifest();
			Manifest.DisplayName.Reset();

			FModManifestParser::ValidateManifest(Manifest, FString(), Diagnostics);

			if (TestEqual(TEXT("the sentinel plus one new error"), Diagnostics.Num(), 2))
			{
				ExpectString(TEXT("the sentinel is still first"), Diagnostics[0].Code.ToString(), TEXT("Caller.Sentinel"));
				ExpectString(TEXT("the sentinel keeps its context"), Diagnostics[0].Context, TEXT("caller"));
				TestFalse(TEXT("the sentinel is not restamped with the mod id"), Diagnostics[0].ModId.IsValid());

				ExpectString(TEXT("the new diagnostic"), Diagnostics[1].Code.ToString(), TEXT("Manifest.MissingField"));
				ExpectString(TEXT("the new diagnostic is stamped"), Diagnostics[1].ModId.ToString(), TEXT("com.example.mod"));
			}
		});

		It("passes a manifest that was built in code", [this]()
		{
			TArray<FModDiagnostic> Diagnostics;
			FModManifestParser::ValidateManifest(MakeValidManifest(), FString(), Diagnostics);

			if (Diagnostics.Num() != 0)
			{
				AddError(FString::Printf(TEXT("A hand-built valid manifest produced diagnostics:\n%s"), *ModDiagnostics::Join(Diagnostics)));
			}
		});
	});

	Describe("Files", [this]()
	{
		BeforeEach([this]()
		{
			TempDirectory = FPaths::Combine(FPaths::AutomationTransientDir(),
				TEXT("ModFrameworkManifestSpec"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().MakeDirectory(*TempDirectory, /*Tree=*/true);
		});

		AfterEach([this]()
		{
			if (!TempDirectory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*TempDirectory, /*RequireExists=*/false, /*Tree=*/true);
				TempDirectory.Reset();
			}
		});

		It("names the manifest file mod.json", [this]()
		{
			ExpectString(TEXT("manifest file name"), FModManifestParser::GetManifestFileName(), TEXT("mod.json"));
		});

		It("writes a manifest and reads it back unchanged", [this]()
		{
			const FModManifestParseResult Source = FModManifestParser::ParseFromString(GetFullManifestJson());
			if (!ExpectNoDiagnostics(TEXT("the source manifest"), Source))
			{
				return;
			}

			const FString FilePath = FPaths::Combine(TempDirectory, FModManifestParser::GetManifestFileName());

			if (!TestTrue(TEXT("writes to disk"), FModManifestParser::SerializeToFile(Source.Manifest, FilePath)))
			{
				return;
			}
			TestTrue(TEXT("the file exists"), IFileManager::Get().FileExists(*FilePath));

			const FModManifestParseResult Loaded = FModManifestParser::ParseFromFile(FilePath);
			if (!ExpectNoDiagnostics(TEXT("the manifest read back from disk"), Loaded))
			{
				return;
			}

			CompareManifests(TEXT("file round trip"), Source.Manifest, Loaded.Manifest);
		});

		It("creates the containing directory when it has to", [this]()
		{
			const FString FilePath = FPaths::Combine(TempDirectory, TEXT("Nested"), TEXT("Deeper"), FModManifestParser::GetManifestFileName());

			TestTrue(TEXT("writes through missing directories"), FModManifestParser::SerializeToFile(MakeValidManifest(), FilePath));
			TestTrue(TEXT("the file exists"), IFileManager::Get().FileExists(*FilePath));
			TestTrue(TEXT("and parses"), FModManifestParser::ParseFromFile(FilePath).bSuccess);
		});

		It("reports a missing file", [this]()
		{
			const FString FilePath = FPaths::Combine(TempDirectory, TEXT("does-not-exist.json"));
			const FModManifestParseResult Result = FModManifestParser::ParseFromFile(FilePath);

			TestFalse(TEXT("does not succeed"), Result.bSuccess);
			ExpectDiagnostic(TEXT("a missing file"), Result.Diagnostics, EModDiagnosticSeverity::Error, TEXT("Manifest.ReadFailed"), nullptr);
		});

		It("reports an empty path", [this]()
		{
			const FModManifestParseResult Result = FModManifestParser::ParseFromFile(FString());

			TestFalse(TEXT("does not succeed"), Result.bSuccess);
			ExpectDiagnostic(TEXT("an empty path"), Result.Diagnostics, EModDiagnosticSeverity::Error, TEXT("Manifest.ReadFailed"), nullptr);
		});

		It("names the file in every diagnostic it raises", [this]()
		{
			const FString FilePath = FPaths::Combine(TempDirectory, FModManifestParser::GetManifestFileName());
			FModManifest Broken = MakeValidManifest();
			Broken.DisplayName.Reset();

			if (!TestTrue(TEXT("writes to disk"), FModManifestParser::SerializeToFile(Broken, FilePath)))
			{
				return;
			}

			const FModManifestParseResult Result = FModManifestParser::ParseFromFile(FilePath);
			TestFalse(TEXT("does not succeed"), Result.bSuccess);

			for (const FModDiagnostic& Diagnostic : Result.Diagnostics)
			{
				if (!Diagnostic.Context.Contains(FilePath, ESearchCase::CaseSensitive))
				{
					AddError(FString::Printf(TEXT("Diagnostic '%s' does not name the file it came from: %s"),
						*Diagnostic.Code.ToString(), *Diagnostic.Context));
				}
			}
		});
	});

	Describe("ManifestApi", [this]()
	{
		It("IsValid requires an id and a non-zero version", [this]()
		{
			FModManifest Manifest;
			TestFalse(TEXT("a default manifest is not valid"), Manifest.IsValid());

			Manifest.Id = FModId(FName(TEXT("com.example.mod")));
			TestFalse(TEXT("an id alone is not enough"), Manifest.IsValid());

			Manifest.Version = FModVersion(1, 0, 0);
			TestTrue(TEXT("an id and a version are enough"), Manifest.IsValid());

			Manifest.Id.Reset();
			TestFalse(TEXT("a version alone is not enough"), Manifest.IsValid());
		});

		It("GetDisplayNameOrId falls back to the id", [this]()
		{
			FModManifest Manifest = MakeValidManifest();
			ExpectString(TEXT("with a display name"), Manifest.GetDisplayNameOrId(), TEXT("Example Mod"));

			Manifest.DisplayName.Reset();
			ExpectString(TEXT("without a display name"), Manifest.GetDisplayNameOrId(), TEXT("com.example.mod"));
		});

		It("FindDependency finds required and optional dependencies", [this]()
		{
			FModManifest Manifest = MakeValidManifest();

			FModDependency Required;
			Required.Id = FModId(FName(TEXT("com.example.corelib")));
			Required.VersionRange = FModVersionRange::Any();
			Manifest.Dependencies.Add(Required);

			FModDependency Optional;
			Optional.Id = FModId(FName(TEXT("com.example.ui")));
			Optional.VersionRange = FModVersionRange::Any();
			Optional.bOptional = true;
			Manifest.Dependencies.Add(Optional);

			const FModDependency* FoundRequired = Manifest.FindDependency(FModId(FName(TEXT("com.example.corelib"))));
			const FModDependency* FoundOptional = Manifest.FindDependency(FModId(FName(TEXT("com.example.ui"))));
			const FModDependency* Missing = Manifest.FindDependency(FModId(FName(TEXT("com.example.absent"))));

			if (TestTrue(TEXT("the required dependency is found"), FoundRequired != nullptr))
			{
				TestFalse(TEXT("and is required"), FoundRequired->bOptional);
			}
			if (TestTrue(TEXT("the optional dependency is found"), FoundOptional != nullptr))
			{
				TestTrue(TEXT("and is optional"), FoundOptional->bOptional);
			}
			TestTrue(TEXT("an absent dependency returns nullptr"), Missing == nullptr);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
