# Scripting

Scripts are the third way to write a mod, alongside Blueprint and Data Assets — and the only one
where the permission system is genuinely **enforced** rather than declared.

> **Status:** the runtime is being implemented. `IModScriptRuntime` is the seam; **Lua 5.5** is the
> first implementation, in the `ModFrameworkLua` module. A build without the Lua sources present
> still compiles and reports scripting unavailable. See
> [internal/Status.md](internal/Status.md) for exactly what is done.

## Why scripts exist when Blueprint already works

Three things Blueprint cannot give you:

1. **A real capability boundary.** Blueprint runs in-process with engine-wide reach, so
   [Security.md](Security.md) is explicit that permissions are API access control, not a sandbox. A
   script VM only sees what was bound into it — so "this mod may not touch the filesystem" is true
   rather than aspirational.
2. **Engine independence.** A cooked Blueprint is tied to an engine version; a studio that forks the
   engine may owe mod authors a custom editor. A script is text and survives all of that.
3. **Text.** Scripts diff, review and patch. A curator or a player can read exactly what a mod does
   before installing it — a property no other mod content has.

## The shape of a script mod

```
BrutalCombat/
├── mod.json
└── Scripts/
    └── main.lua
```

```json
"entryPoint": {
	"scriptRuntime": "lua",
	"scripts": ["Scripts/main.lua"]
}
```

Scripts load **after** the mod's context exists and **before** its Blueprint entry point runs, in
the order listed — so a Blueprint entry point can rely on state a script set up. Combining scripts
with Blueprint extensions is normal, not exceptional.

## What a script can reach

Everything is on one global table, `mod`. That is the whole surface.

| | |
|---|---|
| `mod.id()` | this mod's id |
| `mod.log(s)` `mod.warn(s)` `mod.error(s)` | attributed to the mod in the log |
| `mod.config_get(key, default)` | default's type decides how it is read |
| `mod.config_set(key, value)` · `mod.config_save()` | |
| `mod.save(json)` · `mod.load()` | the mod's own save namespace |
| `mod.has_permission(name)` | |
| `mod.request_api(id, range)` | returns the API, or nil plus a reason |
| `mod.subscribe(event, fn)` · `mod.unsubscribe(handle)` | |
| `mod.broadcast(event)` | |

**What is deliberately absent:** `io`, `os.execute`, `os.remove`, `os.getenv`, `require`, `loadfile`,
`dofile`, `load`, and any route to `UObject`, reflection or engine globals. `print` is rebound to the
mod's own log so output is attributable instead of vanishing.

Precompiled bytecode is refused — scripts load in text mode only. Bytecode bypasses the parser and
is a documented way to crash Lua deliberately.

## Lifecycle functions

Define any of these as globals; all are optional.

```lua
function OnModActivated()   end
function OnModDeactivated() end
```

## Resource limits

An API boundary is not a resource boundary. A script that allocates forever or loops forever needs
stopping regardless of what it is allowed to call, so the runtime enforces:

- a **memory budget** through a custom allocator, which turns exhaustion into a clean Lua error
- an **instruction budget** per call via a count hook, so a runaway loop is aborted rather than
  hanging the game

Budgets reset per entry point, so a mod doing legitimate work every frame is not killed by
cumulative drift.

## Worked example

[`Templates/ModAuthorSample/Mod/Scripts/main.lua`](../Templates/ModAuthorSample/Mod/Scripts/main.lua)
is a complete, commented mod script covering config with defaults, requesting an API by version
range, permission checks, event subscription, save data, and cleaning up on deactivate.

## For game developers

You are not obliged to ship scripting. Enabling it means:

1. Enabling the `ModFrameworkLua` module (or implementing `IModScriptRuntime` yourself)
2. Deciding your budgets
3. Accepting that the bindings file is your sandbox boundary — anything added there is reachable by
   untrusted code

Adding a second runtime later breaks nothing: runtimes register rather than being a singleton, and a
manifest names the one it wants.

## See also

- [Security.md](Security.md) — where the trust boundaries actually are
- [ManifestFormat.md](ManifestFormat.md) — the `entryPoint` fields
- [Permissions.md](Permissions.md) — what a mod may request
