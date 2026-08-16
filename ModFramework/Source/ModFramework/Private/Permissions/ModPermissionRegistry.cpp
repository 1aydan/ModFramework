// Copyright (c) 2026. Licensed for use in your own projects.

#include "Permissions/ModPermissionRegistry.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModManifest.h"
#include "Permissions/ModPermissions.h"
#include "Settings/ModFrameworkSettings.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ScriptInterface.h"

namespace ModPermissionRegistryPrivate
{
	/** Permission lists are always handed out in a stable order so logs and UI do not shuffle. */
	static void SortPermissionIds(TArray<FName>& InOut)
	{
		InOut.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	}

	/**
	 * True for the four states the enum actually defines.
	 *
	 * A Blueprint policy cannot normally return anything else, but the value crosses a reflection
	 * boundary and this code refuses to switch on something it has not checked.
	 */
	static bool IsKnownState(const EModPermissionState In)
	{
		switch (In)
		{
		case EModPermissionState::NotRequested:
		case EModPermissionState::Pending:
		case EModPermissionState::Granted:
		case EModPermissionState::Denied:
			return true;
		default:
			return false;
		}
	}

	/** Empty stand-in used when the settings object is somehow unavailable. */
	static const TArray<FName>& GetEmptyPermissionList()
	{
		static const TArray<FName> Empty;
		return Empty;
	}
}

void UModPermissionRegistry::Initialize()
{
	const TArray<FModPermissionDescriptor> Builtins = ModPermissions::GetBuiltinPermissionDescriptors();
	for (const FModPermissionDescriptor& Descriptor : Builtins)
	{
		Descriptors.Add(Descriptor.PermissionId, Descriptor);
	}

	UE_LOG(LogModFramework, Verbose, TEXT("Permissions: registered %d builtin permission(s)."), Builtins.Num());
}

void UModPermissionRegistry::Reset()
{
	Descriptors.Reset();
	ModStates.Reset();
	ReportedDangerousAutoGrants.Reset();

	// Policy survives on purpose - see the header. Re-registering the builtins leaves the registry
	// usable rather than in a state where every permission looks unregistered.
	Initialize();
}

void UModPermissionRegistry::RegisterPermission(const FModPermissionDescriptor& InDescriptor)
{
	if (InDescriptor.PermissionId.IsNone())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Permissions: ignoring a permission descriptor with no permission id."));
		return;
	}

	if (const FModPermissionDescriptor* Existing = Descriptors.Find(InDescriptor.PermissionId))
	{
		if (Existing->bDangerous != InDescriptor.bDangerous)
		{
			// Worth saying out loud: this changes whether AutoGrantedPermissions can hand it out.
			UE_LOG(LogModFramework, Warning,
				TEXT("Permissions: re-registering '%s' changes its dangerous flag from %s to %s."),
				*InDescriptor.PermissionId.ToString(),
				Existing->bDangerous ? TEXT("true") : TEXT("false"),
				InDescriptor.bDangerous ? TEXT("true") : TEXT("false"));
		}
	}

	Descriptors.Add(InDescriptor.PermissionId, InDescriptor);
}

TArray<FModPermissionDescriptor> UModPermissionRegistry::GetAllPermissions() const
{
	TArray<FModPermissionDescriptor> Result;
	Descriptors.GenerateValueArray(Result);
	Result.Sort([](const FModPermissionDescriptor& A, const FModPermissionDescriptor& B)
		{
			return A.PermissionId.LexicalLess(B.PermissionId);
		});

	return Result;
}

bool UModPermissionRegistry::GetPermission(const FName PermissionId, FModPermissionDescriptor& Out) const
{
	if (const FModPermissionDescriptor* Found = Descriptors.Find(PermissionId))
	{
		Out = *Found;
		return true;
	}

	return false;
}

bool UModPermissionRegistry::IsPermissionRegistered(const FName PermissionId) const
{
	return Descriptors.Contains(PermissionId);
}

