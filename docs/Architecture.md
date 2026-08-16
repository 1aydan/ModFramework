# Architecture

## The one rule

`ModFramework` never learns what your game is about.

There is no `UWeaponMod`, no `UQuestMod`, no `UEnemyMod` — and there never will be. The framework
provides *generic* registration and discovery machinery; every game-specific concept lives in that
game's SDK. This is what makes the framework reusable across completely different games, and it is
the constraint that most modding systems abandon early and then cannot recover.

The practical test: if a type name would be wrong for a farming sim, it does not belong in
`ModFramework`.

## Layers

```
┌─────────────────────────────────────────────────────────┐
│  Mod content:  Blueprints · Data Assets · Data Tables   │
├─────────────────────────────────────────────────────────┤
│  UModContext          the only handle a mod is given    │
├─────────────────────────────────────────────────────────┤
│  GameModSDK           game APIs · extension points      │  ← game-specific
├─────────────────────────────────────────────────────────┤
│  UModSubsystem        lifecycle orchestration           │
│  ├── UModRegistry     mods, APIs, extensions            │
│  ├── UModEventBus     generic events                    │
│  ├── UModPermissionRegistry                             │
│  ├── UModSaveDataManager                                │
│  ├── FModContentManager   mounting abstraction          │
│  └── IModProvider[]   acquisition                       │
├─────────────────────────────────────────────────────────┤
│  Pak · IoStore · filesystem                             │
└─────────────────────────────────────────────────────────┘
```

Each seam exists to keep a decision reversible:

- **`IModProvider`** separates *acquiring* a mod from *validating and loading* it. Steam, a CDN, a
  dedicated-server push and a local folder are interchangeable, and none of them is baked into the
  core.
- **`IModContentMounter`** hides Pak/IoStore. Nothing above it mentions either. When a future engine
  version changes packaging, one implementation changes.
- **`UModAPI`** lets a game expose behaviour the framework cannot possibly understand, with
  versioning and permission gating applied generically.
- **`UModContext`** is the only object a mod receives, which makes the mod-facing surface auditable
  in one file.

## Lifecycle

```
Discovered → Validated → DependenciesResolved → Mounted
          → Loading → Loaded → Activated ⇄ Deactivated → Unmounted
```

Every transition goes through `UModRegistry::SetModState`, which rejects illegal transitions, and
then broadcasts on the event bus. Nothing sets state directly. `IsTransitionAllowed` is public so the
state machine is testable in isolation.

**Loaded and Activated are separate**, and this is the most important structural decision in the
system. Loading instantiates the entry point and gives the mod its context. Activation is when it
starts affecting the game. Keeping them apart is what makes it possible to load a mod and *not* run
it, to deactivate without unmounting, and to reason about failure — a mod that fails during
activation was fully loaded, so its resources are known and releasable.

**Failure is a state, not an exception.** Nothing throws, nothing asserts on mod data. A failed mod
carries a reason and diagnostics and the game keeps running.

**Rejection happens before mounting.** Version and dependency checks all run during resolution, so an
incompatible mod never gets to touch the asset registry. There is no partially-applied mod.

## Resolution

Given every discovered manifest:

1. Duplicate ids → keep the highest version, reject the rest
2. Explicitly disabled → rejected
3. Environment checks — game id and version, framework range, SDK id and range
4. Required dependencies missing or version-incompatible → rejected; optional missing → info only
5. **Cascade** — rejecting X rejects everything requiring X, transitively, to a fixed point
6. **Cycles** — Tarjan SCC over required + ordering edges; every member rejected, cycle path reported
7. **Topological sort** — Kahn's algorithm, ready set ordered by `(priority desc, id asc)`

Step 7's tie-break is what makes load order **identical on every machine and every run**. A modding
system whose load order depends on directory enumeration produces bugs that reproduce for one user
and nobody else.

Diagnostics name the actual problem:

```
Mod 'Better Combat' (com.example.bettercombat) requires 'com.example.corelib' >=2.0.0,
but version 1.4.0 is installed.
```

## Extension points

The framework knows only: an id, a required base class, a conflict policy, and whether it is
server-authoritative. What a `game.weapon` *is* lives entirely in the SDK.

```cpp
FModExtensionPointDescriptor Point;
Point.ExtensionPointId      = TEXT("game.weapon");
Point.RequiredBaseClass     = UMyGameWeaponExtension::StaticClass();
Point.DefaultConflictPolicy = EModConflictPolicy::Priority;
Point.bServerAuthoritative  = true;
Subsystem->RegisterExtensionPoint(Point);
```

Extensions are ordered by `(priority desc, owning mod load order asc, extension id asc)` — again,
fully deterministic.

## The data-driven path

Most mods should need no C++. The chain:

```
mod.json  →  entryPoint.class     (Blueprint deriving UModEntryPointBase)
          →  entryPoint.contentBundles  (UModContentBundle data assets)
          →  extension classes registered on activation
```

A mod author subclasses `UModEntryPointBase` in Blueprint, gets `OnModActivated` with a
`UModContext`, and registers extensions or requests APIs from there. A mod with only content and no
entry point is also legal — the pure-asset case.

## Registries

`UModRegistry` owns the API and extension registries, so ownership is unambiguous and unloading a mod
has one place to clean up: release tracked objects, unregister its APIs, extensions and event
subscriptions, then unmount.

Mod-owned objects are tracked forward (`FModId → objects`) and backward
(`weak object → FModId`), so "which mod created this?" is answerable — essential for diagnostics when
something misbehaves three systems away from its cause.

## Permissions and the honest boundary

Permissions gate API access. They are **not** a sandbox — Blueprint runs in-process with engine
privileges, and the framework refuses to imply otherwise. See [Security.md](Security.md).

The registry is injected into the API registry, extension registry and event bus as a
`FModPermissionCheck` delegate rather than a hard reference, so each is independently testable and
none of them owns permission state.

## Multiplayer

Designed in from the start, because manifest fields cannot be added after mods exist.

`FModSessionManifest` travels as a Base64 login option and is validated server-side in `PreLogin`.
The server is authoritative; required mods must match **exactly**, not by range — two "compatible"
versions of a gameplay mod can still simulate differently.

## Save data

Each mod gets a namespace inside one envelope. Records for absent mods are marked orphaned and
preserved byte-for-byte; the envelope also records which mods were present when the save was made.
Removing a mod must never corrupt unrelated data, and that is a data-layout guarantee, not a
best-effort one.

## Module split

| Module | Type | Contains |
|---|---|---|
| `ModFramework` | Runtime, `PostConfigInit` | Everything at runtime |
| `ModFrameworkDeveloper` | UncookedOnly | Packaging, validation, SDK generation, commandlets |
| `ModFrameworkEditor` | Editor | Mod Developer window, SDK generation UI |

`PostConfigInit` is early — before the UObject system exists. Module startup therefore must not touch
any CDO, which is why console command registration is unconditional and the settings check happens
inside each command's callback.

## What the MVP deliberately omits

Lua scripting, native C++ mod loading, and Workshop integration. Each depends on the lifecycle, SDK
boundary and mounting being stable first. Native code additionally needs ABI, signing and revocation
answers that are distribution problems, not framework ones — and shipping a half-answer there is
worse than shipping none.

## See also

- [Versioning.md](Versioning.md) · [ManifestFormat.md](ManifestFormat.md) · [Security.md](Security.md)
- [Conflicts.md](Conflicts.md) · [Multiplayer.md](Multiplayer.md) · [Packaging.md](Packaging.md)
