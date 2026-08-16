# ModAuthorSample

**The mod author's side of the boundary.** This project enables **one plugin** — `GameModSDK` — and
depends on **one module**. No game source, no game content, no access to `ModFrameworkSample`.

Look at [`ModAuthorSample.uproject`](ModAuthorSample.uproject) and
[`Build.cs`](Source/ModAuthorSample/ModAuthorSample.Build.cs): neither one names `ModFramework`.
That is deliberate and verified by a clean build. The framework is present and loaded — it has to be,
since `UGameWeaponExtension` derives from `UModExtension` and a mod's entry point Blueprint derives
from `UModEntryPointBase` — but `GameModSDK.uplugin` declares it as a plugin dependency, so **UE
enables it automatically**.

A mod author installs the SDK. They never install, choose, or name the framework. If either of those
files ever needs `ModFramework` spelled out, the SDK's descriptor is wrong.

That constraint is the entire point. Every other build in this repository has the game present, so
an accidental leak — an SDK header including a game header, an SDK type whose signature names a game
type, a `Build.cs` dependency that crept in — compiles fine there and stays invisible until a mod
author hits it. Here it fails immediately:

```powershell
./Tools/check-sdk-boundary.ps1
```

> If that fails, **do not** fix it by adding a dependency to this project. The failure is telling you
> the SDK needs something it should be exposing through a `UModAPI` instead. Making this project
> build for the wrong reason destroys the only evidence that the SDK is shippable.

## Setup

From the repository root:

```powershell
./Setup.ps1
```

Then right-click `ModAuthorSample.uproject` → **Generate Visual Studio project files**, and open it.

> **This project is a stand-in for the real thing.** `Setup.ps1` junctions both plugin folders in
> from this repository's source tree, because that is how you develop the framework and the SDK
> together. A real mod author instead extracts one SDK bundle:
>
> ```
> YourGameModSDK-1.5.0/
> └── Plugins/
>     ├── ModFramework/       ← present, never mentioned
>     └── YourGameModSDK/     ← the one they enable
> ```
>
> Same two folders on disk, same single plugin enabled — the difference is only where the folders
> came from. That bundle is produced by **Generate Game Mod SDK**; see
> [docs/internal/Status.md](../../docs/internal/Status.md) for its current state.

> **You need C++ here only because this repository builds the plugins from source.** A real mod
> author receiving a generated SDK bundle gets precompiled binaries and can use a content-only
> project with no compiler at all. That is the intended experience — see
> [docs/Security.md](../../docs/Security.md) for why native code is treated separately.

## The workflow

```
     author                cook                 package              install
Content/BrutalCombat/  →  .pak  →  Mod/ + mod.json  →  BrutalCombat.mod  →  <Game>/Mods/
```

### 1. Author

Everything a mod does is built from the SDK's types, in Blueprint and Data Assets:

| To do this | Derive from / create |
|---|---|
| Run code when the mod loads or activates | Blueprint from `UModEntryPointBase` |
| Change a weapon's behaviour | Blueprint from `UGameWeaponExtension` |
| Add or modify an enemy | Blueprint from `UGameEnemyExtension` + a `UGameEnemyDefinition` |
| Add a game rule | Blueprint from `UGameRuleExtension` |
| Add HUD content | Blueprint from `UGameUIExtension` |
| Declare what the mod registers | `UModContentBundle` Data Asset |

Author them under `Content/BrutalCombat/`. The folder name matters: it becomes the mount point, and
`mod.json` references assets by `/BrutalCombat/...` paths.

Inside an entry point you get a `UModContext` — the *only* handle a mod receives. It is how you
request APIs, register extensions, subscribe to events, and read and write your own save data. If
something is not reachable through it, the game did not expose it.

```
Context → RequestAPI("game.combat", UGameCombatModAPI) → ApplyDamage / GetHealth / ...
Context → RegisterExtension(BP_MyEnemyExtension)
Context → SubscribeToEvent("Game.EnemyKilled")
```

### 2. Cook

Cook `Content/BrutalCombat/` into a pak. During development you can skip this entirely: set the
content root in `mod.json` to

```json
{ "path": "Content", "type": "LooseDirectory", "mountPoint": "/BrutalCombat/" }
```

and enable `bAllowLooseContentMounts` in the *game's* settings. Loose mounts are development-only —
a released mod ships a pak.

### 3. Package

`Mod/` is the folder that becomes the distributable: `mod.json`, `Icon.png`, and the cooked
`Content/BrutalCombat.pak`. Package it with:

```bash
UnrealEditor-Cmd.exe ModAuthorSample.uproject -run=PackageMod -mod=Mod -output=Dist
```

The packager validates the manifest first and refuses to produce a `.mod` that could not load — so a
missing field or a bad version range is caught here rather than by a player.

### 4. Install

Drop `BrutalCombat.mod` into the game's `Mods/` directory. Then, in the game's console:

```
Mod.List
Mod.Info com.modframework.example.brutalcombat
Mod.Activate com.modframework.example.brutalcombat
```

`Mod.List` shows discovery and state; if the mod is missing entirely, discovery never saw it. See
[docs/ConsoleCommands.md](../../docs/ConsoleCommands.md) for the diagnostic paths.

## The manifest

[`Mod/mod.json`](Mod/mod.json) is complete and valid. Two fields deserve attention:

- **`id` is forever.** It keys save data, dependency references, load order and multiplayer matching.
  Changing it creates a different mod that cannot read its predecessor's saves. The display name is
  free to change.
- **Pin the SDK, not the game.** `"sdk": { "version": "^0.1.0" }` with `"game": { "version": "*" }`
  means the mod survives every game patch that does not change the modding surface — which is almost
  all of them. See [docs/Versioning.md](../../docs/Versioning.md).

## Current state

The manifest, packaging path and boundary check are real and working. The Blueprint and Data Asset
content it references does not exist yet — `.uasset` files are binary and have to be authored in the
editor. Until then the mod is discovered and validated but fails at load with `EntryPointMissing`,
which is the correct behaviour and a useful thing to see.

## See also

- [ManifestFormat.md](../../docs/ManifestFormat.md) — every field, with diagnostics
- [Packaging.md](../../docs/Packaging.md) — the `.mod` container and a release checklist
- [Permissions.md](../../docs/Permissions.md) — what your mod is allowed to ask for
