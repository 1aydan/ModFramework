// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Manifest/ModVersion.h"
#include "Templates/SubclassOf.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "ModExtension.generated.h"

class UModExtension;
class UWorld;

/**
 * A named slot a game opens for mods to plug into.
 *
 * The framework never understands what an extension point *means*; it only knows the point's
 * identity, which base class an extension must derive from, how many a single mod may contribute,
 * which permissions the contributing mod needs, and how to break a tie when two mods claim the same
 * resource. Everything game-specific lives in the base class the game nominates through
 * RequiredBaseClass.
 *
 * Register points from game code with UModSubsystem::RegisterExtensionPoint before any mod loads:
 * an extension whose point is unknown is rejected with Extension.PointNotFound rather than queued.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModExtensionPointDescriptor
{
	GENERATED_BODY()

	/** Stable machine identifier, e.g. "game.weapon". Must not be None. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FName ExtensionPointId;

	/** Semantic version of the point's contract, so a mod can reason about what it is plugging into. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FModVersion Version;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	FText Description;

	/** Extensions must derive from this. Null means any UModExtension. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TSubclassOf<UModExtension> RequiredBaseClass;

	/** Applied by UModExtensionRegistry::ResolveResource when several extensions claim one resource. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	EModConflictPolicy DefaultConflictPolicy = EModConflictPolicy::LastWins;

	/** When false a mod that already contributed an extension to this point is refused a second one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	bool bAllowMultiplePerMod = true;

	/** Informational: extensions of this point only take effect on the authority. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	bool bServerAuthoritative = false;

	/** Every one of these must be granted to the owning mod before its extension is accepted. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mod")
	TArray<FName> RequiredPermissions;
};

/**
 * Base class of everything a mod contributes to an extension point.
 *
 * A mod author subclasses this in Blueprint (or the game's SDK subclasses it in C++ to define a
 * point's contract), fills in ExtensionPointId and optionally ExtensionId, Priority and
 * ClaimedResourceIds in the class defaults, and hands an instance to
 * UModContext::RegisterExtension. The framework then owns the lifecycle:
 *
 *   OnExtensionRegistered  - accepted by the registry; the object is now discoverable.
 *   OnExtensionActivated   - the owning mod reached EModState::Activated.
 *   OnExtensionDeactivated - the owning mod was deactivated; the object stays registered.
 *   OnExtensionUnregistered- removed from the registry; the object is about to be released.
 *
 * Only *activated* extensions are returned by UModExtensionRegistry::GetExtensions, so game code
 * that walks a point never sees contributions from a mod that is loaded but not running.
 *
 * Everything on this class that a mod can author is untrusted data. Nothing here asserts on it.
 */
UCLASS(Abstract, Blueprintable, BlueprintType)
class MODFRAMEWORK_API UModExtension : public UObject
{
	GENERATED_BODY()

public:
	/** Which point this plugs into. Set in the CDO/asset by the mod author. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod")
	FName ExtensionPointId;

	/** Unique within the extension point. Defaults to "<modid>:<classname>" when left empty. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod")
	FName ExtensionId;

	/** Higher wins under EModConflictPolicy::Priority; also a tie-break for ordering. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod")
	int32 Priority = 0;

	/** Resources this extension claims, for conflict detection. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Mod")
	TArray<FName> ClaimedResourceIds;

	/** Assigned by the registry at registration time. Read-only for mods. */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModId OwningModId;

	/** Called once the registry accepted this extension. Overrides must call Super. */
	virtual void OnExtensionRegistered();

	/** Called when the owning mod activates. Overrides must call Super. */
	virtual void OnExtensionActivated();

	/** Called when the owning mod deactivates. Overrides must call Super. */
	virtual void OnExtensionDeactivated();

	/** Called immediately before the registry drops its reference. Overrides must call Super. */
	virtual void OnExtensionUnregistered();

	UFUNCTION(BlueprintImplementableEvent, Category = "Mod", meta = (DisplayName = "On Extension Registered"))
	void ReceiveOnRegistered();

	UFUNCTION(BlueprintImplementableEvent, Category = "Mod", meta = (DisplayName = "On Extension Activated"))
	void ReceiveOnActivated();

	UFUNCTION(BlueprintImplementableEvent, Category = "Mod", meta = (DisplayName = "On Extension Deactivated"))
	void ReceiveOnDeactivated();

	UFUNCTION(BlueprintImplementableEvent, Category = "Mod", meta = (DisplayName = "On Extension Unregistered"))
	void ReceiveOnUnregistered();

	/**
	 * The identity the registry files this extension under.
	 *
	 * ExtensionId when the author set one, otherwise "<owning mod id>:<class name>". The fallback is
	 * only stable once OwningModId has been assigned, which the registry does before it computes the
	 * id, so a mod that leaves ExtensionId empty still gets a collision-free identifier.
	 */
	UFUNCTION(BlueprintPure, Category = "Mod")
	FName GetResolvedExtensionId() const;

	/** True between OnExtensionActivated and OnExtensionDeactivated. Driven only by the registry. */
	UFUNCTION(BlueprintPure, Category = "Mod")
	bool IsExtensionActive() const;

	//~ Begin UObject interface
	/**
	 * Routes world context through the outer chain so a Blueprint extension can use latent nodes,
	 * timers and GetGameInstance. Returns nullptr for class default objects and whenever the outer
	 * chain reaches a package without passing through a world.
	 */
	virtual UWorld* GetWorld() const override;

#if WITH_EDITOR
	/** Tells the Blueprint compiler that GetWorld above is a real implementation. */
	virtual bool ImplementsGetWorld() const override { return true; }
#endif
	//~ End UObject interface

protected:
	/**
	 * Activation state. Owned exclusively by UModExtensionRegistry, which is a friend so that it can
	 * flip this without exposing a public setter that a mod could call to fake being active.
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Mod")
	bool bExtensionActive = false;

private:
	friend class UModExtensionRegistry;
};
