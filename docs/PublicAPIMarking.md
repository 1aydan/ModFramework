# Marking a Public API

SDK generation does not export your project. It exports **only what you explicitly mark**, using
reflection metadata. Anything unmarked stays private by default, so forgetting to mark something
produces a missing API — never a leak.

## The metadata

| Metadata | Applies to | Meaning |
|---|---|---|
| `ModPublic` | class, struct, enum, interface, function, property | Include in the generated SDK |
| `ModApiId` | `UModAPI` subclass | Stable API id, e.g. `"game.gameplay"` |
| `ModApiVersion` | `UModAPI` subclass | Semver of the API |
| `ModApiPermissions` | `UModAPI` subclass | Comma-separated permission ids required to obtain it |
| `ModApiServerAuthoritative` | `UModAPI` subclass | `"true"` refuses calls on clients |
| `ModExtensionPoint` | `UModExtension` subclass | The extension point id this base class serves |
| `ModSince` | anything | SDK version the symbol appeared in |
| `ModDeprecated` | anything | SDK version the symbol was deprecated in |

## Example

```cpp
UCLASS(BlueprintType, meta = (
	ModPublic,
	ModApiId = "game.combat",
	ModApiVersion = "1.2.0",
	ModApiPermissions = "gameplay.modify",
	ModApiServerAuthoritative = "true"))
class MYGAMEMODSDK_API UMyGameCombatModAPI : public UModAPI
{
	GENERATED_BODY()

public:
	/** Applies damage through the game's own damage pipeline. */
	UFUNCTION(BlueprintCallable, Category = "Combat", meta = (ModPublic, ModSince = "1.0.0"))
	bool ApplyDamage(AActor* Target, float Amount, FName DamageType);

	UFUNCTION(BlueprintCallable, Category = "Combat", meta = (
		ModPublic, ModSince = "1.0.0", ModDeprecated = "1.2.0"))
	bool DealDamage(AActor* Target, float Amount);   // superseded by ApplyDamage
};
```

Marking a class does **not** mark its members. Each function and property carries its own
`ModPublic`. That is deliberate: a class often needs to be visible so mods can hold a pointer to it,
while most of its surface stays internal.

## A shipping-build hazard

`UCLASS` metadata is editor-only. In a cooked shipping build it is stripped, so
`GetClass()->GetMetaData(TEXT("ModApiId"))` returns nothing and an API would fall back to a
name-derived id — a *silent* id change between editor and shipping, which is the worst kind of bug.

For any API you actually ship, override the native accessors as well:

```cpp
virtual FName GetApiId() const override { return TEXT("game.combat"); }
virtual FModVersion GetApiVersion() const override { return FModVersion(1, 2, 0); }
```

The metadata then drives SDK **generation** (an editor-time operation, where it is always present),
and the overrides drive **runtime** identity. Keep the two in agreement; the SDK generator warns
when they disagree.

## What the generator emits

For each `ModPublic` symbol:

- The public header, with private members and unmarked functions stripped
- Blueprint-visible types preserved with their `UFUNCTION`/`UPROPERTY` metadata intact
- `ModSince` / `ModDeprecated` rendered as documentation and `UE_DEPRECATED` where applicable
- An API index in `SDKVersion.json` — every API id, version, permission and authority flag

It does not emit implementation. The generated SDK declares the surface; the game supplies the
behaviour at runtime by registering a `UModAPI` instance.

## Guidelines

**Mark the minimum.** Every marked symbol is a compatibility commitment. Removing one later is a
MAJOR version bump for the whole SDK.

**Never mark a type that exposes internals.** If a struct has a pointer to a game-internal class,
marking it drags that class into the SDK or breaks generation. Design a flat, mod-facing struct
instead.

**Prefer functions over properties.** A marked `UPROPERTY` freezes a field layout; a marked
`UFUNCTION` lets you change the storage behind it.

**Version the API, not the game.** See [Versioning.md](Versioning.md).

## Auditing

```
Mod.DumpAPIs
```

lists every registered API with its id, version, class, authority flag and required permissions.
In the editor, **Window → Mod Developer → API Inspector** additionally shows unmarked public symbols
that look like they were *meant* to be marked — a `UModAPI` subclass with no `ModApiId`, a
`BlueprintCallable` function on a marked class with no `ModPublic`.

## See also

- [Versioning.md](Versioning.md) — what MINOR vs MAJOR means for an API
- [Permissions.md](Permissions.md) — gating an API with `ModApiPermissions`
- [Multiplayer.md](Multiplayer.md) — choosing server authority
