# ModFramework — Binding Implementation Contract

**Every agent MUST read this file completely before writing code, and MUST NOT deviate from any
signature, type name, file path, enum value, or include path declared here.** Other agents are
writing code against these exact declarations in parallel. If a declaration here seems wrong,
implement it anyway and note the concern in your return value.

---

## 0. Environment & house style

- Unreal Engine **5.8.1** (`"EngineVersion": "5.8.0"` in the .uplugin). Windows. C++20.
- Repo root == the plugin root: `F:/SelfProjects/Unreal/Plugins/ModFramework`
- Every `.h`, `.cpp` and `.cs` file starts with exactly this line, then a blank line:
  `// Copyright (c) 2026. Licensed for use in your own projects.`
- Headers use `#pragma once`.
- Tabs for indentation in `.cs` and `.json`. Tabs in `.cpp`/`.h` too (Epic style).
- Allman braces, Epic naming (`b` prefix for bools, `F`/`U`/`E`/`I` type prefixes).
- `PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;` and `IWYUSupport = IWYUSupport.Full;`
  in every Build.cs. That means **include what you use** — no reliance on transitive includes.
- Log with the category `LogModFramework` (declared in `Core/ModFrameworkLog.h`).
- Never use `check()` on data that comes from a mod. Mod input is untrusted: validate and return
  diagnostics instead of asserting.
- All user-facing strings that a game might localize use `FText`; identifiers/ids use `FName`;
  paths and machine-readable strings use `FString`.
- Do not add third-party dependencies. Only engine modules.

### Module API macros
| Module | Macro | Type |
|---|---|---|
| `ModFramework` | `MODFRAMEWORK_API` | Runtime, LoadingPhase `PostConfigInit` |
| `ModFrameworkDeveloper` | `MODFRAMEWORKDEVELOPER_API` | UncookedOnly, LoadingPhase `Default` |
| `ModFrameworkEditor` | `MODFRAMEWORKEDITOR_API` | Editor, LoadingPhase `Default` |

### Include paths
`Source/ModFramework/Public` is the public include root, so headers are included as
`#include "Core/ModFrameworkTypes.h"`, `#include "Manifest/ModManifest.h"`, etc. **Never** use
relative `../` includes. Private headers live under `Source/ModFramework/Private/...` and are
included as `#include "Content/ModPakContentMounter.h"` (the Private root is also on the path for
that module only).

---

## 1. File map (authoritative)

```
ModFramework.uplugin
Source/ModFramework/ModFramework.Build.cs
Source/ModFramework/Public/ModFrameworkModule.h
Source/ModFramework/Private/ModFrameworkModule.cpp

Public/Core/ModFrameworkLog.h
Public/Core/ModFrameworkVersion.h
Public/Core/ModFrameworkTypes.h          (enums + FModDiagnostic + FModId)
Private/Core/ModFrameworkTypes.cpp

Public/Manifest/ModVersion.h             (FModVersion, FModVersionRange)
Private/Manifest/ModVersion.cpp
Public/Manifest/ModManifest.h            (FModManifest + sub-structs)
Private/Manifest/ModManifest.cpp
Public/Manifest/ModManifestParser.h      (FModManifestParser)
Private/Manifest/ModManifestParser.cpp

Public/Settings/ModFrameworkSettings.h
Private/Settings/ModFrameworkSettings.cpp

Public/Registry/ModInfo.h                (FModInfo)
Public/Registry/ModRegistry.h            (UModRegistry)
Private/Registry/ModRegistry.cpp

Public/API/ModAPI.h                      (UModAPI, FModAPIDescriptor)
Private/API/ModAPI.cpp
Public/API/ModAPIRegistry.h              (UModAPIRegistry)
Private/API/ModAPIRegistry.cpp

Public/Extensions/ModExtension.h         (UModExtension, FModExtensionPointDescriptor)
Private/Extensions/ModExtension.cpp
Public/Extensions/ModExtensionRegistry.h (UModExtensionRegistry)
Private/Extensions/ModExtensionRegistry.cpp
Public/Extensions/ModContentBundle.h     (UModContentBundle : UPrimaryDataAsset)
Private/Extensions/ModContentBundle.cpp

Public/Events/ModEventTypes.h            (FModEventPayload, FModEventContext, FModEventDescriptor, ModFrameworkEvents:: names)
Private/Events/ModEventTypes.cpp
Public/Events/ModEventBus.h              (UModEventBus)
Private/Events/ModEventBus.cpp

Public/Permissions/ModPermissions.h      (FModPermissionDescriptor, ModPermissions:: names, IModPermissionPolicy)
Private/Permissions/ModPermissions.cpp
Public/Permissions/ModPermissionRegistry.h
Private/Permissions/ModPermissionRegistry.cpp

Public/Content/ModContentTypes.h         (FModContentMount, IModContentMounter)
Public/Content/ModContentManager.h       (FModContentManager)
Private/Content/ModContentManager.cpp
Private/Content/ModPakContentMounter.h / .cpp
Private/Content/ModLooseContentMounter.h / .cpp
Public/Content/ModAssetLibrary.h         (UModAssetLibrary : UBlueprintFunctionLibrary)
Private/Content/ModAssetLibrary.cpp

Public/Providers/ModProvider.h           (IModProvider, FModDiscoveryResult)
Public/Providers/LocalFileModProvider.h  (FLocalFileModProvider)
Private/Providers/LocalFileModProvider.cpp

Public/Packaging/ModPackageFormat.h      (FModPackageHeader, FModPackageEntry, FModPackageReader)
Private/Packaging/ModPackageFormat.cpp

Public/Dependencies/ModDependencyTypes.h (FModEnvironment, FModResolveRequest, FModResolveResult, FModRejection)
Public/Dependencies/ModDependencyResolver.h
Private/Dependencies/ModDependencyResolver.cpp

Public/Conflicts/ModConflictTypes.h
Public/Conflicts/ModConflictDetector.h
Private/Conflicts/ModConflictDetector.cpp

Public/Save/ModSaveTypes.h               (FModSaveRecord, FModSaveEnvelope, FModSaveDependency)
Public/Save/ModSaveDataManager.h         (UModSaveDataManager)
Private/Save/ModSaveDataManager.cpp
Public/Save/ModSaveGame.h                (UModSaveGame : USaveGame)
Private/Save/ModSaveGame.cpp

Public/Net/ModSessionManifest.h
Public/Net/ModNetworkValidator.h
Private/Net/ModNetworkValidator.cpp
Public/Net/ModNetworkStatics.h           (UModNetworkStatics : UBlueprintFunctionLibrary)
Private/Net/ModNetworkStatics.cpp

Public/Runtime/ModContext.h              (UModContext)
Private/Runtime/ModContext.cpp
Public/Runtime/ModEntryPointBase.h       (UModEntryPointBase)
Private/Runtime/ModEntryPointBase.cpp

Public/Subsystem/ModSubsystem.h          (UModSubsystem)
Private/Subsystem/ModSubsystem.cpp

Private/Debug/ModConsoleCommands.h / .cpp
Private/Tests/*.spec.cpp                 (WITH_DEV_AUTOMATION_TESTS)
```

---

## 2. `Core/ModFrameworkLog.h`

```cpp
#pragma once
#include "Logging/LogMacros.h"
MODFRAMEWORK_API DECLARE_LOG_CATEGORY_EXTERN(LogModFramework, Log, All);
```
Defined in `ModFrameworkModule.cpp` with `DEFINE_LOG_CATEGORY(LogModFramework);`

## 3. `Core/ModFrameworkVersion.h`

```cpp
#define MODFRAMEWORK_VERSION_MAJOR 0
#define MODFRAMEWORK_VERSION_MINOR 1
#define MODFRAMEWORK_VERSION_PATCH 0
#define MODFRAMEWORK_MANIFEST_VERSION 1
#define MODFRAMEWORK_PACKAGE_FORMAT_VERSION 1

namespace ModFrameworkVersion
{
	/** Semantic version of the framework itself. */
	MODFRAMEWORK_API FModVersion Get();
	MODFRAMEWORK_API FString GetString();
}
```
(Include `Manifest/ModVersion.h`.)

## 4. `Core/ModFrameworkTypes.h`

Exact enums — do not rename or reorder (values are serialized by name, but code switches on them):

```cpp
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

UENUM(BlueprintType)
enum class EModNetworkScope : uint8
{
	ClientAndServer,
	ClientOnly,
	ServerOnly
};

UENUM(BlueprintType)
enum class EModConflictPolicy : uint8
{
	Error,
	FirstWins,
	LastWins,
	Priority,
	Merge
};

UENUM(BlueprintType)
enum class EModPermissionState : uint8
{
	NotRequested,
	Pending,
	Granted,
	Denied
};

UENUM(BlueprintType)
enum class EModDiagnosticSeverity : uint8
{
	Info,
	Warning,
	Error
};

UENUM(BlueprintType)
enum class EModContentRootType : uint8
{
	Pak,
	IoStore,
	LooseDirectory
};
```

```cpp
/** Stable machine identifier for a mod. Never use display names as identifiers. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModId
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FName Value;

	FModId() = default;
	explicit FModId(FName InValue) : Value(InValue) {}

	/** Lowercases and validates. Returns false + reason if the id is malformed. */
	static bool TryParse(const FString& InString, FModId& OutId, FString& OutError);
	/** Lowercases without validating. Prefer TryParse for untrusted input. */
	static FModId FromString(const FString& InString);
	/** Validity rules: 3..128 chars, starts [a-z0-9], contains only [a-z0-9._-], no '..', no trailing separator. */
	static bool IsValidIdString(const FString& InString, FString& OutError);

	bool IsValid() const { return !Value.IsNone(); }
	FString ToString() const { return Value.ToString(); }
	void Reset() { Value = NAME_None; }

	bool operator==(const FModId& Other) const { return Value == Other.Value; }
	bool operator!=(const FModId& Other) const { return Value != Other.Value; }
	bool operator<(const FModId& Other) const { return Value.LexicalLess(Other.Value); }

	friend uint32 GetTypeHash(const FModId& In) { return GetTypeHash(In.Value); }
};
```

