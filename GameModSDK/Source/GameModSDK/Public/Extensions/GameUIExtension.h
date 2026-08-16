// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreTypes.h"
#include "Extensions/ModExtension.h"
#include "Templates/SubclassOf.h"
#include "UObject/ObjectMacros.h"

#include "GameUIExtension.generated.h"

class UUserWidget;

/**
 * Base class for everything a mod contributes to the "game.ui" extension point.
 *
 * A UI extension names a widget class and says where in the stack it wants to sit. It never creates,
 * adds or removes a widget: the game does that through UGameUIModAPI, which is what decides whether
 * a mod's panel is allowed on screen at all and where. An extension that could add its own widget
 * would need the game's HUD, and this class would stop being shippable in a standalone SDK.
 *
 * ExtensionPointId is filled in by the constructor. A Blueprint author never has to know the id.
 *
 * Ordering and conflicts: "game.ui" resolves with EModConflictPolicy::Merge - several mods can each
 * add a panel, so the registry deliberately picks no single winner. The game walks every active
 * extension and lays them out by the sort priority each one returns.
 *
 * This is the one point that is NOT server authoritative: a client decorating its own HUD changes
 * nothing anybody else can see, so a UI-only mod does not have to match the server's mod set.
 *
 * The widget class comes from mod-authored script, so NativeGetWidgetClass refuses anything that
 * cannot actually be constructed - null, abstract, deprecated, or a stale class left behind by a
 * Blueprint recompile - rather than handing the game something CreateWidget would choke on.
 */
UCLASS(Abstract, Blueprintable, BlueprintType, meta = (ModPublic, ModExtensionPoint = "game.ui"))
class GAMEMODSDK_API UGameUIExtension : public UModExtension
{
	GENERATED_BODY()

public:
	/** Stamps ExtensionPointId with GameModExtensionPoints::UI so a Blueprint author never does. */
	UGameUIExtension();

	// --- Blueprint hooks ---------------------------------------------------------------------------
	// Implement these in a Blueprint subclass. Game code calls the Native* counterparts below, which
	// validate what the mod returned.

	/**
	 * The widget the mod wants the game to create, or nothing when this extension only reorders.
	 *
	 * Return a widget Blueprint the mod ships. The game constructs it, owns it and can refuse it.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|UI", meta = (DisplayName = "Get Widget Class"))
	TSubclassOf<UUserWidget> GetWidgetClass() const;

	/**
	 * Where this widget sits relative to other mods' widgets. Higher sorts first.
	 *
	 * Implement it only to override the extension's own Priority, which is what the default returns.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Game|UI", meta = (DisplayName = "Get Sort Priority"))
	int32 GetSortPriority() const;

	// --- Native counterparts -----------------------------------------------------------------------
	// What the game calls. Each dispatches to the Blueprint hook only when a Blueprint subclass
	// actually implements it. BlueprintCallable rather than BlueprintPure on purpose: they run
	// mod-authored script, so they must never sit on a pure node that silently re-executes.

	/**
	 * A widget class the game can actually construct, or nullptr.
	 *
	 * Returns nullptr when the mod implemented nothing, when the extension cannot be dispatched into
	 * script (a class default object, or an object already collected during teardown), and when the
	 * returned class is unusable: null, garbage, abstract, deprecated, or superseded by a newer
	 * version after a Blueprint recompile. Each refusal is logged once with the offending class name
	 * so the mod author can see what happened.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|UI", meta = (DisplayName = "Resolve Widget Class"))
	virtual TSubclassOf<UUserWidget> NativeGetWidgetClass() const;

	/**
	 * The mod's sort priority, defaulting to the extension's inherited Priority.
	 *
	 * Falling back to Priority keeps one number in charge of ordering: a mod author who set Priority
	 * in the class defaults to win a conflict gets the matching HUD order for free.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|UI", meta = (DisplayName = "Resolve Sort Priority"))
	virtual int32 NativeGetSortPriority() const;
};
