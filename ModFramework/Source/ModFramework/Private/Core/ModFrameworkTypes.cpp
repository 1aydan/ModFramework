// Copyright (c) 2026. Licensed for use in your own projects.

#include "Core/ModFrameworkTypes.h"

#include "Core/ModFrameworkLog.h"
#include "Logging/LogMacros.h"
#include "Misc/CString.h"
#include "Templates/UnrealTemplate.h"

namespace ModFrameworkTypesPrivate
{
	/** Lowest/highest accepted length of a mod id, inclusive. */
	static constexpr int32 MinModIdLength = 3;
	static constexpr int32 MaxModIdLength = 128;

	/** True for the characters a mod id may start with. */
	static bool IsLowercaseAlphanumeric(TCHAR In)
	{
		return (In >= TEXT('a') && In <= TEXT('z')) || (In >= TEXT('0') && In <= TEXT('9'));
	}

	/** True for the separator characters a mod id may contain but not start or end with. */
	static bool IsModIdSeparator(TCHAR In)
	{
		return In == TEXT('.') || In == TEXT('_') || In == TEXT('-');
	}

	/**
	 * Renders a single character for an error message. Printable ASCII is quoted verbatim, anything
	 * else (control characters, whitespace, non-ASCII) becomes "U+XXXX" so the message stays
	 * readable in a log file.
	 */
	static FString DescribeChar(TCHAR In)
	{
		const uint32 Code = static_cast<uint32>(In);
		if (Code >= 0x21 && Code <= 0x7E)
		{
			return FString::Printf(TEXT("'%c'"), In);
		}
		return FString::Printf(TEXT("U+%04X"), Code);
	}

	static bool EqualsIgnoreCase(const FString& In, const TCHAR* Literal)
	{
		return FCString::Stricmp(*In, Literal) == 0;
	}
}

//////////////////////////////////////////////////////////////////////////
// FModId

bool FModId::IsValidIdString(const FString& InString, FString& OutError)
{
	using namespace ModFrameworkTypesPrivate;

	const int32 Length = InString.Len();

	if (Length == 0)
	{
		OutError = TEXT("Mod id is empty.");
		return false;
	}

	if (Length < MinModIdLength)
	{
		OutError = FString::Printf(
			TEXT("Mod id '%s' is too short: %d characters, the minimum is %d."),
			*InString, Length, MinModIdLength);
		return false;
	}

	if (Length > MaxModIdLength)
	{
		OutError = FString::Printf(
			TEXT("Mod id '%s' is too long: %d characters, the maximum is %d."),
			*InString, Length, MaxModIdLength);
		return false;
	}

	const TCHAR* Chars = *InString;

	if (!IsLowercaseAlphanumeric(Chars[0]))
	{
		OutError = FString::Printf(
			TEXT("Mod id '%s' must start with a lowercase letter or a digit, but starts with %s at index 0."),
			*InString, *DescribeChar(Chars[0]));
		return false;
	}

	for (int32 Index = 0; Index < Length; ++Index)
	{
		const TCHAR Current = Chars[Index];

		if (!IsLowercaseAlphanumeric(Current) && !IsModIdSeparator(Current))
		{
			OutError = FString::Printf(
				TEXT("Mod id '%s' contains the invalid character %s at index %d; only lowercase letters, digits, '.', '_' and '-' are allowed."),
				*InString, *DescribeChar(Current), Index);
			return false;
		}

		if (Current == TEXT('.') && Index + 1 < Length && Chars[Index + 1] == TEXT('.'))
		{
			OutError = FString::Printf(
				TEXT("Mod id '%s' contains \"..\" at index %d."),
				*InString, Index);
			return false;
		}
	}

	const TCHAR Last = Chars[Length - 1];
	if (IsModIdSeparator(Last))
	{
		OutError = FString::Printf(
			TEXT("Mod id '%s' must not end with a separator, but ends with %s at index %d."),
			*InString, *DescribeChar(Last), Length - 1);
		return false;
	}

	OutError.Reset();
	return true;
}

