// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Manifest/ModManifest.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "GameModValidator.generated.h"

/**
 * The stable machine-readable codes this validator emits.
 *
 * Every one is prefixed "GameMod." so a tool can tell a game-specific finding apart from the
 * framework's own "Manifest.*", "Dependency.*" and "Extension.*" codes at a glance. They are part of
 * the published contract with mod authors and with any packaging pipeline that greps for them:
 * never rename one, and never change what it means. Retire a code by no longer emitting it.
 */
namespace GameModValidationCodes
{
	/** Error. A claim names an extension point this game does not open. */
	GAMEMODSDK_API extern const FName UnknownExtensionPoint;

	/** Error. A claim carries no extension point id at all, so it can never match anything. */
	GAMEMODSDK_API extern const FName ClaimMissingExtensionPoint;

	/** Warning. A requested permission is neither a framework builtin nor one this SDK's surface uses. */
	GAMEMODSDK_API extern const FName UnknownPermission;

	/** Warning. A claim's extension point requires a permission the manifest never requests. */
	GAMEMODSDK_API extern const FName MissingPermissionForClaim;

	/** Error. `sdk.id` names a different SDK than this one. */
	GAMEMODSDK_API extern const FName SdkMismatch;

	/** Error. `sdk.version` is not valid range syntax, so the constraint the author meant is not applied. */
	GAMEMODSDK_API extern const FName SdkVersionRangeInvalid;

	/** Error. `sdk.version` is a well-formed range that this SDK's version can never satisfy. */
	GAMEMODSDK_API extern const FName SdkVersionUnsatisfiable;

	/** Warning/Info. The manifest pins no SDK, so nothing checks it against this game's modding surface. */
	GAMEMODSDK_API extern const FName SdkNotDeclared;

	/** Warning/Info. The project's configured SDK identity disagrees with this SDK binary. */
	GAMEMODSDK_API extern const FName SdkIdentityDrift;

	/** Info. Content bundles with no entry point class - legal, and worth saying out loud. */
	GAMEMODSDK_API extern const FName ContentBundleWithoutEntryPoint;

	/** Warning. The mod declares intent (claims, permissions) but has nothing that can ever run. */
	GAMEMODSDK_API extern const FName NoRuntimeSurface;

	/** Warning. The mod will register extensions but declares no claims, so conflict detection is blind to it. */
	GAMEMODSDK_API extern const FName NoClaimsDeclared;

	/** Every code above, in declaration order. For documentation generators and code-to-help tables. */
	GAMEMODSDK_API TArray<FName> GetAllCodes();
}

/**
 * The outcome of validating one manifest against this game's modding surface.
 *
 * bValid is exactly "no diagnostic has EModDiagnosticSeverity::Error". Warnings never make a mod
 * invalid: several of them describe shapes that are legal and occasionally deliberate, and a
 * packaging tool that refused them would be wrong about mods it has no business refusing. Show the
 * warnings, package anyway.
 */
USTRUCT(BlueprintType)
struct GAMEMODSDK_API FGameModValidationResult
{
	GENERATED_BODY()

	/** True when Diagnostics holds no Error. Warnings and Info entries do not affect it. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod|Validation")
	bool bValid = false;

	/** Findings in a fixed order: SDK identity, claims, permissions, then entry point. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod|Validation")
	TArray<FModDiagnostic> Diagnostics;

	int32 CountErrors() const;
	int32 CountWarnings() const;

	/** One line per diagnostic, in FModDiagnostic::ToString() form. Empty when there are none. */
	FString ToDebugString() const;

	/** "3 errors, 1 warning" / "no findings". For a packaging tool's one-line summary. */
	FString ToSummaryString() const;
};