void UModPermissionRegistry::EvaluateManifest(const FModManifest& Manifest)
{
	using namespace ModPermissionRegistryPrivate;

	// The manifest came off disk. A caller that skipped validation is a framework bug, but never a
	// reason to bring the game down.
	if (!Manifest.Id.IsValid())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Permissions: ignoring a permission evaluation for a manifest with no mod id."));
		return;
	}

	const FModId& ModId = Manifest.Id;

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();

	// Fall back to the strict answer when there is no settings object at all, so a broken
	// configuration denies rather than grants.
	const bool bDenyUnknown = Settings != nullptr ? Settings->bDenyUnknownPermissions : true;
	const TArray<FName>& AutoGranted = Settings != nullptr ? Settings->AutoGrantedPermissions : GetEmptyPermissionList();
	const TArray<FName>& AlwaysDenied = Settings != nullptr ? Settings->AlwaysDeniedPermissions : GetEmptyPermissionList();

	// Snapshot the previous record so the delegates below report genuine changes only.
	FModPermissionStateMap PreviousStates;
	if (const FModPermissionStateMap* Existing = ModStates.Find(ModId))
	{
		PreviousStates = *Existing;
	}

	FModPermissionStateMap NewStates;
	NewStates.States.Reserve(Manifest.RequestedPermissions.Num());

	TArray<FName> PendingPermissions;
	int32 GrantedCount = 0;
	int32 DeniedCount = 0;

	for (const FName RequestedPermission : Manifest.RequestedPermissions)
	{
		if (RequestedPermission.IsNone())
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("Permissions: mod '%s' requested a permission with an empty id; ignoring the entry."),
				*ModId.ToString());
			continue;
		}

		if (NewStates.States.Contains(RequestedPermission))
		{
			// Harmless, but the mod author probably did not mean to write it twice.
			UE_LOG(LogModFramework, Verbose,
				TEXT("Permissions: mod '%s' requested '%s' more than once; the duplicate is ignored."),
				*ModId.ToString(), *RequestedPermission.ToString());
			continue;
		}

		// 1. The game's own policy speaks first, and its verdict is final.
		const EModPermissionState PolicyVerdict = ResolveWithPolicy(Manifest, RequestedPermission);

		// Looked up only after the policy has run: a policy is game code, and a policy that registers
		// a permission of its own would rehash Descriptors and invalidate a pointer taken earlier.
		const FModPermissionDescriptor* Descriptor = Descriptors.Find(RequestedPermission);

		EModPermissionState Resolved = EModPermissionState::Pending;
		if (PolicyVerdict != EModPermissionState::NotRequested)
		{
			Resolved = PolicyVerdict;
		}
		// 2. A permission the project put permanently out of reach.
		else if (AlwaysDenied.Contains(RequestedPermission))
		{
			Resolved = EModPermissionState::Denied;
		}
		// 3. Nothing registered this permission, so nothing can even describe it to a player.
		else if (Descriptor == nullptr)
		{
			Resolved = bDenyUnknown ? EModPermissionState::Denied : EModPermissionState::Pending;

			UE_LOG(LogModFramework, Warning,
				TEXT("Permissions: mod '%s' requested the unregistered permission '%s'; %s. Check the id for a typo, or register it with UModPermissionRegistry::RegisterPermission."),
				*ModId.ToString(),
				*RequestedPermission.ToString(),
				bDenyUnknown ? TEXT("denying it") : TEXT("leaving it pending"));
		}
		// 4. Auto-granting is a convenience for harmless capabilities, and only for those.
		else if (AutoGranted.Contains(RequestedPermission))
		{
			if (Descriptor->bDangerous)
			{
				if (!ReportedDangerousAutoGrants.Contains(RequestedPermission))
				{
					ReportedDangerousAutoGrants.Add(RequestedPermission);
					UE_LOG(LogModFramework, Warning,
						TEXT("Permissions: the project lists the dangerous permission '%s' in AutoGrantedPermissions. Dangerous permissions are never auto-granted; requests for it stay pending until something grants them explicitly. Remove it from AutoGrantedPermissions to silence this."),
						*RequestedPermission.ToString());
				}

				Resolved = EModPermissionState::Pending;
			}
			else
			{
				Resolved = EModPermissionState::Granted;
			}
		}
		// 5. Everything else waits for a decision.
		else
		{
			Resolved = EModPermissionState::Pending;
		}

		NewStates.States.Add(RequestedPermission, Resolved);

		switch (Resolved)
		{
		case EModPermissionState::Pending:
			PendingPermissions.Add(RequestedPermission);
			break;
		case EModPermissionState::Granted:
			++GrantedCount;
			break;
		case EModPermissionState::Denied:
			++DeniedCount;
			break;
		default:
			break;
		}
	}

	// Work out what moved before touching anything, so that a delegate handler which calls straight
	// back into the registry sees a fully consistent record.
	TArray<FName> ChangedPermissions;
	for (const TPair<FName, EModPermissionState>& Pair : NewStates.States)
	{
		const EModPermissionState* Previous = PreviousStates.States.Find(Pair.Key);
		if (Previous == nullptr || *Previous != Pair.Value)
		{
			ChangedPermissions.Add(Pair.Key);
		}
	}

	for (const TPair<FName, EModPermissionState>& Pair : PreviousStates.States)
	{
		// A permission this manifest no longer asks for falls back to NotRequested.
		if (Pair.Value != EModPermissionState::NotRequested && !NewStates.States.Contains(Pair.Key))
		{
			ChangedPermissions.Add(Pair.Key);
		}
	}

	const int32 EvaluatedCount = NewStates.States.Num();
	if (EvaluatedCount > 0)
	{
		ModStates.Add(ModId, MoveTemp(NewStates));
	}
	else
	{
		ModStates.Remove(ModId);
	}

	SortPermissionIds(ChangedPermissions);
	SortPermissionIds(PendingPermissions);

	for (const FName Changed : ChangedPermissions)
	{
		OnPermissionChanged.Broadcast(ModId, Changed);
	}

	for (const FName Pending : PendingPermissions)
	{
		OnPermissionRequested.Broadcast(ModId, Pending);
	}

	UE_LOG(LogModFramework, Verbose,
		TEXT("Permissions: evaluated %d permission(s) for mod '%s' - %d granted, %d pending, %d denied."),
		EvaluatedCount, *ModId.ToString(), GrantedCount, PendingPermissions.Num(), DeniedCount);
}

