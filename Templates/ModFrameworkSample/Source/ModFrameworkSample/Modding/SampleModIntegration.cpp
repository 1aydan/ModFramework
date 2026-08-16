// Copyright (c) 2026. Licensed for use in your own projects.

#include "SampleModIntegration.h"

#include "Events/GameModEvents.h"
#include "Extensions/GameModSDKExtensionPoints.h"
#include "Logging/LogMacros.h"
#include "SampleCombatModAPI.h"
#include "SampleUIModAPI.h"
#include "SampleWorldModAPI.h"
#include "Subsystem/ModSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSampleModIntegration, Log, All);

void USampleModIntegrationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	// UModSubsystem is a sibling UGameInstanceSubsystem and initialisation order between siblings is
	// not defined. Without this, registration would work or not depending on which happened to be
	// constructed first - the sort of bug that reproduces on one machine and nowhere else.
	Collection.InitializeDependency<UModSubsystem>();

	Super::Initialize(Collection);

	UModSubsystem* ModSubsystem = GetGameInstance() != nullptr
		? GetGameInstance()->GetSubsystem<UModSubsystem>()
		: nullptr;

	if (ModSubsystem == nullptr)
	{
		// Not fatal: the game must still run with the framework disabled or absent.
		UE_LOG(LogSampleModIntegration, Warning,
			TEXT("ModFramework is unavailable; this game will run without mod support."));
		return;
	}

	// --- Extension points ---------------------------------------------------------------------
	// These declare the shapes a mod may plug into. They must be registered before any mod
	// activates, or a mod's extension has nothing to attach to.
	int32 PointsRegistered = 0;
	for (const FModExtensionPointDescriptor& Descriptor : BuildGameExtensionPointDescriptors())
	{
		if (ModSubsystem->RegisterExtensionPoint(Descriptor))
		{
			++PointsRegistered;
		}
		else
		{
			UE_LOG(LogSampleModIntegration, Error,
				TEXT("Failed to register extension point '%s'."), *Descriptor.ExtensionPointId.ToString());
		}
	}

	// --- Events -------------------------------------------------------------------------------
	int32 EventsRegistered = 0;
	for (const FModEventDescriptor& Descriptor : BuildGameEventDescriptors())
	{
		if (ModSubsystem->RegisterEventType(Descriptor))
		{
			++EventsRegistered;
		}
		else
		{
			UE_LOG(LogSampleModIntegration, Error,
				TEXT("Failed to register event type '%s'."), *Descriptor.EventId.ToString());
		}
	}

	// --- APIs ---------------------------------------------------------------------------------
	// The SDK declares these abstractly; these concrete subclasses are what actually touch game
	// code. Outered to this subsystem so their lifetime matches the game instance.
	CombatAPI = NewObject<USampleCombatModAPI>(this);
	WorldAPI = NewObject<USampleWorldModAPI>(this);
	UIAPI = NewObject<USampleUIModAPI>(this);

	const bool bCombat = ModSubsystem->RegisterGameAPI(CombatAPI);
	const bool bWorld = ModSubsystem->RegisterGameAPI(WorldAPI);
	const bool bUI = ModSubsystem->RegisterGameAPI(UIAPI);

	bRegistered = bCombat && bWorld && bUI
		&& PointsRegistered == GameModExtensionPoints::GetAllPointIds().Num()
		&& EventsRegistered > 0;

	UE_LOG(LogSampleModIntegration, Log,
		TEXT("Mod surface registered: %d extension points, %d events, APIs [combat %s, world %s, ui %s]."),
		PointsRegistered, EventsRegistered,
		bCombat ? TEXT("ok") : TEXT("FAILED"),
		bWorld ? TEXT("ok") : TEXT("FAILED"),
		bUI ? TEXT("ok") : TEXT("FAILED"));
}

void USampleModIntegrationSubsystem::Deinitialize()
{
	// The framework unregisters everything during its own teardown, and UModSubsystem may already
	// be gone by the time we get here. Dropping the references is all that is needed; the widgets
	// USampleUIModAPI created are cleaned up in its OnAPIUnregistered.
	CombatAPI = nullptr;
	WorldAPI = nullptr;
	UIAPI = nullptr;
	bRegistered = false;

	Super::Deinitialize();
}
