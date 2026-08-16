// Copyright (c) 2026. Licensed for use in your own projects.

#include "API/ModAPI.h"

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "CoreTypes.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModVersion.h"
#include "Misc/CString.h"
#include "Misc/Char.h"
#include "Misc/CoreMiscDefines.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"

namespace
{
	/** Class metadata keys from the reflection metadata contract. See section 25 of the framework contract. */
	const TCHAR* const ModApiIdMetaKey = TEXT("ModApiId");
	const TCHAR* const ModApiVersionMetaKey = TEXT("ModApiVersion");
	const TCHAR* const ModApiPermissionsMetaKey = TEXT("ModApiPermissions");
	const TCHAR* const ModApiServerAuthoritativeMetaKey = TEXT("ModApiServerAuthoritative");

	/**
	 * Looks a class metadata key up along the superclass chain.
	 *
	 * Walking the chain is what lets a Blueprint subclass - which can never carry UCLASS metadata of
	 * its own - inherit the identity declared by its native parent. The flip side is that a native
	 * subclass which needs a *different* identity has to declare its own ModApiId (or override
	 * NativeGetApiId), otherwise it inherits the parent's and collides with it on registration.
	 *
	 * Always returns nullptr when WITH_METADATA is off, which is the case in cooked Shipping and
	 * Test builds: UField::FindMetaData does not even exist there.
	 */
	const FString* FindInheritedClassMetaData(const UClass* InClass, const TCHAR* Key)
	{
#if WITH_METADATA
		for (const UClass* Cursor = InClass; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
		{
			if (const FString* Found = Cursor->FindMetaData(Key))
			{
				return Found;
			}
		}
#endif // WITH_METADATA

		return nullptr;
	}

	/** Trims, rejects blank ids and lowercases. FName comparison is case insensitive, but the id is also displayed. */
	FName NormalizeApiId(FName In)
	{
		if (In.IsNone())
		{
			return NAME_None;
		}

		FString AsString = In.ToString();
		AsString.TrimStartAndEndInline();

		if (AsString.IsEmpty())
		{
			return NAME_None;
		}

		AsString.ToLowerInline();
		return FName(*AsString);
	}

	/**
	 * Interprets a boolean class metadata value.
	 *
	 * UnrealHeaderTool stores a valueless specifier - meta = (ModApiServerAuthoritative) - with an
	 * EMPTY value, exactly the way the ModPublic flag in the same metadata table is written, so the
	 * mere presence of the key counts as true. "true", "yes" and "1" are accepted for the explicit
	 * spelling; anything else is false.
	 */
	bool ParseMetadataFlag(const FString& In)
	{
		const FString Trimmed = In.TrimStartAndEnd();

		if (Trimmed.IsEmpty())
		{
			return true;
		}

		return Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("1"), ESearchCase::CaseSensitive);
	}

	/** Splits the comma separated ModApiPermissions value, trimming each entry and dropping blanks. */
	void ParsePermissionList(const FString& In, TArray<FName>& Out)
	{
		TArray<FString> Tokens;
		In.ParseIntoArray(Tokens, TEXT(","), true);

		for (FString& Token : Tokens)
		{
			Token.TrimStartAndEndInline();

			if (!Token.IsEmpty())
			{
				Out.AddUnique(FName(*Token));
			}
		}
	}
}

FName UModAPI::GetApiId() const
{
	ResolveIdentity();
	return CachedApiId;
}

FModVersion UModAPI::GetApiVersion() const
{
	ResolveIdentity();
	return CachedApiVersion;
}

FString UModAPI::GetApiDescription() const
{
	ResolveIdentity();
	return CachedApiDescription;
}

bool UModAPI::IsServerAuthoritative() const
{
	ResolveIdentity();
	return bCachedServerAuthoritative;
}

TArray<FName> UModAPI::GetRequiredPermissions() const
{
	ResolveIdentity();
	return CachedRequiredPermissions;
}

FName UModAPI::NativeGetApiId() const
{
	return NAME_None;
}

FModVersion UModAPI::NativeGetApiVersion() const
{
	return FModVersion();
}

FString UModAPI::NativeGetApiDescription() const
{
	return FString();
}

bool UModAPI::NativeGetServerAuthoritative(bool& bOutServerAuthoritative) const
{
	bOutServerAuthoritative = false;
	return false;
}

