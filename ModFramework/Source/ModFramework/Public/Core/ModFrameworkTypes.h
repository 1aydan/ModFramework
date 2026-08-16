// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "ModFrameworkTypes.generated.h"

/**
 * Lifecycle state of a single mod.
 *
 * The values are serialized by name (never by index) but framework code switches on them, so the
 * order is part of the binding contract: do not rename, reorder or insert values.
 */
UENUM(BlueprintType)
enum class EModState : uint8
{
	Unknown              UMETA(DisplayName = "Unknown"),
	Discovered,
	Validated,
	DependenciesResolved UMETA(DisplayName = "Dependencies Resolved"),
	Mounted,
	Loading,
	Loaded,
	Activated,
	Deactivated,
	Unmounted,
	Failed,
	Disabled
};

/** Why a mod failed to load. `None` means "no failure recorded". */
UENUM(BlueprintType)
enum class EModLoadFailureReason : uint8
{
	None,
	ManifestMissing,
	ManifestInvalid,
	DuplicateModId,
	UnsupportedManifestVersion,
	IncompatibleGame,
	IncompatibleFramework,
	IncompatibleSdk,
	MissingDependency,
	IncompatibleDependencyVersion,
	CircularDependency,
	DependencyFailed,
	MountFailed,
	PermissionDenied,
	EntryPointMissing,
	EntryPointInvalid,
	ActivationFailed,
	ProviderError,
	ConflictRejected,
	Disabled,
	Internal
};

/** Which side of a network session a mod is meaningful on. */
UENUM(BlueprintType)
enum class EModNetworkScope : uint8
{
	ClientAndServer,
	ClientOnly,
	ServerOnly
};

/** How two mods claiming the same resource are reconciled. */
UENUM(BlueprintType)
enum class EModConflictPolicy : uint8
{
	Error,
	FirstWins,
	LastWins,
	Priority,
	Merge
};

/** Result of evaluating one permission request of one mod. */
UENUM(BlueprintType)
enum class EModPermissionState : uint8
{
	NotRequested,
	Pending,
	Granted,
	Denied
};

/** Severity of an FModDiagnostic. */
UENUM(BlueprintType)
enum class EModDiagnosticSeverity : uint8
{
	Info,
	Warning,
	Error
};

/** Packaging technology backing one content root of a mod. */
UENUM(BlueprintType)
enum class EModContentRootType : uint8
{
	Pak,
	IoStore,
	LooseDirectory
};

/** Stable machine identifier for a mod. Never use display names as identifiers. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModId
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FName Value;

	FModId() = default;
	explicit FModId(FName InValue) : Value(InValue) {}

	/**
	 * Lowercases and validates. Returns false + reason if the id is malformed.
	 * Surrounding whitespace is *not* trimmed: it is reported as an invalid character so that a
	 * mod author gets a precise diagnostic instead of a silently different id.
	 */
	static bool TryParse(const FString& InString, FModId& OutId, FString& OutError);

	/** Lowercases without validating. Prefer TryParse for untrusted input. */
	static FModId FromString(const FString& InString);

	/**
	 * Validity rules: 3..128 chars, starts [a-z0-9], contains only [a-z0-9._-], no '..',
	 * no trailing separator.
	 * OutError names the offending character and its index when the id is rejected, and is emptied
	 * when the id is accepted.
	 */
	static bool IsValidIdString(const FString& InString, FString& OutError);

	bool IsValid() const { return !Value.IsNone(); }
	FString ToString() const { return Value.ToString(); }
	void Reset() { Value = NAME_None; }

	bool operator==(const FModId& Other) const { return Value == Other.Value; }
	bool operator!=(const FModId& Other) const { return Value != Other.Value; }
	bool operator<(const FModId& Other) const { return Value.LexicalLess(Other.Value); }

	friend uint32 GetTypeHash(const FModId& In) { return GetTypeHash(In.Value); }
};

