// Copyright (c) 2026. Licensed for use in your own projects.

using UnrealBuildTool;

public class ModFrameworkLua : ModuleRules
{
	public ModFrameworkLua(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ModFramework"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Vendored Lua 5.5, at Source/ThirdParty/Lua. It carries its own compiler relaxations
			// (no unity, no shared PCH, warnings that vendored C trips), so none of them apply to
			// this module's C++ - which is why the interpreter is a separate module rather than
			// having its .c files folded in here.
			//
			// MODFRAMEWORK_WITH_LUA and LUA_USE_LONGJMP are PUBLIC definitions on that module, so
			// they arrive with the dependency and are not restated here.
			"Lua"
		});
	}
}
