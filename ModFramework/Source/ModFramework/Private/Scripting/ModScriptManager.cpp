// Copyright (c) 2026. Licensed for use in your own projects.

#include "Scripting/ModScriptManager.h"

#include "Core/ModFrameworkLog.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Registry/ModInfo.h"
#include "Runtime/ModContext.h"
#include "Scripting/ModScriptRuntime.h"
#include "Subsystem/ModSubsystem.h"

namespace ModScriptFunctions
{
	// Must match docs/Scripting.md and the example scripts. Renaming one stops existing mods being
	// called, silently, because an absent function is a legitimate case rather than an error.
	const FName OnModActivated(TEXT("OnModActivated"));
	const FName OnModDeactivated(TEXT("OnModDeactivated"));
}

namespace ModScriptManagerPrivate
{
	const TCHAR* const CodeUnknownRuntime  = TEXT("Script.UnknownRuntime");
	const TCHAR* const CodeRuntimeFailed   = TEXT("Script.RuntimeFailed");
	const TCHAR* const CodeFileMissing     = TEXT("Script.FileMissing");
	const TCHAR* const CodeFileTooLarge    = TEXT("Script.FileTooLarge");
	const TCHAR* const CodeReadFailed      = TEXT("Script.ReadFailed");
	const TCHAR* const CodeWrongExtension  = TEXT("Script.WrongExtension");
	const TCHAR* const CodeLoadFailed      = TEXT("Script.LoadFailed");

	// Deliberately not MakeError - Templates/ValueOrError.h declares a global variadic MakeError that
	// wins overload resolution wherever this namespace is pulled in with a using-directive.
	FModDiagnostic MakeScriptError(const TCHAR* Code, FString Message, const FString& Context = FString())
	{
		return FModDiagnostic::Error(FName(Code), MoveTemp(Message), Context);
	}

	/**
	 * The module-global recipe list.
	 *
	 * A function-local static rather than a file-scope one: a runtime module can register during
	 * static initialisation, and this guarantees the map is constructed before the first use rather
	 * than depending on translation-unit ordering.
	 */
	TMap<FName, FModScriptRuntimeFactory>& GetFactories()
	{
		static TMap<FName, FModScriptRuntimeFactory> Factories;
		return Factories;
	}
}

void UModScriptManager::RegisterRuntimeFactory(FName RuntimeId, FModScriptRuntimeFactory Factory)
{
	using namespace ModScriptManagerPrivate;

	if (RuntimeId.IsNone() || !Factory.IsBound())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Ignoring a script runtime factory with no id or no bound delegate."));
		return;
	}

	if (GetFactories().Contains(RuntimeId))
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("Script runtime factory '%s' was already registered; replacing it."), *RuntimeId.ToString());
	}

	GetFactories().Add(RuntimeId, MoveTemp(Factory));
	UE_LOG(LogModFramework, Verbose, TEXT("Registered script runtime factory '%s'."), *RuntimeId.ToString());
}

void UModScriptManager::UnregisterRuntimeFactory(FName RuntimeId)
{
	ModScriptManagerPrivate::GetFactories().Remove(RuntimeId);
}

TArray<FName> UModScriptManager::GetRegisteredFactoryIds()
{
	TArray<FName> Ids;
	ModScriptManagerPrivate::GetFactories().GetKeys(Ids);
	Ids.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	return Ids;
}

void UModScriptManager::Initialize(UModSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;

	// Build one runtime per registered recipe. Doing this once at Initialize - rather than lazily on
	// first use - means a mod that names a runtime gets a straight yes or no, instead of a failure
	// that depends on whether some other mod happened to load first.
	for (const TPair<FName, FModScriptRuntimeFactory>& Pair : ModScriptManagerPrivate::GetFactories())
	{
		if (!Pair.Value.IsBound())
		{
			continue;
		}

		TSharedPtr<IModScriptRuntime> Runtime = Pair.Value.Execute();
		if (!Runtime.IsValid())
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("Script runtime factory '%s' produced nothing; that runtime will be unavailable."),
				*Pair.Key.ToString());
			continue;
		}

		// The recipe's id and the runtime's own id must agree, or a manifest naming one would find
		// the other. Trust the runtime and warn, rather than filing it under a name it disowns.
		const FName ReportedId = Runtime->GetRuntimeId();
		if (ReportedId != Pair.Key)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("Script runtime factory '%s' produced a runtime calling itself '%s'; registering it under '%s'."),
				*Pair.Key.ToString(), *ReportedId.ToString(), *ReportedId.ToString());
		}

		Runtimes.Add(ReportedId.IsNone() ? Pair.Key : ReportedId, Runtime);
	}

	if (Runtimes.Num() > 0)
	{
		TArray<FName> Ids;
		Runtimes.GetKeys(Ids);
		Ids.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

		TArray<FString> Names;
		for (const FName& Id : Ids)
		{
			Names.Add(Id.ToString());
		}

		UE_LOG(LogModFramework, Log, TEXT("Script runtimes available: %s."), *FString::Join(Names, TEXT(", ")));
	}
}

