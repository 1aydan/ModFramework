// Copyright (c) 2026. Licensed for use in your own projects.

#include "API/ModAPIRegistry.h"

#include "API/ModAPI.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModVersion.h"
#include "Templates/SubclassOf.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectPtr.h"

namespace
{
	/** Deterministic ordering for everything the registry reports. */
	bool ApiIdLess(FName A, FName B)
	{
		return A.LexicalLess(B);
	}

	/** "the game" or "mod '<id>'", for log lines and diagnostics. */
	FString DescribeProvider(const FModId& ProviderModId)
	{
		return ProviderModId.IsValid()
			? FString::Printf(TEXT("mod '%s'"), *ProviderModId.ToString())
			: FString(TEXT("the game"));
	}
}

bool UModAPIRegistry::RegisterAPI(UModAPI* InApi, FModDiagnostic& OutError)
{
	return RegisterAPIForMod(InApi, FModId(), OutError);
}

bool UModAPIRegistry::RegisterAPIForMod(UModAPI* InApi, const FModId& ProviderModId, FModDiagnostic& OutError)
{
	OutError = FModDiagnostic();

	if (!IsValid(InApi))
	{
		OutError = FModDiagnostic::Error(
			TEXT("API.NullApi"),
			FString::Printf(TEXT("%s tried to register a mod API, but the object was null or already garbage."),
				*DescribeProvider(ProviderModId)),
			ProviderModId.ToString());
		OutError.ModId = ProviderModId;

		UE_LOG(LogModFramework, Warning, TEXT("%s"), *OutError.Message);
		return false;
	}

	const UClass* ApiClass = InApi->GetClass();
	const FString ApiClassPath = ApiClass != nullptr ? ApiClass->GetPathName() : FString(TEXT("<null class>"));

	const FName ApiId = InApi->GetApiId();

	if (ApiId.IsNone())
	{
		OutError = FModDiagnostic::Error(
			TEXT("API.InvalidId"),
			FString::Printf(TEXT("The mod API class '%s' resolves to an empty id, so it cannot be registered. Declare ModApiId class metadata, override NativeGetApiId, or set ApiIdOverride."),
				*ApiClassPath),
			ApiClassPath);
		OutError.ModId = ProviderModId;

		UE_LOG(LogModFramework, Warning, TEXT("%s"), *OutError.Message);
		return false;
	}

	if (const FModAPIRegistration* Existing = Registrations.Find(ApiId))
	{
		OutError = FModDiagnostic::Error(
			TEXT("API.Duplicate"),
			FString::Printf(TEXT("A mod API is already registered under the id '%s' (class '%s', provided by %s); the registration of '%s' by %s was refused."),
				*ApiId.ToString(),
				*Existing->Descriptor.ClassPath,
				*DescribeProvider(Existing->Descriptor.ProviderModId),
				*ApiClassPath,
				*DescribeProvider(ProviderModId)),
			ApiId.ToString());
		OutError.ModId = ProviderModId;

		UE_LOG(LogModFramework, Warning, TEXT("%s"), *OutError.Message);
		return false;
	}

	// The same object under two ids would be notified - and torn down - twice, so refuse that too.
	for (const TPair<FName, FModAPIRegistration>& Pair : Registrations)
	{
		if (Pair.Value.Api.Get() == InApi)
		{
			OutError = FModDiagnostic::Error(
				TEXT("API.Duplicate"),
				FString::Printf(TEXT("The mod API object '%s' is already registered under the id '%s' and cannot also be registered as '%s'."),
					*InApi->GetPathName(),
					*Pair.Key.ToString(),
					*ApiId.ToString()),
				ApiId.ToString());
			OutError.ModId = ProviderModId;

			UE_LOG(LogModFramework, Warning, TEXT("%s"), *OutError.Message);
			return false;
		}
	}

	FModAPIRegistration Registration;
	Registration.Api = InApi;
	Registration.Descriptor = InApi->BuildDescriptor();

	// BuildDescriptor cannot know either of these: the id is re-stamped so the map key and the
	// descriptor can never disagree, and only the registry knows who supplied the API.
	Registration.Descriptor.ApiId = ApiId;
	Registration.Descriptor.ProviderModId = ProviderModId;

	const FModVersion RegisteredVersion = Registration.Descriptor.Version;
	Registrations.Add(ApiId, MoveTemp(Registration));

	// Notify before broadcasting so listeners always observe a fully initialised API.
	InApi->OnAPIRegistered(this);

	UE_LOG(LogModFramework, Log, TEXT("Registered mod API '%s' version %s from %s (class '%s')."),
		*ApiId.ToString(),
		*RegisteredVersion.ToString(),
		*DescribeProvider(ProviderModId),
		*ApiClassPath);

	OnAPIRegistered.Broadcast(ApiId);
	return true;
}

