// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModFrameworkModule.h"

#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkVersion.h"
#include "Misc/Build.h"
#include "Modules/ModuleManager.h"
#include "UObject/NameTypes.h"

// The console commands only exist where a console can reach them. Matching the guard here as well
// as inside the command implementation keeps shipping builds free of the registration call.
#if !UE_BUILD_SHIPPING || ALLOW_CONSOLE
#include "Debug/ModConsoleCommands.h"
#endif

DEFINE_LOG_CATEGORY(LogModFramework);

void FModFrameworkModule::StartupModule()
{
	// PostConfigInit: the UObject system is not up yet, so nothing here may touch a CDO
	// (UModFrameworkSettings included). Console command registration must only talk to
	// IConsoleManager and defer any settings lookup to the point the command is executed.
	UE_LOG(LogModFramework, Log,
		TEXT("Mod Framework %s starting up (manifest schema v%d, package format v%d)."),
		*ModFrameworkVersion::GetString(),
		MODFRAMEWORK_MANIFEST_VERSION,
		MODFRAMEWORK_PACKAGE_FORMAT_VERSION);

#if !UE_BUILD_SHIPPING || ALLOW_CONSOLE
	FModConsoleCommands::Register();
#endif
}

void FModFrameworkModule::ShutdownModule()
{
	// Torn down in the reverse order of StartupModule.
#if !UE_BUILD_SHIPPING || ALLOW_CONSOLE
	FModConsoleCommands::Unregister();
#endif

	UE_LOG(LogModFramework, Log, TEXT("Mod Framework shut down."));
}

FModFrameworkModule& FModFrameworkModule::Get()
{
	return FModuleManager::LoadModuleChecked<FModFrameworkModule>(FName(GetModuleName()));
}

bool FModFrameworkModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(FName(GetModuleName()));
}

const TCHAR* FModFrameworkModule::GetModuleName()
{
	return TEXT("ModFramework");
}

IMPLEMENT_MODULE(FModFrameworkModule, ModFramework)
