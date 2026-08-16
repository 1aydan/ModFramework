// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Templates/TypeHash.h"
#include "UObject/ObjectMacros.h"

#include "ModVersion.generated.h"

/**
 * Semantic Versioning 2.0.0 version.
 *
 * Ordering follows the specification exactly:
 *  - major, minor and patch are compared numerically;
 *  - a version WITH a pre-release is lower than the same major.minor.patch WITHOUT one;
 *  - pre-release identifiers are compared left to right, numeric identifiers compare numerically
 *    and always sort below alphanumeric ones, and when every shared identifier is equal the version
 *    with MORE identifiers wins;
 *  - build metadata is parsed and preserved but ignored for every comparison.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModVersion
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	int32 Major = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	int32 Minor = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	int32 Patch = 0;

	/** Dot separated pre-release identifiers without the leading '-'. Empty = release. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString PreRelease;

	/** Build metadata without the leading '+'. Ignored for ordering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString BuildMetadata;

	FModVersion() = default;
	FModVersion(int32 InMajor, int32 InMinor, int32 InPatch, FString InPreRelease = FString(), FString InBuild = FString());

	/**
	 * Parses a semver string.
	 *
	 * Accepted: "1.2.3", "1.2.3-rc.1", "1.2.3+build.7", "1.2.3-rc.1+build.7", and - because a
	 * single leading 'v' is so common in the wild - "v1.2.3".
	 *
	 * Rejected: leading zeroes in a numeric component or numeric pre-release identifier, negative
	 * numbers, empty identifiers, characters outside [0-9A-Za-z-] in a pre-release or build
	 * identifier, components that do not fit in an int32, more than three dot separated core
	 * numbers, and a pre-release or build suffix on a partial core (e.g. "1.2-rc").
	 *
	 * @param bAllowPartial  When true (the default) "1" and "1.2" are accepted and normalised to
	 *                       1.0.0 and 1.2.0. When false only a full "major.minor.patch" core is
	 *                       accepted. Pass false when the caller needs strict semver, for example
	 *                       when validating a published version number.
	 * @return true on success; Out is reset to 0.0.0 and OutError (when supplied) describes the
	 *         problem on failure. Never asserts: every input is untrusted.
	 */
	static bool Parse(const FString& In, FModVersion& Out, FString* OutError = nullptr, bool bAllowPartial = true);

	/** Convenience wrapper around Parse. Returns 0.0.0 on failure. */
	static FModVersion FromString(const FString& In);

	/** Canonical "major.minor.patch[-prerelease][+build]". */
	FString ToString() const;

	/** True for 0.0.0 with no pre-release. Build metadata is ignored. */
	bool IsZero() const;

	bool IsPreRelease() const { return !PreRelease.IsEmpty(); }

	/** Full semver precedence. Returns <0, 0 or >0. BuildMetadata is ignored. */
	int32 Compare(const FModVersion& Other) const;

	bool operator==(const FModVersion& O) const { return Compare(O) == 0; }
	bool operator!=(const FModVersion& O) const { return Compare(O) != 0; }
	bool operator<(const FModVersion& O) const { return Compare(O) < 0; }
	bool operator<=(const FModVersion& O) const { return Compare(O) <= 0; }
	bool operator>(const FModVersion& O) const { return Compare(O) > 0; }
	bool operator>=(const FModVersion& O) const { return Compare(O) >= 0; }

	/** Hashes the precedence-relevant fields only, so equal versions always hash equal. */
	friend uint32 GetTypeHash(const FModVersion& In)
	{
		return HashCombine(
			HashCombine(GetTypeHash(In.Major), GetTypeHash(In.Minor)),
			HashCombine(GetTypeHash(In.Patch), GetTypeHash(In.PreRelease)));
	}
};

/** Relational operator of a single version comparator. */
UENUM()
enum class EModVersionOp : uint8
{
	Equal,
	NotEqual,
	Greater,
	GreaterEqual,
	Less,
	LessEqual
};

/**
 * One `<operator> <version>` term of a version range.
 *
 * Satisfies() applies the operator alone. The npm pre-release gate lives in FModVersionClause,
 * because it is a property of the whole AND-ed set of comparators, not of a single term.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModVersionComparator
{
	GENERATED_BODY()

	UPROPERTY()
	EModVersionOp Op = EModVersionOp::GreaterEqual;

	UPROPERTY()
	FModVersion Version;

	bool Satisfies(const FModVersion& In) const;

	/** Machine-readable form, e.g. ">=1.2.3". */
	FString ToString() const;
};

