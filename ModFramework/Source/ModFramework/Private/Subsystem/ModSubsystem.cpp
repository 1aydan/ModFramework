// Copyright (c) 2026. Licensed for use in your own projects.

#include "Subsystem/ModSubsystem.h"

#include "API/ModAPI.h"
#include "API/ModAPIRegistry.h"
#include "Algo/Reverse.h"
#include "Conflicts/ModConflictDetector.h"
#include "Conflicts/ModConflictTypes.h"
#include "Containers/Set.h"
#include "Content/ModIconCache.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkVersion.h"
#include "Dependencies/ModDependencyResolver.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Events/ModEventBus.h"
#include "Extensions/ModContentBundle.h"
#include "Extensions/ModExtensionRegistry.h"
#include "HAL/FileManager.h"
#include "Manifest/ModManifest.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"
#include "Packaging/ModPackageFormat.h"
#include "Permissions/ModPermissionRegistry.h"
#include "Providers/LocalFileModProvider.h"
#include "Registry/ModRegistry.h"
#include "Runtime/ModContext.h"
#include "Runtime/ModEntryPointBase.h"
#include "Config/ModConfigManager.h"
#include "Save/ModSaveDataManager.h"
#include "Settings/ModFrameworkSettings.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace ModSubsystemPrivate
{
	/** Stable machine-readable diagnostic codes produced by the subsystem. Tooling matches on these. */
	constexpr const TCHAR* CodeDiscoveryReport = TEXT("Subsystem.DiscoveryReport");
	constexpr const TCHAR* CodeDuplicateDiscovery = TEXT("Subsystem.DuplicateDiscovery");
	constexpr const TCHAR* CodeManifestInvalid = TEXT("Subsystem.ManifestInvalid");
	constexpr const TCHAR* CodeManifestVersion = TEXT("Subsystem.UnsupportedManifestVersion");
	constexpr const TCHAR* CodeMountFailed = TEXT("Subsystem.MountFailed");
	constexpr const TCHAR* CodeProviderError = TEXT("Subsystem.ProviderError");
	constexpr const TCHAR* CodeEntryPointMissing = TEXT("Subsystem.EntryPointMissing");
	constexpr const TCHAR* CodeEntryPointInvalid = TEXT("Subsystem.EntryPointInvalid");
	constexpr const TCHAR* CodeBundleUnresolved = TEXT("Subsystem.BundleUnresolved");
	constexpr const TCHAR* CodeBundleWrongType = TEXT("Subsystem.BundleWrongType");
	constexpr const TCHAR* CodeBundleExtension = TEXT("Subsystem.BundleExtensionRejected");
	constexpr const TCHAR* CodeBundlePointMissing = TEXT("Subsystem.BundleExtensionPointMissing");
	constexpr const TCHAR* CodeConflictBlocked = TEXT("Subsystem.ConflictBlocked");
	constexpr const TCHAR* CodePermissionsPending = TEXT("Subsystem.PermissionsPending");
	constexpr const TCHAR* CodePermissionsDenied = TEXT("Subsystem.PermissionsDenied");

	/** "<none>" rather than an empty string, so a log line never ends in a dangling quote pair. */
	FString DescribeModId(const FModId& ModId)
	{
		return ModId.IsValid() ? ModId.ToString() : FString(TEXT("<no id>"));
	}

	/**
	 * The ModFrameworkEvents id that announces arriving in a state, or NAME_None for the states that
	 * have no event of their own (Loading, DependenciesResolved, Disabled, Unknown).
	 *
	 * Keeping this mapping in one place is what lets UModSubsystem raise every lifecycle event from a
	 * single registry callback instead of trusting a dozen call sites to remember.
	 */
	FName LifecycleEventForState(EModState State)
	{
		switch (State)
		{
		case EModState::Discovered:  return ModFrameworkEvents::ModDiscovered;
		case EModState::Validated:   return ModFrameworkEvents::ModValidated;
		case EModState::Mounted:     return ModFrameworkEvents::ModMounted;
		case EModState::Loaded:      return ModFrameworkEvents::ModLoaded;
		case EModState::Activated:   return ModFrameworkEvents::ModActivated;
		case EModState::Deactivated: return ModFrameworkEvents::ModDeactivated;
		case EModState::Unmounted:   return ModFrameworkEvents::ModUnmounted;
		case EModState::Failed:      return ModFrameworkEvents::ModFailed;
		default:                     return NAME_None;
		}
	}

	/** Comma separated permission ids, for a one-line diagnostic. */
	FString JoinNames(const TArray<FName>& Names)
	{
		return FString::JoinBy(Names, TEXT(", "), [](FName Name) { return Name.ToString(); });
	}

	/** The first error in a batch, or the first entry of any severity, or an empty string. */
	FString SummariseDiagnostics(const TArray<FModDiagnostic>& In)
	{
		for (const FModDiagnostic& Diagnostic : In)
		{
			if (Diagnostic.IsError())
			{
				return Diagnostic.Message;
			}
		}

		return In.Num() > 0 ? In[0].Message : FString();
	}

	/**
	 * Absolute path of a mod's icon file, or an empty string.
	 *
	 * Empty covers three different situations and deliberately does not distinguish them here: the mod
	 * ships no icon, the declared path is not containable (which the manifest validator already
	 * refused, re-checked because an FModInfo can be built without going through it), or the mod was
	 * discovered as a `.mod` container and its icon has to be read out of the table of contents.
	 * UModIconCache handles all three.
	 */
	FString ResolveIconPath(const FString& RootPath, const FString& IconPath)
	{
		if (RootPath.IsEmpty() || IconPath.IsEmpty())
		{
			return FString();
		}

		FString PathError;
		if (!ModPackage::IsSafeRelativePath(IconPath, PathError))
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("Icon.UnsafePath: ignoring the icon path '%s' declared under '%s': %s"),
				*IconPath, *RootPath, *PathError);
			return FString();
		}

		if (!IFileManager::Get().DirectoryExists(*RootPath))
		{
			// A packaged mod. The bytes live inside the container; there is no loose file to point at.
			return FString();
		}

		return FPaths::ConvertRelativePathToFull(RootPath / IconPath);
	}

	/** True when the class can be instantiated right now. Covers everything a stale asset can be. */
	bool IsInstantiableClass(const UClass* Class)
	{
		return Class != nullptr
			&& !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
			&& IsValid(Class);
	}
}

//////////////////////////////////////////////////////////////////////////
// Lifetime

void UModSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogModFramework, Log, TEXT("Mod framework %s starting up."), *ModFrameworkVersion::GetString());

	// --- Owned objects -------------------------------------------------------------------------
	Registry = NewObject<UModRegistry>(this, TEXT("ModRegistry"));
	if (Registry)
	{
		Registry->Initialize();
		StateChangedHandle = Registry->OnModStateChanged.AddUObject(this, &UModSubsystem::HandleModStateChanged);
	}
	else
	{
		UE_LOG(LogModFramework, Error, TEXT("The mod registry could not be created; the framework will not load any mods."));
	}

	EventBus = NewObject<UModEventBus>(this, TEXT("ModEventBus"));
	PermissionRegistry = NewObject<UModPermissionRegistry>(this, TEXT("ModPermissionRegistry"));
	if (PermissionRegistry)
	{
		// Registers the builtin permission descriptors. A game adds its own on top, after this.
		PermissionRegistry->Initialize();
	}

	SaveDataManager = NewObject<UModSaveDataManager>(this, TEXT("ModSaveDataManager"));
	if (SaveDataManager)
	{
		SaveDataManager->Initialize(this);
	}

	ConfigManager = NewObject<UModConfigManager>(this, TEXT("ModConfigManager"));
	if (ConfigManager)
	{
		ConfigManager->Initialize(this);
	}

	IconCache = NewObject<UModIconCache>(this, TEXT("ModIconCache"));
	if (IconCache)
	{
		IconCache->Initialize(this);
	}

	ContentManager.Initialize();

	// --- The permission gate -------------------------------------------------------------------
	// The registries and the bus deliberately own no permission state; they ask through this one
	// delegate. Injecting it here is what makes UModPermissionRegistry the single source of truth.
	const UModAPIRegistry::FModPermissionCheck PermissionCheck =
		UModAPIRegistry::FModPermissionCheck::CreateUObject(this, &UModSubsystem::HandlePermissionCheck);

	if (Registry)
	{
		if (UModAPIRegistry* APIRegistry = Registry->GetAPIRegistry())
		{
			APIRegistry->SetPermissionCheck(PermissionCheck);
		}

		if (UModExtensionRegistry* ExtensionRegistry = Registry->GetExtensionRegistry())
		{
			ExtensionRegistry->SetPermissionCheck(PermissionCheck);
		}
	}

	if (EventBus)
	{
		EventBus->SetPermissionCheck(PermissionCheck);
	}

	// --- Providers -----------------------------------------------------------------------------
	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	TArray<FString> SearchDirectories;
	if (Settings)
	{
		SearchDirectories = Settings->GetResolvedSearchDirectories();
	}
	else
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("The mod framework settings object could not be reached; falling back to no search directories."));
	}

	LocalProvider = MakeShared<FLocalFileModProvider>(MoveTemp(SearchDirectories));
	RegisterProvider(LocalProvider.ToSharedRef());

	// --- World delegates -----------------------------------------------------------------------
	// OnPostWorldInitialization carries an InitializationValues argument this class has no use for, so
	// it is bound through a weak lambda rather than dragging the type into the public header.
	WorldCreatedHandle = FWorldDelegates::OnPostWorldInitialization.AddWeakLambda(this,
		[this](UWorld* World, const UWorld::InitializationValues /*InitValues*/)
		{
			HandleWorldCreated(World);
		});

	WorldDestroyedHandle = FWorldDelegates::OnPreWorldFinishDestroy.AddUObject(this, &UModSubsystem::HandleWorldDestroyed);
	GameStartedHandle = FWorldDelegates::OnStartGameInstance.AddUObject(this, &UModSubsystem::HandleGameInstanceStarted);

	bInitialized = true;

	if (Settings && Settings->bAutoDiscoverOnStartup)
	{
		RefreshMods(Settings->bAutoActivateLoadedMods);
	}
	else
	{
		UE_LOG(LogModFramework, Log, TEXT("Automatic mod discovery is disabled; call RefreshMods when the game is ready."));
	}
}

