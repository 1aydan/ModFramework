// Copyright (c) 2026. Licensed for use in your own projects.

#include "Scripting/ModScriptManager.h"

#include "Algo/Sort.h"
#include "Containers/ArrayView.h"
#include "Core/ModFrameworkLog.h"
#include "HAL/FileManager.h"
#include "Manifest/ModManifest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Packaging/ModPackageFormat.h"
#include "Registry/ModInfo.h"
#include "Registry/ModRegistry.h"
#include "Scripting/ModScriptRuntime.h"
// The header only forward-declares UModSubsystem, which is enough for a TWeakObjectPtr member but
// not to assign one or to reach the registry through it.
#include "Subsystem/ModSubsystem.h"

/**
 * The lifecycle hooks a script may define.
 *
 * The spellings are part of the binding contract - they are what a mod author types at the top of
 * `main.lua` - so they must never change. FName construction during static initialisation is safe:
 * the name table initialises lazily on first use.
 */
namespace ModScriptFunctions
{
	const FName OnModActivated(TEXT("OnModActivated"));
	const FName OnModDeactivated(TEXT("OnModDeactivated"));
}

namespace ModScriptManagerPrivate
{
	/** Stable machine-readable diagnostic codes. Tooling and the console commands match on these. */
	constexpr const TCHAR* CodeUnknownRuntime = TEXT("Script.UnknownRuntime");
	constexpr const TCHAR* CodeNoScripts = TEXT("Script.NoScripts");
	constexpr const TCHAR* CodeContextFailed = TEXT("Script.ContextFailed");
	constexpr const TCHAR* CodeUnsafePath = TEXT("Script.UnsafePath");
	constexpr const TCHAR* CodeUnsupportedExtension = TEXT("Script.UnsupportedExtension");
	constexpr const TCHAR* CodeNotFound = TEXT("Script.NotFound");
	constexpr const TCHAR* CodeTooLarge = TEXT("Script.TooLarge");
	constexpr const TCHAR* CodeReadFailed = TEXT("Script.ReadFailed");
	constexpr const TCHAR* CodeLoadFailed = TEXT("Script.LoadFailed");
	constexpr const TCHAR* CodeCallFailed = TEXT("Script.CallFailed");
	constexpr const TCHAR* CodeInvalidRuntime = TEXT("Script.InvalidRuntime");
	constexpr const TCHAR* CodeDuplicateRuntime = TEXT("Script.DuplicateRuntime");

	/**
	 * The module-global factory list.
	 *
	 * A function-local static rather than a file-scope one: a language module may call
	 * RegisterRuntimeFactory from StartupModule at any loading phase, and construct-on-first-use is
	 * the only ordering guarantee that survives that. Nothing here touches a UObject or a CDO.
	 */
	TMap<FName, FModScriptRuntimeFactory>& GetFactoryRegistry()
	{
		static TMap<FName, FModScriptRuntimeFactory> Factories;
		return Factories;
	}

	/** ".LUA", "LUA" and "lua" all become ".lua", so a runtime's spelling never decides a mod's fate. */
	FString NormaliseExtension(const FString& In)
	{
		FString Result = In.TrimStartAndEnd().ToLower();
		if (!Result.IsEmpty() && !Result.StartsWith(TEXT("."), ESearchCase::CaseSensitive))
		{
			Result = FString(TEXT(".")) + Result;
		}

		return Result;
	}

	/** Every extension a runtime claims, normalised and deduplicated. May be empty. */
	TArray<FString> GatherExtensions(const IModScriptRuntime& Runtime)
	{
		TArray<FString> Result;
		for (const FString& Declared : Runtime.GetSourceExtensions())
		{
			const FString Normalised = NormaliseExtension(Declared);
			if (!Normalised.IsEmpty())
			{
				Result.AddUnique(Normalised);
			}
		}

		return Result;
	}

	/** ".lua, .luau", or "no file extensions at all". */
	FString JoinExtensions(const TArray<FString>& Extensions)
	{
		if (Extensions.Num() == 0)
		{
			return FString(TEXT("no file extensions at all"));
		}

		return FString::Join(Extensions, TEXT(", "));
	}

