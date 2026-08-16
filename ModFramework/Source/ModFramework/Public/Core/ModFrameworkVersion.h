// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/UnrealString.h"
#include "Manifest/ModVersion.h"

/**
 * Semantic version of the framework itself.
 *
 * Bump MAJOR for a breaking change to any public API or to the manifest schema, MINOR for additive
 * changes, PATCH for fixes. Mod manifests constrain the framework they run against through the
 * "framework": { "version": "^0.1.0" } field, which is matched against ModFrameworkVersion::Get().
 */
#define MODFRAMEWORK_VERSION_MAJOR 0
#define MODFRAMEWORK_VERSION_MINOR 1
#define MODFRAMEWORK_VERSION_PATCH 0

/**
 * Schema version of mod.json. A manifest declaring a higher value than this is rejected with
 * EModLoadFailureReason::UnsupportedManifestVersion rather than being parsed on a guess.
 */
#define MODFRAMEWORK_MANIFEST_VERSION 1

/**
 * Container format version of the .mod package. Bumping it invalidates previously written packages
 * unless FModPackageReader is taught to read the older layout.
 */
#define MODFRAMEWORK_PACKAGE_FORMAT_VERSION 1

namespace ModFrameworkVersion
{
	/** Semantic version of the framework itself. */
	MODFRAMEWORK_API FModVersion Get();

	/** The same version rendered as "MAJOR.MINOR.PATCH". */
	MODFRAMEWORK_API FString GetString();
}
