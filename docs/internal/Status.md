# Project Status & Handoff

Internal working document. Read this first when resuming work with no prior conversation context.

**Last updated:** 2026-08-16 — core runtime written and restructured, first compile pass done
(2 real bugs found and fixed), integration workflow in flight. Nothing committed to git yet.

---

## How to resume

1. Read [ImplementationContract.md](ImplementationContract.md) — the binding spec every file was
   written against. Signatures, file map, algorithms, JSON schema, diagnostic codes.
2. Read the **Decisions** section below — several are non-obvious and were argued through once
   already. Do not re-litigate them.
3. Check **Next steps** for where the work stopped.

Engine: **UE 5.8.1** at `F:\SelfProjects\Unreal\UE_5.8`. Grep its source to verify any engine API
before using it — that habit caught several version-drift bugs already (see Gotchas).

---

## Repository

```
Plugins/ModFramework/              repo root, git origin -> github.com/1aydan/ModFramework (private)
├── ModFramework/                  the framework plugin
│   └── Source/{ModFramework, ModFrameworkDeveloper, ModFrameworkEditor}
├── GameModSDK/                    the reference SDK plugin (scaffolded, no content yet)
├── Templates/ModFrameworkSample/  UE5 Third Person sample game, user-created
│   └── Mods/ExampleMod/           manifest + Icon.png; Blueprint content not yet authored
├── docs/                          10 public references + internal/
├── .dev/                          workflow scripts and the compile harness script
└── Setup.ps1                      links both plugins into the sample project
```

**Nothing is committed.** The user commits themselves — never commit or push on their behalf.

---

## Decisions (settled — do not re-open)

| Decision | Why |
|---|---|
| **One repo, sibling plugin folders** | User's explicit choice over a two-repo split. Public/private boundary handled at publish time by which files get copied into a bundle. |
| **SDK generation lives inside ModFramework** | Not a separate plugin. It only scans the reflection data of whatever project it runs in, so shipping it to mod authors is dead weight, not a leak. Core in `ModFrameworkDeveloper`, UI in `ModFrameworkEditor`. |
| **Junctions, not `AdditionalPluginDirectories`** | That setting is `#if WITH_EDITOR` only — a packaged build discards it and looks in `../RemappedPlugins/`. It works in-editor and then breaks exactly where this framework matters. See `ProjectDescriptor.cpp:154`. |
| **No stray `.uplugin` anywhere else** | UE plugin discovery recurses until it finds one, then stops. Templates use `*.uplugin.in`; the example mod is a `mod.json` folder, never a plugin. |
| **Icon loads before mount** | A mod browser must show icons for discovered, disabled and *rejected* mods. So it is a plain image at the mod root, not a `.uasset` in the pak. |
| **Loaded and Activated are separate states** | Core architectural commitment. Do not collapse them. |
| **Blueprint mods are not a sandbox** | Documented honestly in `docs/Security.md`. Permissions are API access control. Do not add UI or docs implying process isolation. |
| **Sample game `Content/` is gitignored** | ~135 MB of stock Epic template assets this project didn't author. Repo is 1.1 MB / 202 files instead of 135 MB / 851. Ships as a GitHub release asset once the repo goes public — build it with `.dev/pack-sample-content.ps1`. The ignore is **anchored** (`/Templates/ModFrameworkSample/Content/`) so the example mod's content and any plugin `Content/` stay tracked. Never widen it to a bare `Content/`. |

---

## Gotchas discovered (verified against engine source)

- **UBT enforces a 260-character `MAX_PATH` limit on action paths** and refuses to build *before
  compiling anything*, with `"The following action paths are longer than 260 characters"` followed by
  hundreds of `[N characters] <path>` lines. It reads like a wall of compile errors and is not one.
  Intermediate paths are long on their own —
  `<host>\Plugins\ModFramework\Intermediate\Build\Win64\x64\UnrealEditor\Development\ModFramework\<file>.cpp.dep.json`
  is ~123 characters — so **the build host project must live at a short path**. The harness is at
  `F:\SelfProjects\Unreal\_ModHarness` (longest action path ≈156 chars). Do not move it under a
  session scratch directory; that is what broke the first build attempt.
- **`ModFramework` loads at `PostConfigInit`** — before the UObject system exists. Module startup
  must not touch any CDO. Console commands register unconditionally; the
  `bEnableConsoleCommands` check happens *inside* each command's lambda. Getting this wrong crashes
  at engine start.
- **`UncookedOnly` includes program targets.** `UnrealEd.Build.cs` throws a hard `BuildException`
  when `bCompileAgainstEditor` is false. `ModFrameworkDeveloper` currently lists `UnrealEd`
  unconditionally — wrap it in `if (Target.bCompileAgainstEditor)` and guard the code with
  `WITH_EDITOR`. **Not yet done.**