	/**
	 * What a runtime said went wrong, or a stand-in.
	 *
	 * A runtime is allowed to fail without filling in the diagnostic, and "the script failed" with no
	 * reason attached is not a diagnostic - it is a shrug. This makes sure one sentence always lands.
	 */
	FString DescribeRuntimeError(const FModDiagnostic& In)
	{
		const FString Message = In.Message.TrimStartAndEnd();
		return Message.IsEmpty() ? FString(TEXT("the runtime gave no reason")) : Message;
	}
}

//////////////////////////////////////////////////////////////////////////
// Lifetime

void UModScriptManager::Initialize(UModSubsystem* InSubsystem)
{
	using namespace ModScriptManagerPrivate;

	Subsystem = InSubsystem;

	// Sorted, so two runs of the same game build their runtimes in the same order and every list this
	// class prints is reproducible. A TMap's iteration order is not.
	TArray<FName> FactoryIds = GetRuntimeFactoryIds();

	for (const FName& FactoryId : FactoryIds)
	{
		const FModScriptRuntimeFactory* const Factory = GetFactoryRegistry().Find(FactoryId);
		if (Factory == nullptr || !Factory->IsBound())
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("The script runtime factory registered as '%s' is not bound; no runtime was built from it."),
				*FactoryId.ToString());
			continue;
		}

		// Executed rather than trusted: a language module that cannot start - no interpreter, a
		// failed self-test - answers null here, and that costs this one runtime and nothing else.
		const TSharedPtr<IModScriptRuntime> Runtime = Factory->Execute();
		if (!Runtime.IsValid())
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("The script runtime factory '%s' produced nothing; that language will be unavailable to mods."),
				*FactoryId.ToString());
			continue;
		}

		// The runtime, not the factory, is the authority on its own id - FindRuntime is called with
		// the name a manifest wrote, which must match IModScriptRuntime::GetRuntimeId.
		if (Runtime->GetRuntimeId() != FactoryId)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("The script runtime factory registered as '%s' produced a runtime that calls itself '%s'; registering it under its own id."),
				*FactoryId.ToString(), *Runtime->GetRuntimeId().ToString());
		}

		FModDiagnostic Error;
		if (!RegisterRuntime(Runtime.ToSharedRef(), Error))
		{
			UE_LOG(LogModFramework, Error, TEXT("%s"), *Error.ToString());
		}
	}

	UE_LOG(LogModFramework, Log, TEXT("Script runtimes available to mods: %s."), *DescribeRegisteredRuntimes());
}

void UModScriptManager::Shutdown()
{
	// Contexts before runtimes. A runtime destructor closes whatever states it still holds, but doing
	// it explicitly here means a mod's context is torn down while its record still says which runtime
	// owns it, which is what keeps a partly-initialised manager survivable.
	TArray<FModId> Scripted;
	ModRecords.GetKeys(Scripted);
	for (const FModId& ModId : Scripted)
	{
		UnloadModScripts(ModId);
	}

	ModRecords.Reset();
	Runtimes.Reset();
	Subsystem.Reset();
}

//////////////////////////////////////////////////////////////////////////
// Runtime factories

void UModScriptManager::RegisterRuntimeFactory(FName RuntimeId, FModScriptRuntimeFactory Factory)
{
	using namespace ModScriptManagerPrivate;

	if (RuntimeId.IsNone())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("RegisterRuntimeFactory: refusing a factory that reports no runtime id."));
		return;
	}

	if (!Factory.IsBound())
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("RegisterRuntimeFactory('%s'): refusing an unbound factory."), *RuntimeId.ToString());
		return;
	}

	// Replaces rather than refuses: a hot-reloaded language module registers again from its new
	// StartupModule, and the newest recipe is the one that still points at loaded code.
	GetFactoryRegistry().Add(RuntimeId, MoveTemp(Factory));

	UE_LOG(LogModFramework, Verbose, TEXT("Registered the '%s' script runtime factory."), *RuntimeId.ToString());
}

void UModScriptManager::UnregisterRuntimeFactory(FName RuntimeId)
{
	using namespace ModScriptManagerPrivate;

	if (GetFactoryRegistry().Remove(RuntimeId) > 0)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Unregistered the '%s' script runtime factory."), *RuntimeId.ToString());
	}
}

TArray<FName> UModScriptManager::GetRuntimeFactoryIds()
{
	using namespace ModScriptManagerPrivate;

	TArray<FName> Ids;
	GetFactoryRegistry().GetKeys(Ids);

	// FName::operator< is a comparison of internal indices, which is stable within a run but says
	// nothing about spelling, so the sort is on the text.
	Algo::Sort(Ids, [](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });
	return Ids;
}

