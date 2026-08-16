# Versioning

The framework tracks **five independent versions**. Collapsing any two of them into one is the most
common way a modding API ends up permanently frozen, so they are kept separate on purpose.

| Version | Owner | Changes when | A mod declares it as |
|---|---|---|---|
| **UE Version** | Epic | The game upgrades engine | implicit — mods must build against the same engine |
| **Game Version** | Game developer | Every game patch | `game.version` |
| **Framework Version** | This repository | ModFramework releases | `framework.version` |
| **Game SDK Version** | Game developer | Every published SDK | `sdk.version` |
| **Game API Version** | Game developer | Per individual `UModAPI` | requested at `RequestAPI` time |

## Why five and not one

A game ships patches constantly. Almost none of them change the modding surface. If mods declared
compatibility against the **game** version, every hotfix would break every mod, and the game
developer would face a choice between never patching and constantly invalidating the ecosystem.

So the rule is: **prefer API-version compatibility over game-version compatibility.**

```
Game 1.5.0 → 1.5.1 → 1.5.2      SDK stays 1.5.0, APIs stay 1.0.0
                                 → every mod keeps working, no author action
Game 1.5.2 → 1.6.0              SDK 1.6.0, gameplay API 1.1.0 (additive)
                                 → mods requesting "^1.0.0" keep working
Game 1.6.0 → 2.0.0              SDK 2.0.0, gameplay API 2.0.0 (breaking)
                                 → mods requesting "^1.0.0" are rejected before loading,
                                   with a message naming the API and both versions
```

A mod that wants to be maximally durable pins the **API** it uses and leaves `game.version` as `*`.

## Semantic versioning

All five use [Semantic Versioning 2.0.0](https://semver.org). `MAJOR.MINOR.PATCH`, optional
`-prerelease` and `+build`. Build metadata is parsed, preserved, and **ignored for ordering**.

Pre-release ordering follows the spec exactly: numeric identifiers compare numerically and rank
below alphanumeric ones, more identifiers beat fewer when all preceding are equal, and
`1.0.0-rc.1 < 1.0.0`.

For an API, the contract is:

- **PATCH** — internal fix, no signature change. Never breaks a mod.
- **MINOR** — additive only. New functions, new optional parameters, new enum values appended.
  Existing mods keep working.
- **MAJOR** — anything a mod could observe breaking: removed or renamed symbols, changed parameter
  types or order, changed return semantics, reordered enum values, or a behavioural change a mod
  could reasonably depend on.

Reordering an existing `UENUM`'s values is a **MAJOR** change, because Blueprint assets serialise
enum values by name but native code and data tables may not. Append new values; never insert.

## Version ranges

Every compatibility field takes a range expression, not a bare version. The grammar is a documented
subset of npm's, chosen because it is the syntax most mod authors already know.

| Expression | Matches |
|---|---|
| `*`, `any`, or omitted | everything |
| `1.2.3` | exactly 1.2.3 |
| `>=1.2.3`, `>1.2`, `<2.0.0`, `<=2`, `!=1.5.0` | comparison |
| `^1.2.3` | `>=1.2.3 <2.0.0` — "compatible with 1.2.3" |
| `^0.2.3` | `>=0.2.3 <0.3.0` — below 1.0.0, MINOR is the breaking axis |
| `^0.0.3` | `>=0.0.3 <0.0.4` |
| `~1.2.3` | `>=1.2.3 <1.3.0` — patch-level only |
| `1.2.x`, `1.x`, `1` | partial wildcard |
| `1.2.3 - 2.0.0` | inclusive hyphen range |
| `>=1.2.3 <2.0.0` | space or comma separated comparators are **AND**ed |
| `^1.0.0 \|\| ^2.0.0` | `\|\|` separated clauses are **OR**ed |

### The pre-release rule

A pre-release version satisfies a comparator **only** when some comparator in the same clause names
a version with the identical `major.minor.patch` *and* a pre-release of its own.

```
2.0.0-rc.1  vs  ">=1.0.0"          → NO   (an untested rc must not silently satisfy a stable range)
2.0.0-rc.1  vs  ">=2.0.0-rc.1"     → YES
2.0.0-rc.1  vs  ">=1.0.0 <3.0.0"   → NO
1.2.3       vs  ">=1.2.3-alpha"    → YES  (a release outranks its own pre-releases)
```

This is npm's rule, and it exists so that opting into pre-releases is always deliberate.

## Where each version comes from

- **Framework** — compiled in. `ModFrameworkVersion::Get()`, backed by the
  `MODFRAMEWORK_VERSION_*` macros in `Core/ModFrameworkVersion.h`.
- **Game and SDK** — project settings, under **Project Settings → Plugins → Mod Framework**
  (`UModFrameworkSettings::GameId` / `GameVersion` / `SdkId` / `SdkVersion`).
- **API** — per `UModAPI` subclass, from the `ModApiVersion` class metadata, with a
  `NativeGetApiId`/version override available for shipping builds where metadata is stripped.

## Rejection happens before loading

Incompatible mods are rejected during dependency resolution — before any content is mounted and
before any mod code runs. A rejected mod cannot have partially applied itself. The rejection carries
the reason, both versions, and the range that failed, so the message a player sees names the actual
problem:

```
Mod 'Better Combat' (com.example.bettercombat) requires SDK com.example.game.sdk ^1.5.0,
but the installed SDK is 2.0.1.
```

## Two separate version numbers in this repository

`ModFramework` and `GameModSDK` version independently, and both are separate from any game.

- `ModFramework/ModFramework.uplugin` → `VersionName` is the **Framework Version**.
- `GameModSDK/GameModSDK.uplugin` → `VersionName` is the **SDK Version** of the reference SDK.

A generated SDK bundle stamps its own `SDKVersion.json` with all five values it was built against,
so a mod author can always tell what they are targeting.

## See also

- [ManifestFormat.md](ManifestFormat.md) — where a mod declares each of these
- [../CHANGELOG.md](../CHANGELOG.md) — tracks the Framework Version only