- `UGameplayStatics::ParseOption(FString Options, ...)` takes `Options` **by value**.
- `FARFilter::ClassNames` deprecated since 5.1 — use `ClassPaths` (`TArray<FTopLevelAssetPath>`).
- `TInstancedStruct` → `StructUtils/InstancedStruct.h` (CoreUObject).
- `EAutomationTestFlags` is an `enum class` in 5.8 — bare int flags will not compile.
- `FAppStyle` from SlateCore; `FEditorStyle` deprecated since 5.1.
- `PakFileUtilities` is a real linkable Developer module, not program-only.
- `FImageUtils::ImportBufferAsTexture2D(const TArray<uint8>&)` takes raw PNG/JPEG bytes → `UTexture2D`.
- `FPackageName::RegisterMountPoint(const FString& RootPath, const FString& ContentPath)`.
- **`TArray<TObjectPtr<T>>::Sort` dereferences before calling your predicate.** It routes through
  `TDereferenceWrapper` (`ObjectPtr.h:1288`), which calls `Predicate(*A, *B)`. So the predicate takes
  `const T&`, **not** `const TObjectPtr<T>&`. Two consequences: a null-check inside the predicate is
  unreachable dead code, and a null slot crashes *inside the engine wrapper* before your code runs.
  GC can null a `UPROPERTY` `TObjectPtr` slot at any time, so always `RemoveAll` invalid entries
  **before** sorting. This was a live latent crash in `ModExtensionRegistry::SortExtensionList`.
- **Never name a file-local helper `MakeError`.** `Templates/ValueOrError.h` declares a global
  variadic `MakeError(ArgTypes&&...)` that is an exact match for any argument list. Inside your own
  namespace it resolves fine; at any call site reached via a `using`-directive both land in one
  overload set and the engine template wins, producing a baffling
  `cannot convert TValueOrError_ErrorProxy` error. Cost 8 of the first build's 13 diagnostics.

---

## Next steps

1. ~~Foundation, manifest layer, 11 core systems~~ — done (74 files, 14 agents, 0 errors)
2. ~~Restructure into `ModFramework/`~~ — done
3. ~~First compile~~ — done. 13 diagnostics → 2 real bugs (both in Gotchas above), both fixed.
   4 of the 13 were files not yet written.
4. **IN FLIGHT: `.dev/workflows/02-integration.js`** (run `wf_4c206c88-a55`) — `UModContext`,
   `UModEntryPointBase`, `UModIconCache` + the manifest `icon` field, `UModSubsystem`, the full
   `Mod.*` console command set, and 4 automation test suites.
   **If resuming and unsure whether it finished:** check
   `<session>/subagents/workflows/wf_4c206c88-a55/journal.jsonl` for `{"type":"result"}` lines
   (8 expected). If it did not finish, just re-run the script — agents overwrite whole files, so a
   partial run is not corrupting, but do check for a truncated file from an agent killed mid-write.
5. **Recompile.** `.dev/build-harness.ps1` builds a throwaway host project at
   `F:\SelfProjects\Unreal\_ModHarness` that junctions both plugins in. Prefer it over the sample
   project while the user's editor is open — building would fight the editor for module DLL locks
   and kill their MCP server. Pass `-Clean` to wipe plugin intermediates.
5. `GameModSDK` content for the Combat variant — extension points (weapon/attack, enemy, spawner,
   game rule), a `UModAPI` the game registers, SDK events, data assets. All `ModPublic`. **Must not
   reference the sample game's module.**
6. SDK generation + `.mod` packaging (`FModPackageWriter` must match the reader's byte layout, which
   is documented at the top of `ModPackageFormat.h`).
7. **Editor tooling** — explicitly requested as a final phase, both sides. Mod-author side must make
   cook-and-package-to-a-chosen-location genuinely convenient.
8. Example mod `.uasset` content — **requires a live editor over MCP** (see the MCP section below).
   `.uasset` is binary and cannot be hand-authored, so this is the one task that is genuinely
   blocked without a running editor. Aim for a visible change in the Combat variant.

---

## MCP / live editor

Server is `ModelContextProtocol`, a built-in UE 5.8 engine plugin (`EnabledByDefault: false`),
serving `http://127.0.0.1:8000/mcp`. **Only runs while the editor is open.** `.mcp.json` must be in
the session's working directory (repo root — a copy is there), and MCP servers register at session
start, so a config change needs a new session.

Add `ModelContextProtocol` to the sample `.uproject`'s `Plugins` array so this is reproducible
rather than depending on local editor state. **Not yet done.**

---

## Known incomplete

- **Not yet compiled clean.** The first build got to 13 diagnostics; the 2 real bugs are fixed but
  the build has not been re-run since, and `UModSubsystem` did not exist for it. Expect a fresh
  crop of errors once workflow 2's files land — nothing after `UModSubsystem` has ever compiled.
- `ModFrameworkDeveloper` `UnrealEd` dependency not yet guarded with
  `if (Target.bCompileAgainstEditor)`. Harmless today (editor targets only) — breaks the moment the
  plugin is enabled for a program target.
- These are **placeholders** that workflow 2 / the editor phase replace, not finished work:
  - `Private/Debug/ModConsoleCommands.{h,cpp}` — empty `Register()`/`Unregister()` so the module links
  - `ModDeveloperWindow` — registers a real nomad tab but its content is a "not implemented" label.
    `FModEditorCommands` **is** real and can be built on.
- Sample `.uproject` not yet wired to the plugins (`Setup.ps1` not run), and `ModelContextProtocol`
  not yet added to its `Plugins` array
- `GameModSDK` is scaffolding only — descriptor, Build.cs, module, icon. No APIs or extension points.
- No `.uasset` content anywhere; `ExampleMod` will fail at load with `EntryPointMissing` by design
- `docs/SDKGeneration.md` and the two getting-started guides not written — deliberately deferred
  until the tooling exists so they describe reality
