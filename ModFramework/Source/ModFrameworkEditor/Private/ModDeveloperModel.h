// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "Containers/Ticker.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Dependencies/ModDependencyTypes.h"
#include "Internationalization/Text.h"
#include "Manifest/ModManifest.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "ModDeveloperModel.generated.h"

class FModDeveloperModel;
class UModSubsystem;

/**
 * One mod as the developer window needs it, flattened out of either the live registry or an offline
 * scan so that every panel reads the same shape whichever of the two produced it.
 *
 * Every string in here originated in a mod.json somebody else wrote. Nothing is trusted: the window
 * displays these values, it never parses meaning back out of them.
 */
struct FModDeveloperModRow
{
	FModId Id;

	/** Manifest display name, falling back to the raw id. Never empty for a row with a valid id. */
	FString DisplayName;

	FString Author;

	/** Manifest version rendered as semver text. */
	FString VersionText;

	/** Mod root directory, or the `.mod` file for a package that has not been installed. */
	FString RootPath;

	/** Absolute path of the icon image when one is known, otherwise empty. */
	FString IconPath;

	FModManifest Manifest;

	TArray<FModDiagnostic> Diagnostics;

	EModState State = EModState::Unknown;

	EModLoadFailureReason FailureReason = EModLoadFailureReason::None;

	FString FailureMessage;

	/** Resolved load order, or INDEX_NONE when the mod is not in the order. */
	int32 LoadOrder = INDEX_NONE;

	bool bEnabled = true;

	/** True when RootPath names a `.mod` container rather than an unpacked folder. */
	bool bPackaged = false;

	/** True when the manifest parsed and validated. A false row still carries its diagnostics. */
	bool bManifestValid = false;

	/** True when this row came from the running subsystem, so lifecycle actions apply to it. */
	bool bLive = false;
};

/**
 * The UObject the model needs in order to listen to UModSubsystem.
 *
 * UModSubsystem::OnModStateChanged and OnModsRefreshed are BlueprintAssignable, which makes them
 * DYNAMIC multicast delegates: they can only be bound to a UFUNCTION on a UObject, so a plain shared
 * C++ object cannot subscribe directly. This proxy is the smallest thing that can, and it forwards
 * to the model through a weak pointer so a subsystem event arriving after the window closed is a
 * no-op rather than a use-after-free.
 */
UCLASS()
class UModDeveloperEventProxy : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleModStateChanged(FModId ModId, EModState OldState, EModState NewState);

	UFUNCTION()
	void HandleModsRefreshed();

	/** Deliberately not a UPROPERTY: FModDeveloperModel is a plain C++ type, not a UObject. */
	TWeakPtr<FModDeveloperModel> Owner;
};

/**
 * Everything the Mod Developer window knows, in one place, refreshed as a whole.
 *
 * TWO SOURCES, ONE SHAPE
 * With a game instance running (Play In Editor, or a game launched in-process) the model mirrors the
 * live UModSubsystem: real states, the real load order, the real conflict report. With no game
 * instance - the normal state of an editor sitting idle - it falls back to an OFFLINE SCAN that runs
 * FLocalFileModProvider, FModDependencyResolver and FModConflictDetector directly over the configured
 * search directories. Those three are pure, game-agnostic and side-effect free, so the offline view
 * is a true prediction of what the game would do rather than a mock-up.
 *
 * The difference is never hidden. IsLive() is false offline, lifecycle actions are disabled, and
 * GetSourceDescription() says so in a sentence a mod author can act on.
 *
 * REFRESH IS EVENT DRIVEN
 * The model subscribes to UModSubsystem::OnModStateChanged and OnModsRefreshed (through
 * UModDeveloperEventProxy) and to the editor's PIE begin/end delegates. It never ticks. A burst of
 * state changes - which is exactly what one RefreshMods pass produces - is coalesced into a single
 * rebuild on the next frame, so a hundred transitions cost one refresh rather than a hundred.
 *
 * Game thread only, like everything it reads.
 */
class FModDeveloperModel : public TSharedFromThis<FModDeveloperModel>
{
public:
	/** Creates a model and binds it to whatever is currently running. Always call this, not new. */
	static TSharedRef<FModDeveloperModel> Create();

	~FModDeveloperModel();

	FModDeveloperModel(const FModDeveloperModel&) = delete;
	FModDeveloperModel& operator=(const FModDeveloperModel&) = delete;

	/** Rebuilds everything from the current source and broadcasts OnChanged. Safe to call at will. */
	void Refresh();

	/** Rebuilds on the next frame, collapsing any number of calls made before then into one. */
	void RequestRefresh();

	/** True when a game instance is running and the rows describe live state. */
	bool IsLive() const { return bLive; }

	/** The live subsystem, or nullptr. Re-resolved on every refresh; never store the result. */
	UModSubsystem* GetSubsystem() const;

	const TArray<TSharedPtr<FModDeveloperModRow>>& GetMods() const { return Mods; }

	/** The row for one mod, or an invalid pointer. */
	TSharedPtr<FModDeveloperModRow> FindMod(const FModId& InId) const;

	/** The last resolve pass: load order, rejections and resolver diagnostics. */
	const FModResolveResult& GetResolveResult() const { return ResolveResult; }

	const TArray<FModConflict>& GetConflicts() const { return Conflicts; }

	/** Pipeline-level diagnostics that belong to no single mod. */
	const TArray<FModDiagnostic>& GetPipelineDiagnostics() const { return PipelineDiagnostics; }

	/** The directories the scan looked in, absolute and in scan order. */
	const TArray<FString>& GetSearchDirectories() const { return SearchDirectories; }

	/** One sentence naming where the current data came from, for the window's status strip. */
	FText GetSourceDescription() const;

	/** Fired at the end of every refresh, live or offline. */
	DECLARE_MULTICAST_DELEGATE(FOnModelChanged);
	FOnModelChanged OnChanged;

	/** Called by UModDeveloperEventProxy. Public only because the proxy is a separate class. */
	void HandleSubsystemEvent();

private:
	FModDeveloperModel() = default;

	/** Second half of Create(), split out because it needs a live TSharedRef to this. */
	void Initialize();

	void BindSubsystem(UModSubsystem* InSubsystem);
	void UnbindSubsystem();

	/** The subsystem of the first running game instance, or nullptr when none is running. */
	static UModSubsystem* FindLiveSubsystem();

	void RefreshFromSubsystem(UModSubsystem* InSubsystem);
	void RefreshFromDisk();

	/** PIE start and end both change which source applies, so both force a rebuild. */
	void HandlePieEvent(bool bIsSimulating);

	TArray<TSharedPtr<FModDeveloperModRow>> Mods;
	FModResolveResult ResolveResult;
	TArray<FModConflict> Conflicts;
	TArray<FModDiagnostic> PipelineDiagnostics;
	TArray<FString> SearchDirectories;

	bool bLive = false;

	/** Guards against a refresh triggered from inside a refresh by a delegate we ourselves fired. */
	bool bRefreshing = false;

	TWeakObjectPtr<UModSubsystem> BoundSubsystem;

	/** Strong: nothing else references the proxy, and it has to outlive every delegate it is on. */
	TStrongObjectPtr<UModDeveloperEventProxy> EventProxy;

	/** Valid while a coalesced refresh is queued for the next frame. */
	FTSTicker::FDelegateHandle PendingRefreshHandle;

	FDelegateHandle PostPieStartedHandle;
	FDelegateHandle EndPieHandle;
};