void UModSubsystem::Deinitialize()
{
	// Set first: every refusal path below reads it, and a handler firing during teardown must not be
	// able to start a new refresh.
	bInitialized = false;
	bRefreshInProgress = false;

	if (WorldCreatedHandle.IsValid())
	{
		FWorldDelegates::OnPostWorldInitialization.Remove(WorldCreatedHandle);
		WorldCreatedHandle.Reset();
	}

	if (WorldDestroyedHandle.IsValid())
	{
		FWorldDelegates::OnPreWorldFinishDestroy.Remove(WorldDestroyedHandle);
		WorldDestroyedHandle.Reset();
	}

	if (GameStartedHandle.IsValid())
	{
		FWorldDelegates::OnStartGameInstance.Remove(GameStartedHandle);
		GameStartedHandle.Reset();
	}

	// Reverse load order: a mod that loaded later may be using one that loaded earlier.
	const int32 TornDown = UnloadAllMods();
	if (TornDown > 0)
	{
		UE_LOG(LogModFramework, Log, TEXT("Mod framework shutdown tore down %d mod(s)."), TornDown);
	}

	// The state mirror is unbound before the registry goes, so nothing tries to broadcast on a bus
	// that is about to be reset.
	if (Registry && StateChangedHandle.IsValid())
	{
		Registry->OnModStateChanged.Remove(StateChangedHandle);
	}
	StateChangedHandle.Reset();

	ModContexts.Empty();
	EntryPoints.Empty();
	AppliedBundles.Empty();

	ContentManager.Shutdown();

	if (IconCache)
	{
		IconCache->Shutdown();
		IconCache = nullptr;
	}

	if (SaveDataManager)
	{
		SaveDataManager->Shutdown();
		SaveDataManager = nullptr;
	}

	if (ConfigManager)
	{
		ConfigManager->Shutdown();
		ConfigManager = nullptr;
	}

	if (EventBus)
	{
		EventBus->Reset();
		EventBus = nullptr;
	}

	if (PermissionRegistry)
	{
		PermissionRegistry->Reset();
		PermissionRegistry = nullptr;
	}

	if (Registry)
	{
		Registry->Shutdown();
		Registry = nullptr;
	}

	Providers.Empty();
	LocalProvider.Reset();

	LastResolveResult = FModResolveResult();
	Conflicts.Empty();
	Diagnostics.Empty();

	OnModStateChanged.Clear();
	OnModsRefreshed.Clear();

	Super::Deinitialize();
}

UModSubsystem* UModSubsystem::Get(const UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	if (const UModSubsystem* Self = Cast<UModSubsystem>(WorldContextObject))
	{
		return const_cast<UModSubsystem*>(Self);
	}

	if (const UGameInstance* DirectGameInstance = Cast<UGameInstance>(WorldContextObject))
	{
		return UGameInstance::GetSubsystem<UModSubsystem>(DirectGameInstance);
	}

	// The engine's own resolution first: it understands actors, components, widgets and every other
	// shape of world context object, and it answers null rather than asserting when asked to.
	const UWorld* World = nullptr;
	if (GEngine)
	{
		World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
	}

	if (!World)
	{
		World = WorldContextObject->GetWorld();
	}

	if (World)
	{
		if (UGameInstance* WorldGameInstance = World->GetGameInstance())
		{
			return UGameInstance::GetSubsystem<UModSubsystem>(WorldGameInstance);
		}
	}

	// Objects the framework creates are outered to the subsystem, which is outered to the game
	// instance, so the outer chain answers even before there is a world - which is exactly when a mod
	// context or an entry point is most likely to ask.
	for (const UObject* Outer = WorldContextObject->GetOuter(); Outer != nullptr; Outer = Outer->GetOuter())
	{
		if (const UModSubsystem* OuterSubsystem = Cast<UModSubsystem>(Outer))
		{
			return const_cast<UModSubsystem*>(OuterSubsystem);
		}

		if (const UGameInstance* OuterGameInstance = Cast<UGameInstance>(Outer))
		{
			return UGameInstance::GetSubsystem<UModSubsystem>(OuterGameInstance);
		}
	}

	return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// Discovery

int32 UModSubsystem::DiscoverMods()
{
	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("DiscoverMods: there is no mod registry; nothing was discovered."));
		return 0;
	}

	// A snapshot: a provider is entitled to register another one while it scans (a workshop provider
	// that discovers a local cache, say), and that would otherwise invalidate this iteration.
	const TArray<TSharedRef<IModProvider>> ProviderSnapshot = Providers;

	TArray<FModDiscoveryResult> Results;
	for (const TSharedRef<IModProvider>& Provider : ProviderSnapshot)
	{
		const int32 Before = Results.Num();
		Provider->DiscoverMods(Results);

		UE_LOG(LogModFramework, Verbose, TEXT("Provider '%s' reported %d result(s)."),
			*Provider->GetProviderId().ToString(), Results.Num() - Before);
	}

	int32 Registered = 0;
	for (const FModDiscoveryResult& Result : Results)
	{
		if (RegisterDiscoveryResult(Result))
		{
			++Registered;
		}
	}

	UE_LOG(LogModFramework, Log, TEXT("Discovery produced %d result(s); %d mod(s) were newly registered."),
		Results.Num(), Registered);

	return Registered;
}

bool UModSubsystem::RegisterDiscoveryResult(const FModDiscoveryResult& Result)
{
	using namespace ModSubsystemPrivate;

	if (!Registry)
	{
		return false;
	}

	const FModId ModId = Result.Manifest.Id;

	// A result with no id is a provider reporting about itself - an unreadable search directory, say -
	// rather than a mod to register. Keep the message, drop the record.
	if (!ModId.IsValid())
	{
		if (Result.Diagnostics.Num() > 0)
		{
			RecordDiagnostics(FModId(), Result.Diagnostics);
		}
		else
		{
			RecordDiagnostic(FModId(), FModDiagnostic::Warning(FName(CodeDiscoveryReport),
				TEXT("A provider returned a discovery result carrying no mod id and no explanation."),
				Result.RootPath));
		}

		return false;
	}

	if (Registry->IsModRegistered(ModId))
	{
		// Either two providers can see the same mod, or the caller is re-running discovery over an
		// already-populated registry. Neither is fatal: the first record wins, as it does everywhere.
		RecordDiagnostic(ModId, FModDiagnostic::Warning(FName(CodeDuplicateDiscovery),
			FString::Printf(TEXT("Mod '%s' is already registered; the copy found at '%s' was ignored."),
				*ModId.ToString(), *Result.RootPath),
			Result.RootPath));
		return false;
	}

	FModInfo Info;
	Info.Manifest = Result.Manifest;
	Info.RootPath = Result.RootPath;
	Info.ProviderId = Result.ProviderId;
	Info.Diagnostics = Result.Diagnostics;
	Info.ResolvedIconPath = ResolveIconPath(Result.RootPath, Result.Manifest.IconPath);

	// Registered enabled whatever the project says, because SetModEnabled below is what performs the
	// Discovered -> Disabled transition, and it short-circuits when the flag already matches.
	Info.bEnabled = true;
	Info.State = EModState::Unknown;

	FModDiagnostic RegisterError;
	if (!Registry->RegisterMod(Info, RegisterError))
	{
		RecordDiagnostic(ModId, RegisterError);
		return false;
	}

	// RegisterMod files a record straight into Discovered without a transition, so the discovery event
	// is the one lifecycle event this class raises by hand.
	if (EventBus)
	{
		EventBus->BroadcastLifecycle(ModFrameworkEvents::ModDiscovered, ModId, EModState::Discovered);
	}

	// --- Validation ----------------------------------------------------------------------------
	if (IsDisabledByProject(ModId))
	{
		UE_LOG(LogModFramework, Log, TEXT("Mod '%s' is switched off by the project settings."), *ModId.ToString());
		Registry->SetModEnabled(ModId, false);
		return true;
	}

	if (Result.Manifest.ManifestVersion > MODFRAMEWORK_MANIFEST_VERSION)
	{
		FailMod(ModId, EModLoadFailureReason::UnsupportedManifestVersion,
			FString::Printf(TEXT("'%s' declares manifest version %d; this framework understands up to %d."),
				*ModId.ToString(), Result.Manifest.ManifestVersion, MODFRAMEWORK_MANIFEST_VERSION));

		RecordDiagnostic(ModId, FModDiagnostic::Error(FName(CodeManifestVersion),
			TEXT("The manifest was written for a newer version of the mod framework."), Result.ManifestPath));
		return true;
	}

	if (!Result.bManifestValid || !Result.Manifest.IsValid())
	{
		FString Reason = SummariseDiagnostics(Result.Diagnostics);
		if (Reason.IsEmpty())
		{
			Reason = TEXT("the manifest did not pass validation");
		}

		FailMod(ModId, EModLoadFailureReason::ManifestInvalid,
			FString::Printf(TEXT("'%s' could not be validated: %s"), *ModId.ToString(), *Reason));

		RecordDiagnostic(ModId, FModDiagnostic::Error(FName(CodeManifestInvalid), Reason, Result.ManifestPath));
		return true;
	}

	Registry->SetModState(ModId, EModState::Validated);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// Permissions and dependencies

void UModSubsystem::EvaluatePermissions()
{
	using namespace ModSubsystemPrivate;

	if (!Registry || !PermissionRegistry)
	{
		return;
	}

	// A snapshot: EvaluateManifest broadcasts, and a handler is allowed to touch the registry.
	const TArray<FModInfo> AllMods = Registry->GetAllMods();
	for (const FModInfo& Info : AllMods)
	{
		const FModId ModId = Info.GetId();
		if (!ModId.IsValid() || Info.State == EModState::Failed)
		{
			continue;
		}

		PermissionRegistry->EvaluateManifest(Info.Manifest);
		Registry->SetGrantedPermissions(ModId, PermissionRegistry->GetGrantedPermissions(ModId));

		// Neither a pending nor a denied permission stops a mod loading. The capability itself is
		// gated at the point of use, so a mod simply runs with less than it asked for - which is what
		// keeps a game playable while a consent prompt is still on screen.
		const TArray<FName> Pending = PermissionRegistry->GetPendingPermissions(ModId);
		if (Pending.Num() > 0)
		{
			RecordDiagnostic(ModId, FModDiagnostic::Info(FName(CodePermissionsPending),
				FString::Printf(TEXT("'%s' is waiting on a decision for: %s"), *ModId.ToString(), *JoinNames(Pending)),
				Info.RootPath));
		}

		const TArray<FName> Denied = PermissionRegistry->GetDeniedPermissions(ModId);
		if (Denied.Num() > 0)
		{
			RecordDiagnostic(ModId, FModDiagnostic::Warning(FName(CodePermissionsDenied),
				FString::Printf(TEXT("'%s' was refused: %s. It will run without those capabilities."),
					*ModId.ToString(), *JoinNames(Denied)),
				Info.RootPath));
		}
	}
}

FModResolveResult UModSubsystem::ResolveDependencies()
{
	FModResolveResult Result;

	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("ResolveDependencies: there is no mod registry."));
		LastResolveResult = Result;
		return Result;
	}

	FModResolveRequest Request;
	Request.Environment = GetEnvironment();
	Request.bCheckEnvironment = true;

	const TArray<FModInfo> AllMods = Registry->GetAllMods();
	Request.Candidates.Reserve(AllMods.Num());

	for (const FModInfo& Info : AllMods)
	{
		if (!Info.GetId().IsValid())
		{
			continue;
		}

		// A mod that already failed - a broken manifest, an unsupported schema - is not a candidate.
		// Feeding it in would make everything that depends on it fail for the wrong reason.
		if (Info.State == EModState::Failed)
		{
			continue;
		}

		Request.Candidates.Add(Info.Manifest);

		if (!Info.bEnabled || Info.State == EModState::Disabled)
		{
			Request.DisabledMods.AddUnique(Info.GetId());
		}
	}

	Result = FModDependencyResolver::Resolve(Request);
	LastResolveResult = Result;

	UE_LOG(LogModFramework, Log, TEXT("Dependency resolution: %d accepted, %d rejected (%s)."),
		Result.LoadOrder.Num(), Result.Rejections.Num(), Result.bSuccess ? TEXT("clean") : TEXT("with problems"));

	ApplyResolveResult(Result);
	return Result;
}

