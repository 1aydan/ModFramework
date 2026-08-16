// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModDeveloperModel.h"

#include "Conflicts/ModConflictDetector.h"
#include "Containers/Set.h"
#include "Dependencies/ModDependencyResolver.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/GameInstance.h"
#include "Misc/Paths.h"
#include "Packaging/ModPackageFormat.h"
#include "Providers/LocalFileModProvider.h"
#include "Providers/ModProvider.h"
#include "Registry/ModInfo.h"
#include "Registry/ModRegistry.h"
#include "Settings/ModFrameworkSettings.h"
#include "Subsystem/ModSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "ModDeveloperModel"

void UModDeveloperEventProxy::HandleModStateChanged(FModId /*ModId*/, EModState /*OldState*/, EModState /*NewState*/)
{
	if (const TSharedPtr<FModDeveloperModel> Pinned = Owner.Pin())
	{
		Pinned->HandleSubsystemEvent();
	}
}

void UModDeveloperEventProxy::HandleModsRefreshed()
{
	if (const TSharedPtr<FModDeveloperModel> Pinned = Owner.Pin())
	{
		Pinned->HandleSubsystemEvent();
	}
}

TSharedRef<FModDeveloperModel> FModDeveloperModel::Create()
{
	TSharedRef<FModDeveloperModel> Model = MakeShareable(new FModDeveloperModel());
	Model->Initialize();
	return Model;
}

FModDeveloperModel::~FModDeveloperModel()
{
	if (PendingRefreshHandle.IsValid())
	{
		FTSTicker::RemoveTicker(PendingRefreshHandle);
		PendingRefreshHandle.Reset();
	}

	UnbindSubsystem();

	if (PostPieStartedHandle.IsValid())
	{
		FEditorDelegates::PostPIEStarted.Remove(PostPieStartedHandle);
		PostPieStartedHandle.Reset();
	}

	if (EndPieHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(EndPieHandle);
		EndPieHandle.Reset();
	}

	EventProxy.Reset();
}

void FModDeveloperModel::Initialize()
{
	EventProxy.Reset(NewObject<UModDeveloperEventProxy>());
	if (EventProxy.IsValid())
	{
		EventProxy->Owner = AsShared();
	}

	// Starting and stopping Play In Editor is what swaps the model between its live and offline
	// sources, so both ends of it force a rebuild.
	PostPieStartedHandle = FEditorDelegates::PostPIEStarted.AddSP(this, &FModDeveloperModel::HandlePieEvent);
	EndPieHandle = FEditorDelegates::EndPIE.AddSP(this, &FModDeveloperModel::HandlePieEvent);

	Refresh();
}

UModSubsystem* FModDeveloperModel::GetSubsystem() const
{
	return BoundSubsystem.Get();
}

void FModDeveloperModel::HandlePieEvent(bool /*bIsSimulating*/)
{
	// PostPIEStarted fires before the game instance has finished its first refresh pass, and EndPIE
	// fires while the subsystem is still being torn down. Neither is a good moment to read state, so
	// both go through the next-frame path.
	RequestRefresh();
}

void FModDeveloperModel::HandleSubsystemEvent()
{
	RequestRefresh();
}

void FModDeveloperModel::RequestRefresh()
{
	if (PendingRefreshHandle.IsValid())
	{
		return;
	}

	TWeakPtr<FModDeveloperModel> WeakThis = AsShared();
	PendingRefreshHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakThis](float /*DeltaTime*/) -> bool
		{
			if (const TSharedPtr<FModDeveloperModel> Pinned = WeakThis.Pin())
			{
				Pinned->PendingRefreshHandle.Reset();
				Pinned->Refresh();
			}

			// One shot: the handle is re-armed by the next RequestRefresh, so the model never ticks
			// when nothing has happened.
			return false;
		}),
		0.0f);
}

