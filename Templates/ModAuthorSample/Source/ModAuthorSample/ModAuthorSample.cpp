// Copyright (c) 2026. Licensed for use in your own projects.

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

// A mod author writing a Blueprint/Data Asset mod needs no C++ at all. This module exists only so
// the project can compile the plugins from source in this repository - a real mod author receiving
// a generated SDK bundle gets precompiled binaries and can use a content-only project.
IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, ModAuthorSample, "ModAuthorSample");
