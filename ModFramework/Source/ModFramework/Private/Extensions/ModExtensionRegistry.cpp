// Copyright (c) 2026. Licensed for use in your own projects.

#include "Extensions/ModExtensionRegistry.h"

#include "API/ModAPIRegistry.h"
#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Extensions/ModExtension.h"
#include "Logging/LogMacros.h"
#include "Math/NumericLimits.h"
#include "Misc/CoreMiscDefines.h"
#include "Templates/SubclassOf.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectPtr.h"

namespace ModExtensionRegistryPrivate
{
	/** Diagnostic codes. Declared once so a typo cannot drift between call sites. */
	static const FName CodeExtensionInvalid(TEXT("Extension.Invalid"));
	static const FName CodePointNotFound(TEXT("Extension.PointNotFound"));
	static const FName CodeBaseClassMismatch(TEXT("Extension.BaseClassMismatch"));
	static const FName CodeDuplicateId(TEXT("Extension.DuplicateId"));
	static const FName CodeMultipleNotAllowed(TEXT("Extension.MultipleNotAllowed"));
	static const FName CodePermissionDenied(TEXT("Extension.PermissionDenied"));
	static const FName CodePointDuplicate(TEXT("ExtensionPoint.Duplicate"));
	static const FName CodePointInvalidId(TEXT("ExtensionPoint.InvalidId"));
	static const FName CodePointInUse(TEXT("ExtensionPoint.InUse"));

	/** "<class name>" of an extension, safe to call on a partially torn down object. */
	static FString DescribeClass(const UModExtension* In)
	{
		const UClass* Class = In ? In->GetClass() : nullptr;
		return Class ? Class->GetName() : FString(TEXT("<unknown class>"));
	}

	/**
	 * Puts a set of extension point ids into a deterministic order.
	 *
	 * TMap iteration order depends on the insertion and removal history, so anything that feeds a
	 * report, a digest or a conflict list has to sort the keys first.
	 */
	static void SortPointIds(TArray<FName>& InOutPointIds)
	{
		InOutPointIds.Sort([](const FName& A, const FName& B)
		{
			return A.LexicalLess(B);
		});
	}
}

//////////////////////////////////////////////////////////////////////////
// Extension points

bool UModExtensionRegistry::RegisterExtensionPoint(const FModExtensionPointDescriptor& InDescriptor, FModDiagnostic& OutError)
{
	using namespace ModExtensionRegistryPrivate;

	if (InDescriptor.ExtensionPointId.IsNone())
	{
		OutError = FModDiagnostic::Error(
			CodePointInvalidId,
			TEXT("An extension point must declare a non-empty ExtensionPointId."));
		return false;
	}

	if (ExtensionPoints.Contains(InDescriptor.ExtensionPointId))
	{
		OutError = FModDiagnostic::Error(
			CodePointDuplicate,
			FString::Printf(TEXT("Extension point '%s' is already registered."), *InDescriptor.ExtensionPointId.ToString()),
			InDescriptor.ExtensionPointId.ToString());
		return false;
	}

	ExtensionPoints.Add(InDescriptor.ExtensionPointId, InDescriptor);

	// Create the (empty) bucket up front so every later lookup can assume the two maps agree.
	Extensions.FindOrAdd(InDescriptor.ExtensionPointId);

	UE_LOG(LogModFramework, Verbose, TEXT("Registered extension point '%s'."), *InDescriptor.ExtensionPointId.ToString());

	OnExtensionPointsChanged.Broadcast(InDescriptor.ExtensionPointId, FModId());
	return true;
}

