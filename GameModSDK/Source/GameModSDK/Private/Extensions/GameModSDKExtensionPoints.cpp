// Copyright (c) 2026. Licensed for use in your own projects.

#include "Extensions/GameModSDKExtensionPoints.h"

#include "Containers/Array.h"
#include "Core/ModFrameworkTypes.h"
#include "Extensions/GameEnemyExtension.h"
#include "Extensions/GameRuleExtension.h"
#include "Extensions/GameUIExtension.h"
#include "Extensions/GameWeaponExtension.h"
#include "Extensions/ModExtension.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Text.h"
#include "Manifest/ModVersion.h"
#include "Permissions/ModPermissions.h"
#include "Templates/SubclassOf.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"

#define LOCTEXT_NAMESPACE "GameModExtensionPoints"

namespace GameModExtensionPoints
{
	const FName Weapon(TEXT("game.weapon"));
	const FName Enemy(TEXT("game.enemy"));
	const FName GameRule(TEXT("game.gamerule"));
	const FName UI(TEXT("game.ui"));

	TArray<FName> GetAllPointIds()
	{
		return TArray<FName>({ Weapon, Enemy, GameRule, UI });
	}
}

namespace GameModExtensionPointsPrivate
{
	/**
	 * One descriptor, with the fields every point of this game shares already decided.
	 *
	 * Version is the version of the *point's contract*, not of the SDK: it changes when what an
	 * extension of this point is expected to do changes, so a mod can reason about what it is
	 * plugging into. All four start at 1.0.0 and move independently after that.
	 *
	 * bAllowMultiplePerMod is true everywhere. A mod that adds three weapons, or a HUD panel and a
	 * minimap, is doing the ordinary thing; forcing it to ship as three mods would be a tax with no
	 * benefit, and the per-resource conflict rules already handle two mods wanting the same slot.
	 */
	FModExtensionPointDescriptor BuildPointDescriptor(
		const FName PointId,
		const FText& DisplayName,
		const FText& Description,
		const TSubclassOf<UModExtension>& RequiredBaseClass,
		const EModConflictPolicy ConflictPolicy,
		const bool bServerAuthoritative,
		const FName RequiredPermission)
	{
		FModExtensionPointDescriptor Descriptor;
		Descriptor.ExtensionPointId = PointId;
		Descriptor.Version = FModVersion(1, 0, 0);
		Descriptor.DisplayName = DisplayName;
		Descriptor.Description = Description;
		Descriptor.RequiredBaseClass = RequiredBaseClass;
		Descriptor.DefaultConflictPolicy = ConflictPolicy;
		Descriptor.bAllowMultiplePerMod = true;
		Descriptor.bServerAuthoritative = bServerAuthoritative;
		Descriptor.RequiredPermissions.Add(RequiredPermission);

		return Descriptor;
	}
}

TArray<FModExtensionPointDescriptor> BuildGameExtensionPointDescriptors()
{
	using namespace GameModExtensionPointsPrivate;

	// The permissions are the framework's own builtins, used for exactly what their documentation
	// says: "gameplay.modify" is described as changing gameplay rules through the game's APIs *and
	// extension points*, and "ui.modify" as adding to or restyling the interface. A mod that has not
	// been granted them is refused at registration with Extension.PermissionDenied, so a player who
	// declined a permission prompt does not end up with the mod half-installed.
	TArray<FModExtensionPointDescriptor> Descriptors;
	Descriptors.Reserve(4);

	Descriptors.Add(BuildPointDescriptor(
		GameModExtensionPoints::Weapon,
		LOCTEXT("WeaponPointName", "Weapons"),
		LOCTEXT("WeaponPointDescription", "Adds weapons and adjusts outgoing damage. An extension returns a weapon definition and adjusted numbers; the game applies them through the combat API."),
		UGameWeaponExtension::StaticClass(),
		EModConflictPolicy::Priority,
		/*bServerAuthoritative*/ true,
		ModPermissions::GameplayModify));

	Descriptors.Add(BuildPointDescriptor(
		GameModExtensionPoints::Enemy,
		LOCTEXT("EnemyPointName", "Enemies"),
		LOCTEXT("EnemyPointDescription", "Adds enemies, adjusts their health and observes spawns. An extension returns an enemy definition and adjusted numbers; the game spawns and applies them through the world API."),
		UGameEnemyExtension::StaticClass(),
		EModConflictPolicy::Priority,
		/*bServerAuthoritative*/ true,
		ModPermissions::GameplayModify));

	Descriptors.Add(BuildPointDescriptor(
		GameModExtensionPoints::GameRule,
		LOCTEXT("GameRulePointName", "Game Rules"),
		LOCTEXT("GameRulePointDescription", "Reacts to the session and to each wave, and may veto a spawn. Rules compose: the game consults every contributing mod rather than picking one winner."),
		UGameRuleExtension::StaticClass(),
		EModConflictPolicy::Merge,
		/*bServerAuthoritative*/ true,
		ModPermissions::GameplayModify));

	Descriptors.Add(BuildPointDescriptor(
		GameModExtensionPoints::UI,
		LOCTEXT("UIPointName", "User Interface"),
		LOCTEXT("UIPointDescription", "Adds widgets to the game's interface. Several mods can each contribute a panel; the game orders them by the sort priority every extension returns."),
		UGameUIExtension::StaticClass(),
		EModConflictPolicy::Merge,
		/*bServerAuthoritative*/ false,
		ModPermissions::UiModify));

	return Descriptors;
}

#undef LOCTEXT_NAMESPACE
