// Copyright (c) 2026. Licensed for use in your own projects.

using UnrealBuildTool;

public class ModFrameworkDeveloper : ModuleRules
{
	public ModFrameworkDeveloper(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",

				// The runtime framework: manifests, versions, package format, diagnostics.
				"ModFramework",

				// FJsonObject / FJsonSerializer for writing mod.json and SDK descriptor files.
				"Json",

				// FJsonObjectConverter for USTRUCT <-> JSON when emitting generated SDK metadata.
				"JsonUtilities",

				// IPluginManager (Interfaces/IPluginManager.h) for resolving plugin and template paths.
				"Projects"
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// IAssetRegistry / FAssetRegistryModule for enumerating the assets that go into a mod pak.
				"AssetRegistry",

				// ITargetPlatform / ITargetPlatformManagerModule (Interfaces/ITargetPlatform.h) so the
				// packager can resolve the cook target a mod pak is being staged for.
				"TargetPlatform",

				// ExecuteUnrealPak / WritePakFooter (PakFileUtilities.h). Verified present in 5.8 at
				// Engine/Source/Developer/PakFileUtilities and consumed by UnrealEd the same way.
				"PakFileUtilities"
			});

		// UnrealEd and DesktopPlatform MUST stay conditional.
		//
		// "UncookedOnly" resolves to !Target.bBuildRequiresCookedData, which is Editor targets AND
		// Program targets. UnrealEd.Build.cs throws a hard BuildException - "Unable to instantiate
		// UnrealEd module for non-editor targets" - whenever bCompileAgainstEditor is false, so
		// listing it unconditionally makes the entire build fail the moment this plugin is enabled
		// for a program target. That is a failure in someone else's build, triggered by a
		// combination this plugin never tests.
		//
		// Any code behind these must be guarded with WITH_EDITOR.
		if (Target.bCompileAgainstEditor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					// Editor-only tooling used by the mod packager and the SDK generator.
					"UnrealEd",

					// IDesktopPlatform (DesktopPlatformModule.h) for open/save file dialogs used by
					// the packaging entry points.
					"DesktopPlatform"
				});
		}
	}
}
