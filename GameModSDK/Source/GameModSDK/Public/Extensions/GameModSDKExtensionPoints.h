// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "CoreTypes.h"
#include "Extensions/ModExtension.h"
#include "UObject/NameTypes.h"

/**
 * The extension points this game opens to mods.
 *
 * An extension point is a named slot: the game promises to walk it at a defined moment, and a mod
 * fills it with a subclass of the point's base class. The framework enforces identity, typing,
 * quota, permissions and ordering and nothing else - what a contribution *means* is defined
 * entirely by the base class named in the point's descriptor.
 *
 * These ids are part of the published contract with mod authors: they are written into `mod.json`,
 * baked into the generated SDK and hard-coded in every mod already shipped. Never rename one, and
 * never change what it means. Retire a point by keeping it registered and ignoring what arrives.
 *
 * Nothing here reaches the game. An extension returns data and adjusted numbers; deciding what to
 * do with them is the game's job, through UGameCombatModAPI, UGameWorldModAPI and UGameUIModAPI.
 */
namespace GameModExtensionPoints
{
	/**
	 * "game.weapon" - weapon definitions and outgoing damage adjustments.
	 *
	 * Base class UGameWeaponExtension. Conflict policy Priority, because two mods retuning the same
	 * weapon cannot both win. Server authoritative: damage decided on a client is not damage.
	 */
	GAMEMODSDK_API extern const FName Weapon;

	/**
	 * "game.enemy" - enemy definitions, health adjustments and spawn notifications.
	 *
	 * Base class UGameEnemyExtension. Conflict policy Priority, server authoritative.
	 */
	GAMEMODSDK_API extern const FName Enemy;

	/**
	 * "game.gamerule" - wave and session rules, and spawn vetoes.
	 *
	 * Base class UGameRuleExtension. Conflict policy Merge, because rules compose: every contributing
	 * mod is consulted rather than one winning outright. Server authoritative.
	 */
	GAMEMODSDK_API extern const FName GameRule;

	/**
	 * "game.ui" - widgets a mod adds to the game's HUD.
	 *
	 * Base class UGameUIExtension. Conflict policy Merge, because several mods can each add a panel.
	 * NOT server authoritative: a client decorating its own HUD changes nothing anybody else sees.
	 */
	GAMEMODSDK_API extern const FName UI;

	/**
	 * Every point id above, in declaration order.
	 *
	 * The order is stable across calls, so it is safe to use directly for anything that shows the
	 * points to a human - a console dump, a mod browser, generated documentation - without sorting.
	 */
	GAMEMODSDK_API TArray<FName> GetAllPointIds();
}

/**
 * Descriptors for all four points, ready to hand to the framework:
 *
 *     for (const FModExtensionPointDescriptor& Descriptor : BuildGameExtensionPointDescriptors())
 *     {
 *         ModSubsystem->RegisterExtensionPoint(Descriptor);
 *     }
 *
 * Register them once, before any mod loads. UModExtensionRegistry rejects an extension whose point
 * is not open yet with Extension.PointNotFound; it does not queue it and try again later.
 *
 * Registering the same descriptor twice is refused by the registry with ExtensionPoint.Duplicate and
 * changes nothing, so calling this a second time is harmless but pointless.
 */
GAMEMODSDK_API TArray<FModExtensionPointDescriptor> BuildGameExtensionPointDescriptors();