TArray<FName> UModAPI::NativeGetRequiredPermissions() const
{
	return TArray<FName>();
}

void UModAPI::InvalidateResolvedIdentity()
{
	bIdentityResolved = false;
	CachedApiId = NAME_None;
	CachedApiVersion = FModVersion();
	CachedApiDescription.Reset();
	bCachedServerAuthoritative = false;
	CachedRequiredPermissions.Reset();
}

void UModAPI::ResolveIdentity() const
{
	if (bIdentityResolved)
	{
		return;
	}

	// Marked resolved up front: the hooks below are virtual and a subclass could reach back into one
	// of the getters, which would otherwise recurse into this function forever.
	bIdentityResolved = true;

	const UClass* Class = GetClass();

	// --- Id ------------------------------------------------------------------------------------
	FName ResolvedId = NormalizeApiId(NativeGetApiId());

	if (ResolvedId.IsNone())
	{
		ResolvedId = NormalizeApiId(ApiIdOverride);
	}

	if (ResolvedId.IsNone())
	{
		if (const FString* MetaValue = FindInheritedClassMetaData(Class, ModApiIdMetaKey))
		{
			FString Trimmed = MetaValue->TrimStartAndEnd();

			if (!Trimmed.IsEmpty())
			{
				Trimmed.ToLowerInline();
				ResolvedId = FName(*Trimmed);
			}
		}
	}

	if (ResolvedId.IsNone())
	{
		ResolvedId = MakeFallbackApiId(Class);
	}

	CachedApiId = ResolvedId;

	// --- Version -------------------------------------------------------------------------------
	FModVersion ResolvedVersion = NativeGetApiVersion();

	if (ResolvedVersion.IsZero())
	{
		ResolvedVersion = ApiVersionOverride;
	}

	if (ResolvedVersion.IsZero())
	{
		if (const FString* MetaValue = FindInheritedClassMetaData(Class, ModApiVersionMetaKey))
		{
			FModVersion Parsed;
			FString ParseError;

			if (FModVersion::Parse(*MetaValue, Parsed, &ParseError) && !Parsed.IsZero())
			{
				ResolvedVersion = Parsed;
			}
			else if (!MetaValue->TrimStartAndEnd().IsEmpty())
			{
				const FString ClassPath = Class != nullptr ? Class->GetPathName() : FString(TEXT("<null class>"));
				const FString Reason = ParseError.IsEmpty() ? FString(TEXT("the version is 0.0.0")) : ParseError;

				UE_LOG(LogModFramework, Warning,
					TEXT("Mod API '%s' (%s) declares ModApiVersion=\"%s\", which is not a usable semantic version (%s). Falling back to 1.0.0."),
					*CachedApiId.ToString(),
					*ClassPath,
					**MetaValue,
					*Reason);
			}
		}
	}

	if (ResolvedVersion.IsZero())
	{
		ResolvedVersion = FModVersion(1, 0, 0);
	}

	CachedApiVersion = ResolvedVersion;

	// --- Description ---------------------------------------------------------------------------
	FString ResolvedDescription = NativeGetApiDescription();

	if (ResolvedDescription.IsEmpty())
	{
		ResolvedDescription = ApiDescriptionOverride;
	}

	CachedApiDescription = MoveTemp(ResolvedDescription);

	// --- Server authority ----------------------------------------------------------------------
	bool bResolvedAuthority = false;
	bool bAuthorityDecided = NativeGetServerAuthoritative(bResolvedAuthority);

	if (!bAuthorityDecided && bOverrideServerAuthoritative)
	{
		bResolvedAuthority = bServerAuthoritativeOverride;
		bAuthorityDecided = true;
	}

	if (!bAuthorityDecided)
	{
		if (const FString* MetaValue = FindInheritedClassMetaData(Class, ModApiServerAuthoritativeMetaKey))
		{
			bResolvedAuthority = ParseMetadataFlag(*MetaValue);
			bAuthorityDecided = true;
		}
	}

	bCachedServerAuthoritative = bAuthorityDecided && bResolvedAuthority;

	// --- Required permissions ------------------------------------------------------------------
	TArray<FName> ResolvedPermissions = NativeGetRequiredPermissions();

	if (ResolvedPermissions.Num() == 0)
	{
		ResolvedPermissions = RequiredPermissionsOverride;
	}

	if (ResolvedPermissions.Num() == 0)
	{
		if (const FString* MetaValue = FindInheritedClassMetaData(Class, ModApiPermissionsMetaKey))
		{
			ParsePermissionList(*MetaValue, ResolvedPermissions);
		}
	}

	CachedRequiredPermissions.Reset();

	for (const FName Permission : ResolvedPermissions)
	{
		if (!Permission.IsNone())
		{
			CachedRequiredPermissions.AddUnique(Permission);
		}
	}
}

