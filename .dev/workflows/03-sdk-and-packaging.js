export const meta = {
	name: 'modframework-sdk-and-packaging',
	description: 'GameModSDK public surface for the Combat variant, game-side registration, .mod packaging and SDK generation',
	phases: [
		{ title: 'SDK', detail: 'extension points, APIs, events, data assets' },
		{ title: 'Game', detail: 'sample game registers its APIs and extension points' },
		{ title: 'Tooling', detail: 'package writer and SDK generator' },
	],
}

const REPO = 'F:/SelfProjects/Unreal/Plugins/ModFramework'
const FW = `${REPO}/ModFramework`
const SDK = `${REPO}/GameModSDK`
const SAMPLE = `${REPO}/Templates/ModFrameworkSample`

const PREAMBLE = `You are extending a UE 5.8 modding framework. The framework runtime is COMPLETE, COMPILES CLEAN and
must not be modified unless your task says so.

MANDATORY FIRST STEP: read the real headers you depend on. They are the truth; do not guess a
signature. The ones that matter most:
  ${FW}/Source/ModFramework/Public/Subsystem/ModSubsystem.h
  ${FW}/Source/ModFramework/Public/API/ModAPI.h
  ${FW}/Source/ModFramework/Public/API/ModAPIRegistry.h
  ${FW}/Source/ModFramework/Public/Extensions/ModExtension.h
  ${FW}/Source/ModFramework/Public/Extensions/ModExtensionRegistry.h
  ${FW}/Source/ModFramework/Public/Events/ModEventTypes.h
  ${FW}/Source/ModFramework/Public/Events/ModEventBus.h
  ${FW}/Source/ModFramework/Public/Core/ModFrameworkTypes.h
  ${FW}/Source/ModFramework/Public/Runtime/ModContext.h
Design reference (accurate but NOT authoritative where it disagrees with a header):
  ${REPO}/docs/internal/ImplementationContract.md

ENGINE SOURCE: F:/SelfProjects/Unreal/UE_5.8/Engine/Source — grep it to verify any engine API.

TWO BUGS ALREADY FOUND IN THIS CODEBASE — do not reintroduce:
1. TArray<TObjectPtr<T>>::Sort routes through TDereferenceWrapper, which calls Predicate(*A, *B).
   The predicate takes \`const T&\`, NOT \`const TObjectPtr<T>&\`, a null check inside it is
   unreachable, and a null slot crashes inside the engine. RemoveAll invalid entries BEFORE sorting.
2. Never name a file-local helper \`MakeError\` — it collides with the global variadic template in
   Templates/ValueOrError.h and produces an unreadable TValueOrError_ErrorProxy error.

THE ARCHITECTURAL RULE YOU MUST NOT BREAK:
  GameModSDK may NEVER reference the game's own module (ModFrameworkSample). Its Build.cs
  PrivateDependencyModuleNames is deliberately empty. A mod author installs the SDK WITHOUT the
  game's source, so anything the SDK needs from the game must go through a UModAPI the game
  registers at runtime. If you find yourself wanting to #include a game header from the SDK, the
  design is wrong — declare an API instead.

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

// The concrete public surface. Fixed here so parallel agents cannot drift.
const SURFACE = `
THE GAME'S PUBLIC MODDING SURFACE (fixed — use these exact ids and names):

Extension points, all declared in GameModSDKExtensionPoints.h as namespace GameModExtensionPoints:
  "game.weapon"    base UGameWeaponExtension    policy Priority   server-authoritative
  "game.enemy"     base UGameEnemyExtension     policy Priority   server-authoritative
  "game.gamerule"  base UGameRuleExtension      policy Merge      server-authoritative
  "game.ui"        base UGameUIExtension        policy Merge      NOT server-authoritative

APIs (each a UModAPI subclass, ModPublic, with BOTH class metadata and native overrides — metadata
is stripped in shipping builds, so GetApiId/GetApiVersion must be overridden natively too):
  UGameCombatModAPI   id "game.combat"  v1.0.0  perms "gameplay.modify"  server-authoritative
  UGameWorldModAPI    id "game.world"   v1.0.0  perms "gameplay.modify"  server-authoritative
  UGameUIModAPI       id "game.ui"      v1.0.0  perms "ui.modify"        NOT server-authoritative

