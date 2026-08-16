export const meta = {
	name: 'modframework-tests-sdkgen-editor',
	description: 'Remaining automation tests, SDK generation, SDK-side validation and editor tooling',
	phases: [
		{ title: 'Tests', detail: 'lifecycle and systems automation suites' },
		{ title: 'SDKGen', detail: 'ModPublic scanner, bundle generator, SDK-side validation' },
		{ title: 'Editor', detail: 'mod-author window and game-developer tooling' },
	],
}

const REPO = 'F:/SelfProjects/Unreal/Plugins/ModFramework'
const FW = `${REPO}/ModFramework`
const SDK = `${REPO}/GameModSDK`

const PREAMBLE = `You are extending a UE 5.8 modding framework that COMPILES CLEAN today. Do not break that.

MANDATORY FIRST STEP: read the real headers you depend on under
${FW}/Source/ModFramework/Public/ and ${SDK}/Source/GameModSDK/Public/.
They are the truth. The design reference at ${REPO}/docs/internal/ImplementationContract.md is
accurate but NOT authoritative where it disagrees with a header - follow the header and say so.

Verify the current state before assuming anything: ${REPO}/docs/internal/Status.md lists what is
finished, what is placeholder, and every engine gotcha found so far. READ IT.

ENGINE SOURCE: F:/SelfProjects/Unreal/UE_5.8/Engine/Source - grep it to verify any engine API.

BUGS ALREADY FOUND HERE. Reintroducing one is a defect:
1. TArray<TObjectPtr<T>>::Sort routes through TDereferenceWrapper, which calls Predicate(*A, *B).
   The predicate takes \`const T&\`, NOT \`const TObjectPtr<T>&\`. A null check inside it is
   unreachable and a null slot crashes inside the engine. RemoveAll invalid entries BEFORE sorting.
2. Never name a file-local helper \`MakeError\` - it collides with the global variadic template in
   Templates/ValueOrError.h and yields an unreadable TValueOrError_ErrorProxy error.
3. In automation specs, never name a local \`Description\` - FAutomationTestBase has that member and
   UE builds warnings-as-errors, so C4458 fails the build. Use \`FailureMessage\`.
4. ModFrameworkDeveloper's UnrealEd/DesktopPlatform deps are behind
   \`if (Target.bCompileAgainstEditor)\`. Guard code needing them with WITH_EDITOR. Keep it that way.
5. ModFramework loads at PostConfigInit, BEFORE the UObject system exists. Module startup must not
   touch any CDO.

THE ARCHITECTURAL RULE: ModFramework contains NO game-specific concepts - verified by grep, keep it
true. GameModSDK may never reference the game's own module. If an SDK type needs something from the
game, it goes behind a UModAPI the game registers at runtime.

Style: Epic conventions, tabs, Allman braces, #pragma once, strict IWYU, every file starts with
  // Copyright (c) 2026. Licensed for use in your own projects.
then a blank line. Mod input is untrusted: validate, never check() on it.

Write complete implementations. No TODO stubs, no empty bodies.

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

phase('Tests')

const TEST_SPECS = [
	{
		key: 'lifecycle',
		file: 'ModLifecycleTests.spec.cpp',
		prefix: 'Lifecycle',
		scope: `Registry, the state machine, the API registry and the extension registry.
- UModRegistry::IsTransitionAllowed accepts every legal edge and rejects a representative set of
  illegal ones; any->Failed is always allowed. Read the real transition table in ModRegistry.cpp.
- register/unregister/lookup by id; GetAllMods ordering (LoadOrder asc with INDEX_NONE last, then id)
- the state-changed delegate fires AFTER the state is committed, never during
- mod object tracking, reverse lookup by object, ReleaseModObjects, stale weak pointers pruned
- API registry: register, duplicate id rejected, version-range request accepted and rejected,
  permission denial, class mismatch on the typed path, UnregisterAllForMod
- Extension registry: point registration and duplicate rejection; base class enforcement; duplicate
  extension id; bAllowMultiplePerMod; permission gating; ORDERING by
  (priority desc, mod load order asc, id asc); ResolveResource under each conflict policy;
  unregistering a point that still has extensions is refused; UnregisterAllForMod cleans up fully.
