// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class FJsonObject;
class FProperty;
class UClass;
class UEnum;
class UField;
class UFunction;
class UScriptStruct;
class UStruct;

/**
 * The reflection metadata keys that describe a game's public modding surface.
 *
 * Normative table: docs/PublicAPIMarking.md, mirrored in ImplementationContract.md section 25.
 * `ModPublic` is a *flag*: UHT emits it with an empty value, so presence must be tested with
 * HasMetaData and never by comparing the value against a non-empty string.
 */
namespace ModPublicApiMetadata
{
	/** Marks a class, struct, enum, interface, function or property for inclusion in the SDK. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ModPublic;

	/** UModAPI subclass: the stable id mods use to request the API. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ModApiId;

	/** UModAPI subclass: semver of the API surface. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ModApiVersion;

	/** UModAPI subclass: comma separated permission ids required to obtain the API. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ModApiPermissions;

	/** UModAPI subclass: "true" marks the API server authoritative. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ModApiServerAuthoritative;

	/** UModExtension subclass: the extension point id this base class serves. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ModExtensionPoint;

	/** Any symbol: the SDK version the symbol first appeared in. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ModSince;

	/** Any symbol: the SDK version the symbol was deprecated in. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ModDeprecated;

	/** UHT-emitted: the header path relative to the module's Source/<Module> directory. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ModuleRelativePath;

	/** UHT-emitted on classes: the path an #include directive would name. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const IncludePath;
}

/** Which reflection category a scanned symbol came from. */
enum class EModPublicSymbolKind : uint8
{
	Class,
	Interface,
	Struct,
	Enum
};

/** Stable machine-readable code of every diagnostic the scanner can emit. */
namespace ModPublicApiCodes
{
	/** Metadata is compiled out in this build configuration; the scan produced nothing. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const MetadataUnavailable;

	/** A UModAPI subclass carries no ModApiId. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ApiMissingId;

	/** A UModAPI subclass declares a ModApiId its NativeGetApiId override does not agree with. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ApiIdentityMismatch;

	/** A UModAPI subclass declares metadata identity but no native override to survive cooking. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ApiIdentityNotNative;

	/** A ModApiVersion / ModSince / ModDeprecated value is not parseable semver. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const InvalidVersion;

	/** A UModExtension subclass carries no ModExtensionPoint. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ExtensionMissingPoint;

	/** A ModPublic class has BlueprintCallable functions and none of them is marked. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const NoMarkedMembers;

	/** Two ModPublic APIs claim the same ModApiId. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const DuplicateApiId;

	/**
	 * A marked symbol names a type that will not exist in a mod author's project. This is the
	 * failure that makes a generated SDK refuse to compile for the person receiving it.
	 */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const UnmarkedTypeLeak;

	/** The report could not be serialised or written to disk. */
	MODFRAMEWORKDEVELOPER_API extern const TCHAR* const ReportWriteFailed;
}

/**
 * The Mod* metadata block read off one symbol.
 *
 * Every field is "unset" when empty; nothing here is defaulted to a guess, because a guessed api id
 * silently becomes part of a published compatibility contract.
 */
struct MODFRAMEWORKDEVELOPER_API FModPublicMetadata
{
	/** ModApiId, verbatim. Lowercased by UModAPI at runtime; NOT lowercased here. */
	FString ApiId;

	/** ModApiVersion, verbatim. */
	FString ApiVersion;

	/** ModApiPermissions, split on commas and trimmed. */
	TArray<FString> ApiPermissions;

	/** Value of ModApiServerAuthoritative, parsed. Only meaningful when bHasServerAuthoritative. */
	bool bServerAuthoritative = false;

	/** True when ModApiServerAuthoritative was present at all - "absent" and "false" differ. */
	bool bHasServerAuthoritative = false;

	/** ModExtensionPoint, verbatim. */
	FString ExtensionPointId;

	/** ModSince, verbatim. */
	FString Since;

	/** ModDeprecated, verbatim. Non-empty means the symbol is deprecated. */
	FString Deprecated;

	bool IsEmpty() const;

