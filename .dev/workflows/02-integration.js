export const meta = {
	name: 'modframework-integration',
	description: 'ModContext, icon cache, UModSubsystem, console commands and automation tests',
	phases: [
		{ title: 'ModAPI', detail: 'UModContext, UModEntryPointBase, UModIconCache' },
		{ title: 'Subsystem', detail: 'UModSubsystem lifecycle orchestration' },
		{ title: 'Diagnostics', detail: 'Mod.* console commands' },
		{ title: 'Tests', detail: 'automation specs across every area' },
	],
}

// NOTE: paths assume the restructure has already happened.
const ROOT = 'F:/SelfProjects/Unreal/Plugins/ModFramework/ModFramework'
const CONTRACT = 'C:/Users/aydan/AppData/Local/Temp/claude/F--SelfProjects-Unreal-Plugins-ModFramework/fff4afaa-4ac9-404c-85de-4eb57e5f031e/scratchpad/CONTRACTS.md'

const PREAMBLE = `You are completing a UE 5.8 modding framework plugin. The foundation, manifest layer and all
eleven core systems are ALREADY WRITTEN AND ON DISK.

MANDATORY FIRST STEPS:
1. Read the contract at ${CONTRACT} — it is binding.
2. READ THE ACTUAL HEADERS you depend on under ${ROOT}/Source/ModFramework/Public/.
   The contract is the design; the headers are the truth. Where they differ, FOLLOW THE HEADERS and
   say so in your return value. Do not invent signatures — open the file.

Plugin root: ${ROOT}

ENGINE SOURCE IS AVAILABLE — USE IT: F:/SelfProjects/Unreal/UE_5.8/Engine/Source
Verify every engine API against it with Grep/Read before use. Already verified, do not re-derive:
  TInstancedStruct        -> "StructUtils/InstancedStruct.h" (CoreUObject)
  EAutomationTestFlags    -> enum class in "Misc/AutomationTest.h"
  FPakPlatformFile::Mount -> (const TCHAR*, uint32 PakOrder, const TCHAR* InPath, bool bLoadIndex, FPakListEntry*)
  FPackageName::RegisterMountPoint(const FString& RootPath, const FString& ContentPath)
  UGameplayStatics::ParseOption(FString Options, const FString& Key)   // Options BY VALUE
  FARFilter::ClassPaths is TArray<FTopLevelAssetPath>; ClassNames deprecated since 5.1
  FImageUtils::ImportBufferAsTexture2D(const TArray<uint8>&) -> "ImageUtils.h" (Engine)
  IImageWrapperModule::DetectImageFormat(const void*, int64) -> ImageWrapper module
  FSHA1::HashBuffer(const void*, uint64, uint8* OutHash)
  FCompression::UncompressMemory(FName, void*, int64, const void*, int64, ECompressionFlags, uintptr_t)

TWO BUGS ALREADY FOUND AND FIXED IN THE EXISTING CODE — do not reintroduce either:
1. TArray<TObjectPtr<T>>::Sort does NOT hand your predicate the TObjectPtr. It routes through
   TDereferenceWrapper (ObjectPtr.h:1288) which calls Predicate(*A, *B), so the predicate must take
   \`const T&\`, not \`const TObjectPtr<T>&\`. A null-check inside such a predicate is unreachable AND
   a null slot crashes inside the engine wrapper before your code runs. GC can null a UPROPERTY
   TObjectPtr slot at any time, so ALWAYS RemoveAll invalid entries BEFORE sorting.
2. Never name a file-local helper \`MakeError\`. Templates/ValueOrError.h declares a global variadic
   \`MakeError(ArgTypes&&...)\` that is an exact match for any argument list, so at any call site with
   a using-directive it wins the overload and you get an unreadable
   "cannot convert TValueOrError_ErrorProxy" error. Use a distinct name.

You cannot compile. Write complete, production-quality implementations — no TODO stubs, no
placeholder bodies. Every declared function gets a real body.

Style: Epic conventions, tabs, Allman braces, #pragma once, strict IWYU, and every file begins with
  // Copyright (c) 2026. Licensed for use in your own projects.
followed by a blank line.

Mod data is untrusted: validate, never check()/assert on it, return FModDiagnostic.

Return JSON: { "files": [...], "notes": "...", "concerns": [...] }`