//////////////////////////////////////////////////////////////////////////
// Runtimes

bool UModScriptManager::RegisterRuntime(TSharedRef<IModScriptRuntime> Runtime, FModDiagnostic& OutError)
{
	using namespace ModScriptManagerPrivate;

	const FName RuntimeId = Runtime->GetRuntimeId();
	if (RuntimeId.IsNone())
	{
		OutError = FModDiagnostic::Error(FName(CodeInvalidRuntime),
			TEXT("A script runtime that reports no id cannot be registered: a manifest names a runtime by that id, so nothing could ever ask for it."));
		return false;
	}

	if (Runtimes.Contains(RuntimeId))
	{
		// Never silently replaced. The first runtime may already own live mod states, and swapping it
		// out from under them would strand every one of them.
		OutError = FModDiagnostic::Error(FName(CodeDuplicateRuntime),
			FString::Printf(TEXT("A script runtime called '%s' is already registered. Unregister it first, or give the new one a different id."),
				*RuntimeId.ToString()),
			RuntimeId.ToString());
		return false;
	}

	if (GatherExtensions(Runtime.Get()).Num() == 0)
	{
		// Refused rather than warned about: LoadModScripts matches a script to a runtime by extension,
		// so a runtime claiming none could never run anything. Accepting it would produce exactly the
		// "the mod loaded and nothing happened" outcome this class exists to make impossible.
		OutError = FModDiagnostic::Error(FName(CodeInvalidRuntime),
			FString::Printf(TEXT("The '%s' script runtime claims no source file extensions, so no script could ever be matched to it. GetSourceExtensions must return at least one, such as \".lua\"."),
				*RuntimeId.ToString()),
			RuntimeId.ToString());
		return false;
	}

	Runtimes.Add(RuntimeId, Runtime);

	UE_LOG(LogModFramework, Log, TEXT("Registered the '%s' script runtime (version %s, %s)."),
		*RuntimeId.ToString(),
		*Runtime->GetRuntimeVersion().ToString(),
		*JoinExtensions(GatherExtensions(Runtime.Get())));

	return true;
}

bool UModScriptManager::UnregisterRuntime(FName RuntimeId)
{
	TSharedRef<IModScriptRuntime>* const Found = Runtimes.Find(RuntimeId);
	if (Found == nullptr)
	{
		return false;
	}

	// Every mod this runtime was holding goes with it. Leaving the records behind would leave
	// CallModFunction pointing at a runtime that no longer exists.
	TArray<FModId> Orphaned;
	for (const TPair<FModId, FModScriptRecord>& Pair : ModRecords)
	{
		if (Pair.Value.RuntimeId == RuntimeId)
		{
			Orphaned.Add(Pair.Key);
		}
	}

	for (const FModId& ModId : Orphaned)
	{
		(*Found)->DestroyContext(ModId);
		ModRecords.Remove(ModId);
	}

	Runtimes.Remove(RuntimeId);

	UE_LOG(LogModFramework, Log, TEXT("Unregistered the '%s' script runtime; %d mod context(s) went with it."),
		*RuntimeId.ToString(), Orphaned.Num());

	return true;
}

IModScriptRuntime* UModScriptManager::FindRuntime(FName RuntimeId) const
{
	if (RuntimeId.IsNone())
	{
		return nullptr;
	}

	const TSharedRef<IModScriptRuntime>* const Found = Runtimes.Find(RuntimeId);
	return Found != nullptr ? &Found->Get() : nullptr;
}

TArray<FName> UModScriptManager::GetRuntimeIds() const
{
	TArray<FName> Ids;
	Runtimes.GetKeys(Ids);

	Algo::Sort(Ids, [](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });
	return Ids;
}

//////////////////////////////////////////////////////////////////////////
// Per-mod scripts

