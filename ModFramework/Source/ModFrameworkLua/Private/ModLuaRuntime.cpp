// Copyright (c) 2026. Licensed for use in your own projects.

#include "ModLuaRuntime.h"

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/Map.h"
#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "HAL/PlatformTime.h"
#include "HAL/UnrealMemory.h"
#include "Internationalization/Text.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModVersion.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CString.h"
#include "Misc/Char.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "ModFrameworkLuaModule.h"
#include "Runtime/ModContext.h"
#include "Templates/UniquePtr.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"
#include "UObject/WeakObjectPtrTemplates.h"

#if MODFRAMEWORK_WITH_LUA
#include "ModLuaBindings.h"

// Lua is C. Everything from it must be reached through extern "C" or the symbols will not link.
THIRD_PARTY_INCLUDES_START
extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
THIRD_PARTY_INCLUDES_END
#endif

#define LOCTEXT_NAMESPACE "ModLuaRuntime"

namespace
{
	/** Diagnostic codes. Stable and machine-readable, matching the framework's "Area.Thing" habit. */
	const FName CodeUnavailable(TEXT("Lua.Unavailable"));
	const FName CodeInvalidModId(TEXT("Lua.InvalidModId"));
	const FName CodeInvalidContext(TEXT("Lua.InvalidContext"));
	const FName CodeDuplicateContext(TEXT("Lua.DuplicateContext"));
	const FName CodeNoContext(TEXT("Lua.NoContext"));
	const FName CodeContextExpired(TEXT("Lua.ContextExpired"));
	const FName CodeStateCreationFailed(TEXT("Lua.StateCreationFailed"));
	const FName CodeSandboxFailed(TEXT("Lua.SandboxFailed"));
	const FName CodeBindingsFailed(TEXT("Lua.BindingsFailed"));
	const FName CodeEmptySource(TEXT("Lua.EmptySource"));
	const FName CodeSourceTooLarge(TEXT("Lua.SourceTooLarge"));
	const FName CodePrecompiledRejected(TEXT("Lua.PrecompiledRejected"));
	const FName CodeSyntaxError(TEXT("Lua.SyntaxError"));
	const FName CodeRuntimeError(TEXT("Lua.RuntimeError"));
	const FName CodeMemoryBudget(TEXT("Lua.MemoryBudget"));
	const FName CodeInstructionBudget(TEXT("Lua.InstructionBudget"));
	const FName CodeFunctionMissing(TEXT("Lua.FunctionMissing"));

	FModDiagnostic MakeLuaError(const FModId& ModId, const FName Code, FString Message, FString Context = FString())
	{
		FModDiagnostic Diagnostic = FModDiagnostic::Error(Code, MoveTemp(Message), MoveTemp(Context));
		Diagnostic.ModId = ModId;
		return Diagnostic;
	}

	FModDiagnostic MakeLuaInfo(const FModId& ModId, const FName Code, FString Message, FString Context = FString())
	{
		FModDiagnostic Diagnostic = FModDiagnostic::Info(Code, MoveTemp(Message), MoveTemp(Context));
		Diagnostic.ModId = ModId;
		return Diagnostic;
	}
}

#if MODFRAMEWORK_WITH_LUA

/** One script LoadScript was given, kept verbatim so ReloadScripts can replay it. */
struct FModLuaScript
{
	FString VirtualPath;
	TArray<uint8> Source;
};

/**
 * Everything one mod's VM needs that is not inside the VM.
 *
 * Heap allocated and never moved: the address is handed to lua_newstate as the allocator's user
 * data and stamped into the state's extra space, so both the allocator and every C function can
 * find it again. A record that moved would leave the VM holding a dangling pointer, which is why
 * FModLuaRuntime::States stores TUniquePtrs rather than values.
 */
struct FModLuaState
{
	FModId ModId;

	/** Weak on purpose: a state can outlive its context during teardown and must fail safe. */
	TWeakObjectPtr<UModContext> Context;

	lua_State* L = nullptr;

	/** Seed handed to lua_newstate. Derived from the mod id, never from a clock - see ComputeSeed. */
	uint32 Seed = 0;

	// --- Memory budget ---------------------------------------------------------------------

	/** Bytes. Zero means unlimited. */
	int64 MaxBytes = ModLuaLimits::DefaultMemoryBudgetBytes;
	int64 BytesInUse = 0;
	int64 PeakBytes = 0;

	/** Set when the allocator last refused. Purely for diagnostics - memory errors stay catchable. */
	bool bMemoryBudgetHit = false;

	// --- Instruction budget ----------------------------------------------------------------

	/** VM instructions per entry point. Zero means unlimited. */
	int64 MaxInstructionsPerCall = ModLuaLimits::DefaultInstructionsPerCall;
	int64 InstructionsThisCall = 0;
	int64 LastCallInstructions = 0;
	int32 HookGranularity = ModLuaLimits::HookGranularity;

	/**
	 * Sticky for the duration of one entry point once the count hook has aborted the script. It is
	 * what makes the abort unswallowable - see LuaFinishProtectedCall.
	 */
	bool bInstructionBudgetHit = false;

	// --- Misc --------------------------------------------------------------------------------

	/** Reference point for os.clock, so a script sees time since its own state was created. */
	double CreatedAtSeconds = 0.0;

	/** Partial `warn` message, accumulated until Lua signals the final piece. */
	FString WarnBuffer;

	/** Sources handed to LoadScript, in load order, for hot reload. */
	TArray<FModLuaScript> Scripts;

	~FModLuaState()
	{
		if (L != nullptr)
		{
			// Closes every coroutine, runs every finaliser and returns every byte through the
			// allocator below, which is still valid: `this` outlives its own destructor body.
			lua_close(L);
			L = nullptr;
		}
	}
};

namespace
{
	/**
	 * The record behind a running state.
	 *
	 * lua_newthread memcpys the extra space from the main thread into every coroutine (lstate.c),
	 * so this resolves inside a coroutine too - which matters, because the count hook is inherited
	 * by coroutines as well and has to find the same budget.
	 */
	FModLuaState* StateFromLua(lua_State* L)
	{
		static_assert(LUA_EXTRASPACE >= sizeof(FModLuaState*),
			"LUA_EXTRASPACE must be wide enough to hold one pointer; see luaconf.h.");
		return *static_cast<FModLuaState**>(lua_getextraspace(L));
	}