Events, in namespace GameModEvents:
  "Game.EnemyKilled"   payload FGameEnemyKilledEvent
  "Game.PlayerDamaged" payload FGamePlayerDamagedEvent
  "Game.WaveStarted"   payload FGameWaveStartedEvent
All payloads derive from FModEventPayload. All server-authoritative.

Data assets (UPrimaryDataAsset, ModPublic, the data-driven authoring path):
  UGameWeaponDefinition  — display name, damage, attack rate, combo count, icon, mesh, montages
  UGameEnemyDefinition   — display name, health, damage, move speed, class to spawn, icon
`

phase('SDK')

const sdk = await parallel([
	() => agent(`${PREAMBLE}
${SURFACE}

YOUR SCOPE: the extension points and extension base classes. Write under ${SDK}/Source/GameModSDK/:
  Public/Extensions/GameModSDKExtensionPoints.h  + Private/.../.cpp   (the id constants + descriptors)
  Public/Extensions/GameWeaponExtension.h        + Private/.../.cpp
  Public/Extensions/GameEnemyExtension.h         + Private/.../.cpp
  Public/Extensions/GameRuleExtension.h          + Private/.../.cpp
  Public/Extensions/GameUIExtension.h            + Private/.../.cpp

Each extension base class:
- derives UModExtension, is UCLASS(Abstract, Blueprintable, BlueprintType, meta=(ModPublic, ModExtensionPoint="game.xxx"))
- sets its ExtensionPointId in the constructor so a Blueprint author never has to
- exposes BlueprintImplementableEvent hooks meaningful for that point. Weapon: GetWeaponDefinition,
  ModifyOutgoingDamage(float BaseDamage, AActor* Target) -> float. Enemy: GetEnemyDefinition,
  ModifyEnemyHealth(float Base) -> float, OnEnemySpawned(AActor*). Rule: OnWaveStarted(int32),
  OnGameStarted, ShouldAllowSpawn(FName EnemyId) -> bool. UI: GetWidgetClass, GetSortPriority.
- has native virtual counterparts with sensible defaults that call the BP event when implemented.

GameModSDKExtensionPoints.h declares:
  namespace GameModExtensionPoints { GAMEMODSDK_API extern const FName Weapon; ... }
  GAMEMODSDK_API TArray<FModExtensionPointDescriptor> BuildGameExtensionPointDescriptors();
so the game registers all four with one call.

Extensions must never reach the game directly — a weapon extension changes numbers and returns data;
applying it is the game's job through UGameCombatModAPI.`,
		{ label: 'sdk:extensions', phase: 'SDK', schema: SCHEMA }),

	() => agent(`${PREAMBLE}
${SURFACE}

YOUR SCOPE: the APIs, events and data assets. Write under ${SDK}/Source/GameModSDK/:
  Public/API/GameCombatModAPI.h   + Private/.../.cpp
  Public/API/GameWorldModAPI.h    + Private/.../.cpp
  Public/API/GameUIModAPI.h       + Private/.../.cpp
  Public/Events/GameModEvents.h   + Private/.../.cpp
  Public/Data/GameWeaponDefinition.h + Private/.../.cpp
  Public/Data/GameEnemyDefinition.h  + Private/.../.cpp

CRITICAL DESIGN POINT: these API classes are ABSTRACT DECLARATIONS of what mods may do. They must
NOT implement game behaviour — the game subclasses them and registers an instance. So every API
method is a BlueprintCallable virtual with a safe default (log + return false/zero), and the game
overrides it. Document that in each header.

UGameCombatModAPI: ApplyDamage(AActor* Target, float Amount, FName DamageType) -> bool;
  GetHealth(AActor*) -> float; GetMaxHealth(AActor*) -> float; IsAlive(AActor*) -> bool;
  GetPlayerPawn(int32 PlayerIndex) -> APawn*.
UGameWorldModAPI: SpawnEnemy(FName EnemyId, FTransform) -> AActor*; GetActiveEnemyCount() -> int32;
  GetCurrentWave() -> int32; DespawnEnemy(AActor*) -> bool.
UGameUIModAPI: AddWidgetToHUD(TSubclassOf<UUserWidget>, int32 ZOrder) -> UUserWidget*;
  RemoveWidgetFromHUD(UUserWidget*) -> bool; ShowNotification(FText, float Duration).

Every server-authoritative API method must call VerifyServerAuthority(this, TEXT("MethodName"))
and bail when it returns false — read UModAPI.h for its exact signature first.

GameModEvents.h: the three FName constants, the three payload USTRUCTs deriving FModEventPayload,
and GAMEMODSDK_API TArray<FModEventDescriptor> BuildGameEventDescriptors() so the game registers all
three in one call.

Data assets: UPrimaryDataAsset subclasses with GetPrimaryAssetId overridden to a stable type name
("GameWeaponDefinition" / "GameEnemyDefinition"). Use TSoftObjectPtr for mesh/icon and
TSoftClassPtr for classes so a mod's data asset does not drag content into memory on load.`,
		{ label: 'sdk:api', phase: 'SDK', schema: SCHEMA }),
])

