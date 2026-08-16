// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Manifest/ModManifest.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "ModInfo.generated.h"

/**
 * Everything the framework knows about one mod in the current session.
 *
 * FModInfo is the union of what a provider discovered (Manifest, RootPath, ProviderId), what the
 * framework decided (State, FailureReason, LoadOrder, bEnabled, GrantedPermissions) and what it did
 * (MountedPaths, Diagnostics). It is a plain value type: UModRegistry owns the authoritative copies
 * and every accessor hands out a snapshot, so a caller that stashes an FModInfo is looking at the
 * past rather than at live state.
 *
 * Nothing in here is trusted. Manifest came from a file on disk that anybody may have written, so
 * treat every string in it as attacker-controlled and never assert on its contents.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModInfo
{
	GENERATED_BODY()

	/** The mod's parsed mod.json. Its Id doubles as this record's key in the registry. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModManifest Manifest;

	/** Where the mod currently sits in the lifecycle. Only UModRegistry::SetModState may change it. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModState State = EModState::Unknown;

	/** Why the mod last failed. Cleared when the mod leaves EModState::Failed. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	EModLoadFailureReason FailureReason = EModLoadFailureReason::None;

	/** Human readable companion to FailureReason. Cleared alongside it. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString FailureMessage;

	/** Absolute path of the mod root directory (the folder containing mod.json). */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString RootPath;

	/** Which IModProvider produced this record, e.g. FLocalFileModProvider::StaticProviderId(). */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FName ProviderId;

	/**
	 * Position in the resolved load order, assigned by UModRegistry::SetLoadOrder.
	 * INDEX_NONE means "not ordered yet"; such mods sort last.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	int32 LoadOrder = INDEX_NONE;

	/** False for a mod the player or the project settings switched off. Independent of State. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bEnabled = true;

	/** Permissions actually granted to this mod, a subset of Manifest.RequestedPermissions. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FName> GrantedPermissions;

	/** Virtual mount points this mod's content is currently mounted under, e.g. "/BetterCombat/". */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FString> MountedPaths;

	/** Everything validation, resolution, mounting and loading had to say about this mod. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModDiagnostic> Diagnostics;

	/**
	 * Absolute path of the mod's icon file when the mod was discovered as a loose folder and its
	 * manifest declares one. Empty for a mod discovered as a .mod package - there the bytes come
	 * from FModPackageReader::ReadEntry, which does not need the package extracted.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FString ResolvedIconPath;

	/** Shorthand for Manifest.Id: the stable key this record is filed under. */
	const FModId& GetId() const { return Manifest.Id; }

	/**
	 * True once the mod's code and content are live, i.e. in Loaded, Activated or Deactivated.
	 * Deliberately still true for Deactivated: a deactivated mod is loaded but idle, and unloading
	 * it is a separate step.
	 */
	bool IsLoaded() const
	{
		return static_cast<uint8>(State) >= static_cast<uint8>(EModState::Loaded)
			&& State != EModState::Unmounted
			&& State != EModState::Failed
			&& State != EModState::Disabled;
	}

	/** True only while the mod is actually running. */
	bool IsActive() const { return State == EModState::Activated; }
};

/**
 * Raised by UModRegistry after a mod's state has been committed, never while it is changing.
 * Parameters are the mod id, the state it left and the state it is now in.
 */
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnModStateChanged, const FModId&, EModState /*Old*/, EModState /*New*/);

/** Blueprint-facing spelling of FOnModStateChanged, for games that surface mod state in UI. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnModStateChangedDynamic, const FModId&, ModId, EModState, OldState, EModState, NewState);