	bool IsDeprecated() const { return !Deprecated.IsEmpty(); }
};

/** One reflected parameter of a scanned function. */
struct MODFRAMEWORKDEVELOPER_API FModPublicParameterInfo
{
	FString Name;

	/** The C++ spelling, e.g. "AActor*" or "TArray<FName>". */
	FString CppType;

	bool bReturnValue = false;
	bool bOutParameter = false;
	bool bConstReference = false;
};

/** One reflected UFUNCTION of a scanned type. */
struct MODFRAMEWORKDEVELOPER_API FModPublicFunctionInfo
{
	FString Name;

	/** Path name of the declaring type, so an inherited member is attributable. */
	FString OwnerPath;

	/** Rendered declaration, e.g. "bool ApplyDamage(AActor* Target, float Amount, FName DamageType)". */
	FString Signature;

	FString ReturnCppType;

	TArray<FModPublicParameterInfo> Parameters;

	bool bMarkedPublic = false;
	bool bBlueprintCallable = false;
	bool bBlueprintPure = false;
	bool bBlueprintEvent = false;
	bool bBlueprintImplementable = false;
	bool bStatic = false;
	bool bNative = false;

	/** Module-relative header the function was declared in. */
	FString HeaderPath;

	FModPublicMetadata Metadata;
};

/** One reflected UPROPERTY of a scanned type. */
struct MODFRAMEWORKDEVELOPER_API FModPublicPropertyInfo
{
	FString Name;

	/** Path name of the declaring type. */
	FString OwnerPath;

	/** The C++ spelling of the property's type. */
	FString CppType;

	bool bMarkedPublic = false;
	bool bBlueprintVisible = false;
	bool bBlueprintReadOnly = false;
	bool bEditable = false;
	bool bConfig = false;

	/** Module-relative header the property was declared in. */
	FString HeaderPath;

	FModPublicMetadata Metadata;
};

/** One scanned class, interface, struct or enum. */
struct MODFRAMEWORKDEVELOPER_API FModPublicTypeInfo
{
	EModPublicSymbolKind Kind = EModPublicSymbolKind::Class;

	/** Reflection name without the C++ prefix, e.g. "GameCombatModAPI". */
	FString Name;

	/** Full object path, e.g. "/Script/GameModSDK.GameCombatModAPI". */
	FString PathName;

	/** Owning module, e.g. "GameModSDK". For a Blueprint type this is the package path instead. */
	FString ModuleName;

	/** ModuleRelativePath metadata, e.g. "Public/API/GameCombatModAPI.h". Empty when unknown. */
	FString HeaderPath;

	/** IncludePath metadata (classes only), e.g. "API/GameCombatModAPI.h". Empty when unknown. */
	FString IncludePath;

	/** Reflection name of the immediate super, empty for a root type. */
	FString SuperName;

	/** Path name of the immediate super. */
	FString SuperPath;

	bool bIsModAPI = false;
	bool bIsModExtension = false;
	bool bIsBlueprintType = false;
	bool bIsBlueprintable = false;
	bool bIsAbstract = false;
	bool bIsNative = true;

	FModPublicMetadata Metadata;

	/** Declared on this type only; inherited members belong to the super's entry. */
	TArray<FModPublicFunctionInfo> Functions;
	TArray<FModPublicPropertyInfo> Properties;

	/** Enumerator identifiers, in declaration order. Enums only. */
	TArray<FString> Enumerators;

	/** Marked functions, i.e. the ones that will appear in the generated SDK. */
	int32 CountMarkedFunctions() const;

	/** Marked properties. */
	int32 CountMarkedProperties() const;
};

/** One entry of the API index written into SDKVersion.json. */
struct MODFRAMEWORKDEVELOPER_API FModPublicApiEntry
{
	FString ApiId;
	FString Version;
	TArray<FString> RequiredPermissions;
	bool bServerAuthoritative = false;

	FString ClassName;
	FString ClassPath;
	FString ModuleName;
	FString HeaderPath;

	/**
	 * True when the class overrides NativeGetApiId, i.e. its identity survives cooking.
	 * False means the id below exists only in editor metadata; see the hazard note on UModAPI.
	 */
	bool bNativeIdentity = false;

