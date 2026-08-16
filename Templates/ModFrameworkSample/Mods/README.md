# Mods directory

This is a **mod discovery directory**. `UModFrameworkSettings::ModSearchDirectories` defaults to
`{ProjectSaved}/Mods` and `{Project}/Mods`, so anything dropped here is found automatically when the
sample game starts — no configuration, no command line switch.

A folder is treated as a mod when it contains a `mod.json` at its root. Loose `.mod` package files
placed directly in this directory are also discovered; their manifest is read from inside the
package without extracting it.

```
Mods/
├── ExampleMod/
│   ├── mod.json          ← makes this folder a mod
│   └── Content/          ← Blueprints and Data Assets, mounted at /BrutalCombat/
└── SomethingElse.mod     ← packaged mod, discovered without extraction
```

## About ExampleMod

`ExampleMod/mod.json` is complete and parses today, but the mod is **not yet loadable** — its
`entryPoint.class` and `contentBundles` reference Blueprint and Data Assets that have not been
authored yet. Until they exist, the mod is discovered and validated but fails at load with
`EntryPointMissing`, which is the correct behaviour and a useful thing to see.

It uses `"type": "LooseDirectory"` for its content root, which is the development path: it requires
`UModFrameworkSettings::bAllowLooseContentMounts` to be enabled and is not how a mod ships. A
released mod cooks its content to a pak and switches to `"type": "Pak"` — see
[Packaging.md](../../../docs/Packaging.md).

## Verifying discovery

From the in-game console:

```
Mod.List
Mod.Info com.modframework.example.brutalcombat
Mod.Validate
```

`Mod.List` shows every discovered mod and its state; `Mod.Validate` re-runs manifest validation and
prints the diagnostics without loading anything.

## See also

- [ManifestFormat.md](../../../docs/ManifestFormat.md) — every field in `mod.json`
- [Versioning.md](../../../docs/Versioning.md) — the compatibility ranges
