// Copyright (c) 2026. Licensed for use in your own projects.

#include "Manifest/ModVersion.h"

#include "Math/NumericLimits.h"
#include "Misc/CString.h"
#include "Misc/Char.h"
#include "Templates/UnrealTemplate.h"

namespace ModVersionPrivate
{
	static const TCHAR* const CoreComponentNames[3] = { TEXT("major"), TEXT("minor"), TEXT("patch") };

	static void SetError(FString* OutError, FString&& Message)
	{
		if (OutError != nullptr)
		{
			*OutError = MoveTemp(Message);
		}
	}

	static bool IsAsciiDigit(TCHAR In)
	{
		return In >= TEXT('0') && In <= TEXT('9');
	}

	static bool IsAsciiLetter(TCHAR In)
	{
		return (In >= TEXT('a') && In <= TEXT('z')) || (In >= TEXT('A') && In <= TEXT('Z'));
	}

	/** Characters allowed inside a pre-release or build metadata identifier: [0-9A-Za-z-]. */
	static bool IsIdentifierChar(TCHAR In)
	{
		return IsAsciiDigit(In) || IsAsciiLetter(In) || In == TEXT('-');
	}

	static bool IsAllDigits(const FString& In)
	{
		if (In.IsEmpty())
		{
			return false;
		}
		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			if (!IsAsciiDigit(In[Index]))
			{
				return false;
			}
		}
		return true;
	}

	/** Renders one character for a diagnostic. Printable ASCII verbatim, anything else as U+XXXX. */
	static FString DescribeChar(TCHAR In)
	{
		const uint32 Code = static_cast<uint32>(In);
		if (Code >= 0x21 && Code <= 0x7E)
		{
			return FString::Printf(TEXT("'%c'"), In);
		}
		return FString::Printf(TEXT("U+%04X"), Code);
	}

	/** "", "*", "x" and "X" all stand for "any value in this position". */
	static bool IsWildcardComponent(const FString& In)
	{
		return In.IsEmpty()
			|| FCString::Strcmp(*In, TEXT("*")) == 0
			|| FCString::Stricmp(*In, TEXT("x")) == 0;
	}

	static bool SafeIncrement(int32 In, int32& Out)
	{
		if (In == MAX_int32)
		{
			return false;
		}
		Out = In + 1;
		return true;
	}

	/** Parses one dot separated core number: digits only, no leading zero, fits an int32. */
	static bool ParseCoreComponent(const FString& In, int32& Out, FString* OutError, const TCHAR* ComponentName)
	{
		if (In.IsEmpty())
		{
			SetError(OutError, FString::Printf(TEXT("the %s component is empty"), ComponentName));
			return false;
		}

		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			if (!IsAsciiDigit(In[Index]))
			{
				SetError(OutError, FString::Printf(
					TEXT("the %s component '%s' is not a non-negative integer"), ComponentName, *In));
				return false;
			}
		}

		if (In.Len() > 1 && In[0] == TEXT('0'))
		{
			SetError(OutError, FString::Printf(
				TEXT("the %s component '%s' has a leading zero"), ComponentName, *In));
			return false;
		}

		if (In.Len() > 10 || FCString::Atoi64(*In) > static_cast<int64>(MAX_int32))
		{
			SetError(OutError, FString::Printf(
				TEXT("the %s component '%s' is larger than the maximum of %d"), ComponentName, *In, MAX_int32));
			return false;
		}

		Out = static_cast<int32>(FCString::Atoi64(*In));
		return true;
	}

	/**
	 * Validates a dot separated identifier list (a pre-release or build metadata suffix).
	 * @param bRejectNumericLeadingZero true for pre-releases (semver forbids "01"), false for build
	 *        metadata (semver explicitly allows it there).
	 */
	static bool ValidateIdentifierList(const FString& In, bool bRejectNumericLeadingZero, FString* OutError, const TCHAR* PartName)
	{
		if (In.IsEmpty())
		{
			SetError(OutError, FString::Printf(TEXT("the %s is empty"), PartName));
			return false;
		}

		TArray<FString> Identifiers;
		In.ParseIntoArray(Identifiers, TEXT("."), false);

		if (Identifiers.Num() == 0)
		{
			SetError(OutError, FString::Printf(TEXT("the %s is empty"), PartName));
			return false;
		}

		for (const FString& Identifier : Identifiers)
		{
			if (Identifier.IsEmpty())
			{
				SetError(OutError, FString::Printf(
					TEXT("the %s '%s' contains an empty identifier"), PartName, *In));
				return false;
			}

			bool bAllDigits = true;
			for (int32 Index = 0; Index < Identifier.Len(); ++Index)
			{
				const TCHAR Current = Identifier[Index];
				if (!IsIdentifierChar(Current))
				{
					SetError(OutError, FString::Printf(
						TEXT("the %s '%s' contains the invalid character %s; only letters, digits and '-' are allowed"),
						PartName, *In, *DescribeChar(Current)));
					return false;
				}
				if (!IsAsciiDigit(Current))
				{
					bAllDigits = false;
				}
			}

			if (bRejectNumericLeadingZero && bAllDigits && Identifier.Len() > 1 && Identifier[0] == TEXT('0'))
			{
				SetError(OutError, FString::Printf(
					TEXT("the %s '%s' has the numeric identifier '%s' with a leading zero"),
					PartName, *In, *Identifier));
				return false;
			}
		}

		return true;
	}

	/**
	 * Compares two all-digit identifiers as numbers without ever converting them, so arbitrarily
	 * long identifiers are handled exactly. Leading zeroes are tolerated here even though the parser
	 * rejects them, because PreRelease is a public UPROPERTY that anyone can assign to.
	 */
	static int32 CompareNumericIdentifier(const FString& A, const FString& B)
	{
		int32 AStart = 0;
		while (AStart + 1 < A.Len() && A[AStart] == TEXT('0'))
		{
			++AStart;
		}

		int32 BStart = 0;
		while (BStart + 1 < B.Len() && B[BStart] == TEXT('0'))
		{
			++BStart;
		}

		const int32 ALength = A.Len() - AStart;
		const int32 BLength = B.Len() - BStart;
		if (ALength != BLength)
		{
			return ALength < BLength ? -1 : 1;
		}

		for (int32 Index = 0; Index < ALength; ++Index)
		{
			const TCHAR CharA = A[AStart + Index];
			const TCHAR CharB = B[BStart + Index];
			if (CharA != CharB)
			{
				return CharA < CharB ? -1 : 1;
			}
		}

		return 0;
	}

	static int32 ComparePreReleaseIdentifier(const FString& A, const FString& B)
	{
		const bool bNumericA = IsAllDigits(A);
		const bool bNumericB = IsAllDigits(B);

		if (bNumericA && bNumericB)
		{
			return CompareNumericIdentifier(A, B);
		}

		// Semver 2.0.0 rule 11.4.3: numeric identifiers always have lower precedence than
		// alphanumeric ones.
		if (bNumericA)
		{
			return -1;
		}
		if (bNumericB)
		{
			return 1;
		}

		const int32 Raw = FCString::Strcmp(*A, *B);
		return Raw < 0 ? -1 : (Raw > 0 ? 1 : 0);
	}

	static int32 ComparePreRelease(const FString& A, const FString& B)
	{
		const bool bEmptyA = A.IsEmpty();
		const bool bEmptyB = B.IsEmpty();

		if (bEmptyA && bEmptyB)
		{
			return 0;
		}

		// A release outranks any pre-release of the same major.minor.patch.
		if (bEmptyA)
		{
			return 1;
		}
		if (bEmptyB)
		{
			return -1;
		}

		TArray<FString> IdentifiersA;
		TArray<FString> IdentifiersB;
		A.ParseIntoArray(IdentifiersA, TEXT("."), false);
		B.ParseIntoArray(IdentifiersB, TEXT("."), false);

		const int32 Shared = IdentifiersA.Num() < IdentifiersB.Num() ? IdentifiersA.Num() : IdentifiersB.Num();
		for (int32 Index = 0; Index < Shared; ++Index)
		{
			const int32 Result = ComparePreReleaseIdentifier(IdentifiersA[Index], IdentifiersB[Index]);
			if (Result != 0)
			{
				return Result;
			}
		}

		// A larger set of pre-release fields wins when all preceding identifiers are equal.
		if (IdentifiersA.Num() == IdentifiersB.Num())
		{
			return 0;
		}
		return IdentifiersA.Num() < IdentifiersB.Num() ? -1 : 1;
	}

	/**
	 * A possibly partial / wildcarded version as written inside a range expression.
	 * Specified counts the leading concrete numeric components (0..3); the remaining slots stay 0.
	 */
	struct FPartialVersion
	{
		int32 Nums[3] = { 0, 0, 0 };
		int32 Specified = 0;
		FString PreRelease;
		FString BuildMetadata;
	};

	static bool ParsePartialVersion(const FString& In, FPartialVersion& Out, FString* OutError)
	{
		Out = FPartialVersion();

		FString Work = In;
		if (Work.IsEmpty())
		{
			SetError(OutError, FString(TEXT("expected a version")));
			return false;
		}

		if (Work[0] == TEXT('v') || Work[0] == TEXT('V'))
		{
			Work.RightChopInline(1);
			if (Work.IsEmpty())
			{
				SetError(OutError, FString::Printf(TEXT("'%s' is not a version"), *In));
				return false;
			}
		}

		// Build metadata must be split off first: it may itself contain '-'.
		FString BuildMetadata;
		int32 PlusIndex = INDEX_NONE;
		if (Work.FindChar(TEXT('+'), PlusIndex))
		{
			BuildMetadata = Work.Mid(PlusIndex + 1);
			Work.LeftInline(PlusIndex);
		}

		FString PreRelease;
		int32 DashIndex = INDEX_NONE;
		if (Work.FindChar(TEXT('-'), DashIndex))
		{
			PreRelease = Work.Mid(DashIndex + 1);
			Work.LeftInline(DashIndex);
		}

		TArray<FString> Components;
		Work.ParseIntoArray(Components, TEXT("."), false);

		if (Components.Num() > 3)
		{
			SetError(OutError, FString::Printf(
				TEXT("'%s' has %d dot separated numeric components, at most three are allowed"),
				*In, Components.Num()));
			return false;
		}

		bool bWildcardSeen = false;
		int32 Specified = 0;
		int32 Nums[3] = { 0, 0, 0 };

		for (int32 Index = 0; Index < Components.Num(); ++Index)
		{
			if (IsWildcardComponent(Components[Index]))
			{
				bWildcardSeen = true;
				continue;
			}

			if (bWildcardSeen)
			{
				SetError(OutError, FString::Printf(
					TEXT("'%s' places the concrete %s component after a wildcard"),
					*In, CoreComponentNames[Index]));
				return false;
			}

			if (!ParseCoreComponent(Components[Index], Nums[Index], OutError, CoreComponentNames[Index]))
			{
				return false;
			}
			Specified = Index + 1;
		}

		if (DashIndex != INDEX_NONE || PlusIndex != INDEX_NONE)
		{
			if (bWildcardSeen || Specified != 3)
			{
				SetError(OutError, FString::Printf(
					TEXT("'%s' attaches a pre-release or build metadata to a partial or wildcarded version; an exact major.minor.patch is required there"),
					*In));
				return false;
			}

			if (DashIndex != INDEX_NONE && !ValidateIdentifierList(PreRelease, true, OutError, TEXT("pre-release")))
			{
				return false;
			}

			if (PlusIndex != INDEX_NONE && !ValidateIdentifierList(BuildMetadata, false, OutError, TEXT("build metadata")))
			{
				return false;
			}
		}

		Out.Nums[0] = Nums[0];
		Out.Nums[1] = Nums[1];
		Out.Nums[2] = Nums[2];
		Out.Specified = Specified;
		Out.PreRelease = MoveTemp(PreRelease);
		Out.BuildMetadata = MoveTemp(BuildMetadata);
		return true;
	}

	static FModVersion MakeLowerBound(const FPartialVersion& In)
	{
		FModVersion Result;
		Result.Major = In.Nums[0];
		Result.Minor = In.Nums[1];
		Result.Patch = In.Nums[2];
		Result.PreRelease = In.PreRelease;
		Result.BuildMetadata = In.BuildMetadata;
		return Result;
	}

	/** Exclusive upper bound implied by a partial version: "1.2" -> 1.3.0, "1" -> 2.0.0. */
	static bool MakePartialUpperBound(const FPartialVersion& In, FModVersion& Out)
	{
		int32 Next = 0;

		if (In.Specified >= 2)
		{
			if (!SafeIncrement(In.Nums[1], Next))
			{
				return false;
			}
			Out = FModVersion(In.Nums[0], Next, 0);
			return true;
		}

		if (!SafeIncrement(In.Nums[0], Next))
		{
			return false;
		}
		Out = FModVersion(Next, 0, 0);
		return true;
	}

	/** ^1.2.3 -> <2.0.0, ^0.2.3 -> <0.3.0, ^0.0.3 -> <0.0.4, ^0.0 -> <0.1.0, ^0 -> <1.0.0. */
	static bool MakeCaretUpperBound(const FPartialVersion& In, FModVersion& Out)
	{
		int32 Next = 0;

		if (In.Nums[0] != 0)
		{
			if (!SafeIncrement(In.Nums[0], Next))
			{
				return false;
			}
			Out = FModVersion(Next, 0, 0);
			return true;
		}

		if (In.Specified == 1)
		{
			Out = FModVersion(1, 0, 0);
			return true;
		}

		if (In.Nums[1] != 0)
		{
			if (!SafeIncrement(In.Nums[1], Next))
			{
				return false;
			}
			Out = FModVersion(0, Next, 0);
			return true;
		}

		if (In.Specified == 2)
		{
			Out = FModVersion(0, 1, 0);
			return true;
		}

		if (!SafeIncrement(In.Nums[2], Next))
		{
			return false;
		}
		Out = FModVersion(0, 0, Next);
		return true;
	}

	/** ~1.2.3 and ~1.2 -> <1.3.0, ~1 -> <2.0.0. */
	static bool MakeTildeUpperBound(const FPartialVersion& In, FModVersion& Out)
	{
		int32 Next = 0;

		if (In.Specified >= 2)
		{
			if (!SafeIncrement(In.Nums[1], Next))
			{
				return false;
			}
			Out = FModVersion(In.Nums[0], Next, 0);
			return true;
		}

		if (!SafeIncrement(In.Nums[0], Next))
		{
			return false;
		}
		Out = FModVersion(Next, 0, 0);
		return true;
	}

	static void AddComparator(TArray<FModVersionComparator>& Out, EModVersionOp Op, FModVersion InVersion)
	{
		FModVersionComparator& Comparator = Out.AddDefaulted_GetRef();
		Comparator.Op = Op;
		Comparator.Version = MoveTemp(InVersion);
	}

	static const TCHAR* OperatorSymbol(EModVersionOp In)
	{
		switch (In)
		{
		case EModVersionOp::Equal:        return TEXT("=");
		case EModVersionOp::NotEqual:     return TEXT("!=");
		case EModVersionOp::Greater:      return TEXT(">");
		case EModVersionOp::GreaterEqual: return TEXT(">=");
		case EModVersionOp::Less:         return TEXT("<");
		case EModVersionOp::LessEqual:    return TEXT("<=");
		}
		return TEXT("=");
	}

	static bool EmitCaret(const FPartialVersion& Partial, const FString& Token, TArray<FModVersionComparator>& Out, FString* OutError)
	{
		if (Partial.Specified == 0)
		{
			// "^*" is the same as "*".
			return true;
		}

		FModVersion Upper;
		if (!MakeCaretUpperBound(Partial, Upper))
		{
			SetError(OutError, FString::Printf(TEXT("'%s' overflows the version range"), *Token));
			return false;
		}

		AddComparator(Out, EModVersionOp::GreaterEqual, MakeLowerBound(Partial));
		AddComparator(Out, EModVersionOp::Less, MoveTemp(Upper));
		return true;
	}

	static bool EmitTilde(const FPartialVersion& Partial, const FString& Token, TArray<FModVersionComparator>& Out, FString* OutError)
	{
		if (Partial.Specified == 0)
		{
			return true;
		}

		FModVersion Upper;
		if (!MakeTildeUpperBound(Partial, Upper))
		{
			SetError(OutError, FString::Printf(TEXT("'%s' overflows the version range"), *Token));
			return false;
		}

		AddComparator(Out, EModVersionOp::GreaterEqual, MakeLowerBound(Partial));
		AddComparator(Out, EModVersionOp::Less, MoveTemp(Upper));
		return true;
	}

	static bool EmitBare(const FPartialVersion& Partial, const FString& Token, TArray<FModVersionComparator>& Out, FString* OutError)
	{
		if (Partial.Specified == 0)
		{
			// "*", "x" and "" match everything.
			return true;
		}

		if (Partial.Specified == 3)
		{
			AddComparator(Out, EModVersionOp::Equal, MakeLowerBound(Partial));
			return true;
		}

		FModVersion Upper;
		if (!MakePartialUpperBound(Partial, Upper))
		{
			SetError(OutError, FString::Printf(TEXT("'%s' overflows the version range"), *Token));
			return false;
		}

		AddComparator(Out, EModVersionOp::GreaterEqual, MakeLowerBound(Partial));
		AddComparator(Out, EModVersionOp::Less, MoveTemp(Upper));
		return true;
	}

	static bool EmitOperator(EModVersionOp Op, const FPartialVersion& Partial, const FString& Token,
		TArray<FModVersionComparator>& Out, FString* OutError)
	{
		if (Partial.Specified == 3)
		{
			AddComparator(Out, Op, MakeLowerBound(Partial));
			return true;
		}

		if (Op == EModVersionOp::NotEqual)
		{
			SetError(OutError, FString::Printf(
				TEXT("'%s' is not supported: '!=' requires an exact major.minor.patch version"), *Token));
			return false;
		}

		if (Partial.Specified == 0)
		{
			// ">=*", "<=*" and "=*" accept everything; "<*" and ">*" are almost certainly a mistake.
			if (Op == EModVersionOp::GreaterEqual || Op == EModVersionOp::LessEqual || Op == EModVersionOp::Equal)
			{
				return true;
			}

			SetError(OutError, FString::Printf(
				TEXT("'%s' is not supported: the operator '%s' cannot be applied to a wildcard version"),
				*Token, OperatorSymbol(Op)));
			return false;
		}

		FModVersion Upper;
		const bool bHasUpper = MakePartialUpperBound(Partial, Upper);

		switch (Op)
		{
		case EModVersionOp::GreaterEqual:
			AddComparator(Out, EModVersionOp::GreaterEqual, MakeLowerBound(Partial));
			return true;

		case EModVersionOp::Less:
			AddComparator(Out, EModVersionOp::Less, MakeLowerBound(Partial));
			return true;

		case EModVersionOp::Greater:
			if (!bHasUpper)
			{
				SetError(OutError, FString::Printf(TEXT("'%s' overflows the version range"), *Token));
				return false;
			}
			AddComparator(Out, EModVersionOp::GreaterEqual, MoveTemp(Upper));
			return true;

		case EModVersionOp::LessEqual:
			if (!bHasUpper)
			{
				SetError(OutError, FString::Printf(TEXT("'%s' overflows the version range"), *Token));
				return false;
			}
			AddComparator(Out, EModVersionOp::Less, MoveTemp(Upper));
			return true;

		case EModVersionOp::Equal:
			if (!bHasUpper)
			{
				SetError(OutError, FString::Printf(TEXT("'%s' overflows the version range"), *Token));
				return false;
			}
			AddComparator(Out, EModVersionOp::GreaterEqual, MakeLowerBound(Partial));
			AddComparator(Out, EModVersionOp::Less, MoveTemp(Upper));
			return true;

		default:
			break;
		}

		SetError(OutError, FString::Printf(TEXT("'%s' uses an unsupported operator"), *Token));
		return false;
	}

	static bool ParseComparatorToken(const FString& Token, TArray<FModVersionComparator>& Out, FString* OutError)
	{
		FString Work = Token;
		EModVersionOp Op = EModVersionOp::Equal;
		bool bHasOperator = false;
		bool bCaret = false;
		bool bTilde = false;

		auto StartsWithSymbol = [&Work](const TCHAR* Symbol)
		{
			return Work.StartsWith(Symbol, ESearchCase::CaseSensitive);
		};

		if (StartsWithSymbol(TEXT(">=")))
		{
			Op = EModVersionOp::GreaterEqual;
			bHasOperator = true;
			Work.RightChopInline(2);
		}
		else if (StartsWithSymbol(TEXT("<=")))
		{
			Op = EModVersionOp::LessEqual;
			bHasOperator = true;
			Work.RightChopInline(2);
		}
		else if (StartsWithSymbol(TEXT("!=")))
		{
			Op = EModVersionOp::NotEqual;
			bHasOperator = true;
			Work.RightChopInline(2);
		}
		else if (StartsWithSymbol(TEXT("==")))
		{
			Op = EModVersionOp::Equal;
			bHasOperator = true;
			Work.RightChopInline(2);
		}
		else if (StartsWithSymbol(TEXT("~>")))
		{
			bTilde = true;
			Work.RightChopInline(2);
		}
		else if (StartsWithSymbol(TEXT(">")))
		{
			Op = EModVersionOp::Greater;
			bHasOperator = true;
			Work.RightChopInline(1);
		}
		else if (StartsWithSymbol(TEXT("<")))
		{
			Op = EModVersionOp::Less;
			bHasOperator = true;
			Work.RightChopInline(1);
		}
		else if (StartsWithSymbol(TEXT("=")))
		{
			Op = EModVersionOp::Equal;
			bHasOperator = true;
			Work.RightChopInline(1);
		}
		else if (StartsWithSymbol(TEXT("^")))
		{
			bCaret = true;
			Work.RightChopInline(1);
		}
		else if (StartsWithSymbol(TEXT("~")))
		{
			bTilde = true;
			Work.RightChopInline(1);
		}

		Work.TrimStartAndEndInline();

		if (Work.IsEmpty())
		{
			SetError(OutError, FString::Printf(TEXT("'%s' is missing a version"), *Token));
			return false;
		}

		FPartialVersion Partial;
		if (!ParsePartialVersion(Work, Partial, OutError))
		{
			return false;
		}

		if (bCaret)
		{
			return EmitCaret(Partial, Token, Out, OutError);
		}
		if (bTilde)
		{
			return EmitTilde(Partial, Token, Out, OutError);
		}
		if (!bHasOperator)
		{
			return EmitBare(Partial, Token, Out, OutError);
		}
		return EmitOperator(Op, Partial, Token, Out, OutError);
	}

	static bool ParseHyphenRange(const FString& FromToken, const FString& ToToken,
		TArray<FModVersionComparator>& Out, FString* OutError)
	{
		FPartialVersion From;
		if (!ParsePartialVersion(FromToken, From, OutError))
		{
			return false;
		}

		FPartialVersion To;
		if (!ParsePartialVersion(ToToken, To, OutError))
		{
			return false;
		}

		if (From.Specified > 0)
		{
			AddComparator(Out, EModVersionOp::GreaterEqual, MakeLowerBound(From));
		}

		if (To.Specified == 3)
		{
			// The hyphen range is inclusive on both ends.
			AddComparator(Out, EModVersionOp::LessEqual, MakeLowerBound(To));
		}
		else if (To.Specified > 0)
		{
			// "1.2.3 - 2.3" means everything up to and including the whole 2.3.x line.
			FModVersion Upper;
			if (!MakePartialUpperBound(To, Upper))
			{
				SetError(OutError, FString::Printf(TEXT("'%s' overflows the version range"), *ToToken));
				return false;
			}
			AddComparator(Out, EModVersionOp::Less, MoveTemp(Upper));
		}

		return true;
	}

	static bool IsBareOperatorToken(const FString& In)
	{
		static const TCHAR* const Operators[] =
		{
			TEXT(">="), TEXT("<="), TEXT("!="), TEXT("=="), TEXT("~>"),
			TEXT(">"), TEXT("<"), TEXT("="), TEXT("^"), TEXT("~")
		};

		for (const TCHAR* const Operator : Operators)
		{
			if (FCString::Strcmp(*In, Operator) == 0)
			{
				return true;
			}
		}
		return false;
	}

	static bool ParseClauseText(const FString& ClauseText, FModVersionClause& OutClause, FString* OutError)
	{
		OutClause.Comparators.Reset();

		if (FCString::Stricmp(*ClauseText, TEXT("any")) == 0)
		{
			return true;
		}

		TArray<FString> RawTokens;
		ClauseText.ParseIntoArrayWS(RawTokens, TEXT(","), true);

		if (RawTokens.Num() == 0)
		{
			// An empty clause matches everything.
			return true;
		}

		// Allow "> = 1.2.3" style spacing by gluing a lone operator onto the token that follows it.
		TArray<FString> Tokens;
		Tokens.Reserve(RawTokens.Num());
		for (int32 Index = 0; Index < RawTokens.Num(); ++Index)
		{
			if (!IsBareOperatorToken(RawTokens[Index]))
			{
				Tokens.Add(RawTokens[Index]);
				continue;
			}

			if (Index + 1 >= RawTokens.Num() || IsBareOperatorToken(RawTokens[Index + 1]))
			{
				SetError(OutError, FString::Printf(
					TEXT("the operator '%s' is not followed by a version"), *RawTokens[Index]));
				return false;
			}

			Tokens.Add(RawTokens[Index] + RawTokens[Index + 1]);
			++Index;
		}

		int32 HyphenIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Tokens.Num(); ++Index)
		{
			if (FCString::Strcmp(*Tokens[Index], TEXT("-")) == 0)
			{
				HyphenIndex = Index;
				break;
			}
		}

		if (HyphenIndex != INDEX_NONE)
		{
			if (Tokens.Num() != 3 || HyphenIndex != 1)
			{
				SetError(OutError, FString(TEXT("a hyphen range must be written as '<from> - <to>'")));
				return false;
			}
			return ParseHyphenRange(Tokens[0], Tokens[2], OutClause.Comparators, OutError);
		}

		for (const FString& Token : Tokens)
		{
			if (!ParseComparatorToken(Token, OutClause.Comparators, OutError))
			{
				return false;
			}
		}

		return true;
	}

	/** Cheap, allocation free test for the expressions that mean "any version". */
	static bool IsTriviallyAnyExpression(const FString& In)
	{
		const TCHAR* Begin = *In;
		const TCHAR* End = Begin + In.Len();

		while (Begin < End && FChar::IsWhitespace(*Begin))
		{
			++Begin;
		}
		while (End > Begin && FChar::IsWhitespace(*(End - 1)))
		{
			--End;
		}

		const int32 Length = static_cast<int32>(End - Begin);
		if (Length == 0)
		{
			return true;
		}
		if (Length == 1)
		{
			return *Begin == TEXT('*') || *Begin == TEXT('x') || *Begin == TEXT('X');
		}
		if (Length == 3)
		{
			return FChar::ToLower(Begin[0]) == TEXT('a')
				&& FChar::ToLower(Begin[1]) == TEXT('n')
				&& FChar::ToLower(Begin[2]) == TEXT('y');
		}
		return false;
	}

	/**
	 * Returns the clause set to evaluate without mutating Range. Scratch receives the clauses when
	 * the range has not been parsed yet. Returns nullptr when the expression is malformed.
	 */
	static const TArray<FModVersionClause>* ResolveClauses(const FModVersionRange& Range, TArray<FModVersionClause>& Scratch)
	{
		if (Range.bParsed)
		{
			return Range.bParseFailed ? nullptr : &Range.Clauses;
		}

		if (!FModVersionRange::ParseExpression(Range.Expression, Scratch, nullptr))
		{
			return nullptr;
		}
		return &Scratch;
	}

	static FString DescribeComparatorInWords(const FModVersionComparator& In)
	{
		const FString VersionText = In.Version.ToString();

		switch (In.Op)
		{
		case EModVersionOp::Equal:        return FString::Printf(TEXT("exactly %s"), *VersionText);
		case EModVersionOp::NotEqual:     return FString::Printf(TEXT("any version other than %s"), *VersionText);
		case EModVersionOp::Greater:      return FString::Printf(TEXT("newer than %s"), *VersionText);
		case EModVersionOp::GreaterEqual: return FString::Printf(TEXT("%s or newer"), *VersionText);
		case EModVersionOp::Less:         return FString::Printf(TEXT("older than %s"), *VersionText);
		case EModVersionOp::LessEqual:    return FString::Printf(TEXT("%s or older"), *VersionText);
		}

		return VersionText;
	}

	static FString DescribeClause(const FModVersionClause& In)
	{
		if (In.Comparators.Num() == 0)
		{
			return TEXT("any version");
		}

		if (In.Comparators.Num() == 1)
		{
			return DescribeComparatorInWords(In.Comparators[0]);
		}

		FString Result;
		for (int32 Index = 0; Index < In.Comparators.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += TEXT(" and ");
			}
			Result += OperatorSymbol(In.Comparators[Index].Op);
			Result += TEXT(" ");
			Result += In.Comparators[Index].Version.ToString();
		}
		return Result;
	}
}

