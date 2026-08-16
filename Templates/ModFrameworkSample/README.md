# ModFrameworkSample

The sample game. In this repository it plays the role of **the game developer's project** — the game
that installs `ModFramework`, defines its public modding surface in `GameModSDK`, generates an SDK,
and loads mods.

It is the stock UE 5.8 **Third Person (C++)** template with the Combat, Platforming and
SideScrolling variants. The Combat variant is what the SDK's extension points are built around:
`CombatCharacter`, `CombatEnemy`, `CombatEnemySpawner`, `CombatGameMode`, StateTree AI and
`AnimNotify_DoAttackTrace` give mods something real to extend.

## `Content/` is not in git

The template's content is ~135 MB of stock Epic assets — mannequins, animations, VFX, prototyping
meshes. None of it is authored by this project, and `.uasset` files don't delta-compress, so every
clone would pay for it forever. It's gitignored.

`Source/`, `Config/`, `Mods/` and the `.uproject` **are** tracked. So is the example mod's own
content — only this project's stock `Content/` is excluded.

### Restoring it

**Option A — download (preferred).** Grab `ModFrameworkSample-Content.zip` from the repository's
[Releases](https://github.com/1aydan/ModFramework/releases) page and extract it so that
`Templates/ModFrameworkSample/Content/` exists. This is the exact content this project was built
against.

**Option B — regenerate from the engine template.** Always works, no download needed:

1. In the Epic Launcher, create a new project: **Games → Third Person**, with **C++** selected,
   using **UE 5.8**.
2. Copy that project's `Content/` folder into `Templates/ModFrameworkSample/Content/`.
3. Delete the throwaway project.

The `.uproject`, `Source/` and `Config/` here already match what that template generates, so the
copied content lines up. If a future engine version changes the template, this project's `Source/`
is the authority — reconcile toward it.

> **Maintainers:** produce the release archive with `.dev/pack-sample-content.ps1`, then attach the
> result to a GitHub release. Re-cut it whenever the sample's content changes, and note the engine
> version in the release body — content cooked by a newer editor will not open in an older one.

## Setup

From the repository root:

```powershell
./Setup.ps1
```

That links `ModFramework` and `GameModSDK` into `Plugins/` here as directory junctions. It uses
junctions rather than the project's `AdditionalPluginDirectories` setting deliberately: that setting
is honoured only under `WITH_EDITOR`, so it works in the editor and then silently breaks the
packaged build — the exact case this framework exists to serve.

Then right-click `ModFrameworkSample.uproject` → **Generate Visual Studio project files**, and open
it.

## Mods

`Mods/` is a mod discovery directory — `{Project}/Mods` is one of the framework's default search
paths, so anything dropped there is found with no configuration. See [Mods/README.md](Mods/README.md).

## MCP

`.mcp.json` here points at the editor's built-in `ModelContextProtocol` server on
`http://127.0.0.1:8000/mcp`, which only runs while the editor is open. Note that a Claude Code
session reads `.mcp.json` from **its working directory** — if you're working from the repository
root, the copy there is the one that counts.