void UModSubsystem::ApplyResolveResult(const FModResolveResult& Result)
{
	if (!Registry)
	{
		return;
	}

	Registry->SetLoadOrder(Result.LoadOrder);

	if (UModExtensionRegistry* ExtensionRegistry = Registry->GetExtensionRegistry())
	{
		// Extension ordering has to agree with load order, or two mods extending one point would
		// resolve differently from the order they were loaded in.
		ExtensionRegistry->SetModLoadOrder(Result.LoadOrder);
	}

	for (const FModDiagnostic& Diagnostic : Result.Diagnostics)
	{
		RecordDiagnostic(Diagnostic.ModId, Diagnostic);
	}

	// A DuplicateModId rejection names the id of the copy that lost, which is the same id as the copy
	// that was kept - so a mod present in the load order is loading, whatever else was said about it.
	TSet<FModId> Accepted;
	Accepted.Reserve(Result.LoadOrder.Num());
	for (const FModId& ModId : Result.LoadOrder)
	{
		Accepted.Add(ModId);
	}

	for (const FModRejection& Rejection : Result.Rejections)
	{
		if (!Rejection.ModId.IsValid() || Accepted.Contains(Rejection.ModId))
		{
			continue;
		}

		const FModInfo* Info = Registry->FindMod(Rejection.ModId);
		if (!Info)
		{
			continue;
		}

		// A mod the player switched off is not a failure; it is a choice. Leave it Disabled.
		if (Rejection.Reason == EModLoadFailureReason::Disabled || Info->State == EModState::Disabled)
		{
			continue;
		}

		FailMod(Rejection.ModId, Rejection.Reason, Rejection.Message);
	}

	// Only what survived moves on. SetModState refuses anything that is not sitting in Validated, so a
	// mod failed above quietly stays failed.
	for (const FModId& ModId : Result.LoadOrder)
	{
		Registry->SetModState(ModId, EModState::DependenciesResolved);
	}
}

void UModSubsystem::DetectConflicts(TArray<FModId>& InOutLoadOrder)
{
	using namespace ModSubsystemPrivate;

	Conflicts.Reset();

	if (!Registry)
	{
		return;
	}

	FModConflictPolicyTable Policies;
	if (const UModFrameworkSettings* Settings = UModFrameworkSettings::Get())
	{
		Policies.DefaultPolicy = Settings->DefaultConflictPolicy;
	}

	UModExtensionRegistry* ExtensionRegistry = Registry->GetExtensionRegistry();
	if (ExtensionRegistry)
	{
		// An extension point knows better than the project default what should happen when two mods
		// fight over one of its resources, so its own policy is the per-point override.
		for (const FModExtensionPointDescriptor& Point : ExtensionRegistry->GetExtensionPoints())
		{
			if (!Point.ExtensionPointId.IsNone())
			{
				Policies.PointPolicies.Add(Point.ExtensionPointId, Point.DefaultConflictPolicy);
			}
		}
	}

	TArray<FModResourceClaim> Claims;
	for (const FModId& ModId : InOutLoadOrder)
	{
		const FModInfo* Info = Registry->FindMod(ModId);
		if (!Info)
		{
			continue;
		}

		for (const FModResourceClaimDeclaration& Declaration : Info->Manifest.Claims)
		{
			if (Declaration.ExtensionPointId.IsNone() || Declaration.ResourceId.IsNone())
			{
				continue;
			}

			FModResourceClaim Claim;
			Claim.ModId = ModId;
			Claim.ExtensionPointId = Declaration.ExtensionPointId;
			Claim.ResourceId = Declaration.ResourceId;
			Claim.Priority = Info->Manifest.Priority;
			Claim.LoadOrder = Info->LoadOrder;
			Claim.PreferredPolicy = Declaration.PreferredPolicy;
			Claims.Add(MoveTemp(Claim));
		}
	}

	if (ExtensionRegistry)
	{
		// Empty during a cold refresh - extensions register at activation - but not during a reload,
		// where some mods are still live while others are being brought back.
		Claims.Append(ExtensionRegistry->CollectResourceClaims());
	}

	if (Claims.Num() == 0)
	{
		return;
	}

	Conflicts = FModConflictDetector::Detect(Claims, Policies);
	if (Conflicts.Num() == 0)
	{
		return;
	}

	UE_LOG(LogModFramework, Log, TEXT("%s"), *FModConflictDetector::BuildReport(Conflicts));

	const TArray<FModId> Blocked = FModConflictDetector::GetBlockedMods(Conflicts);
	for (const FModId& ModId : Blocked)
	{
		if (InOutLoadOrder.Remove(ModId) == 0)
		{
			continue;
		}

		FString Explanation;
		for (const FModConflict& Conflict : Conflicts)
		{
			if (Conflict.bBlocking && Conflict.Contenders.Contains(ModId))
			{
				Explanation = Conflict.Explanation;
				break;
			}
		}

		if (Explanation.IsEmpty())
		{
			Explanation = TEXT("the mod is part of a conflict the game has no policy to arbitrate");
		}

		FailMod(ModId, EModLoadFailureReason::ConflictRejected, Explanation);
		RecordDiagnostic(ModId, FModDiagnostic::Error(FName(CodeConflictBlocked), Explanation, FString()));
	}
}

//////////////////////////////////////////////////////////////////////////
// Per-mod lifecycle