```cpp
/** A structured message produced by validation, resolution, mounting or conflict detection. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModDiagnostic
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mod") EModDiagnosticSeverity Severity = EModDiagnosticSeverity::Error;
	/** Stable machine-readable code, e.g. "Manifest.MissingField". */
	UPROPERTY(BlueprintReadOnly, Category = "Mod") FName Code;
	UPROPERTY(BlueprintReadOnly, Category = "Mod") FString Message;
	/** Optional context: file path, field name, mod id... */
	UPROPERTY(BlueprintReadOnly, Category = "Mod") FString Context;
	UPROPERTY(BlueprintReadOnly, Category = "Mod") FModId ModId;

	FModDiagnostic() = default;
	FModDiagnostic(EModDiagnosticSeverity InSeverity, FName InCode, FString InMessage, FString InContext = FString());

	bool IsError() const { return Severity == EModDiagnosticSeverity::Error; }
	FString ToString() const;   // "[Error] Manifest.MissingField: <message> (<context>)"

	static FModDiagnostic Error(FName Code, FString Message, FString Context = FString());
	static FModDiagnostic Warning(FName Code, FString Message, FString Context = FString());
	static FModDiagnostic Info(FName Code, FString Message, FString Context = FString());
};

/** Helpers used everywhere. */
namespace ModDiagnostics
{
	MODFRAMEWORK_API bool HasErrors(const TArray<FModDiagnostic>& In);
	MODFRAMEWORK_API FString Join(const TArray<FModDiagnostic>& In, const TCHAR* Separator = TEXT("\n"));
	MODFRAMEWORK_API void LogAll(const TArray<FModDiagnostic>& In);
}
```

## 5. `Manifest/ModVersion.h`

```cpp
/** Semantic Versioning 2.0.0 version. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModVersion
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") int32 Major = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") int32 Minor = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") int32 Patch = 0;
	/** Dot separated pre-release identifiers without the leading '-'. Empty = release. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FString PreRelease;
	/** Build metadata without the leading '+'. Ignored for ordering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FString BuildMetadata;

	FModVersion() = default;
	FModVersion(int32 InMajor, int32 InMinor, int32 InPatch, FString InPreRelease = FString(), FString InBuild = FString());

	static bool Parse(const FString& In, FModVersion& Out, FString* OutError = nullptr);
	static FModVersion FromString(const FString& In);   // returns 0.0.0 on failure
	FString ToString() const;

	bool IsZero() const;
	bool IsPreRelease() const { return !PreRelease.IsEmpty(); }

	/** Full semver precedence. BuildMetadata is ignored. */
	int32 Compare(const FModVersion& Other) const;
	bool operator==(const FModVersion& O) const; // ignores BuildMetadata
	bool operator!=(const FModVersion& O) const;
	bool operator<(const FModVersion& O) const;
	bool operator<=(const FModVersion& O) const;
	bool operator>(const FModVersion& O) const;
	bool operator>=(const FModVersion& O) const;

	friend uint32 GetTypeHash(const FModVersion& In);
};

UENUM()
enum class EModVersionOp : uint8 { Equal, NotEqual, Greater, GreaterEqual, Less, LessEqual };

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModVersionComparator
{
	GENERATED_BODY()
	UPROPERTY() EModVersionOp Op = EModVersionOp::GreaterEqual;
	UPROPERTY() FModVersion Version;
	bool Satisfies(const FModVersion& In) const;
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
 * pre-release with the same major.minor.patch (npm rule) — keeps 2.0.0-rc1 out of ">=1.0.0".
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModVersionRange
{
	GENERATED_BODY()

	/** The original text, preserved for diagnostics and round-tripping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FString Expression;

	static bool Parse(const FString& In, FModVersionRange& Out, FString* OutError = nullptr);
	static FModVersionRange Any();
	static FModVersionRange Exactly(const FModVersion& In);

	bool IsAny() const;
	bool IsValid() const;                  // parsed successfully
	bool Satisfies(const FModVersion& In) const;
	FString ToString() const { return Expression; }
	FString Describe() const;              // human sentence, e.g. ">= 1.2.3 and < 2.0.0"

	// Parsed form; rebuilt lazily from Expression when needed.
	UPROPERTY() TArray<FModVersionClause> Clauses;   // OR of clauses
	UPROPERTY() bool bParsed = false;
	UPROPERTY() bool bParseFailed = false;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModVersionClause     // AND of comparators
{
	GENERATED_BODY()
	UPROPERTY() TArray<FModVersionComparator> Comparators;
	bool Satisfies(const FModVersion& In) const;
	FString ToString() const;
};
```
Note ordering: declare `FModVersionComparator` and `FModVersionClause` *before* `FModVersionRange`.

## 6. `Manifest/ModManifest.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModDependency
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FModId Id;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FModVersionRange VersionRange;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") bool bOptional = false;
	/** Human explanation shown when the dependency cannot be satisfied. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FString Reason;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModGameRequirement
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FString GameId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FModVersionRange VersionRange;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSdkRequirement
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FString SdkId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FModVersionRange VersionRange;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModContentRoot
{
	GENERATED_BODY()
	/** Path relative to the mod root, e.g. "Content/MyMod.pak" or "Content". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FString RelativePath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") EModContentRootType Type = EModContentRootType::Pak;
	/** Virtual root the content mounts under. Defaults to "/" + <mod root folder name>. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FString MountPoint;
	/** Pak mount order; higher wins for identical paths. Clamped by the framework. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") int32 MountOrder = 0;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEntryPoint
{
	GENERATED_BODY()
	/** Optional native module name (advanced; not loaded by the MVP). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FString NativeModule;
	/** Blueprint or native class deriving from UModEntryPointBase. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FSoftClassPath EntryClass;
	/** Content bundles (UModContentBundle) loaded when the mod activates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") TArray<FSoftObjectPath> ContentBundles;
};

/** What this mod declares it will modify, for conflict detection. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModResourceClaimDeclaration
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FName ExtensionPointId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FName ResourceId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") EModConflictPolicy PreferredPolicy = EModConflictPolicy::Error;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModManifest
{
	GENERATED_BODY()

	UPROPERTY(...) int32 ManifestVersion = MODFRAMEWORK_MANIFEST_VERSION;
	UPROPERTY(...) FModId Id;
	UPROPERTY(...) FString DisplayName;
	UPROPERTY(...) FString Description;
	UPROPERTY(...) FString Author;
	UPROPERTY(...) FString Homepage;
	UPROPERTY(...) FString License;
	UPROPERTY(...) FModVersion Version;
	UPROPERTY(...) FModGameRequirement Game;
	UPROPERTY(...) FModVersionRange FrameworkVersionRange;
	UPROPERTY(...) FModSdkRequirement Sdk;
	UPROPERTY(...) TArray<FModDependency> Dependencies;      // bOptional distinguishes optional
	UPROPERTY(...) TArray<FModId> LoadBefore;
	UPROPERTY(...) TArray<FModId> LoadAfter;
	UPROPERTY(...) int32 Priority = 0;                        // higher loads earlier among equals
	UPROPERTY(...) EModNetworkScope NetworkScope = EModNetworkScope::ClientAndServer;
	/** If true a server running this mod requires connecting clients to run it too. */
	UPROPERTY(...) bool bRequiredForNetworkPlay = false;
	UPROPERTY(...) TArray<FName> RequestedPermissions;
	UPROPERTY(...) FModEntryPoint EntryPoint;
	UPROPERTY(...) TArray<FModContentRoot> ContentRoots;
	UPROPERTY(...) TArray<FModResourceClaimDeclaration> Claims;
	UPROPERTY(...) TMap<FString, FString> Metadata;

	bool IsValid() const;             // has an Id and a non-zero Version
	FString GetDisplayNameOrId() const;
	/** Required + optional dependency lookup. */
	const FModDependency* FindDependency(const FModId& InId) const;
};
```
`UPROPERTY(...)` above means `UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod")`.

## 7. `Manifest/ModManifestParser.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModManifestParseResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bSuccess = false;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModManifest Manifest;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModDiagnostic> Diagnostics;
};

class MODFRAMEWORK_API FModManifestParser
{
public:
	/** The canonical manifest file name inside a mod root. */
	static const TCHAR* GetManifestFileName();        // "mod.json"

	static FModManifestParseResult ParseFromString(const FString& JsonText, const FString& ContextPath = FString());
	static FModManifestParseResult ParseFromFile(const FString& AbsoluteFilePath);
	static bool SerializeToString(const FModManifest& In, FString& OutJson, bool bPrettyPrint = true);
	static bool SerializeToFile(const FModManifest& In, const FString& AbsoluteFilePath);

	/** Semantic validation independent of JSON shape. Appends to OutDiagnostics. */
	static void ValidateManifest(const FModManifest& In, const FString& ContextPath, TArray<FModDiagnostic>& OutDiagnostics);
};
```

### Canonical `mod.json` shape (the parser must accept exactly this; unknown keys -> Warning `Manifest.UnknownField`)

```json
{
	"manifestVersion": 1,
	"id": "com.example.bettercombat",
	"name": "Better Combat",
	"description": "Rebalances melee combat.",
	"author": "Example Author",
	"homepage": "https://example.com",
	"license": "MIT",
	"version": "2.1.0",
	"game": { "id": "com.example.game", "version": ">=1.5.0 <2.0.0" },
	"framework": { "version": "^0.1.0" },
	"sdk": { "id": "com.example.game.sdk", "version": "^1.5.0" },
	"dependencies": [
		{ "id": "com.example.corelib", "version": ">=2.0.0" },
		{ "id": "com.example.ui", "version": "*", "optional": true, "reason": "Adds a settings page." }
	],
	"loadBefore": ["com.example.late"],
	"loadAfter": ["com.example.early"],
	"priority": 0,
	"network": { "scope": "ClientAndServer", "requiredForNetworkPlay": true },
	"permissions": ["gameplay.modify", "save.modify"],
	"entryPoint": {
		"nativeModule": "",
		"class": "/BetterCombat/BP_BetterCombatEntry.BP_BetterCombatEntry_C",
		"contentBundles": ["/BetterCombat/DA_BetterCombatBundle.DA_BetterCombatBundle"]
	},
	"content": [
		{ "path": "Content/BetterCombat.pak", "type": "Pak", "mountPoint": "/BetterCombat/", "mountOrder": 0 }
	],
	"claims": [
		{ "extensionPoint": "game.weapon", "resource": "weapon.longsword", "policy": "Priority" }
	],
	"metadata": { "category": "Gameplay" }
}
```
Required fields: `id`, `name`, `version`, `game.id`. Everything else optional with the defaults above.
`framework.version` defaults to `*`. Enum strings parse case-insensitively.

## 8. `Settings/ModFrameworkSettings.h`

```cpp
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Mod Framework"))
class MODFRAMEWORK_API UModFrameworkSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UModFrameworkSettings();
	static const UModFrameworkSettings* Get();
	virtual FName GetCategoryName() const override;   // "Plugins"

	UPROPERTY(config, EditAnywhere, Category = "Identity") FString GameId;
	UPROPERTY(config, EditAnywhere, Category = "Identity") FString GameVersion;      // semver string
	UPROPERTY(config, EditAnywhere, Category = "Identity") FString SdkId;
	UPROPERTY(config, EditAnywhere, Category = "Identity") FString SdkVersion;

	/** Directories scanned for mods. Supports {Project}, {ProjectSaved}, {ProjectUser}, {Engine} tokens. */
	UPROPERTY(config, EditAnywhere, Category = "Discovery") TArray<FString> ModSearchDirectories;
	UPROPERTY(config, EditAnywhere, Category = "Discovery") bool bScanCommandLineDirectories = true;
	UPROPERTY(config, EditAnywhere, Category = "Discovery") int32 MaxMods = 512;

	UPROPERTY(config, EditAnywhere, Category = "Loading") bool bAutoDiscoverOnStartup = true;
	UPROPERTY(config, EditAnywhere, Category = "Loading") bool bAutoLoadDiscoveredMods = true;
	UPROPERTY(config, EditAnywhere, Category = "Loading") bool bAutoActivateLoadedMods = true;
	UPROPERTY(config, EditAnywhere, Category = "Loading") TArray<FString> DisabledModIds;

	UPROPERTY(config, EditAnywhere, Category = "Content") bool bAllowLooseContentMounts = false;   // dev only
	UPROPERTY(config, EditAnywhere, Category = "Content") bool bVerifyContentHashes = true;

	UPROPERTY(config, EditAnywhere, Category = "Permissions") TArray<FName> AutoGrantedPermissions;
	UPROPERTY(config, EditAnywhere, Category = "Permissions") TArray<FName> AlwaysDeniedPermissions;
	UPROPERTY(config, EditAnywhere, Category = "Permissions") bool bDenyUnknownPermissions = true;

	UPROPERTY(config, EditAnywhere, Category = "Conflicts") EModConflictPolicy DefaultConflictPolicy = EModConflictPolicy::Error;

	UPROPERTY(config, EditAnywhere, Category = "Debug") bool bEnableConsoleCommands = true;

	/** Resolves {Project} style tokens in ModSearchDirectories into absolute paths. */
	TArray<FString> GetResolvedSearchDirectories() const;
};
```

## 9. `Registry/ModInfo.h` + `Registry/ModRegistry.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModInfo
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModManifest Manifest;
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModState State = EModState::Unknown;
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModLoadFailureReason FailureReason = EModLoadFailureReason::None;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString FailureMessage;
	/** Absolute path of the mod root directory (the folder containing mod.json). */
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString RootPath;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName ProviderId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") int32 LoadOrder = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bEnabled = true;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FName> GrantedPermissions;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FString> MountedPaths;   // virtual mount points
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModDiagnostic> Diagnostics;

	const FModId& GetId() const { return Manifest.Id; }
	bool IsLoaded() const;      // State >= Loaded && State != Failed/Unmounted/Disabled
	bool IsActive() const { return State == EModState::Activated; }
};

DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnModStateChanged, const FModId&, EModState /*Old*/, EModState /*New*/);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnModStateChangedDynamic, const FModId&, ModId, EModState, OldState, EModState, NewState);

UCLASS(BlueprintType)
class MODFRAMEWORK_API UModRegistry : public UObject
{
	GENERATED_BODY()
public:
	void Initialize();      // creates the owned sub-registries
	void Shutdown();

	bool RegisterMod(const FModInfo& InInfo, FModDiagnostic& OutError);
	bool UnregisterMod(const FModId& InId);
	void Reset();

	const FModInfo* FindMod(const FModId& InId) const;
	FModInfo* FindModMutable(const FModId& InId);

	UFUNCTION(BlueprintPure, Category="Mod|Registry") bool GetModInfo(const FModId& ModId, FModInfo& OutInfo) const;
	UFUNCTION(BlueprintPure, Category="Mod|Registry") bool IsModRegistered(const FModId& ModId) const;
	UFUNCTION(BlueprintPure, Category="Mod|Registry") TArray<FModId> GetAllModIds() const;              // sorted
	UFUNCTION(BlueprintPure, Category="Mod|Registry") TArray<FModInfo> GetAllMods() const;              // load order then id
	UFUNCTION(BlueprintPure, Category="Mod|Registry") TArray<FModInfo> GetModsInState(EModState State) const;
	UFUNCTION(BlueprintPure, Category="Mod|Registry") TArray<FModInfo> GetLoadedMods() const;
	UFUNCTION(BlueprintPure, Category="Mod|Registry") TArray<FModInfo> GetActiveMods() const;
	UFUNCTION(BlueprintPure, Category="Mod|Registry") int32 GetModCount() const;

	bool SetModState(const FModId& InId, EModState NewState);
	bool SetModFailed(const FModId& InId, EModLoadFailureReason Reason, const FString& Message);
	bool SetLoadOrder(const TArray<FModId>& InOrder);   // assigns FModInfo::LoadOrder
	bool SetModEnabled(const FModId& InId, bool bEnabled);
	void AddDiagnostic(const FModId& InId, const FModDiagnostic& Diagnostic);
	void SetGrantedPermissions(const FModId& InId, const TArray<FName>& Permissions);
	void SetMountedPaths(const FModId& InId, const TArray<FString>& Paths);

	/** Mod-owned UObject tracking so unload can release everything a mod created. */
	void TrackModObject(const FModId& InId, UObject* Object);
	void UntrackModObject(const FModId& InId, UObject* Object);
	TArray<UObject*> GetModObjects(const FModId& InId) const;
	void ReleaseModObjects(const FModId& InId);
	/** Reverse lookup: which mod owns this object (INDEX_NONE-ish -> invalid FModId). */
	FModId FindOwningMod(const UObject* Object) const;

	UFUNCTION(BlueprintPure, Category="Mod|Registry") UModAPIRegistry* GetAPIRegistry() const;
	UFUNCTION(BlueprintPure, Category="Mod|Registry") UModExtensionRegistry* GetExtensionRegistry() const;

	FOnModStateChanged OnModStateChanged;

private:
	UPROPERTY() TMap<FModId, FModInfo> Mods;                 // TMap with USTRUCT key requires GetTypeHash - provided
	UPROPERTY() TObjectPtr<UModAPIRegistry> APIRegistry;
	UPROPERTY() TObjectPtr<UModExtensionRegistry> ExtensionRegistry;
	UPROPERTY() TMap<FModId, FModObjectSet> ModObjects;
};

USTRUCT()
struct FModObjectSet
{
	GENERATED_BODY()
	UPROPERTY() TArray<TObjectPtr<UObject>> Objects;
};
```
> Note: `UPROPERTY() TMap<FModId, FModInfo>` is legal because `FModId` has `GetTypeHash` and `operator==`.
> Declare `FModObjectSet` before `UModRegistry`.

## 10. `API/ModAPI.h`, `API/ModAPIRegistry.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModAPIDescriptor
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName ApiId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModVersion Version;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString ClassPath;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString Description;
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bServerAuthoritative = false;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FName> RequiredPermissions;
	/** Invalid FModId when the API is provided by the game rather than by a mod. */
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModId ProviderModId;
};

/**
 * Base class for every game-exposed modding API. The framework never understands what an API
 * does - it only stores, versions and permission-gates it.
 * Subclasses declare identity with class metadata:
 *   UCLASS(BlueprintType, meta = (ModPublic, ModApiId = "game.gameplay", ModApiVersion = "1.0.0"))
 */
UCLASS(Abstract, BlueprintType)
class MODFRAMEWORK_API UModAPI : public UObject
{
	GENERATED_BODY()
public:
	/** Defaults to the ModApiId class metadata, else the class name. */
	UFUNCTION(BlueprintPure, Category="Mod|API") virtual FName GetApiId() const;
	/** Defaults to the ModApiVersion class metadata, else 1.0.0. */
	UFUNCTION(BlueprintPure, Category="Mod|API") virtual FModVersion GetApiVersion() const;
	UFUNCTION(BlueprintPure, Category="Mod|API") virtual FString GetApiDescription() const;
	/** Server-authoritative APIs refuse client calls when the world is a client. */
	UFUNCTION(BlueprintPure, Category="Mod|API") virtual bool IsServerAuthoritative() const;
	/** Permissions a mod must hold to obtain this API. From ModApiPermissions metadata (comma separated). */
	virtual TArray<FName> GetRequiredPermissions() const;

	virtual void OnAPIRegistered(UModAPIRegistry* InRegistry);
	virtual void OnAPIUnregistered();

	FModAPIDescriptor BuildDescriptor() const;

	/** Guard helper for server-authoritative implementations. Logs + returns false on clients. */
	bool VerifyServerAuthority(const UObject* WorldContext, const TCHAR* CallingFunction) const;

	UFUNCTION(BlueprintImplementableEvent, Category="Mod|API", meta=(DisplayName="On API Registered"))
	void ReceiveOnAPIRegistered();
};

UCLASS(BlueprintType)
class MODFRAMEWORK_API UModAPIRegistry : public UObject
{
	GENERATED_BODY()
public:
	bool RegisterAPI(UModAPI* InApi, FModDiagnostic& OutError);
	bool RegisterAPIForMod(UModAPI* InApi, const FModId& ProviderModId, FModDiagnostic& OutError);
	bool UnregisterAPI(FName ApiId);
	void UnregisterAllForMod(const FModId& ModId);
	void Reset();

	UFUNCTION(BlueprintPure, Category="Mod|API") UModAPI* FindAPI(FName ApiId) const;
	UFUNCTION(BlueprintPure, Category="Mod|API") bool HasAPI(FName ApiId) const;
	UFUNCTION(BlueprintPure, Category="Mod|API") TArray<FModAPIDescriptor> GetAllAPIs() const;
	UFUNCTION(BlueprintPure, Category="Mod|API") bool GetAPIDescriptor(FName ApiId, FModAPIDescriptor& OutDescriptor) const;

	/**
	 * The gated path mods must use. Checks: API exists, version satisfies RequiredRange,
	 * requesting mod holds every RequiredPermission. Fills OutError otherwise.
	 */
	UModAPI* RequestAPI(FName ApiId, const FModVersionRange& RequiredRange, const FModId& RequestingMod, FModDiagnostic& OutError);

	UFUNCTION(BlueprintCallable, Category="Mod|API", meta=(DeterminesOutputType="ApiClass", DisplayName="Request Mod API"))
	UModAPI* K2_RequestAPI(FName ApiId, TSubclassOf<UModAPI> ApiClass, const FString& VersionRange, FModId RequestingMod, FModDiagnostic& OutError);