//////////////////////////////////////////////////////////////////////////
// FModVersion

FModVersion::FModVersion(int32 InMajor, int32 InMinor, int32 InPatch, FString InPreRelease, FString InBuild)
	: Major(InMajor)
	, Minor(InMinor)
	, Patch(InPatch)
	, PreRelease(MoveTemp(InPreRelease))
	, BuildMetadata(MoveTemp(InBuild))
{
}

bool FModVersion::Parse(const FString& In, FModVersion& Out, FString* OutError, bool bAllowPartial)
{
	using namespace ModVersionPrivate;

	Out = FModVersion();

	FString Work = In.TrimStartAndEnd();
	if (Work.IsEmpty())
	{
		SetError(OutError, FString(TEXT("the version string is empty")));
		return false;
	}

	if (Work[0] == TEXT('v') || Work[0] == TEXT('V'))
	{
		Work.RightChopInline(1);
		if (Work.IsEmpty())
		{
			SetError(OutError, FString::Printf(TEXT("'%s' is not a version"), *In));
			return false;
		}
	}

	// Build metadata first: it is allowed to contain '-'.
	FString BuildMetadata;
	int32 PlusIndex = INDEX_NONE;
	if (Work.FindChar(TEXT('+'), PlusIndex))
	{
		BuildMetadata = Work.Mid(PlusIndex + 1);
		Work.LeftInline(PlusIndex);
	}

	FString PreRelease;
	int32 DashIndex = INDEX_NONE;
	if (Work.FindChar(TEXT('-'), DashIndex))
	{
		PreRelease = Work.Mid(DashIndex + 1);
		Work.LeftInline(DashIndex);
	}

	TArray<FString> Components;
	Work.ParseIntoArray(Components, TEXT("."), false);

	if (Components.Num() == 0)
	{
		SetError(OutError, FString::Printf(TEXT("'%s' has no numeric components"), *In));
		return false;
	}

	if (Components.Num() > 3)
	{
		SetError(OutError, FString::Printf(
			TEXT("'%s' has %d dot separated numeric components, at most three are allowed"),
			*In, Components.Num()));
		return false;
	}

	if (Components.Num() < 3 && !bAllowPartial)
	{
		SetError(OutError, FString::Printf(
			TEXT("'%s' is a partial version; a full major.minor.patch is required here"), *In));
		return false;
	}

	int32 Numbers[3] = { 0, 0, 0 };
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		if (!ParseCoreComponent(Components[Index], Numbers[Index], OutError, CoreComponentNames[Index]))
		{
			return false;
		}
	}

	if ((DashIndex != INDEX_NONE || PlusIndex != INDEX_NONE) && Components.Num() < 3)
	{
		SetError(OutError, FString::Printf(
			TEXT("'%s' attaches a pre-release or build metadata to a partial version; an exact major.minor.patch is required there"),
			*In));
		return false;
	}

	if (DashIndex != INDEX_NONE && !ValidateIdentifierList(PreRelease, true, OutError, TEXT("pre-release")))
	{
		return false;
	}

	if (PlusIndex != INDEX_NONE && !ValidateIdentifierList(BuildMetadata, false, OutError, TEXT("build metadata")))
	{
		return false;
	}

	Out.Major = Numbers[0];
	Out.Minor = Numbers[1];
	Out.Patch = Numbers[2];
	Out.PreRelease = MoveTemp(PreRelease);
	Out.BuildMetadata = MoveTemp(BuildMetadata);
	return true;
}