bool UModSubsystem::MountMod(FModId ModId)
{
	using namespace ModSubsystemPrivate;

	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("MountMod('%s'): there is no mod registry."), *DescribeModId(ModId));
		return false;
	}

	const FModInfo* Found = Registry->FindMod(ModId);
	if (!Found)
	{
		UE_LOG(LogModFramework, Warning, TEXT("MountMod('%s'): that mod is not registered."), *DescribeModId(ModId));
		return false;
	}

	// A copy, because everything below can invalidate the record: the provider may rewrite the root
	// path, and mounting broadcasts.
	FModInfo Info = *Found;
	Found = nullptr;

	if (Info.State == EModState::Mounted || Info.IsLoaded())
	{
		UE_LOG(LogModFramework, Verbose, TEXT("MountMod('%s'): already mounted."), *ModId.ToString());
		return true;
	}

	if (!Info.bEnabled)
	{
		UE_LOG(LogModFramework, Warning, TEXT("MountMod('%s'): the mod is switched off."), *ModId.ToString());
		return false;
	}

	if (Info.State != EModState::DependenciesResolved && Info.State != EModState::Unmounted)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("MountMod('%s'): mounting needs the mod in DependenciesResolved or Unmounted, but it is %s."),
			*ModId.ToString(), *ModFrameworkEnums::ToString(Info.State));
		return false;
	}

	// Ask the provider to make the mod local. For a folder on disk that is a no-op; for a `.mod`
	// package that has only been *seen* so far, this is where it gets installed.
	if (IModProvider* Provider = FindProvider(Info.ProviderId))
	{
		FString LocalRootPath;
		FModDiagnostic AcquireError;
		if (!Provider->AcquireMod(ModId, LocalRootPath, AcquireError))
		{
			FString Message = AcquireError.Message;
			if (Message.IsEmpty())
			{
				Message = FString::Printf(TEXT("provider '%s' could not make the mod available locally"),
					*Info.ProviderId.ToString());
			}

			if (AcquireError.Code.IsNone())
			{
				AcquireError = FModDiagnostic::Error(FName(CodeProviderError), Message, Info.RootPath);
			}

			RecordDiagnostic(ModId, AcquireError);
			FailMod(ModId, EModLoadFailureReason::ProviderError, Message);
			return false;
		}

		if (!LocalRootPath.IsEmpty() && LocalRootPath != Info.RootPath)
		{
			// The package was extracted somewhere; the record has to follow it or every path derived
			// from the root - content roots, the icon - would point at the container.
			Info.RootPath = LocalRootPath;
			Info.ResolvedIconPath = ResolveIconPath(LocalRootPath, Info.Manifest.IconPath);

			if (FModInfo* Mutable = Registry->FindModMutable(ModId))
			{
				Mutable->RootPath = Info.RootPath;
				Mutable->ResolvedIconPath = Info.ResolvedIconPath;
			}
		}
	}
	else if (!Info.ProviderId.IsNone())
	{
		UE_LOG(LogModFramework, Verbose,
			TEXT("MountMod('%s'): provider '%s' is no longer registered; mounting from the recorded root path."),
			*ModId.ToString(), *Info.ProviderId.ToString());
	}

	TArray<FModContentMount> Mounts;
	TArray<FModDiagnostic> MountDiagnostics;
	const bool bMounted = ContentManager.MountMod(Info, Mounts, MountDiagnostics);
	RecordDiagnostics(ModId, MountDiagnostics);

	if (!bMounted)
	{
		FString Message = SummariseDiagnostics(MountDiagnostics);
		if (Message.IsEmpty())
		{
			Message = FString::Printf(TEXT("the content of '%s' could not be mounted"), *ModId.ToString());
		}

		RecordDiagnostic(ModId, FModDiagnostic::Error(FName(CodeMountFailed), Message, Info.RootPath));
		FailMod(ModId, EModLoadFailureReason::MountFailed, Message);
		return false;
	}

	TArray<FString> MountedPaths;
	MountedPaths.Reserve(Mounts.Num());
	for (const FModContentMount& Mount : Mounts)
	{
		MountedPaths.Add(Mount.VirtualMountPoint);
	}

	Registry->SetMountedPaths(ModId, MountedPaths);
	return Registry->SetModState(ModId, EModState::Mounted);
}

bool UModSubsystem::UnmountMod(FModId ModId)
{
	using namespace ModSubsystemPrivate;

	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("UnmountMod('%s'): there is no mod registry."), *DescribeModId(ModId));
		return false;
	}

	const FModInfo* Found = Registry->FindMod(ModId);
	if (!Found)
	{
		UE_LOG(LogModFramework, Warning, TEXT("UnmountMod('%s'): that mod is not registered."), *DescribeModId(ModId));
		return false;
	}

	const EModState State = Found->State;
	Found = nullptr;

	if (State == EModState::Activated)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UnmountMod('%s'): the mod is running. Call UnloadMod, which deactivates it first."),
			*ModId.ToString());
		return false;
	}

	if (State == EModState::Loading)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UnmountMod('%s'): the mod is mid-load and its content cannot be pulled out from under it."),
			*ModId.ToString());
		return false;
	}

	bool bResult = true;
	if (ContentManager.IsModMounted(ModId))
	{
		TArray<FModDiagnostic> UnmountDiagnostics;
		bResult = ContentManager.UnmountMod(ModId, UnmountDiagnostics);
		RecordDiagnostics(ModId, UnmountDiagnostics);
	}

	Registry->SetMountedPaths(ModId, TArray<FString>());

	if (UModRegistry::IsTransitionAllowed(State, EModState::Unmounted))
	{
		Registry->SetModState(ModId, EModState::Unmounted);
	}
	else
	{
		// Discovered, Validated, Failed and the rest never had content to release. Saying so at
		// Verbose keeps "unmount everything" loops from filling the log with non-events.
		UE_LOG(LogModFramework, Verbose, TEXT("UnmountMod('%s'): nothing was mounted (state %s)."),
			*ModId.ToString(), *ModFrameworkEnums::ToString(State));
	}

	return bResult;
}

bool UModSubsystem::LoadMod(FModId ModId)
{
	using namespace ModSubsystemPrivate;

	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("LoadMod('%s'): there is no mod registry."), *DescribeModId(ModId));
		return false;
	}

	const FModInfo* Found = Registry->FindMod(ModId);
	if (!Found)
	{
		UE_LOG(LogModFramework, Warning, TEXT("LoadMod('%s'): that mod is not registered."), *DescribeModId(ModId));
		return false;
	}

	FModInfo Info = *Found;
	Found = nullptr;

	if (Info.IsLoaded())
	{
		UE_LOG(LogModFramework, Verbose, TEXT("LoadMod('%s'): already loaded."), *ModId.ToString());
		return true;
	}

	if (!Info.bEnabled)
	{
		UE_LOG(LogModFramework, Warning, TEXT("LoadMod('%s'): the mod is switched off."), *ModId.ToString());
		return false;
	}

	// Loaded and Activated are separate states and so are the steps that reach them: loading demands
	// mounted content and nothing else.
	if (Info.State != EModState::Mounted)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("LoadMod('%s'): loading needs the mod Mounted, but it is %s. Mount it first."),
			*ModId.ToString(), *ModFrameworkEnums::ToString(Info.State));
		return false;
	}

	if (!Registry->SetModState(ModId, EModState::Loading))
	{
		return false;
	}

	// Config is loaded before the entry point runs: OnModLoaded is the natural place for a mod to
	// read its settings, so they have to be there already. A config failure is never fatal - a mod
	// with a broken config file still loads and falls back to its shipped defaults.
	if (ConfigManager)
	{
		const FModInfo* ConfigModInfo = Registry->FindMod(ModId);
		TArray<FModDiagnostic> ConfigDiagnostics;
		ConfigManager->LoadConfig(ModId, ConfigModInfo ? ConfigModInfo->RootPath : FString(), ConfigDiagnostics);
		for (const FModDiagnostic& Diagnostic : ConfigDiagnostics)
		{
			Registry->AddDiagnostic(ModId, Diagnostic);
		}
	}

	// The context exists before the entry point does, because the entry point is handed one.
	UModContext* ModContext = NewObject<UModContext>(this);
	if (!ModContext)
	{
		FailMod(ModId, EModLoadFailureReason::Internal,
			FString::Printf(TEXT("the mod context of '%s' could not be created"), *ModId.ToString()));
		return false;
	}

	ModContext->InitializeContext(this, ModId);
	Registry->TrackModObject(ModId, ModContext);
	ModContexts.Add(ModId, ModContext);

	UModEntryPointBase* EntryPoint = nullptr;
	const FSoftClassPath& EntryClassPath = Info.Manifest.EntryPoint.EntryClass;

	if (EntryClassPath.IsValid())
	{
		// Loaded as a plain UClass rather than through TryLoadClass<UModEntryPointBase>, because that
		// overload answers null for both "missing" and "wrong base class" and those are different
		// failures with different fixes.
		UClass* EntryClass = EntryClassPath.TryLoadClass<UObject>();
		if (!EntryClass)
		{
			const FString Message = FString::Printf(
				TEXT("the entry point class '%s' declared by '%s' could not be loaded"),
				*EntryClassPath.ToString(), *ModId.ToString());

			RecordDiagnostic(ModId, FModDiagnostic::Error(FName(CodeEntryPointMissing), Message, Info.RootPath));
			ReleaseModLoadState(ModId);
			FailMod(ModId, EModLoadFailureReason::EntryPointMissing, Message);
			return false;
		}

		if (!EntryClass->IsChildOf(UModEntryPointBase::StaticClass()) || !IsInstantiableClass(EntryClass))
		{
			const FString Message = FString::Printf(
				TEXT("the entry point class '%s' declared by '%s' is not a concrete UModEntryPointBase"),
				*EntryClassPath.ToString(), *ModId.ToString());

			RecordDiagnostic(ModId, FModDiagnostic::Error(FName(CodeEntryPointInvalid), Message, Info.RootPath));
			ReleaseModLoadState(ModId);
			FailMod(ModId, EModLoadFailureReason::EntryPointInvalid, Message);
			return false;
		}

		EntryPoint = NewObject<UModEntryPointBase>(this, EntryClass);
		if (!EntryPoint)
		{
			const FString Message = FString::Printf(
				TEXT("the entry point '%s' of '%s' could not be instantiated"),
				*EntryClassPath.ToString(), *ModId.ToString());

			RecordDiagnostic(ModId, FModDiagnostic::Error(FName(CodeEntryPointInvalid), Message, Info.RootPath));
			ReleaseModLoadState(ModId);
			FailMod(ModId, EModLoadFailureReason::EntryPointInvalid, Message);
			return false;
		}

		EntryPoint->Context = ModContext;
		Registry->TrackModObject(ModId, EntryPoint);
		EntryPoints.Add(ModId, EntryPoint);
	}
	else
	{
		// Entirely legal, and the common case for a content-only mod: it ships assets and a bundle and
		// never a line of code.
		UE_LOG(LogModFramework, Verbose,
			TEXT("LoadMod('%s'): the manifest declares no entry point class; loading as a content-only mod."),
			*ModId.ToString());
	}

	if (!Registry->SetModState(ModId, EModState::Loaded))
	{
		ReleaseModLoadState(ModId);
		FailMod(ModId, EModLoadFailureReason::Internal,
			FString::Printf(TEXT("'%s' could not be moved to Loaded"), *ModId.ToString()));
		return false;
	}

	// Called last, so a mod that inspects its own state from OnModLoaded sees Loaded rather than
	// Loading. Mod code runs after the framework's bookkeeping, never during it.
	if (EntryPoint)
	{
		EntryPoint->NativeOnModLoaded();
	}

	UE_LOG(LogModFramework, Log, TEXT("Loaded mod '%s' (%s)."),
		*ModId.ToString(), *Info.Manifest.Version.ToString());
	return true;
}

