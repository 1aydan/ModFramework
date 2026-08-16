// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/DelegateCombinations.h"
#include "Manifest/ModManifest.h"
#include "Permissions/ModPermissions.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptInterface.h"

#include "ModPermissionRegistry.generated.h"

/**
 * The permission decisions recorded for a single mod.
 *
 * This exists only because a `TMap<FModId, TMap<FName, EModPermissionState>>` is not a legal
 * UPROPERTY - the reflection system cannot describe a container nested directly inside another
 * container - so the inner map is wrapped in a struct that it can.
 *
 * A permission absent from States has never been evaluated for that mod, which the registry reports
 * as EModPermissionState::NotRequested.
 */
USTRUCT()
struct FModPermissionStateMap
{
	GENERATED_BODY()

	UPROPERTY()
	TMap<FName, EModPermissionState> States;
};

/** Raised with (mod id, permission id) whenever a recorded permission state changes. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnModPermissionChanged, const FModId&, FName);

/**
 * Decides which capabilities each mod is allowed to use, and remembers the answers.
 *
 * The registry holds two things: a catalogue of permissions that exist (the builtins from
 * ModPermissions:: plus whatever the game registers), and a per-mod record of what each mod asked
 * for and what it got. It never enforces anything itself - enforcement is the job of whoever guards
 * the capability, which asks HasPermission before doing the dangerous thing.
 *
 * Evaluation order for each permission a manifest requests, first rule that applies wins:
 *
 *   1. The permission policy, when one is installed and it returns something other than
 *      NotRequested. The game's own judgement always beats the framework's.
 *   2. UModFrameworkSettings::AlwaysDeniedPermissions -> Denied. A permission listed here is out of
 *      reach of the default rules entirely.
 *   3. A permission nothing registered -> Denied when bDenyUnknownPermissions is set, otherwise
 *      Pending. Either way the log names the mod and the permission, because this is almost always
 *      a typo in a manifest.
 *   4. UModFrameworkSettings::AutoGrantedPermissions and not dangerous -> Granted.
 *   5. Anything else -> Pending, and OnPermissionRequested fires so the game can prompt.
 *
 * A dangerous permission is never auto-granted, whatever the project configuration says; listing one
 * in AutoGrantedPermissions produces a warning and leaves the request Pending.
 *
 * Threading: game thread only, like every other UObject in the framework.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModPermissionRegistry : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Registers the builtin permissions.
	 *
	 * Safe to call more than once; a builtin id already present is overwritten with the builtin
	 * descriptor. Register a game's own permissions *after* this, so that they are not clobbered.
	 */
	void Initialize();

	/**
	 * Returns the registry to the state Initialize leaves it in: every recorded per-mod decision and
	 * every custom permission is dropped, and the builtins are registered again.
	 *
	 * The permission policy is deliberately kept. It is game infrastructure rather than mod state,
	 * and silently dropping it would quietly weaken the gate at exactly the moment - a reload - when
	 * a game is least likely to notice. Clear it explicitly with SetPolicy(nullptr).
	 *
	 * No delegates fire: use ResetForMod when a single mod's listeners need telling.
	 */
	void Reset();

	/**
	 * Adds or replaces a permission descriptor.
	 *
	 * A descriptor with no PermissionId is ignored with a warning. Re-registering an existing id
	 * replaces its descriptor, and a change to bDangerous is logged because it silently changes what
	 * auto-granting will do.
	 */
	void RegisterPermission(const FModPermissionDescriptor& InDescriptor);

	/** Every registered descriptor, sorted by permission id. */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	TArray<FModPermissionDescriptor> GetAllPermissions() const;

	/** Looks up one descriptor. Out is left untouched when the permission is not registered. */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	bool GetPermission(FName PermissionId, FModPermissionDescriptor& Out) const;

	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	bool IsPermissionRegistered(FName PermissionId) const;

	/**
	 * Evaluates every permission in Manifest.RequestedPermissions and stores the results.
	 *
	 * The mod's previous record is replaced rather than merged, so re-evaluating the same manifest
	 * always produces the same answer instead of accumulating whatever an earlier pass decided. That
	 * means an explicit GrantPermission made before this call does not survive it; evaluate first,
	 * then grant.
	 *
	 * OnPermissionChanged fires once for every permission whose state actually moved, and
	 * OnPermissionRequested fires once for every permission left Pending. Both are broadcast after
	 * the new state is fully stored, so a handler may call straight back into the registry.
	 *
	 * Manifest is untrusted: an empty mod id or an empty permission id is logged and skipped, never
	 * asserted on.
	 */
	void EvaluateManifest(const FModManifest& Manifest);

	/** NotRequested when the mod never asked for this permission, or was never evaluated. */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	EModPermissionState GetPermissionState(const FModId& ModId, FName PermissionId) const;

	/** True only for EModPermissionState::Granted. Pending is not permission to do anything. */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	bool HasPermission(const FModId& ModId, FName PermissionId) const;

	/** Granted permissions of one mod, sorted by id. */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	TArray<FName> GetGrantedPermissions(const FModId& ModId) const;

	/** Permissions awaiting a decision, sorted by id. This is the list a consent prompt shows. */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	TArray<FName> GetPendingPermissions(const FModId& ModId) const;

	/** Permissions that were refused, sorted by id. Useful for explaining a degraded mod to a player. */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	TArray<FName> GetDeniedPermissions(const FModId& ModId) const;

	/**
	 * True when every listed permission is Granted for this mod. An empty list is trivially true.
	 *
	 * Entries with no id are skipped: an empty FName names no capability, so there is nothing to
	 * withhold. This is the check an API or extension point gate should use for its
	 * RequiredPermissions array.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod|Permissions")
	bool HasAllPermissions(const FModId& ModId, const TArray<FName>& PermissionIds) const;

	/**
	 * Grants a permission, typically because a player answered a prompt.
	 *
	 * A permission listed in UModFrameworkSettings::AlwaysDeniedPermissions cannot be granted this
	 * way: the call logs a warning and records Denied instead. Nothing else is refused, so a game may
	 * grant a dangerous permission once it has actually asked.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Permissions")
	void GrantPermission(const FModId& ModId, FName PermissionId);

	/** Records a refusal. Always succeeds; a denied permission stays denied until re-evaluated. */
	UFUNCTION(BlueprintCallable, Category = "Mod|Permissions")
	void DenyPermission(const FModId& ModId, FName PermissionId);

	/**
	 * Grants every registered permission - plus anything this mod already asked for - to one mod.
	 *
	 * This bypasses the entire permission system and exists purely as a development convenience, so
	 * it logs at Error level with a "SECURITY" prefix naming the mod. Permissions in
	 * AlwaysDeniedPermissions are still refused, because "no mod ever receives this" has to mean it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Permissions")
	void GrantAll(const FModId& ModId);

	/**
	 * Forgets every decision recorded for one mod, broadcasting OnPermissionChanged for each of them.
	 * Call this when a mod is unloaded so a later reload starts from a clean slate.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Permissions")
	void ResetForMod(const FModId& ModId);

	/**
	 * Installs the game's permission policy, or clears it when InPolicy holds no object.
	 *
	 * An object that does not implement IModPermissionPolicy is rejected with a warning and the
	 * previous policy is kept - refusing to install is safer than silently running with none.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|Permissions")
	void SetPolicy(TScriptInterface<IModPermissionPolicy> InPolicy);

	/** Fired with (mod id, permission id) whenever a recorded state changes. */
	FOnModPermissionChanged OnPermissionChanged;

	/** Raised for permissions left Pending so the game can drive UI. */
	FOnModPermissionChanged OnPermissionRequested;