bool UModAPIRegistry::UnregisterAPI(FName ApiId)
{
	FModAPIRegistration Removed;

	if (!Registrations.RemoveAndCopyValue(ApiId, Removed))
	{
		return false;
	}

	FinalizeRemoval(Removed, ApiId);
	return true;
}

void UModAPIRegistry::UnregisterAllForMod(const FModId& ModId)
{
	// An invalid provider id marks a game-provided API, which no mod may ever take away.
	if (!ModId.IsValid())
	{
		return;
	}

	TArray<FName> ToRemove;

	for (const TPair<FName, FModAPIRegistration>& Pair : Registrations)
	{
		if (Pair.Value.Descriptor.ProviderModId == ModId)
		{
			ToRemove.Add(Pair.Key);
		}
	}

	ToRemove.Sort([](FName A, FName B) { return ApiIdLess(A, B); });

	for (const FName ApiId : ToRemove)
	{
		UnregisterAPI(ApiId);
	}
}

void UModAPIRegistry::Reset()
{
	// Detach everything first: a notified API may call back into the registry, and it has to see an
	// empty one rather than a half-dismantled map.
	TMap<FName, FModAPIRegistration> Removed = MoveTemp(Registrations);
	Registrations.Reset();

	TArray<FName> ApiIds;
	Removed.GetKeys(ApiIds);
	ApiIds.Sort([](FName A, FName B) { return ApiIdLess(A, B); });

	for (const FName ApiId : ApiIds)
	{
		if (const FModAPIRegistration* Registration = Removed.Find(ApiId))
		{
			FinalizeRemoval(*Registration, ApiId);
		}
	}
}

void UModAPIRegistry::FinalizeRemoval(const FModAPIRegistration& Registration, FName ApiId)
{
	UModAPI* Api = Registration.Api.Get();

	if (IsValid(Api))
	{
		Api->OnAPIUnregistered();
	}

	UE_LOG(LogModFramework, Log, TEXT("Unregistered mod API '%s'."), *ApiId.ToString());

	OnAPIUnregistered.Broadcast(ApiId);
}

UModAPI* UModAPIRegistry::FindAPI(FName ApiId) const
{
	const FModAPIRegistration* Registration = Registrations.Find(ApiId);

	if (Registration == nullptr)
	{
		return nullptr;
	}

	UModAPI* Api = Registration->Api.Get();
	return IsValid(Api) ? Api : nullptr;
}

bool UModAPIRegistry::HasAPI(FName ApiId) const
{
	return FindAPI(ApiId) != nullptr;
}

TArray<FModAPIDescriptor> UModAPIRegistry::GetAllAPIs() const
{
	TArray<FModAPIDescriptor> Descriptors;
	Descriptors.Reserve(Registrations.Num());

	for (const TPair<FName, FModAPIRegistration>& Pair : Registrations)
	{
		Descriptors.Add(Pair.Value.Descriptor);
	}

	Descriptors.Sort([](const FModAPIDescriptor& A, const FModAPIDescriptor& B)
	{
		return ApiIdLess(A.ApiId, B.ApiId);
	});

	return Descriptors;
}

bool UModAPIRegistry::GetAPIDescriptor(FName ApiId, FModAPIDescriptor& OutDescriptor) const
{
	if (const FModAPIRegistration* Registration = Registrations.Find(ApiId))
	{
		OutDescriptor = Registration->Descriptor;
		return true;
	}

	OutDescriptor = FModAPIDescriptor();
	return false;
}