void UModScriptManager::Shutdown()
{
	// Destroy every context before dropping the runtimes: a runtime released while it still owns a
	// mod's state would take that state down without the mod's teardown ever running.
	for (const TPair<FModId, FModScriptBinding>& Pair : ModBindings)
	{
		if (const TSharedPtr<IModScriptRuntime>* Runtime = Runtimes.Find(Pair.Value.RuntimeId))
		{
			(*Runtime)->DestroyContext(Pair.Key);
		}
	}

	ModBindings.Reset();
	Runtimes.Reset();
	Subsystem.Reset();
}

IModScriptRuntime* UModScriptManager::FindRuntime(FName RuntimeId) const
{
	const TSharedPtr<IModScriptRuntime>* Found = Runtimes.Find(RuntimeId);
	return Found ? Found->Get() : nullptr;
}

bool UModScriptManager::HasRuntime(FName RuntimeId) const
{
	return FindRuntime(RuntimeId) != nullptr;
}

TArray<FName> UModScriptManager::GetRuntimeIds() const
{
	TArray<FName> Ids;
	Runtimes.GetKeys(Ids);
	Ids.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	return Ids;
}

bool UModScriptManager::LoadModScripts(const FModInfo& ModInfo, UModContext* Context,
	TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModScriptManagerPrivate;

	const FModId& ModId = ModInfo.GetId();
	const FName RuntimeId = ModInfo.Manifest.EntryPoint.ScriptRuntime;

	// No scripting declared. The manifest validator already guarantees a runtime and scripts are
	// either both present or both absent, so this one check covers the whole "not a script mod" case.
	if (RuntimeId.IsNone())
	{
		return true;
	}

	IModScriptRuntime* Runtime = FindRuntime(RuntimeId);
	if (Runtime == nullptr)
	{
		const TArray<FName> Available = GetRuntimeIds();
		FString AvailableText = TEXT("none are registered");
		if (Available.Num() > 0)
		{
			TArray<FString> Names;
			for (const FName& Id : Available)
			{
				Names.Add(Id.ToString());
			}
			AvailableText = FString::Printf(TEXT("registered runtimes are: %s"), *FString::Join(Names, TEXT(", ")));
		}

		OutDiagnostics.Add(MakeScriptError(CodeUnknownRuntime,
			FString::Printf(TEXT("Script runtime '%s' is not available - %s."), *RuntimeId.ToString(), *AvailableText),
			ModId.ToString()));
		return false;
	}

	if (Context == nullptr)
	{
		OutDiagnostics.Add(MakeScriptError(CodeRuntimeFailed,
			TEXT("No mod context to bind scripts to."), ModId.ToString()));
		return false;
	}

	FModDiagnostic RuntimeError;
	if (!Runtime->CreateContext(ModId, Context, RuntimeError))
	{
		OutDiagnostics.Add(RuntimeError);
		return false;
	}

	// From here on, any failure has to tear the context down again - leaving a half-populated script
	// state behind would let a later CallModFunction run against scripts that never finished loading.
	const TArray<FString> Extensions = Runtime->GetSourceExtensions();
	int32 Loaded = 0;

	for (const FString& RelativePath : ModInfo.Manifest.EntryPoint.Scripts)
	{
		// The manifest guarantees the path is contained and unique, but never that it resolves.
		const FString AbsolutePath = FPaths::Combine(ModInfo.RootPath, RelativePath);

		// The extension check belongs here rather than in the parser: the parser has no runtime
		// registry, and a manifest must stay parseable on a machine with no runtimes at all.
		if (Extensions.Num() > 0)
		{
			const FString Extension = FString(TEXT(".")) + FPaths::GetExtension(RelativePath);
			const bool bClaimed = Extensions.ContainsByPredicate([&Extension](const FString& Candidate)
			{
				return Candidate.Equals(Extension, ESearchCase::IgnoreCase);
			});

			if (!bClaimed)
			{
				OutDiagnostics.Add(MakeScriptError(CodeWrongExtension,
					FString::Printf(TEXT("Runtime '%s' does not handle '%s' files."),
						*RuntimeId.ToString(), *Extension),
					RelativePath));
				Runtime->DestroyContext(ModId);
				return false;
			}
		}

		if (!FPaths::FileExists(AbsolutePath))
		{
			OutDiagnostics.Add(MakeScriptError(CodeFileMissing,
				TEXT("The script named by the manifest does not exist."), RelativePath));
			Runtime->DestroyContext(ModId);
			return false;
		}

		const int64 Size = IFileManager::Get().FileSize(*AbsolutePath);
		if (Size > MaxScriptFileBytes)
		{
			OutDiagnostics.Add(MakeScriptError(CodeFileTooLarge,
				FString::Printf(TEXT("The script is %lld bytes, over the %lld byte limit."),
					Size, MaxScriptFileBytes),
				RelativePath));
			Runtime->DestroyContext(ModId);
			return false;
		}

		TArray<uint8> Source;
		if (!FFileHelper::LoadFileToArray(Source, *AbsolutePath))
		{
			OutDiagnostics.Add(MakeScriptError(CodeReadFailed,
				TEXT("The script could not be read."), RelativePath));
			Runtime->DestroyContext(ModId);
			return false;
		}

		// The virtual path, not the absolute one: it is what appears in stack traces, and a mod
		// author's error should name the file they wrote rather than wherever it happens to be
		// installed on this machine.
		FModDiagnostic LoadError;
		if (!Runtime->LoadScript(ModId, RelativePath, Source, LoadError))
		{
			OutDiagnostics.Add(LoadError);
			Runtime->DestroyContext(ModId);
			return false;
		}

		++Loaded;
	}

	FModScriptBinding Binding;
	Binding.RuntimeId = RuntimeId;
	Binding.ScriptCount = Loaded;
	ModBindings.Add(ModId, Binding);

	UE_LOG(LogModFramework, Log, TEXT("Mod '%s' loaded %d script%s into runtime '%s'."),
		*ModId.ToString(), Loaded, Loaded == 1 ? TEXT("") : TEXT("s"), *RuntimeId.ToString());
	return true;
}