EModPermissionState UModPermissionRegistry::GetPermissionState(const FModId& ModId, const FName PermissionId) const
{
	return FindState(ModId, PermissionId);
}

bool UModPermissionRegistry::HasPermission(const FModId& ModId, const FName PermissionId) const
{
	// Deliberately narrow: Pending is not permission to do anything.
	return FindState(ModId, PermissionId) == EModPermissionState::Granted;
}

TArray<FName> UModPermissionRegistry::GetGrantedPermissions(const FModId& ModId) const
{
	return CollectStates(ModId, EModPermissionState::Granted);
}

TArray<FName> UModPermissionRegistry::GetPendingPermissions(const FModId& ModId) const
{
	return CollectStates(ModId, EModPermissionState::Pending);
}

TArray<FName> UModPermissionRegistry::GetDeniedPermissions(const FModId& ModId) const
{
	return CollectStates(ModId, EModPermissionState::Denied);
}

bool UModPermissionRegistry::HasAllPermissions(const FModId& ModId, const TArray<FName>& PermissionIds) const
{
	const FModPermissionStateMap* States = ModStates.Find(ModId);

	for (const FName PermissionId : PermissionIds)
	{
		if (PermissionId.IsNone())
		{
			// An empty entry names no capability, so there is nothing to withhold.
			continue;
		}

		const EModPermissionState* State = States != nullptr ? States->States.Find(PermissionId) : nullptr;
		if (State == nullptr || *State != EModPermissionState::Granted)
		{
			return false;
		}
	}

	return true;
}

