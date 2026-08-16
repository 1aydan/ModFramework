// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "API/ModAPI.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Delegates/Delegate.h"
#include "Manifest/ModVersion.h"
#include "Templates/SubclassOf.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "ModAPIRegistry.generated.h"

/**
 * One live entry of UModAPIRegistry.
 *
 * The descriptor is a snapshot taken when the API was registered rather than something rebuilt on
 * demand, so what GetAPIDescriptor reports can never drift away from what RequestAPI validated.
 * Keeping the object and its descriptor in a single reflected struct is also what keeps the API
 * alive against garbage collection.
 */
USTRUCT()
struct MODFRAMEWORK_API FModAPIRegistration
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UModAPI> Api = nullptr;

	UPROPERTY()
	FModAPIDescriptor Descriptor;
};

/**
 * The framework's directory of game-provided APIs.
 *
 * The registry is deliberately ignorant: it knows an API's id, version, required permissions and
 * server-authority flag, and nothing at all about what the API does. Its whole job is to make
 * `RequestAPI` a chokepoint that a mod cannot get around.
 *
 * Failure diagnostics use these stable codes:
 *   API.NotFound          no API is registered under that id
 *   API.VersionMismatch   the registered version does not satisfy the requested range (also used
 *                         when the requested range expression itself does not parse)
 *   API.PermissionDenied  the requesting mod is missing at least one required permission
 *   API.ClassMismatch     the registered API is not of the Blueprint-requested class
 *   API.Duplicate         registration refused, that id is already taken
 *   API.NullApi           registration refused, the API object was null or garbage
 *   API.InvalidId         registration refused, the API resolved to an empty id
 *
 * Permission checking is injected by UModSubsystem through SetPermissionCheck so the registry never
 * has to own permission state. It fails CLOSED: while no check is installed, any API that declares
 * required permissions is refused to everyone. APIs that declare no permissions are unaffected, so
 * editor tooling and automation tests keep working without wiring anything up.
 *
 * Not thread-safe. Registration and lookup happen on the game thread.
 */
UCLASS(BlueprintType)
class MODFRAMEWORK_API UModAPIRegistry : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Registers a game-provided API. Equivalent to RegisterAPIForMod with an invalid provider id.
	 *
	 * @return false with OutError filled when InApi is null or garbage (API.NullApi), when it
	 *         resolves to an empty id (API.InvalidId), or when the id - or the object itself - is
	 *         already registered (API.Duplicate).
	 */
	bool RegisterAPI(UModAPI* InApi, FModDiagnostic& OutError);

	/**
	 * Registers an API supplied by a mod, so UnregisterAllForMod can take it away again when that
	 * mod unloads. Pass an invalid FModId for an API the game itself provides.
	 */
	bool RegisterAPIForMod(UModAPI* InApi, const FModId& ProviderModId, FModDiagnostic& OutError);

	/** Removes one API and calls UModAPI::OnAPIUnregistered on it. False when nothing was registered under ApiId. */
	bool UnregisterAPI(FName ApiId);

	/** Removes every API whose provider is ModId. A no-op for an invalid ModId, which never owns anything. */
	void UnregisterAllForMod(const FModId& ModId);

	/** Removes every API. Each one is notified exactly once, and the change delegate fires for each. */
	void Reset();

	UFUNCTION(BlueprintPure, Category = "Mod|API")
	UModAPI* FindAPI(FName ApiId) const;

	UFUNCTION(BlueprintPure, Category = "Mod|API")
	bool HasAPI(FName ApiId) const;

	/** Every registered API, sorted by id so console output and tests are reproducible. */
	UFUNCTION(BlueprintPure, Category = "Mod|API")
	TArray<FModAPIDescriptor> GetAllAPIs() const;

	UFUNCTION(BlueprintPure, Category = "Mod|API")
	bool GetAPIDescriptor(FName ApiId, FModAPIDescriptor& OutDescriptor) const;

	/**
	 * The gated path mods must use. Checks: API exists, version satisfies RequiredRange,
	 * requesting mod holds every RequiredPermission. Fills OutError otherwise.
	 *
	 * OutError is cleared on entry and is only meaningful when this returns nullptr.
	 *
	 * @param ApiId          Id of the wanted API. Matching is case insensitive, as FName always is.
	 * @param RequiredRange  Acceptable versions. An empty or unparsed range accepts nothing, so
	 *                       pass FModVersionRange::Any() to mean "any version".
	 * @param RequestingMod  The mod asking. Permissions are always checked, including for an
	 *                       invalid id, so passing a blank id is not a way around the gate.
	 */
	UModAPI* RequestAPI(FName ApiId, const FModVersionRange& RequiredRange, const FModId& RequestingMod, FModDiagnostic& OutError);

	/**
	 * Blueprint-facing RequestAPI.
	 *
	 * On top of the native checks it verifies that the registered API actually is an ApiClass, so a
	 * Blueprint that casts the result cannot end up with a wrongly typed object; a mismatch returns
	 * nullptr with API.ClassMismatch. A null ApiClass skips that check.
	 *
	 * VersionRange is the textual form documented on FModVersionRange ("^1.0.0", ">=1.2 <2.0.0",
	 * "*"). An empty string means "any version". A malformed expression is refused with
	 * API.VersionMismatch rather than being silently widened.
	 */
	UFUNCTION(BlueprintCallable, Category = "Mod|API", meta = (DeterminesOutputType = "ApiClass", DisplayName = "Request Mod API"))
	UModAPI* K2_RequestAPI(FName ApiId, TSubclassOf<UModAPI> ApiClass, const FString& VersionRange, FModId RequestingMod, FModDiagnostic& OutError);

	/** Injected by UModSubsystem so the registry can ask about permissions without owning them. */
	DECLARE_DELEGATE_RetVal_TwoParams(bool, FModPermissionCheck, const FModId&, FName);

	/** Pass an unbound delegate to remove the check; the registry then refuses every gated API. */
	void SetPermissionCheck(FModPermissionCheck InCheck);

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnModAPIRegistryChanged, FName /*ApiId*/);

	/** Fired after the API is in the map and has been notified. */
	FOnModAPIRegistryChanged OnAPIRegistered;

	/** Fired after the API has been notified and removed from the map. */
	FOnModAPIRegistryChanged OnAPIUnregistered;

private:
	/**
	 * Runs the permission gate for one request.
	 *
	 * @return true when the mod may have the API. Fills OutDeniedPermission with the first missing
	 *         permission, and sets bOutNoCheckInstalled when the refusal is only due to the absence
	 *         of a permission delegate, so the diagnostic can say so.
	 */
	bool CheckPermissions(const TArray<FName>& RequiredPermissions, const FModId& RequestingMod,
		FName& OutDeniedPermission, bool& bOutNoCheckInstalled) const;

	/** Notifies and forgets a single entry. Assumes the caller has already removed it from the map. */
	void FinalizeRemoval(const FModAPIRegistration& Registration, FName ApiId);

	UPROPERTY()
	TMap<FName, FModAPIRegistration> Registrations;

	FModPermissionCheck PermissionCheck;
};