bool UModExtensionRegistry::UnregisterExtensionPoint(FName ExtensionPointId)
{
	using namespace ModExtensionRegistryPrivate;

	if (!ExtensionPoints.Contains(ExtensionPointId))
	{
		return false;
	}

	if (const FModExtensionList* List = Extensions.Find(ExtensionPointId))
	{
		if (List->Extensions.Num() > 0)
		{
			// There is no OutError on this signature, so the code goes into the log instead: closing a
			// point out from under live extensions would leave them orphaned but still referenced.
			UE_LOG(LogModFramework, Warning,
				TEXT("%s: refusing to unregister extension point '%s', %d extension(s) are still registered against it."),
				*CodePointInUse.ToString(), *ExtensionPointId.ToString(), List->Extensions.Num());
			return false;
		}
	}

	ExtensionPoints.Remove(ExtensionPointId);
	Extensions.Remove(ExtensionPointId);

	UE_LOG(LogModFramework, Verbose, TEXT("Unregistered extension point '%s'."), *ExtensionPointId.ToString());

	OnExtensionPointsChanged.Broadcast(ExtensionPointId, FModId());
	return true;
}

bool UModExtensionRegistry::GetExtensionPoint(FName ExtensionPointId, FModExtensionPointDescriptor& OutDescriptor) const
{
	if (const FModExtensionPointDescriptor* Found = ExtensionPoints.Find(ExtensionPointId))
	{
		OutDescriptor = *Found;
		return true;
	}

	OutDescriptor = FModExtensionPointDescriptor();
	return false;
}

TArray<FModExtensionPointDescriptor> UModExtensionRegistry::GetExtensionPoints() const
{
	TArray<FModExtensionPointDescriptor> Result;
	Result.Reserve(ExtensionPoints.Num());

	for (const TPair<FName, FModExtensionPointDescriptor>& Pair : ExtensionPoints)
	{
		Result.Add(Pair.Value);
	}

	Result.Sort([](const FModExtensionPointDescriptor& A, const FModExtensionPointDescriptor& B)
	{
		return A.ExtensionPointId.LexicalLess(B.ExtensionPointId);
	});

	return Result;
}

//////////////////////////////////////////////////////////////////////////
// Extensions