bool FModId::TryParse(const FString& InString, FModId& OutId, FString& OutError)
{
	const FString Lowered = InString.ToLower();

	if (!IsValidIdString(Lowered, OutError))
	{
		OutId.Reset();
		return false;
	}

	OutId.Value = FName(*Lowered);
	OutError.Reset();
	return true;
}

FModId FModId::FromString(const FString& InString)
{
	FModId Result;
	if (!InString.IsEmpty())
	{
		Result.Value = FName(*InString.ToLower());
	}
	return Result;
}

//////////////////////////////////////////////////////////////////////////
// FModDiagnostic

FModDiagnostic::FModDiagnostic(EModDiagnosticSeverity InSeverity, FName InCode, FString InMessage, FString InContext)
	: Severity(InSeverity)
	, Code(InCode)
	, Message(MoveTemp(InMessage))
	, Context(MoveTemp(InContext))
{
}

FString FModDiagnostic::ToString() const
{
	FString Result;
	Result.Reserve(Message.Len() + Context.Len() + 32);

	Result += TEXT("[");
	Result += ModFrameworkEnums::ToString(Severity);
	Result += TEXT("] ");

	if (!Code.IsNone())
	{
		Result += Code.ToString();
		Result += TEXT(": ");
	}

	Result += Message;

	if (!Context.IsEmpty())
	{
		Result += TEXT(" (");
		Result += Context;
		Result += TEXT(")");
	}

	return Result;
}

FModDiagnostic FModDiagnostic::Error(FName Code, FString Message, FString Context)
{
	return FModDiagnostic(EModDiagnosticSeverity::Error, Code, MoveTemp(Message), MoveTemp(Context));
}

FModDiagnostic FModDiagnostic::Warning(FName Code, FString Message, FString Context)
{
	return FModDiagnostic(EModDiagnosticSeverity::Warning, Code, MoveTemp(Message), MoveTemp(Context));
}

FModDiagnostic FModDiagnostic::Info(FName Code, FString Message, FString Context)
{
	return FModDiagnostic(EModDiagnosticSeverity::Info, Code, MoveTemp(Message), MoveTemp(Context));
}

//////////////////////////////////////////////////////////////////////////
// ModDiagnostics

bool ModDiagnostics::HasErrors(const TArray<FModDiagnostic>& In)
{
	for (const FModDiagnostic& Diagnostic : In)
	{
		if (Diagnostic.IsError())
		{
			return true;
		}
	}
	return false;
}

FString ModDiagnostics::Join(const TArray<FModDiagnostic>& In, const TCHAR* Separator)
{
	const TCHAR* EffectiveSeparator = (Separator != nullptr) ? Separator : TEXT("\n");

	FString Result;
	for (int32 Index = 0; Index < In.Num(); ++Index)
	{
		if (Index > 0)
		{
			Result += EffectiveSeparator;
		}
		Result += In[Index].ToString();
	}
	return Result;
}

