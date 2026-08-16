// Copyright (c) 2026. Licensed for use in your own projects.

#include "Runtime/ModContext.h"

#include "API/ModAPIRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Config/ModConfigManager.h"
#include "Containers/Array.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Content/ModContentManager.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "Engine/GameInstance.h"
#include "Events/ModEventBus.h"
#include "Events/ModEventTypes.h"
#include "Extensions/ModExtension.h"
#include "Extensions/ModExtensionRegistry.h"
#include "GameFramework/Actor.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModManifest.h"
#include "Misc/CString.h"
#include "Permissions/ModPermissionRegistry.h"
#include "Registry/ModInfo.h"
#include "Registry/ModRegistry.h"
#include "Save/ModSaveDataManager.h"
#include "Subsystem/ModSubsystem.h"
#include "Templates/SubclassOf.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace ModContextPrivate
{
	/**
	 * Diagnostic codes. Declared once so a typo cannot drift between call sites, and so that a game
	 * can switch on them instead of matching message text.
	 */
	static const FName CodeContextExpired(TEXT("Context.Expired"));
	static const FName CodeRegistryUnavailable(TEXT("Context.RegistryUnavailable"));
	static const FName CodeInvalidClass(TEXT("Context.InvalidClass"));
	static const FName CodeCreationFailed(TEXT("Context.CreationFailed"));
	static const FName CodeNotOwned(TEXT("Context.NotOwned"));

	/** "the unidentified mod" is never a real id, so it can never be confused with one. */
	FString DescribeModId(const FModId& In)
	{
		return In.IsValid() ? In.ToString() : FString(TEXT("<unidentified mod>"));
	}

	/**
	 * An error diagnostic already stamped with the mod it concerns.
	 *
	 * Deliberately NOT called MakeError: Templates/ValueOrError.h declares a global variadic
	 * MakeError(ArgTypes&&...) that is an exact match for any argument list, so a file-local helper of
	 * that name silently loses the overload and produces an unreadable conversion error.
	 */
	FModDiagnostic MakeContextError(const FModId& ModId, FName Code, FString Message, FString ContextText = FString())
	{
		FModDiagnostic Diagnostic = FModDiagnostic::Error(Code, MoveTemp(Message), MoveTemp(ContextText));
		Diagnostic.ModId = ModId;
		return Diagnostic;
	}

	/** Class name of a UClass, or a stable placeholder. Used in messages built from mod input. */
	FString DescribeClass(const UClass* InClass)
	{
		return InClass ? InClass->GetName() : FString(TEXT("<null class>"));
	}
}

void UModContext::InitializeContext(UModSubsystem* InSubsystem, const FModId& InModId)
{
	Subsystem = InSubsystem;
	ModId = InModId;

	// Handles issued under a previous binding named subscriptions on a bus this context is no longer
	// attached to. Keeping them would let a re-pointed context cancel handles it never owned.
	IssuedEventHandles.Reset();

	if (!InSubsystem)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::InitializeContext: the context of mod '%s' was created without a subsystem; every framework call it makes will be refused."),
			*ModContextPrivate::DescribeModId(InModId));
	}

	if (!InModId.IsValid())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::InitializeContext: a context was created with no mod id. It cannot be attributed, and every permission check it runs will fail."));
	}
}

FModId UModContext::GetModId() const
{
	return ModId;
}

bool UModContext::GetManifest(FModManifest& OutManifest) const
{
	FModInfo Info;
	if (!GetModInfo(Info))
	{
		return false;
	}

	OutManifest = MoveTemp(Info.Manifest);
	return true;
}

bool UModContext::GetModInfo(FModInfo& OutInfo) const
{
	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("GetModInfo"));
	if (!ResolvedSubsystem)
	{
		return false;
	}

	UModRegistry* Registry = ResolvedSubsystem->GetRegistry();
	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::GetModInfo: mod '%s' has no reachable mod registry."), *ModContextPrivate::DescribeModId(ModId));
		return false;
	}

	return Registry->GetModInfo(ModId, OutInfo);
}

