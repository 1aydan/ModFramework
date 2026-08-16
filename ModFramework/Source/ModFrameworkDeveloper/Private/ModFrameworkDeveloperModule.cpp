// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModFrameworkDeveloperModule.h"

#include "Modules/ModuleManager.h"
#include "UObject/NameTypes.h"

DEFINE_LOG_CATEGORY(LogModFrameworkDeveloper);

void FModFrameworkDeveloperModule::StartupModule()
{
	UE_LOG(LogModFrameworkDeveloper, Log, TEXT("Mod Framework developer module starting up."));
}

void FModFrameworkDeveloperModule::ShutdownModule()
{
	UE_LOG(LogModFrameworkDeveloper, Log, TEXT("Mod Framework developer module shut down."));
}

FModFrameworkDeveloperModule& FModFrameworkDeveloperModule::Get()
{
	return FModuleManager::LoadModuleChecked<FModFrameworkDeveloperModule>(FName(GetModuleName()));
}

bool FModFrameworkDeveloperModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(FName(GetModuleName()));
}

const TCHAR* FModFrameworkDeveloperModule::GetModuleName()
{
	return TEXT("ModFrameworkDeveloper");
}

IMPLEMENT_MODULE(FModFrameworkDeveloperModule, ModFrameworkDeveloper)