bool UModScriptManager::LoadModScripts(const FModInfo& ModInfo, UModContext* Context,
	TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModScriptManagerPrivate;

	const FModId ModId = ModInfo.GetId();
	const FModEntryPoint& EntryPoint = ModInfo.Manifest.EntryPoint;
	const FString& RootPath = ModInfo.RootPath;

	// A one-line failure builder, so every abort below takes the same exit: the diagnostic is
	// recorded and the mod's context goes with it. A mod is never left running half its own logic.
	const auto FailScripts = [&OutDiagnostics, &ModId, &RootPath](const TCHAR* Code, FString Message)
	{
		FModDiagnostic Diagnostic = FModDiagnostic::Error(FName(Code), MoveTemp(Message), RootPath);
		Diagnostic.ModId = ModId;
		OutDiagnostics.Add(MoveTemp(Diagnostic));
		return false;
	};

	// The overwhelmingly common case: the mod ships no scripting at all.
	if (EntryPoint.ScriptRuntime.IsNone())
	{
		if (EntryPoint.Scripts.Num() > 0)
		{
			// Half a scripting section. FModManifestParser::ValidateManifest already rejects this
			// shape, so reaching it means an FModInfo was built without going through the validator -
			// but saying nothing would mean a mod whose author's code simply never runs.
			FModDiagnostic Diagnostic = FModDiagnostic::Warning(FName(CodeNoScripts),
				FString::Printf(TEXT("'%s' lists %d script(s) but names no script runtime, so none of them will run. Set 'entryPoint.scriptRuntime'."),
					*ModId.ToString(), EntryPoint.Scripts.Num()),
				RootPath);
			Diagnostic.ModId = ModId;
			OutDiagnostics.Add(MoveTemp(Diagnostic));
		}

		return true;
	}

	if (EntryPoint.Scripts.Num() == 0)
	{
		// The mirror image of the case above, and checked before the runtime lookup: with no scripts
		// there is nothing to run whether or not the named runtime exists, so failing the mod over a
		// runtime it would never have used would be noise.
		FModDiagnostic Diagnostic = FModDiagnostic::Warning(FName(CodeNoScripts),
			FString::Printf(TEXT("'%s' names the '%s' script runtime but lists no scripts, so nothing will run. List them under 'entryPoint.scripts', or remove 'scriptRuntime'."),
				*ModId.ToString(), *EntryPoint.ScriptRuntime.ToString()),
			RootPath);
		Diagnostic.ModId = ModId;
		OutDiagnostics.Add(MoveTemp(Diagnostic));
		return true;
	}

	IModScriptRuntime* const Runtime = FindRuntime(EntryPoint.ScriptRuntime);
	if (Runtime == nullptr)
	{
		// An error, never a quiet skip. A mod whose scripts silently did not run looks like a mod that
		// works and does nothing, which is the single worst outcome available here.
		FString Message = FString::Printf(
			TEXT("'%s' asks for the '%s' script runtime, which is not registered. Registered runtimes: %s."),
			*ModId.ToString(), *EntryPoint.ScriptRuntime.ToString(), *DescribeRegisteredRuntimes());

		if (Runtimes.Num() == 0)
		{
			Message += TEXT(" Enable a scripting module (ModFrameworkLua ships with the framework), or register your own IModScriptRuntime through UModScriptManager::RegisterRuntimeFactory.");
		}

		return FailScripts(CodeUnknownRuntime, MoveTemp(Message));
	}

	const FName RuntimeId = Runtime->GetRuntimeId();

	// A context left from an earlier load would leak that load's globals into this one, and
	// CreateContext refuses to replace one silently for exactly that reason.
	if (Runtime->HasContext(ModId))
	{
		Runtime->DestroyContext(ModId);
	}
	ModRecords.Remove(ModId);

	FModDiagnostic ContextError;
	if (!Runtime->CreateContext(ModId, Context, ContextError))
	{
		return FailScripts(CodeContextFailed, FString::Printf(
			TEXT("The '%s' runtime could not create an execution context for '%s': %s"),
			*RuntimeId.ToString(), *ModId.ToString(), *DescribeRuntimeError(ContextError)));
	}

	// Filed before the first script runs, so a mod that fails halfway still appears in Mod.Scripts
	// with an honest count rather than vanishing from the report.
	FModScriptRecord& Record = ModRecords.Add(ModId, FModScriptRecord());
	Record.RuntimeId = RuntimeId;
	Record.DeclaredScripts = EntryPoint.Scripts.Num();

	// Everything past this point aborts through this, which destroys the context on the way out.
	const auto FailAndDestroy = [&FailScripts, Runtime, &ModId](const TCHAR* Code, FString Message)
	{
		Runtime->DestroyContext(ModId);
		return FailScripts(Code, MoveTemp(Message));
	};

	const TArray<FString> Extensions = GatherExtensions(*Runtime);

	// IN THE ORDER LISTED, always. FModEntryPoint::Scripts documents the order as part of the
	// contract because a later script may rely on what an earlier one set up.
	for (const FString& RelativePath : EntryPoint.Scripts)
	{
		// Re-checked even though FModManifestParser::ValidateManifest checks it, for the same reason
		// UModIconCache re-checks the icon path: an FModInfo can be built without ever going through
		// the validator, and this is the point where the string becomes a filesystem path.
		FString PathError;
		if (!ModPackage::IsSafeRelativePath(RelativePath, PathError))
		{
			return FailAndDestroy(CodeUnsafePath, FString::Printf(
				TEXT("'%s' declares the script path '%s', which was rejected because %s. Script paths must stay inside the mod directory, such as \"Scripts/main.lua\"."),
				*ModId.ToString(), *RelativePath, *PathError));
		}

		// The check the manifest layer deliberately skipped: it parses on machines with no runtimes
		// registered, so this is the first place both the path and the runtime are known.
		const FString Extension = NormaliseExtension(FPaths::GetExtension(RelativePath, /*bIncludeDot*/ true));
		if (!Extensions.Contains(Extension))
		{
			return FailAndDestroy(CodeUnsupportedExtension, FString::Printf(
				TEXT("'%s' lists the script '%s', which the '%s' runtime does not run; it accepts %s."),
				*ModId.ToString(), *RelativePath, *RuntimeId.ToString(), *JoinExtensions(Extensions)));
		}

		const FString AbsolutePath = FPaths::ConvertRelativePathToFull(RootPath / RelativePath);

		// FileSize answers -1 for a missing file, so this is the existence test and the size test at
		// once - and it happens before a single byte is read.
		const int64 SizeBytes = IFileManager::Get().FileSize(*AbsolutePath);
		if (SizeBytes < 0)
		{
			return FailAndDestroy(CodeNotFound, FString::Printf(
				TEXT("'%s' declares the script '%s', but there is no such file at '%s'."),
				*ModId.ToString(), *RelativePath, *AbsolutePath));
		}

		if (SizeBytes > MaxScriptFileBytes)
		{
			return FailAndDestroy(CodeTooLarge, FString::Printf(
				TEXT("The script '%s' of '%s' is %lld bytes, over the %lld byte limit."),
				*RelativePath, *ModId.ToString(), SizeBytes, MaxScriptFileBytes));
		}

		TArray<uint8> Source;
		if (!FFileHelper::LoadFileToArray(Source, *AbsolutePath))
		{
			return FailAndDestroy(CodeReadFailed, FString::Printf(
				TEXT("The script '%s' of '%s' could not be read from '%s'."),
				*RelativePath, *ModId.ToString(), *AbsolutePath));
		}

		// Bytes, not a string, and the *relative* path as the virtual name: that is what a stack
		// trace shows the mod's author, and it must not leak the player's directory layout.
		FModDiagnostic LoadError;
		if (!Runtime->LoadScript(ModId, RelativePath, Source, LoadError))
		{
			return FailAndDestroy(CodeLoadFailed, FString::Printf(
				TEXT("The script '%s' of '%s' was rejected by the '%s' runtime: %s"),
				*RelativePath, *ModId.ToString(), *RuntimeId.ToString(), *DescribeRuntimeError(LoadError)));
		}

		++Record.LoadedScripts;
	}

	Record.bLoaded = true;

	UE_LOG(LogModFramework, Log, TEXT("Ran %d '%s' script(s) for mod '%s'."),
		Record.LoadedScripts, *RuntimeId.ToString(), *ModId.ToString());

	return true;
}