bool UModSubsystem::ActivateMod(FModId ModId)
{
	using namespace ModSubsystemPrivate;

	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("ActivateMod('%s'): there is no mod registry."), *DescribeModId(ModId));
		return false;
	}

	const FModInfo* Found = Registry->FindMod(ModId);
	if (!Found)
	{
		UE_LOG(LogModFramework, Warning, TEXT("ActivateMod('%s'): that mod is not registered."), *DescribeModId(ModId));
		return false;
	}

	const EModState State = Found->State;
	const bool bEnabled = Found->bEnabled;
	Found = nullptr;

	if (State == EModState::Activated)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("ActivateMod('%s'): already active."), *ModId.ToString());
		return true;
	}

	if (!bEnabled)
	{
		UE_LOG(LogModFramework, Warning, TEXT("ActivateMod('%s'): the mod is switched off."), *ModId.ToString());
		return false;
	}

	if (State != EModState::Loaded && State != EModState::Deactivated)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("ActivateMod('%s'): activation needs the mod Loaded or Deactivated, but it is %s. Load it first."),
			*ModId.ToString(), *ModFrameworkEnums::ToString(State));
		return false;
	}

	// Bundles are applied once per load. Re-activating a deactivated mod must not register its
	// extensions a second time; it only switches the ones already registered back on.
	ApplyContentBundles(ModId, GetModContext(ModId));

	UModExtensionRegistry* ExtensionRegistry = Registry->GetExtensionRegistry();
	if (ExtensionRegistry)
	{
		ExtensionRegistry->SetModExtensionsActive(ModId, true);
	}

	if (!Registry->SetModState(ModId, EModState::Activated))
	{
		if (ExtensionRegistry)
		{
			ExtensionRegistry->SetModExtensionsActive(ModId, false);
		}

		FailMod(ModId, EModLoadFailureReason::ActivationFailed,
			FString::Printf(TEXT("'%s' could not be moved to Activated"), *ModId.ToString()));
		return false;
	}

	if (UModEntryPointBase* EntryPoint = FindEntryPoint(ModId))
	{
		EntryPoint->NativeOnModActivated();
	}

	UE_LOG(LogModFramework, Log, TEXT("Activated mod '%s'."), *ModId.ToString());
	return true;
}

bool UModSubsystem::DeactivateMod(FModId ModId)
{
	using namespace ModSubsystemPrivate;

	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("DeactivateMod('%s'): there is no mod registry."), *DescribeModId(ModId));
		return false;
	}

	const FModInfo* Found = Registry->FindMod(ModId);
	if (!Found)
	{
		UE_LOG(LogModFramework, Warning, TEXT("DeactivateMod('%s'): that mod is not registered."), *DescribeModId(ModId));
		return false;
	}

	const EModState State = Found->State;
	Found = nullptr;

	if (State != EModState::Activated)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("DeactivateMod('%s'): the mod is %s, so there is nothing to switch off."),
			*ModId.ToString(), *ModFrameworkEnums::ToString(State));
		return State == EModState::Deactivated || State == EModState::Loaded;
	}

	// Mod code first: it is entitled to see its own extensions still live while it shuts down.
	if (UModEntryPointBase* EntryPoint = FindEntryPoint(ModId))
	{
		EntryPoint->NativeOnModDeactivated();
	}

	if (UModExtensionRegistry* ExtensionRegistry = Registry->GetExtensionRegistry())
	{
		ExtensionRegistry->SetModExtensionsActive(ModId, false);
	}

	const bool bResult = Registry->SetModState(ModId, EModState::Deactivated);
	if (bResult)
	{
		UE_LOG(LogModFramework, Log, TEXT("Deactivated mod '%s'."), *ModId.ToString());
	}

	return bResult;
}

bool UModSubsystem::UnloadMod(FModId ModId)
{
	using namespace ModSubsystemPrivate;

	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("UnloadMod('%s'): there is no mod registry."), *DescribeModId(ModId));
		return false;
	}

	const FModInfo* Found = Registry->FindMod(ModId);
	if (!Found)
	{
		UE_LOG(LogModFramework, Warning, TEXT("UnloadMod('%s'): that mod is not registered."), *DescribeModId(ModId));
		return false;
	}

	if (Found->State == EModState::Activated)
	{
		Found = nullptr;
		DeactivateMod(ModId);

		Found = Registry->FindMod(ModId);
		if (!Found)
		{
			return false;
		}
	}

	const EModState State = Found->State;
	Found = nullptr;

	// Loading is included: a load that failed mid-flight still created a context and possibly an
	// entry point, and both have to go.
	const bool bHasCode = State == EModState::Loaded
		|| State == EModState::Deactivated
		|| State == EModState::Loading
		|| ModContexts.Contains(ModId)
		|| EntryPoints.Contains(ModId);

	if (bHasCode)
	{
		if (UModEntryPointBase* EntryPoint = FindEntryPoint(ModId))
		{
			EntryPoint->NativeOnModUnloaded();
		}

		// Extensions before APIs: an extension may be holding an API pointer, and taking the extension
		// away first gives it the chance to drop that reference while the API is still alive. This is
		// the same order UModRegistry::UnregisterMod uses, for the same reason.
		if (UModExtensionRegistry* ExtensionRegistry = Registry->GetExtensionRegistry())
		{
			ExtensionRegistry->UnregisterAllForMod(ModId);
		}

		if (UModAPIRegistry* APIRegistry = Registry->GetAPIRegistry())
		{
			APIRegistry->UnregisterAllForMod(ModId);
		}

		if (EventBus)
		{
			EventBus->UnsubscribeAllForMod(ModId);
		}

		// Release before unmount, always: a mod object that survives its own content is a pointer into
		// a package that is about to stop existing.
		ReleaseModLoadState(ModId);

		if (EventBus)
		{
			EventBus->BroadcastLifecycle(ModFrameworkEvents::ModUnloaded, ModId, State);
		}

		UE_LOG(LogModFramework, Log, TEXT("Unloaded mod '%s'."), *ModId.ToString());
	}
	else
	{
		// Nothing was loaded, but content may still be mounted - a mod that was mounted and never
		// loaded is exactly that case.
		ReleaseModLoadState(ModId);
	}

	const bool bUnmounted = UnmountMod(ModId);
	return bHasCode || bUnmounted;
}

bool UModSubsystem::ReloadMod(FModId ModId)
{
	using namespace ModSubsystemPrivate;

	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("ReloadMod('%s'): there is no mod registry."), *DescribeModId(ModId));
		return false;
	}

	const FModInfo* Found = Registry->FindMod(ModId);
	if (!Found)
	{
		UE_LOG(LogModFramework, Warning, TEXT("ReloadMod('%s'): that mod is not registered."), *DescribeModId(ModId));
		return false;
	}

	const bool bWasActive = Found->IsActive();
	const bool bWasEnabled = Found->bEnabled;
	Found = nullptr;

	if (!bWasEnabled)
	{
		UE_LOG(LogModFramework, Warning, TEXT("ReloadMod('%s'): the mod is switched off."), *ModId.ToString());
		return false;
	}

	UnloadMod(ModId);

	if (!MountMod(ModId))
	{
		return false;
	}

	if (!LoadMod(ModId))
	{
		return false;
	}

	if (bWasActive && !ActivateMod(ModId))
	{
		return false;
	}

	UE_LOG(LogModFramework, Log, TEXT("Reloaded mod '%s'."), *ModId.ToString());
	return true;
}

//////////////////////////////////////////////////////////////////////////
// Batch lifecycle

void UModSubsystem::RefreshMods(bool bActivate)
{
	if (!Registry)
	{
		UE_LOG(LogModFramework, Error, TEXT("RefreshMods: there is no mod registry; the framework did not start up."));
		return;
	}

	if (bRefreshInProgress)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("RefreshMods: a refresh is already running. The nested call was ignored."));
		return;
	}

	bRefreshInProgress = true;

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	const bool bAutoLoad = Settings ? Settings->bAutoLoadDiscoveredMods : true;

	// --- 1. Start from nothing -----------------------------------------------------------------
	// Reset keeps whatever the *game* registered - its APIs, its extension points - and drops only
	// what mods contributed, so a refresh never makes the host game re-announce its own surface.
	UnloadAllMods();
	ModContexts.Empty();
	EntryPoints.Empty();
	AppliedBundles.Empty();
	Registry->Reset();

	Conflicts.Reset();
	Diagnostics.Reset();
	LastResolveResult = FModResolveResult();

	if (IconCache)
	{
		IconCache->ReleaseAll();
	}

	RefreshLocalProviderDirectories();

	// --- 2. Discover, validate, register -------------------------------------------------------
	const int32 DiscoveredCount = DiscoverMods();

	// --- 3. Permissions ------------------------------------------------------------------------
	EvaluatePermissions();

	// --- 4. Dependencies -----------------------------------------------------------------------
	const FModResolveResult Result = ResolveDependencies();
	TArray<FModId> LoadOrder = Result.LoadOrder;

	// --- 5. Conflicts --------------------------------------------------------------------------
	DetectConflicts(LoadOrder);

	// --- 6. Mount ------------------------------------------------------------------------------
	// One failure per stage costs that mod and nothing else. A player with a broken mod keeps the
	// rest of their list.
	TArray<FModId> MountedMods;
	MountedMods.Reserve(LoadOrder.Num());
	for (const FModId& ModId : LoadOrder)
	{
		if (MountMod(ModId))
		{
			MountedMods.Add(ModId);
		}
	}

	// --- 7. Load -------------------------------------------------------------------------------
	TArray<FModId> LoadedMods;
	if (bAutoLoad)
	{
		LoadedMods.Reserve(MountedMods.Num());
		for (const FModId& ModId : MountedMods)
		{
			if (LoadMod(ModId))
			{
				LoadedMods.Add(ModId);
			}
		}
	}
	else
	{
		UE_LOG(LogModFramework, Log,
			TEXT("RefreshMods: bAutoLoadDiscoveredMods is off, so %d mounted mod(s) were left unloaded."),
			MountedMods.Num());
	}

	// --- 8. Activate ---------------------------------------------------------------------------
	int32 ActivatedCount = 0;
	if (bActivate)
	{
		for (const FModId& ModId : LoadedMods)
		{
			if (ActivateMod(ModId))
			{
				++ActivatedCount;
			}
		}
	}

	bRefreshInProgress = false;

	UE_LOG(LogModFramework, Log,
		TEXT("Mod refresh complete: %d discovered, %d in load order, %d mounted, %d loaded, %d activated."),
		DiscoveredCount, LoadOrder.Num(), MountedMods.Num(), LoadedMods.Num(), ActivatedCount);

	if (EventBus)
	{
		EventBus->BroadcastLifecycle(ModFrameworkEvents::ModsRefreshed, FModId(), EModState::Unknown);
	}

	OnModsRefreshed.Broadcast();
}

