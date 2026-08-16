// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Dependencies/ModDependencyTypes.h"
#include "Manifest/ModManifest.h"

/**
 * Turns a pile of discovered manifests into a load order, or into an explanation of why it could
 * not.
 *
 * The resolver is a pure function: no globals, no settings reads, no file access, no UObjects. Feed
 * it the same FModResolveRequest twice and you get the same FModResolveResult twice, down to the
 * order of the diagnostics. That is what makes a mod list reproducible across machines, and what
 * lets the automation tests assert on exact strings.
 *
 * Everything it consumes is untrusted. A manifest may name a dependency on itself, declare a version
 * range that does not parse, list the same mod twice, or form a cycle with four other mods. None of
 * that asserts; all of it produces an FModRejection whose Message names the mods involved and the
 * versions actually installed.
 *
 * Resolution runs in this order:
 *   1. Duplicate ids       - the highest version wins, the rest are rejected (DuplicateModId).
 *   2. Disabled mods       - rejected outright (Disabled).
 *   3. Environment         - game id/version, framework range, sdk id/range (IncompatibleGame,
 *                            IncompatibleFramework, IncompatibleSdk). Skipped when
 *                            FModResolveRequest::bCheckEnvironment is false.
 *   4. Required deps       - absent (MissingDependency) or version-incompatible
 *                            (IncompatibleDependencyVersion). Optional deps never reject.
 *   5. Cascade             - a mod requiring a rejected mod is rejected too (DependencyFailed),
 *                            repeated to a fixed point so the failure travels the whole chain.
 *   6. Cycles              - strongly connected components over the required-dependency and ordering
 *                            edges; every member is rejected (CircularDependency), then the cascade
 *                            runs once more over the survivors.
 *   7. Topological sort    - Kahn's algorithm; the ready set is kept sorted by (Priority desc,
 *                            ModId asc) so the output is stable across runs and platforms.
 *
 * Graph edges all point "loads earlier -> loads later":
 *   - a required dependency -> the mod that requires it;
 *   - an optional dependency that is actually present -> the mod that lists it;
 *   - a `loadAfter` target -> the mod that lists it;
 *   - the mod -> each of its `loadBefore` targets.
 * Ordering entries naming a mod that is not in the resolved set are dropped with an Info diagnostic.
 *
 * Diagnostic codes emitted: `Resolve.DuplicateId`, `Resolve.Disabled`, `Resolve.Environment`,
 * `Resolve.MissingDependency`, `Resolve.VersionConflict`, `Resolve.Cycle`, `Resolve.CascadeReject`,
 * `Resolve.OptionalMissing`, `Resolve.OrderingIgnored`, plus `Resolve.InvalidManifest` for a
 * candidate that carries no usable id and can therefore not take part at all.
 */
class MODFRAMEWORK_API FModDependencyResolver
{
public:
	/** Runs the whole pipeline. Never fails, never asserts: failure is described, not thrown. */
	static FModResolveResult Resolve(const FModResolveRequest& Request);

	/**
	 * Checks a single manifest against the environment. Empty result means compatible.
	 *
	 * A manifest problem that makes a check impossible - an empty required id, a version range that
	 * does not parse - is treated as "any" rather than as a rejection, because punishing a player for
	 * a mod author's typo helps nobody. Resolve() records a warning diagnostic in that case; this
	 * standalone entry point has nowhere to put one, so it stays silent.
	 */
	static TArray<FModRejection> CheckEnvironment(const FModManifest& Manifest, const FModEnvironment& Environment);

	/**
	 * Just the graph edges, for the editor's dependency visualiser.
	 *
	 * Every manifest with a usable id becomes a key, even when it has no edges, so the map doubles as
	 * the node list. Edges point from the mod that must load earlier to the mod that must load later
	 * and are de-duplicated and sorted. Edges naming a mod that is not in Manifests are dropped; a
	 * self-edge (a mod that depends on or orders itself) is kept, because FindCycles has to see it.
	 * When Manifests holds several entries with the same id the highest version wins, matching
	 * Resolve().
	 */
	static void BuildGraph(const TArray<FModManifest>& Manifests, TMap<FModId, TArray<FModId>>& OutEdges);

	/**
	 * Every cycle in the graph, via Tarjan's strongly connected components.
	 *
	 * A component of more than one node is a cycle; so is a single node that links to itself. Each
	 * returned entry is one cycle in canonical rotation: it starts at the lexicographically smallest
	 * member and does not repeat that member at the end, so "b -> c -> a -> b" is returned as
	 * {a, b, c}. Components larger than the cycle they contain are reported through the shortest
	 * cycle that passes through their smallest member. The outer array is sorted by first element,
	 * which is unique because components are disjoint - so the result is fully deterministic.
	 */
	static TArray<TArray<FModId>> FindCycles(const TMap<FModId, TArray<FModId>>& Edges);
};