bool UModExtensionRegistry::RegisterExtension(UModExtension* InExtension, const FModId& OwningModId, FModDiagnostic& OutError)
{
	using namespace ModExtensionRegistryPrivate;

	// 1. The object itself.
	if (!IsValid(InExtension))
	{
		OutError = FModDiagnostic::Error(
			CodeExtensionInvalid,
			TEXT("Cannot register a null or garbage-collected extension."),
			OwningModId.ToString());
		OutError.ModId = OwningModId;
		return false;
	}

	if (InExtension->GetClass() == nullptr)
	{
		OutError = FModDiagnostic::Error(
			CodeExtensionInvalid,
			TEXT("Extension has no class and cannot be type-checked against its extension point."),
			OwningModId.ToString());
		OutError.ModId = OwningModId;
		return false;
	}

	const FName PointId = InExtension->ExtensionPointId;
	if (PointId.IsNone())
	{
		OutError = FModDiagnostic::Error(
			CodeExtensionInvalid,
			FString::Printf(TEXT("Extension '%s' does not declare an ExtensionPointId."), *DescribeClass(InExtension)),
			OwningModId.ToString());
		OutError.ModId = OwningModId;
		return false;
	}

	// 2. The extension point has to exist. Extensions are never queued for a point that may open
	//    later: a mod would then silently do nothing, which is far harder to diagnose than an error.
	const FModExtensionPointDescriptor* Descriptor = ExtensionPoints.Find(PointId);
	if (Descriptor == nullptr)
	{
		OutError = FModDiagnostic::Error(
			CodePointNotFound,
			FString::Printf(TEXT("Extension '%s' targets extension point '%s', which no game system registered."),
				*DescribeClass(InExtension), *PointId.ToString()),
			PointId.ToString());
		OutError.ModId = OwningModId;
		return false;
	}

	// 3. Typing.
	if (UClass* RequiredBase = Descriptor->RequiredBaseClass.Get())
	{
		if (!InExtension->GetClass()->IsChildOf(RequiredBase))
		{
			OutError = FModDiagnostic::Error(
				CodeBaseClassMismatch,
				FString::Printf(TEXT("Extension '%s' must derive from '%s' to plug into extension point '%s'."),
					*DescribeClass(InExtension), *RequiredBase->GetName(), *PointId.ToString()),
				PointId.ToString());
			OutError.ModId = OwningModId;
			return false;
		}
	}

	// The resolved id depends on OwningModId, so the assignment has to happen before it is computed.
	// Every failure path below restores the previous owner: a rejected extension must come out of
	// this function exactly as it went in.
	const FModId PreviousOwner = InExtension->OwningModId;
	InExtension->OwningModId = OwningModId;

	const FName ResolvedId = InExtension->GetResolvedExtensionId();

	FModExtensionList& List = Extensions.FindOrAdd(PointId);

	// 4. Identity within the point.
	for (const TObjectPtr<UModExtension>& Existing : List.Extensions)
	{
		if (Existing && Existing->GetResolvedExtensionId() == ResolvedId)
		{
			InExtension->OwningModId = PreviousOwner;

			OutError = FModDiagnostic::Error(
				CodeDuplicateId,
				FString::Printf(TEXT("Extension id '%s' is already registered against extension point '%s' by mod '%s'."),
					*ResolvedId.ToString(), *PointId.ToString(), *Existing->OwningModId.ToString()),
				PointId.ToString());
			OutError.ModId = OwningModId;
			return false;
		}
	}

	// 5. Quota.
	if (!Descriptor->bAllowMultiplePerMod)
	{
		for (const TObjectPtr<UModExtension>& Existing : List.Extensions)
		{
			if (Existing && Existing->OwningModId == OwningModId)
			{
				InExtension->OwningModId = PreviousOwner;

				OutError = FModDiagnostic::Error(
					CodeMultipleNotAllowed,
					FString::Printf(TEXT("Extension point '%s' accepts one extension per mod and '%s' already registered '%s'."),
						*PointId.ToString(), *OwningModId.ToString(), *Existing->GetResolvedExtensionId().ToString()),
					PointId.ToString());
				OutError.ModId = OwningModId;
				return false;
			}
		}
	}

	// 6. Permissions. With no check injected every permission counts as held, so a registry used
	//    standalone (tests, editor tooling) is not crippled by a policy that does not exist there.
	if (PermissionCheck.IsBound())
	{
		for (const FName RequiredPermission : Descriptor->RequiredPermissions)
		{
			if (RequiredPermission.IsNone())
			{
				continue;
			}

			if (!PermissionCheck.Execute(OwningModId, RequiredPermission))
			{
				InExtension->OwningModId = PreviousOwner;

				OutError = FModDiagnostic::Error(
					CodePermissionDenied,
					FString::Printf(TEXT("Mod '%s' does not hold permission '%s', required by extension point '%s'."),
						*OwningModId.ToString(), *RequiredPermission.ToString(), *PointId.ToString()),
					PointId.ToString());
				OutError.ModId = OwningModId;
				return false;
			}
		}
	}

	List.Extensions.Add(InExtension);
	SortExtensionList(List);

	// An extension is registered inactive; UModSubsystem activates it with the owning mod.
	InExtension->bExtensionActive = false;

	UE_LOG(LogModFramework, Verbose, TEXT("Mod '%s' registered extension '%s' at point '%s'."),
		*OwningModId.ToString(), *ResolvedId.ToString(), *PointId.ToString());

	// Mod-authored code runs from here on, so nothing below may touch `List` again: a Blueprint
	// handler is free to register or unregister more extensions and reallocate that array.
	InExtension->OnExtensionRegistered();

	OnExtensionsChanged.Broadcast(PointId, OwningModId);
	return true;
}