	/** A Lua byte string as an FString, length-clamped. Lua strings may contain anything at all. */
	FString LuaToFString(const char* Bytes, const size_t Length)
	{
		if (Bytes == nullptr)
		{
			return FString();
		}

		const int32 Clamped = static_cast<int32>(
			FMath::Min<size_t>(Length, static_cast<size_t>(ModLuaLimits::MaxLuaTextChars)));
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes), Clamped);
	}

	/** The value on top of the stack as text, without ever converting (and so allocating) in place. */
	FString LuaStringAt(lua_State* L, const int Index)
	{
		if (lua_type(L, Index) != LUA_TSTRING)
		{
			return FString();
		}

		size_t Length = 0;
		const char* Bytes = lua_tolstring(L, Index, &Length);
		return LuaToFString(Bytes, Length);
	}

	/**
	 * Pulls "<source>:<line>:" off the front of a Lua error message.
	 *
	 * "A mod's script errored" with no location is useless to the person who has to fix it, and Lua
	 * only ever reports the position inside the message text, so it has to be parsed back out.
	 */
	void ExtractLocation(const FString& Message, FString& OutSource, int32& OutLine)
	{
		OutSource.Reset();
		OutLine = 0;

		int32 NewLineIndex = INDEX_NONE;
		const FString FirstLine = Message.FindChar(TEXT('\n'), NewLineIndex)
			? Message.Left(NewLineIndex)
			: Message;

		for (int32 Index = 0; Index < FirstLine.Len(); ++Index)
		{
			if (FirstLine[Index] != TEXT(':'))
			{
				continue;
			}

			int32 DigitsEnd = Index + 1;
			while (DigitsEnd < FirstLine.Len() && FChar::IsDigit(FirstLine[DigitsEnd]))
			{
				++DigitsEnd;
			}

			if (DigitsEnd > Index + 1 && DigitsEnd < FirstLine.Len() && FirstLine[DigitsEnd] == TEXT(':'))
			{
				OutSource = FirstLine.Left(Index);
				OutLine = FCString::Atoi(*FirstLine.Mid(Index + 1, DigitsEnd - Index - 1));
				return;
			}
		}
	}

	/**
	 * The seed lua_newstate uses to randomise string hashing.
	 *
	 * DERIVED FROM THE MOD ID, NEVER FROM A CLOCK. Lua's own luaL_makeseed mixes the time and a few
	 * stack addresses; that would make table iteration order differ between two runs of the same
	 * mods, which is precisely the class of "works on my machine" bug that deterministic load order
	 * exists to prevent. GetTypeHash(FName) is the tempting shortcut and is wrong for the same
	 * reason: an FName's hash follows the name table's allocation order, which is a property of the
	 * session, not of the string. A CRC of the characters is stable everywhere, forever.
	 */
	uint32 ComputeSeed(const FModId& ModId)
	{
		const FString IdString = ModId.ToString();
		return FCrc::StrCrc32<TCHAR>(*IdString);
	}

	// --- Lua callbacks -------------------------------------------------------------------------
	//
	// Everything below runs inside the VM. Two rules apply to all of it:
	//
	//  1. The module is built with LUA_USE_LONGJMP, so a Lua error unwinds the C stack with longjmp
	//     and NO C++ DESTRUCTOR RUNS. Never hold an FString, TArray or any other non-trivially
	//     destructible local across a call that can raise. In practice: do all Lua work first, then
	//     build UE objects, then return.
	//  2. Scripts are untrusted. Nothing here asserts, checks or assumes a shape.

	/**
	 * The allocator every mod's state is created with.
	 *
	 * This is the memory budget. Lua asks for every byte it uses through here, so refusing is both
	 * complete and clean: Lua responds to a null by running an emergency collection, retrying, and
	 * only then raising a normal LUA_ERRMEM, which unwinds like any other error. The alternative -
	 * letting a mod allocate until the process dies - is not a failure mode a game can report.
	 *
	 * Note the calling convention Lua uses: `Ptr == nullptr` means "new block", and in that case
	 * OldSize is not a size at all but a tag describing what is being allocated. It must not be
	 * counted, or the running total drifts up and every mod eventually gets refused for free.
	 */
	void* LuaAlloc(void* Ud, void* Ptr, const size_t OldSize, const size_t NewSize)
	{
		FModLuaState* const State = static_cast<FModLuaState*>(Ud);

		if (NewSize == 0)
		{
			if (Ptr != nullptr)
			{
				if (State != nullptr)
				{
					State->BytesInUse -= static_cast<int64>(OldSize);
				}
				FMemory::Free(Ptr);
			}
			return nullptr;
		}

		const int64 PreviousBytes = (Ptr != nullptr) ? static_cast<int64>(OldSize) : 0;
		const int64 Delta = static_cast<int64>(NewSize) - PreviousBytes;

		if (State != nullptr && State->MaxBytes > 0 && Delta > 0
			&& State->BytesInUse + Delta > State->MaxBytes)
		{
			// Refuse. Freeing is always allowed, so a script that hits this can still unwind.
			State->bMemoryBudgetHit = true;
			return nullptr;
		}

		void* const NewPtr = FMemory::Realloc(Ptr, NewSize);
		if (NewPtr == nullptr)
		{
			return nullptr;
		}

		if (State != nullptr)
		{
			State->BytesInUse += Delta;
			State->PeakBytes = FMath::Max(State->PeakBytes, State->BytesInUse);
		}
		return NewPtr;
	}

	/**
	 * The count hook. This is the CPU budget.
	 *
	 * A capability sandbox does nothing about `while true do end`, and there is no other way to
	 * regain control of a Lua script that will not yield. Raising from the hook is the documented
	 * mechanism and unwinds exactly like a script-level error.
	 */
	void LuaInstructionHook(lua_State* L, lua_Debug* /*Debug*/)
	{
		FModLuaState* const State = StateFromLua(L);
		if (State == nullptr)
		{
			return;
		}

		State->InstructionsThisCall += State->HookGranularity;

		if (State->MaxInstructionsPerCall <= 0)
		{
			return;
		}
		if (!State->bInstructionBudgetHit && State->InstructionsThisCall <= State->MaxInstructionsPerCall)
		{
			return;
		}

		// Sticky for the rest of this entry point. A script may catch the error (Lua has no
		// uncatchable errors), so the abort has to keep re-arriving until the call really does
		// unwind - see LuaFinishProtectedCall, which refuses to let pcall swallow it.
		State->bInstructionBudgetHit = true;

		// Fire on the very next instruction from here on, so a script that does manage to trap the
		// error somewhere makes essentially no progress before being aborted again.
		lua_sethook(L, &LuaInstructionHook, LUA_MASKCOUNT, 1);

		// A plain char buffer, not an FString: luaL_error longjmps out and nothing here would be
		// destroyed.
		ANSICHAR Message[192];
		FCStringAnsi::Snprintf(Message, UE_ARRAY_COUNT(Message),
			"instruction budget exhausted (limit %lld per call); the script was aborted",
			static_cast<long long>(State->MaxInstructionsPerCall));
		(void)luaL_error(L, "%s", Message);
	}

	/**
	 * Message handler installed for every protected call, so the error arrives with a traceback
	 * instead of a bare string. Modelled on the standard interpreter's msghandler.
	 */
	int LuaMessageHandler(lua_State* L)
	{
		const char* Message = lua_tostring(L, 1);
		if (Message == nullptr)
		{
			if (luaL_callmeta(L, 1, "__tostring") != 0 && lua_type(L, -1) == LUA_TSTRING)
			{
				return 1;
			}
			Message = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
		}

		luaL_traceback(L, L, Message, 1);
		return 1;
	}

	/** Panic handler. Unreachable if every entry point is protected - which is the point of it. */
	int LuaPanic(lua_State* L)
	{
		const FString Message = LuaStringAt(L, -1);
		UE_LOG(LogModLua, Error,
			TEXT("Unprotected Lua error - the process is about to abort. This is a framework bug, ")
			TEXT("not a mod bug: every call into Lua is supposed to be protected. %s"),
			Message.IsEmpty() ? TEXT("(error object is not a string)") : *Message);
		return 0;
	}

	/**
	 * Routes Lua's `warn` to the owning mod's log so it is attributed rather than dumped on stderr.
	 * Lua delivers one warning as a run of pieces, with ToCont set on all but the last.
	 */
	void LuaWarn(void* Ud, const char* Message, const int ToCont)
	{
		FModLuaState* const State = static_cast<FModLuaState*>(Ud);
		if (State == nullptr || Message == nullptr)
		{
			return;
		}

		if (State->WarnBuffer.Len() < ModLuaLimits::MaxLuaTextChars)
		{
			State->WarnBuffer += LuaToFString(Message, FCStringAnsi::Strlen(Message));
		}

		if (ToCont != 0)
		{
			return;
		}

		// A leading '@' marks one of Lua's control messages ("@on", "@off"); it is not output.
		const bool bIsControlMessage = State->WarnBuffer.StartsWith(TEXT("@"), ESearchCase::CaseSensitive);
		if (!bIsControlMessage && !State->WarnBuffer.IsEmpty())
		{
			if (UModContext* const Context = State->Context.Get())
			{
				Context->LogWarning(State->WarnBuffer);
			}
		}

		State->WarnBuffer.Reset();
	}

	/**
	 * The sandbox's `print`.
	 *
	 * Stock print writes to stdout, where a packaged game's script output goes to die unattributed.
	 * Routing it through UModContext::LogInfo means every line arrives prefixed with the mod id,
	 * newline-safe and length-capped, and reaches the in-game console - which is the only way a
	 * player can tell which mod is talking.
	 */
	int LuaPrint(lua_State* L)
	{
		const int NumArgs = lua_gettop(L);

		// Two stack slots per argument: the separator and the converted string.
		luaL_checkstack(L, 2 * NumArgs + LUA_MINSTACK, "too many arguments to print");

		int Pushed = 0;
		for (int Index = 1; Index <= NumArgs; ++Index)
		{
			if (Index > 1)
			{
				lua_pushliteral(L, "\t");
				++Pushed;
			}

			// May run a __tostring metamethod, so may raise. Nothing of ours is alive yet.
			luaL_tolstring(L, Index, nullptr);
			++Pushed;
		}

		if (Pushed == 0)
		{
			lua_pushliteral(L, "");
			Pushed = 1;
		}

		lua_concat(L, Pushed);

		// Everything above this line can raise; nothing below it can. Only now is it safe to build
		// a UE object with a destructor.
		size_t Length = 0;
		const char* const Text = lua_tolstring(L, -1, &Length);

		FModLuaState* const State = StateFromLua(L);
		if (State != nullptr)
		{
			if (UModContext* const Context = State->Context.Get())
			{
				Context->LogInfo(LuaToFString(Text, Length));
			}
		}

		return 0;
	}

	/** os.time. Seconds since the Unix epoch, UTC. No table form - the sandbox has no locale. */
	int LuaOsTime(lua_State* L)
	{
		lua_pushinteger(L, static_cast<lua_Integer>(FDateTime::UtcNow().ToUnixTimestamp()));
		return 1;
	}

	/** os.clock. Seconds since this mod's state was created, not CPU time - close enough, and monotonic. */
	int LuaOsClock(lua_State* L)
	{
		const FModLuaState* const State = StateFromLua(L);
		const double Now = FPlatformTime::Seconds();
		lua_pushnumber(L, static_cast<lua_Number>(State != nullptr ? Now - State->CreatedAtSeconds : Now));
		return 1;
	}

	/**
	 * Shared tail of the sandbox's pcall and xpcall. Identical to Lua's own finishpcall except for
	 * the first branch.
	 *
	 * WHY pcall IS REPLACED AT ALL: Lua has no uncatchable error, so stock pcall lets a script trap
	 * the count hook's abort and carry straight on looping - the CPU budget would be advisory, and
	 * this framework does not ship advisory boundaries. Re-raising when the budget flag is set
	 * makes exactly one error uncatchable while leaving every ordinary error catchable, which is
	 * what a mod author actually needs pcall for.
	 *
	 * Memory errors are deliberately NOT treated this way: Lua only raises one after an emergency
	 * collection failed to help, and a script that catches it, drops some references and continues
	 * is behaving correctly.
	 */
	int LuaFinishProtectedCall(lua_State* L, const int Status, const lua_KContext Extra)
	{
		if (Status != LUA_OK && Status != LUA_YIELD)
		{
			const FModLuaState* const State = StateFromLua(L);
			if (State != nullptr && State->bInstructionBudgetHit)
			{
				// The error object is on top of the stack, which is what lua_error re-raises.
				return lua_error(L);
			}

			lua_pushboolean(L, 0);
			lua_pushvalue(L, -2);
			return 2;
		}

		return lua_gettop(L) - static_cast<int>(Extra);
	}

	int LuaSandboxPCall(lua_State* L)
	{
		luaL_checkany(L, 1);
		lua_pushboolean(L, 1);
		lua_insert(L, 1);
		const int Status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, &LuaFinishProtectedCall);
		return LuaFinishProtectedCall(L, Status, 0);
	}

	int LuaSandboxXPCall(lua_State* L)
	{
		const int NumArgs = lua_gettop(L);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		lua_pushboolean(L, 1);
		lua_pushvalue(L, 1);
		lua_rotate(L, 3, 2);
		const int Status = lua_pcallk(L, NumArgs - 2, LUA_MULTRET, 2, 2, &LuaFinishProtectedCall);
		return LuaFinishProtectedCall(L, Status, 2);
	}

	/**
	 * Builds the sandbox. Runs inside a lua_pcall, because luaL_requiref and every table write
	 * below can raise a memory error, and a mod with a tight budget must fail cleanly rather than
	 * panic the process.
	 */
	int LuaBuildSandbox(lua_State* L)
	{
		FModLuaState* const State = StateFromLua(L);

		// NEVER luaL_openlibs. It is a one-line way to delete this module's entire reason to exist:
		// it opens `io` (the filesystem), `os` (os.execute, os.exit, os.remove, os.getenv), the
		// `package` library (require, loadlib, and with them arbitrary DLL loading) and `debug`
		// (which can rewrite upvalues, reach other coroutines' stacks and walk straight out of any
		// sandbox built on top of it). Those four are not merely unnecessary here - each one alone
		// makes a permission system meaningless. Open the harmless ones individually instead.
		static const luaL_Reg SandboxLibraries[] =
		{
			{ LUA_GNAME,       luaopen_base },       // assert/pairs/type/tostring/pcall - unavoidable
			{ LUA_TABLIBNAME,  luaopen_table },      // pure data manipulation
			{ LUA_STRLIBNAME,  luaopen_string },     // pure data manipulation
			{ LUA_MATHLIBNAME, luaopen_math },       // pure computation
			{ LUA_COLIBNAME,   luaopen_coroutine },  // in-VM only; coroutines inherit the count hook
		};

		for (const luaL_Reg& Library : SandboxLibraries)
		{
			luaL_requiref(L, Library.name, Library.func, 1);
			lua_pop(L, 1);
		}

		lua_pushglobaltable(L);
		const int Globals = lua_gettop(L);

		// Base-library globals removed, each for its own reason. A future reader will be tempted to
		// put these back "for convenience"; the reason each one is gone is written down so that
		// temptation has to be argued with rather than merely acted on.
		static const char* const RemovedGlobals[] =
		{
			// Read and execute any file the game process can reach. There is no version of this
			// that respects a permission system.
			"dofile",
			"loadfile",

			// Compiles a string - or, given a bytecode string, loads it without going through the
			// parser at all. Precompiled chunks are unverified by design and malformed ones are a
			// documented way to crash the VM deliberately. LoadScript exists precisely so that all
			// code entering a state has been through the text parser.
			"load",

			// Only exists once `package` is opened, which it is not. Cleared anyway so that adding
			// package later cannot quietly re-open the module search path.
			"require",

			// Lets a script stop the collector outright (and so escape its memory budget's intent
			// by ballooning garbage), or force a full collection every frame and tank the frame
			// time for the whole game. The framework offers CollectGarbage per mod instead, where
			// the game decides when it happens.
			"collectgarbage",

			// Replaced below rather than removed: stock print writes to stdout, unattributed.
			"print",
		};

		for (const char* const Name : RemovedGlobals)
		{
			lua_pushnil(L);
			lua_setfield(L, Globals, Name);
		}

		lua_pushcfunction(L, &LuaPrint);
		lua_setfield(L, Globals, "print");

		// See LuaFinishProtectedCall: these two exist only so a script cannot swallow the count
		// hook's abort. In every other respect they behave exactly like the originals.
		lua_pushcfunction(L, &LuaSandboxPCall);
		lua_setfield(L, Globals, "pcall");
		lua_pushcfunction(L, &LuaSandboxXPCall);
		lua_setfield(L, Globals, "xpcall");

		// A hand-built `os` with two harmless readers. NOT luaopen_os, which would also bring
		// os.execute (shell), os.exit (kills the game), os.remove and os.rename (the filesystem),
		// os.tmpname and os.getenv (environment disclosure).
		lua_createtable(L, 0, 2);
		lua_pushcfunction(L, &LuaOsTime);
		lua_setfield(L, -2, "time");
		lua_pushcfunction(L, &LuaOsClock);
		lua_setfield(L, -2, "clock");
		lua_setfield(L, Globals, "os");

		// string.dump turns a function back into bytecode. Nothing in this sandbox can load
		// bytecode, so its only remaining use is producing something to smuggle out - and if a
		// future change ever re-adds `load`, this would be half of a working escape.
		lua_getfield(L, Globals, LUA_STRLIBNAME);
		if (lua_istable(L, -1))
		{
			lua_pushnil(L);
			lua_setfield(L, -2, "dump");
		}
		lua_pop(L, 1);

		// math.random is seeded from the clock and a stack address when the library opens. Re-seed
		// from the mod id for the same reason the state's own seed is derived that way: two runs of
		// the same mod set should behave the same, and an address-derived seed is also a small ASLR
		// disclosure to untrusted code.
		lua_getfield(L, Globals, LUA_MATHLIBNAME);
		if (lua_istable(L, -1))
		{
			lua_getfield(L, -1, "randomseed");
			if (lua_isfunction(L, -1))
			{
				lua_pushinteger(L, static_cast<lua_Integer>(State != nullptr ? State->Seed : 0u));
				lua_call(L, 1, 2);  // Protected by the pcall this whole function runs under.
				lua_pop(L, 2);
			}
			else
			{
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);

		lua_settop(L, 0);
		return 0;
	}

	/** Carries the binding call's inputs and result across a frame that longjmp may skip. */
	struct FInstallBindingsPayload
	{
		UModContext* Context = nullptr;
		FModDiagnostic* OutError = nullptr;
		bool bSucceeded = false;
	};

	/**
	 * Calls the binding layer from inside a protected call, so a Lua error it raises is caught
	 * rather than reaching UE. The payload lives in the caller's frame precisely because longjmp
	 * would skip this one.
	 */
	int LuaInstallBindings(lua_State* L)
	{
		FInstallBindingsPayload* const Payload =
			static_cast<FInstallBindingsPayload*>(lua_touserdata(L, 1));
		if (Payload == nullptr || Payload->OutError == nullptr)
		{
			return 0;
		}

		Payload->bSucceeded = ModLuaBindings::InstallModContext(L, Payload->Context, *Payload->OutError);
		return 0;
	}

	/**
	 * Raw read of a global, run under pcall.
	 *
	 * Raw because a script may install a metatable on _G, and lua_getglobal honours __index - so
	 * merely asking whether a mod defines OnLoad could run arbitrary script code, or raise outside
	 * a protected call and panic the process. The name arrives as light userdata so that pushing it
	 * (which allocates, and so can raise) also happens inside the protection.
	 */
	int LuaRawGetGlobal(lua_State* L)
	{
		const char* const Name = static_cast<const char*>(lua_touserdata(L, 1));
		if (Name == nullptr)
		{
			lua_pushnil(L);
			return 1;
		}

		lua_pushglobaltable(L);
		lua_pushstring(L, Name);
		lua_rawget(L, -2);
		return 1;
	}

	// --- Budget plumbing -----------------------------------------------------------------------

	/**
	 * Arms the count hook and zeroes the per-call counter.
	 *
	 * PER CALL, NOT PER LIFETIME. A mod with an every-frame tick would trip a cumulative budget
	 * eventually no matter how modest each tick was, which would punish exactly the well-behaved
	 * mods the limit is not aimed at.
	 */
	void BeginBudgetedCall(FModLuaState& State)
	{
		State.InstructionsThisCall = 0;
		State.bInstructionBudgetHit = false;
		State.bMemoryBudgetHit = false;

		if (State.MaxInstructionsPerCall <= 0)
		{
			lua_sethook(State.L, nullptr, 0, 0);
			return;
		}

		State.HookGranularity = static_cast<int32>(FMath::Clamp<int64>(
			State.MaxInstructionsPerCall, 1, ModLuaLimits::HookGranularity));
		lua_sethook(State.L, &LuaInstructionHook, LUA_MASKCOUNT, State.HookGranularity);
	}

	/**
	 * Disarms the hook once the call is over.
	 *
	 * Not cosmetic: lua_gc can run __gc metamethods written in Lua, so a hook left armed between
	 * calls could abort a collection - raising an error from inside the collector, from C code that
	 * is not expecting one.
	 */
	void EndBudgetedCall(FModLuaState& State)
	{
		State.LastCallInstructions = State.InstructionsThisCall;
		lua_sethook(State.L, nullptr, 0, 0);
	}

	/**
	 * Calls whatever is on the stack under NumArgs arguments, protected, budgeted, and with a
	 * traceback handler. Consumes the function and its arguments; discards results.
	 */
	bool ProtectedCall(FModLuaState& State, const int NumArgs, const FName FallbackCode,
		const FString& Where, FModDiagnostic& OutError)
	{
		lua_State* const L = State.L;

		// Neither of these can allocate, so neither can raise before protection is in place.
		const int FunctionIndex = lua_gettop(L) - NumArgs;
		lua_pushcfunction(L, &LuaMessageHandler);
		lua_insert(L, FunctionIndex);

		BeginBudgetedCall(State);
		const int Status = lua_pcall(L, NumArgs, 0, FunctionIndex);
		EndBudgetedCall(State);

		lua_remove(L, FunctionIndex);

		if (Status == LUA_OK)
		{
			return true;
		}

		// The handler already turned the error into a string with a traceback attached. If even
		// that failed - LUA_ERRERR, or a memory error inside the handler - the object may be
		// anything, so it is read without converting, which would allocate.
		FString Message = LuaStringAt(L, -1);
		lua_pop(L, 1);

		FName Code = FallbackCode;
		if (State.bInstructionBudgetHit)
		{
			Code = CodeInstructionBudget;
		}
		else if (Status == LUA_ERRMEM || State.bMemoryBudgetHit)
		{
			Code = CodeMemoryBudget;
			if (Message.IsEmpty())
			{
				Message = FString::Printf(
					TEXT("The script ran out of memory against its %lld byte budget."), State.MaxBytes);
			}
		}
		else if (Status == LUA_ERRSYNTAX)
		{
			Code = CodeSyntaxError;
		}

		if (Message.IsEmpty())
		{
			Message = FString::Printf(TEXT("Lua reported error status %d with no message."), Status);
		}

		FString Source;
		int32 Line = 0;
		ExtractLocation(Message, Source, Line);

		const FString Context = (Line > 0)
			? FString::Printf(TEXT("%s:%d"), Source.IsEmpty() ? *Where : *Source, Line)
			: Where;

		OutError = MakeLuaError(State.ModId, Code, MoveTemp(Message), Context);
		return false;
	}

	/**
	 * Pushes the named global onto the stack. Leaves exactly one value there on success and
	 * nothing on failure.
	 */
	bool PushGlobal(FModLuaState& State, const FString& Name, FModDiagnostic& OutError)
	{
		lua_State* const L = State.L;

		if (lua_checkstack(L, 4) == 0)
		{
			OutError = MakeLuaError(State.ModId, CodeMemoryBudget,
				TEXT("The Lua stack could not be grown to look up a global."), Name);
			return false;
		}

		const FTCHARToUTF8 NameUtf8(*Name);

		lua_pushcfunction(L, &LuaRawGetGlobal);
		lua_pushlightuserdata(L, const_cast<ANSICHAR*>(NameUtf8.Get()));

		// No traceback handler: LuaRawGetGlobal cannot run script code, so the only possible
		// failure is memory, whose message needs no stack.
		const int Status = lua_pcall(L, 1, 1, 0);
		if (Status == LUA_OK)
		{
			return true;
		}

		FString Message = LuaStringAt(L, -1);
		lua_pop(L, 1);
		if (Message.IsEmpty())
		{
			Message = FString::Printf(TEXT("Lua reported error status %d looking up a global."), Status);
		}

		OutError = MakeLuaError(State.ModId,
			(Status == LUA_ERRMEM || State.bMemoryBudgetHit) ? CodeMemoryBudget : CodeRuntimeError,
			MoveTemp(Message), Name);
		return false;
	}
}

