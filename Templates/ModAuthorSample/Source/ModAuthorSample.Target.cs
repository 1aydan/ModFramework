// Copyright (c) 2026. Licensed for use in your own projects.

using UnrealBuildTool;

public class ModAuthorSampleTarget : TargetRules
{
	public ModAuthorSampleTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ModAuthorSample");
	}
}