bool UModExtensionRegistry::UnregisterExtension(FName ExtensionPointId, FName ExtensionId)
{
	FModExtensionList* List = Extensions.Find(ExtensionPointId);
	if (List == nullptr)
	{
		return false;
	}

	int32 FoundIndex = INDEX_NONE;
	for (int32 Index = 0; Index < List->Extensions.Num(); ++Index)
	{
		const TObjectPtr<UModExtension>& Candidate = List->Extensions[Index];
		if (Candidate && Candidate->GetResolvedExtensionId() == ExtensionId)
		{
			FoundIndex = Index;
			break;
		}
	}

	if (FoundIndex == INDEX_NONE)
	{
		return false;
	}

	UModExtension* Extension = List->Extensions[FoundIndex];
	const FModId OwnerId = Extension ? Extension->OwningModId : FModId();

	// Detach before running the callbacks so a handler that queries the registry sees the new state.
	List->Extensions.RemoveAt(FoundIndex);

	ReleaseExtension(Extension);

	UE_LOG(LogModFramework, Verbose, TEXT("Unregistered extension '%s' from point '%s'."),
		*ExtensionId.ToString(), *ExtensionPointId.ToString());

	OnExtensionsChanged.Broadcast(ExtensionPointId, OwnerId);
	return true;
}

void UModExtensionRegistry::UnregisterAllForMod(const FModId& ModId)
{
	TArray<UModExtension*> Removed;
	TArray<FName> AffectedPoints;

	for (TPair<FName, FModExtensionList>& Pair : Extensions)
	{
		bool bPointChanged = false;

		for (int32 Index = Pair.Value.Extensions.Num() - 1; Index >= 0; --Index)
		{
			UModExtension* Candidate = Pair.Value.Extensions[Index];

			// A null slot is an extension the GC already reclaimed; drop it while we are here.
			if (Candidate == nullptr)
			{
				Pair.Value.Extensions.RemoveAt(Index);
				bPointChanged = true;
				continue;
			}

			if (Candidate->OwningModId == ModId)
			{
				Removed.Add(Candidate);
				Pair.Value.Extensions.RemoveAt(Index);
				bPointChanged = true;
			}
		}

		if (bPointChanged)
		{
			AffectedPoints.Add(Pair.Key);
		}
	}

	// Callbacks only after the whole registry is consistent, so a handler cannot observe a half
	// removed mod - and cannot invalidate the map iterator above either.
	for (UModExtension* Extension : Removed)
	{
		ReleaseExtension(Extension);
	}

	if (Removed.Num() > 0)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Unregistered %d extension(s) belonging to mod '%s'."),
			Removed.Num(), *ModId.ToString());
	}

	for (const FName PointId : AffectedPoints)
	{
		OnExtensionsChanged.Broadcast(PointId, ModId);
	}
}

void UModExtensionRegistry::SetModExtensionsActive(const FModId& ModId, bool bActive)
{
	TArray<UModExtension*> Changed;
	TArray<FName> AffectedPoints;

	for (TPair<FName, FModExtensionList>& Pair : Extensions)
	{
		bool bPointChanged = false;

		for (const TObjectPtr<UModExtension>& Candidate : Pair.Value.Extensions)
		{
			if (Candidate && Candidate->OwningModId == ModId && Candidate->bExtensionActive != bActive)
			{
				Candidate->bExtensionActive = bActive;
				Changed.Add(Candidate.Get());
				bPointChanged = true;
			}
		}

		if (bPointChanged)
		{
			AffectedPoints.Add(Pair.Key);
		}
	}

	// The flags are all set before any handler runs, so an extension that inspects its siblings
	// during activation sees the mod as a whole rather than a partially activated one.
	for (UModExtension* Extension : Changed)
	{
		if (!IsValid(Extension))
		{
			continue;
		}

		if (bActive)
		{
			Extension->OnExtensionActivated();
		}
		else
		{
			Extension->OnExtensionDeactivated();
		}
	}

	for (const FName PointId : AffectedPoints)
	{
		OnExtensionsChanged.Broadcast(PointId, ModId);
	}
}