FString UModContext::GetModRootPath() const
{
	FModInfo Info;
	if (!GetModInfo(Info))
	{
		return FString();
	}

	return Info.RootPath;
}

UModAPI* UModContext::RequestAPI(FName ApiId, TSubclassOf<UModAPI> ApiClass, const FString& VersionRange,
	FModDiagnostic& OutError)
{
	using namespace ModContextPrivate;

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("RequestAPI"));
	if (!ResolvedSubsystem)
	{
		OutError = MakeContextError(ModId, CodeContextExpired,
			FString::Printf(TEXT("Mod '%s' asked for API '%s' through a context whose mod subsystem no longer exists."),
				*DescribeModId(ModId), *ApiId.ToString()),
			ApiId.ToString());
		return nullptr;
	}

	UModAPIRegistry* APIRegistry = ResolvedSubsystem->GetAPIRegistry();
	if (!APIRegistry)
	{
		OutError = MakeContextError(ModId, CodeRegistryUnavailable,
			FString::Printf(TEXT("Mod '%s' asked for API '%s', but the API registry is not available."),
				*DescribeModId(ModId), *ApiId.ToString()),
			ApiId.ToString());
		return nullptr;
	}

	// The requesting mod id comes from the context, never from the caller: this is what stops a mod
	// borrowing another mod's permissions to reach a gated API.
	return APIRegistry->K2_RequestAPI(ApiId, ApiClass, VersionRange, ModId, OutError);
}

UModExtension* UModContext::RegisterExtension(TSubclassOf<UModExtension> ExtensionClass, FModDiagnostic& OutError)
{
	using namespace ModContextPrivate;

	UClass* ResolvedClass = ExtensionClass.Get();
	if (!ResolvedClass)
	{
		OutError = MakeContextError(ModId, CodeInvalidClass,
			FString::Printf(TEXT("Mod '%s' called RegisterExtension without an extension class."), *DescribeModId(ModId)));
		return nullptr;
	}

	if (!ResolvedClass->IsChildOf(UModExtension::StaticClass()))
	{
		OutError = MakeContextError(ModId, CodeInvalidClass,
			FString::Printf(TEXT("Mod '%s' tried to register '%s', which does not derive from UModExtension."),
				*DescribeModId(ModId), *DescribeClass(ResolvedClass)),
			DescribeClass(ResolvedClass));
		return nullptr;
	}

	// An abstract or reinstanced-away class cannot be instantiated; NewObject would assert on it, and
	// mod-authored class references are exactly where a stale one turns up.
	if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists | CLASS_Deprecated))
	{
		OutError = MakeContextError(ModId, CodeInvalidClass,
			FString::Printf(TEXT("Mod '%s' tried to register extension class '%s', which is abstract, deprecated or superseded."),
				*DescribeModId(ModId), *DescribeClass(ResolvedClass)),
			DescribeClass(ResolvedClass));
		return nullptr;
	}

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("RegisterExtension"));
	if (!ResolvedSubsystem)
	{
		OutError = MakeContextError(ModId, CodeContextExpired,
			FString::Printf(TEXT("Mod '%s' tried to register extension class '%s' through a context whose mod subsystem no longer exists."),
				*DescribeModId(ModId), *DescribeClass(ResolvedClass)),
			DescribeClass(ResolvedClass));
		return nullptr;
	}

	UModExtensionRegistry* ExtensionRegistry = ResolvedSubsystem->GetExtensionRegistry();
	UModRegistry* Registry = ResolvedSubsystem->GetRegistry();
	if (!ExtensionRegistry || !Registry)
	{
		OutError = MakeContextError(ModId, CodeRegistryUnavailable,
			FString::Printf(TEXT("Mod '%s' tried to register extension class '%s', but the framework registries are not available."),
				*DescribeModId(ModId), *DescribeClass(ResolvedClass)),
			DescribeClass(ResolvedClass));
		return nullptr;
	}

	// Outered to the context, whose own outer chain reaches the game instance. That is what gives a
	// Blueprint extension a world, and what makes the object's ownership obvious in any object dump.
	UModExtension* Extension = NewObject<UModExtension>(this, ResolvedClass);
	if (!Extension)
	{
		OutError = MakeContextError(ModId, CodeCreationFailed,
			FString::Printf(TEXT("Mod '%s' could not instantiate extension class '%s'."),
				*DescribeModId(ModId), *DescribeClass(ResolvedClass)),
			DescribeClass(ResolvedClass));
		return nullptr;
	}

	// Track before registering. A bare UObject* on the stack is not a garbage collection root, so the
	// object needs a strong reference from the moment it exists, not from the moment it is accepted.
	Registry->TrackModObject(ModId, Extension);

	// OwningModId is stamped by the registry from the id this context carries: a mod cannot register
	// an extension in another mod's name.
	if (!ExtensionRegistry->RegisterExtension(Extension, ModId, OutError))
	{
		Registry->UntrackModObject(ModId, Extension);
		return nullptr;
	}

	return Extension;
}

