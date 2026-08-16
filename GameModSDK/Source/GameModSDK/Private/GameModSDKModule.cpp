// Copyright (c) 2026. Licensed for use in your own projects.

#include "GameModSDKModule.h"

#define LOCTEXT_NAMESPACE "FGameModSDKModule"

DEFINE_LOG_CATEGORY(LogGameModSDK);

void FGameModSDKModule::StartupModule()
{
	UE_LOG(LogGameModSDK, Log, TEXT("Game Mod SDK loaded."));
}

void FGameModSDKModule::ShutdownModule()
{
	UE_LOG(LogGameModSDK, Log, TEXT("Game Mod SDK unloaded."));
}

FGameModSDKModule& FGameModSDKModule::Get()
{
	return FModuleManager::LoadModuleChecked<FGameModSDKModule>(GetModuleName());
}

bool FGameModSDKModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(GetModuleName());
}

const TCHAR* FGameModSDKModule::GetModuleName()
{
	return TEXT("GameModSDK");
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGameModSDKModule, GameModSDK)
