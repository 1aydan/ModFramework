// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Manifest/ModManifest.h"
#include "Misc/CoreMiscDefines.h"
#include "Permissions/ModPermissions.h"
#include "Save/ModSaveTypes.h"
#include "Templates/Tuple.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "ModSystemsTestDoubles.generated.h"

/**
 * Test doubles for the two interfaces a game is expected to implement: IModPermissionPolicy and
 * IModSaveMigration. They exist only for ModSystemsTests.spec.cpp.
 *
 * WHY THIS IS A HEADER AND NOT PART OF THE SPEC FILE
 *
 * Both interfaces are UINTERFACEs, so implementing one needs reflection data, and UnrealHeaderTool
 * only parses headers - never a .cpp. The classes below also cannot be wrapped in
 * `#if WITH_DEV_AUTOMATION_TESTS`: UHT does not evaluate that symbol, so it would emit reflection
 * data for types the compiler never sees in a shipping build. The engine hits exactly the same wall
 * and solves it the same way; see the note in
 * Engine/Source/Runtime/Engine/Private/Tests/Streaming/SSAMTestTypes.h.
 *
 * Both classes are therefore compiled unconditionally. They are Transient, hold no state of their
 * own beyond a recording buffer, are never registered with anything at startup, and are referenced
 * from exactly one automation spec.
 */

/**
 * An IModPermissionPolicy whose verdict for each permission is set by the test.
 *
 * A permission absent from Verdicts yields NotRequested, which is the interface's documented way of
 * saying "no opinion" and hands the decision back to the registry's default rules. Queries records
 * every question the registry asked, in order, so a test can prove the policy really was consulted
 * first rather than after the project settings.
 */
UCLASS(Transient)
class UModSystemsTestPermissionPolicy : public UObject, public IModPermissionPolicy
{
	GENERATED_BODY()

public:
	/** Permission id -> the verdict this policy returns for it. */
	TMap<FName, EModPermissionState> Verdicts;

	/** (mod id, permission id) of every call the registry made, in call order. */
	TArray<TPair<FModId, FName>> Queries;

	//~ Begin IModPermissionPolicy interface
	virtual EModPermissionState ResolvePermission_Implementation(FModManifest const& Manifest, FName PermissionId) override
	{
		Queries.Emplace(Manifest.Id, PermissionId);

		if (const EModPermissionState* Found = Verdicts.Find(PermissionId))
		{
			return *Found;
		}

		return EModPermissionState::NotRequested;
	}
	//~ End IModPermissionPolicy interface
};

/**
 * An IModSaveMigration that records every step it is asked to perform.
 *
 * Each successful step appends "[From->To]" to the record's JSON payload, which lets a test assert
 * both that the walk ran one version at a time and that the steps ran in ascending order - a
 * migration that jumped straight from 1 to 4 would leave a visibly different payload.
 */
UCLASS(Transient)
class UModSystemsTestSaveMigration : public UObject, public IModSaveMigration
{
	GENERATED_BODY()

public:
	/** (FromVersion, ToVersion) of every step the framework asked for, in call order. */
	TArray<TPair<int32, int32>> Steps;

	/** When set, the step whose FromVersion equals this refuses, simulating a mod that cannot upgrade. */
	int32 FailAtFromVersion = INDEX_NONE;

	/** When set, every step tries to rename the record and claim a version of its own choosing. */
	bool bRewriteBookkeeping = false;

	//~ Begin IModSaveMigration interface
	virtual bool MigrateModSave_Implementation(FModId const& ModId, int32 FromVersion, int32 ToVersion, FModSaveRecord& Record) override
	{
		Steps.Emplace(FromVersion, ToVersion);

		if (FromVersion == FailAtFromVersion)
		{
			return false;
		}

		Record.Json += FString::Printf(TEXT("[%d->%d]"), FromVersion, ToVersion);

		if (bRewriteBookkeeping)
		{
			// The framework is expected to overwrite both of these after every step.
			Record.ModId = FModId(FName(TEXT("some.other.mod")));
			Record.DataVersion = 9999;
		}

		return true;
	}
	//~ End IModSaveMigration interface
};