void ModDiagnostics::LogAll(const TArray<FModDiagnostic>& In)
{
	for (const FModDiagnostic& Diagnostic : In)
	{
		const FString Line = Diagnostic.ToString();

		if (Diagnostic.Severity == EModDiagnosticSeverity::Error)
		{
			UE_LOG(LogModFramework, Error, TEXT("%s"), *Line);
		}
		else if (Diagnostic.Severity == EModDiagnosticSeverity::Warning)
		{
			UE_LOG(LogModFramework, Warning, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogModFramework, Log, TEXT("%s"), *Line);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// ModFrameworkEnums

FString ModFrameworkEnums::ToString(EModState In)
{
	switch (In)
	{
	case EModState::Unknown:              return TEXT("Unknown");
	case EModState::Discovered:           return TEXT("Discovered");
	case EModState::Validated:            return TEXT("Validated");
	case EModState::DependenciesResolved: return TEXT("DependenciesResolved");
	case EModState::Mounted:              return TEXT("Mounted");
	case EModState::Loading:              return TEXT("Loading");
	case EModState::Loaded:               return TEXT("Loaded");
	case EModState::Activated:            return TEXT("Activated");
	case EModState::Deactivated:          return TEXT("Deactivated");
	case EModState::Unmounted:            return TEXT("Unmounted");
	case EModState::Failed:               return TEXT("Failed");
	case EModState::Disabled:             return TEXT("Disabled");
	}

	return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(In));
}

FString ModFrameworkEnums::ToString(EModLoadFailureReason In)
{
	switch (In)
	{
	case EModLoadFailureReason::None:                          return TEXT("None");
	case EModLoadFailureReason::ManifestMissing:               return TEXT("ManifestMissing");
	case EModLoadFailureReason::ManifestInvalid:               return TEXT("ManifestInvalid");
	case EModLoadFailureReason::DuplicateModId:                return TEXT("DuplicateModId");
	case EModLoadFailureReason::UnsupportedManifestVersion:    return TEXT("UnsupportedManifestVersion");
	case EModLoadFailureReason::IncompatibleGame:              return TEXT("IncompatibleGame");
	case EModLoadFailureReason::IncompatibleFramework:         return TEXT("IncompatibleFramework");
	case EModLoadFailureReason::IncompatibleSdk:               return TEXT("IncompatibleSdk");
	case EModLoadFailureReason::MissingDependency:             return TEXT("MissingDependency");
	case EModLoadFailureReason::IncompatibleDependencyVersion: return TEXT("IncompatibleDependencyVersion");
	case EModLoadFailureReason::CircularDependency:            return TEXT("CircularDependency");
	case EModLoadFailureReason::DependencyFailed:              return TEXT("DependencyFailed");
	case EModLoadFailureReason::MountFailed:                   return TEXT("MountFailed");
	case EModLoadFailureReason::PermissionDenied:              return TEXT("PermissionDenied");
	case EModLoadFailureReason::EntryPointMissing:             return TEXT("EntryPointMissing");
	case EModLoadFailureReason::EntryPointInvalid:             return TEXT("EntryPointInvalid");
	case EModLoadFailureReason::ActivationFailed:              return TEXT("ActivationFailed");
	case EModLoadFailureReason::ProviderError:                 return TEXT("ProviderError");
	case EModLoadFailureReason::ConflictRejected:              return TEXT("ConflictRejected");
	case EModLoadFailureReason::Disabled:                      return TEXT("Disabled");
	case EModLoadFailureReason::Internal:                      return TEXT("Internal");
	}

	return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(In));
}

FString ModFrameworkEnums::ToString(EModNetworkScope In)
{
	switch (In)
	{
	case EModNetworkScope::ClientAndServer: return TEXT("ClientAndServer");
	case EModNetworkScope::ClientOnly:      return TEXT("ClientOnly");
	case EModNetworkScope::ServerOnly:      return TEXT("ServerOnly");
	}

	return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(In));
}

FString ModFrameworkEnums::ToString(EModConflictPolicy In)
{
	switch (In)
	{
	case EModConflictPolicy::Error:     return TEXT("Error");
	case EModConflictPolicy::FirstWins: return TEXT("FirstWins");
	case EModConflictPolicy::LastWins:  return TEXT("LastWins");
	case EModConflictPolicy::Priority:  return TEXT("Priority");
	case EModConflictPolicy::Merge:     return TEXT("Merge");
	}

	return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(In));
}

FString ModFrameworkEnums::ToString(EModPermissionState In)
{
	switch (In)
	{
	case EModPermissionState::NotRequested: return TEXT("NotRequested");
	case EModPermissionState::Pending:      return TEXT("Pending");
	case EModPermissionState::Granted:      return TEXT("Granted");
	case EModPermissionState::Denied:       return TEXT("Denied");
	}

	return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(In));
}

FString ModFrameworkEnums::ToString(EModContentRootType In)
{
	switch (In)
	{
	case EModContentRootType::Pak:            return TEXT("Pak");
	case EModContentRootType::IoStore:        return TEXT("IoStore");
	case EModContentRootType::LooseDirectory: return TEXT("LooseDirectory");
	}

	return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(In));
}