- SortExtensionList must survive a null/garbage slot in the array - that was a real crash. Write a
  test that puts an invalid entry in and sorts.`,
	},
	{
		key: 'systems',
		file: 'ModSystemsTests.spec.cpp',
		prefix: 'Systems',
		scope: `Permissions, config, save data, packaging round-trip and multiplayer.
- Permissions: the default policy chain in order; dangerous permissions NEVER auto-granted even when
  listed in AutoGrantedPermissions; unknown permission handled both ways; Pending is NOT granted;
  a custom IModPermissionPolicy overrides; per-mod isolation.
- Config (UModConfigManager, newly added - read its header): a user value shadows a default; a read
  falls through to the default when the user layer lacks the key; ResetToDefaults restores
  fall-through; SetConfigJson rejects malformed JSON WITHOUT corrupting the existing store;
  GetKeys is the union of both layers and is sorted; per-mod isolation.
- Save: write/read json and bytes round-trip; per-mod isolation; orphan marking when a mod is
  absent; ORPHANS SURVIVE A SAVE/LOAD ROUND TRIP BYTE-FOR-BYTE (the core guarantee - removing a mod
  must not corrupt a save); purge is explicit-only; payload size caps; migration walks one version
  at a time and refuses to go downwards.
- Packaging ROUND TRIP - the highest-value test here. FModPackageWriter now exists in
  ModFrameworkDeveloper, so this suite CANNOT reference it (ModFramework cannot depend on its own
  developer module). Instead test FModPackageReader against hostile fixtures you build by hand with
  FArchive in FPaths::AutomationTransientDir(): bad magic, unsupported version, offsets past EOF,
  an absurd entry count, and unsafe entry paths (absolute, "..", drive letter, backslash). These are
  security tests - say so in comments so nobody "simplifies" them away.
- Net: session manifest Base64 round-trip; FromBase64 rejects empty, garbage, wrong format version
  and an oversized entry count; ValidateClient for each mismatch type; ClientOnly mods never cause a
  mismatch; ServerOnly on the client is a ScopeViolation; the digest is stable and order-independent.`,
	},
]

const tests = await parallel(TEST_SPECS.map(t => () => agent(`${PREAMBLE}

YOUR SCOPE: automation tests. Write ${FW}/Source/ModFramework/Private/Tests/${t.file}

Two suites already exist and COMPILE - read them first and match their structure, helpers and naming
exactly rather than inventing your own:
  ${FW}/Source/ModFramework/Private/Tests/ModManifestTests.spec.cpp
  ${FW}/Source/ModFramework/Private/Tests/ModDependencyTests.spec.cpp

Wrap the file in #if WITH_DEV_AUTOMATION_TESTS ... #endif. Test names prefixed
"ModFramework.${t.prefix}". Tests must not require a world or a game instance; anything needing files
uses FPaths::AutomationTransientDir() and cleans up.

${t.scope}

Assert on specific values and orderings, not just "returns true". Where a behaviour is a documented
guarantee (determinism, orphan preservation, path containment, config fall-through), say so in a
comment above the test.`, { label: `test:${t.key}`, phase: 'Tests', schema: SCHEMA })))

log(`Tests: ${tests.filter(Boolean).length}/2`)

phase('SDKGen')

const sdkgen = await parallel([
	() => agent(`${PREAMBLE}

YOUR SCOPE: SDK generation. Write under ${FW}/Source/ModFrameworkDeveloper/:
  Public/SDK/ModPublicApiScanner.h + Private/SDK/ModPublicApiScanner.cpp
  Public/SDK/ModSDKGenerator.h     + Private/SDK/ModSDKGenerator.cpp
  Public/SDK/ModSDKCommandlet.h    + Private/SDK/ModSDKCommandlet.cpp  (UGenerateModSDKCommandlet, -run=GenerateModSDK)

