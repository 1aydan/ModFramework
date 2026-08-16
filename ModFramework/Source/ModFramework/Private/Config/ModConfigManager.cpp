// Copyright (c) 2026. Licensed for use in your own projects.

#include "Config/ModConfigManager.h"

#include "Core/ModFrameworkLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
// The header only forward-declares UModSubsystem, which is enough to declare a TWeakObjectPtr member
// but not to assign one - that needs the complete type.
#include "Subsystem/ModSubsystem.h"

namespace ModConfigPrivate
{
	const TCHAR* const CodeReadFailed  = TEXT("Config.ReadFailed");
	const TCHAR* const CodeInvalidJson = TEXT("Config.InvalidJson");
	const TCHAR* const CodeTooLarge    = TEXT("Config.TooLarge");

	FModDiagnostic MakeConfigWarning(const TCHAR* Code, FString Message, const FString& Context)
	{
		return FModDiagnostic::Warning(FName(Code), MoveTemp(Message), Context);
	}

	/** Reads a JSON object from disk. Missing file is not an error; a malformed one is. */
	bool ReadJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject,
		TArray<FModDiagnostic>& OutDiagnostics)
	{
		if (!FPaths::FileExists(Path))
		{
			return false;
		}

		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size > UModConfigManager::MaxConfigFileBytes)
		{
			OutDiagnostics.Add(MakeConfigWarning(CodeTooLarge,
				FString::Printf(TEXT("Config file is %lld bytes, over the %lld byte limit; ignored."),
					Size, UModConfigManager::MaxConfigFileBytes), Path));
			return false;
		}

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutDiagnostics.Add(MakeConfigWarning(CodeReadFailed, TEXT("Config file could not be read."), Path));
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		TSharedPtr<FJsonObject> Parsed;
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			// A player hand-edits these, so a broken file is expected rather than exceptional. Warn
			// and fall back to defaults instead of failing the mod's load.
			OutDiagnostics.Add(MakeConfigWarning(CodeInvalidJson,
				TEXT("Config file is not valid JSON; falling back to defaults."), Path));
			return false;
		}

		OutObject = Parsed;
		return true;
	}
}

void UModConfigManager::Initialize(UModSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;
}

void UModConfigManager::Shutdown()
{
	Stores.Reset();
	OnConfigChanged.Clear();
	Subsystem.Reset();
}

FString UModConfigManager::GetUserConfigPath(FModId ModId) const
{
	// The mod never supplies any part of this path, so it cannot escape the directory.
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ModConfigs"), ModId.ToString() + TEXT(".json"));
}

const UModConfigManager::FModConfigStore* UModConfigManager::FindStore(const FModId& ModId) const
{
	return Stores.Find(ModId);
}

UModConfigManager::FModConfigStore* UModConfigManager::FindOrAddStore(const FModId& ModId)
{
	FModConfigStore& Store = Stores.FindOrAdd(ModId);
	if (!Store.Values.IsValid())
	{
		Store.Values = MakeShared<FJsonObject>();
	}
	return &Store;
}

bool UModConfigManager::LoadConfig(const FModId& ModId, const FString& ModRootPath,
	TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModConfigPrivate;

	if (!ModId.IsValid())
	{
		return false;
	}

	FModConfigStore Store;
	Store.Defaults = MakeShared<FJsonObject>();
	Store.Values = MakeShared<FJsonObject>();

	// Defaults: every *.json under the mod's Config/ folder, merged. Multiple files let a mod group
	// its settings; later files win on a key collision, which is why the order is sorted and stable.
	if (!ModRootPath.IsEmpty())
	{
		const FString ConfigDirectory = FPaths::Combine(ModRootPath, TEXT("Config"));
		if (FPaths::DirectoryExists(ConfigDirectory))
		{
			TArray<FString> ConfigFiles;
			IFileManager::Get().FindFiles(ConfigFiles, *(ConfigDirectory / TEXT("*.json")), true, false);
			ConfigFiles.Sort();

			for (const FString& FileName : ConfigFiles)
			{
				TSharedPtr<FJsonObject> Parsed;
				if (ReadJsonObject(FPaths::Combine(ConfigDirectory, FileName), Parsed, OutDiagnostics))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Parsed->Values)
					{
						Store.Defaults->SetField(Pair.Key, Pair.Value);
					}
				}
			}
		}
	}

	// User layer. Absent on first run, which is normal.
	TSharedPtr<FJsonObject> UserValues;
	if (ReadJsonObject(GetUserConfigPath(ModId), UserValues, OutDiagnostics) && UserValues.IsValid())
	{
		Store.Values = UserValues;
	}

	Stores.Add(ModId, MoveTemp(Store));

	UE_LOG(LogModFramework, Verbose, TEXT("Config loaded for '%s': %d defaults, %d user values."),
		*ModId.ToString(),
		Stores[ModId].Defaults->Values.Num(),
		Stores[ModId].Values->Values.Num());

	NotifyChanged(ModId, NAME_None);
	return true;
}

bool UModConfigManager::SaveConfig(FModId ModId)
{
	const FModConfigStore* Store = FindStore(ModId);
	if (Store == nullptr || !Store->Values.IsValid())
	{
		return false;
	}

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);

	if (!FJsonSerializer::Serialize(Store->Values.ToSharedRef(), Writer))
	{
		return false;
	}

	const FString Path = GetUserConfigPath(ModId);
	if (!FFileHelper::SaveStringToFile(Json, *Path))
	{
		UE_LOG(LogModFramework, Warning, TEXT("Could not write config for '%s' to '%s'."),
			*ModId.ToString(), *Path);
		return false;
	}

	return true;
}

