// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModFrameworkEditorModule.h"

#include "Modules/ModuleManager.h"
#include "UObject/NameTypes.h"

// This module is declared "Editor" in ModFramework.uplugin, so WITH_EDITOR is always 1 here. The
// guard is kept anyway so the file states its dependency honestly: FModEditorCommands is a
// TCommands set and FModDeveloperWindow registers a nomad tab, and neither has any meaning - or
// any linkable implementation - outside an editor build.
#if WITH_EDITOR
#include "ModDeveloperWindow.h"
#include "ModEditorCommands.h"
#include "ModSDKWindow.h"
#endif

DEFINE_LOG_CATEGORY(LogModFrameworkEditor);

void FModFrameworkEditorModule::StartupModule()
{
	UE_LOG(LogModFrameworkEditor, Log, TEXT("Mod Framework editor module starting up."));

#if WITH_EDITOR
	// Commands first: the developer window binds its UI actions to the command set while it is
	// registering, so the command set has to exist by then.
	FModEditorCommands::Register();
	FModDeveloperWindow::Register();

	// The SDK window registers its own tab spawner *and* the shared "Mod Framework" menu section that
	// carries the entries for both windows, so it has to come after the developer window's spawner
	// exists - its menu entry invokes that tab.
	FModSDKWindow::Register();
#endif
}

void FModFrameworkEditorModule::ShutdownModule()
{
#if WITH_EDITOR
	// Reverse order of StartupModule: drop the windows (and the bindings they hold) before the
	// commands those bindings point at go away. The SDK window goes first because it owns the menu
	// section whose entries invoke the developer window's tab.
	FModSDKWindow::Unregister();
	FModDeveloperWindow::Unregister();
	FModEditorCommands::Unregister();
#endif

	UE_LOG(LogModFrameworkEditor, Log, TEXT("Mod Framework editor module shut down."));
}

FModFrameworkEditorModule& FModFrameworkEditorModule::Get()
{
	return FModuleManager::LoadModuleChecked<FModFrameworkEditorModule>(FName(GetModuleName()));
}

bool FModFrameworkEditorModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(FName(GetModuleName()));
}

const TCHAR* FModFrameworkEditorModule::GetModuleName()
{
	return TEXT("ModFrameworkEditor");
}

IMPLEMENT_MODULE(FModFrameworkEditorModule, ModFrameworkEditor)