log(`SDK surface: ${sdk.filter(Boolean).length}/2`)

phase('Game')

const game = await agent(`${PREAMBLE}
${SURFACE}

YOUR SCOPE: the game side — the sample game implements and registers its modding surface.
READ the SDK headers just written under ${SDK}/Source/GameModSDK/Public/ before writing anything.

Write under ${SAMPLE}/Source/ModFrameworkSample/Modding/:
  SampleModIntegration.h + .cpp   — a UGameInstanceSubsystem, USampleModIntegrationSubsystem, that
    on Initialize: gets UModSubsystem, registers the four extension point descriptors, the three
    event descriptors, and one instance of each concrete API subclass below. On Deinitialize it
    tears them down. It must degrade gracefully when UModSubsystem is unavailable.
  SampleCombatModAPI.h + .cpp     — UGameCombatModAPI subclass wired to the Combat variant
  SampleWorldModAPI.h + .cpp      — UGameWorldModAPI subclass
  SampleUIModAPI.h + .cpp         — UGameUIModAPI subclass

READ THE GAME'S OWN CODE FIRST to wire these correctly — do not invent APIs:
  ${SAMPLE}/Source/ModFrameworkSample/Variant_Combat/CombatCharacter.h
  ${SAMPLE}/Source/ModFrameworkSample/Variant_Combat/AI/CombatEnemy.h
  ${SAMPLE}/Source/ModFrameworkSample/Variant_Combat/AI/CombatEnemySpawner.h
  ${SAMPLE}/Source/ModFrameworkSample/Variant_Combat/CombatGameMode.h
  and any damage/health interface under Variant_Combat/Interfaces/.
Use what actually exists. Where the game genuinely has no equivalent (e.g. no wave counter), return
a documented safe default rather than inventing state — and say so in your notes.

DO NOT heavily edit existing game files. Add new files. If a hook is unavoidable, make it the
smallest possible addition and describe it in your notes.

Also set the framework's project identity in ${SAMPLE}/Config/DefaultGame.ini under
[/Script/ModFramework.ModFrameworkSettings]: GameId=com.modframework.sample, GameVersion=1.0.0,
SdkId=com.modframework.sample.sdk, SdkVersion=0.1.0, and bAllowLooseContentMounts=True (the example
mod uses a loose content root). Read ModFrameworkSettings.h for the exact property names and the
correct ini syntax for its array properties.`,
	{ label: 'game:integration', phase: 'Game', schema: SCHEMA })

phase('Tooling')

