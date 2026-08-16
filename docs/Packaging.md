# Packaging a Mod

A distributable mod is a single `.mod` file. The format is deliberately provider-agnostic — nothing
about it assumes Steam Workshop, or any other distribution channel.

## Layout of a mod, before packaging

```
BrutalCombat/
├── mod.json              the manifest — required, at the root
├── Icon.png              optional, read without mounting or extracting
├── Content/
│   └── BrutalCombat.pak  cooked content
├── Scripts/              optional — plain text, listed in entryPoint.scripts
│   └── main.lua
├── Config/               optional — shipped defaults, see Permissions/config
└── Localization/         optional
```

`Scripts/` is a convention, not a rule — the manifest names each script explicitly, so the folder can
be called anything. Keeping it conventional matters for a different reason: scripts are **text**, so
they diff, review and patch cleanly, and a curator or player can read exactly what a mod does before
installing it. That is a property no other mod content has.

## Cooking content

Mod content is cooked like any other UE content, into a pak mounted at the mod's virtual mount point.
During development you can skip this: set the content root to

```json
{ "path": "Content", "type": "LooseDirectory", "mountPoint": "/BrutalCombat/" }
```

and enable `UModFrameworkSettings::bAllowLooseContentMounts`. Loose mounts are a **development-only**
path — a released mod ships a pak.

## Creating the package

Editor: **Window → Mod Developer → Package**.

Commandlet, for CI:

```bash
UnrealEditor-Cmd.exe YourGame.uproject -run=PackageMod -mod=Path/To/BrutalCombat -output=Dist
```

Packaging validates the manifest first and refuses to produce a package that could not load.

## The `.mod` container

A self-describing container written with `FArchive`. No third-party dependencies, no zip library.

```
┌──────────────────────────────────────┐
│ Header (fixed size)                  │
│   Magic 'UMOD'  (0x444F4D55)         │
│   FormatVersion                      │
│   ManifestOffset / ManifestSize      │
│   TocOffset / TocSize                │
│   ContentHash (SHA-1 hex, 40 chars)  │
├──────────────────────────────────────┤
│ Manifest — the mod.json bytes, UTF-8 │
├──────────────────────────────────────┤
│ TOC — one entry per file:            │
│   RelativePath, Offset,              │
│   CompressedSize, UncompressedSize,  │
│   bCompressed, SHA-1                 │
├──────────────────────────────────────┤
│ Payloads (optionally zlib)           │
└──────────────────────────────────────┘
```

The manifest sits **before** the payloads on purpose: a mod browser can read a package's identity,
version, dependencies and icon without extracting anything, and a provider can list a directory of
`.mod` files cheaply. `FModPackageReader::PeekManifest` does exactly that.

## Reading is hostile-input handling

A `.mod` may come from anywhere, so the reader assumes it is adversarial:

- Magic and format version checked before anything else
- Every offset and size validated against the real file length **before** seeking or allocating
- Entry count and total uncompressed size capped before allocation, so a lying header cannot induce
  a huge allocation
- Entry paths rejected when absolute, containing `..`, a drive letter, a leading slash, a backslash,
  a NUL, or a reserved Windows device name
- `ExtractAll` re-verifies each destination stays under the target directory *after* normalisation
- SHA-1 verified per entry on extract and in `VerifyIntegrity`

Failures are diagnostics, never asserts: `Package.BadMagic`, `Package.UnsupportedVersion`,
`Package.Corrupt`, `Package.UnsafePath`, `Package.HashMismatch`, `Package.TooLarge`,
`Package.DecompressFailed`.

## Installing

Drop the `.mod` into a discovery directory — by default `{Project}/Mods` or `{ProjectSaved}/Mods`.
It is discovered without extraction; installation (extraction into `<InstallDir>/<modid>/`) happens
on demand, verifying integrity first and refusing to overwrite an existing install unless the
existing manifest has a lower version, in which case it replaces it atomically via a temp directory.

## Distribution

The framework never talks to a store. `IModProvider` is the seam:

```cpp
class FMyWorkshopProvider : public IModProvider
{
	virtual FName GetProviderId() const override { return TEXT("Workshop"); }
	virtual void DiscoverMods(TArray<FModDiscoveryResult>& OutResults) override;
	virtual bool AcquireMod(const FModId& ModId, FString& OutLocalRootPath,
		FModDiagnostic& OutError) override;
	// ...
};
```

Providers **acquire**; the framework **validates and loads**. That split is what keeps Steam,
a CDN, a dedicated-server push and a local folder interchangeable — and it means your provider is
security-relevant code, since curation is the real trust boundary
(see [Security.md](Security.md)).

`FLocalFileModProvider` ships with the framework and covers the local filesystem case.

## Checklist before releasing

- [ ] `mod.json` validates cleanly — `Mod.Validate` reports no errors
- [ ] Content root is `Pak`, not `LooseDirectory`
- [ ] `version` bumped; `id` **unchanged** from previous releases
- [ ] Compatibility ranges pin the **SDK**, not the game version
- [ ] `permissions` lists only what the mod actually uses
- [ ] `claims` declares everything the mod modifies
- [ ] `network.scope` and `requiredForNetworkPlay` are correct
- [ ] `Icon.png` present and under 2 MiB
- [ ] Installed from the `.mod` into a clean game and loads end to end

## See also

- [ManifestFormat.md](ManifestFormat.md) — every manifest field
- [ConsoleCommands.md](ConsoleCommands.md) — `Mod.Validate`, `Mod.Mounts`
- [Security.md](Security.md) — why curation matters more than the format
