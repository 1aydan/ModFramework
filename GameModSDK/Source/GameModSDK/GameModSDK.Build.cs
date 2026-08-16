// Copyright (c) 2026. Licensed for use in your own projects.

using UnrealBuildTool;

public class GameModSDK : ModuleRules
{
	public GameModSDK(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		// Everything here is public on purpose: this module IS the mod-facing surface, so a mod
		// author's project must be able to see all of it after installing only the SDK bundle.
		//
		// UMG is here because UGameUIModAPI's public signature names UUserWidget, as does
		// UGameUIExtension::GetWidgetClass. It is an engine module, not game code, so it does not
		// weaken the rule below.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ModFramework",
			"UMG"
		});

		// The SDK must never depend on the game's own module. If this list ever needs to grow to
		// reach game code, the API being added belongs behind a UModAPI instead.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