private:
	/**
	 * Asks the installed policy about one permission.
	 * Returns NotRequested when there is no policy, the policy object died, it does not actually
	 * implement the interface, or it returned a value outside the enum.
	 */
	EModPermissionState ResolveWithPolicy(const FModManifest& Manifest, FName PermissionId) const;

	/** Writes one state without broadcasting. Returns true when the stored value actually changed. */
	bool SetStateInternal(const FModId& ModId, FName PermissionId, EModPermissionState NewState);

	/** Current state of one permission without creating an entry for it. */
	EModPermissionState FindState(const FModId& ModId, FName PermissionId) const;

	/** Ids of a mod's permissions whose state equals Filter, sorted lexically. */
	TArray<FName> CollectStates(const FModId& ModId, EModPermissionState Filter) const;

	/** Every permission that exists, keyed by id. */
	UPROPERTY()
	TMap<FName, FModPermissionDescriptor> Descriptors;

	/** Per-mod decisions. Absent mod, or absent permission within a mod, means NotRequested. */
	UPROPERTY()
	TMap<FModId, FModPermissionStateMap> ModStates;

	/** The game's override, if it installed one. Held as a UPROPERTY so it is not collected. */
	UPROPERTY()
	TScriptInterface<IModPermissionPolicy> Policy;

	/**
	 * Permission ids already reported as "dangerous, yet listed in AutoGrantedPermissions".
	 * The project configuration is wrong once, not once per mod, so the warning is logged once.
	 */
	TSet<FName> ReportedDangerousAutoGrants;
};