bool UModContext::RegisterExtensionInstance(UModExtension* Extension, FModDiagnostic& OutError)
{
	using namespace ModContextPrivate;

	if (!::IsValid(Extension))
	{
		OutError = MakeContextError(ModId, CodeInvalidClass,
			FString::Printf(TEXT("Mod '%s' called RegisterExtensionInstance with a null or garbage extension."),
				*DescribeModId(ModId)));
		return false;
	}

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("RegisterExtensionInstance"));
	if (!ResolvedSubsystem)
	{
		OutError = MakeContextError(ModId, CodeContextExpired,
			FString::Printf(TEXT("Mod '%s' tried to register an extension through a context whose mod subsystem no longer exists."),
				*DescribeModId(ModId)));
		return false;
	}

	UModExtensionRegistry* ExtensionRegistry = ResolvedSubsystem->GetExtensionRegistry();
	UModRegistry* Registry = ResolvedSubsystem->GetRegistry();
	if (!ExtensionRegistry || !Registry)
	{
		OutError = MakeContextError(ModId, CodeRegistryUnavailable,
			FString::Printf(TEXT("Mod '%s' tried to register an extension, but the framework registries are not available."),
				*DescribeModId(ModId)));
		return false;
	}

	// Registering an object another mod already owns would make that mod's unload destroy an object
	// this mod is using - or worse, let this mod smuggle its own object into that mod's lifetime.
	const FModId ExistingOwner = Registry->FindOwningMod(Extension);
	if (ExistingOwner.IsValid() && ExistingOwner != ModId)
	{
		OutError = MakeContextError(ModId, CodeNotOwned,
			FString::Printf(TEXT("Mod '%s' tried to register extension '%s', which is already owned by mod '%s'."),
				*DescribeModId(ModId), *DescribeClass(Extension->GetClass()), *ExistingOwner.ToString()),
			ExistingOwner.ToString());
		return false;
	}

	if (!ExtensionRegistry->RegisterExtension(Extension, ModId, OutError))
	{
		return false;
	}

	// Only after acceptance: an object the registry refused is the mod's own business, and tracking it
	// would tie an unrelated object's lifetime to this mod.
	Registry->TrackModObject(ModId, Extension);
	return true;
}

