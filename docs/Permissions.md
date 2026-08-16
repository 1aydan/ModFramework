# Permissions

A mod **requests** permissions in its manifest. The game **decides**. Requesting is not granting, and
nothing is granted implicitly.

> Before relying on this system, read [Security.md](Security.md). Permissions are API access control,
> not a sandbox. They constrain what a mod can do through the framework's APIs; they do not constrain
> what Blueprint can do to the process.

## Declaring

```json
"permissions": ["gameplay.modify", "save.modify"]
```

Request the minimum. A mod asking for `filesystem.write` and `network` to reskin a sword will be
denied by any sensible policy, and players notice.

## States

| State | Meaning |
|---|---|
| `NotRequested` | The mod never asked. `HasPermission` is false. |
| `Pending` | Asked, undecided. The game should prompt. **Not** granted. |
| `Granted` | Allowed. |
| `Denied` | Refused. Calls that need it fail with `API.PermissionDenied`. |

Only `Granted` passes `HasPermission`. `Pending` deliberately does not — an undecided permission
behaves as denied until someone decides, so a game that never implements a prompt fails closed.

## Built-in permissions

| Permission | Dangerous | Grants |
|---|---|---|
| `assets.read` | no | Read the mod's own and public game assets |
| `gameplay.modify` | no | Call gameplay-modifying game APIs |
| `save.modify` | no | Write to the mod's own save namespace |
| `ui.modify` | no | Register UI extensions |
| `assets.write` | **yes** | Modify assets |
| `console` | **yes** | Execute console commands |
| `filesystem.read` | **yes** | Read outside the mod root |
| `filesystem.write` | **yes** | Write outside the mod root |
| `network` | **yes** | Make network requests |
| `native_code` | **yes** | Load a native module |

**Dangerous permissions are never auto-granted.** Listing one in `AutoGrantedPermissions` does not
work — the framework logs a warning and ignores it. That is deliberate: the config exists to reduce
prompt fatigue for harmless capabilities, not to disable the system.

## Default policy

With no custom policy installed, each requested permission resolves in this order:

1. In `AlwaysDeniedPermissions` → **Denied**
2. Not a registered permission → **Denied** if `bDenyUnknownPermissions` (the default), else
   **Pending**, with a warning naming the mod and the permission
3. In `AutoGrantedPermissions` **and** not dangerous → **Granted**
4. Otherwise → **Pending**, raising `OnPermissionRequested`

Project defaults: `AutoGrantedPermissions = ["assets.read"]`,
`AlwaysDeniedPermissions = ["native_code"]`.

## Custom policy

Implement `IModPermissionPolicy` to decide for yourself — from a curated allowlist, a player setting,
a trust level attached to the provider the mod came from, or a prompt.

```cpp
EModPermissionState UMyPolicy::ResolvePermission_Implementation(
	const FModManifest& Manifest, FName PermissionId)
{
	// Mods from the curated catalogue get the safe set without asking.
	if (IsCurated(Manifest.Id) && !IsDangerous(PermissionId))
	{
		return EModPermissionState::Granted;
	}
	return EModPermissionState::Pending;   // fall through to a player prompt
}
```

Install it with `UModPermissionRegistry::SetPolicy`. Returning anything other than `NotRequested`
overrides the default chain entirely.

## Prompting the player

`OnPermissionRequested` fires for every permission left `Pending`. Drive UI from it and resolve with
`GrantPermission` / `DenyPermission`. A mod stays loadable while decisions are outstanding — its
permission-gated calls simply fail until then, so you can prompt lazily rather than blocking startup.

`GrantAll` exists for development and logs a loud `SECURITY` error naming the mod. Do not ship a code
path that calls it.

## Registering your own

Permissions are not a closed set. A game with a meaningful capability of its own registers it:

```cpp
FModPermissionDescriptor Descriptor;
Descriptor.PermissionId = TEXT("game.economy.modify");
Descriptor.DisplayName  = NSLOCTEXT("MyGame", "EconomyPerm", "Modify the economy");
Descriptor.Description  = NSLOCTEXT("MyGame", "EconomyPermDesc",
	"Lets a mod change prices, currency and trade rules.");
Descriptor.bDangerous   = true;
PermissionRegistry->RegisterPermission(Descriptor);
```

Register during game startup, **before** mods are discovered — otherwise a mod requesting it hits the
unknown-permission path and is denied.

Then gate the API with it, either declaratively:

```cpp
UCLASS(meta = (ModPublic, ModApiId = "game.economy", ModApiPermissions = "game.economy.modify"))
class UMyEconomyModAPI : public UModAPI { ... };
```

or per call:

```cpp
if (!PermissionRegistry->HasPermission(CallingMod, TEXT("game.economy.modify")))
{
	return false;
}
```

Declarative gating is checked once at `RequestAPI` time and is the right default. Per-call checks are
for when a single API mixes capabilities of different sensitivity — but prefer splitting the API.

## Inspecting

```
Mod.DumpPermissions
```

lists every mod with each requested permission and its state. In the editor, **Window → Mod
Developer → Permissions** shows the same with the deciding rule for each.

## See also

- [Security.md](Security.md) — what this system does and does not protect against
- [ManifestFormat.md](ManifestFormat.md) — the `permissions` field
