// Copyright (c) 2026. Licensed for use in your own projects.

#include "Registry/ModRegistry.h"

#include "API/ModAPIRegistry.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "Extensions/ModExtensionRegistry.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "Math/NumericLimits.h"
#include "Registry/ModInfo.h"
#include "Settings/ModFrameworkSettings.h"
#include "Templates/Function.h"
#include "Templates/Tuple.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectPtr.h"
#include "UObject/UObjectBaseUtility.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace ModRegistryPrivate
{
	/** Stable machine-readable diagnostic codes produced by the registry. */
	constexpr const TCHAR* CodeInvalidModId = TEXT("Registry.InvalidModId");
	constexpr const TCHAR* CodeDuplicateModId = TEXT("Registry.DuplicateModId");
	constexpr const TCHAR* CodeLimitExceeded = TEXT("Registry.LimitExceeded");
	constexpr const TCHAR* CodeModFailed = TEXT("Registry.ModFailed");
	constexpr const TCHAR* CodeDiagnosticsTruncated = TEXT("Registry.DiagnosticsTruncated");

	/**
	 * Upper bound on how many diagnostics one mod may accumulate.
	 *
	 * Diagnostics are produced while chewing through untrusted data - a manifest with ten thousand
	 * malformed dependencies produces ten thousand messages - and FModInfo is copied out by value on
	 * every accessor, so an unbounded list is both a memory and a copy-cost problem. Overflow is
	 * announced with a single CodeDiagnosticsTruncated entry so the truncation is never silent, and
	 * dropped diagnostics still reach the log.
	 */
	constexpr int32 MaxDiagnosticsPerMod = 256;

	/** Floor for the amortised stale-entry sweep of the reverse object-owner index. */
	constexpr int32 MinObjectOwnersSweepThreshold = 64;

	/** MaxMods <= 0 disables the limit. Settings are absent only in exotic early-startup cases. */
	int32 GetConfiguredMaxMods()
	{
		if (const UModFrameworkSettings* Settings = UModFrameworkSettings::Get())
		{
			return Settings->MaxMods;
		}

		return 0;
	}

	/** Sort key for FModInfo::LoadOrder: anything unassigned (INDEX_NONE, or negative) sorts last. */
	int32 SortableLoadOrder(int32 In)
	{
		return In < 0 ? MAX_int32 : In;
	}

	/**
	 * The one ordering every plural accessor uses: load order ascending, unordered mods last, ties
	 * broken by id. Mod ids are unique, so this is a total order and the result is identical from
	 * run to run even though TMap iteration order is not.
	 */
	bool LessByLoadOrderThenId(const FModInfo& A, const FModInfo& B)
	{
		const int32 OrderA = SortableLoadOrder(A.LoadOrder);
		const int32 OrderB = SortableLoadOrder(B.LoadOrder);
		if (OrderA != OrderB)
		{
			return OrderA < OrderB;
		}

		return A.GetId() < B.GetId();
	}

	TArray<FModInfo> GatherSorted(const TMap<FModId, FModInfo>& InMods, TFunctionRef<bool(const FModInfo&)> Predicate)
	{
		TArray<FModInfo> Result;
		Result.Reserve(InMods.Num());

		for (const TPair<FModId, FModInfo>& Pair : InMods)
		{
			if (Predicate(Pair.Value))
			{
				Result.Add(Pair.Value);
			}
		}

		Result.Sort([](const FModInfo& A, const FModInfo& B) { return LessByLoadOrderThenId(A, B); });
		return Result;
	}

	/** Mirrors a diagnostic into LogModFramework at the verbosity matching its severity. */
	void LogDiagnostic(const FModDiagnostic& In)
	{
		switch (In.Severity)
		{
		case EModDiagnosticSeverity::Error:
			UE_LOG(LogModFramework, Error, TEXT("%s"), *In.ToString());
			break;

		case EModDiagnosticSeverity::Warning:
			UE_LOG(LogModFramework, Warning, TEXT("%s"), *In.ToString());
			break;

		default:
			UE_LOG(LogModFramework, Log, TEXT("%s"), *In.ToString());
			break;
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Lifetime

void UModRegistry::Initialize()
{
	if (!APIRegistry)
	{
		APIRegistry = NewObject<UModAPIRegistry>(this);
	}

	if (!ExtensionRegistry)
	{
		ExtensionRegistry = NewObject<UModExtensionRegistry>(this);
	}
}

void UModRegistry::Shutdown()
{
	Reset();

	// Reset() only removes what mods contributed. Shutdown is the end of the session, so anything
	// the game itself registered goes too.
	if (ExtensionRegistry)
	{
		ExtensionRegistry->Reset();
		ExtensionRegistry = nullptr;
	}

	if (APIRegistry)
	{
		APIRegistry->Reset();
		APIRegistry = nullptr;
	}

	OnModStateChanged.Clear();
}

//////////////////////////////////////////////////////////////////////////
// Registration

bool UModRegistry::RegisterMod(const FModInfo& InInfo, FModDiagnostic& OutError)
{
	using namespace ModRegistryPrivate;

	const FModId Id = InInfo.Manifest.Id;

	if (!Id.IsValid())
	{
		OutError = FModDiagnostic::Error(
			FName(CodeInvalidModId),
			TEXT("A mod cannot be registered without a valid manifest id."),
			InInfo.RootPath);
		LogDiagnostic(OutError);
		return false;
	}

	if (const FModInfo* Existing = Mods.Find(Id))
	{
		OutError = FModDiagnostic::Error(
			FName(CodeDuplicateModId),
			FString::Printf(
				TEXT("Mod id '%s' is already registered from '%s'."),
				*Id.ToString(),
				Existing->RootPath.IsEmpty() ? TEXT("<unknown location>") : *Existing->RootPath),
			InInfo.RootPath);
		OutError.ModId = Id;
		LogDiagnostic(OutError);
		return false;
	}

	const int32 MaxMods = GetConfiguredMaxMods();
	if (MaxMods > 0 && Mods.Num() >= MaxMods)
	{
		OutError = FModDiagnostic::Error(
			FName(CodeLimitExceeded),
			FString::Printf(
				TEXT("Refusing to register '%s': the configured limit of %d mods has been reached."),
				*Id.ToString(),
				MaxMods),
			InInfo.RootPath);
		OutError.ModId = Id;
		LogDiagnostic(OutError);
		return false;
	}

	FModInfo& Stored = Mods.Add(Id, InInfo);

	// Unknown is the state of a record nobody has looked at yet; registering it *is* the act of
	// discovering it, and Discovered is the only edge out of Unknown.
	if (Stored.State == EModState::Unknown)
	{
		Stored.State = EModState::Discovered;
	}

	// Normalise any other spelling of "unordered" so the sort comparator and SetLoadOrder agree.
	if (Stored.LoadOrder < 0)
	{
		Stored.LoadOrder = INDEX_NONE;
	}

	UE_LOG(LogModFramework, Verbose, TEXT("Registered mod '%s' version %s from '%s' (provider '%s')."),
		*Id.ToString(),
		*Stored.Manifest.Version.ToString(),
		Stored.RootPath.IsEmpty() ? TEXT("<unknown location>") : *Stored.RootPath,
		*Stored.ProviderId.ToString());

	return true;
}

bool UModRegistry::UnregisterMod(const FModId& InId)
{
	// Copied up front: callers routinely pass a reference that lives inside the record about to be
	// destroyed, e.g. UnregisterMod(FindMod(X)->GetId()).
	const FModId Id = InId;

	if (!Mods.Contains(Id))
	{
		UE_LOG(LogModFramework, Warning, TEXT("Cannot unregister mod '%s': it is not registered."), *Id.ToString());
		return false;
	}

	// Extensions before APIs: an extension may have been handed an API pointer, and unregistering
	// it first gives it a chance to drop that reference while the API is still alive.
	if (ExtensionRegistry)
	{
		ExtensionRegistry->UnregisterAllForMod(Id);
	}

	if (APIRegistry)
	{
		APIRegistry->UnregisterAllForMod(Id);
	}

	ReleaseModObjects(Id);
	Mods.Remove(Id);

	UE_LOG(LogModFramework, Verbose, TEXT("Unregistered mod '%s'."), *Id.ToString());
	return true;
}

void UModRegistry::Reset()
{
	using namespace ModRegistryPrivate;

	TArray<FModId> Ids;
	Mods.GenerateKeyArray(Ids);

	// A mod can have tracked objects without still having a record, for instance after a failed
	// registration retry. Release those too so nothing is stranded.
	TArray<FModId> TrackedIds;
	ModObjects.GenerateKeyArray(TrackedIds);
	for (const FModId& TrackedId : TrackedIds)
	{
		Ids.AddUnique(TrackedId);
	}

	for (const FModId& Id : Ids)
	{
		if (ExtensionRegistry)
		{
			ExtensionRegistry->UnregisterAllForMod(Id);
		}

		if (APIRegistry)
		{
			APIRegistry->UnregisterAllForMod(Id);
		}

		ReleaseModObjects(Id);
	}

	Mods.Empty();
	ModObjects.Empty();
	ObjectOwners.Empty();
	ObjectOwnersSweepThreshold = MinObjectOwnersSweepThreshold;

	UE_LOG(LogModFramework, Verbose, TEXT("Mod registry reset; %d mod record(s) dropped."), Ids.Num());
}

//////////////////////////////////////////////////////////////////////////
// Lookup

const FModInfo* UModRegistry::FindMod(const FModId& InId) const
{
	return Mods.Find(InId);
}

FModInfo* UModRegistry::FindModMutable(const FModId& InId)
{
	return Mods.Find(InId);
}

bool UModRegistry::GetModInfo(const FModId& ModId, FModInfo& OutInfo) const
{
	if (const FModInfo* Found = Mods.Find(ModId))
	{
		OutInfo = *Found;
		return true;
	}

	return false;
}

bool UModRegistry::IsModRegistered(const FModId& ModId) const
{
	return Mods.Contains(ModId);
}

TArray<FModId> UModRegistry::GetAllModIds() const
{
	TArray<FModId> Ids;
	Mods.GenerateKeyArray(Ids);
	Ids.Sort([](const FModId& A, const FModId& B) { return A < B; });
	return Ids;
}

TArray<FModInfo> UModRegistry::GetAllMods() const
{
	return ModRegistryPrivate::GatherSorted(Mods, [](const FModInfo&) { return true; });
}

TArray<FModInfo> UModRegistry::GetModsInState(EModState State) const
{
	return ModRegistryPrivate::GatherSorted(Mods, [State](const FModInfo& Info) { return Info.State == State; });
}

TArray<FModInfo> UModRegistry::GetLoadedMods() const
{
	return ModRegistryPrivate::GatherSorted(Mods, [](const FModInfo& Info) { return Info.IsLoaded(); });
}

TArray<FModInfo> UModRegistry::GetActiveMods() const
{
	return ModRegistryPrivate::GatherSorted(Mods, [](const FModInfo& Info) { return Info.IsActive(); });
}

int32 UModRegistry::GetModCount() const
{
	return Mods.Num();
}

//////////////////////////////////////////////////////////////////////////
// State machine

bool UModRegistry::IsTransitionAllowed(EModState From, EModState To)
{
	// Anything can fail, at any point, including a mod that has already failed.
	if (To == EModState::Failed)
	{
		return true;
	}

	switch (From)
	{
	case EModState::Unknown:
		return To == EModState::Discovered;

	case EModState::Discovered:
		return To == EModState::Validated
			|| To == EModState::Disabled;

	case EModState::Validated:
		return To == EModState::DependenciesResolved
			|| To == EModState::Disabled;

	case EModState::DependenciesResolved:
		return To == EModState::Mounted;

	case EModState::Mounted:
		return To == EModState::Loading
			|| To == EModState::Unmounted;

	case EModState::Loading:
		return To == EModState::Loaded;

	case EModState::Loaded:
		return To == EModState::Activated
			|| To == EModState::Unmounted;

	case EModState::Activated:
		return To == EModState::Deactivated;

	case EModState::Deactivated:
		return To == EModState::Activated
			|| To == EModState::Unmounted;

	case EModState::Unmounted:
		return To == EModState::Discovered
			|| To == EModState::Mounted;

	case EModState::Failed:
		return To == EModState::Discovered;

	case EModState::Disabled:
		return To == EModState::Discovered;

	default:
		// Reached only for a value outside the enum, which deserialisation of untrusted data can
		// produce. Refuse rather than assert.
		return false;
	}
}

bool UModRegistry::SetModState(const FModId& InId, EModState NewState)
{
	FModInfo* Info = Mods.Find(InId);
	if (!Info)
	{
		UE_LOG(LogModFramework, Warning, TEXT("Cannot move mod '%s' to state %s: it is not registered."),
			*InId.ToString(), *ModFrameworkEnums::ToString(NewState));
		return false;
	}

	const EModState OldState = Info->State;
	if (!IsTransitionAllowed(OldState, NewState))
	{
		UE_LOG(LogModFramework, Warning, TEXT("Rejected illegal state transition %s -> %s for mod '%s'."),
			*ModFrameworkEnums::ToString(OldState), *ModFrameworkEnums::ToString(NewState), *InId.ToString());
		return false;
	}

	Info->State = NewState;

	// Leaving Failed means somebody is retrying the mod, so the stale reason must not linger.
	if (OldState == EModState::Failed && NewState != EModState::Failed)
	{
		Info->FailureReason = EModLoadFailureReason::None;
		Info->FailureMessage.Reset();
	}

	UE_LOG(LogModFramework, Verbose, TEXT("Mod '%s': %s -> %s."),
		*InId.ToString(), *ModFrameworkEnums::ToString(OldState), *ModFrameworkEnums::ToString(NewState));

	// The state is committed before anyone hears about it, and the id is copied first: a handler is
	// allowed to register or unregister mods, which would invalidate both Info and InId if InId
	// happened to reference something inside the table.
	const FModId ChangedId = InId;
	OnModStateChanged.Broadcast(ChangedId, OldState, NewState);

	return true;
}

bool UModRegistry::SetModFailed(const FModId& InId, EModLoadFailureReason Reason, const FString& Message)
{
	using namespace ModRegistryPrivate;

	FModInfo* Info = Mods.Find(InId);
	if (!Info)
	{
		UE_LOG(LogModFramework, Warning, TEXT("Cannot fail mod '%s': it is not registered."), *InId.ToString());
		return false;
	}

	Info->FailureReason = Reason;
	Info->FailureMessage = Message;

	FModDiagnostic Diagnostic = FModDiagnostic::Error(
		FName(CodeModFailed),
		FString::Printf(
			TEXT("%s: %s"),
			*ModFrameworkEnums::ToString(Reason),
			Message.IsEmpty() ? TEXT("no further detail was recorded") : *Message),
		InId.ToString());
	Diagnostic.ModId = InId;

	// AddDiagnostic re-finds the record, so Info must not be used after this point.
	AddDiagnostic(InId, Diagnostic);

	return SetModState(InId, EModState::Failed);
}

//////////////////////////////////////////////////////////////////////////
// Mutation

bool UModRegistry::SetLoadOrder(const TArray<FModId>& InOrder)
{
	bool bClean = true;

	for (TPair<FModId, FModInfo>& Pair : Mods)
	{
		Pair.Value.LoadOrder = INDEX_NONE;
	}

	TSet<FModId> Seen;
	Seen.Reserve(InOrder.Num());

	int32 NextOrder = 0;
	for (const FModId& Id : InOrder)
	{
		if (!Id.IsValid())
		{
			UE_LOG(LogModFramework, Warning, TEXT("Ignoring an invalid mod id in the requested load order."));
			bClean = false;
			continue;
		}

		if (Seen.Contains(Id))
		{
			UE_LOG(LogModFramework, Warning, TEXT("Mod '%s' appears more than once in the requested load order; keeping the first position."),
				*Id.ToString());
			bClean = false;
			continue;
		}

		Seen.Add(Id);

		FModInfo* Info = Mods.Find(Id);
		if (!Info)
		{
			UE_LOG(LogModFramework, Warning, TEXT("Ignoring unregistered mod '%s' in the requested load order."), *Id.ToString());
			bClean = false;
			continue;
		}

		Info->LoadOrder = NextOrder++;
	}

	return bClean;
}

bool UModRegistry::SetModEnabled(const FModId& InId, bool bEnabled)
{
	FModInfo* Info = Mods.Find(InId);
	if (!Info)
	{
		UE_LOG(LogModFramework, Warning, TEXT("Cannot change the enabled flag of mod '%s': it is not registered."), *InId.ToString());
		return false;
	}

	if (Info->bEnabled == bEnabled)
	{
		return true;
	}

	Info->bEnabled = bEnabled;
	const EModState CurrentState = Info->State;

	// Info may be invalidated by the broadcast inside SetModState, so nothing below touches it.
	if (!bEnabled)
	{
		if (IsTransitionAllowed(CurrentState, EModState::Disabled))
		{
			SetModState(InId, EModState::Disabled);
		}
		else
		{
			UE_LOG(LogModFramework, Verbose,
				TEXT("Mod '%s' was disabled while in state %s; it keeps that state until it is torn down."),
				*InId.ToString(), *ModFrameworkEnums::ToString(CurrentState));
		}
	}
	else if (CurrentState == EModState::Disabled)
	{
		SetModState(InId, EModState::Discovered);
	}

	return true;
}

void UModRegistry::AddDiagnostic(const FModId& InId, const FModDiagnostic& Diagnostic)
{
	using namespace ModRegistryPrivate;

	FModInfo* Info = Mods.Find(InId);
	if (!Info)
	{
		UE_LOG(LogModFramework, Warning, TEXT("Dropping a diagnostic for unregistered mod '%s': %s"),
			*InId.ToString(), *Diagnostic.ToString());
		return;
	}

	FModDiagnostic Copy = Diagnostic;
	if (!Copy.ModId.IsValid())
	{
		Copy.ModId = InId;
	}

	// The log always gets the message, even when the record is full.
	LogDiagnostic(Copy);

	const int32 Count = Info->Diagnostics.Num();
	if (Count > MaxDiagnosticsPerMod)
	{
		return;
	}

	if (Count == MaxDiagnosticsPerMod)
	{
		FModDiagnostic Truncated = FModDiagnostic::Warning(
			FName(CodeDiagnosticsTruncated),
			FString::Printf(
				TEXT("Reached the per-mod limit of %d stored diagnostics; further ones are logged only."),
				MaxDiagnosticsPerMod),
			InId.ToString());
		Truncated.ModId = InId;
		LogDiagnostic(Truncated);
		Info->Diagnostics.Add(MoveTemp(Truncated));
		return;
	}

	Info->Diagnostics.Add(MoveTemp(Copy));
}

void UModRegistry::SetGrantedPermissions(const FModId& InId, const TArray<FName>& Permissions)
{
	FModInfo* Info = Mods.Find(InId);
	if (!Info)
	{
		UE_LOG(LogModFramework, Warning, TEXT("Cannot set granted permissions for mod '%s': it is not registered."), *InId.ToString());
		return;
	}

	TArray<FName> Unique;
	Unique.Reserve(Permissions.Num());
	for (const FName& Permission : Permissions)
	{
		if (Permission.IsNone())
		{
			continue;
		}

		Unique.AddUnique(Permission);
	}

	Info->GrantedPermissions = MoveTemp(Unique);
}

void UModRegistry::SetMountedPaths(const FModId& InId, const TArray<FString>& Paths)
{
	FModInfo* Info = Mods.Find(InId);
	if (!Info)
	{
		UE_LOG(LogModFramework, Warning, TEXT("Cannot set mounted paths for mod '%s': it is not registered."), *InId.ToString());
		return;
	}

	TArray<FString> Unique;
	Unique.Reserve(Paths.Num());
	for (const FString& Path : Paths)
	{
		if (Path.IsEmpty())
		{
			continue;
		}

		Unique.AddUnique(Path);
	}

	Info->MountedPaths = MoveTemp(Unique);
}

//////////////////////////////////////////////////////////////////////////
// Mod-owned object tracking

void UModRegistry::TrackModObject(const FModId& InId, UObject* Object)
{
	if (!InId.IsValid())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Refusing to track object '%s' against an invalid mod id."),
			::IsValid(Object) ? *Object->GetPathName() : TEXT("<null or garbage>"));
		return;
	}

	if (!::IsValid(Object))
	{
		UE_LOG(LogModFramework, Warning, TEXT("Refusing to track a null or already-garbage object for mod '%s'."),
			*InId.ToString());
		return;
	}

	const TWeakObjectPtr<UObject> Key(Object);

	if (const FModId* ExistingOwner = ObjectOwners.Find(Key))
	{
		if (*ExistingOwner == InId)
		{
			// Already tracked by this mod; tracking is idempotent.
			return;
		}

		// Re-pointing ownership would let one mod's unload destroy another mod's object, so the
		// first claim wins and the second is only reported.
		UE_LOG(LogModFramework, Warning,
			TEXT("Object '%s' is already owned by mod '%s'; ignoring the ownership claim from mod '%s'."),
			*Object->GetPathName(), *ExistingOwner->ToString(), *InId.ToString());
		return;
	}

	ModObjects.FindOrAdd(InId).Objects.Add(Object);
	ObjectOwners.Add(Key, InId);

	if (ObjectOwners.Num() >= ObjectOwnersSweepThreshold)
	{
		PruneStaleObjectOwners();
	}
}

void UModRegistry::UntrackModObject(const FModId& InId, UObject* Object)
{
	if (!Object)
	{
		return;
	}

	if (FModObjectSet* Set = ModObjects.Find(InId))
	{
		Set->Objects.RemoveAll([Object](const TObjectPtr<UObject>& Tracked) { return Tracked.Get() == Object; });

		if (Set->Objects.Num() == 0)
		{
			ModObjects.Remove(InId);
		}
	}

	const TWeakObjectPtr<UObject> Key(Object);
	if (const FModId* Owner = ObjectOwners.Find(Key))
	{
		// Only the recorded owner may drop the reverse entry, so an untrack call naming the wrong
		// mod cannot orphan another mod's object.
		if (*Owner == InId)
		{
			ObjectOwners.Remove(Key);
		}
	}
}

TArray<UObject*> UModRegistry::GetModObjects(const FModId& InId) const
{
	TArray<UObject*> Result;

	const FModObjectSet* Set = ModObjects.Find(InId);
	if (!Set)
	{
		return Result;
	}

	Result.Reserve(Set->Objects.Num());
	for (const TObjectPtr<UObject>& Tracked : Set->Objects)
	{
		UObject* Object = Tracked.Get();
		if (::IsValid(Object))
		{
			Result.Add(Object);
		}
	}

	return Result;
}

void UModRegistry::ReleaseModObjects(const FModId& InId)
{
	// Copied because the entry that InId may reference is about to be removed.
	const FModId Id = InId;

	FModObjectSet Released;
	if (FModObjectSet* Set = ModObjects.Find(Id))
	{
		Released = MoveTemp(*Set);
	}
	ModObjects.Remove(Id);

	int32 MarkedCount = 0;
	int32 RootedCount = 0;

	for (const TObjectPtr<UObject>& Tracked : Released.Objects)
	{
		UObject* Object = Tracked.Get();
		if (!::IsValid(Object))
		{
			// Null, already garbage or already collected: nothing left to do.
			continue;
		}

		if (Object->IsRooted())
		{
			// UObjectBaseUtility::MarkAsGarbage() asserts on a rooted object, and force-unrooting
			// something that was deliberately rooted would be worse than leaking it. Forget it and
			// say so loudly.
			++RootedCount;
			continue;
		}

		Object->MarkAsGarbage();
		++MarkedCount;
	}

	RemoveObjectOwnersForMod(Id);

	if (RootedCount > 0)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Mod '%s': %d tracked object(s) are in the root set and were left alive; whoever rooted them must release them."),
			*Id.ToString(), RootedCount);
	}

	if (MarkedCount > 0)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Mod '%s': released %d tracked object(s)."), *Id.ToString(), MarkedCount);
	}
}

