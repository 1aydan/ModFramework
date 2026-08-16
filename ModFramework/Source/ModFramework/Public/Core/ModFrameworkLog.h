// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Logging/LogMacros.h"

/**
 * The single log category for the whole framework.
 *
 * Defined in Private/ModFrameworkModule.cpp with DEFINE_LOG_CATEGORY(LogModFramework).
 * Everything the framework logs - discovery, validation, resolution, mounting, lifecycle,
 * permissions and networking - goes through this category so a game can silence or raise the
 * verbosity of all mod chatter with a single -LogCmds="LogModFramework Verbose" switch.
 *
 * Note that mod-supplied data is untrusted: log it, never assert on it.
 */
MODFRAMEWORK_API DECLARE_LOG_CATEGORY_EXTERN(LogModFramework, Log, All);