#endif // MODFRAMEWORK_WITH_LUA

// --- FModLuaRuntime --------------------------------------------------------------------------

FModLuaRuntime::FModLuaRuntime() = default;

FModLuaRuntime::~FModLuaRuntime()
{
	DestroyAllContexts();
}

bool FModLuaRuntime::IsAvailable()
{
#if MODFRAMEWORK_WITH_LUA
	return true;
#else
	return false;
#endif
}

FName FModLuaRuntime::GetRuntimeId() const
{
	// Stable and version-free: a manifest asks for "Lua" and expresses which Lua through
	// GetRuntimeVersion, the same way it expresses every other dependency.
	return FName(TEXT("Lua"));
}

FText FModLuaRuntime::GetDisplayName() const
{
	return LOCTEXT("LuaRuntimeDisplayName", "Lua");
}

FModVersion FModLuaRuntime::GetRuntimeVersion() const
{
	// THE BINDING SURFACE'S VERSION, NOT LUA'S. A mod cares whether the API it was written against
	// is still there; the interpreter's patch level is not what breaks it. Bump minor when
	// something is added to the sandbox, major when something is taken away or changes meaning.
	return FModVersion(0, 1, 0);
}

TArray<FString> FModLuaRuntime::GetSourceExtensions() const
{
	return TArray<FString>{ TEXT(".lua") };
}

