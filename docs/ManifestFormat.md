# Mod Manifest Format (`mod.json`)

Every mod is identified by a single `mod.json` at the root of its folder. This file is the contract
between the mod and the game: it is read and fully validated **before** any of the mod's content is
mounted and before any of its code runs.

Current `manifestVersion`: **1**

## Complete example

```json
{
	"manifestVersion": 1,

	"id": "com.example.bettercombat",
	"name": "Better Combat",
	"description": "Rebalances melee combat and adds two enemy variants.",
	"author": "Example Author",
	"homepage": "https://example.com/bettercombat",
	"license": "MIT",
	"version": "2.1.0",

	"game":      { "id": "com.example.game", "version": ">=1.5.0 <2.0.0" },
	"framework": { "version": "^0.1.0" },
	"sdk":       { "id": "com.example.game.sdk", "version": "^1.5.0" },

	"dependencies": [
		{ "id": "com.example.corelib", "version": ">=2.0.0" },
		{ "id": "com.example.ui", "version": "*", "optional": true,
		  "reason": "Adds a settings page when present." }
	],

	"loadBefore": ["com.example.late"],
	"loadAfter":  ["com.example.early"],
	"priority": 0,

	"network": { "scope": "ClientAndServer", "requiredForNetworkPlay": true },

	"permissions": ["gameplay.modify", "save.modify"],

	"entryPoint": {
		"nativeModule": "",
		"class": "/BetterCombat/BP_BetterCombatEntry.BP_BetterCombatEntry_C",
		"contentBundles": ["/BetterCombat/DA_BetterCombatBundle.DA_BetterCombatBundle"]
	},

	"content": [
		{ "path": "Content/BetterCombat.pak", "type": "Pak",
		  "mountPoint": "/BetterCombat/", "mountOrder": 0 }
	],

	"claims": [
		{ "extensionPoint": "game.weapon", "resource": "weapon.longsword", "policy": "Priority" }
	],

	"metadata": { "category": "Gameplay" }
}
```

## Fields

### Identity

