# Project Status & Handoff

Internal working document. Read this first when resuming work with no prior conversation context.

**Last updated:** 2026-08-16 — **the whole stack compiles clean**, including the sample game with
its mod integration. 121 C++ files. Verify plugins alone with `.dev/build-harness.ps1`, or the full
integration with `.dev/build-sample.ps1` (close the editor first — it locks module DLLs).

Working end to end in code: manifests, versioning, dependency resolution, lifecycle, content
mounting, API/extension/event registries, permissions, save isolation, multiplayer validation,
`.mod` read **and write**, console commands, and a real game registering a real modding surface.
Outstanding: SDK generation, 2 of 4 test suites, editor tooling, and any `.uasset` content.

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
| **Lua build layout: two modules** | `Source/ThirdParty/Lua` (vendored 5.5, `bRequiresImplementModule = false`) and `Source/ModFrameworkLua` (our runtime + bindings) — matching the engine's own `libpas`. Three things this buys, all learned the hard way: the compiler relaxations vendored C needs (`bUseUnity=false`, `NoSharedPCHs`, `UnreachableCodeWarningLevel=Off`) apply to Lua alone instead of our C++; upgrading is a single-folder replace; and licence auditing finds `ThirdParty/` where it expects. **`LUA_BUILD_AS_DLL=1` is mandatory and public** — each UE module is its own DLL and Lua's `LUA_API` is a bare `extern` by default, so splitting it out produced 102 unresolved externals until that define was added. Every vendored `.c` already defines `LUA_CORE`/`LUA_LIB`, so export-in/import-out falls out automatically. **There is deliberately no `LUA_ANSI`** — removed after Lua 5.1, a no-op in 5.5, and setting it would falsely imply io/os/package were compiled out. |
| **Scripting: Lua, behind `IModScriptRuntime`** | User's decision. Seam at `Public/Scripting/ModScriptRuntime.h`; **Lua 5.5.1 is implemented** in `ModFrameworkLua` and verified running in the sample game. Lua chosen for reach (lingua franca of game modding), zero engine coupling, and because it is the first path where permissions become *enforceable* rather than advisory — a VM sees only what is bound, unlike Blueprint. AngelScript was considered: vanilla embedded is viable and its static typing suits this codebase's fail-early habit, but UnrealEngineAngelscript is an engine fork whose bindings are auto-generated from full UE reflection, which is the opposite of a capability boundary. `LoadScript` takes **bytes, not a string**, so a bytecode or WASM runtime can implement the same interface — WASM being the only option with real memory isolation. |
| **Native C++ mods stay unimplemented — a custom engine makes them undistributable** | The manifest parses and validates `entryPoint.nativeModule` and `native_code` is a registered dangerous permission, but nothing loads it. The blocker is not framework work (`IPluginManager::MountExplicitlyLoadedPlugin` exists and the hook is ~a day). It is that `FModuleManager` rejects any module whose `.modules` BuildId mismatches (`ModuleManager.cpp:1944`) — and worse, **a studio with a modified engine has a different ABI entirely** (struct layouts, vtables, inlined code), so a mod built against stock UE would corrupt memory rather than fail cleanly. Enabling native mods therefore requires distributing the studio's whole custom engine, which is often legally impossible: console platform code is under NDA. **Lua is the better second tier precisely because it has zero engine coupling** — see the scripting note below. Same reasoning one level down applies to Blueprint: if engine changes touch asset serialisation, mod authors need a custom editor dev kit to cook against. |
| **Mental model: ModFramework is Forge, GameModSDK is the game's published API** | The user's framing, and a useful checklist. Framework = loader, discovery, lifecycle, registries, event bus, config, packaging format. SDK = the game-specific surface a studio publishes. A mod jar ↔ a `.mod`; `mods/` ↔ `{Project}/Mods`; `mods.toml` ↔ `mod.json`. When asking "should X live in the framework?", ask whether Forge would own it. |
| **Package reader AND writer both live in ModFramework** | Considered moving the writer to the SDK (mod authors need packaging, and they receive the SDK). Rejected: the writer serialises using the runtime module's own `operator<<`, so reader/writer agreement is structural rather than disciplinary — splitting them makes two implementations of one spec. Packaging is also game-agnostic, so every generated SDK would carry a duplicate, and a format change would need every SDK regenerated. Mod authors already get packaging via `ModFrameworkDeveloper`, which ships in the bundle. **What genuinely belongs in the SDK is game-specific pre-package validation** (do the referenced extension points exist? are the requested permissions ones this game defines?) — not yet written. |
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
  when `bCompileAgainstEditor` is false, so an unconditional `UnrealEd` dependency in an
  `UncookedOnly` module breaks any build that enables the plugin for a program target. Fixed:
  `ModFrameworkDeveloper` now guards `UnrealEd` and `DesktopPlatform` behind
  `if (Target.bCompileAgainstEditor)`. **Keep it that way**, and guard code behind them with
  `WITH_EDITOR`.
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
- **In automation specs, do not name a local `Description`.** `FAutomationTestBase` has a
  `Description` member, and UE builds with warnings-as-errors, so `C4458: declaration of
  'Description' hides class member` fails the build. Use `FailureMessage` or similar. Same applies
  to any other base-class member name — specs inherit a lot.
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
4. `.dev/workflows/02-integration.js` (run `wf_4c206c88-a55`) — **partially complete.** Landed and
   verified non-truncated: `UModContext`, `UModEntryPointBase`, `UModIconCache` + the manifest
   `icon` field, `UModSubsystem` (2448-line .cpp), and the full `Mod.*` console command set
   (2454-line .cpp).
   **Still missing: all four automation test suites.** Those agents were killed by a monthly API
   spend limit, not by any code problem. To finish them:
   `Workflow({scriptPath: '.dev/workflows/02-integration.js', resumeFromRunId: 'wf_4c206c88-a55'})`
   replays the five completed agents from cache and re-runs only the test agents. If the run id has
   expired, just run the script fresh — agents overwrite whole files, so re-running is safe; only
   check for a file truncated by an agent killed mid-write.

   **Two lifecycle constraints the ModContext agent flagged — verify `UModSubsystem` honours them:**
   - `UnloadMod` must call `UModExtensionRegistry::UnregisterAllForMod`,
     `UModEventBus::UnsubscribeAllForMod` and `UModAPIRegistry::UnregisterAllForMod` **before**
     `UModRegistry::ReleaseModObjects`. Extensions created via `UModContext` are both held by the
     extension registry and tracked by the mod registry; releasing first leaves the extension
     registry holding garbage `TObjectPtr`s.
   - `UModContext` must be created with the subsystem as its outer, and the entry point with its
     context as outer — both `GetWorld()` fallbacks depend on that chain.
5. ~~Compile clean~~ — **done.** All four modules build and link. Two bugs fixed along the way, both
   recorded in Gotchas. Re-verify any time with `.dev/build-harness.ps1`, which builds a host project at
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

- **Two of four automation test suites are missing** — `ModLifecycleTests.spec.cpp` and
  `ModSystemsTests.spec.cpp`. `ModManifestTests` and `ModDependencyTests` landed and compile.
  The missing agents were killed by an API spend limit, not by any code problem.
- **SDK generation is not written.** `FModPublicApiScanner`, `FModSDKGenerator` and the
  `GenerateModSDK` commandlet. Everything they depend on exists and compiles; see
  `.dev/workflows/03-sdk-and-packaging.js` for the full brief (the `tool:sdkgen` agent).
- **Editor tooling is placeholder only** — see the placeholder list below. This is the phase the
  user explicitly asked for last, both mod-author and game-developer sides.
- **No `.uasset` content exists**, so the example mod cannot actually load yet. Needs a live editor
  over MCP; `.uasset` is binary and cannot be hand-authored.
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