	/** What NativeGetApiId actually returned, so a disagreement with ApiId is visible in the index. */
	FString NativeApiId;
};

/** One entry of the extension point index written into SDKVersion.json. */
struct MODFRAMEWORKDEVELOPER_API FModPublicExtensionPointEntry
{
	FString ExtensionPointId;
	FString BaseClassName;
	FString BaseClassPath;
	FString ModuleName;
	FString HeaderPath;
	bool bAbstract = false;
	bool bBlueprintable = false;
};

/** Everything one scan found. */
struct MODFRAMEWORKDEVELOPER_API FModPublicApiReport
{
	TArray<FModPublicTypeInfo> Classes;
	TArray<FModPublicTypeInfo> Interfaces;
	TArray<FModPublicTypeInfo> Structs;
	TArray<FModPublicTypeInfo> Enums;

	/** Derived index of every ModPublic UModAPI subclass. */
	TArray<FModPublicApiEntry> Apis;

	/** Derived index of every ModPublic UModExtension subclass carrying ModExtensionPoint. */
	TArray<FModPublicExtensionPointEntry> ExtensionPoints;

	/** Every module that contributed at least one marked symbol, sorted. */
	TArray<FString> Modules;

	/** Warnings and errors. Never fatal: a scan always returns a report. */
	TArray<FModDiagnostic> Diagnostics;

	/** How many reflected types were examined, marked or not. */
	int32 TypesExamined = 0;

	/** Wall time of the scan, seconds. */
	double ScanSeconds = 0.0;

	/** False when metadata was compiled out, so an empty report means "could not look", not "nothing". */
	bool bMetadataAvailable = true;

	int32 GetTypeCount() const;
	int32 GetFunctionCount() const;
	int32 GetPropertyCount() const;

	bool HasErrors() const;
	bool HasWarnings() const;

	/** Nullptr when the path is not in the report. Searches classes, interfaces, structs and enums. */
	const FModPublicTypeInfo* FindType(const FString& InPathName) const;

	/** One line per counted category, for logs and the commandlet's summary. */
	FString DescribeCounts() const;
};

/** Knobs for one scan. The defaults are what SDK generation wants. */
struct MODFRAMEWORKDEVELOPER_API FModPublicApiScanOptions
{
	/**
	 * Only scan symbols from these modules. Empty means every loaded module.
	 *
	 * Scanning everything is the honest default: a game may mark types in its own module, in its SDK
	 * plugin, or in a shared plugin, and a scan that silently skipped one would report a smaller
	 * surface than the game actually publishes.
	 */
	TArray<FString> ModuleAllowList;

	/** Never scan symbols from these modules, even when the allow list is empty. */
	TArray<FString> ModuleDenyList;

	/**
	 * Plugins whose source ships inside the generated SDK bundle. Every module of these plugins is
	 * reachable by a mod author, so an unmarked type from one of them is not a leak.
	 *
	 * The generator adds the SDK plugin to this list; ModFramework is always included.
	 */
	TArray<FString> ShippingPluginNames;

	/** Include Blueprint-generated classes. Off: an SDK is a C++ surface, and BP classes are content. */
	bool bIncludeBlueprintGeneratedTypes = false;

	/** Emit ApiMissingId / ExtensionMissingPoint warnings. */
	bool bWarnOnMissingIdentity = true;

	/** Emit ApiIdentityNotNative / ApiIdentityMismatch warnings by inspecting each API's CDO. */
	bool bWarnOnNonNativeIdentity = true;

	/** Emit NoMarkedMembers warnings. */
	bool bWarnOnUnmarkedMembers = true;

