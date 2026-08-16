// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ModFrameworkSample : ModuleRules
{
	public ModFrameworkSample(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			// The game installs the framework and defines its public modding surface in the SDK.
			// Both are private: the game's own headers must never leak modding types into whatever
			// includes them, and nothing here should tempt the SDK into depending back on the game.
			"ModFramework",
			"GameModSDK"
		});

		PublicIncludePaths.AddRange(new string[] {
			"ModFrameworkSample",
			"ModFrameworkSample/Modding",
			"ModFrameworkSample/Variant_Platforming",
			"ModFrameworkSample/Variant_Platforming/Animation",
			"ModFrameworkSample/Variant_Combat",
			"ModFrameworkSample/Variant_Combat/AI",
			"ModFrameworkSample/Variant_Combat/Animation",
			"ModFrameworkSample/Variant_Combat/Gameplay",
			"ModFrameworkSample/Variant_Combat/Interfaces",
			"ModFrameworkSample/Variant_Combat/UI",
			"ModFrameworkSample/Variant_SideScrolling",
			"ModFrameworkSample/Variant_SideScrolling/AI",
			"ModFrameworkSample/Variant_SideScrolling/Gameplay",
			"ModFrameworkSample/Variant_SideScrolling/Interfaces",
			"ModFrameworkSample/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