int32 UModSubsystem::LoadAllMods()
{
	if (!Registry)
	{
		return 0;
	}

	int32 Count = 0;
	for (const FModId& ModId : GetLoadOrderedIds(/*bReverse*/ false))
	{
		const FModInfo* Info = Registry->FindMod(ModId);
		if (!Info || Info->State != EModState::Mounted)
		{
			continue;
		}

		if (LoadMod(ModId))
		{
			++Count;
		}
	}

	return Count;
}

int32 UModSubsystem::ActivateAllMods()
{
	if (!Registry)
	{
		return 0;
	}

	int32 Count = 0;
	for (const FModId& ModId : GetLoadOrderedIds(/*bReverse*/ false))
	{
		const FModInfo* Info = Registry->FindMod(ModId);
		if (!Info || (Info->State != EModState::Loaded && Info->State != EModState::Deactivated))
		{
			continue;
		}

		if (ActivateMod(ModId))
		{
			++Count;
		}
	}

	return Count;
}

int32 UModSubsystem::DeactivateAllMods()
{
	if (!Registry)
	{
		return 0;
	}

	int32 Count = 0;
	for (const FModId& ModId : GetLoadOrderedIds(/*bReverse*/ true))
	{
		const FModInfo* Info = Registry->FindMod(ModId);
		if (!Info || Info->State != EModState::Activated)
		{
			continue;
		}

		if (DeactivateMod(ModId))
		{
			++Count;
		}
	}

	return Count;
}

int32 UModSubsystem::UnloadAllMods()
{
	if (!Registry)
	{
		return 0;
	}

	int32 Count = 0;
	for (const FModId& ModId : GetLoadOrderedIds(/*bReverse*/ true))
	{
		const FModInfo* Info = Registry->FindMod(ModId);
		if (!Info)
		{
			continue;
		}

		const bool bNeedsTeardown = Info->IsLoaded()
			|| Info->State == EModState::Mounted
			|| Info->State == EModState::Loading;

		if (!bNeedsTeardown)
		{
			continue;
		}

		if (UnloadMod(ModId))
		{
			++Count;
		}
	}

	return Count;
}

void UModSubsystem::ReloadAllMods()
{
	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("ReloadAllMods: there is no mod registry."));
		return;
	}

	// Captured before anything is torn down, because unloading clears the state it is read from.
	const TArray<FModId> Order = GetLoadOrderedIds(/*bReverse*/ false);

	TSet<FModId> PreviouslyActive;
	TSet<FModId> PreviouslyLoaded;
	for (const FModId& ModId : Order)
	{
		const FModInfo* Info = Registry->FindMod(ModId);
		if (!Info)
		{
			continue;
		}

		if (Info->IsActive())
		{
			PreviouslyActive.Add(ModId);
		}

		if (Info->IsLoaded())
		{
			PreviouslyLoaded.Add(ModId);
		}
	}

	TArray<FModId> ReverseOrder = Order;
	Algo::Reverse(ReverseOrder);
	for (const FModId& ModId : ReverseOrder)
	{
		UnloadMod(ModId);
	}

	for (const FModId& ModId : Order)
	{
		if (!MountMod(ModId))
		{
			continue;
		}

		if (!PreviouslyLoaded.Contains(ModId))
		{
			continue;
		}

		if (!LoadMod(ModId))
		{
			continue;
		}

		if (PreviouslyActive.Contains(ModId))
		{
			ActivateMod(ModId);
		}
	}

	UE_LOG(LogModFramework, Log, TEXT("Reloaded %d mod(s)."), Order.Num());
}

bool UModSubsystem::SetModEnabled(FModId ModId, bool bEnabled)
{
	using namespace ModSubsystemPrivate;

	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("SetModEnabled('%s'): there is no mod registry."), *DescribeModId(ModId));
		return false;
	}

	const FModInfo* Found = Registry->FindMod(ModId);
	if (!Found)
	{
		UE_LOG(LogModFramework, Warning, TEXT("SetModEnabled('%s'): that mod is not registered."), *DescribeModId(ModId));
		return false;
	}

	if (Found->bEnabled == bEnabled)
	{
		return true;
	}

	const bool bWasLive = Found->IsLoaded() || Found->State == EModState::Mounted;
	Found = nullptr;

	if (!bEnabled)
	{
		if (bWasLive)
		{
			UnloadMod(ModId);
		}

		// Unmounted has no edge to Disabled, but it has one back to Discovered, which does. Taking it
		// keeps the record honest instead of leaving a switched-off mod parked in Unmounted.
		if (const FModInfo* Current = Registry->FindMod(ModId))
		{
			if (Current->State == EModState::Unmounted)
			{
				Registry->SetModState(ModId, EModState::Discovered);
			}
		}
	}

	const bool bResult = Registry->SetModEnabled(ModId, bEnabled);

	UE_LOG(LogModFramework, Log,
		TEXT("Mod '%s' was switched %s for this session. The change is not written to the project settings.%s"),
		*ModId.ToString(),
		bEnabled ? TEXT("on") : TEXT("off"),
		bEnabled ? TEXT(" Run RefreshMods to bring it up.") : TEXT(""));

	return bResult;
}

//////////////////////////////////////////////////////////////////////////
// Registration (game side)

bool UModSubsystem::RegisterGameAPI(UModAPI* Api)
{
	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("RegisterGameAPI: there is no mod registry."));
		return false;
	}

	UModAPIRegistry* APIRegistry = Registry->GetAPIRegistry();
	if (!APIRegistry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("RegisterGameAPI: there is no API registry."));
		return false;
	}

	FModDiagnostic Error;
	if (!APIRegistry->RegisterAPI(Api, Error))
	{
		RecordDiagnostic(FModId(), Error);
		return false;
	}

	return true;
}

bool UModSubsystem::RegisterGameAPIByClass(TSubclassOf<UModAPI> ApiClass)
{
	using namespace ModSubsystemPrivate;

	UClass* ResolvedClass = ApiClass.Get();
	if (!IsInstantiableClass(ResolvedClass))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("RegisterGameAPIByClass: '%s' is null, abstract or otherwise not instantiable."),
			ResolvedClass ? *ResolvedClass->GetName() : TEXT("<null>"));
		return false;
	}

	// Outered to the subsystem so the API lives exactly as long as the framework does; the registry
	// keeps its own reference on top of that.
	UModAPI* Api = NewObject<UModAPI>(this, ResolvedClass);
	if (!Api)
	{
		UE_LOG(LogModFramework, Warning, TEXT("RegisterGameAPIByClass: '%s' could not be instantiated."),
			*ResolvedClass->GetName());
		return false;
	}

	return RegisterGameAPI(Api);
}

bool UModSubsystem::RegisterExtensionPoint(const FModExtensionPointDescriptor& Descriptor)
{
	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("RegisterExtensionPoint: there is no mod registry."));
		return false;
	}

	UModExtensionRegistry* ExtensionRegistry = Registry->GetExtensionRegistry();
	if (!ExtensionRegistry)
	{
		UE_LOG(LogModFramework, Warning, TEXT("RegisterExtensionPoint: there is no extension registry."));
		return false;
	}

	FModDiagnostic Error;
	if (!ExtensionRegistry->RegisterExtensionPoint(Descriptor, Error))
	{
		RecordDiagnostic(FModId(), Error);
		return false;
	}

	return true;
}

bool UModSubsystem::RegisterEventType(const FModEventDescriptor& Descriptor)
{
	if (!EventBus)
	{
		UE_LOG(LogModFramework, Warning, TEXT("RegisterEventType: there is no event bus."));
		return false;
	}

	FModDiagnostic Error;
	if (!EventBus->RegisterEventType(Descriptor, Error))
	{
		RecordDiagnostic(FModId(), Error);
		return false;
	}

	return true;
}

void UModSubsystem::RegisterProvider(TSharedRef<IModProvider> Provider)
{
	const FName ProviderId = Provider->GetProviderId();
	if (ProviderId.IsNone())
	{
		UE_LOG(LogModFramework, Warning, TEXT("RegisterProvider: refusing a provider that reports no id."));
		return;
	}

	for (int32 Index = 0; Index < Providers.Num(); ++Index)
	{
		if (Providers[Index]->GetProviderId() != ProviderId)
		{
			continue;
		}

		UE_LOG(LogModFramework, Log, TEXT("Provider '%s' replaced an earlier registration under the same id."),
			*ProviderId.ToString());
		Providers[Index] = Provider;
		return;
	}

	Providers.Add(Provider);
	UE_LOG(LogModFramework, Verbose, TEXT("Registered mod provider '%s'."), *ProviderId.ToString());
}