void FModDeveloperModel::Refresh()
{
	// A panel reacting to OnChanged may well ask for another refresh. Collapsing that into the pass
	// already running keeps the window from recursing through the whole rebuild.
	if (bRefreshing)
	{
		return;
	}

	TGuardValue<bool> RefreshGuard(bRefreshing, true);

	Mods.Reset();
	Conflicts.Reset();
	PipelineDiagnostics.Reset();
	SearchDirectories.Reset();
	ResolveResult = FModResolveResult();

	if (const UModFrameworkSettings* Settings = UModFrameworkSettings::Get())
	{
		SearchDirectories = Settings->GetResolvedSearchDirectories();
	}

	UModSubsystem* Live = FindLiveSubsystem();
	if (Live != BoundSubsystem.Get())
	{
		UnbindSubsystem();
		BindSubsystem(Live);
	}

	bLive = Live != nullptr;

	if (bLive)
	{
		RefreshFromSubsystem(Live);
	}
	else
	{
		RefreshFromDisk();
	}

	OnChanged.Broadcast();
}

UModSubsystem* FModDeveloperModel::FindLiveSubsystem()
{
	if (GEngine == nullptr)
	{
		return nullptr;
	}

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE && Context.WorldType != EWorldType::Game)
		{
			continue;
		}

		if (UGameInstance* GameInstance = Context.OwningGameInstance)
		{
			if (UModSubsystem* Found = GameInstance->GetSubsystem<UModSubsystem>())
			{
				return Found;
			}
		}
	}

	return nullptr;
}

void FModDeveloperModel::BindSubsystem(UModSubsystem* InSubsystem)
{
	BoundSubsystem = InSubsystem;

	if (InSubsystem == nullptr || !EventProxy.IsValid())
	{
		return;
	}

	UModDeveloperEventProxy* Proxy = EventProxy.Get();
	InSubsystem->OnModStateChanged.AddDynamic(Proxy, &UModDeveloperEventProxy::HandleModStateChanged);
	InSubsystem->OnModsRefreshed.AddDynamic(Proxy, &UModDeveloperEventProxy::HandleModsRefreshed);
}

void FModDeveloperModel::UnbindSubsystem()
{
	UModSubsystem* Previous = BoundSubsystem.Get();
	BoundSubsystem.Reset();

	if (Previous == nullptr || !EventProxy.IsValid())
	{
		return;
	}

	UModDeveloperEventProxy* Proxy = EventProxy.Get();
	Previous->OnModStateChanged.RemoveDynamic(Proxy, &UModDeveloperEventProxy::HandleModStateChanged);
	Previous->OnModsRefreshed.RemoveDynamic(Proxy, &UModDeveloperEventProxy::HandleModsRefreshed);
}

void FModDeveloperModel::RefreshFromSubsystem(UModSubsystem* InSubsystem)
{
	const UModRegistry* Registry = InSubsystem->GetRegistry();
	if (Registry != nullptr)
	{
		const TArray<FModInfo> AllMods = Registry->GetAllMods();
		Mods.Reserve(AllMods.Num());

		for (const FModInfo& Info : AllMods)
		{
			TSharedRef<FModDeveloperModRow> Row = MakeShared<FModDeveloperModRow>();
			Row->Id = Info.GetId();
			Row->DisplayName = Info.Manifest.GetDisplayNameOrId();
			Row->Author = Info.Manifest.Author;
			Row->VersionText = Info.Manifest.Version.ToString();
			Row->RootPath = Info.RootPath;
			Row->IconPath = Info.ResolvedIconPath;
			Row->Manifest = Info.Manifest;
			Row->Diagnostics = Info.Diagnostics;
			Row->State = Info.State;
			Row->FailureReason = Info.FailureReason;
			Row->FailureMessage = Info.FailureMessage;
			Row->LoadOrder = Info.LoadOrder;
			Row->bEnabled = Info.bEnabled;
			Row->bPackaged = Info.RootPath.EndsWith(ModPackage::GetFileExtension(), ESearchCase::IgnoreCase);
			Row->bManifestValid = Info.Manifest.IsValid();
			Row->bLive = true;

			Mods.Add(Row);
		}
	}
	else
	{
		PipelineDiagnostics.Add(FModDiagnostic::Error(FName(TEXT("Editor.NoRegistry")),
			TEXT("The running game instance has a mod subsystem but no registry. It probably failed to initialise; check the log for LogModFramework errors."),
			TEXT("UModSubsystem::GetRegistry")));
	}

	ResolveResult = InSubsystem->GetLastResolveResult();
	Conflicts = InSubsystem->GetConflicts();
	PipelineDiagnostics.Append(InSubsystem->GetDiagnostics());
}