void UModPermissionRegistry::GrantPermission(const FModId& ModId, const FName PermissionId)
{
	if (!ModId.IsValid() || PermissionId.IsNone())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Permissions: GrantPermission ignored a call with an empty mod id or permission id."));
		return;
	}

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	if (Settings != nullptr && Settings->AlwaysDeniedPermissions.Contains(PermissionId))
	{
		// "No mod ever receives this" has to hold for explicit grants too, or the setting is advice.
		UE_LOG(LogModFramework, Warning,
			TEXT("Permissions: refusing to grant '%s' to mod '%s' because the project lists it in AlwaysDeniedPermissions. Recording it as denied instead."),
			*PermissionId.ToString(), *ModId.ToString());

		if (SetStateInternal(ModId, PermissionId, EModPermissionState::Denied))
		{
			OnPermissionChanged.Broadcast(ModId, PermissionId);
		}

		return;
	}

	if (!Descriptors.Contains(PermissionId))
	{
		UE_LOG(LogModFramework, Verbose,
			TEXT("Permissions: granting the unregistered permission '%s' to mod '%s'."),
			*PermissionId.ToString(), *ModId.ToString());
	}

	if (SetStateInternal(ModId, PermissionId, EModPermissionState::Granted))
	{
		UE_LOG(LogModFramework, Log, TEXT("Permissions: granted '%s' to mod '%s'."), *PermissionId.ToString(), *ModId.ToString());
		OnPermissionChanged.Broadcast(ModId, PermissionId);
	}
}

void UModPermissionRegistry::DenyPermission(const FModId& ModId, const FName PermissionId)
{
	if (!ModId.IsValid() || PermissionId.IsNone())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Permissions: DenyPermission ignored a call with an empty mod id or permission id."));
		return;
	}

	if (SetStateInternal(ModId, PermissionId, EModPermissionState::Denied))
	{
		UE_LOG(LogModFramework, Log, TEXT("Permissions: denied '%s' to mod '%s'."), *PermissionId.ToString(), *ModId.ToString());
		OnPermissionChanged.Broadcast(ModId, PermissionId);
	}
}

void UModPermissionRegistry::GrantAll(const FModId& ModId)
{
	using namespace ModPermissionRegistryPrivate;

	if (!ModId.IsValid())
	{
		UE_LOG(LogModFramework, Warning, TEXT("Permissions: GrantAll ignored a call with an empty mod id."));
		return;
	}

	UE_LOG(LogModFramework, Error,
		TEXT("SECURITY: GrantAll granted every known permission to mod '%s'. This bypasses the permission system completely and exists only as a development convenience - it must never run in a shipping build."),
		*ModId.ToString());

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();

	// Everything registered, plus anything this mod already asked for, so that a mod which requested
	// a permission nobody registered is not quietly left out of "all".
	TArray<FName> Targets;
	Descriptors.GenerateKeyArray(Targets);

	if (const FModPermissionStateMap* Existing = ModStates.Find(ModId))
	{
		for (const TPair<FName, EModPermissionState>& Pair : Existing->States)
		{
			Targets.AddUnique(Pair.Key);
		}
	}

	SortPermissionIds(Targets);

	TArray<FName> ChangedPermissions;
	for (const FName PermissionId : Targets)
	{
		if (PermissionId.IsNone())
		{
			continue;
		}

		const bool bAlwaysDenied = Settings != nullptr && Settings->AlwaysDeniedPermissions.Contains(PermissionId);
		if (bAlwaysDenied)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("Permissions: GrantAll left '%s' denied for mod '%s' because the project lists it in AlwaysDeniedPermissions."),
				*PermissionId.ToString(), *ModId.ToString());
		}

		const EModPermissionState NewState = bAlwaysDenied ? EModPermissionState::Denied : EModPermissionState::Granted;
		if (SetStateInternal(ModId, PermissionId, NewState))
		{
			ChangedPermissions.Add(PermissionId);
		}
	}

	for (const FName Changed : ChangedPermissions)
	{
		OnPermissionChanged.Broadcast(ModId, Changed);
	}
}

void UModPermissionRegistry::ResetForMod(const FModId& ModId)
{
	using namespace ModPermissionRegistryPrivate;

	FModPermissionStateMap Removed;
	if (!ModStates.RemoveAndCopyValue(ModId, Removed))
	{
		return;
	}

	TArray<FName> ClearedPermissions;
	Removed.States.GenerateKeyArray(ClearedPermissions);
	SortPermissionIds(ClearedPermissions);

	for (const FName Cleared : ClearedPermissions)
	{
		OnPermissionChanged.Broadcast(ModId, Cleared);
	}

	UE_LOG(LogModFramework, Verbose,
		TEXT("Permissions: cleared %d recorded permission(s) for mod '%s'."),
		ClearedPermissions.Num(), *ModId.ToString());
}

