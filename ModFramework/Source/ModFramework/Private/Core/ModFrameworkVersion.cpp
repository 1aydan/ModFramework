// Copyright (c) 2026. Licensed for use in your own projects.

#include "Core/ModFrameworkVersion.h"

#include "Containers/UnrealString.h"
#include "Manifest/ModVersion.h"

namespace ModFrameworkVersion
{
	FModVersion Get()
	{
		// Deliberately built fresh on every call rather than cached in a static: FModVersion holds
		// FStrings and this can be called during module startup, before static init ordering across
		// translation units is settled.
		return FModVersion(MODFRAMEWORK_VERSION_MAJOR, MODFRAMEWORK_VERSION_MINOR, MODFRAMEWORK_VERSION_PATCH);
	}

	FString GetString()
	{
		return Get().ToString();
	}
}