	/** Injected by UModSubsystem so the registry can ask about permissions without owning them. */
	DECLARE_DELEGATE_RetVal_TwoParams(bool, FModPermissionCheck, const FModId&, FName);
	void SetPermissionCheck(FModPermissionCheck InCheck);

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnModAPIRegistryChanged, FName /*ApiId*/);
	FOnModAPIRegistryChanged OnAPIRegistered;
	FOnModAPIRegistryChanged OnAPIUnregistered;
};
```

## 11. `Extensions/ModExtension.h`, `ModExtensionRegistry.h`, `ModContentBundle.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModExtensionPointDescriptor
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FName ExtensionPointId;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FModVersion Version;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FText DisplayName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") FText Description;
	/** Extensions must derive from this. Null means any UModExtension. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") TSubclassOf<UModExtension> RequiredBaseClass;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") EModConflictPolicy DefaultConflictPolicy = EModConflictPolicy::LastWins;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") bool bAllowMultiplePerMod = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") bool bServerAuthoritative = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mod") TArray<FName> RequiredPermissions;
};

UCLASS(Abstract, Blueprintable, BlueprintType)
class MODFRAMEWORK_API UModExtension : public UObject
{
	GENERATED_BODY()
public:
	/** Which point this plugs into. Set in the CDO/asset by the mod author. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Mod") FName ExtensionPointId;
	/** Unique within the extension point. Defaults to "<modid>:<classname>" when left empty. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Mod") FName ExtensionId;
	/** Higher wins under EModConflictPolicy::Priority; also a tie-break for ordering. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Mod") int32 Priority = 0;
	/** Resources this extension claims, for conflict detection. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Mod") TArray<FName> ClaimedResourceIds;

	/** Assigned by the registry at registration time. Read-only for mods. */
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModId OwningModId;

	virtual void OnExtensionRegistered();
	virtual void OnExtensionActivated();
	virtual void OnExtensionDeactivated();
	virtual void OnExtensionUnregistered();

	UFUNCTION(BlueprintImplementableEvent, Category="Mod", meta=(DisplayName="On Extension Registered"))   void ReceiveOnRegistered();
	UFUNCTION(BlueprintImplementableEvent, Category="Mod", meta=(DisplayName="On Extension Activated"))    void ReceiveOnActivated();
	UFUNCTION(BlueprintImplementableEvent, Category="Mod", meta=(DisplayName="On Extension Deactivated"))  void ReceiveOnDeactivated();
	UFUNCTION(BlueprintImplementableEvent, Category="Mod", meta=(DisplayName="On Extension Unregistered")) void ReceiveOnUnregistered();

	UFUNCTION(BlueprintPure, Category="Mod") FName GetResolvedExtensionId() const;
	UFUNCTION(BlueprintPure, Category="Mod") bool IsExtensionActive() const;

	// UObject
	virtual UWorld* GetWorld() const override;   // route to Outer so BP extensions have world context
};

UCLASS(BlueprintType)
class MODFRAMEWORK_API UModExtensionRegistry : public UObject
{
	GENERATED_BODY()
public:
	bool RegisterExtensionPoint(const FModExtensionPointDescriptor& InDescriptor, FModDiagnostic& OutError);
	bool UnregisterExtensionPoint(FName ExtensionPointId);
	UFUNCTION(BlueprintPure, Category="Mod|Extensions") bool GetExtensionPoint(FName ExtensionPointId, FModExtensionPointDescriptor& OutDescriptor) const;
	UFUNCTION(BlueprintPure, Category="Mod|Extensions") TArray<FModExtensionPointDescriptor> GetExtensionPoints() const;

	bool RegisterExtension(UModExtension* InExtension, const FModId& OwningModId, FModDiagnostic& OutError);
	bool UnregisterExtension(FName ExtensionPointId, FName ExtensionId);
	void UnregisterAllForMod(const FModId& ModId);
	void SetModExtensionsActive(const FModId& ModId, bool bActive);
	void Reset();

	UFUNCTION(BlueprintPure, Category="Mod|Extensions") TArray<UModExtension*> GetExtensions(FName ExtensionPointId) const;             // active only, ordered
	UFUNCTION(BlueprintPure, Category="Mod|Extensions") TArray<UModExtension*> GetAllExtensions(FName ExtensionPointId) const;          // includes inactive
	UFUNCTION(BlueprintPure, Category="Mod|Extensions") UModExtension* FindExtension(FName ExtensionPointId, FName ExtensionId) const;
	UFUNCTION(BlueprintPure, Category="Mod|Extensions") TArray<UModExtension*> GetExtensionsForMod(const FModId& ModId) const;
	/** Applies the point's conflict policy to pick a single winner for a claimed resource. */
	UFUNCTION(BlueprintPure, Category="Mod|Extensions") UModExtension* ResolveResource(FName ExtensionPointId, FName ResourceId) const;

	/** Every claim currently registered, for FModConflictDetector. */
	TArray<FModResourceClaim> CollectResourceClaims() const;

	/** Ordering input from the resolver; extensions sort by (Priority desc, ModLoadOrder asc, ExtensionId asc). */
	void SetModLoadOrder(const TArray<FModId>& InOrder);

	void SetPermissionCheck(UModAPIRegistry::FModPermissionCheck InCheck);   // same delegate type

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnModExtensionsChanged, FName /*PointId*/, const FModId& /*ModId*/);
	FOnModExtensionsChanged OnExtensionsChanged;
	FOnModExtensionsChanged OnExtensionPointsChanged;
};

/** Data-driven registration payload: the "no C++ required" path. */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModContentBundle : public UPrimaryDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mod") FText DisplayName;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mod") TArray<TSoftClassPtr<UModExtension>> ExtensionClasses;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mod") TArray<TSoftObjectPtr<UPrimaryDataAsset>> DataAssets;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mod") TArray<TSoftObjectPtr<UDataTable>> DataTables;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Mod") TArray<FName> RequiredExtensionPoints;
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;   // type "ModContentBundle"
};
```

## 12. `Events/ModEventTypes.h`, `Events/ModEventBus.h`

```cpp
/** Base for typed event payloads. Games/SDKs derive their own USTRUCTs from this. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEventPayload
{
	GENERATED_BODY()
	virtual ~FModEventPayload() = default;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEventContext
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName EventId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModId SourceModId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TInstancedStruct<FModEventPayload> Payload;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TObjectPtr<UObject> WorldContext = nullptr;

	template <typename T> const T* GetPayload() const;   // nullptr when the type does not match
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEventDescriptor
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName EventId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString Description;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TObjectPtr<UScriptStruct> PayloadType = nullptr;
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bServerAuthoritative = false;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FName> RequiredPermissions;
};

/** Framework event ids. Game-specific events live in the SDK, never here. */
namespace ModFrameworkEvents
{
	MODFRAMEWORK_API extern const FName ModDiscovered;
	MODFRAMEWORK_API extern const FName ModValidated;
	MODFRAMEWORK_API extern const FName ModMounted;
	MODFRAMEWORK_API extern const FName ModLoaded;
	MODFRAMEWORK_API extern const FName ModActivated;
	MODFRAMEWORK_API extern const FName ModDeactivated;
	MODFRAMEWORK_API extern const FName ModUnloaded;
	MODFRAMEWORK_API extern const FName ModUnmounted;
	MODFRAMEWORK_API extern const FName ModFailed;
	MODFRAMEWORK_API extern const FName ModsRefreshed;
	MODFRAMEWORK_API extern const FName WorldCreated;
	MODFRAMEWORK_API extern const FName WorldDestroyed;
	MODFRAMEWORK_API extern const FName GameStarted;
}

/** Payload for every ModFrameworkEvents::Mod* event. */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModLifecycleEventPayload : public FModEventPayload
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModId ModId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModState State = EModState::Unknown;
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModLoadFailureReason FailureReason = EModLoadFailureReason::None;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString Message;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEventHandle
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") int64 Id = 0;
	bool IsValid() const { return Id != 0; }
	friend uint32 GetTypeHash(const FModEventHandle& In) { return GetTypeHash(In.Id); }
	bool operator==(const FModEventHandle& O) const { return Id == O.Id; }
};

DECLARE_DELEGATE_OneParam(FModEventDelegate, const FModEventContext&);
DECLARE_DYNAMIC_DELEGATE_OneParam(FModEventDynamicDelegate, const FModEventContext&, EventContext);

UCLASS(BlueprintType)
class MODFRAMEWORK_API UModEventBus : public UObject
{
	GENERATED_BODY()
public:
	bool RegisterEventType(const FModEventDescriptor& InDescriptor, FModDiagnostic& OutError);
	bool UnregisterEventType(FName EventId);
	UFUNCTION(BlueprintPure, Category="Mod|Events") TArray<FModEventDescriptor> GetEventTypes() const;
	UFUNCTION(BlueprintPure, Category="Mod|Events") bool GetEventType(FName EventId, FModEventDescriptor& OutDescriptor) const;

	FModEventHandle Subscribe(FName EventId, const FModId& Subscriber, FModEventDelegate Delegate, int32 Priority = 0);
	UFUNCTION(BlueprintCallable, Category="Mod|Events", meta=(DisplayName="Subscribe To Mod Event"))
	FModEventHandle K2_Subscribe(FName EventId, FModId Subscriber, FModEventDynamicDelegate Delegate, int32 Priority = 0);

	UFUNCTION(BlueprintCallable, Category="Mod|Events") bool Unsubscribe(FModEventHandle Handle);
	void UnsubscribeAllForMod(const FModId& ModId);
	void Reset();

	void Broadcast(const FModEventContext& Context);
	UFUNCTION(BlueprintCallable, Category="Mod|Events", meta=(DisplayName="Broadcast Mod Event"))
	void K2_Broadcast(FName EventId, FModId SourceModId, const TInstancedStruct<FModEventPayload>& Payload, UObject* WorldContext);

	/** Convenience used by the subsystem. */
	void BroadcastLifecycle(FName EventId, const FModId& ModId, EModState State,
		EModLoadFailureReason Reason = EModLoadFailureReason::None, const FString& Message = FString());

	void SetPermissionCheck(UModAPIRegistry::FModPermissionCheck InCheck);
	UFUNCTION(BlueprintPure, Category="Mod|Events") int32 GetSubscriberCount(FName EventId) const;
};
```
Re-entrancy: `Broadcast` must be safe if a handler subscribes/unsubscribes during dispatch (copy the
handler list, mark removals as pending, compact afterwards).

## 13. `Permissions/ModPermissions.h`, `ModPermissionRegistry.h`

```cpp
namespace ModPermissions
{
	MODFRAMEWORK_API extern const FName GameplayModify;   // "gameplay.modify"
	MODFRAMEWORK_API extern const FName AssetsRead;       // "assets.read"
	MODFRAMEWORK_API extern const FName AssetsWrite;      // "assets.write"
	MODFRAMEWORK_API extern const FName FilesystemRead;   // "filesystem.read"
	MODFRAMEWORK_API extern const FName FilesystemWrite;  // "filesystem.write"
	MODFRAMEWORK_API extern const FName Network;          // "network"
	MODFRAMEWORK_API extern const FName Console;          // "console"
	MODFRAMEWORK_API extern const FName SaveModify;       // "save.modify"
	MODFRAMEWORK_API extern const FName NativeCode;       // "native_code"
	MODFRAMEWORK_API extern const FName UiModify;         // "ui.modify"
	MODFRAMEWORK_API TArray<FName> GetBuiltinPermissions();
}

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModPermissionDescriptor
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName PermissionId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FText DisplayName;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FText Description;
	/** Dangerous permissions are never auto-granted. */
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bDangerous = false;
};

