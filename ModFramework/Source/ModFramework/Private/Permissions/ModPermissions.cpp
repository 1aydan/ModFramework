// Copyright (c) 2026. Licensed for use in your own projects.

#include "Permissions/ModPermissions.h"

#include "Containers/Array.h"
#include "CoreTypes.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Text.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"

namespace ModPermissionsPrivate
{
	/**
	 * Fills a descriptor.
	 *
	 * A free function rather than a constructor on FModPermissionDescriptor, so that the struct stays
	 * a plain aggregate for every other caller.
	 */
	static FModPermissionDescriptor MakeDescriptor(const FName InPermissionId, FText InDisplayName, FText InDescription, const bool bInDangerous)
	{
		FModPermissionDescriptor Descriptor;
		Descriptor.PermissionId = InPermissionId;
		Descriptor.DisplayName = MoveTemp(InDisplayName);
		Descriptor.Description = MoveTemp(InDescription);
		Descriptor.bDangerous = bInDangerous;
		return Descriptor;
	}
}

namespace ModPermissions
{
	// `extern` on the definitions as well as on the declarations: a const variable at namespace scope
	// would otherwise default to internal linkage, and these have to be one shared FName per id.
	extern const FName GameplayModify(TEXT("gameplay.modify"));
	extern const FName AssetsRead(TEXT("assets.read"));
	extern const FName AssetsWrite(TEXT("assets.write"));
	extern const FName FilesystemRead(TEXT("filesystem.read"));
	extern const FName FilesystemWrite(TEXT("filesystem.write"));
	extern const FName Network(TEXT("network"));
	extern const FName Console(TEXT("console"));
	extern const FName SaveModify(TEXT("save.modify"));
	extern const FName NativeCode(TEXT("native_code"));
	extern const FName UiModify(TEXT("ui.modify"));

	TArray<FName> GetBuiltinPermissions()
	{
		return TArray<FName>
		{
			GameplayModify,
			AssetsRead,
			AssetsWrite,
			FilesystemRead,
			FilesystemWrite,
			Network,
			Console,
			SaveModify,
			NativeCode,
			UiModify
		};
	}

	TArray<FModPermissionDescriptor> GetBuiltinPermissionDescriptors()
	{
		using namespace ModPermissionsPrivate;

		// The bDangerous flags below are the security policy of the whole framework in one place.
		// A permission is dangerous when granting it lets a mod reach something the framework cannot
		// police afterwards: the filesystem, the network, content another mod owns, the console, or
		// native code. Everything else is a gameplay concern that stays inside the sandbox. A game
		// that wants to be stricter re-registers the descriptor with bDangerous set.
		TArray<FModPermissionDescriptor> Descriptors;
		Descriptors.Reserve(10);

		Descriptors.Add(MakeDescriptor(
			GameplayModify,
			NSLOCTEXT("ModPermissions", "GameplayModify_Name", "Modify Gameplay"),
			NSLOCTEXT("ModPermissions", "GameplayModify_Description", "Change gameplay rules, balance and behaviour through the game's modding APIs and extension points."),
			/*bDangerous*/ false));

		Descriptors.Add(MakeDescriptor(
			AssetsRead,
			NSLOCTEXT("ModPermissions", "AssetsRead_Name", "Read Game Content"),
			NSLOCTEXT("ModPermissions", "AssetsRead_Description", "Load and inspect content shipped by the game and by other installed mods."),
			/*bDangerous*/ false));

		Descriptors.Add(MakeDescriptor(
			AssetsWrite,
			NSLOCTEXT("ModPermissions", "AssetsWrite_Name", "Replace Game Content"),
			NSLOCTEXT("ModPermissions", "AssetsWrite_Description", "Override content owned by the game or by another mod. A mod with this permission can change things you installed a different mod for."),
			/*bDangerous*/ true));

		Descriptors.Add(MakeDescriptor(
			FilesystemRead,
			NSLOCTEXT("ModPermissions", "FilesystemRead_Name", "Read Files"),
			NSLOCTEXT("ModPermissions", "FilesystemRead_Description", "Read files outside this mod's own folder, including game files, other mods and your documents."),
			/*bDangerous*/ true));

		Descriptors.Add(MakeDescriptor(
			FilesystemWrite,
			NSLOCTEXT("ModPermissions", "FilesystemWrite_Name", "Write Files"),
			NSLOCTEXT("ModPermissions", "FilesystemWrite_Description", "Create, change or delete files outside this mod's own folder. That covers anything the game itself is allowed to write to."),
			/*bDangerous*/ true));

		Descriptors.Add(MakeDescriptor(
			Network,
			NSLOCTEXT("ModPermissions", "Network_Name", "Access the Network"),
			NSLOCTEXT("ModPermissions", "Network_Description", "Connect to servers on the internet. A mod with this permission can send information about your game and your machine elsewhere."),
			/*bDangerous*/ true));

		Descriptors.Add(MakeDescriptor(
			Console,
			NSLOCTEXT("ModPermissions", "Console_Name", "Run Console Commands"),
			NSLOCTEXT("ModPermissions", "Console_Description", "Execute arbitrary console commands, which reach engine functionality the rest of the permission system cannot check."),
			/*bDangerous*/ true));

		Descriptors.Add(MakeDescriptor(
			SaveModify,
			NSLOCTEXT("ModPermissions", "SaveModify_Name", "Modify Saved Games"),
			NSLOCTEXT("ModPermissions", "SaveModify_Description", "Read and write this game's save data, including the parts written by the base game and by other mods."),
			/*bDangerous*/ false));

		Descriptors.Add(MakeDescriptor(
			NativeCode,
			NSLOCTEXT("ModPermissions", "NativeCode_Name", "Load Native Code"),
			NSLOCTEXT("ModPermissions", "NativeCode_Description", "Load a compiled code module shipped with this mod. Native code runs with the full privileges of the game and is entirely outside the framework's control."),
			/*bDangerous*/ true));

		Descriptors.Add(MakeDescriptor(
			UiModify,
			NSLOCTEXT("ModPermissions", "UiModify_Name", "Modify the Interface"),
			NSLOCTEXT("ModPermissions", "UiModify_Description", "Add to, restyle or replace parts of the game's user interface."),
			/*bDangerous*/ false));

		return Descriptors;
	}
}