FModVersion FModVersion::FromString(const FString& In)
{
	FModVersion Result;
	if (!Parse(In, Result, nullptr))
	{
		Result = FModVersion();
	}
	return Result;
}

FString FModVersion::ToString() const
{
	FString Result = FString::Printf(TEXT("%d.%d.%d"), Major, Minor, Patch);

	if (!PreRelease.IsEmpty())
	{
		Result += TEXT("-");
		Result += PreRelease;
	}

	if (!BuildMetadata.IsEmpty())
	{
		Result += TEXT("+");
		Result += BuildMetadata;
	}

	return Result;
}

bool FModVersion::IsZero() const
{
	return Major == 0 && Minor == 0 && Patch == 0 && PreRelease.IsEmpty();
}

int32 FModVersion::Compare(const FModVersion& Other) const
{
	if (Major != Other.Major)
	{
		return Major < Other.Major ? -1 : 1;
	}
	if (Minor != Other.Minor)
	{
		return Minor < Other.Minor ? -1 : 1;
	}
	if (Patch != Other.Patch)
	{
		return Patch < Other.Patch ? -1 : 1;
	}
	return ModVersionPrivate::ComparePreRelease(PreRelease, Other.PreRelease);
}

//////////////////////////////////////////////////////////////////////////
// FModVersionComparator