bool UModScriptManager::UnloadModScripts(const FModId& ModId)
{
	FModScriptRecord Record;
	if (!ModRecords.RemoveAndCopyValue(ModId, Record))
	{
		// A mod with no scripts, which is the normal case. Not an error and not worth a log line.
		return false;
	}

	IModScriptRuntime* const Runtime = FindRuntime(Record.RuntimeId);
	if (Runtime == nullptr)
	{
		return false;
	}

	const bool bDestroyed = Runtime->DestroyContext(ModId);
	if (bDestroyed)
	{
		UE_LOG(LogModFramework, Verbose, TEXT("Destroyed the '%s' script context of mod '%s'."),
			*Record.RuntimeId.ToString(), *ModId.ToString());
	}

	return bDestroyed;
}

bool UModScriptManager::CallModFunction(const FModId& ModId, FName FunctionName)
{
	using namespace ModScriptManagerPrivate;

	if (FunctionName.IsNone())
	{
		return false;
	}

	const FModScriptRecord* const Record = ModRecords.Find(ModId);
	if (Record == nullptr || !Record->bLoaded)
	{
		return false;
	}

	IModScriptRuntime* const Runtime = FindRuntime(Record->RuntimeId);
	if (Runtime == nullptr)
	{
		return false;
	}

	// Asked before called. Every script entry point is optional - most mods define neither of the
	// lifecycle hooks - so "not defined" must not travel back up as something that went wrong.
	if (!Runtime->HasFunction(ModId, FunctionName))
	{
		return false;
	}

	FModDiagnostic CallError;
	if (Runtime->CallFunction(ModId, FunctionName, CallError))
	{
		return true;
	}

	// Reported rather than swallowed, and reported against the mod: an error inside OnModActivated is
	// the mod's own bug, and the player needs to be able to see which mod it was.
	FModDiagnostic Diagnostic = FModDiagnostic::Error(FName(CodeCallFailed),
		FString::Printf(TEXT("The script function '%s' of '%s' failed: %s"),
			*FunctionName.ToString(), *ModId.ToString(), *DescribeRuntimeError(CallError)),
		Record->RuntimeId.ToString());
	Diagnostic.ModId = ModId;
	ReportDiagnostic(ModId, Diagnostic);

	return false;
}

