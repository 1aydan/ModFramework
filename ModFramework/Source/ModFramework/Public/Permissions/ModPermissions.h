// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Manifest/ModManifest.h"
#include "UObject/Interface.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "ModPermissions.generated.h"

/**
 * The capability ids the framework itself understands.
 *
 * A permission id is a lowercase, dot separated name. Games are free to register their own with
 * UModPermissionRegistry::RegisterPermission - these are simply the ones every game gets for free,
 * because every game's modding surface eventually needs them.
 *
 * The ids are the strings a mod author writes in `mod.json`:
 *
 *     "permissions": ["gameplay.modify", "save.modify"]
 *
 * They are part of the published contract with mod authors: never rename one, and never change what
 * a name means. Retire a capability by leaving its id registered and denying it.
 */
namespace ModPermissions
{
	/** "gameplay.modify" - change gameplay rules through the game's APIs and extension points. */
	MODFRAMEWORK_API extern const FName GameplayModify;

	/** "assets.read" - load and inspect assets owned by the game or by other mods. */
	MODFRAMEWORK_API extern const FName AssetsRead;

	/** "assets.write" - override assets the mod does not own. Dangerous. */
	MODFRAMEWORK_API extern const FName AssetsWrite;

	/** "filesystem.read" - read files outside the mod's own directory. Dangerous. */
	MODFRAMEWORK_API extern const FName FilesystemRead;

	/** "filesystem.write" - write files outside the mod's own directory. Dangerous. */
	MODFRAMEWORK_API extern const FName FilesystemWrite;

	/** "network" - open outbound network connections. Dangerous. */
	MODFRAMEWORK_API extern const FName Network;

	/** "console" - execute console commands. Dangerous. */
	MODFRAMEWORK_API extern const FName Console;

	/** "save.modify" - read and write the game's save data. */
	MODFRAMEWORK_API extern const FName SaveModify;

	/** "native_code" - load a native module shipped by the mod. Dangerous. */
	MODFRAMEWORK_API extern const FName NativeCode;

	/** "ui.modify" - add to or restyle the game's user interface. */
	MODFRAMEWORK_API extern const FName UiModify;

	/**
	 * Every builtin permission id, in the declaration order above.
	 *
	 * The order is stable across calls, so it is safe to use for anything that presents the builtins
	 * to a human (a settings page, a console dump) without sorting first.
	 */
	MODFRAMEWORK_API TArray<FName> GetBuiltinPermissions();
}

/**
 * Everything the framework knows about one permission: its id, how to describe it to a player, and
 * whether handing it out unattended would be reckless.
 *
 * Descriptors are supplied by whoever owns the capability - the framework for the builtins, the game
 * for anything it exposes through its own APIs. The registry stores them so that a permission prompt
 * can be built entirely from data, and so that a request naming a permission nobody registered is
 * recognisably a typo rather than a silent grant.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModPermissionDescriptor
{
	GENERATED_BODY()

	/** Lowercase dot separated id, e.g. "gameplay.modify". Matched exactly against manifests. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName PermissionId;

	/** Short label for a permission prompt. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FText DisplayName;

	/** One or two sentences telling a player what accepting this actually allows. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FText Description;

	/**
	 * Dangerous permissions are never auto-granted.
	 *
	 * Listing one in UModFrameworkSettings::AutoGrantedPermissions does not grant it; the request is
	 * left Pending and the project gets a warning in the log. A dangerous permission only ever
	 * becomes Granted through an explicit UModPermissionRegistry::GrantPermission call, a permission
	 * policy that says so, or the development-only GrantAll escape hatch.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bDangerous = false;
};

namespace ModPermissions
{
	/**
	 * Descriptors for every builtin permission, in GetBuiltinPermissions() order.
	 *
	 * UModPermissionRegistry::Initialize registers exactly this list. It is exposed so that editor
	 * tooling and documentation generators can describe the builtins without spinning up a registry.
	 */
	MODFRAMEWORK_API TArray<FModPermissionDescriptor> GetBuiltinPermissionDescriptors();
}

/**
 * Optional game-supplied override for the framework's permission decisions.
 *
 * The framework's own rules are deliberately blunt: they know nothing about who is playing, whether
 * the mod came from a trusted source, or whether the player has already answered a prompt about it.
 * A game that does know those things implements this interface - in C++ or in Blueprint - and hands
 * it to UModPermissionRegistry::SetPolicy.
 *
 * The policy is consulted first for every requested permission, and its verdict is final. Returning
 * EModPermissionState::NotRequested means "no opinion" and lets the default rules run.
 *
 * Note that the manifest passed in is untrusted mod-supplied data. Read it, but never trust it.
 */
UINTERFACE(BlueprintType, MinimalAPI)
class UModPermissionPolicy : public UInterface
{
	GENERATED_BODY()
};

class MODFRAMEWORK_API IModPermissionPolicy
{
	GENERATED_BODY()

public:
	/**
	 * Decides what happens to one permission request.
	 *
	 * @param Manifest      The requesting mod's manifest. Untrusted.
	 * @param PermissionId  The permission being requested. May be one nothing registered.
	 * @return Granted, Denied or Pending to decide; NotRequested to defer to the default rules.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Mod|Permissions")
	EModPermissionState ResolvePermission(const FModManifest& Manifest, FName PermissionId);
};
