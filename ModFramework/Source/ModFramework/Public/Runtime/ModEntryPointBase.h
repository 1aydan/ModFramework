// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "ModEntryPointBase.generated.h"

class UModContext;
class UWorld;

/**
 * The class a mod names in `entryPoint.class`, and the only code of a mod the framework calls
 * directly.
 *
 * A mod author subclasses this - almost always in Blueprint - and implements whichever of the four
 * lifecycle events it cares about. UModSubsystem instantiates the class when the mod loads, assigns
 * Context, and drives the Native* hooks in lifecycle order:
 *
 *   NativeOnModLoaded      - the mod's content is mounted and its entry point exists. The mod may
 *                            register extensions and subscribe to events, but nothing it registered
 *                            is live yet.
 *   NativeOnModActivated   - the mod is running. Its extensions are active and its content bundles
 *                            are applied.
 *   NativeOnModDeactivated - the mod has been switched off but is still loaded. It may be activated
 *                            again without reloading.
 *   NativeOnModUnloaded    - the last call the mod ever gets. Everything created through Context is
 *                            released immediately afterwards.
 *
 * Loaded and Activated are deliberately separate states, so do not treat OnModLoaded as "start
 * running": a mod can sit loaded and inactive indefinitely.
 *
 * The Native* methods are the extension point for a C++ entry point; each one raises the matching
 * BlueprintImplementableEvent, so an override must call Super or the Blueprint half of the mod stops
 * being told anything. They are safe to call whether or not a Blueprint implements the event, and
 * they refuse to dispatch on a class default object or on an object that is already garbage - the
 * subsystem tears mods down during shutdown, when both are possible.
 *
 * Everything a mod can reach goes through Context, which is the framework's security boundary. There
 * is deliberately no other handle on this class.
 */
UCLASS(Abstract, Blueprintable, BlueprintType)
class MODFRAMEWORK_API UModEntryPointBase : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * The mod's handle on the framework, assigned by UModSubsystem before NativeOnModLoaded runs.
	 *
	 * Read-only to Blueprint on purpose: a mod that could swap its own context could act as another
	 * mod. Null only for a class default object, or after the owning mod has been released.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TObjectPtr<UModContext> Context;

	/** Raises OnModLoaded. Overrides must call Super. */
	virtual void NativeOnModLoaded();

	/** Raises OnModActivated. Overrides must call Super. */
	virtual void NativeOnModActivated();

	/** Raises OnModDeactivated. Overrides must call Super. */
	virtual void NativeOnModDeactivated();

	/** Raises OnModUnloaded. Overrides must call Super. */
	virtual void NativeOnModUnloaded();

	/** The mod's content is mounted and its code exists. Register extensions and subscribe here. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Mod", meta = (DisplayName = "On Mod Loaded"))
	void OnModLoaded();

	/** The mod is running: its extensions are live. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Mod", meta = (DisplayName = "On Mod Activated"))
	void OnModActivated();

	/** The mod has been switched off but is still loaded, and may be activated again. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Mod", meta = (DisplayName = "On Mod Deactivated"))
	void OnModDeactivated();

	/** The last call the mod receives. Everything it created is released right after this returns. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Mod", meta = (DisplayName = "On Mod Unloaded"))
	void OnModUnloaded();

	//~ Begin UObject interface
	/**
	 * Routed through Context, so a Blueprint entry point can use timers, latent nodes and
	 * GetGameInstance. Falls back to the outer chain when there is no context, and returns nullptr
	 * for class default objects and when no world can be resolved at all.
	 */
	virtual UWorld* GetWorld() const override;

#if WITH_EDITOR
	/** Tells the Blueprint compiler that GetWorld above is a real implementation. */
	virtual bool ImplementsGetWorld() const override { return true; }
#endif
	//~ End UObject interface

protected:
	/**
	 * Whether it is safe to run Blueprint script on this object right now.
	 *
	 * False - with a logged warning naming CallingFunction - for a class default object, an archetype
	 * or an object that is already garbage. Every Native* hook consults this before raising its
	 * event, because a mod's teardown can legitimately race the garbage collector and calling
	 * ProcessEvent on a dead object is not survivable.
	 */
	bool CanDispatchScriptEvent(const TCHAR* CallingFunction) const;
};