bool FModVersionComparator::Satisfies(const FModVersion& In) const
{
	const int32 Result = In.Compare(Version);

	switch (Op)
	{
	case EModVersionOp::Equal:        return Result == 0;
	case EModVersionOp::NotEqual:     return Result != 0;
	case EModVersionOp::Greater:      return Result > 0;
	case EModVersionOp::GreaterEqual: return Result >= 0;
	case EModVersionOp::Less:         return Result < 0;
	case EModVersionOp::LessEqual:    return Result <= 0;
	}

	return false;
}

FString FModVersionComparator::ToString() const
{
	FString Result = ModVersionPrivate::OperatorSymbol(Op);
	Result += Version.ToString();
	return Result;
}

//////////////////////////////////////////////////////////////////////////
// FModVersionClause

bool FModVersionClause::Satisfies(const FModVersion& In) const
{
	if (Comparators.Num() == 0)
	{
		return true;
	}

	// npm pre-release gate: a pre-release only ever participates in a clause that explicitly names a
	// pre-release on the same major.minor.patch. Without this, 2.0.0-rc1 would match ">=1.0.0".
	if (In.IsPreRelease())
	{
		bool bGateOpen = false;
		for (const FModVersionComparator& Comparator : Comparators)
		{
			if (Comparator.Version.IsPreRelease()
				&& Comparator.Version.Major == In.Major
				&& Comparator.Version.Minor == In.Minor
				&& Comparator.Version.Patch == In.Patch)
			{
				bGateOpen = true;
				break;
			}
		}

		if (!bGateOpen)
		{
			return false;
		}
	}

	for (const FModVersionComparator& Comparator : Comparators)
	{
		if (!Comparator.Satisfies(In))
		{
			return false;
		}
	}

	return true;
}