FModLuaBudget FModLuaRuntime::GetBudget(const FModId& ModId) const
{
	if (const FModLuaBudget* const Found = Budgets.Find(ModId))
	{
		return *Found;
	}
	return FModLuaBudget();
}

void FModLuaRuntime::SetMemoryBudget(const FModId& ModId, const int64 MaxBytes)
{
	FModLuaBudget& Budget = Budgets.FindOrAdd(ModId);

	// Zero or below means unlimited. Anything else is floored, because a budget too small to build
	// a sandbox in produces a mod that fails to initialise for reasons that read like a framework
	// bug rather than a configuration one.
	Budget.MaxBytes = (MaxBytes <= 0)
		? 0
		: FMath::Max(MaxBytes, ModLuaLimits::MinMemoryBudgetBytes);

#if MODFRAMEWORK_WITH_LUA
	if (FModLuaState* const State = FindState(ModId))
	{
		// Lowering below what the mod already holds is not retroactive - nothing is taken away.
		// The next allocation that would push it further over is the one that fails.
		State->MaxBytes = Budget.MaxBytes;
	}
#endif
}

void FModLuaRuntime::SetInstructionBudget(const FModId& ModId, const int64 MaxInstructionsPerCall)
{
	FModLuaBudget& Budget = Budgets.FindOrAdd(ModId);
	Budget.MaxInstructionsPerCall = FMath::Max<int64>(MaxInstructionsPerCall, 0);

#if MODFRAMEWORK_WITH_LUA
	if (FModLuaState* const State = FindState(ModId))
	{
		// Takes effect at the next entry point; the hook is armed per call.
		State->MaxInstructionsPerCall = Budget.MaxInstructionsPerCall;
	}
#endif
}

