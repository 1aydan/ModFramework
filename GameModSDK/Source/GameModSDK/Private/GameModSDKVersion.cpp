// Copyright (c) 2026. Licensed for use in your own projects.

#include "GameModSDKVersion.h"

#include "Containers/UnrealString.h"
#include "Manifest/ModVersion.h"

// A zero version is how the framework spells "this build declares no SDK version", and it makes
// FModDependencyResolver skip the SDK version check entirely (see the SdkVersion.IsZero() branch in
// ModDependencyResolver.cpp). An SDK that ships as 0.0.0 would therefore accept every mod whatever
// it pinned, which is precisely the failure this file exists to prevent.
static_assert(
	GAMEMODSDK_VERSION_MAJOR + GAMEMODSDK_VERSION_MINOR + GAMEMODSDK_VERSION_PATCH > 0,
	"The SDK version must not be 0.0.0: the mod framework reads that as 'no SDK version declared' and stops checking mods against it.");

namespace GameModSDK
{
	FString GetSdkId()
	{
		return FString(GAMEMODSDK_SDK_ID);
	}

	FModVersion GetSdkVersion()
	{
		// Built fresh on every call rather than cached in a static, for the reason
		// ModFrameworkVersion::Get() gives: FModVersion holds FStrings, and static initialization
		// order across translation units is not settled while modules are still starting up.
		return FModVersion(GAMEMODSDK_VERSION_MAJOR, GAMEMODSDK_VERSION_MINOR, GAMEMODSDK_VERSION_PATCH);
	}

	FString GetSdkVersionString()
	{
		return GetSdkVersion().ToString();
	}
}
