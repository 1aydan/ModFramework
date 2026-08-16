// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NameTypes.h"
#include "Templates/SharedPointer.h"

class SDockTab;
class FSpawnTabArgs;

/**
 * The Mod Developer window: the mod author's primary tool surface.
 *
 * Currently a placeholder that registers the tab so the menu path and the module wiring are real.
 * The tabs it will host - mod list, manifest editor with live validation, dependency graph,
 * conflicts, permissions, and one-click cook & package to a chosen output directory - are tracked
 * as the editor tooling phase in docs/internal/Status.md.
 */
class MODFRAMEWORKEDITOR_API FModDeveloperWindow
{
public:
	/** Registers the nomad tab spawner. No-op during commandlet runs, which have no tab manager. */
	static void Register();
	static void Unregister();

	/** Brings the window up, invoking the tab if it is not already open. */
	static void Open();

	static const FName TabId;

private:
	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);
};