#if MODFRAMEWORK_WITH_LUA

FModLuaState* FModLuaRuntime::FindState(const FModId& ModId) const
{
	const TUniquePtr<FModLuaState>* const Found = States.Find(ModId);
	return (Found != nullptr) ? Found->Get() : nullptr;
}

bool FModLuaRuntime::BuildState(const FModId& ModId, UModContext* Context,
	TUniquePtr<FModLuaState>& OutState, FModDiagnostic& OutError) const
{
	TUniquePtr<FModLuaState> State = MakeUnique<FModLuaState>();
	State->ModId = ModId;
	State->Context = Context;
	State->CreatedAtSeconds = FPlatformTime::Seconds();
	State->Seed = ComputeSeed(ModId);

	const FModLuaBudget Budget = GetBudget(ModId);
	State->MaxBytes = Budget.MaxBytes;
	State->MaxInstructionsPerCall = Budget.MaxInstructionsPerCall;

	// Three arguments in Lua 5.5: the seed is new, and is what randomises string hashing.
	lua_State* const L = lua_newstate(&LuaAlloc, State.Get(), State->Seed);
	if (L == nullptr)
	{
		OutError = MakeLuaError(ModId, CodeStateCreationFailed,
			FString::Printf(
				TEXT("A Lua state could not be created for '%s'. The memory budget of %lld bytes is ")
				TEXT("probably too small to hold one."),
				*ModId.ToString(), State->MaxBytes),
			ModId.ToString());
		return false;
	}

	State->L = L;

	// Stamp the record into the state's extra space before anything can run: every C function
	// below, and every coroutine created later, finds its budget and its mod through this.
	*static_cast<FModLuaState**>(lua_getextraspace(L)) = State.Get();

	lua_atpanic(L, &LuaPanic);
	lua_setwarnf(L, &LuaWarn, State.Get());

	// The sandbox is built inside a protected call: luaL_requiref allocates, and a mod on a tight
	// budget must get a diagnostic rather than panic the process.
	lua_pushcfunction(L, &LuaMessageHandler);
	lua_pushcfunction(L, &LuaBuildSandbox);
	if (lua_pcall(L, 0, 0, 1) != LUA_OK)
	{
		FString Message = LuaStringAt(L, -1);
		if (Message.IsEmpty())
		{
			Message = TEXT("no message");
		}
		OutError = MakeLuaError(ModId, CodeSandboxFailed,
			FString::Printf(TEXT("The Lua sandbox could not be built for '%s': %s"),
				*ModId.ToString(), *Message),
			ModId.ToString());
		return false;
	}
	lua_settop(L, 0);

	// Bindings last, so they are installed into a state that is already stripped. Also protected -
	// the binding layer is written by other hands and must not be able to abort the game.
	FInstallBindingsPayload Payload;
	Payload.Context = Context;
	Payload.OutError = &OutError;

	lua_pushcfunction(L, &LuaMessageHandler);
	lua_pushcfunction(L, &LuaInstallBindings);
	lua_pushlightuserdata(L, &Payload);
	const int BindStatus = lua_pcall(L, 1, 0, 1);

	if (BindStatus != LUA_OK)
	{
		FString Message = LuaStringAt(L, -1);
		if (Message.IsEmpty())
		{
			Message = TEXT("no message");
		}
		OutError = MakeLuaError(ModId, CodeBindingsFailed,
			FString::Printf(TEXT("Installing the mod API into '%s' raised a Lua error: %s"),
				*ModId.ToString(), *Message),
			ModId.ToString());
		return false;
	}

	if (!Payload.bSucceeded)
	{
		// The binding layer filled OutError itself. Only stamp the parts it cannot know.
		if (OutError.Code.IsNone())
		{
			OutError = MakeLuaError(ModId, CodeBindingsFailed,
				FString::Printf(TEXT("The mod API could not be installed into '%s'."), *ModId.ToString()),
				ModId.ToString());
		}
		else if (!OutError.ModId.IsValid())
		{
			OutError.ModId = ModId;
		}
		return false;
	}

	lua_settop(L, 0);

	OutState = MoveTemp(State);
	return true;
}