FString ModFrameworkEnums::ToString(EModDiagnosticSeverity In)
{
	switch (In)
	{
	case EModDiagnosticSeverity::Info:    return TEXT("Info");
	case EModDiagnosticSeverity::Warning: return TEXT("Warning");
	case EModDiagnosticSeverity::Error:   return TEXT("Error");
	}

	return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(In));
}

bool ModFrameworkEnums::ParseNetworkScope(const FString& In, EModNetworkScope& Out)
{
	using namespace ModFrameworkTypesPrivate;

	const FString Trimmed = In.TrimStartAndEnd();

	if (EqualsIgnoreCase(Trimmed, TEXT("ClientAndServer"))
		|| EqualsIgnoreCase(Trimmed, TEXT("Both"))
		|| EqualsIgnoreCase(Trimmed, TEXT("Any")))
	{
		Out = EModNetworkScope::ClientAndServer;
		return true;
	}

	if (EqualsIgnoreCase(Trimmed, TEXT("ClientOnly"))
		|| EqualsIgnoreCase(Trimmed, TEXT("Client")))
	{
		Out = EModNetworkScope::ClientOnly;
		return true;
	}

	if (EqualsIgnoreCase(Trimmed, TEXT("ServerOnly"))
		|| EqualsIgnoreCase(Trimmed, TEXT("Server")))
	{
		Out = EModNetworkScope::ServerOnly;
		return true;
	}

	return false;
}

bool ModFrameworkEnums::ParseConflictPolicy(const FString& In, EModConflictPolicy& Out)
{
	using namespace ModFrameworkTypesPrivate;

	const FString Trimmed = In.TrimStartAndEnd();

	if (EqualsIgnoreCase(Trimmed, TEXT("Error")))
	{
		Out = EModConflictPolicy::Error;
		return true;
	}

	if (EqualsIgnoreCase(Trimmed, TEXT("FirstWins")) || EqualsIgnoreCase(Trimmed, TEXT("First")))
	{
		Out = EModConflictPolicy::FirstWins;
		return true;
	}

	if (EqualsIgnoreCase(Trimmed, TEXT("LastWins")) || EqualsIgnoreCase(Trimmed, TEXT("Last")))
	{
		Out = EModConflictPolicy::LastWins;
		return true;
	}

	if (EqualsIgnoreCase(Trimmed, TEXT("Priority")))
	{
		Out = EModConflictPolicy::Priority;
		return true;
	}

	if (EqualsIgnoreCase(Trimmed, TEXT("Merge")))
	{
		Out = EModConflictPolicy::Merge;
		return true;
	}

	return false;
}

bool ModFrameworkEnums::ParseContentRootType(const FString& In, EModContentRootType& Out)
{
	using namespace ModFrameworkTypesPrivate;

	const FString Trimmed = In.TrimStartAndEnd();

	if (EqualsIgnoreCase(Trimmed, TEXT("Pak")))
	{
		Out = EModContentRootType::Pak;
		return true;
	}

	if (EqualsIgnoreCase(Trimmed, TEXT("IoStore")))
	{
		Out = EModContentRootType::IoStore;
		return true;
	}

	if (EqualsIgnoreCase(Trimmed, TEXT("LooseDirectory"))
		|| EqualsIgnoreCase(Trimmed, TEXT("Loose"))
		|| EqualsIgnoreCase(Trimmed, TEXT("Directory")))
	{
		Out = EModContentRootType::LooseDirectory;
		return true;
	}

	return false;
}

bool ModFrameworkEnums::Parse(const FString& In, EModNetworkScope& Out)
{
	return ParseNetworkScope(In, Out);
}

bool ModFrameworkEnums::Parse(const FString& In, EModConflictPolicy& Out)
{
	return ParseConflictPolicy(In, Out);
}

bool ModFrameworkEnums::Parse(const FString& In, EModContentRootType& Out)
{
	return ParseContentRootType(In, Out);
}