const SCHEMA = {
	type: 'object',
	additionalProperties: false,
	required: ['files', 'notes', 'concerns'],
	properties: {
		files: { type: 'array', items: { type: 'string' } },
		notes: { type: 'string' },
		concerns: { type: 'array', items: { type: 'string' } },
	},
}

phase('ModAPI')

const modApi = await parallel([
	() => agent(`${PREAMBLE}

YOUR SCOPE: the mod-facing handle. Write per contract section 21:
  Public/Runtime/ModContext.h        + Private/Runtime/ModContext.cpp
  Public/Runtime/ModEntryPointBase.h + Private/Runtime/ModEntryPointBase.cpp

UModContext is the ONLY object a mod receives, so it is the security and API boundary. Everything a
mod can do passes through it. Requirements:
- Every method resolves the owning subsystem through a TWeakObjectPtr and fails safe (logs + returns
  a null/false result) when it is gone. A mod calling into a torn-down context must never crash.
- RegisterExtension stamps OwningModId itself; a mod cannot claim to be another mod.
- CreateModObject news the object with the context as outer AND registers it with
  UModRegistry::TrackModObject, so unload releases it.
- SaveJson/LoadJson route through UModSaveDataManager, which enforces the save.modify permission.
- LogInfo/LogWarning/LogError prefix every message with [<modid>] so mod output is attributable in a
  shared log. This is the single most useful diagnostic affordance in the whole system.
- GetWorld() routes through the subsystem's GameInstance.
- GetSubsystem() is exposed per the contract but add a header comment stating plainly that it is an
  escape hatch, that anything reached through it is NOT permission-gated, and that games hardening
  against untrusted mods should consider removing it.

UModEntryPointBase: the Blueprint-subclassable entry point. Native* methods call the matching
BlueprintImplementableEvent and are safe to call when the Blueprint does not implement them.
GetWorld() routes via Context.`, { label: 'context', phase: 'ModAPI', schema: SCHEMA }),

	() => agent(`${PREAMBLE}

YOUR SCOPE: mod icons — contract section 26. This is an ADDITIVE change to files another agent
already wrote, so EDIT them surgically rather than rewriting.

1. EDIT Public/Manifest/ModManifest.h — add \`FString IconPath;\` to FModManifest with the same
   UPROPERTY form as its neighbours.
2. EDIT Private/Manifest/ModManifestParser.cpp — parse the optional "icon" field, serialise it when
   non-empty, and validate it in ValidateManifest with code Manifest.InvalidIcon: reject absolute
   paths, ".." segments, backslashes, a leading slash, and any extension other than .png/.jpg/.jpeg
   (case-insensitive). Reuse whatever path-containment helper the existing content-root validation
   uses — read the file first and match its style exactly.
3. EDIT Public/Registry/ModInfo.h — add \`FString ResolvedIconPath;\`.
4. EDIT Public/Settings/ModFrameworkSettings.h + .cpp — add MaxIconFileBytes (2 MiB),
   MaxIconDimension (1024), DefaultModIcon (TSoftObjectPtr<UTexture2D>), category "Icons".
5. EDIT Source/ModFramework/ModFramework.Build.cs — add "ImageWrapper" to public dependencies.
6. CREATE Public/Content/ModIconCache.h + Private/Content/ModIconCache.cpp per the contract.

Icon rules that matter:
- Must load BEFORE the mod is mounted. For a loose mod folder read ResolvedIconPath from disk; for a
  mod discovered as a .mod package read the entry through FModPackageReader — read that header first
  for its real API.
- Untrusted input: enforce MaxIconFileBytes before reading the whole file, DetectImageFormat before
  decoding, and reject anything not PNG/JPEG.
- File read + validation on a task thread (AsyncTask / UE::Tasks — verify what 5.8 offers), texture
  import and delegate on the game thread.
- Coalesce concurrent requests for the same mod. A request for an already-cached icon still fires
  the delegate on the next tick, never synchronously.
- EVERY failure path returns the default icon, never nullptr, so UI code needs no null branch. A bad
  icon must never affect whether the mod loads — log a warning and move on.
- Guard against the subsystem or cache being torn down while an async read is in flight (weak this).`,
		{ label: 'icons', phase: 'ModAPI', schema: SCHEMA }),
])