UINTERFACE(BlueprintType, MinimalAPI)
class UModPermissionPolicy : public UInterface { GENERATED_BODY() };

class MODFRAMEWORK_API IModPermissionPolicy
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Mod|Permissions")
	EModPermissionState ResolvePermission(const FModManifest& Manifest, FName PermissionId);
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnModPermissionChanged, const FModId&, FName);

UCLASS(BlueprintType)
class MODFRAMEWORK_API UModPermissionRegistry : public UObject
{
	GENERATED_BODY()
public:
	void Initialize();       // registers the builtin permissions
	void Reset();

	void RegisterPermission(const FModPermissionDescriptor& InDescriptor);
	UFUNCTION(BlueprintPure, Category="Mod|Permissions") TArray<FModPermissionDescriptor> GetAllPermissions() const;
	UFUNCTION(BlueprintPure, Category="Mod|Permissions") bool GetPermission(FName PermissionId, FModPermissionDescriptor& Out) const;
	UFUNCTION(BlueprintPure, Category="Mod|Permissions") bool IsPermissionRegistered(FName PermissionId) const;

	/** Evaluates every requested permission in the manifest and stores the result. */
	void EvaluateManifest(const FModManifest& Manifest);

	UFUNCTION(BlueprintPure, Category="Mod|Permissions") EModPermissionState GetPermissionState(const FModId& ModId, FName PermissionId) const;
	UFUNCTION(BlueprintPure, Category="Mod|Permissions") bool HasPermission(const FModId& ModId, FName PermissionId) const;
	UFUNCTION(BlueprintPure, Category="Mod|Permissions") TArray<FName> GetGrantedPermissions(const FModId& ModId) const;
	UFUNCTION(BlueprintPure, Category="Mod|Permissions") TArray<FName> GetPendingPermissions(const FModId& ModId) const;

	UFUNCTION(BlueprintCallable, Category="Mod|Permissions") void GrantPermission(const FModId& ModId, FName PermissionId);
	UFUNCTION(BlueprintCallable, Category="Mod|Permissions") void DenyPermission(const FModId& ModId, FName PermissionId);
	UFUNCTION(BlueprintCallable, Category="Mod|Permissions") void GrantAll(const FModId& ModId);   // logs a loud warning
	UFUNCTION(BlueprintCallable, Category="Mod|Permissions") void ResetForMod(const FModId& ModId);

	UFUNCTION(BlueprintCallable, Category="Mod|Permissions") void SetPolicy(TScriptInterface<IModPermissionPolicy> InPolicy);

	FOnModPermissionChanged OnPermissionChanged;
	/** Raised for permissions left Pending so the game can drive UI. */
	FOnModPermissionChanged OnPermissionRequested;
};
```
Default evaluation when no policy is set: unknown permission -> Denied if
`bDenyUnknownPermissions`, else Pending. Dangerous permission -> Pending (never auto-granted).
In `AutoGrantedPermissions` and not dangerous -> Granted. In `AlwaysDeniedPermissions` -> Denied.
Otherwise -> Pending.

## 14. `Content/ModContentTypes.h`, `ModContentManager.h`, `ModAssetLibrary.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModContentMount
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModId OwnerModId;
	/** Virtual package root, always of the form "/Something/". */
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString VirtualMountPoint;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString PhysicalPath;
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModContentRootType Type = EModContentRootType::Pak;
	UPROPERTY(BlueprintReadOnly, Category="Mod") int32 MountOrder = 0;
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bMounted = false;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName MounterId;
	/** Long package names discovered under this mount. */
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FName> DiscoveredPackages;
};

/** Packaging-technology abstraction. Nothing above this layer may mention Pak or IoStore. */
class MODFRAMEWORK_API IModContentMounter
{
public:
	virtual ~IModContentMounter() = default;
	virtual FName GetMounterId() const = 0;
	virtual bool CanMount(const FModContentRoot& Root, const FString& AbsolutePath) const = 0;
	virtual bool Mount(const FModId& ModId, const FModContentRoot& Root, const FString& AbsolutePath,
		FModContentMount& OutMount, FModDiagnostic& OutError) = 0;
	virtual bool Unmount(const FModContentMount& Mount, FModDiagnostic& OutError) = 0;
};

class MODFRAMEWORK_API FModContentManager
{
public:
	FModContentManager();
	~FModContentManager();

	void Initialize();
	void Shutdown();

	void RegisterMounter(TSharedRef<IModContentMounter> InMounter);
	void UnregisterMounter(FName MounterId);

	/** Mounts every content root of a mod. All-or-nothing: rolls back on partial failure. */
	bool MountMod(const FModInfo& ModInfo, TArray<FModContentMount>& OutMounts, TArray<FModDiagnostic>& OutDiagnostics);
	bool UnmountMod(const FModId& ModId, TArray<FModDiagnostic>& OutDiagnostics);
	bool IsModMounted(const FModId& ModId) const;

	TArray<FModContentMount> GetMounts() const;
	TArray<FModContentMount> GetMountsForMod(const FModId& ModId) const;
	/** Which mod owns a given /VirtualRoot/... object path. Invalid id when it is base game content. */
	FModId FindOwningMod(const FString& LongPackageNameOrObjectPath) const;

	/** Asset queries scoped to a mod's mounts (asset registry backed). */
	TArray<FAssetData> GetModAssets(const FModId& ModId, UClass* ClassFilter = nullptr, bool bRecursiveClasses = true) const;
	TArray<FAssetData> GetAllModAssets(UClass* ClassFilter = nullptr, bool bRecursiveClasses = true) const;

	/** Normalises "/MyMod" / "MyMod" / "/MyMod/" into "/MyMod/". */
	static FString NormalizeMountPoint(const FString& In);
	/** Default mount point for a mod when the manifest does not specify one. */
	static FString MakeDefaultMountPoint(const FModId& ModId, const FString& ModRootPath);

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnModContentMountsChanged, const FModId&);
	FOnModContentMountsChanged OnMounted;
	FOnModContentMountsChanged OnUnmounted;
};

UCLASS()
class MODFRAMEWORK_API UModAssetLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="Mod|Assets", meta=(WorldContext="WorldContextObject"))
	static TArray<FAssetData> GetModAssets(const UObject* WorldContextObject, FModId ModId, UClass* ClassFilter);

	UFUNCTION(BlueprintCallable, Category="Mod|Assets", meta=(WorldContext="WorldContextObject", DeterminesOutputType="AssetClass"))
	static UObject* LoadModAsset(const UObject* WorldContextObject, FModId ModId, FName AssetName, UClass* AssetClass);

	UFUNCTION(BlueprintCallable, Category="Mod|Assets", meta=(WorldContext="WorldContextObject"))
	static TArray<UClass*> GetModClasses(const UObject* WorldContextObject, FModId ModId, UClass* BaseClass);

	UFUNCTION(BlueprintPure, Category="Mod|Assets", meta=(WorldContext="WorldContextObject"))
	static FModId FindOwningMod(const UObject* WorldContextObject, const UObject* Asset);

	UFUNCTION(BlueprintPure, Category="Mod|Assets", meta=(WorldContext="WorldContextObject"))
	static TArray<FModContentMount> GetModContentMounts(const UObject* WorldContextObject);
};
```

## 15. `Providers/ModProvider.h`, `LocalFileModProvider.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModDiscoveryResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName ProviderId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString RootPath;        // folder containing mod.json
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString ManifestPath;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModManifest Manifest;
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bManifestValid = false;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModDiagnostic> Diagnostics;
};

DECLARE_DELEGATE_OneParam(FOnModDiscoveryComplete, const TArray<FModDiscoveryResult>&);

class MODFRAMEWORK_API IModProvider
{
public:
	virtual ~IModProvider() = default;
	virtual FName GetProviderId() const = 0;
	virtual FText GetDisplayName() const = 0;
	virtual void DiscoverMods(TArray<FModDiscoveryResult>& OutResults) = 0;
	virtual bool SupportsAsyncDiscovery() const { return false; }
	virtual void DiscoverModsAsync(FOnModDiscoveryComplete OnComplete) { TArray<FModDiscoveryResult> R; DiscoverMods(R); OnComplete.ExecuteIfBound(R); }
	/** Makes the mod available on the local filesystem. Already-local providers just return the path. */
	virtual bool AcquireMod(const FModId& ModId, FString& OutLocalRootPath, FModDiagnostic& OutError) = 0;
	virtual bool ReleaseMod(const FModId& ModId) { return true; }
	virtual bool SupportsInstall() const { return false; }
	/** Installs a .mod package. Only providers that own a mod directory implement this. */
	virtual bool InstallModPackage(const FString& PackagePath, FModId& OutInstalledId, TArray<FModDiagnostic>& OutDiagnostics) { return false; }
	virtual bool UninstallMod(const FModId& ModId, TArray<FModDiagnostic>& OutDiagnostics) { return false; }
};

class MODFRAMEWORK_API FLocalFileModProvider : public IModProvider
{
public:
	explicit FLocalFileModProvider(TArray<FString> InSearchDirectories);
	static FName StaticProviderId();     // "LocalFile"
	// IModProvider overrides, all of them, including install/uninstall.
	void SetSearchDirectories(TArray<FString> In);
	TArray<FString> GetSearchDirectories() const;
	/** Where extracted .mod packages are installed. Defaults to the first writable search dir. */
	void SetInstallDirectory(const FString& In);
};
```
Discovery rules: for each search directory, enumerate immediate subdirectories; a subdirectory is a
mod root if it contains `mod.json`. Also treat loose `*.mod` files in the search directory as
packaged mods — read their embedded manifest via `FModPackageReader` without extracting.

## 16. `Packaging/ModPackageFormat.h`

Self-describing container, no third-party deps.
```
[FModPackageHeader]  [manifest json bytes]  [TOC]  [entry payloads...]
```
```cpp
namespace ModPackage
{
	inline constexpr uint32 Magic = 0x444F4D55;         // 'UMOD' little-endian
	inline constexpr uint32 CurrentFormatVersion = 1;
	MODFRAMEWORK_API const TCHAR* GetFileExtension();   // ".mod"
}

