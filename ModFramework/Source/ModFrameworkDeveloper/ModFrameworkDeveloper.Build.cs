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
				// Editor-only tooling used by the mod packager and the SDK generator.
				// Legal here because "UncookedOnly" modules are only compiled for targets where
				// bBuildRequiresCookedData is false (Editor and Program targets).
				"UnrealEd",

				// IAssetRegistry / FAssetRegistryModule for enumerating the assets that go into a mod pak.
				"AssetRegistry",

				// IDesktopPlatform (DesktopPlatformModule.h) for open/save file dialogs used by the
				// packaging entry points.
				"DesktopPlatform",

				// ITargetPlatform / ITargetPlatformManagerModule (Interfaces/ITargetPlatform.h) so the
				// packager can resolve the cook target a mod pak is being staged for.
				"TargetPlatform",

				// ExecuteUnrealPak / WritePakFooter (PakFileUtilities.h). Verified present in 5.8 at
				// Engine/Source/Developer/PakFileUtilities and consumed by UnrealEd the same way.
				"PakFileUtilities"
			});
	}
}