bool UModScriptManager::UnloadModScripts(const FModId& ModId)
{
	const FModScriptBinding* Binding = ModBindings.Find(ModId);
	if (Binding == nullptr)
	{
		return true;
	}

	bool bDestroyed = true;
	if (IModScriptRuntime* Runtime = FindRuntime(Binding->RuntimeId))
	{
		bDestroyed = Runtime->DestroyContext(ModId);
	}

	ModBindings.Remove(ModId);
	return bDestroyed;
}

bool UModScriptManager::CallModFunction(const FModId& ModId, FName FunctionName)
{
	const FModScriptBinding* Binding = ModBindings.Find(ModId);
	if (Binding == nullptr)
	{
		return false;
	}

	IModScriptRuntime* Runtime = FindRuntime(Binding->RuntimeId);
	if (Runtime == nullptr)
	{
		return false;
	}

	// An absent function is the normal case - the lifecycle hooks are all optional - so it is not
	// worth a diagnostic. A function that exists and then errors is, and the runtime reports that.
	if (!Runtime->HasFunction(ModId, FunctionName))
	{
		return false;
	}

	FModDiagnostic CallError;
	if (!Runtime->CallFunction(ModId, FunctionName, CallError))
	{
		UE_LOG(LogModFramework, Warning, TEXT("%s"), *CallError.ToString());
		return false;
	}

	return true;
}

bool UModScriptManager::AreScriptsLoaded(FModId ModId) const
{
	return ModBindings.Contains(ModId);
}

TArray<FModId> UModScriptManager::GetScriptedMods() const
{
	TArray<FModId> Ids;
	ModBindings.GetKeys(Ids);
	Ids.Sort([](const FModId& A, const FModId& B) { return A < B; });
	return Ids;
}

FName UModScriptManager::GetModRuntimeId(FModId ModId) const
{
	const FModScriptBinding* Binding = ModBindings.Find(ModId);
	return Binding ? Binding->RuntimeId : NAME_None;
}

int32 UModScriptManager::GetModScriptCount(FModId ModId) const
{
	const FModScriptBinding* Binding = ModBindings.Find(ModId);
	return Binding ? Binding->ScriptCount : 0;
}