USTRUCT()
struct MODFRAMEWORK_API FModPackageEntry
{
	GENERATED_BODY()
	UPROPERTY() FString RelativePath;      // forward slashes, no leading slash, no ".."
	UPROPERTY() int64 Offset = 0;
	UPROPERTY() int64 CompressedSize = 0;
	UPROPERTY() int64 UncompressedSize = 0;
	UPROPERTY() bool bCompressed = false;
	UPROPERTY() FString Hash;              // SHA1 hex of the uncompressed bytes
	friend FArchive& operator<<(FArchive& Ar, FModPackageEntry& E);
};

USTRUCT()
struct MODFRAMEWORK_API FModPackageHeader
{
	GENERATED_BODY()
	UPROPERTY() uint32 Magic = ModPackage::Magic;
	UPROPERTY() uint32 FormatVersion = ModPackage::CurrentFormatVersion;
	UPROPERTY() int64 ManifestOffset = 0;
	UPROPERTY() int64 ManifestSize = 0;
	UPROPERTY() int64 TocOffset = 0;
	UPROPERTY() int64 TocSize = 0;
	UPROPERTY() FString ContentHash;       // SHA1 hex over the TOC entry hashes, in TOC order
	friend FArchive& operator<<(FArchive& Ar, FModPackageHeader& H);
};

class MODFRAMEWORK_API FModPackageReader
{
public:
	bool Open(const FString& AbsolutePath, TArray<FModDiagnostic>& OutDiagnostics);
	void Close();
	bool IsOpen() const;
	const FModManifest& GetManifest() const;
	const FModPackageHeader& GetHeader() const;
	TArray<FModPackageEntry> GetEntries() const;
	bool ReadEntry(const FString& RelativePath, TArray<uint8>& OutBytes, FModDiagnostic& OutError);
	/** Extracts everything under DestinationDirectory. Rejects entries that escape the root. */
	bool ExtractAll(const FString& DestinationDirectory, TArray<FModDiagnostic>& OutDiagnostics);
	bool VerifyIntegrity(TArray<FModDiagnostic>& OutDiagnostics);
	/** Reads only the manifest without keeping the file open. */
	static bool PeekManifest(const FString& AbsolutePath, FModManifest& OutManifest, TArray<FModDiagnostic>& OutDiagnostics);
	static FString ComputeContentHash(const TArray<FModPackageEntry>& Entries);
};
```
The writer (`FModPackageWriter`) lives in `ModFrameworkDeveloper`, not here.

## 17. `Dependencies/ModDependencyTypes.h` + `ModDependencyResolver.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModEnvironment
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString GameId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModVersion GameVersion;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModVersion FrameworkVersion;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString SdkId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModVersion SdkVersion;
	static FModEnvironment FromSettings();     // reads UModFrameworkSettings + ModFrameworkVersion
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModRejection
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModId ModId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModLoadFailureReason Reason = EModLoadFailureReason::None;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString Message;
	/** Mods implicated in the rejection (missing dependency, cycle members, ...). */
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModId> RelatedMods;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModResolveRequest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModManifest> Candidates;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModId> DisabledMods;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModEnvironment Environment;
	/** When false, framework/game/sdk version checks are skipped (used by editor tooling). */
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bCheckEnvironment = true;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModResolveResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bSuccess = false;
	/** Deterministic load order. Only contains accepted mods. */
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModId> LoadOrder;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModRejection> Rejections;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModDiagnostic> Diagnostics;

	const FModRejection* FindRejection(const FModId& ModId) const;
	FString ToDebugString() const;
};

class MODFRAMEWORK_API FModDependencyResolver
{
public:
	static FModResolveResult Resolve(const FModResolveRequest& Request);
	/** Checks a single manifest against the environment. Empty result means compatible. */
	static TArray<FModRejection> CheckEnvironment(const FModManifest& Manifest, const FModEnvironment& Environment);
	/** Just the graph edges, for the editor's dependency visualiser. */
	static void BuildGraph(const TArray<FModManifest>& Manifests, TMap<FModId, TArray<FModId>>& OutEdges);
	static TArray<TArray<FModId>> FindCycles(const TMap<FModId, TArray<FModId>>& Edges);
};
```
Algorithm requirements:
1. Reject duplicate ids (keep the highest version, reject the rest with `DuplicateModId`).
2. Reject explicitly disabled mods with `Disabled`.
3. Environment checks (game id/version, framework range, sdk id/range).
4. Missing/incompatible required dependencies -> reject. Optional missing -> Info diagnostic only.
5. Cascade: rejecting X rejects everything that requires X (`DependencyFailed`), transitively.
6. Cycle detection over required edges -> every member rejected with `CircularDependency` and the
   cycle path in `RelatedMods`. `loadBefore`/`loadAfter` cycles are also errors.
7. Topological sort with Kahn's algorithm; ready-set tie-break is **(Priority desc, ModId asc)** so
   the order is fully deterministic and stable across runs.
8. Edges: `dependency` -> dependent, `loadAfter` target -> mod, mod -> `loadBefore` target.

## 18. `Conflicts/ModConflictTypes.h` + `ModConflictDetector.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModResourceClaim
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModId ModId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName ExtensionPointId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName ResourceId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") int32 Priority = 0;
	UPROPERTY(BlueprintReadOnly, Category="Mod") int32 LoadOrder = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModConflictPolicy PreferredPolicy = EModConflictPolicy::Error;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName ExtensionId;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModConflict
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName ExtensionPointId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FName ResourceId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModId> Contenders;
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModConflictPolicy AppliedPolicy = EModConflictPolicy::Error;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModId Winner;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModId> Losers;
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bBlocking = false;      // Error policy -> blocking
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString Explanation;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModConflictPolicyTable
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModConflictPolicy DefaultPolicy = EModConflictPolicy::Error;
	/** Per extension point override. */
	UPROPERTY(BlueprintReadOnly, Category="Mod") TMap<FName, EModConflictPolicy> PointPolicies;
	/** Per "point:resource" override; takes precedence. */
	UPROPERTY(BlueprintReadOnly, Category="Mod") TMap<FName, EModConflictPolicy> ResourcePolicies;
	EModConflictPolicy Resolve(FName ExtensionPointId, FName ResourceId) const;
};

class MODFRAMEWORK_API FModConflictDetector
{
public:
	static TArray<FModConflict> Detect(const TArray<FModResourceClaim>& Claims, const FModConflictPolicyTable& Policies);
	static TArray<FModId> GetBlockedMods(const TArray<FModConflict>& Conflicts);
	static FString BuildReport(const TArray<FModConflict>& Conflicts);
};
```
Winner selection: `FirstWins` -> lowest LoadOrder; `LastWins` -> highest LoadOrder;
`Priority` -> highest Priority, tie-break highest LoadOrder; `Merge` -> no single winner
(`Winner` invalid, all contenders kept, `bBlocking=false`); `Error` -> `bBlocking=true`, no winner.

## 19. `Save/ModSaveTypes.h`, `ModSaveDataManager.h`, `ModSaveGame.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSaveDependency
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModId ModId;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModVersion Version;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FString DisplayName;
	UPROPERTY(BlueprintReadWrite, Category="Mod") bool bWasRequired = false;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSaveRecord
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModId ModId;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModVersion ModVersion;
	/** Mod-authored schema version, used for migration. */
	UPROPERTY(BlueprintReadWrite, Category="Mod") int32 DataVersion = 1;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FString Json;
	UPROPERTY(BlueprintReadWrite, Category="Mod") TArray<uint8> Binary;
	/** True when the owning mod was absent at load time - preserved verbatim, never dropped. */
	UPROPERTY(BlueprintReadWrite, Category="Mod") bool bOrphaned = false;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSaveEnvelope
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Mod") int32 EnvelopeVersion = 1;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FString GameId;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModVersion FrameworkVersion;
	UPROPERTY(BlueprintReadWrite, Category="Mod") TArray<FModSaveDependency> RequiredMods;
	UPROPERTY(BlueprintReadWrite, Category="Mod") TArray<FModSaveRecord> Records;

	const FModSaveRecord* FindRecord(const FModId& ModId) const;
	FModSaveRecord& FindOrAddRecord(const FModId& ModId);
};

UCLASS(BlueprintType)
class MODFRAMEWORK_API UModSaveGame : public USaveGame
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModSaveEnvelope Envelope;
};

UINTERFACE(BlueprintType, MinimalAPI)
class UModSaveMigration : public UInterface { GENERATED_BODY() };
class MODFRAMEWORK_API IModSaveMigration
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Mod|Save")
	bool MigrateModSave(const FModId& ModId, int32 FromVersion, int32 ToVersion, UPARAM(ref) FModSaveRecord& Record);
};

UCLASS(BlueprintType)
class MODFRAMEWORK_API UModSaveDataManager : public UObject
{
	GENERATED_BODY()
public:
	void Initialize(class UModSubsystem* InSubsystem);
	void Shutdown();

	UFUNCTION(BlueprintCallable, Category="Mod|Save") void ResetEnvelope();
	UFUNCTION(BlueprintCallable, Category="Mod|Save") void SetEnvelope(const FModSaveEnvelope& In);
	UFUNCTION(BlueprintPure,     Category="Mod|Save") const FModSaveEnvelope& GetEnvelope() const;
	/** Stamps the envelope with the mods currently active. Call before writing a save. */
	UFUNCTION(BlueprintCallable, Category="Mod|Save") void StampRequiredMods();

	UFUNCTION(BlueprintCallable, Category="Mod|Save") bool WriteModJson(const FModId& ModId, const FString& Json, int32 DataVersion = 1);
	UFUNCTION(BlueprintPure,     Category="Mod|Save") bool ReadModJson(const FModId& ModId, FString& OutJson, int32& OutDataVersion) const;
	UFUNCTION(BlueprintCallable, Category="Mod|Save") bool WriteModBytes(const FModId& ModId, const TArray<uint8>& Bytes, int32 DataVersion = 1);
	UFUNCTION(BlueprintPure,     Category="Mod|Save") bool ReadModBytes(const FModId& ModId, TArray<uint8>& OutBytes, int32& OutDataVersion) const;
	UFUNCTION(BlueprintCallable, Category="Mod|Save") bool ClearModData(const FModId& ModId);

	/** Typed helpers for native code. */
	template <typename T> bool WriteModStruct(const FModId& ModId, const T& Value, int32 DataVersion = 1);
	template <typename T> bool ReadModStruct(const FModId& ModId, T& OutValue, int32& OutDataVersion) const;

