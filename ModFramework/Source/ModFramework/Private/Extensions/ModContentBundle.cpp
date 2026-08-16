// Copyright (c) 2026. Licensed for use in your own projects.

#include "Extensions/ModContentBundle.h"

#include "UObject/NameTypes.h"
#include "UObject/PrimaryAssetId.h"

namespace ModContentBundlePrivate
{
	/**
	 * The primary asset type every bundle reports, whichever mod it came from.
	 *
	 * UPrimaryDataAsset's default implementation derives the type from the most-derived native class,
	 * which would give a Blueprint-subclassed bundle a different type per mod and make the asset
	 * manager unable to enumerate "all mod bundles" with one query. Pinning the type here is the
	 * whole reason this override exists.
	 */
	static const FPrimaryAssetType BundleAssetType(TEXT("ModContentBundle"));
}

FPrimaryAssetId UModContentBundle::GetPrimaryAssetId() const
{
	return FPrimaryAssetId(ModContentBundlePrivate::BundleAssetType, GetFName());
}
