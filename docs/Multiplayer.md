# Multiplayer

Mod compatibility is designed in from the start rather than bolted on, because retrofitting it means
changing the manifest format after mods already exist.

Two rules drive everything here:

1. **The server is authoritative.** A client's claims about its own mods are input to validate, never
   facts to trust.
2. **Reject at connect time, not at play time.** A mismatch discovered mid-session is a desync; the
   same mismatch discovered during login is an error message.

## Scopes

Each mod declares how it participates:

```json
"network": { "scope": "ClientAndServer", "requiredForNetworkPlay": true }
```

| Scope | Meaning |
|---|---|
| `ClientAndServer` | Both sides need it. The default. |
| `ClientOnly` | Purely local — a HUD tweak, a texture pack. The server neither needs nor checks it. |
| `ServerOnly` | Server-side rules. A client presenting one is a **scope violation**. |

`requiredForNetworkPlay` promotes a mod to mandatory: a server running it refuses clients that lack
it at the same version.

## The session manifest

`UModSubsystem::BuildSessionManifest()` produces an `FModSessionManifest` — game id and version,
framework version, SDK id and version, and one entry per mod with its id, version, scope, required
flag and content hash.

It travels as a Base64 login option:

```
127.0.0.1:7777?ModManifest=<base64>
```

`UModNetworkStatics::AppendModManifestToTravelURL` builds it client-side;
`ParseSessionManifestFromOptions` reads it server-side.

## Server-side validation

Call this from `AGameModeBase::PreLogin`:

```cpp
void AMyGameMode::PreLogin(const FString& Options, const FString& Address,
	const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	if (!ErrorMessage.IsEmpty())
	{
		return;
	}

	FString ModError;
	if (!UModNetworkStatics::ValidateJoiningPlayer(this, Options, ModError))
	{
		ErrorMessage = ModError;   // non-empty ErrorMessage rejects the connection
	}
}
```

Checks applied, in order:

| Check | Mismatch |
|---|---|
| Game id differs | `GameMismatch` |
| Game major version differs | `GameMismatch` |
| Framework major version differs | `FrameworkMismatch` |
| SDK id differs, or SDK major differs | `SdkMismatch` |
| Server has a required non-`ServerOnly` mod the client lacks | `MissingOnClient` |
| Both have it at different versions | `VersionMismatch` |
| Content hashes differ (when `bVerifyContentHashes`) | `ContentHashMismatch` |
| Client presents a `ServerOnly` mod | `ScopeViolation` |
| Client has a required non-`ClientOnly` mod the server lacks | `MissingOnServer` |

`ClientOnly` mods on the client are always fine — that is the point of the scope.

Required mods must match **exactly**, not by range. Two clients running "compatible" versions of a
gameplay mod can still simulate differently, and a version range cannot express whether a patch
release changed simulation behaviour.

## Error messages players can act on

`FModNetworkValidationResult::BuildUserFacingMessage()` names the actual problem:

```
Cannot join this server.

Server requires: BetterCombat 2.1.0
You have:        BetterCombat 1.8.0

Reason: Incompatible required mod version.
```

Up to eight mismatches are listed, then `(+N more)`. Surface this text directly — "connection
failed" tells a player nothing and generates a support request.

## Content hashes

The content hash detects a mod whose id and version match but whose *content* does not — a
mid-release repack, a partial download, or a tampered pak. Enable with
`UModFrameworkSettings::bVerifyContentHashes`. Hashes are compared only when both sides supply a
non-empty one, so a provider that cannot compute them degrades to id-and-version matching rather than
failing everyone.

## Server authority in the API layer

Manifest matching gets the right mods loaded. It does not stop a loaded mod from trying to drive
gameplay from a client. That is the API layer's job:

- `UModAPI::IsServerAuthoritative()` — the API refuses calls when `GetNetMode() == NM_Client`, and
  logs the mod and function.
- `FModExtensionPointDescriptor::bServerAuthoritative` — same for extensions.
- `FModEventDescriptor::bServerAuthoritative` — clients cannot broadcast it.

**The SDK decides which APIs are server-authoritative, and this is the most consequential security
decision in an SDK.** Default to: if it changes gameplay state, it is server-authoritative. A mod
that needs a client-side prediction hook should get a separate, explicitly client-safe API rather
than a relaxed authoritative one.

## Listen servers and single-player

`GetNetMode()` returns `NM_Standalone` in single-player and `NM_ListenServer` on a listen server —
neither is `NM_Client`, so authoritative APIs work normally. No special-casing needed.

## Dedicated servers

A dedicated server discovers and loads mods the same way. Use `ServerOnly` scope for server-side
rules so clients are never asked to install them, and `ModSearchDirectories` to point at wherever
your server deployment stages mods.

## Inspecting

```
Mod.SessionManifest
```

prints the local session manifest with the digest, which is the fastest way to see what the server
would advertise or what a client would send.

## See also

- [Security.md](Security.md) — why client content is never authoritative
- [ManifestFormat.md](ManifestFormat.md) — the `network` block
- [Versioning.md](Versioning.md) — why exact matching, not ranges, for required mods