void UModPermissionRegistry::SetPolicy(TScriptInterface<IModPermissionPolicy> InPolicy)
{
	UObject* PolicyObject = InPolicy.GetObject();

	if (PolicyObject == nullptr)
	{
		if (Policy.GetObject() != nullptr)
		{
			UE_LOG(LogModFramework, Log, TEXT("Permissions: permission policy cleared; evaluation falls back to the project settings."));
		}

		Policy = nullptr;
		return;
	}

	if (!PolicyObject->GetClass()->ImplementsInterface(UModPermissionPolicy::StaticClass()))
	{
		// Keeping the previous policy is safer than installing something that cannot be called.
		UE_LOG(LogModFramework, Warning,
			TEXT("Permissions: '%s' does not implement IModPermissionPolicy and was not installed as the permission policy."),
			*PolicyObject->GetName());
		return;
	}

	Policy = InPolicy;

	UE_LOG(LogModFramework, Log, TEXT("Permissions: permission policy set to '%s'."), *PolicyObject->GetName());
}

EModPermissionState UModPermissionRegistry::ResolveWithPolicy(const FModManifest& Manifest, const FName PermissionId) const
{
	using namespace ModPermissionRegistryPrivate;

	UObject* PolicyObject = Policy.GetObject();
	if (PolicyObject == nullptr || !::IsValid(PolicyObject))
	{
		return EModPermissionState::NotRequested;
	}

	// Execute_ asserts on both of these conditions, and the policy object is supplied by the game
	// rather than by this module, so they are checked rather than assumed.
	if (!PolicyObject->GetClass()->ImplementsInterface(UModPermissionPolicy::StaticClass()))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Permissions: the installed permission policy '%s' no longer implements IModPermissionPolicy; ignoring it."),
			*PolicyObject->GetName());
		return EModPermissionState::NotRequested;
	}

	const EModPermissionState Verdict = IModPermissionPolicy::Execute_ResolvePermission(PolicyObject, Manifest, PermissionId);
	if (!IsKnownState(Verdict))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Permissions: policy '%s' returned an out of range state (%d) for '%s'; treating it as no verdict."),
			*PolicyObject->GetName(), static_cast<int32>(Verdict), *PermissionId.ToString());
		return EModPermissionState::NotRequested;
	}

	return Verdict;
}

bool UModPermissionRegistry::SetStateInternal(const FModId& ModId, const FName PermissionId, const EModPermissionState NewState)
{
	if (NewState == EModPermissionState::NotRequested)
	{
		// NotRequested is the absence of a record, not a value worth storing.
		FModPermissionStateMap* ExistingStates = ModStates.Find(ModId);
		if (ExistingStates == nullptr || ExistingStates->States.Remove(PermissionId) == 0)
		{
			return false;
		}

		if (ExistingStates->States.Num() == 0)
		{
			ModStates.Remove(ModId);
		}

		return true;
	}

	FModPermissionStateMap& States = ModStates.FindOrAdd(ModId);
	if (EModPermissionState* Existing = States.States.Find(PermissionId))
	{
		if (*Existing == NewState)
		{
			return false;
		}

		*Existing = NewState;
		return true;
	}

	States.States.Add(PermissionId, NewState);
	return true;
}

EModPermissionState UModPermissionRegistry::FindState(const FModId& ModId, const FName PermissionId) const
{
	if (const FModPermissionStateMap* States = ModStates.Find(ModId))
	{
		if (const EModPermissionState* State = States->States.Find(PermissionId))
		{
			return *State;
		}
	}

	return EModPermissionState::NotRequested;
}

TArray<FName> UModPermissionRegistry::CollectStates(const FModId& ModId, const EModPermissionState Filter) const
{
	using namespace ModPermissionRegistryPrivate;

	TArray<FName> Result;

	if (const FModPermissionStateMap* States = ModStates.Find(ModId))
	{
		Result.Reserve(States->States.Num());
		for (const TPair<FName, EModPermissionState>& Pair : States->States)
		{
			if (Pair.Value == Filter)
			{
				Result.Add(Pair.Key);
			}
		}
	}

	SortPermissionIds(Result);
	return Result;
}