FModId UModRegistry::FindOwningMod(const UObject* Object) const
{
	if (!Object)
	{
		return FModId();
	}

	// An entry whose object has been collected can never match a live key again - index and serial
	// number together are unique - so stale entries are pure overhead. Sweeping them amortised
	// keeps this lookup O(1) instead of O(tracked objects).
	if (ObjectOwners.Num() >= ObjectOwnersSweepThreshold)
	{
		PruneStaleObjectOwners();
	}

	const TWeakObjectPtr<UObject> Key(const_cast<UObject*>(Object));
	if (const FModId* Owner = ObjectOwners.Find(Key))
	{
		return *Owner;
	}

	return FModId();
}

void UModRegistry::PruneStaleObjectOwners() const
{
	using namespace ModRegistryPrivate;

	for (auto It = ObjectOwners.CreateIterator(); It; ++It)
	{
		if (It.Key().Get() == nullptr)
		{
			It.RemoveCurrent();
		}
	}

	ObjectOwnersSweepThreshold = ObjectOwners.Num() * 2;
	if (ObjectOwnersSweepThreshold < MinObjectOwnersSweepThreshold)
	{
		ObjectOwnersSweepThreshold = MinObjectOwnersSweepThreshold;
	}
}

void UModRegistry::RemoveObjectOwnersForMod(const FModId& InId)
{
	for (auto It = ObjectOwners.CreateIterator(); It; ++It)
	{
		if (It.Value() == InId)
		{
			It.RemoveCurrent();
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Sub-registries

UModAPIRegistry* UModRegistry::GetAPIRegistry() const
{
	return APIRegistry;
}

UModExtensionRegistry* UModRegistry::GetExtensionRegistry() const
{
	return ExtensionRegistry;
}
