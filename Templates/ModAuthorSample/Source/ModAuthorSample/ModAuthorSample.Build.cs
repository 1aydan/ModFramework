// Copyright (c) 2026. Licensed for use in your own projects.

using UnrealBuildTool;

public class ModAuthorSample : ModuleRules
{
	public ModAuthorSample(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// THIS LIST IS THE BOUNDARY TEST.
		//
		// A mod author gets the SDK and the framework it depends on - nothing else. There is
		// deliberately no reference to ModFrameworkSample here, and there must never be one: adding
		// it would make this project build for the wrong reason and destroy the only automated
		// evidence that the SDK is self-sufficient.
		//
		// If something a mod needs is not reachable from GameModSDK, the fix is to expose it through
		// a UModAPI in the SDK, not to widen this list.
		// Note there is no "ModFramework" here either. GameModSDK depends on it publicly, so its
		// headers and types come through transitively. A mod author depends on the SDK; the
		// framework is plumbing they never name.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameModSDK"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