bool UModContext::UnregisterExtension(FName ExtensionPointId, FName ExtensionId)
{
	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("UnregisterExtension"));
	if (!ResolvedSubsystem)
	{
		return false;
	}

	UModExtensionRegistry* ExtensionRegistry = ResolvedSubsystem->GetExtensionRegistry();
	if (!ExtensionRegistry)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::UnregisterExtension: mod '%s' has no reachable extension registry."),
			*ModContextPrivate::DescribeModId(ModId));
		return false;
	}

	UModExtension* Existing = ExtensionRegistry->FindExtension(ExtensionPointId, ExtensionId);
	if (!Existing)
	{
		UE_LOG(LogModFramework, Verbose,
			TEXT("UModContext::UnregisterExtension: mod '%s' asked to remove extension '%s' of point '%s', which is not registered."),
			*ModContextPrivate::DescribeModId(ModId), *ExtensionId.ToString(), *ExtensionPointId.ToString());
		return false;
	}

	// Extension ids are readable from GetExtensions, so without this check any mod could unregister
	// any other mod's contribution simply by naming it.
	if (Existing->OwningModId != ModId)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::UnregisterExtension: mod '%s' tried to remove extension '%s' of point '%s', which belongs to mod '%s'. Refused."),
			*ModContextPrivate::DescribeModId(ModId), *ExtensionId.ToString(), *ExtensionPointId.ToString(),
			*ModContextPrivate::DescribeModId(Existing->OwningModId));
		return false;
	}

	if (!ExtensionRegistry->UnregisterExtension(ExtensionPointId, ExtensionId))
	{
		return false;
	}

	// The registry has dropped its reference, so the tracking entry is the last thing keeping the
	// object alive. Release it too, otherwise unregistering never actually frees anything.
	if (UModRegistry* Registry = ResolvedSubsystem->GetRegistry())
	{
		Registry->UntrackModObject(ModId, Existing);
	}

	return true;
}

FModEventHandle UModContext::SubscribeToEvent(FName EventId, FModEventDynamicDelegate Delegate, int32 Priority)
{
	if (EventId.IsNone())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::SubscribeToEvent: mod '%s' subscribed to an empty event id."),
			*ModContextPrivate::DescribeModId(ModId));
		return FModEventHandle();
	}

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("SubscribeToEvent"));
	if (!ResolvedSubsystem)
	{
		return FModEventHandle();
	}

	UModEventBus* EventBus = ResolvedSubsystem->GetEventBus();
	if (!EventBus)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::SubscribeToEvent: mod '%s' has no reachable event bus."),
			*ModContextPrivate::DescribeModId(ModId));
		return FModEventHandle();
	}

	// A runaway guard, not a budget: an unbounded subscriber list is a denial of service against every
	// other mod's broadcasts as much as against the host game.
	if (IssuedEventHandles.Num() >= MaxEventSubscriptions)
	{
		UE_LOG(LogModFramework, Error,
			TEXT("UModContext::SubscribeToEvent: mod '%s' already holds %d event subscriptions, which is the per-mod limit. The subscription to '%s' was refused."),
			*ModContextPrivate::DescribeModId(ModId), IssuedEventHandles.Num(), *EventId.ToString());
		return FModEventHandle();
	}

	// The subscriber id is the context's own: this is what lets the subsystem cancel every one of a
	// mod's handlers when it unloads, without the mod being able to hide them under another id.
	const FModEventHandle Handle = EventBus->K2_Subscribe(EventId, ModId, Delegate, Priority);
	if (Handle.IsValid())
	{
		IssuedEventHandles.Add(Handle.Id);
	}

	return Handle;
}

bool UModContext::UnsubscribeFromEvent(FModEventHandle Handle)
{
	if (!Handle.IsValid())
	{
		return false;
	}

	// Handles are sequential integers, so anything else would let a mod silence its neighbours'
	// handlers by counting upwards.
	if (!IssuedEventHandles.Contains(Handle.Id))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::UnsubscribeFromEvent: mod '%s' tried to cancel event subscription %lld, which this context did not issue. Refused."),
			*ModContextPrivate::DescribeModId(ModId), Handle.Id);
		return false;
	}

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("UnsubscribeFromEvent"));
	if (!ResolvedSubsystem)
	{
		return false;
	}

	UModEventBus* EventBus = ResolvedSubsystem->GetEventBus();
	if (!EventBus)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::UnsubscribeFromEvent: mod '%s' has no reachable event bus."),
			*ModContextPrivate::DescribeModId(ModId));
		return false;
	}

	const bool bCancelled = EventBus->Unsubscribe(Handle);

	// Forget it either way. A handle the bus no longer knows was already cancelled - most likely by
	// UnsubscribeAllForMod - and handle ids are never reused, so this can never free a live one.
	IssuedEventHandles.Remove(Handle.Id);

	return bCancelled;
}

