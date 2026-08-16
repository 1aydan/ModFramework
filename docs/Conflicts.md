# Mod Conflicts

Two mods editing the same thing is normal, not exceptional. The framework's job is to make it
**visible and deterministic** rather than to let load order silently decide.

The model is generic. The framework has no idea what a "weapon" is — it only knows that two mods
claimed the resource `weapon.longsword` at the extension point `game.weapon`, and that a policy says
who wins.

## Declaring a claim

A mod states what it modifies in its manifest:

```json
"claims": [
	{ "extensionPoint": "game.weapon", "resource": "weapon.longsword", "policy": "Priority" }
]
```

Extensions can also claim at runtime via `UModExtension::ClaimedResourceIds`, which is the usual path
for a data-driven mod — the claim comes from the extension asset rather than being duplicated in the
manifest.

Declaring is voluntary but strongly encouraged: an undeclared overlap is only discovered when
something behaves oddly in play, whereas a declared one is caught at load.

## Policies

| Policy | Winner | Use when |
|---|---|---|
| `Error` | none — **blocking** | The resource genuinely cannot be shared. Both mods are refused with a diagnostic. |
| `FirstWins` | earliest in load order | Base-layer resources where the first definition should stick. |
| `LastWins` | latest in load order | Overrides, where "installed later" reasonably means "meant to win". |
| `Priority` | highest `priority`, ties break to latest load order | The mod author should have a say. |
| `Merge` | none — all kept | Additive resources like loot tables or spawn lists, where combining is meaningful. |

`Merge` does not merge anything itself — it declares that the extension point's own code will
combine all contenders. The framework hands you every contender and stays out of the way.

## Which policy applies

Resolved in this order, first match wins:

1. A per-resource override in the policy table (key `"<extensionPoint>:<resource>"`)
2. A per-extension-point override in the policy table
3. The mods' **unanimous** preference — when every contender declared the same `policy`
4. `FModConflictPolicyTable::DefaultPolicy`, from
   `UModFrameworkSettings::DefaultConflictPolicy` (ships as `Error`)

Rule 3 is why the manifest's `policy` field is a *preference*: it is honoured only when nobody
disagrees. One mod cannot unilaterally decide it wins. The applied rule is named in the conflict's
`Explanation`, so "why did this mod win?" always has an answer.

The default is `Error` on purpose. Silently picking a winner at project scope trains developers to
ignore conflicts; opt into leniency per extension point, where you know whether overriding is safe.

## Reading the report

```
Mod.Conflicts
```

```
Extension Point   Resource           Policy     Winner              Contenders
game.weapon       weapon.longsword   Priority   com.b.sharpblades   com.a.betterweapons (p0),
                                                                    com.b.sharpblades (p10)
game.enemy        enemy.grunt        Error      —                   com.a.hardmode,
                                                                    com.c.enemyrework   [BLOCKING]

1 blocking conflict. Mods refused: com.a.hardmode, com.c.enemyrework
```

Same data in the editor under **Window → Mod Developer → Conflicts**.

Output ordering is deterministic — sorted by extension point then resource — so it diffs cleanly
between runs.

## Blocking vs resolved

A conflict under `Error` is **blocking**: every contender is rejected with
`EModLoadFailureReason::ConflictRejected`. Nothing partially applies.

Every other policy resolves. Losers stay loaded and active — they simply do not own that resource.
A mod that changes ten things and loses one of them keeps its other nine. This matters: rejecting a
whole mod over one overlapping resource is almost always the wrong trade.

## Same mod, same resource

Two claims from the same mod on the same resource are not a conflict. A conflict needs two
**distinct** mods.

## For SDK authors

Each extension point sets its own default:

```cpp
FModExtensionPointDescriptor Point;
Point.ExtensionPointId       = TEXT("game.lootTable");
Point.DefaultConflictPolicy  = EModConflictPolicy::Merge;    // loot is additive
Point.bAllowMultiplePerMod   = true;
```

Choosing well is most of the work:

- Additive collections — loot, spawn lists, recipes → `Merge`
- Singular definitions where later-installed should win → `LastWins`
- Anything where two mods disagreeing would corrupt state → `Error`

`ResolveResource(PointId, ResourceId)` applies the policy and returns the single winner, or
`nullptr` under `Error`/`Merge` where there is no single winner by definition.

## See also

- [ManifestFormat.md](ManifestFormat.md) — the `claims` field
- [ConsoleCommands.md](ConsoleCommands.md) — `Mod.Conflicts`