FString FModVersionClause::ToString() const
{
	if (Comparators.Num() == 0)
	{
		return TEXT("*");
	}

	FString Result;
	for (int32 Index = 0; Index < Comparators.Num(); ++Index)
	{
		if (Index > 0)
		{
			Result += TEXT(" ");
		}
		Result += Comparators[Index].ToString();
	}
	return Result;
}

//////////////////////////////////////////////////////////////////////////
// FModVersionRange

bool FModVersionRange::ParseExpression(const FString& InExpression, TArray<FModVersionClause>& OutClauses, FString* OutError)
{
	using namespace ModVersionPrivate;

	OutClauses.Reset();

	if (IsTriviallyAnyExpression(InExpression))
	{
		OutClauses.AddDefaulted();
		return true;
	}

	TArray<FString> ClauseTexts;
	InExpression.ParseIntoArray(ClauseTexts, TEXT("||"), false);

	if (ClauseTexts.Num() == 0)
	{
		OutClauses.AddDefaulted();
		return true;
	}

	OutClauses.Reserve(ClauseTexts.Num());

	for (const FString& ClauseText : ClauseTexts)
	{
		const FString Trimmed = ClauseText.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutClauses.Reset();
			SetError(OutError, FString::Printf(
				TEXT("version range '%s' contains an empty '||' clause"), *InExpression));
			return false;
		}

		FModVersionClause Clause;
		FString ClauseError;
		if (!ParseClauseText(Trimmed, Clause, &ClauseError))
		{
			OutClauses.Reset();
			SetError(OutError, FString::Printf(
				TEXT("invalid version range '%s': %s"), *InExpression, *ClauseError));
			return false;
		}

		OutClauses.Add(MoveTemp(Clause));
	}

	return true;
}