/** A structured message produced by validation, resolution, mounting or conflict detection. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModDiagnostic
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModDiagnosticSeverity Severity = EModDiagnosticSeverity::Error;

	/** Stable machine-readable code, e.g. "Manifest.MissingField". */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName Code;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString Message;

	/** Optional context: file path, field name, mod id... */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString Context;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId ModId;

	FModDiagnostic() = default;
	FModDiagnostic(EModDiagnosticSeverity InSeverity, FName InCode, FString InMessage, FString InContext = FString());

	bool IsError() const { return Severity == EModDiagnosticSeverity::Error; }

	/** "[Error] Manifest.MissingField: <message> (<context>)" */
	FString ToString() const;

	static FModDiagnostic Error(FName Code, FString Message, FString Context = FString());
	static FModDiagnostic Warning(FName Code, FString Message, FString Context = FString());
	static FModDiagnostic Info(FName Code, FString Message, FString Context = FString());
};

/** Helpers used everywhere. */
namespace ModDiagnostics
{
	/** True when at least one diagnostic has EModDiagnosticSeverity::Error. */
	MODFRAMEWORK_API bool HasErrors(const TArray<FModDiagnostic>& In);

	/** Concatenates FModDiagnostic::ToString() of every entry. A null Separator means "\n". */
	MODFRAMEWORK_API FString Join(const TArray<FModDiagnostic>& In, const TCHAR* Separator = TEXT("\n"));

	/** Writes every diagnostic to LogModFramework at the verbosity matching its severity. */
	MODFRAMEWORK_API void LogAll(const TArray<FModDiagnostic>& In);
}

/**
 * Conversion helpers for the framework enums.
 *
 * ToString returns the *identifier* of the enumerator ("DependenciesResolved"), never its
 * UMETA display name, so the result is stable and round-trips through the Parse counterparts.
 * An out-of-range value (which untrusted deserialization can produce) yields "Unknown(<n>)".
 *
 * The implementations are plain switches: they never touch the reflection system, so they are safe
 * to call before UObject registration completes and during module shutdown.
 */
namespace ModFrameworkEnums
{
	MODFRAMEWORK_API FString ToString(EModState In);
	MODFRAMEWORK_API FString ToString(EModLoadFailureReason In);
	MODFRAMEWORK_API FString ToString(EModNetworkScope In);
	MODFRAMEWORK_API FString ToString(EModConflictPolicy In);
	MODFRAMEWORK_API FString ToString(EModPermissionState In);
	MODFRAMEWORK_API FString ToString(EModContentRootType In);
	MODFRAMEWORK_API FString ToString(EModDiagnosticSeverity In);

	/**
	 * Case-insensitive parsers for the enums a mod manifest may contain. Out is left untouched when
	 * parsing fails. Besides the enumerator identifiers these accept a small set of documented
	 * aliases:
	 *   EModNetworkScope    : "Both"/"Any" -> ClientAndServer, "Client" -> ClientOnly, "Server" -> ServerOnly
	 *   EModConflictPolicy  : "First" -> FirstWins, "Last" -> LastWins
	 *   EModContentRootType : "Loose"/"Directory" -> LooseDirectory
	 * Surrounding whitespace is ignored.
	 */
	MODFRAMEWORK_API bool ParseNetworkScope(const FString& In, EModNetworkScope& Out);
	MODFRAMEWORK_API bool ParseConflictPolicy(const FString& In, EModConflictPolicy& Out);
	MODFRAMEWORK_API bool ParseContentRootType(const FString& In, EModContentRootType& Out);

	/** Overloaded spellings of the three parsers above, for call sites that prefer overloading. */
	MODFRAMEWORK_API bool Parse(const FString& In, EModNetworkScope& Out);
	MODFRAMEWORK_API bool Parse(const FString& In, EModConflictPolicy& Out);
	MODFRAMEWORK_API bool Parse(const FString& In, EModContentRootType& Out);
}
