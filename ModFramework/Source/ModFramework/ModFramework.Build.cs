// Copyright (c) 2026. Licensed for use in your own projects.

using UnrealBuildTool;

public class ModFramework : ModuleRules
{
	public ModFramework(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				// Containers, FString/FName/FText, FSHA1 (Misc/SecureHash.h), FBase64 (Misc/Base64.h),
				// FCompression (Misc/Compression.h), FPaths, IFileManager, console variables.
				"Core",

				// UObject/UClass reflection, USTRUCT support, FSoftObjectPath/FSoftClassPath,
				// TInstancedStruct (StructUtils/InstancedStruct.h) and FAssetData (AssetRegistry/AssetData.h).
				"CoreUObject",

				// UGameInstanceSubsystem, UBlueprintFunctionLibrary, UPrimaryDataAsset, UDataTable,
				// USaveGame, UGameplayStatics, UWorld.
				"Engine",

				// FJsonObject / FJsonSerializer for the mod manifest reader and writer.
				"Json",

				// FJsonObjectConverter for USTRUCT <-> JSON round-tripping of mod save records.
				"JsonUtilities",

				// UDeveloperSettings base class for UModFrameworkSettings.
				"DeveloperSettings",

				// FPakPlatformFile / IPlatformFilePak (Public/IPlatformFilePak.h) for mounting mod paks.
				"PakFile",

				// IAssetRegistry / FAssetRegistryModule for scoping asset queries to a mod's mounts.
				"AssetRegistry",

				// IPluginManager (Interfaces/IPluginManager.h) for locating the plugin's own content root
				// and for registering mod mount points alongside plugin mount points.
				"Projects",

				// IImageWrapperModule::DetectImageFormat and IImageWrapper, used by UModIconCache to
				// sniff and measure a mod's icon before anything decodes it. Only the format and the
				// dimensions are read here; FImageUtils (Engine) does the actual import.
				"ImageWrapper"
			});

		// Everything this module needs is part of its public surface (the headers above are all
		// referenced from Public/ headers), so there is deliberately nothing extra here.
		// RenderCore, Slate and the editor modules are intentionally NOT dependencies: this module
		// must stay loadable at PostConfigInit in a cooked shipping game.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
			});
	}
}