| Field | Type | Required | Notes |
|---|---|---|---|
| `manifestVersion` | int | no (default `1`) | A manifest newer than the framework understands is rejected rather than guessed at. |
| `id` | string | **yes** | Stable machine identifier. See [Mod IDs](#mod-ids). |
| `name` | string | **yes** | Display name. **Never** used as an identifier. |
| `description` | string | no | |
| `author` | string | no | |
| `homepage` | string | no | |
| `license` | string | no | |
| `icon` | path | no | Image shown in a mod-browser UI. See [Icons](#icons). |
| `version` | semver | **yes** | The mod's own version. Must be non-zero. |

### Icons

```json
"icon": "Icon.png"
```

A path **relative to the mod root**, pointing at a `.png`, `.jpg` or `.jpeg` file. 256×256 is the
recommended size.

The icon deliberately lives outside the mod's cooked content, as a plain image file:

```
BrutalCombat/
├── mod.json
├── Icon.png          ← readable without mounting anything
└── Content/
    └── BrutalCombat.pak
```

This is so it can be loaded **before the mod is mounted** — a mod list needs to show an icon for
mods that are merely discovered, disabled, or *rejected*, and a `.uasset` inside a pak is
unreachable in all three cases. The same file is read directly out of a `.mod` package without
extracting it.

Being untrusted input, icons are capped at 2 MiB and 1024×1024 by default
(`MaxIconFileBytes` / `MaxIconDimension`). A missing, oversized, or corrupt icon falls back to the
game's default icon and **never** affects whether the mod loads.

Game developers populate their UI through `UModIconCache`:

```cpp
// Non-blocking, safe every frame — returns null until the icon has been requested and decoded.
UTexture2D* Icon = IconCache->FindIcon(ModId);

// Kick off a load; the delegate fires on the game thread.
IconCache->RequestIcon(ModId, OnIconLoaded);
```

### Compatibility

| Field | Type | Required | Default |
|---|---|---|---|
| `game.id` | string | **yes** | — |
| `game.version` | range | no | `*` |
| `framework.version` | range | no | `*` |
| `sdk.id` | string | no | — (unchecked when empty) |
| `sdk.version` | range | no | `*` |

Range syntax is documented in [Versioning.md](Versioning.md#version-ranges). Prefer pinning the
**SDK**, not the game — a game patches far more often than its modding surface changes.

### Dependencies

`dependencies` is a single array; `optional` distinguishes the two kinds.

| Field | Type | Default | Notes |
|---|---|---|---|
| `id` | string | — | Must be a valid mod id, and never this mod's own id. |
| `version` | range | `*` | |
| `optional` | bool | `false` | When absent, an optional dependency is skipped with an info diagnostic. When present, it still creates an ordering edge and loads first. |
| `reason` | string | — | Shown to the player when the dependency cannot be satisfied. Worth writing. |

A **required** dependency that is missing, disabled, rejected, or version-incompatible rejects this
mod too, and the rejection cascades transitively to anything depending on *it*.

### Load order

| Field | Type | Default | Notes |
|---|---|---|---|
| `loadBefore` | string[] | `[]` | This mod loads before each listed mod. |
| `loadAfter` | string[] | `[]` | This mod loads after each listed mod. |
| `priority` | int | `0` | Range `-1000..1000`. Higher loads earlier among otherwise-unordered mods. |

Ordering is a deterministic topological sort. Ties break on `(priority desc, id asc)`, so the load
order is identical on every machine and every run. Entries naming an absent mod are ignored with an
info diagnostic; a **cycle** — through dependencies or through ordering constraints — rejects every
mod in the cycle and reports the path:

```
Dependency cycle: com.a.mod -> com.b.mod -> com.c.mod -> com.a.mod
```

### Network

| Field | Type | Default | Notes |
|---|---|---|---|
| `network.scope` | `ClientAndServer` \| `ClientOnly` \| `ServerOnly` | `ClientAndServer` | |
| `network.requiredForNetworkPlay` | bool | `false` | A server running this mod refuses clients that lack it at the same version. |

See [Multiplayer.md](Multiplayer.md).

### Permissions

`permissions` is an array of permission ids the mod is **requesting**, not being granted. Requesting
does not grant: the game's permission policy decides, and dangerous permissions are never
auto-granted. See [Permissions.md](Permissions.md).

### Entry point

| Field | Type | Notes |
|---|---|---|
| `entryPoint.class` | object path | A Blueprint or native class deriving from `UModEntryPointBase`. Note the `_C` suffix for Blueprint classes. |
| `entryPoint.contentBundles` | object path[] | `UModContentBundle` assets whose extensions register on activation. |
| `entryPoint.nativeModule` | string | Advanced. Not loaded by the MVP; see [Security.md](Security.md). |

All three are optional. A mod with only `content` and no entry point is legal — that is the
pure-asset case.

### Content roots

| Field | Type | Default | Notes |
|---|---|---|---|
| `path` | string | — | **Relative to the mod root.** Absolute paths and `..` segments are rejected. |
| `type` | `Pak` \| `IoStore` \| `LooseDirectory` | `Pak` | `LooseDirectory` is development-only and off unless the project opts in. |
| `mountPoint` | string | `/<ModFolderName>/` | Virtual package root. Must be `/Word/`. Must be unique across all loaded mods. |
| `mountOrder` | int | `0` | Higher wins for identical asset paths. |

Mounting is **all-or-nothing**: if any root of a mod fails, every root already mounted for that mod
is rolled back, so a mod is never half-present.

### Claims

`claims` declares what the mod modifies, so conflicts are detected *before* two mods fight over the
same resource at runtime.

| Field | Type | Notes |
|---|---|---|
| `extensionPoint` | string | An extension point id defined by the game's SDK. |
| `resource` | string | The specific resource within that point. |
| `policy` | `Error` \| `FirstWins` \| `LastWins` \| `Priority` \| `Merge` | The mod's *preferred* policy. The game's policy table has the final say. |

See [Conflicts.md](Conflicts.md).

### Metadata

`metadata` is a free-form `string → string` map. The framework never interprets it; it is there so a
game or a mod browser can carry its own fields without a manifest schema change.

## Mod IDs

An id must be:

- 3–128 characters
- start with `[a-z0-9]`
- contain only `[a-z0-9._-]`
- contain no `..`, and no leading or trailing `.`, `-` or `_`

IDs are **lowercased** on parse, so `Com.Example.Mod` and `com.example.mod` are the same mod.
Reverse-DNS (`com.yourname.modname`) is strongly recommended — ids are a flat global namespace across
everything a player installs, and collisions are resolved by rejecting a mod.

**Never rename an id.** It is the key for save data, dependency references, load order, and
multiplayer matching. Change the display name freely; changing the id creates a different mod that
cannot read its predecessor's saves.

## Validation

Parsing produces a manifest **and** a list of diagnostics, each with a stable machine-readable code.

Errors — the mod will not load:

| Code | Meaning |
|---|---|
| `Manifest.InvalidJson` | The file is not well-formed JSON. |
| `Manifest.MissingField` | A required field is absent. |
| `Manifest.InvalidId` | `id` violates the id rules. |
| `Manifest.InvalidVersion` | `version` is not valid semver. |
| `Manifest.InvalidVersionRange` | A range expression could not be parsed. |
| `Manifest.UnsupportedManifestVersion` | `manifestVersion` is newer than this framework. |
| `Manifest.SelfDependency` | The mod depends on itself. |
| `Manifest.DuplicateDependency` | The same id appears twice in `dependencies`. |
| `Manifest.InvalidContentRoot` | An absolute path, a `..` escape, or a malformed mount point. |
| `Manifest.InvalidEntryPoint` | `entryPoint.class` is not a valid object path. |
| `Manifest.InvalidPermission` | A permission name is empty or contains whitespace. |
| `Manifest.InvalidClaim` | A claim is missing its extension point or resource. |

Warnings — the mod still loads:

| Code | Meaning |
|---|---|
| `Manifest.UnknownField` | A field this framework version does not recognise. Usually a typo. |
| `Manifest.EmptyContent` | No content roots and no entry point — the mod does nothing. |

Validate without launching the game:

```bash
Mod.Validate
```

from the console, or via **Window → Mod Developer → Validation** in the editor.

## Round-tripping

`FModManifestParser::SerializeToString` round-trips: parsing a serialised manifest reproduces every
field. Fields sitting at their default are omitted, so a hand-authored manifest stays small and a
generated one stays readable.

## See also

- [Versioning.md](Versioning.md) — the five versions and range syntax
- [Permissions.md](Permissions.md) — what each permission allows
- [Multiplayer.md](Multiplayer.md) — how manifests are matched between server and client
- [Packaging.md](Packaging.md) — turning a mod folder into a distributable `.mod`