bool FModVersionRange::Parse(const FString& In, FModVersionRange& Out, FString* OutError)
{
	Out.Expression = In;
	Out.Clauses.Reset();
	Out.bParsed = true;
	Out.bParseFailed = !ParseExpression(In, Out.Clauses, OutError);

	if (Out.bParseFailed)
	{
		Out.Clauses.Reset();
	}

	return !Out.bParseFailed;
}

FModVersionRange FModVersionRange::Any()
{
	FModVersionRange Result;
	Result.Expression = TEXT("*");
	Result.bParsed = true;
	Result.bParseFailed = false;
	Result.Clauses.AddDefaulted();
	return Result;
}

FModVersionRange FModVersionRange::Exactly(const FModVersion& In)
{
	FModVersionRange Result;
	Result.Expression = In.ToString();
	Result.bParsed = true;
	Result.bParseFailed = false;

	FModVersionClause& Clause = Result.Clauses.AddDefaulted_GetRef();
	FModVersionComparator& Comparator = Clause.Comparators.AddDefaulted_GetRef();
	Comparator.Op = EModVersionOp::Equal;
	Comparator.Version = In;

	return Result;
}

bool FModVersionRange::IsAny() const
{
	using namespace ModVersionPrivate;

	if (!bParsed && IsTriviallyAnyExpression(Expression))
	{
		return true;
	}

	TArray<FModVersionClause> Scratch;
	const TArray<FModVersionClause>* Resolved = ResolveClauses(*this, Scratch);
	if (Resolved == nullptr)
	{
		return false;
	}

	if (Resolved->Num() == 0)
	{
		return true;
	}

	for (const FModVersionClause& Clause : *Resolved)
	{
		if (Clause.Comparators.Num() == 0)
		{
			return true;
		}
	}

	return false;
}