void UModConfigManager::UnloadConfig(FModId ModId)
{
	Stores.Remove(ModId);
}

void UModConfigManager::ResetToDefaults(FModId ModId)
{
	if (FModConfigStore* Store = Stores.Find(ModId))
	{
		Store->Values = MakeShared<FJsonObject>();
		NotifyChanged(ModId, NAME_None);
	}
}

bool UModConfigManager::HasConfig(FModId ModId) const
{
	return FindStore(ModId) != nullptr;
}

TSharedPtr<FJsonValue> UModConfigManager::ResolveValue(const FModId& ModId, FName Key) const
{
	const FModConfigStore* Store = FindStore(ModId);
	if (Store == nullptr)
	{
		return nullptr;
	}

	const FString KeyString = Key.ToString();

	// User layer wins; defaults are the fallback. This is the whole two-layer behaviour.
	if (Store->Values.IsValid())
	{
		if (const TSharedPtr<FJsonValue> Value = Store->Values->TryGetField(KeyString))
		{
			return Value;
		}
	}

	if (Store->Defaults.IsValid())
	{
		if (const TSharedPtr<FJsonValue> Value = Store->Defaults->TryGetField(KeyString))
		{
			return Value;
		}
	}

	return nullptr;
}

bool UModConfigManager::HasKey(FModId ModId, FName Key) const
{
	return ResolveValue(ModId, Key).IsValid();
}

TArray<FName> UModConfigManager::GetKeys(FModId ModId) const
{
	TArray<FName> Keys;
	const FModConfigStore* Store = FindStore(ModId);
	if (Store == nullptr)
	{
		return Keys;
	}

	TSet<FString> Seen;
	if (Store->Defaults.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Store->Defaults->Values)
		{
			Seen.Add(Pair.Key);
		}
	}
	if (Store->Values.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Store->Values->Values)
		{
			Seen.Add(Pair.Key);
		}
	}

	Keys.Reserve(Seen.Num());
	for (const FString& Key : Seen)
	{
		Keys.Add(FName(*Key));
	}

	// Stable order so a config listing does not shuffle between runs.
	Keys.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	return Keys;
}

bool UModConfigManager::GetBool(FModId ModId, FName Key, bool DefaultValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolveValue(ModId, Key);
	bool Out = DefaultValue;
	return (Value.IsValid() && Value->TryGetBool(Out)) ? Out : DefaultValue;
}

int32 UModConfigManager::GetInt(FModId ModId, FName Key, int32 DefaultValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolveValue(ModId, Key);
	int32 Out = DefaultValue;
	return (Value.IsValid() && Value->TryGetNumber(Out)) ? Out : DefaultValue;
}

float UModConfigManager::GetFloat(FModId ModId, FName Key, float DefaultValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolveValue(ModId, Key);
	double Out = DefaultValue;
	return (Value.IsValid() && Value->TryGetNumber(Out)) ? static_cast<float>(Out) : DefaultValue;
}

FString UModConfigManager::GetString(FModId ModId, FName Key, const FString& DefaultValue) const
{
	const TSharedPtr<FJsonValue> Value = ResolveValue(ModId, Key);
	FString Out = DefaultValue;
	return (Value.IsValid() && Value->TryGetString(Out)) ? Out : DefaultValue;
}

void UModConfigManager::SetBool(FModId ModId, FName Key, bool Value)
{
	FindOrAddStore(ModId)->Values->SetBoolField(Key.ToString(), Value);
	NotifyChanged(ModId, Key);
}

void UModConfigManager::SetInt(FModId ModId, FName Key, int32 Value)
{
	FindOrAddStore(ModId)->Values->SetNumberField(Key.ToString(), Value);
	NotifyChanged(ModId, Key);
}

void UModConfigManager::SetFloat(FModId ModId, FName Key, float Value)
{
	FindOrAddStore(ModId)->Values->SetNumberField(Key.ToString(), Value);
	NotifyChanged(ModId, Key);
}

void UModConfigManager::SetString(FModId ModId, FName Key, const FString& Value)
{
	FindOrAddStore(ModId)->Values->SetStringField(Key.ToString(), Value);
	NotifyChanged(ModId, Key);
}

bool UModConfigManager::GetConfigJson(FModId ModId, FString& OutJson) const
{
	const FModConfigStore* Store = FindStore(ModId);
	if (Store == nullptr)
	{
		return false;
	}

	// Merged view: defaults first, user values over the top.
	const TSharedRef<FJsonObject> Merged = MakeShared<FJsonObject>();
	if (Store->Defaults.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Store->Defaults->Values)
		{
			Merged->SetField(Pair.Key, Pair.Value);
		}
	}
	if (Store->Values.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Store->Values->Values)
		{
			Merged->SetField(Pair.Key, Pair.Value);
		}
	}

	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	return FJsonSerializer::Serialize(Merged, Writer);
}

bool UModConfigManager::SetConfigJson(FModId ModId, const FString& Json)
{
	if (Json.Len() > MaxConfigFileBytes)
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TSharedPtr<FJsonObject> Parsed;
	if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
	{
		// Reject rather than partially apply: a mod handing us broken JSON should not end up with a
		// half-written config it then reads back as truth.
		return false;
	}

	FindOrAddStore(ModId)->Values = Parsed;
	NotifyChanged(ModId, NAME_None);
	return true;
}

void UModConfigManager::NotifyChanged(const FModId& ModId, FName Key)
{
	OnConfigChanged.Broadcast(ModId, Key);
}
