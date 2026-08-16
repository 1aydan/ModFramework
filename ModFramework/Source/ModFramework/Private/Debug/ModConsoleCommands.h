// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"

/**
 * Registration for the Mod.* console commands.
 *
 * NOTE: ModFramework loads at PostConfigInit, BEFORE the UObject system exists. Register() must not
 * read UModFrameworkSettings or touch any CDO - the command objects are created unconditionally and
 * the bEnableConsoleCommands gate is evaluated inside each command's execution callback instead.
 */
class FModConsoleCommands
{
public:
	static void Register();
	static void Unregister();
};
