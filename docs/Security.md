# Security Model

Read this before shipping a game with mod support enabled.

## The one claim this framework will not make

**Blueprint and content-only mods are not a security sandbox.**

It is tempting to reason that because a mod ships Blueprints and Data Assets rather than a DLL, it
is inherently safe. That is false, and building a permission system on top of that assumption
produces security theatre — a UI that tells players they are protected when they are not.

Blueprint executes in-process with full engine privileges. A Blueprint can reach the file system,
spawn actors, modify game state, call most Blueprint-exposed engine functions, and consume unbounded
memory and CPU. The permission system in this framework is an **API access-control and
capability-declaration mechanism**, not a process boundary. It answers "may this mod call the API we
built for mods?" It does not and cannot answer "can this mod harm the player's machine?"

Real isolation requires process or VM boundaries, which UE does not provide for content plugins.

What the permission system genuinely buys you:

- **Intent declaration.** A mod that never requests `network` and then cannot use the networking API
  is a meaningful signal, and the manifest gives players and curators something to inspect.
- **Blast-radius reduction for honest mods.** Most breakage is accidental, not malicious. Denying
  `save.modify` to a mod that never needed it turns a class of bugs into a clean refusal.
- **An audit trail.** Every grant, denial and refused call is logged with the mod id.

What it does not buy you: protection from a mod author who is actively hostile.

## Therefore

**Distribution and curation are your real security boundary, not the framework.** Whatever provider
you ship — Workshop, a CDN, a curated list — is where trust is established. Treat
`IModProvider` implementations as security-relevant code.

State this honestly in your own player-facing UI. "Mods run with the same privileges as the game.
Only install mods from sources you trust" is accurate. "Mods are sandboxed" is not.

## Trust levels

| Content type | Trust | Notes |
|---|---|---|
| Data Assets, Data Tables | Low risk | Data only. Still validate ranges — a weapon with `damage = 1e38` is a bug surface. |
| Materials, textures, audio, UI | Low risk | Can degrade performance; cannot execute logic. |
| Blueprint classes | **In-process, engine-privileged** | Not sandboxed. Permission-gated only at the API layer. |
| Native C++ modules | **Fully trusted** | Equivalent to shipping your own code. See below. |

## Native code is fully trusted

Loading a third-party DLL means executing arbitrary code with the game's privileges. There is no
partial version of this. Consequently, in this framework:

- `native_code` is a **dangerous** permission and is never auto-granted.
- It is in `AlwaysDeniedPermissions` by default.
- The MVP does **not** load native mod modules at all — `entryPoint.nativeModule` is parsed and
  validated but not acted on.

Before ever enabling it, you need answers to: ABI compatibility across engine patch versions, code
signing and verification, per-platform loader behaviour, what happens on a mod crash, and how you
revoke a malicious mod already in the wild. Those are distribution and platform problems, not
framework problems, which is why the framework does not pretend to solve them.

## Untrusted input, handled as untrusted

Everything arriving from a mod is hostile until validated. The framework's own code follows these
rules and yours should too:

- **Never `check()` on mod data.** An assert on a malformed manifest turns a bad mod into a crash.
  Validation returns `FModDiagnostic`s; the mod is rejected, the game keeps running.
- **Path containment.** Content roots are resolved to absolute paths and verified to be inside the
  mod root. Absolute paths, `..` segments, drive letters and reserved device names are rejected
  (`Content.PathEscape`, `Package.UnsafePath`).
- **Bounds before allocation.** The `.mod` reader validates every offset and size against the real
  file length, caps entry count and total uncompressed size, and verifies SHA-1 per entry *before*
  allocating. A hostile package cannot induce a huge allocation by lying in its header.
- **Extraction containment.** `ExtractAll` re-verifies each destination stays under the target
  directory after normalisation, so a crafted entry cannot write outside it.
- **Network input is hostile.** A joining client's session manifest is attacker-controlled. It is
  length-capped and fully validated before decode, and the **server is authoritative** — the client's
  claims about its own mods are checked against the server's, never trusted.

## Server authority

In multiplayer, client-side content is never authoritative.

- APIs marked `ModApiServerAuthoritative` refuse calls when the world is `NM_Client`, and log the
  offending mod and function.
- Extension points can be marked `bServerAuthoritative` with the same effect.
- Events with `bServerAuthoritative` cannot be broadcast from a client.

Deciding which of *your* APIs are server-authoritative is the SDK author's job, and it is the single
most consequential security decision you will make. The default should be: if it changes gameplay
state, it is server-authoritative.

## Permissions reference

Dangerous permissions are never auto-granted, even if a project config lists them in
`AutoGrantedPermissions` — the framework logs a warning and ignores the attempt.

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

Default policy with no custom policy set: `AlwaysDenied` → Denied; unregistered permission → Denied
(when `bDenyUnknownPermissions`); auto-granted and not dangerous → Granted; everything else →
**Pending**, which raises `OnPermissionRequested` so you can drive a player prompt.

Implement `IModPermissionPolicy` to take full control.

## Save data

Removing a mod must never corrupt unrelated save data. Records for absent mods are marked
`bOrphaned` and preserved byte-for-byte across save/load cycles; only an explicit
`PurgeOrphanedRecord` call deletes one. Payloads are size-capped (16 MiB JSON, 64 MiB binary) so a
mod cannot bloat a player's save indefinitely.

## Reporting

This is a framework, not a hardened product. If you find a path traversal, an unbounded allocation,
or a way for a mod to reach an API it lacks permission for, open an issue — those are real bugs
regardless of the sandbox caveat above.

## See also

- [Permissions.md](Permissions.md) — the permission system in detail
- [Multiplayer.md](Multiplayer.md) — server authority and manifest validation
- [ManifestFormat.md](ManifestFormat.md) — what a mod declares