log(`ModAPI phase done: ${modApi.filter(Boolean).length}/2`)

phase('Subsystem')

const subsystem = await agent(`${PREAMBLE}

YOUR SCOPE: UModSubsystem — the orchestrator, per contract section 22. This is the largest and most
important file in the plugin.

Write Public/Subsystem/ModSubsystem.h + Private/Subsystem/ModSubsystem.cpp.

READ FIRST — you depend on all of these and must use their real signatures:
  Public/Registry/ModRegistry.h        Public/API/ModAPIRegistry.h
  Public/Extensions/ModExtensionRegistry.h  Public/Events/ModEventBus.h
  Public/Permissions/ModPermissionRegistry.h Public/Save/ModSaveDataManager.h
  Public/Content/ModContentManager.h   Public/Providers/ModProvider.h
  Public/Dependencies/ModDependencyResolver.h Public/Conflicts/ModConflictDetector.h
  Public/Net/ModSessionManifest.h      Public/Runtime/ModContext.h
  Public/Content/ModIconCache.h        Public/Settings/ModFrameworkSettings.h

Non-negotiable behaviour:
- Initialize(): create the registries, the event bus, permission registry, save manager, icon cache;
  wire the FModPermissionCheck delegate into the API registry, extension registry and event bus;
  register FLocalFileModProvider; register the built-in permissions. Then, if
  bAutoDiscoverOnStartup, call RefreshMods(bAutoActivateLoadedMods).
- EVERY state change goes through UModRegistry::SetModState and then broadcasts the matching
  ModFrameworkEvents::* on the bus. Never assign state directly, anywhere.
- LoadMod requires state Mounted; ActivateMod requires Loaded. Keep them strictly separate.
- LoadMod: resolve the entry point class (soft class path -> TryLoadClass), NewObject it, create the
  UModContext, assign it, call NativeOnModLoaded. A missing/invalid entry class where the manifest
  declared one is EntryPointMissing/EntryPointInvalid — but a manifest with NO entry point is legal
  and loads fine (the pure-asset case).
- ActivateMod: load the content bundles, register their extension classes, then NativeOnModActivated.
- UnloadMod: deactivate if needed, NativeOnModUnloaded, unregister the mod's APIs, extensions and
  event subscriptions, ReleaseModObjects, then unmount. Order matters — release before unmount.
- RefreshMods: discover (all providers) -> parse/validate -> register -> evaluate permissions ->
  resolve dependencies -> apply load order -> detect conflicts (blocking conflicts reject with
  ConflictRejected) -> mount -> load -> optionally activate. Every stage failure marks that mod
  Failed with a reason and continues with the others. One bad mod must never abort the batch.
- ReloadMod / ReloadAll: unload in reverse load order, reload in load order.
- Deinitialize(): deactivate and unload everything in reverse load order, then shut down every
  owned object. Must be safe when Initialize partially failed.
- Get(WorldContext) resolves via UGameInstance::GetSubsystem — verify the correct 5.8 path from a
  UObject world context, and return nullptr rather than asserting when there is no game instance.
- BuildSessionManifest() populates from active mods + settings identity.
- Broadcast WorldCreated/WorldDestroyed/GameStarted by binding the appropriate FWorldDelegates —
  verify which delegates exist in 5.8 and unbind them in Deinitialize.
- Blueprint delegates OnModStateChanged/OnModsRefreshed mirror the registry's native delegate.

Every public entry point must be safe to call in any order and any state. Assume a console command
or a Blueprint will call ActivateMod on an unmounted mod — return false with a clear log, do not
crash.`, { label: 'subsystem', phase: 'Subsystem', schema: SCHEMA })

phase('Diagnostics')