//////////////////////////////////////////////////////////////////////////
// Introspection

FName UModScriptManager::GetModRuntimeId(FModId ModId) const
{
	const FModScriptRecord* const Record = ModRecords.Find(ModId);
	return Record != nullptr ? Record->RuntimeId : NAME_None;
}

int32 UModScriptManager::GetModScriptCount(FModId ModId) const
{
	const FModScriptRecord* const Record = ModRecords.Find(ModId);
	return Record != nullptr ? Record->DeclaredScripts : 0;
}

bool UModScriptManager::AreScriptsLoaded(FModId ModId) const
{
	const FModScriptRecord* const Record = ModRecords.Find(ModId);
	return Record != nullptr && Record->bLoaded;
}

TArray<FModId> UModScriptManager::GetScriptedMods() const
{
	TArray<FModId> Ids;
	ModRecords.GetKeys(Ids);
	Ids.Sort();
	return Ids;
}

//////////////////////////////////////////////////////////////////////////
// Helpers

void UModScriptManager::ReportDiagnostic(const FModId& ModId, const FModDiagnostic& Diagnostic) const
{
	// AddDiagnostic mirrors the entry to the log at the verbosity matching its severity, so a
	// registered mod's problems are logged exactly once and show up under Mod.Info.
	if (const UModSubsystem* const OwningSubsystem = Subsystem.Get())
	{
		if (UModRegistry* const Registry = OwningSubsystem->GetRegistry())
		{
			if (ModId.IsValid() && Registry->IsModRegistered(ModId))
			{
				Registry->AddDiagnostic(ModId, Diagnostic);
				return;
			}
		}
	}

	switch (Diagnostic.Severity)
	{
	case EModDiagnosticSeverity::Error:
		UE_LOG(LogModFramework, Error, TEXT("%s"), *Diagnostic.ToString());
		break;
	case EModDiagnosticSeverity::Warning:
		UE_LOG(LogModFramework, Warning, TEXT("%s"), *Diagnostic.ToString());
		break;
	default:
		UE_LOG(LogModFramework, Log, TEXT("%s"), *Diagnostic.ToString());
		break;
	}
}

FString UModScriptManager::DescribeRegisteredRuntimes() const
{
	if (Runtimes.Num() == 0)
	{
		return FString(TEXT("none are registered"));
	}

	FString Result;
	for (const FName& RuntimeId : GetRuntimeIds())
	{
		if (!Result.IsEmpty())
		{
			Result += TEXT(", ");
		}
		Result += RuntimeId.ToString();
	}

	return Result;
}