bool UModContext::BroadcastEvent(FName EventId, const TInstancedStruct<FModEventPayload>& Payload)
{
	if (EventId.IsNone())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::BroadcastEvent: mod '%s' tried to raise an event with an empty id."),
			*ModContextPrivate::DescribeModId(ModId));
		return false;
	}

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("BroadcastEvent"));
	if (!ResolvedSubsystem)
	{
		return false;
	}

	UModEventBus* EventBus = ResolvedSubsystem->GetEventBus();
	if (!EventBus)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::BroadcastEvent: mod '%s' has no reachable event bus."),
			*ModContextPrivate::DescribeModId(ModId));
		return false;
	}

	// The bus is the authority on whether this broadcast is allowed and enforces the same rule itself.
	// Pre-flighting it here is purely so the mod gets a truthful bool back instead of a silent no-op:
	// UModEventBus::Broadcast returns void and reports refusals only to the log.
	FModEventDescriptor Descriptor;
	if (EventBus->GetEventType(EventId, Descriptor))
	{
		for (const FName& Required : Descriptor.RequiredPermissions)
		{
			if (Required.IsNone())
			{
				continue;
			}

			if (!HasPermission(Required))
			{
				UE_LOG(LogModFramework, Warning,
					TEXT("UModContext::BroadcastEvent: mod '%s' may not raise '%s' without the '%s' permission."),
					*ModContextPrivate::DescribeModId(ModId), *EventId.ToString(), *Required.ToString());
				return false;
			}
		}
	}

	FModEventContext EventContext;
	EventContext.EventId = EventId;

	// Stamped, never taken from the caller: an event always names the mod that actually raised it.
	EventContext.SourceModId = ModId;
	EventContext.Payload = Payload;
	EventContext.WorldContext = this;

	EventBus->Broadcast(EventContext);
	return true;
}

bool UModContext::HasPermission(FName PermissionId) const
{
	if (PermissionId.IsNone())
	{
		// An empty id names no capability, so there is nothing that could have been granted.
		return false;
	}

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("HasPermission"));
	if (!ResolvedSubsystem)
	{
		return false;
	}

	UModPermissionRegistry* PermissionRegistry = ResolvedSubsystem->GetPermissionRegistry();
	if (!PermissionRegistry)
	{
		// Fail closed. "Cannot verify" is not "granted".
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::HasPermission: mod '%s' has no reachable permission registry; '%s' is treated as denied."),
			*ModContextPrivate::DescribeModId(ModId), *PermissionId.ToString());
		return false;
	}

	return PermissionRegistry->HasPermission(ModId, PermissionId);
}

TArray<FName> UModContext::GetGrantedPermissions() const
{
	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("GetGrantedPermissions"));
	if (!ResolvedSubsystem)
	{
		return TArray<FName>();
	}

	UModPermissionRegistry* PermissionRegistry = ResolvedSubsystem->GetPermissionRegistry();
	if (!PermissionRegistry)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::GetGrantedPermissions: mod '%s' has no reachable permission registry."),
			*ModContextPrivate::DescribeModId(ModId));
		return TArray<FName>();
	}

	return PermissionRegistry->GetGrantedPermissions(ModId);
}