const tooling = await parallel([
	() => agent(`${PREAMBLE}

YOUR SCOPE: mod packaging — the writer half of the .mod container.
Write under ${FW}/Source/ModFrameworkDeveloper/:
  Public/Packaging/ModPackageWriter.h + Private/Packaging/ModPackageWriter.cpp
  Public/Packaging/ModPackagerCommandlet.h + Private/.../.cpp   (UPackageModCommandlet, -run=PackageMod)

READ ${FW}/Source/ModFramework/Public/Packaging/ModPackageFormat.h FIRST and match its documented
byte layout EXACTLY. The reader is already written, compiles, and is the specification — a writer
that disagrees by one byte produces packages nothing can open. Read the reader's .cpp too
(${FW}/Source/ModFramework/Private/Packaging/ModPackageFormat.cpp) to see precisely how it parses.

FModPackageWriter: Open(destination), AddFile(relativePath, absoluteSourcePath), AddBytes(relativePath,
bytes), SetManifest(FModManifest), Close(diagnostics). It must compute per-entry SHA-1, honour the
compression flag, patch the header offsets after the fact, and refuse unsafe relative paths using the
same rules the reader enforces. Add a static PackageDirectory(sourceDir, destFile, diagnostics) that
packages a whole mod folder: reads mod.json, includes the icon, walks the tree, skips
Intermediate/Binaries/Saved and any dotfile.

VERIFY YOUR WORK: after writing, mentally round-trip it against the reader's parsing code, field by
field, and state in your notes that you did and what you checked.

Remember ModFrameworkDeveloper's UnrealEd/DesktopPlatform deps are behind
if (Target.bCompileAgainstEditor) — guard any code needing them with WITH_EDITOR.`,
		{ label: 'tool:packager', phase: 'Tooling', schema: SCHEMA }),

	() => agent(`${PREAMBLE}

YOUR SCOPE: SDK generation — scan the project's reflection data for ModPublic symbols and assemble a
publishable bundle.
Write under ${FW}/Source/ModFrameworkDeveloper/:
  Public/SDK/ModPublicApiScanner.h  + Private/SDK/ModPublicApiScanner.cpp
  Public/SDK/ModSDKGenerator.h      + Private/SDK/ModSDKGenerator.cpp
  Public/SDK/ModSDKCommandlet.h     + Private/SDK/ModSDKCommandlet.cpp  (UGenerateModSDKCommandlet, -run=GenerateModSDK)

FModPublicApiScanner: walk TObjectRange<UClass>/UScriptStruct/UEnum, collect anything whose metadata
has "ModPublic", plus ModApiId/ModApiVersion/ModApiPermissions/ModApiServerAuthoritative/
ModExtensionPoint/ModSince/ModDeprecated. Verify how to read metadata in 5.8 (UField::GetMetaData /
HasMetaData) and note that it is WITH_EDITORONLY_DATA — this is editor-time tooling so that is fine,
but say so in a comment. Produce a structured FModPublicApiReport: classes, structs, enums,
interfaces, functions and properties, each with its owning module, header path and metadata.
Warn about likely mistakes: a UModAPI subclass with no ModApiId; a ModPublic class whose
BlueprintCallable functions are all unmarked; a ModPublic type exposing an unmarked type in its
signature (that last one is the leak that breaks a generated SDK's compile).

FModSDKGenerator::GenerateBundle(FModSDKGenerateOptions, diagnostics) assembles:
  <Output>/<SdkName>-<Version>/
    Plugins/ModFramework/      copied from the sibling plugin folder, EXCLUDING Binaries,
                               Intermediate, Saved and .dev
    Plugins/<SdkName>/         copied from the SDK plugin folder, same exclusions
    Docs/                      copied from repo docs/, EXCLUDING docs/internal
    SDKVersion.json            all five versions (UE, game, framework, SDK) + the API index from the scanner
    README.md                  generated, mod-author facing
Locate the plugin folders with IPluginManager rather than hardcoding paths.
docs/internal MUST be excluded — it contains the internal status and contract documents.

Nothing here may be required at runtime by a mod.`,
		{ label: 'tool:sdkgen', phase: 'Tooling', schema: SCHEMA }),
])

return {
	sdk: sdk.filter(Boolean),
	game,
	tooling: tooling.filter(Boolean),
}
