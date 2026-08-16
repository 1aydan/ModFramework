// Copyright (c) 2026. Licensed for use in your own projects.

#include "Content/ModAssetLibrary.h"

#include "Blueprint/BlueprintSupport.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Content/ModContentManager.h"
#include "Core/ModFrameworkLog.h"
#include "Misc/PackageName.h"
#include "Subsystem/ModSubsystem.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	/**
	 * The content manager reachable from a world context, or nullptr with a log line explaining why
	 * not. Every function here is a Blueprint entry point, so "no subsystem" has to be survivable:
	 * a mod-browser widget must not crash because it ticked one frame before the game instance was
	 * ready.
	 */
	const FModContentManager* FindContentManager(const UObject* WorldContextObject, const TCHAR* CallingFunction)
	{
		UModSubsystem* Subsystem = UModSubsystem::Get(WorldContextObject);
		if (!Subsystem)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("UModAssetLibrary::%s: no mod subsystem for the supplied world context object."), CallingFunction);
			return nullptr;
		}

		return &Subsystem->GetContentManager();
	}
}

TArray<FAssetData> UModAssetLibrary::GetModAssets(const UObject* WorldContextObject, FModId ModId, UClass* ClassFilter)
{
	const FModContentManager* ContentManager = FindContentManager(WorldContextObject, TEXT("GetModAssets"));
	if (!ContentManager)
	{
		return TArray<FAssetData>();
	}

	return ContentManager->GetModAssets(ModId, ClassFilter, /*bRecursiveClasses*/ true);
}

UObject* UModAssetLibrary::LoadModAsset(const UObject* WorldContextObject, FModId ModId, FName AssetName,
	UClass* AssetClass)
{
	if (AssetName.IsNone())
	{
		UE_LOG(LogModFramework, Warning, TEXT("UModAssetLibrary::LoadModAsset called with no asset name."));
		return nullptr;
	}

	const FModContentManager* ContentManager = FindContentManager(WorldContextObject, TEXT("LoadModAsset"));
	if (!ContentManager)
	{
		return nullptr;
	}

	const TArray<FAssetData> Assets = ContentManager->GetModAssets(ModId, AssetClass, /*bRecursiveClasses*/ true);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName != AssetName)
		{
			continue;
		}

		UObject* Loaded = Asset.GetAsset();
		if (!Loaded)
		{
			UE_LOG(LogModFramework, Warning, TEXT("'%s' from mod '%s' could not be loaded."),
				*Asset.GetSoftObjectPath().ToString(), *ModId.ToString());
			return nullptr;
		}

		// GetModAssets filtered on the class the asset registry recorded, which for a Blueprint is
		// UBlueprint rather than the class it generates. Re-check what actually came back.
		if (AssetClass && !Loaded->IsA(AssetClass))
		{
			UE_LOG(LogModFramework, Warning, TEXT("'%s' from mod '%s' is a %s, not a %s."),
				*Asset.GetSoftObjectPath().ToString(), *ModId.ToString(),
				*Loaded->GetClass()->GetName(), *AssetClass->GetName());
			return nullptr;
		}

		return Loaded;
	}

	UE_LOG(LogModFramework, Warning, TEXT("Mod '%s' has no asset named '%s'."), *ModId.ToString(), *AssetName.ToString());
	return nullptr;
}

TArray<UClass*> UModAssetLibrary::GetModClasses(const UObject* WorldContextObject, FModId ModId, UClass* BaseClass)
{
	TArray<UClass*> Result;

	const FModContentManager* ContentManager = FindContentManager(WorldContextObject, TEXT("GetModClasses"));
	if (!ContentManager)
	{
		return Result;
	}

	const TArray<FAssetData> Assets = ContentManager->GetModAssets(ModId, nullptr, /*bRecursiveClasses*/ true);

	TSet<FString> SeenClassPaths;
	SeenClassPaths.Reserve(Assets.Num());

	for (const FAssetData& Asset : Assets)
	{
		FString ClassPath;

		FString GeneratedClassPath;
		if (Asset.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClassPath))
		{
			// Uncooked: the UBlueprint asset records the class it generates as an export-text path
			// ("BlueprintGeneratedClass'/MyMod/BP_Foo.BP_Foo_C'").
			ClassPath = FPackageName::ExportTextPathToObjectPath(GeneratedClassPath);
		}
		else if (const UClass* AssetClass = Asset.GetClass())
		{
			// Cooked: the class itself is the asset, so the asset's own registry class is a UClass
			// (UBlueprintGeneratedClass and friends all derive from it).
			if (AssetClass->IsChildOf(UClass::StaticClass()))
			{
				ClassPath = Asset.GetSoftObjectPath().ToString();
			}
		}

		if (ClassPath.IsEmpty())
		{
			continue;
		}

		bool bAlreadySeen = false;
		SeenClassPaths.Add(ClassPath, &bAlreadySeen);
		if (bAlreadySeen)
		{
			continue;
		}

		UClass* LoadedClass = FSoftClassPath(ClassPath).TryLoadClass<UObject>();
		if (!LoadedClass)
		{
			UE_LOG(LogModFramework, Verbose, TEXT("Class '%s' from mod '%s' could not be loaded; skipping it."),
				*ClassPath, *ModId.ToString());
			continue;
		}

		if (BaseClass && !LoadedClass->IsChildOf(BaseClass))
		{
			continue;
		}

		Result.Add(LoadedClass);
	}

	// Stable output: two runs over the same mod produce the same list in the same order.
	Result.Sort([](const UClass& A, const UClass& B) { return A.GetPathName() < B.GetPathName(); });

	return Result;
}

FModId UModAssetLibrary::FindOwningMod(const UObject* WorldContextObject, const UObject* Asset)
{
	if (!Asset)
	{
		return FModId();
	}

	const FModContentManager* ContentManager = FindContentManager(WorldContextObject, TEXT("FindOwningMod"));
	if (!ContentManager)
	{
		return FModId();
	}

	return ContentManager->FindOwningMod(Asset->GetPathName());
}

TArray<FModContentMount> UModAssetLibrary::GetModContentMounts(const UObject* WorldContextObject)
{
	const FModContentManager* ContentManager = FindContentManager(WorldContextObject, TEXT("GetModContentMounts"));
	if (!ContentManager)
	{
		return TArray<FModContentMount>();
	}

	return ContentManager->GetMounts();
}