bool UModContext::SaveJson(const FString& Json, int32 DataVersion)
{
	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("SaveJson"));
	if (!ResolvedSubsystem)
	{
		return false;
	}

	UModSaveDataManager* SaveDataManager = ResolvedSubsystem->GetSaveDataManager();
	if (!SaveDataManager)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::SaveJson: mod '%s' has no reachable save data manager."),
			*ModContextPrivate::DescribeModId(ModId));
		return false;
	}

	// The manager enforces the "save.modify" permission, the payload size cap and the data version
	// floor, and the record is keyed by this context's mod id, so a mod can only ever write its own.
	return SaveDataManager->WriteModJson(ModId, Json, DataVersion);
}

// Every config call resolves the manager the same way and fails closed. The macro keeps that
// uniform: a variant that silently skipped the null check would be a crash on a torn-down context.
#define MODCONTEXT_RESOLVE_CONFIG(FunctionName, FailureResult)                        \
	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT(FunctionName));          \
	if (!ResolvedSubsystem) { return FailureResult; }                                 \
	UModConfigManager* Config = ResolvedSubsystem->GetConfigManager();                \
	if (!Config) { return FailureResult; }

bool UModContext::GetConfigBool(FName Key, bool DefaultValue) const
{
	MODCONTEXT_RESOLVE_CONFIG("GetConfigBool", DefaultValue)
	return Config->GetBool(ModId, Key, DefaultValue);
}

int32 UModContext::GetConfigInt(FName Key, int32 DefaultValue) const
{
	MODCONTEXT_RESOLVE_CONFIG("GetConfigInt", DefaultValue)
	return Config->GetInt(ModId, Key, DefaultValue);
}

float UModContext::GetConfigFloat(FName Key, float DefaultValue) const
{
	MODCONTEXT_RESOLVE_CONFIG("GetConfigFloat", DefaultValue)
	return Config->GetFloat(ModId, Key, DefaultValue);
}

FString UModContext::GetConfigString(FName Key, const FString& DefaultValue) const
{
	MODCONTEXT_RESOLVE_CONFIG("GetConfigString", DefaultValue)
	return Config->GetString(ModId, Key, DefaultValue);
}

void UModContext::SetConfigBool(FName Key, bool Value)
{
	MODCONTEXT_RESOLVE_CONFIG("SetConfigBool", )
	Config->SetBool(ModId, Key, Value);
}

void UModContext::SetConfigInt(FName Key, int32 Value)
{
	MODCONTEXT_RESOLVE_CONFIG("SetConfigInt", )
	Config->SetInt(ModId, Key, Value);
}

void UModContext::SetConfigFloat(FName Key, float Value)
{
	MODCONTEXT_RESOLVE_CONFIG("SetConfigFloat", )
	Config->SetFloat(ModId, Key, Value);
}

void UModContext::SetConfigString(FName Key, const FString& Value)
{
	MODCONTEXT_RESOLVE_CONFIG("SetConfigString", )
	Config->SetString(ModId, Key, Value);
}

bool UModContext::SaveConfig()
{
	MODCONTEXT_RESOLVE_CONFIG("SaveConfig", false)
	return Config->SaveConfig(ModId);
}

TArray<FName> UModContext::GetConfigKeys() const
{
	MODCONTEXT_RESOLVE_CONFIG("GetConfigKeys", TArray<FName>())
	return Config->GetKeys(ModId);
}

#undef MODCONTEXT_RESOLVE_CONFIG

bool UModContext::LoadJson(FString& OutJson, int32& OutDataVersion) const
{
	OutJson.Reset();
	OutDataVersion = 0;

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("LoadJson"));
	if (!ResolvedSubsystem)
	{
		return false;
	}

	UModSaveDataManager* SaveDataManager = ResolvedSubsystem->GetSaveDataManager();
	if (!SaveDataManager)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::LoadJson: mod '%s' has no reachable save data manager."),
			*ModContextPrivate::DescribeModId(ModId));
		return false;
	}

	return SaveDataManager->ReadModJson(ModId, OutJson, OutDataVersion);
}

