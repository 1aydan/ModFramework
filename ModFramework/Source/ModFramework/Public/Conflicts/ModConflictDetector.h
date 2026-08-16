// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Conflicts/ModConflictTypes.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"

/**
 * The generic conflict model: given a flat list of resource claims and the game's policy table,
 * work out which resources two or more mods fight over and what should happen about it.
 *
 * The detector is deliberately ignorant. It never loads assets, never touches the registry and
 * never decides that a mod should fail; it produces a description of the situation and leaves the
 * acting to UModSubsystem (which rejects blocked mods with EModLoadFailureReason::ConflictRejected)
 * and to UModExtensionRegistry (which uses the same policy to pick a winner at query time).
 *
 * Every entry point is a pure static function over its arguments: no global state, no allocation
 * that outlives the call, and identical input always produces byte-identical output. That matters
 * because the conflict report is shown to players and diffed between sessions.
 *
 * All input is untrusted. Claims naming no extension point, no resource or no mod are skipped;
 * an out-of-range EModConflictPolicy (which deserializing a hand-edited config can produce) is
 * treated as blocking rather than being switched on blindly. Nothing here asserts.
 */
class MODFRAMEWORK_API FModConflictDetector
{
public:
	/**
	 * Groups Claims by (ExtensionPointId, ResourceId) and returns one FModConflict for every group
	 * claimed by two or more *distinct* mods. Several claims from a single mod on one resource are
	 * not a conflict - they are folded into that mod's single entry, taking its highest Priority
	 * and its lowest assigned LoadOrder.
	 *
	 * The applied policy is chosen by the first rule that matches:
	 *   1. `Policies.ResourcePolicies` holds `MakeResourceKey(point, resource)`;
	 *   2. `Policies.PointPolicies` holds the extension point id;
	 *   3. every claim on the resource declared the same `PreferredPolicy`;
	 *   4. `Policies.DefaultPolicy`.
	 * `FModConflict::Explanation` always names which of the four fired.
	 *
	 * Winner selection follows the applied policy:
	 *   - FirstWins  -> the lowest load order;
	 *   - LastWins   -> the highest load order;
	 *   - Priority   -> the highest Priority, ties broken by the highest load order;
	 *   - Merge      -> no winner, every contender is kept, not blocking;
	 *   - Error      -> no winner, blocking.
	 * Every one of those ties is finally broken by ascending mod id, so the winner is deterministic
	 * even when load orders and priorities are identical. A claim whose LoadOrder is INDEX_NONE
	 * (detection ran before the dependency resolver) is ordered after every claim that has one.
	 *
	 * @return conflicts sorted by (ExtensionPointId, ResourceId), both alphabetically. Empty when
	 *         no resource is contested.
	 */
	static TArray<FModConflict> Detect(const TArray<FModResourceClaim>& Claims, const FModConflictPolicyTable& Policies);

	/**
	 * Every contender of every blocking conflict, sorted by id and de-duplicated.
	 *
	 * These are the mods that cannot be loaded as a set. Note that all contenders are returned, not
	 * just the "later" ones: an Error policy states that the game has no way to arbitrate, so the
	 * framework refuses to guess which of them the player meant to keep.
	 */
	static TArray<FModId> GetBlockedMods(const TArray<FModConflict>& Conflicts);

	/**
	 * A column-aligned, multi-line plain-text report: a summary line, a table of one row per
	 * conflict, and the full explanation of each. Used by the `Mod.Conflicts` console command and by
	 * the editor's conflict window. Lines are separated by "\n" and there is no trailing newline.
	 *
	 * Returns "No mod conflicts detected." for an empty array, never an empty string.
	 */
	static FString BuildReport(const TArray<FModConflict>& Conflicts);
};