const console_ = await agent(`${PREAMBLE}

YOUR SCOPE: the Mod.* console commands — contract section 23, plus Mod.Icons from section 26.

Write Private/Debug/ModConsoleCommands.h + Private/Debug/ModConsoleCommands.cpp.

The exact required shape, because ModFrameworkModule.cpp already calls it and nothing else links:
  class FModConsoleCommands { public: static void Register(); static void Unregister(); };

CRITICAL ORDERING CONSTRAINT: the ModFramework module loads at PostConfigInit, BEFORE the UObject
system exists. Register() therefore MUST NOT read UModFrameworkSettings or touch any CDO. Construct
the FAutoConsoleCommandWithWorldAndArgs objects unconditionally and check
UModFrameworkSettings::Get()->bEnableConsoleCommands INSIDE each command's execution lambda, with a
null-safe accessor. Getting this wrong crashes at engine start.

Wrap the whole file in #if !UE_BUILD_SHIPPING || ALLOW_CONSOLE, with Register/Unregister compiled to
empty inline bodies otherwise so the module still links.

Commands: Mod.List, Mod.Info, Mod.Load, Mod.Unload, Mod.Activate, Mod.Deactivate, Mod.Reload,
Mod.ReloadAll, Mod.Refresh, Mod.Validate, Mod.Dependencies, Mod.Mounts, Mod.DumpRegistry,
Mod.DumpAPIs, Mod.DumpExtensions, Mod.DumpEvents, Mod.DumpPermissions, Mod.Conflicts,
Mod.SessionManifest, Mod.Enable, Mod.Disable, Mod.Icons.

Quality bar — these are the primary debugging surface:
- Output goes to LogModFramework AND to the FOutputDevice the command receives when available.
- Aligned column output for the list commands; compute column widths from the data.
- A command taking <ModId> with no argument prints usage plus the list of valid ids. A wrong id
  prints "no such mod" plus the closest matches by edit distance — mod ids are long and get typo'd.
- Resolve the subsystem from the world; print a clear message when there is no game instance rather
  than crashing.
- Every command is safe with mods in any state.`, { label: 'console', phase: 'Diagnostics', schema: SCHEMA })

phase('Tests')