TArray<FAssetData> UModContext::GetOwnAssets(UClass* ClassFilter) const
{
	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("GetOwnAssets"));
	if (!ResolvedSubsystem)
	{
		return TArray<FAssetData>();
	}

	// Scoped to this mod's mounts by construction: a mod cannot enumerate another mod's content here.
	return ResolvedSubsystem->GetContentManager().GetModAssets(ModId, ClassFilter, /*bRecursiveClasses*/ true);
}

UObject* UModContext::LoadOwnAsset(FName AssetName, UClass* AssetClass) const
{
	if (AssetName.IsNone())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::LoadOwnAsset: mod '%s' asked for an asset with no name."),
			*ModContextPrivate::DescribeModId(ModId));
		return nullptr;
	}

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("LoadOwnAsset"));
	if (!ResolvedSubsystem)
	{
		return nullptr;
	}

	const FModContentManager& ContentManager = ResolvedSubsystem->GetContentManager();
	const TArray<FAssetData> Assets = ContentManager.GetModAssets(ModId, AssetClass, /*bRecursiveClasses*/ true);

	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName != AssetName)
		{
			continue;
		}

		UObject* Loaded = Asset.GetAsset();
		if (!Loaded)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("UModContext::LoadOwnAsset: '%s' of mod '%s' could not be loaded."),
				*Asset.GetSoftObjectPath().ToString(), *ModContextPrivate::DescribeModId(ModId));
			return nullptr;
		}

		// The asset registry records a Blueprint asset as a UBlueprint, not as the class it generates,
		// so the filter above is necessary but not sufficient. Re-check what actually came back.
		if (AssetClass && !Loaded->IsA(AssetClass))
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("UModContext::LoadOwnAsset: '%s' of mod '%s' is a %s, not a %s."),
				*Asset.GetSoftObjectPath().ToString(), *ModContextPrivate::DescribeModId(ModId),
				*ModContextPrivate::DescribeClass(Loaded->GetClass()), *ModContextPrivate::DescribeClass(AssetClass));
			return nullptr;
		}

		return Loaded;
	}

	UE_LOG(LogModFramework, Warning,
		TEXT("UModContext::LoadOwnAsset: mod '%s' has no asset named '%s'."),
		*ModContextPrivate::DescribeModId(ModId), *AssetName.ToString());
	return nullptr;
}

UObject* UModContext::CreateModObject(TSubclassOf<UObject> ObjectClass)
{
	using namespace ModContextPrivate;

	UClass* ResolvedClass = ObjectClass.Get();
	if (!ResolvedClass)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::CreateModObject: mod '%s' called CreateModObject without a class."),
			*DescribeModId(ModId));
		return nullptr;
	}

	if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists | CLASS_Deprecated))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::CreateModObject: mod '%s' asked for '%s', which is abstract, deprecated or superseded."),
			*DescribeModId(ModId), *DescribeClass(ResolvedClass));
		return nullptr;
	}

	// Creating a UClass through NewObject produces an object the reflection system will never accept
	// as a class, and it is never what a mod actually meant.
	if (ResolvedClass->IsChildOf(UClass::StaticClass()))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::CreateModObject: mod '%s' asked for the class object '%s'. Classes are not created this way."),
			*DescribeModId(ModId), *DescribeClass(ResolvedClass));
		return nullptr;
	}

	// An actor built with NewObject is never registered with a world: no components, no ticking, no
	// replication. Refusing it with an explanation beats handing back something subtly broken.
	if (ResolvedClass->IsChildOf(AActor::StaticClass()))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::CreateModObject: mod '%s' asked for actor class '%s'. Actors must be spawned into a world with SpawnActor, not created through the mod context."),
			*DescribeModId(ModId), *DescribeClass(ResolvedClass));
		return nullptr;
	}

	UModSubsystem* ResolvedSubsystem = ResolveSubsystem(TEXT("CreateModObject"));
	if (!ResolvedSubsystem)
	{
		return nullptr;
	}

	// Resolve the registry *before* creating anything. Tracking is what holds the only strong
	// reference to the new object, so an untracked object would simply be collected out from under the
	// mod at the next GC - a far worse outcome than a clean refusal.
	UModRegistry* Registry = ResolvedSubsystem->GetRegistry();
	if (!Registry)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::CreateModObject: mod '%s' has no reachable mod registry, so '%s' was not created; an untracked object would be garbage collected immediately."),
			*DescribeModId(ModId), *DescribeClass(ResolvedClass));
		return nullptr;
	}

	UObject* Created = NewObject<UObject>(this, ResolvedClass);
	if (!Created)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::CreateModObject: mod '%s' could not instantiate '%s'."),
			*DescribeModId(ModId), *DescribeClass(ResolvedClass));
		return nullptr;
	}

	Registry->TrackModObject(ModId, Created);
	return Created;
}