FModPublicApiScanner walks TObjectRange<UClass>/UScriptStruct/UEnum collecting everything whose
metadata has "ModPublic", plus ModApiId / ModApiVersion / ModApiPermissions /
ModApiServerAuthoritative / ModExtensionPoint / ModSince / ModDeprecated. Verify the 5.8 metadata API
(UField::GetMetaData / HasMetaData) and note in a comment that metadata is WITH_EDITORONLY_DATA -
fine here because this is editor-time tooling, but it is why APIs also override NativeGetApiId.
Produce a structured FModPublicApiReport: classes, structs, enums, interfaces, functions, properties,
each with owning module, header path and metadata.
Warn about likely mistakes, because these are the ones that break a generated SDK's compile:
  - a UModAPI subclass with no ModApiId
  - a ModPublic class whose BlueprintCallable functions are all unmarked
  - a ModPublic type whose signature names an UNMARKED type (this is the leak that makes a generated
    SDK fail to build for a mod author - flag it loudly)

FModSDKGenerator::GenerateBundle(FModSDKGenerateOptions, diagnostics) assembles:
  <Output>/<SdkName>-<Version>/
    Plugins/ModFramework/   copied from the sibling plugin folder, EXCLUDING Binaries, Intermediate,
                            Saved, .dev, and Source/ModFrameworkDeveloper/Private/SDK (the generator
                            itself does not need to ship, though shipping it is harmless)
    Plugins/<SdkName>/      copied from the SDK plugin folder, same exclusions
    Templates/ModProject/   a .uproject enabling ONLY the SDK plugin - the framework arrives via the
                            SDK's own plugin dependency, exactly as Templates/ModAuthorSample proves
    Docs/                   copied from repo docs/, EXCLUDING docs/internal
    SDKVersion.json         all five versions + the API index from the scanner
    README.md               generated, mod-author facing
Locate plugin folders with IPluginManager, never hardcoded paths.
docs/internal MUST be excluded - it holds internal status and contract documents.`,
		{ label: 'sdk:generator', phase: 'SDKGen', schema: SCHEMA }),

	() => agent(`${PREAMBLE}

YOUR SCOPE: game-specific pre-package validation, in the SDK.

The framework can tell whether a .mod is structurally well formed. Only the game's SDK knows whether
a mod is valid FOR THIS GAME. That gap is real and currently unfilled: a mod can reference an
extension point that does not exist, or request a permission this game never defines, and packaging
happily succeeds - the failure surfaces as "nothing happened" at load time instead.

Write under ${SDK}/Source/GameModSDK/:
  Public/Validation/GameModValidator.h + Private/Validation/GameModValidator.cpp

  USTRUCT FGameModValidationResult { bool bValid; TArray<FModDiagnostic> Diagnostics; }

  UCLASS(BlueprintType) UGameModValidator : public UObject, exposing:
    static FGameModValidationResult ValidateManifest(const FModManifest& Manifest);

Checks, each producing a precise FModDiagnostic (invent stable codes under "GameMod."):
  - every claim's ExtensionPointId is one of GameModExtensionPoints::GetAllPointIds()
  - every requested permission is either a framework builtin (ModPermissions::GetBuiltinPermissions)
    or one this SDK defines - warn on unknown ones, since a typo'd permission is silently denied
  - the sdk.id matches this SDK's id, and sdk.version's range actually admits this SDK's version -
    a mod pinning "^2.0.0" against a 0.1.0 SDK can never load, and saying so at package time is far
    better than at install time
  - entryPoint.class is non-empty when contentBundles is non-empty (a bundle with nothing to
    register it is almost always a mistake)
  - warn when the mod declares no claims but registers extensions, since conflict detection then
    cannot see it

Add a GameModSDKVersion.h declaring this SDK's id and version as native constants
(GameModSDK::GetSdkId() / GetSdkVersion()) so the validator has something authoritative to compare
against rather than reading config.

This must compile in a project with NO game module present - Templates/ModAuthorSample proves that,
and .dev/check-sdk-boundary.ps1 will be run against your work.`,
		{ label: 'sdk:validation', phase: 'SDKGen', schema: SCHEMA }),
])

log(`SDKGen: ${sdkgen.filter(Boolean).length}/2`)

phase('Editor')

const editor = await parallel([
	() => agent(`${PREAMBLE}

YOUR SCOPE: the Mod Developer window - the MOD AUTHOR's tool surface. This ships in the SDK bundle,
so it must be genuinely convenient, not a debug panel.