const TEST_AREAS = [
	{
		key: 'manifest',
		file: 'ModManifestTests.spec.cpp',
		scope: `Manifest parsing and validation, plus FModVersion / FModVersionRange.
Cover: valid full manifest round-trip (parse -> serialise -> parse, all fields equal); every required
field missing in turn; malformed JSON; unknown field warning; invalid mod ids (too short, uppercase
normalisation, illegal characters, "..", leading/trailing separators); self-dependency; duplicate
dependency; content root with an absolute path and with a ".." escape (BOTH must be rejected —
security); malformed mount points; invalid icon paths; manifestVersion newer than supported.
Semver: parse/reject leading zeroes, negative, empty identifiers, too many core numbers, "v" prefix,
partial versions; full pre-release precedence ordering including numeric-vs-alphanumeric and
identifier-count rules; build metadata ignored for ordering.
Ranges: every operator, caret at 1.x/0.x/0.0.x, tilde, hyphen, wildcards, AND, OR, and the
pre-release gating rule (2.0.0-rc.1 must NOT satisfy ">=1.0.0" but must satisfy ">=2.0.0-rc.1").`,
	},
	{
		key: 'dependencies',
		file: 'ModDependencyTests.spec.cpp',
		scope: `Dependency resolution and conflict detection.
Cover: linear and diamond graphs resolve in a valid topological order; DETERMINISM — the same input
in a different array order must produce the identical load order (this is the property most likely to
regress); missing required dependency rejects; version-incompatible dependency rejects with both
versions in the message; optional missing produces info only and still loads; cascade rejection is
transitive; two-node and three-node cycles rejected with the cycle path; self-dependency; ordering
cycles via loadBefore/loadAfter; duplicate ids keep the highest version; disabled mods excluded;
environment checks for game id, game version, framework range and SDK range; priority tie-breaking.
Conflicts: single claim is not a conflict; two claims from the SAME mod are not a conflict; each
policy picks the right winner; Error is blocking and Merge is not; policy precedence order
(resource override > point override > unanimous preference > default).`,
	},
	{
		key: 'lifecycle',
		file: 'ModLifecycleTests.spec.cpp',
		scope: `Registry, state machine, API registry and extension registry.
Cover: IsTransitionAllowed accepts every legal edge and rejects a representative set of illegal ones;
any->Failed always allowed; register/unregister/lookup by id; GetAllMods ordering; state change
delegate fires after the state is committed, not during; mod object tracking, reverse lookup, and
release; stale weak pointers pruned.
API: register, duplicate id rejected, lookup, version-range request accepted and rejected, permission
denial, class mismatch on the typed request path, unregister-all-for-mod.
Extensions: extension point registration and duplicate rejection; base class enforcement; duplicate
extension id; bAllowMultiplePerMod; permission gating; ORDERING by (priority desc, load order asc,
id asc); ResolveResource under each policy; unregistering a point that still has extensions is
refused; UnregisterAllForMod cleans up completely.`,
	},
	{
		key: 'systems',
		file: 'ModSystemsTests.spec.cpp',
		scope: `Permissions, save data, packaging and multiplayer.
Permissions: default policy chain in order; dangerous permissions never auto-granted even when
listed in AutoGrantedPermissions; unknown permission handling both ways; Pending is NOT granted;
custom IModPermissionPolicy overrides; per-mod isolation.
Save: write/read json and bytes round-trip; per-mod isolation (writing mod A never touches mod B);
orphan marking when a mod is absent; orphans survive a save/load round trip BYTE-FOR-BYTE (this is
the core guarantee — removing a mod must not corrupt a save); purge is explicit-only; payload size
caps; migration walks versions one step at a time and refuses to go downwards.
Packaging: FModPackageHeader/FModPackageEntry archive round-trip; reader rejects bad magic, an
unsupported version, offsets past EOF, an absurd entry count, and unsafe entry paths (absolute, "..",
drive letter, backslash) — write these as security tests with hostile fixtures built in
FPaths::AutomationTransientDir().
Net: session manifest Base64 round-trip; FromBase64 rejects empty, garbage, wrong format version and
an oversized entry count; ValidateClient for each mismatch type; ClientOnly mods never cause a
mismatch; ServerOnly on the client is a ScopeViolation; digest is stable and order-independent.`,
	},
]

const tests = await parallel(TEST_AREAS.map(t => () => agent(`${PREAMBLE}

YOUR SCOPE: automation tests. Write ${ROOT}/Source/ModFramework/Private/Tests/${t.file}

Wrap the entire file in #if WITH_DEV_AUTOMATION_TESTS ... #endif.
Use the Spec style (BEGIN_DEFINE_SPEC / DEFINE_SPEC) with flags
  EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter
(EAutomationTestFlags is an enum class in 5.8 — verify the macro's exact expectations in
Misc/AutomationTest.h before writing, including whether it wants EAutomationTestFlags_ApplicationContextMask).
Test names are prefixed "ModFramework.${t.key === 'manifest' ? 'Manifest' : t.key === 'dependencies' ? 'Dependencies' : t.key === 'lifecycle' ? 'Lifecycle' : 'Systems'}".

Tests must not require a world, a game instance, or pre-existing mods on disk. Anything needing files
creates them under FPaths::AutomationTransientDir() and cleans up afterwards. For UObject-based
registries, NewObject<T>(GetTransientPackage()) and add to root only if needed.

READ THE REAL HEADERS for every type you touch before writing a single assertion — the contract may
differ from what shipped, and a test written against the contract instead of the code is worse than
no test.

${t.scope}

Write assertions that would actually catch a regression: assert on specific values and orderings, not
just "returns true". Where a behaviour is a documented guarantee (determinism, orphan preservation,
path containment), say so in a comment above the test so nobody "simplifies" it away later.`,
	{ label: `test:${t.key}`, phase: 'Tests', schema: SCHEMA })))

return {
	modApi: modApi.filter(Boolean),
	subsystem,
	console: console_,
	tests: TEST_AREAS.map((t, i) => ({ key: t.key, result: tests[i] })),
}