	/** Mods listed in the envelope that are not currently loaded. */
	UFUNCTION(BlueprintPure, Category="Mod|Save") TArray<FModSaveDependency> GetMissingMods() const;
	UFUNCTION(BlueprintPure, Category="Mod|Save") TArray<FModSaveDependency> GetMissingRequiredMods() const;
	UFUNCTION(BlueprintPure, Category="Mod|Save") TArray<FModSaveRecord> GetOrphanedRecords() const;
	/** Flags records whose mod is absent. Never deletes them. */
	UFUNCTION(BlueprintCallable, Category="Mod|Save") void MarkOrphanedRecords();
	UFUNCTION(BlueprintCallable, Category="Mod|Save") bool PurgeOrphanedRecord(const FModId& ModId);   // explicit opt-in only

	UFUNCTION(BlueprintCallable, Category="Mod|Save") void RegisterMigration(const FModId& ModId, TScriptInterface<IModSaveMigration> Migration);
	UFUNCTION(BlueprintCallable, Category="Mod|Save") bool MigrateRecord(const FModId& ModId, int32 TargetVersion);

	/** Slot helpers wrapping UGameplayStatics. */
	UFUNCTION(BlueprintCallable, Category="Mod|Save") bool SaveToSlot(const FString& SlotName, int32 UserIndex);
	UFUNCTION(BlueprintCallable, Category="Mod|Save") bool LoadFromSlot(const FString& SlotName, int32 UserIndex);
};
```

## 20. `Net/ModSessionManifest.h`, `ModNetworkValidator.h`, `ModNetworkStatics.h`

```cpp
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModNetworkEntry
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModId ModId;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModVersion Version;
	UPROPERTY(BlueprintReadWrite, Category="Mod") EModNetworkScope Scope = EModNetworkScope::ClientAndServer;
	UPROPERTY(BlueprintReadWrite, Category="Mod") bool bRequired = false;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FString ContentHash;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FString DisplayName;
	friend FArchive& operator<<(FArchive& Ar, FModNetworkEntry& E);
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModSessionManifest
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite, Category="Mod") int32 FormatVersion = 1;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FString GameId;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModVersion GameVersion;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModVersion FrameworkVersion;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FString SdkId;
	UPROPERTY(BlueprintReadWrite, Category="Mod") FModVersion SdkVersion;
	UPROPERTY(BlueprintReadWrite, Category="Mod") TArray<FModNetworkEntry> Entries;

	const FModNetworkEntry* FindEntry(const FModId& ModId) const;
	/** Stable digest of the required, network-relevant entries. */
	FString ComputeDigest() const;
	/** Compact, URL-safe encoding for travel URLs / login options. */
	FString ToBase64() const;
	static bool FromBase64(const FString& In, FModSessionManifest& Out);
	friend FArchive& operator<<(FArchive& Ar, FModSessionManifest& M);
};

UENUM(BlueprintType)
enum class EModNetworkMismatchType : uint8
{
	MissingOnClient,
	MissingOnServer,
	VersionMismatch,
	ContentHashMismatch,
	ScopeViolation,
	GameMismatch,
	FrameworkMismatch,
	SdkMismatch
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModNetworkMismatch
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") EModNetworkMismatchType Type = EModNetworkMismatchType::VersionMismatch;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModId ModId;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString DisplayName;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModVersion Expected;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FModVersion Actual;
	UPROPERTY(BlueprintReadOnly, Category="Mod") FString Message;
};

USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModNetworkValidationResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Mod") bool bCompatible = false;
	UPROPERTY(BlueprintReadOnly, Category="Mod") TArray<FModNetworkMismatch> Mismatches;
	FText BuildUserFacingMessage() const;
	FString ToDebugString() const;
};

class MODFRAMEWORK_API FModNetworkValidator
{
public:
	/** Server-side check of a joining client. ServerOnly mods are ignored on the client side. */
	static FModNetworkValidationResult ValidateClient(const FModSessionManifest& Server, const FModSessionManifest& Client);
	/** Client-side pre-flight against an advertised server manifest. */
	static FModNetworkValidationResult ValidateServer(const FModSessionManifest& Client, const FModSessionManifest& Server);
};

UCLASS()
class MODFRAMEWORK_API UModNetworkStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	static const TCHAR* GetLoginOptionKey();   // "ModManifest"
	UFUNCTION(BlueprintPure, Category="Mod|Network", meta=(WorldContext="WorldContextObject"))
	static FString BuildLocalSessionManifestOption(const UObject* WorldContextObject);
	UFUNCTION(BlueprintCallable, Category="Mod|Network")
	static bool ParseSessionManifestFromOptions(const FString& Options, FModSessionManifest& OutManifest);
	UFUNCTION(BlueprintCallable, Category="Mod|Network", meta=(WorldContext="WorldContextObject"))
	static bool ValidateJoiningPlayer(const UObject* WorldContextObject, const FString& Options, FString& OutErrorMessage);
	UFUNCTION(BlueprintPure, Category="Mod|Network")
	static FString AppendModManifestToTravelURL(const FString& TravelURL, const FString& EncodedManifest);
};
```

## 21. `Runtime/ModContext.h`, `Runtime/ModEntryPointBase.h`

`UModContext` is the *only* handle a mod gets. It is the security/API boundary; it must never
hand out raw framework internals.

```cpp
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModContext : public UObject
{
	GENERATED_BODY()
public:
	void InitializeContext(class UModSubsystem* InSubsystem, const FModId& InModId);

	UFUNCTION(BlueprintPure, Category="Mod") FModId GetModId() const;
	UFUNCTION(BlueprintPure, Category="Mod") bool GetManifest(FModManifest& OutManifest) const;
	UFUNCTION(BlueprintPure, Category="Mod") bool GetModInfo(FModInfo& OutInfo) const;
	UFUNCTION(BlueprintPure, Category="Mod") FString GetModRootPath() const;

	UFUNCTION(BlueprintCallable, Category="Mod|API", meta=(DeterminesOutputType="ApiClass"))
	UModAPI* RequestAPI(FName ApiId, TSubclassOf<UModAPI> ApiClass, const FString& VersionRange, FModDiagnostic& OutError);

	UFUNCTION(BlueprintCallable, Category="Mod|Extensions")
	UModExtension* RegisterExtension(TSubclassOf<UModExtension> ExtensionClass, FModDiagnostic& OutError);
	UFUNCTION(BlueprintCallable, Category="Mod|Extensions")
	bool RegisterExtensionInstance(UModExtension* Extension, FModDiagnostic& OutError);
	UFUNCTION(BlueprintCallable, Category="Mod|Extensions") bool UnregisterExtension(FName ExtensionPointId, FName ExtensionId);

	UFUNCTION(BlueprintCallable, Category="Mod|Events")
	FModEventHandle SubscribeToEvent(FName EventId, FModEventDynamicDelegate Delegate, int32 Priority = 0);
	UFUNCTION(BlueprintCallable, Category="Mod|Events") bool UnsubscribeFromEvent(FModEventHandle Handle);
	UFUNCTION(BlueprintCallable, Category="Mod|Events")
	bool BroadcastEvent(FName EventId, const TInstancedStruct<FModEventPayload>& Payload);

	UFUNCTION(BlueprintPure, Category="Mod|Permissions") bool HasPermission(FName PermissionId) const;
	UFUNCTION(BlueprintPure, Category="Mod|Permissions") TArray<FName> GetGrantedPermissions() const;

	UFUNCTION(BlueprintCallable, Category="Mod|Save") bool SaveJson(const FString& Json, int32 DataVersion = 1);
	UFUNCTION(BlueprintCallable, Category="Mod|Save") bool LoadJson(FString& OutJson, int32& OutDataVersion) const;

	UFUNCTION(BlueprintCallable, Category="Mod|Assets") TArray<FAssetData> GetOwnAssets(UClass* ClassFilter) const;
	UFUNCTION(BlueprintCallable, Category="Mod|Assets", meta=(DeterminesOutputType="AssetClass"))
	UObject* LoadOwnAsset(FName AssetName, UClass* AssetClass) const;

	/** Objects created through the context are tracked and released when the mod unloads. */
	UFUNCTION(BlueprintCallable, Category="Mod", meta=(DeterminesOutputType="ObjectClass"))
	UObject* CreateModObject(TSubclassOf<UObject> ObjectClass);

	UFUNCTION(BlueprintCallable, Category="Mod") void LogInfo(const FString& Message) const;
	UFUNCTION(BlueprintCallable, Category="Mod") void LogWarning(const FString& Message) const;
	UFUNCTION(BlueprintCallable, Category="Mod") void LogError(const FString& Message) const;

	UFUNCTION(BlueprintPure, Category="Mod") UModSubsystem* GetSubsystem() const;   // note: still gated by permissions elsewhere
	virtual UWorld* GetWorld() const override;
};