/**
 * Game-specific pre-package validation: the checks only this game's SDK can make.
 *
 * FModManifestParser already decides whether a `mod.json` is *well formed* - field shapes, id
 * syntax, contained paths, parseable versions - and FModDependencyResolver decides at load time
 * whether a mod fits the environment it landed in. Between them sits a gap this class fills: a
 * manifest can be perfectly well formed, name an extension point that does not exist, request a
 * permission this game never defines, and pin an SDK range that no build of this SDK can satisfy.
 * Nothing refuses it. It packages, it installs, it loads, and then nothing happens - which is the
 * single hardest mod bug to diagnose from the other side of a support ticket.
 *
 * So this runs at package time, in the mod author's editor, where the answer is still cheap.
 *
 * What it deliberately does NOT do:
 *  - re-check anything the framework already checks. A finding here is always game-specific.
 *  - load assets, touch the mod subsystem, or need a running game. Everything it compares against is
 *    a native constant or a descriptor built from code, so it works in a mod author's project that
 *    has the SDK and no game.
 *  - decide anything from project configuration. See GameModSDKVersion.h for why, and
 *    ValidateSdkEnvironment below for the one check that does look at configuration, on purpose.
 *
 * Every manifest handed to this class is untrusted mod-supplied data: nothing here asserts, and an
 * empty, absent or nonsensical field always produces a diagnostic rather than a crash.
 */
UCLASS(BlueprintType)
class GAMEMODSDK_API UGameModValidator : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Validates one manifest against this game's modding surface.
	 *
	 * Errors (the mod cannot work as written):
	 *  - a claim naming an extension point this game does not open, or naming none at all;
	 *  - `sdk.id` naming a different SDK;
	 *  - `sdk.version` that is not valid range syntax, or that this SDK's version cannot satisfy.
	 *
	 * Warnings (the mod will load, and probably not do what its author expects):
	 *  - a permission nothing has registered, which is never granted and so silently disables
	 *    whatever depended on it;
	 *  - a claim on a point whose required permission the manifest does not request;
	 *  - no SDK pinned at all;
	 *  - nothing that can run, yet claims or permissions declared;
	 *  - extensions incoming but no claims declared, so conflict detection cannot see the mod.
	 *
	 * Deterministic: the same manifest always produces the same diagnostics in the same order.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Validation")
	static FGameModValidationResult ValidateManifest(const FModManifest& Manifest);

	/**
	 * Checks that the project's configured SDK identity matches the SDK binary it ships.
	 *
	 * This is the one place configuration is read, and it is about the *game*, not about any mod, so
	 * it is a separate call rather than part of every manifest's result: run it once per packaging
	 * session, not once per mod.
	 *
	 * It matters because the two authorities are consulted at different times. ValidateManifest
	 * compares a mod against GameModSDK::GetSdkId() / GetSdkVersion() at package time;
	 * FModDependencyResolver compares the same mod against `UModFrameworkSettings::SdkId` /
	 * `SdkVersion` at load time. When those disagree, a mod passes validation and is then rejected on
	 * the player's machine with EModLoadFailureReason::IncompatibleSdk - the exact class of failure
	 * this validator exists to move forward in time.
	 *
	 * Only ever produces Warnings and Info, so bValid is always true.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Validation")
	static FGameModValidationResult ValidateSdkEnvironment();

	/** True when GameModExtensionPoints::GetAllPointIds() contains this id. False for None. */
	UFUNCTION(BlueprintPure, Category = "Mod|Validation")
	static bool IsKnownExtensionPoint(FName ExtensionPointId);

	/**
	 * Every permission id a manifest may name without earning a warning: the framework's builtins
	 * from ModPermissions::GetBuiltinPermissions(), plus every permission this SDK's own surface
	 * requires - the RequiredPermissions of the extension point descriptors and the permission each
	 * UModAPI in this SDK gates itself with.
	 *
	 * Derived rather than hand-listed, so adding a capability to the SDK cannot leave a second list
	 * behind to rot. A game that registers further permissions of its own at runtime, through
	 * UModPermissionRegistry::RegisterPermission, is not visible from here - a manifest naming one of
	 * those draws a warning it does not deserve, which is the honest price of validating without the
	 * game present.
	 *
	 * Sorted by id, and free of None.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|Validation")
	static TArray<FName> GetKnownPermissionIds();
};
