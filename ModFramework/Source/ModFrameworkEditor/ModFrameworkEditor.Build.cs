// Copyright (c) 2026. Licensed for use in your own projects.

using UnrealBuildTool;

public class ModFrameworkEditor : ModuleRules
{
	public ModFrameworkEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",

				// Runtime framework types surfaced by the editor UI.
				"ModFramework",

				// Packaging and SDK generation driven by the editor commands and window.
				"ModFrameworkDeveloper"
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Slate widgets (SListView, SDockTab, ...) and the core styling types.
				// FAppStyle lives in SlateCore at Styling/AppStyle.h; the legacy "EditorStyle" module
				// is deprecated since 5.1 (FEditorStyle::Get is UE_DEPRECATED) and is deliberately
				// not listed here.
				"Slate",
				"SlateCore",

				// FGlobalTabmanager, GEditor, editor subsystems, asset editor plumbing.
				"UnrealEd",

				// SSearchBox / SPositiveActionButton style widgets shared by editor tooling.
				"ToolWidgets",

				// SAssetDropTarget and friends for drag-and-drop of mod content bundles.
				"EditorWidgets",

				// IWorkspaceMenuStructure so the mod window can be nested under Tools.
				"WorkspaceMenuStructure",

				// FKey / input chords for the editor command bindings.
				"InputCore",

				// IPluginManager for locating the framework's own resources.
				"Projects",

				// FDetailsViewArgs / IDetailsView for the manifest and settings detail panels.
				"PropertyEditor",

				// FPlatformApplicationMisc (clipboard) for "copy diagnostics" actions.
				"ApplicationCore",

				// UToolMenus for extending the editor's main menu and toolbar.
				"ToolMenus",

				// IDesktopPlatform for the browse/save dialogs used by the mod window.
				"DesktopPlatform",

				// IAssetRegistry for listing the assets a mod contributes.
				"AssetRegistry",

				// FJsonObject / FJsonSerializer for reading and writing mod.json from the editor.
				"Json",

				// FJsonObjectConverter for USTRUCT <-> JSON in the manifest editor.
				"JsonUtilities"
			});
	}
}