${FW}/Source/ModFrameworkEditor/ currently has a PLACEHOLDER FModDeveloperWindow that registers a
real nomad tab whose content is a "not implemented" label, and a real FModEditorCommands TCommands
set. READ BOTH FIRST. Keep FModEditorCommands; replace the window's content.

Build a tabbed window (Public/ModDeveloperWindow.h + Private/, plus Private/Widgets/S*.h/.cpp):
  Mods         every discovered mod: icon (UModIconCache), name, id, version, state, load order.
               Buttons per row: Load / Unload / Activate / Deactivate / Reload / Enable / Disable.
  Validation   re-runs manifest validation, listing diagnostics with severity, code, message and
               context. Clicking a row focuses the offending mod.
  Dependencies a readable dependency view for the selected mod: what it needs, what needs it, and how
               each resolved. A text tree is fine - do NOT attempt a graph widget.
  Conflicts    FModConflictDetector's report: contenders, applied policy, winner, blocking status.
  Package      THE IMPORTANT ONE. Pick a mod folder, pick an output directory (IDesktopPlatform
               directory picker), and package it with FModPackageWriter::PackageDirectory. Remember
               the chosen output path per mod via a UDeveloperSettings or the editor's per-project
               config, so a mod author does not re-pick it every time. Show progress and the
               resulting diagnostics inline; on success reveal the .mod in the file explorer.

ModFrameworkEditor must add "ModFrameworkDeveloper" to its dependencies for the packaging call -
check its Build.cs; it may already be there.

Refresh on UModSubsystem's OnModStateChanged/OnModsRefreshed rather than polling on tick. The window
must behave when there is no game instance (no PIE running): show an explanatory message, never
crash. Guard any nomad tab registration on !IsRunningCommandlet(), symmetrically.

Use FAppStyle from SlateCore - FEditorStyle is deprecated since 5.1.`,
		{ label: 'editor:modauthor', phase: 'Editor', schema: SCHEMA }),

	() => agent(`${PREAMBLE}

YOUR SCOPE: the GAME DEVELOPER's editor tooling - SDK generation and inspectors. This does NOT ship
to mod authors; it lives alongside the mod-author window but in its own files and its own menu entry.

Write under ${FW}/Source/ModFrameworkEditor/:
  Public/ModSDKWindow.h + Private/ModSDKWindow.cpp  and Private/Widgets/ as needed.

  Generate SDK   Output directory picker (IDesktopPlatform), SDK name and version fields, and a
                 Generate button calling FModSDKGenerator::GenerateBundle. Show the resulting
                 diagnostics; on success reveal the bundle folder.
                 NOTE: another agent is writing FModSDKGenerator at
                 ${FW}/Source/ModFrameworkDeveloper/Public/SDK/ModSDKGenerator.h in parallel. Read it
                 if it exists; if it does not yet, code against the shape described here and say so
                 in your notes: GenerateBundle(const FModSDKGenerateOptions&, TArray<FModDiagnostic>&).
  API Inspector  FModPublicApiScanner's report: every ModPublic symbol with its id, version,
                 permissions and authority flag - plus, prominently, the scanner's WARNINGS about
                 symbols that look like they were meant to be marked. That warning list is the most
                 useful thing on this tab: an unmarked type in a marked signature is what makes a
                 generated SDK fail to compile for a mod author.
  Extensions     registered extension points and the extensions filed under each.
  Permissions    the permission catalogue and every mod's resolved decisions, with the deciding rule.

Add both windows' menu entries under a single "Mod Framework" section via UToolMenus, and register
them in FModFrameworkEditorModule alongside the existing FModDeveloperWindow::Register() call - read
that file, do not duplicate its registration.

Same rules: refresh on delegates not tick, survive having no game instance, guard tab spawners on
!IsRunningCommandlet(), FAppStyle not FEditorStyle.`,
		{ label: 'editor:gamedev', phase: 'Editor', schema: SCHEMA }),
])

return {
	tests: tests.filter(Boolean),
	sdkgen: sdkgen.filter(Boolean),
	editor: editor.filter(Boolean),
}