void UModExtensionRegistry::Reset()
{
	TArray<UModExtension*> Released;

	for (TPair<FName, FModExtensionList>& Pair : Extensions)
	{
		for (const TObjectPtr<UModExtension>& Candidate : Pair.Value.Extensions)
		{
			if (Candidate)
			{
				Released.Add(Candidate.Get());
			}
		}
	}

	Extensions.Reset();
	ExtensionPoints.Reset();
	ModLoadOrder.Reset();

	for (UModExtension* Extension : Released)
	{
		ReleaseExtension(Extension);
	}

	// A None point id means "assume everything you cached is stale".
	OnExtensionsChanged.Broadcast(NAME_None, FModId());
	OnExtensionPointsChanged.Broadcast(NAME_None, FModId());
}

//////////////////////////////////////////////////////////////////////////
// Queries

TArray<UModExtension*> UModExtensionRegistry::GetExtensions(FName ExtensionPointId) const
{
	TArray<UModExtension*> Result;

	const FModExtensionList* List = Extensions.Find(ExtensionPointId);
	if (List == nullptr)
	{
		return Result;
	}

	Result.Reserve(List->Extensions.Num());
	for (const TObjectPtr<UModExtension>& Candidate : List->Extensions)
	{
		if (Candidate && Candidate->bExtensionActive)
		{
			Result.Add(Candidate.Get());
		}
	}

	return Result;
}

TArray<UModExtension*> UModExtensionRegistry::GetAllExtensions(FName ExtensionPointId) const
{
	TArray<UModExtension*> Result;

	const FModExtensionList* List = Extensions.Find(ExtensionPointId);
	if (List == nullptr)
	{
		return Result;
	}

	Result.Reserve(List->Extensions.Num());
	for (const TObjectPtr<UModExtension>& Candidate : List->Extensions)
	{
		if (Candidate)
		{
			Result.Add(Candidate.Get());
		}
	}

	return Result;
}

UModExtension* UModExtensionRegistry::FindExtension(FName ExtensionPointId, FName ExtensionId) const
{
	const FModExtensionList* List = Extensions.Find(ExtensionPointId);
	if (List == nullptr)
	{
		return nullptr;
	}

	for (const TObjectPtr<UModExtension>& Candidate : List->Extensions)
	{
		if (Candidate && Candidate->GetResolvedExtensionId() == ExtensionId)
		{
			return Candidate.Get();
		}
	}

	return nullptr;
}

TArray<UModExtension*> UModExtensionRegistry::GetExtensionsForMod(const FModId& ModId) const
{
	TArray<UModExtension*> Result;

	// This result feeds diagnostics that have to read the same way on every machine.
	TArray<FName> PointIds;
	Extensions.GenerateKeyArray(PointIds);
	ModExtensionRegistryPrivate::SortPointIds(PointIds);

	for (const FName PointId : PointIds)
	{
		const FModExtensionList* List = Extensions.Find(PointId);
		if (List == nullptr)
		{
			continue;
		}

		for (const TObjectPtr<UModExtension>& Candidate : List->Extensions)
		{
			if (Candidate && Candidate->OwningModId == ModId)
			{
				Result.Add(Candidate.Get());
			}
		}
	}

	return Result;
}