UModAPI* UModAPIRegistry::RequestAPI(FName ApiId, const FModVersionRange& RequiredRange, const FModId& RequestingMod, FModDiagnostic& OutError)
{
	OutError = FModDiagnostic();

	const FModAPIRegistration* Registration = Registrations.Find(ApiId);
	UModAPI* Api = Registration != nullptr ? Registration->Api.Get() : nullptr;

	if (Registration == nullptr || !IsValid(Api))
	{
		OutError = FModDiagnostic::Error(
			TEXT("API.NotFound"),
			FString::Printf(TEXT("No mod API is registered under the id '%s'."), *ApiId.ToString()),
			ApiId.ToString());
		OutError.ModId = RequestingMod;
		return nullptr;
	}

	const FModAPIDescriptor& Descriptor = Registration->Descriptor;

	if (!RequiredRange.Satisfies(Descriptor.Version))
	{
		const FString RangeText = RequiredRange.ToString().IsEmpty() ? FString(TEXT("*")) : RequiredRange.ToString();

		OutError = FModDiagnostic::Error(
			TEXT("API.VersionMismatch"),
			FString::Printf(TEXT("Mod API '%s' is version %s, which does not satisfy the requested range '%s' (%s)."),
				*ApiId.ToString(),
				*Descriptor.Version.ToString(),
				*RangeText,
				*RequiredRange.Describe()),
			ApiId.ToString());
		OutError.ModId = RequestingMod;
		return nullptr;
	}

	FName DeniedPermission = NAME_None;
	bool bNoCheckInstalled = false;

	if (!CheckPermissions(Descriptor.RequiredPermissions, RequestingMod, DeniedPermission, bNoCheckInstalled))
	{
		const FString Message = bNoCheckInstalled
			? FString::Printf(TEXT("Mod API '%s' requires the permission '%s', but no permission check is installed on the API registry, so the request from mod '%s' was refused."),
				*ApiId.ToString(), *DeniedPermission.ToString(), *RequestingMod.ToString())
			: FString::Printf(TEXT("Mod '%s' does not hold the permission '%s' required by the mod API '%s'."),
				*RequestingMod.ToString(), *DeniedPermission.ToString(), *ApiId.ToString());

		OutError = FModDiagnostic::Error(TEXT("API.PermissionDenied"), Message, ApiId.ToString());
		OutError.ModId = RequestingMod;

		UE_LOG(LogModFramework, Warning, TEXT("%s"), *Message);
		return nullptr;
	}

	return Api;
}

UModAPI* UModAPIRegistry::K2_RequestAPI(FName ApiId, TSubclassOf<UModAPI> ApiClass, const FString& VersionRange, FModId RequestingMod, FModDiagnostic& OutError)
{
	OutError = FModDiagnostic();

	FModVersionRange Range = FModVersionRange::Any();

	if (!VersionRange.TrimStartAndEnd().IsEmpty())
	{
		FString ParseError;

		if (!FModVersionRange::Parse(VersionRange, Range, &ParseError))
		{
			// Refuse rather than silently widening a range the caller got wrong: a mod that asked
			// for ">=2" and typo'd it must not be handed a 1.x API.
			OutError = FModDiagnostic::Error(
				TEXT("API.VersionMismatch"),
				FString::Printf(TEXT("'%s' is not a valid version range (%s), so the request for the mod API '%s' was refused."),
					*VersionRange,
					ParseError.IsEmpty() ? TEXT("unparsable expression") : *ParseError,
					*ApiId.ToString()),
				ApiId.ToString());
			OutError.ModId = RequestingMod;
			return nullptr;
		}
	}

	UModAPI* Api = RequestAPI(ApiId, Range, RequestingMod, OutError);

	if (Api == nullptr)
	{
		return nullptr;
	}

	// DeterminesOutputType makes Blueprint treat the result as an ApiClass, so the cast has to be
	// real. Anything else would hand a Blueprint a wrongly typed object it cannot detect.
	if (const UClass* RequiredClass = ApiClass.Get())
	{
		if (!Api->IsA(RequiredClass))
		{
			OutError = FModDiagnostic::Error(
				TEXT("API.ClassMismatch"),
				FString::Printf(TEXT("Mod API '%s' is a '%s', which is not a '%s'."),
					*ApiId.ToString(),
					*Api->GetClass()->GetPathName(),
					*RequiredClass->GetPathName()),
				ApiId.ToString());
			OutError.ModId = RequestingMod;

			UE_LOG(LogModFramework, Warning, TEXT("%s"), *OutError.Message);
			return nullptr;
		}
	}

	return Api;
}

void UModAPIRegistry::SetPermissionCheck(FModPermissionCheck InCheck)
{
	PermissionCheck = MoveTemp(InCheck);
}

bool UModAPIRegistry::CheckPermissions(const TArray<FName>& RequiredPermissions, const FModId& RequestingMod,
	FName& OutDeniedPermission, bool& bOutNoCheckInstalled) const
{
	OutDeniedPermission = NAME_None;
	bOutNoCheckInstalled = false;

	// Collect the meaningful entries first so an all-blank list still counts as "ungated".
	TArray<FName> Gates;
	Gates.Reserve(RequiredPermissions.Num());

	for (const FName Permission : RequiredPermissions)
	{
		if (!Permission.IsNone())
		{
			Gates.Add(Permission);
		}
	}

	if (Gates.Num() == 0)
	{
		return true;
	}

	if (!PermissionCheck.IsBound())
	{
		// Fail closed. An API that guards itself with permissions must never be handed out merely
		// because nothing has wired the permission registry up yet.
		OutDeniedPermission = Gates[0];
		bOutNoCheckInstalled = true;
		return false;
	}

	for (const FName Permission : Gates)
	{
		if (!PermissionCheck.Execute(RequestingMod, Permission))
		{
			OutDeniedPermission = Permission;
			return false;
		}
	}

	return true;
}