bool FModVersionRange::IsValid() const
{
	using namespace ModVersionPrivate;

	if (bParsed)
	{
		return !bParseFailed;
	}

	if (IsTriviallyAnyExpression(Expression))
	{
		return true;
	}

	TArray<FModVersionClause> Scratch;
	return ParseExpression(Expression, Scratch, nullptr);
}

bool FModVersionRange::Satisfies(const FModVersion& In) const
{
	using namespace ModVersionPrivate;

	if (!bParsed && IsTriviallyAnyExpression(Expression))
	{
		return true;
	}

	TArray<FModVersionClause> Scratch;
	const TArray<FModVersionClause>* Resolved = ResolveClauses(*this, Scratch);
	if (Resolved == nullptr)
	{
		return false;
	}

	if (Resolved->Num() == 0)
	{
		return true;
	}

	for (const FModVersionClause& Clause : *Resolved)
	{
		if (Clause.Satisfies(In))
		{
			return true;
		}
	}

	return false;
}

FString FModVersionRange::Describe() const
{
	using namespace ModVersionPrivate;

	TArray<FModVersionClause> Scratch;
	const TArray<FModVersionClause>* Resolved = ResolveClauses(*this, Scratch);
	if (Resolved == nullptr)
	{
		return FString::Printf(TEXT("invalid version range '%s'"), *Expression);
	}

	if (Resolved->Num() == 0)
	{
		return TEXT("any version");
	}

	FString Result;
	for (int32 Index = 0; Index < Resolved->Num(); ++Index)
	{
		if (Index > 0)
		{
			Result += TEXT(", or ");
		}
		Result += DescribeClause((*Resolved)[Index]);
	}
	return Result;
}

bool operator==(const FModVersionRange& A, const FModVersionRange& B)
{
	return A.Expression.Equals(B.Expression, ESearchCase::CaseSensitive);
}

bool operator!=(const FModVersionRange& A, const FModVersionRange& B)
{
	return !A.Expression.Equals(B.Expression, ESearchCase::CaseSensitive);
}

uint32 GetTypeHash(const FModVersionRange& In)
{
	return GetTypeHash(In.Expression);
}
