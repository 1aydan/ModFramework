// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Manifest/ModManifest.h"
#include "UObject/ObjectMacros.h"

#include "ModManifestParser.generated.h"

/**
 * Outcome of reading one `mod.json`.
 *
 * bSuccess is true only when no diagnostic has EModDiagnosticSeverity::Error. Manifest is still
 * filled in as far as parsing got even when bSuccess is false, so tooling can show the author what
 * it managed to read alongside the errors.
 */
USTRUCT(BlueprintType)
struct MODFRAMEWORK_API FModManifestParseResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	FModManifest Manifest;

	UPROPERTY(BlueprintReadOnly, Category = "Mod")
	TArray<FModDiagnostic> Diagnostics;
};

/**
 * Reader, writer and validator for the mod manifest format.
 *
 * Everything a mod ships is untrusted input, so the parser never asserts: malformed JSON, wrong
 * value types, unknown fields, hostile paths and absurd numbers all come back as FModDiagnostic
 * entries with a stable machine-readable Code and a Context that names the exact field.
 *
 * Diagnostic codes produced here (all of them stable, never renamed):
 *   Manifest.ReadFailed                 the file could not be opened or is implausibly large
 *   Manifest.InvalidJson                the text is not a JSON object
 *   Manifest.MissingField               a required field is absent or empty
 *   Manifest.InvalidField               a field has the wrong JSON type or an out-of-range value
 *   Manifest.InvalidId                  a mod id does not satisfy FModId::IsValidIdString
 *   Manifest.InvalidVersion             `version` is not a semantic version, or is 0.0.0
 *   Manifest.InvalidVersionRange        a version constraint expression does not parse
 *   Manifest.UnsupportedManifestVersion `manifestVersion` is newer than this framework understands
 *   Manifest.UnknownField               (Warning) a key the schema does not define
 *   Manifest.DuplicateDependency        the same dependency id appears twice
 *   Manifest.SelfDependency             the mod depends on, or orders itself against, itself
 *   Manifest.InvalidPermission          a permission name is empty or contains whitespace
 *   Manifest.InvalidEntryPoint          the entry class, a content bundle or the native module name is malformed
 *   Manifest.InvalidContentRoot         a content path escapes the mod root, or a mount point is malformed
 *   Manifest.EmptyContent               a content root declares an empty path
 *   Manifest.InvalidClaim               a resource claim has an empty or malformed id
 *
 * Version parsing note: the mod's own `version` is the number it publishes under, so it is parsed
 * strictly - "2.1.0" is accepted, "2.1" is not. Version *constraints* (`game.version`,
 * `framework.version`, `sdk.version`, `dependencies[].version`) keep the forgiving partial syntax
 * documented on FModVersionRange, so "^1.2" and "1.x" still work there.
 */
class MODFRAMEWORK_API FModManifestParser
{
public:
	/** The canonical manifest file name inside a mod root: "mod.json". */
	static const TCHAR* GetManifestFileName();

	/**
	 * Parses manifest JSON held in memory.
	 *
	 * @param JsonText     The raw file contents.
	 * @param ContextPath  Optional path (or any label) reported alongside every diagnostic so a
	 *                     caller scanning many mods can tell which one produced the message.
	 */
	static FModManifestParseResult ParseFromString(const FString& JsonText, const FString& ContextPath = FString());

	/** Loads and parses a manifest file. A missing or unreadable file yields Manifest.ReadFailed. */
	static FModManifestParseResult ParseFromFile(const FString& AbsoluteFilePath);

	/**
	 * Writes a manifest back out in the canonical shape.
	 *
	 * Fields sitting at their default value are omitted to keep authored manifests small, except
	 * for the four the schema requires (`id`, `name`, `version`, `game.id`) and `manifestVersion`,
	 * which is always written because it is the schema discriminator. The result round-trips:
	 * ParseFromString(SerializeToString(M)) reproduces every field of any M that itself came out of
	 * ParseFromString.
	 */
	static bool SerializeToString(const FModManifest& In, FString& OutJson, bool bPrettyPrint = true);

	/** Serializes and writes to disk as UTF-8, creating the containing directory when needed. */
	static bool SerializeToFile(const FModManifest& In, const FString& AbsoluteFilePath);

	/**
	 * Semantic validation independent of JSON shape - safe to run on a manifest that was built in
	 * code or deserialized from a package rather than read from `mod.json`. Appends to
	 * OutDiagnostics and never clears it.
	 */
	static void ValidateManifest(const FModManifest& In, const FString& ContextPath, TArray<FModDiagnostic>& OutDiagnostics);
};