UCLASS(Abstract, Blueprintable, BlueprintType)
class MODFRAMEWORK_API UModEntryPointBase : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadOnly, Category="Mod") TObjectPtr<UModContext> Context;

	virtual void NativeOnModLoaded();
	virtual void NativeOnModActivated();
	virtual void NativeOnModDeactivated();
	virtual void NativeOnModUnloaded();

	UFUNCTION(BlueprintImplementableEvent, Category="Mod", meta=(DisplayName="On Mod Loaded"))      void OnModLoaded();
	UFUNCTION(BlueprintImplementableEvent, Category="Mod", meta=(DisplayName="On Mod Activated"))   void OnModActivated();
	UFUNCTION(BlueprintImplementableEvent, Category="Mod", meta=(DisplayName="On Mod Deactivated")) void OnModDeactivated();
	UFUNCTION(BlueprintImplementableEvent, Category="Mod", meta=(DisplayName="On Mod Unloaded"))    void OnModUnloaded();

	virtual UWorld* GetWorld() const override;
};
```

## 22. `Subsystem/ModSubsystem.h`

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnModStateChangedBP, FModId, ModId, EModState, OldState, EModState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnModsRefreshedBP);

UCLASS(BlueprintType)
class MODFRAMEWORK_API UModSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	// USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintPure, Category="Mod", meta=(WorldContext="WorldContextObject"))
	static UModSubsystem* Get(const UObject* WorldContextObject);

	// --- Lifecycle -----------------------------------------------------------
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") int32 DiscoverMods();
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") FModResolveResult ResolveDependencies();
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") bool MountMod(FModId ModId);
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") bool UnmountMod(FModId ModId);
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") bool LoadMod(FModId ModId);
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") bool UnloadMod(FModId ModId);
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") bool ActivateMod(FModId ModId);
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") bool DeactivateMod(FModId ModId);
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") bool ReloadMod(FModId ModId);
	/** Discover -> resolve -> mount -> load -> (optionally) activate, honouring settings. */
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") void RefreshMods(bool bActivate = true);
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") int32 LoadAllMods();
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") int32 ActivateAllMods();
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") int32 DeactivateAllMods();
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") int32 UnloadAllMods();
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") void ReloadAllMods();
	UFUNCTION(BlueprintCallable, Category="Mod|Lifecycle") bool SetModEnabled(FModId ModId, bool bEnabled);

	// --- Registration (game side) -------------------------------------------
	UFUNCTION(BlueprintCallable, Category="Mod|API") bool RegisterGameAPI(UModAPI* Api);
	UFUNCTION(BlueprintCallable, Category="Mod|API") bool RegisterGameAPIByClass(TSubclassOf<UModAPI> ApiClass);
	UFUNCTION(BlueprintCallable, Category="Mod|Extensions") bool RegisterExtensionPoint(const FModExtensionPointDescriptor& Descriptor);
	UFUNCTION(BlueprintCallable, Category="Mod|Events") bool RegisterEventType(const FModEventDescriptor& Descriptor);
	void RegisterProvider(TSharedRef<IModProvider> Provider);
	void UnregisterProvider(FName ProviderId);
	TArray<FName> GetProviderIds() const;
	IModProvider* FindProvider(FName ProviderId) const;

	// --- Accessors -----------------------------------------------------------
	UFUNCTION(BlueprintPure, Category="Mod") UModRegistry* GetRegistry() const;
	UFUNCTION(BlueprintPure, Category="Mod") UModAPIRegistry* GetAPIRegistry() const;
	UFUNCTION(BlueprintPure, Category="Mod") UModExtensionRegistry* GetExtensionRegistry() const;
	UFUNCTION(BlueprintPure, Category="Mod") UModEventBus* GetEventBus() const;
	UFUNCTION(BlueprintPure, Category="Mod") UModPermissionRegistry* GetPermissionRegistry() const;
	UFUNCTION(BlueprintPure, Category="Mod") UModSaveDataManager* GetSaveDataManager() const;
	UFUNCTION(BlueprintPure, Category="Mod") UModContext* GetModContext(FModId ModId) const;
	FModContentManager& GetContentManager();
	const FModContentManager& GetContentManager() const;

	UFUNCTION(BlueprintPure, Category="Mod") FModEnvironment GetEnvironment() const;
	UFUNCTION(BlueprintPure, Category="Mod") FModResolveResult GetLastResolveResult() const;
	UFUNCTION(BlueprintPure, Category="Mod") TArray<FModConflict> GetConflicts() const;
	UFUNCTION(BlueprintPure, Category="Mod") FModSessionManifest BuildSessionManifest() const;
	UFUNCTION(BlueprintPure, Category="Mod") TArray<FModDiagnostic> GetDiagnostics() const;

	// --- Delegates -----------------------------------------------------------
	UPROPERTY(BlueprintAssignable, Category="Mod") FOnModStateChangedBP OnModStateChanged;
	UPROPERTY(BlueprintAssignable, Category="Mod") FOnModsRefreshedBP OnModsRefreshed;
};
```
Lifecycle implementation rules:
- Every state transition goes through `UModRegistry::SetModState` and then broadcasts the matching
  `ModFrameworkEvents::*` on the event bus. Never set state directly.
- `LoadMod` requires state `Mounted`; `ActivateMod` requires `Loaded`. Loading instantiates the
  entry point class and calls `NativeOnModLoaded`; activation calls `NativeOnModActivated` and
  registers the content bundles' extensions. **Loaded and Activated stay separate.**
- `UnloadMod` deactivates first if needed, releases mod objects, unregisters APIs/extensions/event
  subscriptions for that mod, then unmounts.
- Failures never throw; they set `EModState::Failed` with a reason and a diagnostic.

## 23. Console commands (`Private/Debug/ModConsoleCommands.h/.cpp`)

Registered only when `!UE_BUILD_SHIPPING || ALLOW_CONSOLE`, and gated on
`UModFrameworkSettings::bEnableConsoleCommands`. Implement with `FAutoConsoleCommandWithWorldAndArgs`.
Commands: `Mod.List`, `Mod.Info <ModId>`, `Mod.Load <ModId>`, `Mod.Unload <ModId>`,
`Mod.Activate <ModId>`, `Mod.Deactivate <ModId>`, `Mod.Reload <ModId>`, `Mod.ReloadAll`,
`Mod.Refresh`, `Mod.Validate`, `Mod.Dependencies <ModId>`, `Mod.Mounts`, `Mod.DumpRegistry`,
`Mod.DumpAPIs`, `Mod.DumpExtensions`, `Mod.DumpEvents`, `Mod.DumpPermissions`, `Mod.Conflicts`,
`Mod.SessionManifest`, `Mod.Enable <ModId>`, `Mod.Disable <ModId>`.
All output goes to `LogModFramework` **and** to the in-game console output device when available.

## 24. Tests

`Source/ModFramework/Private/Tests/*.spec.cpp`, wrapped in `#if WITH_DEV_AUTOMATION_TESTS`.
Use `DEFINE_SPEC` / `BEGIN_DEFINE_SPEC` style with flags
`EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter`.
Test name prefix: `ModFramework.<Area>`.
Tests must not require a world, a game instance, or on-disk mods except where they create temp files
under `FPaths::AutomationTransientDir()`.

## 25. Reflection metadata contract (used by SDK generation)

| Metadata | Applies to | Meaning |
|---|---|---|
| `ModPublic` | UCLASS/USTRUCT/UENUM/UINTERFACE/UFUNCTION/UPROPERTY | Include in the generated SDK |
| `ModApiId` | UCLASS deriving from UModAPI | Stable API id |
| `ModApiVersion` | UCLASS deriving from UModAPI | Semver of the API |
| `ModApiPermissions` | UCLASS deriving from UModAPI | Comma-separated permission ids |
| `ModApiServerAuthoritative` | UCLASS deriving from UModAPI | `"true"` marks it server-authoritative |
| `ModExtensionPoint` | UCLASS deriving from UModExtension | The extension point id this base class serves |
| `ModSince` | any | SDK version the symbol appeared in |
| `ModDeprecated` | any | SDK version the symbol was deprecated in |

---

## 26. Mod icons (ADDED after phase 2 — additive, breaks nothing)

A mod may ship an icon so a game can populate a mod-browser UI.

**Hard requirement: the icon must be loadable BEFORE the mod is mounted.** A mod list has to show
icons for mods that are merely discovered, that are disabled, or that were *rejected* — which is
exactly when a player most needs to identify one. So the icon is a plain image file at the mod root,
never a `.uasset` inside the pak.

### Manifest (additive — `manifestVersion` stays 1)

```json
"icon": "Icon.png"
```

- `FModManifest::IconPath` — `FString`, optional, relative to the mod root.
- Validation, code `Manifest.InvalidIcon`: reject absolute paths, `..` segments, backslashes, a
  leading slash, and any extension other than `.png`, `.jpg`, `.jpeg` (case-insensitive). Same
  containment rules as `FModContentRoot::RelativePath` — this is untrusted input.
- Absent or empty means "no icon"; that is not a warning.

### Runtime resolution

- `FModInfo::ResolvedIconPath` (`FString`) — absolute path for a loose mod folder; for a mod
  discovered as a `.mod` package it stays empty and the bytes come from
  `FModPackageReader::ReadEntry(IconPath, ...)`, which already works without extracting.

### `Public/Content/ModIconCache.h` + `Private/Content/ModIconCache.cpp`

```cpp
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnModIconLoaded, FModId, ModId, UTexture2D*, Icon);

UCLASS(BlueprintType)
class MODFRAMEWORK_API UModIconCache : public UObject
{
	GENERATED_BODY()
public:
	void Initialize(class UModSubsystem* InSubsystem);
	void Shutdown();

	/** Cached texture or nullptr. Never blocks, never decodes. Safe to call every UI frame. */
	UFUNCTION(BlueprintPure, Category="Mod|Icons") UTexture2D* FindIcon(FModId ModId) const;

	/** Reads and validates off the game thread, then imports the texture on it. */
	UFUNCTION(BlueprintCallable, Category="Mod|Icons")
	void RequestIcon(FModId ModId, FOnModIconLoaded OnLoaded);

	/** Blocking. Editor/tooling only — do not call from gameplay. */
	UTexture2D* LoadIconSynchronous(const FModId& ModId, FModDiagnostic& OutError);

	UFUNCTION(BlueprintCallable, Category="Mod|Icons") void ReleaseIcon(FModId ModId);
	UFUNCTION(BlueprintCallable, Category="Mod|Icons") void ReleaseAll();
	/** Fallback for mods that ship no icon. From UModFrameworkSettings::DefaultModIcon. */
	UFUNCTION(BlueprintPure, Category="Mod|Icons") UTexture2D* GetDefaultIcon() const;
	UFUNCTION(BlueprintPure, Category="Mod|Icons") bool HasIcon(FModId ModId) const;

private:
	UPROPERTY() TMap<FModId, TObjectPtr<UTexture2D>> Icons;   // keeps them alive against GC
};
```

Threading: file read, size cap and `IImageWrapperModule::DetectImageFormat` happen on a task thread;
`FImageUtils::ImportBufferAsTexture2D` and the delegate fire on the game thread. In-flight requests
for the same mod coalesce rather than reading twice. A request for an already-cached icon fires the
delegate on the next tick — never synchronously, so callers cannot depend on ordering.

Verified engine APIs (do not substitute):
- `FImageUtils::ImportBufferAsTexture2D(const TArray<uint8>&)` → `Engine/Public/ImageUtils.h`, `ENGINE_API`
- `IImageWrapperModule::DetectImageFormat(const void*, int64)` → `Runtime/ImageWrapper/Public/IImageWrapperModule.h`
- Add `"ImageWrapper"` to `ModFramework.Build.cs` public dependencies.

### Settings additions (`UModFrameworkSettings`)

```cpp
UPROPERTY(config, EditAnywhere, Category = "Icons") int64 MaxIconFileBytes = 2 * 1024 * 1024;
UPROPERTY(config, EditAnywhere, Category = "Icons") int32 MaxIconDimension = 1024;
UPROPERTY(config, EditAnywhere, Category = "Icons") TSoftObjectPtr<UTexture2D> DefaultModIcon;
```

### Diagnostic codes

`Icon.NotFound`, `Icon.TooLarge`, `Icon.UnsupportedFormat`, `Icon.DecodeFailed`,
`Icon.DimensionsExceeded`, `Icon.UnsafePath`.

Every failure returns the default icon rather than nullptr, so UI code never needs a null branch.
A bad icon must never affect whether the mod itself loads.

### Console

`Mod.Icons` — lists each mod, whether it declares an icon, and whether it is currently cached.

---

## 27. Return-value protocol for agents

Return a compact JSON object:
```json
{ "files": ["relative/path.h", "..."], "notes": "anything downstream agents must know",
  "concerns": ["contract problems you had to work around"] }
```
Do not paste file contents into the return value.