UModExtension* UModExtensionRegistry::ResolveResource(FName ExtensionPointId, FName ResourceId) const
{
	const FModExtensionPointDescriptor* Descriptor = ExtensionPoints.Find(ExtensionPointId);
	if (Descriptor == nullptr)
	{
		return nullptr;
	}

	const FModExtensionList* List = Extensions.Find(ExtensionPointId);
	if (List == nullptr)
	{
		return nullptr;
	}

	// Only activated extensions may win a resource: a mod that is merely loaded must not change what
	// the game sees. Contenders inherit the list's total order, which keeps every tie-break below
	// deterministic.
	TArray<UModExtension*> Contenders;
	for (const TObjectPtr<UModExtension>& Candidate : List->Extensions)
	{
		if (Candidate && Candidate->bExtensionActive && Candidate->ClaimedResourceIds.Contains(ResourceId))
		{
			Contenders.Add(Candidate.Get());
		}
	}

	if (Contenders.Num() == 0)
	{
		return nullptr;
	}

	// An uncontested claim wins under every policy, Error and Merge included: there is nothing to
	// reconcile.
	if (Contenders.Num() == 1)
	{
		return Contenders[0];
	}

	UModExtension* Winner = Contenders[0];

	switch (Descriptor->DefaultConflictPolicy)
	{
	case EModConflictPolicy::FirstWins:
	{
		int32 BestOrder = GetSortLoadOrder(Winner->OwningModId);
		for (int32 Index = 1; Index < Contenders.Num(); ++Index)
		{
			const int32 Order = GetSortLoadOrder(Contenders[Index]->OwningModId);
			if (Order < BestOrder)
			{
				BestOrder = Order;
				Winner = Contenders[Index];
			}
		}
		return Winner;
	}

	case EModConflictPolicy::LastWins:
	{
		int32 BestOrder = GetSortLoadOrder(Winner->OwningModId);
		for (int32 Index = 1; Index < Contenders.Num(); ++Index)
		{
			const int32 Order = GetSortLoadOrder(Contenders[Index]->OwningModId);
			if (Order > BestOrder)
			{
				BestOrder = Order;
				Winner = Contenders[Index];
			}
		}
		return Winner;
	}

	case EModConflictPolicy::Priority:
	{
		int32 BestPriority = Winner->Priority;
		int32 BestOrder = GetSortLoadOrder(Winner->OwningModId);
		for (int32 Index = 1; Index < Contenders.Num(); ++Index)
		{
			UModExtension* Candidate = Contenders[Index];
			const int32 CandidatePriority = Candidate->Priority;
			const int32 CandidateOrder = GetSortLoadOrder(Candidate->OwningModId);

			if (CandidatePriority > BestPriority || (CandidatePriority == BestPriority && CandidateOrder > BestOrder))
			{
				BestPriority = CandidatePriority;
				BestOrder = CandidateOrder;
				Winner = Candidate;
			}
		}
		return Winner;
	}

	case EModConflictPolicy::Error:
	case EModConflictPolicy::Merge:
	default:
		// Error means "a human has to decide" and Merge means "there is no single winner"; either way
		// handing back one of the contenders would be a lie.
		UE_LOG(LogModFramework, Verbose,
			TEXT("Resource '%s' on extension point '%s' has %d active contenders under policy %s; no single winner."),
			*ResourceId.ToString(), *ExtensionPointId.ToString(), Contenders.Num(),
			*ModFrameworkEnums::ToString(Descriptor->DefaultConflictPolicy));
		return nullptr;
	}
}

TArray<FModResourceClaim> UModExtensionRegistry::CollectResourceClaims() const
{
	TArray<FModResourceClaim> Claims;

	TArray<FName> PointIds;
	Extensions.GenerateKeyArray(PointIds);
	ModExtensionRegistryPrivate::SortPointIds(PointIds);

	for (const FName PointId : PointIds)
	{
		const FModExtensionList* List = Extensions.Find(PointId);
		if (List == nullptr)
		{
			continue;
		}

		const FModExtensionPointDescriptor* Descriptor = ExtensionPoints.Find(PointId);
		const EModConflictPolicy Policy = Descriptor ? Descriptor->DefaultConflictPolicy : EModConflictPolicy::Error;

		for (const TObjectPtr<UModExtension>& Candidate : List->Extensions)
		{
			if (!Candidate)
			{
				continue;
			}

			const int32* KnownOrder = ModLoadOrder.Find(Candidate->OwningModId);
			const FName ResolvedId = Candidate->GetResolvedExtensionId();

			for (const FName ResourceId : Candidate->ClaimedResourceIds)
			{
				// A mod may leave an empty entry in its ClaimedResourceIds array; it claims nothing.
				if (ResourceId.IsNone())
				{
					continue;
				}

				FModResourceClaim& Claim = Claims.AddDefaulted_GetRef();
				Claim.ModId = Candidate->OwningModId;
				Claim.ExtensionPointId = PointId;
				Claim.ResourceId = ResourceId;
				Claim.Priority = Candidate->Priority;
				Claim.LoadOrder = KnownOrder ? *KnownOrder : INDEX_NONE;
				Claim.PreferredPolicy = Policy;
				Claim.ExtensionId = ResolvedId;
			}
		}
	}

	return Claims;
}