void UModSubsystem::UnregisterProvider(FName ProviderId)
{
	const int32 Removed = Providers.RemoveAll(
		[ProviderId](const TSharedRef<IModProvider>& Provider)
		{
			return Provider->GetProviderId() == ProviderId;
		});

	if (Removed > 0)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Unregistered mod provider '%s'."), *ProviderId.ToString());
	}

	if (LocalProvider.IsValid() && LocalProvider->GetProviderId() == ProviderId)
	{
		LocalProvider.Reset();
	}
}

TArray<FName> UModSubsystem::GetProviderIds() const
{
	TArray<FName> Ids;
	Ids.Reserve(Providers.Num());

	for (const TSharedRef<IModProvider>& Provider : Providers)
	{
		Ids.Add(Provider->GetProviderId());
	}

	return Ids;
}

IModProvider* UModSubsystem::FindProvider(FName ProviderId) const
{
	if (ProviderId.IsNone())
	{
		return nullptr;
	}

	for (const TSharedRef<IModProvider>& Provider : Providers)
	{
		if (Provider->GetProviderId() == ProviderId)
		{
			return &Provider.Get();
		}
	}

	return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// Accessors

UModRegistry* UModSubsystem::GetRegistry() const
{
	return Registry;
}

UModAPIRegistry* UModSubsystem::GetAPIRegistry() const
{
	return Registry ? Registry->GetAPIRegistry() : nullptr;
}

UModExtensionRegistry* UModSubsystem::GetExtensionRegistry() const
{
	return Registry ? Registry->GetExtensionRegistry() : nullptr;
}

UModEventBus* UModSubsystem::GetEventBus() const
{
	return EventBus;
}

UModPermissionRegistry* UModSubsystem::GetPermissionRegistry() const
{
	return PermissionRegistry;
}

UModSaveDataManager* UModSubsystem::GetSaveDataManager() const
{
	return SaveDataManager;
}

UModConfigManager* UModSubsystem::GetConfigManager() const
{
	return ConfigManager;
}

UModIconCache* UModSubsystem::GetIconCache() const
{
	return IconCache;
}

UModContext* UModSubsystem::GetModContext(FModId ModId) const
{
	const TObjectPtr<UModContext>* Found = ModContexts.Find(ModId);
	return Found ? Found->Get() : nullptr;
}

FModContentManager& UModSubsystem::GetContentManager()
{
	return ContentManager;
}

const FModContentManager& UModSubsystem::GetContentManager() const
{
	return ContentManager;
}

FModEnvironment UModSubsystem::GetEnvironment() const
{
	return FModEnvironment::FromSettings();
}

FModResolveResult UModSubsystem::GetLastResolveResult() const
{
	return LastResolveResult;
}

TArray<FModConflict> UModSubsystem::GetConflicts() const
{
	return Conflicts;
}

FModSessionManifest UModSubsystem::BuildSessionManifest() const
{
	FModSessionManifest Manifest;
	Manifest.FormatVersion = ModSessionManifest::CurrentFormatVersion;

	const FModEnvironment Environment = GetEnvironment();
	Manifest.GameId = Environment.GameId;
	Manifest.GameVersion = Environment.GameVersion;
	Manifest.FrameworkVersion = Environment.FrameworkVersion;
	Manifest.SdkId = Environment.SdkId;
	Manifest.SdkVersion = Environment.SdkVersion;

	if (!Registry)
	{
		return Manifest;
	}

	// Only what is actually running is advertised. A mod that is merely mounted changes nothing about
	// how this install behaves, so it has no business making a player fail a compatibility check.
	const TArray<FModInfo> ActiveMods = Registry->GetActiveMods();
	Manifest.Entries.Reserve(FMath::Min(ActiveMods.Num(), ModSessionManifest::MaxEntries));

	for (const FModInfo& Info : ActiveMods)
	{
		if (Manifest.Entries.Num() >= ModSessionManifest::MaxEntries)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("BuildSessionManifest: more than %d active mods; the manifest was truncated."),
				ModSessionManifest::MaxEntries);
			break;
		}

		if (!Info.GetId().IsValid())
		{
			continue;
		}

		FModNetworkEntry Entry;
		Entry.ModId = Info.GetId();
		Entry.Version = Info.Manifest.Version;
		Entry.Scope = Info.Manifest.NetworkScope;
		Entry.bRequired = Info.Manifest.bRequiredForNetworkPlay;
		Entry.DisplayName = Info.Manifest.GetDisplayNameOrId();

		// ContentHash is deliberately left empty: an unpacked mod has no single fingerprint, and an
		// empty hash never causes a mismatch on its own. A game that ships only packaged mods can fill
		// it in from FModPackageHeader::ContentHash.
		Manifest.Entries.Add(MoveTemp(Entry));
	}

	return Manifest;
}

TArray<FModDiagnostic> UModSubsystem::GetDiagnostics() const
{
	TArray<FModDiagnostic> All = Diagnostics;

	if (Registry)
	{
		for (const FModInfo& Info : Registry->GetAllMods())
		{
			All.Append(Info.Diagnostics);
		}
	}

	return All;
}

//////////////////////////////////////////////////////////////////////////
// Content bundles

void UModSubsystem::ApplyContentBundles(const FModId& ModId, UModContext* ModContext)
{
	using namespace ModSubsystemPrivate;

	if (!Registry || AppliedBundles.Contains(ModId))
	{
		return;
	}

	FModActiveBundleSet Applied;

	const FModInfo* Info = Registry->FindMod(ModId);
	if (!Info)
	{
		AppliedBundles.Add(ModId, MoveTemp(Applied));
		return;
	}

	// Copied out: registering an extension broadcasts, and a handler may mutate the registry, which
	// would invalidate Info halfway through the loop below.
	const TArray<FSoftObjectPath> BundlePaths = Info->Manifest.EntryPoint.ContentBundles;
	const FString ModRootPath = Info->RootPath;
	Info = nullptr;

	UModExtensionRegistry* ExtensionRegistry = Registry->GetExtensionRegistry();

	for (const FSoftObjectPath& BundlePath : BundlePaths)
	{
		if (!BundlePath.IsValid())
		{
			continue;
		}

		UObject* LoadedObject = BundlePath.TryLoad();
		if (!LoadedObject)
		{
			// Cosmetic-grade failure: a bundle that will not resolve costs the mod that bundle, not
			// the mod itself. It may still have registered everything it needs from its entry point.
			RecordDiagnostic(ModId, FModDiagnostic::Warning(FName(CodeBundleUnresolved),
				FString::Printf(TEXT("Content bundle '%s' of '%s' could not be loaded."),
					*BundlePath.ToString(), *ModId.ToString()),
				ModRootPath));
			continue;
		}

		UModContentBundle* Bundle = Cast<UModContentBundle>(LoadedObject);
		if (!Bundle)
		{
			RecordDiagnostic(ModId, FModDiagnostic::Warning(FName(CodeBundleWrongType),
				FString::Printf(TEXT("'%s' of '%s' is a %s, not a UModContentBundle."),
					*BundlePath.ToString(), *ModId.ToString(), *LoadedObject->GetClass()->GetName()),
				ModRootPath));
			continue;
		}

		Applied.Objects.Add(Bundle);

		// Purely declarative, but it is the difference between telling a player "this mod needs a
		// feature this build does not have" and burying them in registration failures.
		for (const FName RequiredPoint : Bundle->RequiredExtensionPoints)
		{
			if (RequiredPoint.IsNone())
			{
				continue;
			}

			FModExtensionPointDescriptor Descriptor;
			if (!ExtensionRegistry || !ExtensionRegistry->GetExtensionPoint(RequiredPoint, Descriptor))
			{
				RecordDiagnostic(ModId, FModDiagnostic::Warning(FName(CodeBundlePointMissing),
					FString::Printf(TEXT("Bundle '%s' of '%s' needs the extension point '%s', which this game does not open."),
						*Bundle->GetName(), *ModId.ToString(), *RequiredPoint.ToString()),
					ModRootPath));
			}
		}

		for (const TSoftClassPtr<UModExtension>& ExtensionClassPtr : Bundle->ExtensionClasses)
		{
			UClass* ExtensionClass = ExtensionClassPtr.LoadSynchronous();
			if (!ExtensionClass || !ExtensionClass->IsChildOf(UModExtension::StaticClass()) || !IsInstantiableClass(ExtensionClass))
			{
				RecordDiagnostic(ModId, FModDiagnostic::Warning(FName(CodeBundleExtension),
					FString::Printf(TEXT("Bundle '%s' of '%s' names '%s', which is not a usable UModExtension class."),
						*Bundle->GetName(), *ModId.ToString(), *ExtensionClassPtr.ToString()),
					ModRootPath));
				continue;
			}

			// Outered to the mod's own context so a Blueprint extension gets world context the same
			// way one registered by hand from the entry point does.
			UObject* Outer = ModContext ? static_cast<UObject*>(ModContext) : static_cast<UObject*>(this);
			UModExtension* Extension = NewObject<UModExtension>(Outer, ExtensionClass);
			if (!Extension)
			{
				RecordDiagnostic(ModId, FModDiagnostic::Warning(FName(CodeBundleExtension),
					FString::Printf(TEXT("'%s' of '%s' could not be instantiated."),
						*ExtensionClass->GetName(), *ModId.ToString()),
					ModRootPath));
				continue;
			}

			// Tracked before registration so a rejected extension is untracked again rather than
			// stranded: a failed registration must leak nothing.
			Registry->TrackModObject(ModId, Extension);

			FModDiagnostic Error;
			const bool bRegistered = ExtensionRegistry && ExtensionRegistry->RegisterExtension(Extension, ModId, Error);
			if (!bRegistered)
			{
				Registry->UntrackModObject(ModId, Extension);

				if (Error.Code.IsNone())
				{
					Error = FModDiagnostic::Warning(FName(CodeBundleExtension),
						FString::Printf(TEXT("'%s' of '%s' could not be registered: there is no extension registry."),
							*ExtensionClass->GetName(), *ModId.ToString()),
						ModRootPath);
				}

				RecordDiagnostic(ModId, Error);
				continue;
			}

			Applied.Objects.Add(Extension);
		}

		// A bundle references its payload softly, so nothing here stays resident unless the applied
		// set holds it. Loading them is the whole point of naming them in a bundle.
		for (const TSoftObjectPtr<UPrimaryDataAsset>& DataAssetPtr : Bundle->DataAssets)
		{
			if (UPrimaryDataAsset* DataAsset = DataAssetPtr.LoadSynchronous())
			{
				Applied.Objects.Add(DataAsset);
			}
			else if (!DataAssetPtr.IsNull())
			{
				RecordDiagnostic(ModId, FModDiagnostic::Warning(FName(CodeBundleUnresolved),
					FString::Printf(TEXT("Bundle '%s' of '%s' names the data asset '%s', which could not be loaded."),
						*Bundle->GetName(), *ModId.ToString(), *DataAssetPtr.ToString()),
					ModRootPath));
			}
		}

		for (const TSoftObjectPtr<UDataTable>& DataTablePtr : Bundle->DataTables)
		{
			if (UDataTable* DataTable = DataTablePtr.LoadSynchronous())
			{
				Applied.Objects.Add(DataTable);
			}
			else if (!DataTablePtr.IsNull())
			{
				RecordDiagnostic(ModId, FModDiagnostic::Warning(FName(CodeBundleUnresolved),
					FString::Printf(TEXT("Bundle '%s' of '%s' names the data table '%s', which could not be loaded."),
						*Bundle->GetName(), *ModId.ToString(), *DataTablePtr.ToString()),
					ModRootPath));
			}
		}
	}

	// Added even when it is empty: its presence is what stops a deactivate/activate cycle from
	// registering every one of these extensions a second time.
	AppliedBundles.Add(ModId, MoveTemp(Applied));
}

