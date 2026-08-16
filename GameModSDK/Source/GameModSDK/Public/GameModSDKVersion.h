// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Manifest/ModVersion.h"

/**
 * Reverse-DNS identifier of this SDK.
 *
 * It is the string a mod author writes in `mod.json`:
 *
 *     "sdk": { "id": "com.modframework.sample.sdk", "version": "^0.1.0" }
 *
 * It is part of the published contract with mod authors and must never change once a single mod has
 * shipped: FModDependencyResolver rejects a mod whose `sdk.id` does not match this game's, with
 * EModLoadFailureReason::IncompatibleSdk, so renaming it silently uninstalls every mod in the wild.
 * A game that forks this SDK changes it exactly once, before publishing anything.
 */
#define GAMEMODSDK_SDK_ID TEXT("com.modframework.sample.sdk")

/**
 * Semantic version of this SDK's *mod-facing surface* - the extension point contracts, the UModAPI
 * signatures, the event payloads, the data asset shapes. It has nothing to do with the game's own
 * build number.
 *
 * Bump MAJOR when anything a shipped mod compiles or binds against changes incompatibly: removing an
 * extension point, changing a base class's virtual signature, renaming an event payload field.
 * Bump MINOR when the surface grows without breaking what is already there. Bump PATCH for fixes
 * behind the surface.
 *
 * Keep this in step with GameModSDK.uplugin's "VersionName" and with `SdkVersion` in the project's
 * DefaultGame.ini - UGameModValidator::ValidateSdkEnvironment reports it when they drift.
 *
 * While MAJOR is 0 the surface is explicitly unstable, and npm caret semantics say so: "^0.1.0"
 * admits >=0.1.0 <0.2.0, so every minor bump before 1.0.0 invalidates mods that pinned the previous
 * one. That is intended - it is the pre-1.0 promise, not a bug.
 */
#define GAMEMODSDK_VERSION_MAJOR 0
#define GAMEMODSDK_VERSION_MINOR 1
#define GAMEMODSDK_VERSION_PATCH 0

/**
 * The identity of the SDK this binary actually is.
 *
 * These are native constants rather than project settings on purpose. `UModFrameworkSettings::SdkId`
 * and `SdkVersion` describe what a *project* claims to ship, are editable per branch and per
 * packaging configuration, and are read at runtime; the compatibility of a mod compiled against this
 * SDK is a property of the compiled code, so the authority for it lives next to the code. Anything
 * that has to answer "can this mod work with this SDK?" - above all UGameModValidator, which answers
 * it at package time, long before a game's configuration is in the picture - asks here.
 *
 * The two are still expected to agree, because the runtime dependency resolver reads the settings
 * and only the settings. UGameModValidator::ValidateSdkEnvironment exists to catch the drift.
 */
namespace GameModSDK
{
	/** "com.modframework.sample.sdk". Compared case-insensitively, exactly like the resolver does. */
	GAMEMODSDK_API FString GetSdkId();

	/** The MAJOR.MINOR.PATCH above as an FModVersion. Never 0.0.0, which the framework reads as "unknown". */
	GAMEMODSDK_API FModVersion GetSdkVersion();

	/** The same version rendered as "MAJOR.MINOR.PATCH". */
	GAMEMODSDK_API FString GetSdkVersionString();
}
