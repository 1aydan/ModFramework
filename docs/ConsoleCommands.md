# Console Commands

Runtime diagnostics for mod loading. Every command writes to the `LogModFramework` category **and**
to the in-game console when one is available.

## Availability

Compiled in only when `!UE_BUILD_SHIPPING || ALLOW_CONSOLE`, and gated at execution time on
`UModFrameworkSettings::bEnableConsoleCommands` (default on).

The gate is checked inside each command rather than at registration, because the `ModFramework`
module loads at `PostConfigInit` — before the UObject system exists, so the settings CDO cannot be
read yet. Ship with `bEnableConsoleCommands = false` if you do not want players poking at mod state.

## Inspecting

| Command | Does |
|---|---|
| `Mod.List` | Every discovered mod: id, version, state, load order, provider. Start here. |
| `Mod.Info <ModId>` | Full detail for one mod — manifest, state, failure reason, granted permissions, mount points, diagnostics. |
| `Mod.Dependencies <ModId>` | That mod's dependency tree, resolved versions, and anything unsatisfied. |
| `Mod.Mounts` | Active content mounts: virtual mount point, physical path, owner, mount order. |
| `Mod.Conflicts` | The conflict report — contenders, policy, winner, blocking status. |
| `Mod.SessionManifest` | The local multiplayer session manifest and its digest. |
| `Mod.Validate` | Re-runs manifest validation across all discovered mods and prints diagnostics. Loads nothing. |

## Dumping registries

| Command | Does |
|---|---|
| `Mod.DumpRegistry` | Every registered mod with its full runtime record. |
| `Mod.DumpAPIs` | Registered `UModAPI`s: id, version, class, server-authoritative flag, required permissions. |
| `Mod.DumpExtensions` | Extension points and every extension registered against them, in resolved order. |
| `Mod.DumpEvents` | Registered event types, payload struct, and live subscriber counts. |
| `Mod.DumpPermissions` | Every mod's requested permissions and their resolved state. |

## Lifecycle

Loaded and activated are **separate** states, and these commands respect that — `Mod.Load` does not
activate.

| Command | Does |
|---|---|
| `Mod.Refresh` | Full cycle: discover → resolve → mount → load → activate, honouring settings. |
| `Mod.Load <ModId>` | Mount and load. Requires the mod to have resolved successfully. |
| `Mod.Activate <ModId>` | Activate a loaded mod. |
| `Mod.Deactivate <ModId>` | Deactivate without unloading. |
| `Mod.Unload <ModId>` | Deactivate if needed, release mod objects, unregister, unmount. |
| `Mod.Reload <ModId>` | Unload then load again. The main iteration loop while developing a mod. |
| `Mod.ReloadAll` | Reload every loaded mod in dependency order. |
| `Mod.Enable <ModId>` | Clear the disabled flag. Takes effect on the next refresh. |
| `Mod.Disable <ModId>` | Mark disabled so resolution skips it. |

## Typical sessions

**"My mod isn't showing up."**

```
Mod.List                  → is it discovered at all?
Mod.Validate              → manifest errors, with the exact field
Mod.Info <ModId>          → state and failure reason
```

Not in `Mod.List` at all means discovery never saw it — check that `mod.json` is at the folder root
and that the folder is in a search directory.

**"It loads but does nothing."**

```
Mod.Info <ModId>          → Loaded, or Activated?
Mod.DumpExtensions        → did its extensions register?
Mod.DumpPermissions       → is something Pending or Denied?
```

`Loaded` but not `Activated` is the most common cause, and the distinction is deliberate.

**"Two mods fight."**

```
Mod.Conflicts             → contenders, applied policy, winner
```

**"A client can't join."**

```
Mod.SessionManifest       → run on both ends and compare
```

## Iterating on a mod

With `bAllowLooseContentMounts` enabled, a loose content root plus `Mod.Reload` gives a fast loop
without repackaging. Note that unmounting content whose objects are still referenced is not fully
safe in UE — a full restart is the honest answer when reload behaves oddly.

## See also

- [ManifestFormat.md](ManifestFormat.md) — diagnostic codes `Mod.Validate` reports
- [Conflicts.md](Conflicts.md) — reading `Mod.Conflicts`
- [Multiplayer.md](Multiplayer.md) — session manifests