//////////////////////////////////////////////////////////////////////////
// Helpers

void UModSubsystem::FailMod(const FModId& ModId, EModLoadFailureReason Reason, const FString& Message)
{
	if (!Registry)
	{
		UE_LOG(LogModFramework, Error, TEXT("Mod '%s' failed (%s): %s"),
			*ModSubsystemPrivate::DescribeModId(ModId), *ModFrameworkEnums::ToString(Reason), *Message);
		return;
	}

	// SetModFailed records the reason, appends an Error diagnostic and moves the mod to Failed, which
	// raises ModFrameworkEvents::ModFailed through HandleModStateChanged.
	Registry->SetModFailed(ModId, Reason, Message);
}

void UModSubsystem::RecordDiagnostic(const FModId& ModId, const FModDiagnostic& Diagnostic)
{
	FModDiagnostic Stamped = Diagnostic;
	if (!Stamped.ModId.IsValid() && ModId.IsValid())
	{
		Stamped.ModId = ModId;
	}

	if (Diagnostics.Num() < MaxRetainedDiagnostics)
	{
		Diagnostics.Add(Stamped);
	}

	// AddDiagnostic mirrors the entry to the log at the verbosity matching its severity, so a
	// registered mod's problems are logged exactly once.
	if (Registry && ModId.IsValid() && Registry->IsModRegistered(ModId))
	{
		Registry->AddDiagnostic(ModId, Stamped);
		return;
	}

	switch (Stamped.Severity)
	{
	case EModDiagnosticSeverity::Error:
		UE_LOG(LogModFramework, Error, TEXT("%s"), *Stamped.ToString());
		break;
	case EModDiagnosticSeverity::Warning:
		UE_LOG(LogModFramework, Warning, TEXT("%s"), *Stamped.ToString());
		break;
	default:
		UE_LOG(LogModFramework, Log, TEXT("%s"), *Stamped.ToString());
		break;
	}
}

void UModSubsystem::RecordDiagnostics(const FModId& ModId, const TArray<FModDiagnostic>& InDiagnostics)
{
	for (const FModDiagnostic& Diagnostic : InDiagnostics)
	{
		RecordDiagnostic(ModId, Diagnostic);
	}
}

TArray<FModId> UModSubsystem::GetLoadOrderedIds(bool bReverse) const
{
	TArray<FModId> Ids;
	if (!Registry)
	{
		return Ids;
	}

	// GetAllMods is already sorted by load order ascending, with unordered mods last and ties broken
	// by id, so the traversal is total and reproducible without sorting anything here.
	const TArray<FModInfo> AllMods = Registry->GetAllMods();
	Ids.Reserve(AllMods.Num());

	for (const FModInfo& Info : AllMods)
	{
		if (Info.GetId().IsValid())
		{
			Ids.Add(Info.GetId());
		}
	}

	if (bReverse)
	{
		Algo::Reverse(Ids);
	}

	return Ids;
}

UModEntryPointBase* UModSubsystem::FindEntryPoint(const FModId& ModId) const
{
	const TObjectPtr<UModEntryPointBase>* Found = EntryPoints.Find(ModId);
	return Found ? Found->Get() : nullptr;
}

void UModSubsystem::ReleaseModLoadState(const FModId& ModId)
{
	// The entry point's back-pointer goes first: the object is about to be marked garbage and a mod
	// that captured it must not be able to reach a half-released context through it.
	if (UModEntryPointBase* EntryPoint = FindEntryPoint(ModId))
	{
		EntryPoint->Context = nullptr;
	}

	EntryPoints.Remove(ModId);
	ModContexts.Remove(ModId);
	AppliedBundles.Remove(ModId);

	// Persist before dropping the store: a mod that changed settings during its run should not lose
	// them because it was unloaded rather than the game being closed. Saving an untouched store is a
	// harmless rewrite of identical content.
	if (ConfigManager)
	{
		ConfigManager->SaveConfig(ModId);
		ConfigManager->UnloadConfig(ModId);
	}

	if (Registry)
	{
		Registry->ReleaseModObjects(ModId);
	}
}

void UModSubsystem::RefreshLocalProviderDirectories()
{
	if (!LocalProvider.IsValid())
	{
		return;
	}

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	if (!Settings)
	{
		return;
	}

	// Re-read every refresh: the command line switches and the {Project} tokens are resolved fresh, so
	// a game that changes its mod directory at runtime does not need a new provider.
	LocalProvider->SetSearchDirectories(Settings->GetResolvedSearchDirectories());
}

bool UModSubsystem::IsDisabledByProject(const FModId& ModId)
{
	if (!ModId.IsValid())
	{
		return false;
	}

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	if (!Settings)
	{
		return false;
	}

	for (const FString& Disabled : Settings->DisabledModIds)
	{
		// Parsed rather than compared raw: DisabledModIds is hand-edited ini text, and an id written
		// with the wrong case must still switch the mod off.
		if (FModId::FromString(Disabled) == ModId)
		{
			return true;
		}
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////
// Delegate handlers

bool UModSubsystem::HandlePermissionCheck(const FModId& ModId, FName PermissionId)
{
	// Fails closed. Without a permission registry nothing can be verified, and an unverifiable
	// capability is one a mod does not get.
	return PermissionRegistry != nullptr && PermissionRegistry->HasPermission(ModId, PermissionId);
}

void UModSubsystem::HandleModStateChanged(const FModId& ModId, EModState OldState, EModState NewState)
{
	OnModStateChanged.Broadcast(ModId, OldState, NewState);

	const FName EventId = ModSubsystemPrivate::LifecycleEventForState(NewState);
	if (EventId.IsNone() || !EventBus)
	{
		return;
	}

	EModLoadFailureReason Reason = EModLoadFailureReason::None;
	FString Message;
	if (Registry)
	{
		if (const FModInfo* Info = Registry->FindMod(ModId))
		{
			Reason = Info->FailureReason;
			Message = Info->FailureMessage;
		}
	}

	// Raised centrally rather than from each call site, so a transition can never happen without the
	// matching announcement - including transitions a future code path adds.
	EventBus->BroadcastLifecycle(EventId, ModId, NewState, Reason, Message);
}

void UModSubsystem::HandleWorldCreated(UWorld* World)
{
	// Every world in the process reports here, including editor previews and other game instances'
	// worlds. Only this game instance's worlds are any of this subsystem's business.
	if (!World || World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	BroadcastFrameworkEvent(ModFrameworkEvents::WorldCreated, World);
}

void UModSubsystem::HandleWorldDestroyed(UWorld* World)
{
	if (!World || World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	BroadcastFrameworkEvent(ModFrameworkEvents::WorldDestroyed, World);
}

void UModSubsystem::HandleGameInstanceStarted(UGameInstance* InGameInstance)
{
	if (!InGameInstance || InGameInstance != GetGameInstance() || bGameStartedBroadcast)
	{
		return;
	}

	bGameStartedBroadcast = true;
	BroadcastFrameworkEvent(ModFrameworkEvents::GameStarted, InGameInstance);
}

void UModSubsystem::BroadcastFrameworkEvent(FName EventId, UObject* WorldContext)
{
	if (!EventBus || EventId.IsNone())
	{
		return;
	}

	FModEventContext Context;
	Context.EventId = EventId;
	Context.WorldContext = WorldContext;

	// SourceModId is left invalid on purpose: the framework raises these, so they are never subject to
	// any mod's permissions.
	EventBus->Broadcast(Context);
}