void UModContext::LogInfo(const FString& Message) const
{
	// Display rather than Log: this is a mod author's own output, and it is worth nothing if it only
	// reaches the log file. Framework chatter stays at Log and below so the two do not compete.
	UE_LOG(LogModFramework, Display, TEXT("%s"), *BuildAttributedMessage(Message));
}

void UModContext::LogWarning(const FString& Message) const
{
	UE_LOG(LogModFramework, Warning, TEXT("%s"), *BuildAttributedMessage(Message));
}

void UModContext::LogError(const FString& Message) const
{
	UE_LOG(LogModFramework, Error, TEXT("%s"), *BuildAttributedMessage(Message));
}

UModSubsystem* UModContext::GetSubsystem() const
{
	// Deliberately silent when the subsystem is gone: this is a plain accessor, and callers of an
	// escape hatch are expected to null-check. See the header for why this method is a hazard at all.
	return Subsystem.Get();
}

UWorld* UModContext::GetWorld() const
{
	// A class default object has the package as its outer and no meaningful world. Returning one would
	// let Blueprint latent nodes bind to the wrong context.
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return nullptr;
	}

	if (const UModSubsystem* ResolvedSubsystem = Subsystem.Get())
	{
		if (const UGameInstance* GameInstance = ResolvedSubsystem->GetGameInstance())
		{
			if (UWorld* World = GameInstance->GetWorld())
			{
				return World;
			}
		}
	}

	// The subsystem is the intended route, but a context outered to it can still answer during
	// teardown, and a test-built context has no subsystem at all.
	if (const UObject* Owner = GetOuter())
	{
		return Owner->GetWorld();
	}

	return nullptr;
}

UModSubsystem* UModContext::ResolveSubsystem(const TCHAR* CallingFunction) const
{
	UModSubsystem* ResolvedSubsystem = Subsystem.Get();
	if (!ResolvedSubsystem)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("UModContext::%s: the context of mod '%s' has outlived its mod subsystem. The call was refused."),
			CallingFunction, *ModContextPrivate::DescribeModId(ModId));
	}

	return ResolvedSubsystem;
}

FString UModContext::BuildAttributedMessage(const FString& Message) const
{
	const FString Prefix = FString::Printf(TEXT("[%s] "), *ModContextPrivate::DescribeModId(ModId));

	FString Body = Message;
	if (Body.Len() > MaxLogMessageChars)
	{
		Body = Body.Left(MaxLogMessageChars);
		Body += TEXT("... (truncated)");
	}

	// Attribution has to survive embedded newlines. Without this a mod could write
	// "done\n[other.mod] deleted your save" and have the second line read as another mod's output, so
	// every line of a multi-line message carries the prefix.
	Body.ReplaceInline(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
	Body.ReplaceInline(TEXT("\r"), TEXT("\n"), ESearchCase::CaseSensitive);

	const FString LineBreakWithPrefix = FString(TEXT("\n")) + Prefix;
	Body.ReplaceInline(TEXT("\n"), *LineBreakWithPrefix, ESearchCase::CaseSensitive);

	return Prefix + Body;
}