//////////////////////////////////////////////////////////////////////////
// Ordering and injection

void UModExtensionRegistry::SetModLoadOrder(const TArray<FModId>& InOrder)
{
	ModLoadOrder.Reset();
	ModLoadOrder.Reserve(InOrder.Num());

	for (int32 Index = 0; Index < InOrder.Num(); ++Index)
	{
		// The resolver never emits a duplicate, but keeping the first occurrence makes a hand-built
		// order behave predictably instead of depending on which entry landed last.
		if (!ModLoadOrder.Contains(InOrder[Index]))
		{
			ModLoadOrder.Add(InOrder[Index], Index);
		}
	}

	TArray<FName> AffectedPoints;
	for (TPair<FName, FModExtensionList>& Pair : Extensions)
	{
		if (Pair.Value.Extensions.Num() > 1)
		{
			SortExtensionList(Pair.Value);
			AffectedPoints.Add(Pair.Key);
		}
	}

	for (const FName PointId : AffectedPoints)
	{
		OnExtensionsChanged.Broadcast(PointId, FModId());
	}
}

void UModExtensionRegistry::SetPermissionCheck(UModAPIRegistry::FModPermissionCheck InCheck)
{
	PermissionCheck = MoveTemp(InCheck);
}

//////////////////////////////////////////////////////////////////////////
// Internals

int32 UModExtensionRegistry::GetSortLoadOrder(const FModId& ModId) const
{
	if (const int32* Found = ModLoadOrder.Find(ModId))
	{
		return *Found;
	}

	// A mod the resolver never ordered (a game-owned extension, or one registered before the load
	// order was published) sorts after every known mod.
	return MAX_int32;
}

bool UModExtensionRegistry::ExtensionLess(const UModExtension& A, const UModExtension& B) const
{
	if (A.Priority != B.Priority)
	{
		return A.Priority > B.Priority;
	}

	const int32 OrderA = GetSortLoadOrder(A.OwningModId);
	const int32 OrderB = GetSortLoadOrder(B.OwningModId);
	if (OrderA != OrderB)
	{
		return OrderA < OrderB;
	}

	// Resolved ids are unique within a point, so this always breaks the tie and the whole comparison
	// is a strict total order - the sort result cannot depend on the incoming array order.
	return A.GetResolvedExtensionId().LexicalLess(B.GetResolvedExtensionId());
}

void UModExtensionRegistry::SortExtensionList(FModExtensionList& InOutList)
{
	// Stale slots must be dropped BEFORE sorting, not sorted to the end. TArray<TObjectPtr<T>>::Sort
	// routes through TDereferenceWrapper (ObjectPtr.h), which calls Predicate(*A, *B) - it
	// dereferences every element before the predicate ever sees it. A null-check inside the predicate
	// is therefore unreachable, and a null slot crashes inside the engine wrapper rather than here.
	// GC can null a UPROPERTY TObjectPtr slot at any time, so this is a real path, not a formality.
	InOutList.Extensions.RemoveAll([](const TObjectPtr<UModExtension>& Entry)
	{
		return !IsValid(Entry.Get());
	});

	// Consequently the predicate takes references, not pointers.
	InOutList.Extensions.Sort([this](const UModExtension& A, const UModExtension& B)
	{
		return ExtensionLess(A, B);
	});
}

void UModExtensionRegistry::ReleaseExtension(UModExtension* InExtension)
{
	if (!IsValid(InExtension))
	{
		return;
	}

	if (InExtension->bExtensionActive)
	{
		InExtension->bExtensionActive = false;
		InExtension->OnExtensionDeactivated();
	}

	InExtension->OnExtensionUnregistered();
}
