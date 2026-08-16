// Copyright (c) 2026. Licensed for use in your own projects.

#include "Save/ModSaveGame.h"

#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Manifest/ModVersion.h"
#include "Save/ModSaveTypes.h"

int32 UModSaveGame::GetRecordCount() const
{
	return Envelope.Records.Num();
}

int32 UModSaveGame::GetOrphanedRecordCount() const
{
	int32 Count = 0;
	for (const FModSaveRecord& Record : Envelope.Records)
	{
		if (Record.bOrphaned)
		{
			++Count;
		}
	}

	return Count;
}

FString UModSaveGame::ToDebugString() const
{
	FString Result;

	Result += FString::Printf(TEXT("ModSaveGame envelope v%d\n"), Envelope.EnvelopeVersion);
	Result += FString::Printf(TEXT("  GameId           : %s\n"),
		Envelope.GameId.IsEmpty() ? TEXT("<unset>") : *Envelope.GameId);
	Result += FString::Printf(TEXT("  FrameworkVersion : %s\n"), *Envelope.FrameworkVersion.ToString());

	Result += FString::Printf(TEXT("  Stamped mods (%d):\n"), Envelope.RequiredMods.Num());
	for (const FModSaveDependency& Dependency : Envelope.RequiredMods)
	{
		const FString DisplaySuffix = Dependency.DisplayName.IsEmpty()
			? FString()
			: FString::Printf(TEXT(" \"%s\""), *Dependency.DisplayName);

		Result += FString::Printf(TEXT("    %s %s%s%s\n"),
			*Dependency.ModId.ToString(),
			*Dependency.Version.ToString(),
			Dependency.bWasRequired ? TEXT(" [required for network play]") : TEXT(""),
			*DisplaySuffix);
	}

	Result += FString::Printf(TEXT("  Records (%d, %d orphaned):\n"), Envelope.Records.Num(), GetOrphanedRecordCount());
	for (const FModSaveRecord& Record : Envelope.Records)
	{
		Result += FString::Printf(TEXT("    %s v%s data=%d json=%d chars binary=%d bytes%s\n"),
			*Record.ModId.ToString(),
			*Record.ModVersion.ToString(),
			Record.DataVersion,
			Record.Json.Len(),
			Record.Binary.Num(),
			Record.bOrphaned ? TEXT(" [ORPHANED - preserved]") : TEXT(""));
	}

	return Result;
}