void FModDeveloperModel::RefreshFromDisk()
{
	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();

	// FLocalFileModProvider is the same provider the subsystem installs at runtime, so the offline
	// list is exactly what the game would discover - not an approximation of it.
	FLocalFileModProvider Provider(SearchDirectories);

	TArray<FModDiscoveryResult> Results;
	Provider.DiscoverMods(Results);

	TSet<FModId> DisabledIds;
	if (Settings != nullptr)
	{
		for (const FString& DisabledId : Settings->DisabledModIds)
		{
			const FModId Parsed = FModId::FromString(DisabledId);
			if (Parsed.IsValid())
			{
				DisabledIds.Add(Parsed);
			}
		}
	}

	TArray<FModManifest> Candidates;
	Candidates.Reserve(Results.Num());
	Mods.Reserve(Results.Num());

	for (const FModDiscoveryResult& Result : Results)
	{
		// A result with no id is a report about the scan itself (a search directory that does not
		// exist, say) rather than a mod. IModProvider documents that case explicitly.
		if (!Result.Manifest.Id.IsValid())
		{
			PipelineDiagnostics.Append(Result.Diagnostics);
			continue;
		}

		TSharedRef<FModDeveloperModRow> Row = MakeShared<FModDeveloperModRow>();
		Row->Id = Result.Manifest.Id;
		Row->DisplayName = Result.Manifest.GetDisplayNameOrId();
		Row->Author = Result.Manifest.Author;
		Row->VersionText = Result.Manifest.Version.ToString();
		Row->RootPath = Result.RootPath;
		Row->Manifest = Result.Manifest;
		Row->Diagnostics = Result.Diagnostics;
		Row->bManifestValid = Result.bManifestValid;
		Row->bPackaged = Result.RootPath.EndsWith(ModPackage::GetFileExtension(), ESearchCase::IgnoreCase);
		Row->bEnabled = !DisabledIds.Contains(Row->Id);
		Row->bLive = false;

		// The icon path is resolved by the subsystem at registration time, which has not happened
		// offline. An unpacked mod that declares one can still be resolved here; a package cannot,
		// because its icon lives inside the container.
		if (!Row->bPackaged && !Result.Manifest.IconPath.IsEmpty())
		{
			Row->IconPath = FPaths::Combine(Result.RootPath, Result.Manifest.IconPath);
		}

		if (!Result.bManifestValid)
		{
			Row->State = EModState::Failed;
			Row->FailureReason = EModLoadFailureReason::ManifestInvalid;
			Row->FailureMessage = TEXT("The manifest did not parse or did not validate. See the Validation tab.");
		}
		else if (!Row->bEnabled)
		{
			Row->State = EModState::Disabled;
		}
		else
		{
			Row->State = EModState::Discovered;
			Candidates.Add(Result.Manifest);
		}

		Mods.Add(Row);
	}

	FModResolveRequest Request;
	Request.Candidates = MoveTemp(Candidates);
	Request.DisabledMods = DisabledIds.Array();
	Request.Environment = FModEnvironment::FromSettings();
	Request.bCheckEnvironment = true;

	ResolveResult = FModDependencyResolver::Resolve(Request);

	// Mirror the resolver's answer onto the rows so the offline list reads the same way the live one
	// does: an ordered mod shows its position, a rejected mod shows the reason it was rejected for.
	for (int32 OrderIndex = 0; OrderIndex < ResolveResult.LoadOrder.Num(); ++OrderIndex)
	{
		if (const TSharedPtr<FModDeveloperModRow> Row = FindMod(ResolveResult.LoadOrder[OrderIndex]))
		{
			Row->LoadOrder = OrderIndex;
			Row->State = EModState::Validated;
		}
	}

	for (const FModRejection& Rejection : ResolveResult.Rejections)
	{
		const TSharedPtr<FModDeveloperModRow> Row = FindMod(Rejection.ModId);
		if (!Row.IsValid() || Row->LoadOrder != INDEX_NONE)
		{
			// A DuplicateModId rejection names the id of the copy that lost, which is the same id as
			// the copy that was kept. FModResolveResult documents that; LoadOrder is the tie-break.
			continue;
		}

		Row->FailureReason = Rejection.Reason;
		Row->FailureMessage = Rejection.Message;
		Row->State = (Rejection.Reason == EModLoadFailureReason::Disabled) ? EModState::Disabled : EModState::Failed;
	}

	// Offline conflict detection sees only manifest-declared claims. Claims raised by extension
	// objects need those objects instantiated, which needs the mods mounted and loaded - so those
	// only ever appear in the live view, and the Conflicts panel says so.
	TArray<FModResourceClaim> Claims;
	for (const TSharedPtr<FModDeveloperModRow>& Row : Mods)
	{
		if (!Row.IsValid() || Row->LoadOrder == INDEX_NONE)
		{
			continue;
		}

		for (const FModResourceClaimDeclaration& Declaration : Row->Manifest.Claims)
		{
			if (Declaration.ExtensionPointId.IsNone() || Declaration.ResourceId.IsNone())
			{
				continue;
			}

			FModResourceClaim Claim;
			Claim.ModId = Row->Id;
			Claim.ExtensionPointId = Declaration.ExtensionPointId;
			Claim.ResourceId = Declaration.ResourceId;
			Claim.Priority = Row->Manifest.Priority;
			Claim.LoadOrder = Row->LoadOrder;
			Claim.PreferredPolicy = Declaration.PreferredPolicy;
			Claims.Add(MoveTemp(Claim));
		}
	}

	FModConflictPolicyTable PolicyTable;
	if (Settings != nullptr)
	{
		PolicyTable.DefaultPolicy = Settings->DefaultConflictPolicy;
	}

	Conflicts = FModConflictDetector::Detect(Claims, PolicyTable);

	PipelineDiagnostics.Append(ResolveResult.Diagnostics);

	if (SearchDirectories.Num() == 0)
	{
		PipelineDiagnostics.Add(FModDiagnostic::Warning(FName(TEXT("Editor.NoSearchDirectories")),
			TEXT("No mod search directories are configured. Set them under Project Settings > Plugins > Mod Framework."),
			TEXT("UModFrameworkSettings::ModSearchDirectories")));
	}
}

TSharedPtr<FModDeveloperModRow> FModDeveloperModel::FindMod(const FModId& InId) const
{
	if (!InId.IsValid())
	{
		return nullptr;
	}

	for (const TSharedPtr<FModDeveloperModRow>& Row : Mods)
	{
		if (Row.IsValid() && Row->Id == InId)
		{
			return Row;
		}
	}

	return nullptr;
}

FText FModDeveloperModel::GetSourceDescription() const
{
	if (bLive)
	{
		return FText::Format(
			LOCTEXT("SourceLive", "Live: {0} mod(s) in the running game instance."),
			FText::AsNumber(Mods.Num()));
	}

	if (SearchDirectories.Num() == 0)
	{
		return LOCTEXT("SourceNoDirectories",
			"No game instance is running and no mod search directories are configured. "
			"Set them under Project Settings > Plugins > Mod Framework.");
	}

	return FText::Format(
		LOCTEXT("SourceOffline",
			"Offline scan: {0} mod(s) found in {1} search directory/directories. Start Play In Editor "
			"to see live state and to load, activate or reload a mod."),
		FText::AsNumber(Mods.Num()),
		FText::AsNumber(SearchDirectories.Num()));
}

#undef LOCTEXT_NAMESPACE