/**
 * A set of comparators that must ALL hold (the AND level of a range).
 *
 * Pre-release gate (npm semantics): a version that carries a pre-release satisfies this clause only
 * when at least one comparator in the SAME clause names a version with an identical
 * major.minor.patch AND a pre-release of its own. That is what stops 2.0.0-rc1 from matching
 * ">=1.0.0" while still letting 1.2.3-rc.2 match ">=1.2.3-rc.1 <2.0.0".
 *
 * A clause with no comparators matches every version, pre-releases included; that is the parsed
 * form of "*", "x", "any" and the empty expression.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModVersionClause
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FModVersionComparator> Comparators;

	bool Satisfies(const FModVersion& In) const;

	/** Machine-readable form, e.g. ">=1.2.3 <2.0.0". "*" when the clause is empty. */
	FString ToString() const;
};

/**
 * A version constraint. Syntax (npm-like, deliberately a documented subset):
 *   "*" or "" or "any"        -> matches everything
 *   "1.2.3"                   -> exactly 1.2.3
 *   ">=1.2.3", ">1.2", "<2.0.0", "<=2", "!=1.5.0"
 *   "^1.2.3"                  -> >=1.2.3 <2.0.0   (^0.2.3 -> >=0.2.3 <0.3.0; ^0.0.3 -> >=0.0.3 <0.0.4)
 *   "~1.2.3"                  -> >=1.2.3 <1.3.0   (~1.2 -> >=1.2.0 <1.3.0)
 *   "1.2.x" / "1.x" / "1"     -> partial wildcard
 *   "1.2.3 - 2.0.0"           -> inclusive hyphen range
 *   ">=1.2.3 <2.0.0"          -> space/comma separated comparators are ANDed
 *   "^1.0.0 || ^2.0.0"        -> '||' separated clauses are ORed
 * Pre-release versions only satisfy a comparator when that comparator itself names a
 * pre-release with the same major.minor.patch (npm rule) - keeps 2.0.0-rc1 out of ">=1.0.0".
 *
 * Thread-safety / constness: the parsed form is built eagerly by Parse(), Any() and Exactly().
 * A range that was assigned or deserialized without going through those (bParsed == false) is
 * parsed into a LOCAL array by the const query functions - they never mutate `this`, so a shared
 * const FModVersionRange can be queried from several threads at once. That local parse costs an
 * allocation per call, so prefer constructing ranges through Parse().
 *
 * Writing to Expression after a successful Parse() leaves Clauses stale: assign through Parse()
 * instead, or clear bParsed so the query functions recompile the expression.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModVersionRange
{
	GENERATED_BODY()

	/** The original text, preserved for diagnostics and round-tripping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FString Expression;

	/** Parses In, stores it in Out.Expression and eagerly builds Out.Clauses. */
	static bool Parse(const FString& In, FModVersionRange& Out, FString* OutError = nullptr);

	/** A range that matches every version. */
	static FModVersionRange Any();

	/** A range that matches exactly one version. */
	static FModVersionRange Exactly(const FModVersion& In);

	/**
	 * Compiles an expression into its OR-of-AND clause form without touching any FModVersionRange.
	 * Exposed because the const query functions below use it to stay non-mutating.
	 */
	static bool ParseExpression(const FString& InExpression, TArray<FModVersionClause>& OutClauses, FString* OutError);

	/** True when the range accepts every version (including pre-releases). */
	bool IsAny() const;

	/** True when the expression parses. An unparsed range is compiled on the fly to answer this. */
	bool IsValid() const;

	/** False for every version when the expression is malformed. */
	bool Satisfies(const FModVersion& In) const;

	FString ToString() const { return Expression; }

	/**
	 * Human sentence, e.g. ">= 1.2.3 and < 2.0.0" or "1.0.0 or newer, or exactly 2.1.0".
	 * A clause holding a single comparator is spelled out in words; a clause holding several is
	 * rendered with operator symbols joined by " and ". Clauses are joined by ", or ".
	 */
	FString Describe() const;

	// Parsed form; rebuilt lazily from Expression when needed.
	UPROPERTY()
	TArray<FModVersionClause> Clauses;

	UPROPERTY()
	bool bParsed = false;

	UPROPERTY()
	bool bParseFailed = false;
};

/** Ranges compare and hash on their source expression, case sensitively. */
MODFRAMEWORK_API bool operator==(const FModVersionRange& A, const FModVersionRange& B);
MODFRAMEWORK_API bool operator!=(const FModVersionRange& A, const FModVersionRange& B);
MODFRAMEWORK_API uint32 GetTypeHash(const FModVersionRange& In);