FName UModAPI::MakeFallbackApiId(const UClass* InClass)
{
	if (InClass == nullptr)
	{
		return NAME_None;
	}

	const FString ClassName = InClass->GetName();
	FString Working = ClassName;

	// Blueprint generated classes are named "<AssetName>_C".
	Working.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);

	// Reflection names normally have the C++ type prefix stripped already ("UGameplayAPI" is called
	// "GameplayAPI" here), so a leading 'U' is only removed when the remainder still looks like a
	// prefixed PascalCase identifier: upper case letter followed by a lower case one. That guard is
	// what keeps "UIWidgetAPI" (U, I, W) and "UtilityAPI" (U, t) intact.
	if (Working.Len() >= 3 && Working[0] == TEXT('U') && FChar::IsUpper(Working[1]) && FChar::IsLower(Working[2]))
	{
		Working.RightChopInline(1);
	}

	// A trailing "API" carries no information in an id, but only drop it if a name survives.
	if (Working.Len() > 3 && Working.EndsWith(TEXT("API"), ESearchCase::CaseSensitive))
	{
		Working.LeftChopInline(3);
	}

	Working.TrimStartAndEndInline();

	if (Working.IsEmpty())
	{
		// Every strip fired on a pathological name; fall back to the untouched class name so the id
		// stays unique rather than becoming NAME_None.
		Working = ClassName;
	}

	if (Working.IsEmpty())
	{
		return NAME_None;
	}

	Working.ToLowerInline();
	return FName(*Working);
}

void UModAPI::OnAPIRegistered(UModAPIRegistry* InRegistry)
{
	// Pin the identity now: whatever the registry validated at registration time is what every later
	// query has to keep returning, even if an override property is touched afterwards.
	ResolveIdentity();

	UE_LOG(LogModFramework, Verbose, TEXT("Mod API '%s' registered (version %s)."),
		*CachedApiId.ToString(), *CachedApiVersion.ToString());

	ReceiveOnAPIRegistered();
}

void UModAPI::OnAPIUnregistered()
{
	UE_LOG(LogModFramework, Verbose, TEXT("Mod API '%s' unregistered."), *GetApiId().ToString());
}

FModAPIDescriptor UModAPI::BuildDescriptor() const
{
	FModAPIDescriptor Descriptor;
	Descriptor.ApiId = GetApiId();
	Descriptor.Version = GetApiVersion();
	Descriptor.ClassPath = GetClass() != nullptr ? GetClass()->GetPathName() : FString();
	Descriptor.Description = GetApiDescription();
	Descriptor.bServerAuthoritative = IsServerAuthoritative();
	Descriptor.RequiredPermissions = GetRequiredPermissions();

	// ProviderModId stays invalid: only UModAPIRegistry knows which mod, if any, supplied the API.
	return Descriptor;
}

bool UModAPI::VerifyServerAuthority(const UObject* WorldContext, const TCHAR* CallingFunction) const
{
	const UWorld* World = nullptr;

	if (WorldContext != nullptr && GEngine != nullptr)
	{
		World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	}

	if (World == nullptr)
	{
		World = GetWorld();
	}

	// No world to reason about: the editor, a commandlet, an automation test or a cooker. There is
	// no client to refuse, and blocking here would make the API unusable for tooling.
	if (World == nullptr)
	{
		return true;
	}

	// Everything below NM_Client - standalone, listen server, dedicated server - has authority.
	if (World->GetNetMode() != NM_Client)
	{
		return true;
	}

	UE_LOG(LogModFramework, Warning,
		TEXT("%s was called on a network client, but the mod API '%s' is server authoritative. The call was refused; route it through a server RPC instead."),
		CallingFunction != nullptr ? CallingFunction : TEXT("An unnamed function"),
		*GetApiId().ToString());

	return false;
}
