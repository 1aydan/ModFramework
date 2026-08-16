// Copyright (c) 2026. Licensed for use in your own projects.

#include "Manifest/ModManifest.h"

bool FModManifest::IsValid() const
{
	return Id.IsValid() && !Version.IsZero();
}

FString FModManifest::GetDisplayNameOrId() const
{
	if (!DisplayName.IsEmpty())
	{
		return DisplayName;
	}

	return Id.ToString();
}

const FModDependency* FModManifest::FindDependency(const FModId& InId) const
{
	for (const FModDependency& Dependency : Dependencies)
	{
		if (Dependency.Id == InId)
		{
			return &Dependency;
		}
	}

	return nullptr;
}