bool FModLuaRuntime::CompileAndRun(FModLuaState& State, const FString& VirtualPath,
	TArrayView<const uint8> Source, FModDiagnostic& OutError) const
{
	if (Source.Num() <= 0)
	{
		OutError = MakeLuaError(State.ModId, CodeEmptySource,
			TEXT("The script is empty."), VirtualPath);
		return false;
	}

	if (static_cast<int64>(Source.Num()) > ModLuaLimits::MaxScriptBytes)
	{
		OutError = MakeLuaError(State.ModId, CodeSourceTooLarge,
			FString::Printf(TEXT("The script is %d bytes, over the %lld byte limit."),
				Source.Num(), ModLuaLimits::MaxScriptBytes),
			VirtualPath);
		return false;
	}

	const uint8* Bytes = Source.GetData();
	int32 NumBytes = Source.Num();

	// A UTF-8 BOM is not whitespace to Lua's lexer, and Windows editors add one without asking.
	// Skipping it turns a baffling syntax error on line 1 into a script that just works.
	if (NumBytes >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
	{
		Bytes += 3;
		NumBytes -= 3;
	}

	// Mode "t" below already refuses this, but a mod author who ships a compiled chunk deserves to
	// be told what is wrong rather than "unexpected symbol". Precompiled chunks skip the parser
	// entirely and a malformed one is a documented way to crash the VM on purpose, so text is the
	// only accepted form and always will be.
	if (NumBytes >= 4 && Bytes[0] == 0x1B && Bytes[1] == 'L' && Bytes[2] == 'u' && Bytes[3] == 'a')
	{
		OutError = MakeLuaError(State.ModId, CodePrecompiledRejected,
			TEXT("This is a precompiled Lua chunk. Only text sources are accepted: bytecode bypasses ")
			TEXT("the parser, so a malformed chunk can crash the interpreter outright."),
			VirtualPath);
		return false;
	}

	lua_State* const L = State.L;
	lua_settop(L, 0);

	// A leading '@' tells Lua the chunk name is a file name, which is what makes errors read
	// "MyMod/main.lua:12:" instead of quoting the source back at the reader.
	const FString ChunkName = FString(TEXT("@")) + VirtualPath;
	const FTCHARToUTF8 ChunkNameUtf8(*ChunkName);

	// "t" - TEXT ONLY. See above.
	const int LoadStatus = luaL_loadbufferx(L,
		reinterpret_cast<const char*>(Bytes), static_cast<size_t>(NumBytes),
		ChunkNameUtf8.Get(), "t");

	if (LoadStatus != LUA_OK)
	{
		FString Message = LuaStringAt(L, -1);
		lua_pop(L, 1);
		if (Message.IsEmpty())
		{
			Message = FString::Printf(TEXT("Lua reported load status %d with no message."), LoadStatus);
		}

		FString ParsedSource;
		int32 Line = 0;
		ExtractLocation(Message, ParsedSource, Line);

		OutError = MakeLuaError(State.ModId,
			(LoadStatus == LUA_ERRMEM) ? CodeMemoryBudget : CodeSyntaxError,
			MoveTemp(Message),
			(Line > 0) ? FString::Printf(TEXT("%s:%d"), *VirtualPath, Line) : VirtualPath);
		return false;
	}

	return ProtectedCall(State, 0, CodeRuntimeError, VirtualPath, OutError);
}

#endif // MODFRAMEWORK_WITH_LUA

bool FModLuaRuntime::CreateContext(const FModId& ModId, UModContext* Context, FModDiagnostic& OutError)
{
#if MODFRAMEWORK_WITH_LUA
	if (!ModId.IsValid())
	{
		OutError = MakeLuaError(ModId, CodeInvalidModId,
			TEXT("A Lua context cannot be created for an empty mod id."));
		return false;
	}

	if (Context == nullptr)
	{
		OutError = MakeLuaError(ModId, CodeInvalidContext,
			FString::Printf(TEXT("No mod context was supplied for '%s'. The context is the only thing ")
				TEXT("bound into a state, so there is nothing to create without one."),
				*ModId.ToString()),
			ModId.ToString());
		return false;
	}

	if (States.Contains(ModId))
	{
		// Silently reusing or replacing would let a reload leak the old state's globals into what
		// the caller believes is a fresh one. DestroyContext is the way to start over.
		OutError = MakeLuaError(ModId, CodeDuplicateContext,
			FString::Printf(TEXT("'%s' already has a Lua context. Destroy it before creating another."),
				*ModId.ToString()),
			ModId.ToString());
		return false;
	}

	TUniquePtr<FModLuaState> State;
	if (!BuildState(ModId, Context, State, OutError))
	{
		// State (and with it the half-built lua_State) is released here; nothing partial is kept.
		return false;
	}

	UE_LOG(LogModLua, Log,
		TEXT("Created a sandboxed Lua state for '%s' (memory budget %lld bytes, %lld instructions ")
		TEXT("per call, seed 0x%08x)."),
		*ModId.ToString(), State->MaxBytes, State->MaxInstructionsPerCall, State->Seed);

	States.Add(ModId, MoveTemp(State));
	return true;
#else
	OutError = MakeLuaError(ModId, CodeUnavailable,
		TEXT("This build has no Lua interpreter. Drop the Lua 5.5 sources into ")
		TEXT("ModFramework/Source/ModFrameworkLua/Private/ThirdParty/Lua/src and rebuild."),
		ModId.ToString());
	return false;
#endif
}

bool FModLuaRuntime::DestroyContext(const FModId& ModId)
{
#if MODFRAMEWORK_WITH_LUA
	TUniquePtr<FModLuaState> Removed;
	if (!States.RemoveAndCopyValue(ModId, Removed) || !Removed.IsValid())
	{
		return false;
	}

	UE_LOG(LogModLua, Log, TEXT("Destroying the Lua state for '%s' (peak memory %lld bytes)."),
		*ModId.ToString(), Removed->PeakBytes);

	// ~FModLuaState closes the state here, which runs every finaliser and returns every byte.
	Removed.Reset();
	return true;
#else
	(void)ModId;
	return false;
#endif
}

bool FModLuaRuntime::HasContext(const FModId& ModId) const
{
#if MODFRAMEWORK_WITH_LUA
	return FindState(ModId) != nullptr;
#else
	(void)ModId;
	return false;
#endif
}

void FModLuaRuntime::DestroyAllContexts()
{
#if MODFRAMEWORK_WITH_LUA
	// Empty() destroys every TUniquePtr, and so closes every state.
	States.Empty();
#endif
}

TArray<FModId> FModLuaRuntime::GetActiveContexts() const
{
	TArray<FModId> Result;
#if MODFRAMEWORK_WITH_LUA
	States.GenerateKeyArray(Result);
#endif
	return Result;
}

bool FModLuaRuntime::LoadScript(const FModId& ModId, const FString& VirtualPath,
	TArrayView<const uint8> Source, FModDiagnostic& OutError)
{
#if MODFRAMEWORK_WITH_LUA
	FModLuaState* const State = FindState(ModId);
	if (State == nullptr)
	{
		OutError = MakeLuaError(ModId, CodeNoContext,
			FString::Printf(TEXT("'%s' has no Lua context, so '%s' cannot be loaded."),
				*ModId.ToString(), *VirtualPath),
			VirtualPath);
		return false;
	}

	if (!CompileAndRun(*State, VirtualPath, Source, OutError))
	{
		return false;
	}

	// Remember the source for hot reload. LoadScript is handed bytes with no indication of where
	// they came from - the whole point of the interface taking bytes - so the runtime cannot go and
	// re-read the file later. Keeping a copy is what makes ReloadScripts possible at all.
	const int32 Existing = State->Scripts.IndexOfByPredicate(
		[&VirtualPath](const FModLuaScript& Script) { return Script.VirtualPath == VirtualPath; });

	if (Existing != INDEX_NONE)
	{
		// Re-loading the same path replaces its record, so a reload does not run it twice.
		State->Scripts[Existing].Source = TArray<uint8>(Source.GetData(), Source.Num());
	}
	else if (State->Scripts.Num() < ModLuaLimits::MaxRememberedScripts)
	{
		FModLuaScript& Record = State->Scripts.AddDefaulted_GetRef();
		Record.VirtualPath = VirtualPath;
		Record.Source = TArray<uint8>(Source.GetData(), Source.Num());
	}
	else
	{
		UE_LOG(LogModLua, Warning,
			TEXT("'%s' has loaded more than %d scripts; '%s' ran but will not be replayed by a hot ")
			TEXT("reload."),
			*ModId.ToString(), ModLuaLimits::MaxRememberedScripts, *VirtualPath);
	}

	return true;
#else
	(void)VirtualPath;
	(void)Source;
	OutError = MakeLuaError(ModId, CodeUnavailable,
		TEXT("This build has no Lua interpreter, so no script can be loaded."), VirtualPath);
	return false;
#endif
}

bool FModLuaRuntime::CallFunction(const FModId& ModId, const FName FunctionName, FModDiagnostic& OutError)
{
#if MODFRAMEWORK_WITH_LUA
	FModLuaState* const State = FindState(ModId);
	if (State == nullptr)
	{
		OutError = MakeLuaError(ModId, CodeNoContext,
			FString::Printf(TEXT("'%s' has no Lua context."), *ModId.ToString()),
			ModId.ToString());
		return false;
	}

	if (FunctionName.IsNone())
	{
		OutError = MakeLuaError(ModId, CodeFunctionMissing,
			TEXT("No function name was supplied."), ModId.ToString());
		return false;
	}

	const FString Name = FunctionName.ToString();

	lua_settop(State->L, 0);
	if (!PushGlobal(*State, Name, OutError))
	{
		return false;
	}

	if (lua_isfunction(State->L, -1) == 0)
	{
		lua_settop(State->L, 0);

		// The interface is explicit that an absent entry point is not an error. The diagnostic is
		// Info severity so a caller that logs whatever comes back does not report a mod which
		// simply does not implement this hook as broken.
		OutError = MakeLuaInfo(ModId, CodeFunctionMissing,
			FString::Printf(TEXT("'%s' defines no global function named '%s'."),
				*ModId.ToString(), *Name),
			Name);
		return false;
	}

	const bool bSucceeded = ProtectedCall(*State, 0, CodeRuntimeError, Name, OutError);
	lua_settop(State->L, 0);
	return bSucceeded;
#else
	(void)FunctionName;
	OutError = MakeLuaError(ModId, CodeUnavailable,
		TEXT("This build has no Lua interpreter, so no script function can be called."),
		ModId.ToString());
	return false;
#endif
}

bool FModLuaRuntime::HasFunction(const FModId& ModId, const FName FunctionName) const
{
#if MODFRAMEWORK_WITH_LUA
	FModLuaState* const State = FindState(ModId);
	if (State == nullptr || FunctionName.IsNone())
	{
		return false;
	}

	lua_settop(State->L, 0);

	FModDiagnostic Unused;
	if (!PushGlobal(*State, FunctionName.ToString(), Unused))
	{
		return false;
	}

	const bool bIsFunction = lua_isfunction(State->L, -1) != 0;
	lua_settop(State->L, 0);
	return bIsFunction;
#else
	(void)ModId;
	(void)FunctionName;
	return false;
#endif
}

void FModLuaRuntime::CollectGarbage(const FModId& ModId)
{
#if MODFRAMEWORK_WITH_LUA
	FModLuaState* const State = FindState(ModId);
	if (State == nullptr)
	{
		return;
	}

	// Per mod, so one mod's churn does not stall another's - and with the count hook off, because a
	// finaliser written in Lua would otherwise be counted against, and possibly aborted by, a
	// budget that belongs to a call that is already over.
	lua_sethook(State->L, nullptr, 0, 0);

	const int64 Before = State->BytesInUse;
	lua_gc(State->L, LUA_GCCOLLECT);

	UE_LOG(LogModLua, Verbose, TEXT("Collected '%s': %lld -> %lld bytes."),
		*ModId.ToString(), Before, State->BytesInUse);
#else
	(void)ModId;
#endif
}

bool FModLuaRuntime::ReloadScripts(const FModId& ModId, FModDiagnostic& OutError)
{
#if MODFRAMEWORK_WITH_LUA
	FModLuaState* const Existing = FindState(ModId);
	if (Existing == nullptr)
	{
		OutError = MakeLuaError(ModId, CodeNoContext,
			FString::Printf(TEXT("'%s' has no Lua context to reload."), *ModId.ToString()),
			ModId.ToString());
		return false;
	}

	UModContext* const Context = Existing->Context.Get();
	if (Context == nullptr)
	{
		OutError = MakeLuaError(ModId, CodeContextExpired,
			FString::Printf(TEXT("'%s' has outlived its mod context, so its scripts cannot be reloaded."),
				*ModId.ToString()),
			ModId.ToString());
		return false;
	}

	// Scripts are text, so unlike a native module this really is safe - but only if a failure
	// cannot leave the mod worse off than before. So the replacement is built and fully populated
	// first, and the live state is only swapped out once every script has run. A syntax error in a
	// reloaded file therefore leaves the mod running exactly as it was.
	TArray<FModLuaScript> Snapshot = Existing->Scripts;

	TUniquePtr<FModLuaState> Fresh;
	if (!BuildState(ModId, Context, Fresh, OutError))
	{
		return false;
	}

	for (const FModLuaScript& Script : Snapshot)
	{
		if (!CompileAndRun(*Fresh, Script.VirtualPath, Script.Source, OutError))
		{
			UE_LOG(LogModLua, Warning,
				TEXT("Hot reload of '%s' failed in '%s'; the mod keeps running on its previous state."),
				*ModId.ToString(), *Script.VirtualPath);
			return false;
		}
	}

	Fresh->Scripts = MoveTemp(Snapshot);

	// Adding over the existing key destroys the old record, and with it closes the old state.
	States.Add(ModId, MoveTemp(Fresh));

	UE_LOG(LogModLua, Log, TEXT("Hot reloaded %d script(s) for '%s'."),
		States[ModId]->Scripts.Num(), *ModId.ToString());
	return true;
#else
	OutError = MakeLuaError(ModId, CodeUnavailable,
		TEXT("This build has no Lua interpreter, so there is nothing to reload."),
		ModId.ToString());
	return false;
#endif
}

int64 FModLuaRuntime::GetMemoryUsedBytes(const FModId& ModId) const
{
#if MODFRAMEWORK_WITH_LUA
	const FModLuaState* const State = FindState(ModId);
	return (State != nullptr) ? State->BytesInUse : -1;
#else
	(void)ModId;
	return -1;
#endif
}

int64 FModLuaRuntime::GetPeakMemoryBytes(const FModId& ModId) const
{
#if MODFRAMEWORK_WITH_LUA
	const FModLuaState* const State = FindState(ModId);
	return (State != nullptr) ? State->PeakBytes : -1;
#else
	(void)ModId;
	return -1;
#endif
}

int64 FModLuaRuntime::GetLastCallInstructions(const FModId& ModId) const
{
#if MODFRAMEWORK_WITH_LUA
	const FModLuaState* const State = FindState(ModId);
	return (State != nullptr) ? State->LastCallInstructions : -1;
#else
	(void)ModId;
	return -1;
#endif
}

int32 FModLuaRuntime::GetLoadedScriptCount(const FModId& ModId) const
{
#if MODFRAMEWORK_WITH_LUA
	const FModLuaState* const State = FindState(ModId);
	return (State != nullptr) ? State->Scripts.Num() : -1;
#else
	(void)ModId;
	return -1;
#endif
}

#undef LOCTEXT_NAMESPACE