	/** Emit UnmarkedTypeLeak errors. Turning this off hides the single most damaging mistake. */
	bool bDetectUnmarkedTypeLeaks = true;
};

/**
 * Walks the loaded reflection data and reports every symbol a game marked as part of its public
 * modding surface.
 *
 * ---------------------------------------------------------------------------------------------
 * WHY THIS IS EDITOR-TIME TOOLING
 * ---------------------------------------------------------------------------------------------
 * Reflection metadata is editor-only data: `WITH_METADATA` is defined as `WITH_EDITORONLY_DATA`
 * (Core/Public/Misc/CoreMiscDefines.h), so in a cooked Shipping or Test build UField::FindMetaData
 * does not even exist and every ModPublic / ModApi* key is simply gone. That is fine here - this
 * class lives in ModFrameworkDeveloper, an UncookedOnly module that is never part of a shipped
 * game - but it is exactly why UModAPI resolves its runtime identity through NativeGetApiId first
 * and only falls back to metadata. See the hazard note at the top of ModAPI.h.
 *
 * The scanner therefore cross-checks the two: an API whose metadata declares an id that its native
 * override does not agree with will register under one id in the editor and a different one in the
 * shipped game, which is the defect that only surfaces after mods have already been published.
 *
 * When metadata is unavailable the scan does not assert or return garbage: it returns an empty
 * report with bMetadataAvailable false and a single MetadataUnavailable diagnostic.
 *
 * Everything read here is authored data, including data authored by someone else's plugin. Nothing
 * in this class checks() on it.
 */
class MODFRAMEWORKDEVELOPER_API FModPublicApiScanner
{
public:
	/** Runs a scan. Never fails: check bMetadataAvailable and Diagnostics on the result. */
	static FModPublicApiReport Scan(const FModPublicApiScanOptions& InOptions);

	/** Convenience overload using default options. */
	static FModPublicApiReport Scan();

	//~ Metadata access ---------------------------------------------------------------------------
	// These compile in every configuration and return "nothing found" when metadata is unavailable,
	// so callers never need their own WITH_METADATA guards.

	/** True when metadata exists at all in this build configuration. */
	static bool IsMetadataAvailable();

	/** True when InField carries InKey, whatever its value. ModPublic is stored with an empty value. */
	static bool HasFieldMetadata(const UField* InField, const TCHAR* InKey);

	/** InField's value for InKey, or an empty string. */
	static FString GetFieldMetadata(const UField* InField, const TCHAR* InKey);

	/** True when InProperty carries InKey. */
	static bool HasPropertyMetadata(const FProperty* InProperty, const TCHAR* InKey);

	/** InProperty's value for InKey, or an empty string. */
	static FString GetPropertyMetadata(const FProperty* InProperty, const TCHAR* InKey);

	/** True when InField is marked ModPublic. */
	static bool IsMarkedPublic(const UField* InField);

	/** True when InProperty is marked ModPublic. */
	static bool IsMarkedPublic(const FProperty* InProperty);

	/** Reads the whole Mod* block off a UField. */
	static FModPublicMetadata ReadMetadata(const UField* InField);

	/** Reads the whole Mod* block off an FProperty. */
	static FModPublicMetadata ReadMetadata(const FProperty* InProperty);

	//~ Helpers -----------------------------------------------------------------------------------

	/** "GameModSDK" for /Script/GameModSDK.Foo; the package path for a Blueprint type. */
	static FString GetOwningModuleName(const UField* InField);

	/** Every module name declared by InPluginName's descriptor. Empty when the plugin is unknown. */
	static TArray<FString> GetPluginModuleNames(const FString& InPluginName);

	/**
	 * True when a mod author's project will have this module.
	 *
	 * Engine modules and engine-plugin modules always qualify. A project module qualifies only when
	 * it belongs to one of InShippingModules, which the caller derives from the plugins the bundle
	 * actually copies.
	 */
	static bool IsModuleAvailableToModAuthors(const FString& InModuleName, const TSet<FString>& InShippingModules);

	/** JSON object holding the API index, the extension point index and the symbol counts. */
	static TSharedRef<FJsonObject> BuildApiIndexJson(const FModPublicApiReport& InReport);

	/** The full report as pretty-printed JSON. Returns false only when serialisation itself fails. */
	static bool SerializeReportToString(const FModPublicApiReport& InReport, FString& OutJson);

	/** Writes SerializeReportToString to disk, creating parent directories. */
	static bool WriteReportToFile(const FModPublicApiReport& InReport, const FString& InAbsolutePath,
		FModDiagnostic& OutError);
};
