// Copyright (c) 2026. Licensed for use in your own projects.

// =================================================================================================
// THIS FILE IS THE SANDBOX BOUNDARY.
//
// Everything registered here is reachable, by name, from untrusted mod script. A mod author can
// call any of it with any arguments at all - wrong types, empty strings, gigabyte strings, integers
// that do not fit, handles it never received - and the only thing standing between that and the
// game process is the code below. FModLuaRuntime removes `io`, `os`, `package`, `debug`, `load` and
// `dofile` precisely so that this file is the *entire* list of things a script can do. That
// property is worth more than any single convenience.
//
// So: adding a function here widens the attack surface of every game that ships this plugin.
// Weigh it accordingly. Three questions before anything new goes in the `mod` table:
//
//   1. Is it something UModContext itself offers? If reaching it needs GetSubsystem(), a UObject,
//      a UClass, the reflection system or an engine global, the answer is no - that is exactly the
//      unrestricted reach Blueprint already has, and re-creating it here throws away the only
//      reason a script runtime is more defensible than Blueprint.
//   2. Does every argument get validated with luaL_check*/luaL_opt*, capped, and rejected with a
//      Lua error rather than read raw off the stack?
//   3. Does it still behave when the context has already gone? Teardown order is not something a
//      mod controls, and a dead context must produce nil and a message, never a crash.
//
// Two mechanical rules apply to every line below, and breaking either is a memory bug rather than a
// compile error:
//
//   A. The module is built with LUA_USE_LONGJMP. A Lua error unwinds the C stack with longjmp and
//      NO C++ DESTRUCTOR RUNS. Never hold an FString, TArray, FModDiagnostic or any other
//      non-trivially destructible local across a call that can raise. In practice: validate and
//      raise first, then build UE objects, then push results through the never-raising helpers in
//      this file.
//   B. The UModContext is held as a TWeakObjectPtr and resolved on every single call. A script can
//      outlive its context during teardown; a raw pointer captured anywhere here would be a
//      use-after-free reachable from mod content.
// =================================================================================================

#include "ModLuaBindings.h"

#include "API/ModAPI.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Events/ModEventTypes.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModVersion.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AssertionMacros.h"
#include "Misc/CString.h"
#include "ModFrameworkLuaModule.h"
#include "Runtime/ModContext.h"
#include "Save/ModSaveDataManager.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/NameTypes.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtrTemplates.h"

#if MODFRAMEWORK_WITH_LUA

#include "ModLuaRuntime.h"

#include <new>
#include <type_traits>

// Lua is C. Everything from it must be reached through extern "C" or the symbols will not link.
THIRD_PARTY_INCLUDES_START
extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
THIRD_PARTY_INCLUDES_END

namespace
{
	// --- Limits ----------------------------------------------------------------------------------
	//
	// A script controls the length of every string it hands over. Without a cap a mod can make the
	// engine allocate as much as its own memory budget allows in ONE call, which is not what the
	// budget is there to permit.
	//
	// Where truncating would change the meaning of the value - a half-written JSON document is not a
	// save, a truncated id names a different thing - the value is REFUSED instead. Only log lines are
	// truncated, and only because UModContext::LogInfo would truncate them anyway.

	/** Log lines, matching exactly where UModContext truncates them itself. */
	constexpr int32 MaxLogChars = UModContext::MaxLogMessageChars;

	/** Names: event ids, permission ids, api ids, config keys. Generous for a name, tiny for an abuse. */
	constexpr int32 MaxIdentifierChars = 256;

	/** A version range expression - "^1.0.0", ">=1.2 <2.0.0", "*". */
	constexpr int32 MaxVersionRangeChars = 256;

	/** One configuration string value. */
	constexpr int32 MaxConfigStringChars = 8192;

	/**
	 * Save payloads. Taken from the save manager's own limit rather than guessed, so this check can
	 * never drift into refusing a payload the framework would have accepted. Nothing is called on
	 * the manager here - the request still goes through UModContext::SaveJson.
	 */
	constexpr int64 MaxSaveJsonChars = UModSaveDataManager::MaxJsonPayloadChars;

	/**
	 * How deeply a Lua handler may re-enter Lua by broadcasting an event that reaches itself. The
	 * count hook stops a loop; only a depth limit stops a recursion, because each level is a fresh
	 * C stack frame and running out of those is a crash, not a catchable error.
	 */
	constexpr int32 MaxCallbackDepth = 8;

	// --- Registry keys and metatable names -------------------------------------------------------

	/**
	 * Registry key for the binding record. The ADDRESS of this byte is the key, so no Lua value can
	 * ever collide with it and no script can name it - the registry is not reachable from a sandbox
	 * without the debug library, which is not opened.
	 */
	const char BindingStateKey = 0;

	const char* const BindingStateMetatable = "ModFramework.Bindings";
	const char* const ApiHandleMetatable = "ModFramework.ApiHandle";

	// --- Records ---------------------------------------------------------------------------------

	/** One live `mod.subscribe`. */
	struct FModLuaSubscription
	{
		/** luaL_ref index of the Lua function in LUA_REGISTRYINDEX. LUA_NOREF once released. */
		int CallbackRef = LUA_NOREF;

		/**
		 * Rooted on purpose. A dynamic delegate stores its object weakly, so the event bus holding
		 * the subscription does NOT keep the relay alive - without this the relay is collected at
		 * the next GC and the mod's handler silently stops firing.
		 */
		TStrongObjectPtr<UModLuaEventRelay> Relay;
	};

	/**
	 * Everything the bindings need that is not inside the VM.
	 *
	 * Lives in a full userdata anchored in the state's registry, so its lifetime is exactly the
	 * state's: it is constructed while the state is being built and destroyed by the userdata's __gc
	 * when lua_close runs the finalisers. That is what guarantees no subscription outlives the VM it
	 * would call into.
	 */
	struct FModLuaBindingState
	{
		/**
		 * WEAK, NEVER RAW. The context can be destroyed while this state is still alive - a mod being
		 * unloaded is exactly that sequence - and every binding has to notice rather than dereference.
		 */
		TWeakObjectPtr<UModContext> Context;

		/** Live subscriptions, keyed by the FModEventHandle id the context issued. */
		TMap<int64, FModLuaSubscription> Subscriptions;

		/** Instruction budget for one callback into Lua. Zero means unlimited. */
		int64 MaxCallbackInstructions = ModLuaLimits::DefaultInstructionsPerCall;
		int64 CallbackInstructions = 0;
		int32 CallbackHookGranularity = ModLuaLimits::HookGranularity;
		bool bCallbackBudgetHit = false;

		/** Guards against a handler that broadcasts an event which reaches the same handler. */
		int32 DispatchDepth = 0;
	};

	/**
	 * The userdata behind `mod.request_api`.
	 *
	 * Weak for the same reason the context is: the API belongs to the game, is owned by the API
	 * registry, and can be unregistered while a script still holds a handle to it. Trivially
	 * destructible, which is why its __gc resets rather than destructs - see LuaApiHandleGC.
	 */
	struct FModLuaApiHandle
	{
		TWeakObjectPtr<UModAPI> Api;
	};

	static_assert(std::is_trivially_destructible_v<FModLuaApiHandle>,
		"LuaApiHandleGC resets an api handle instead of destroying it, which is only safe while the "
		"handle owns nothing that needs releasing.");

	// --- Never-raising primitives ----------------------------------------------------------------
	//
	// Pushing a nil, a boolean or a number cannot allocate and so cannot raise. Pushing a STRING
	// can: it allocates through the mod's budgeted allocator, and a mod at its memory ceiling gets a
	// LUA_ERRMEM out of it. Every result in this file that is a string therefore goes through
	// PushBytes, which performs the push inside its own lua_pcall and reports failure by returning
	// false. That is what lets the callers hold FStrings while producing a result.

	struct FLuaStringPush
	{
		const char* Bytes = nullptr;
		size_t Length = 0;
	};

	int LuaPushStringProtected(lua_State* L)
	{
		const FLuaStringPush* const Push = static_cast<const FLuaStringPush*>(lua_touserdata(L, 1));
		if (Push == nullptr || Push->Bytes == nullptr)
		{
			lua_pushliteral(L, "");
			return 1;
		}

		lua_pushlstring(L, Push->Bytes, Push->Length);
		return 1;
	}

	/**
	 * Pushes Length bytes as one Lua string. NEVER RAISES: on failure nothing is pushed and false
	 * comes back, so the caller's FStrings are destroyed normally and the error is reported after.
	 */
	bool PushBytes(lua_State* L, const char* Bytes, const size_t Length)
	{
		if (lua_checkstack(L, 4) == 0)
		{
			return false;
		}

		// The payload lives in this frame, which lua_pcall never unwinds past.
		FLuaStringPush Push;
		Push.Bytes = Bytes;
		Push.Length = Length;

		lua_pushcfunction(L, &LuaPushStringProtected);
		lua_pushlightuserdata(L, &Push);
		if (lua_pcall(L, 1, 1, 0) != LUA_OK)
		{
			lua_pop(L, 1);
			return false;
		}
		return true;
	}

	/** As PushBytes, for a NUL terminated literal. Never raises. */
	bool PushLiteral(lua_State* L, const char* Text)
	{
		return PushBytes(L, Text, (Text != nullptr) ? FCStringAnsi::Strlen(Text) : 0);
	}

	/** As PushBytes, for an FString. Never raises - the conversion happens in this frame. */
	bool PushText(lua_State* L, const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return PushBytes(L, Utf8.Get(), static_cast<size_t>(Utf8.Length()));
	}

	/**
	 * The standard refusal: nil plus a reason.
	 *
	 * Nil rather than false so that `local ok, why = mod.something(...)` reads the same for every
	 * binding, and so `if not mod.broadcast(id) then` still behaves. Never raises; if even the
	 * message cannot be allocated the caller still gets the nil.
	 */
	int PushFailure(lua_State* L, const char* Reason)
	{
		lua_pushnil(L);
		return PushLiteral(L, Reason) ? 2 : 1;
	}

	/** A Lua byte string as an FString, hard-capped. Lua strings may contain absolutely anything. */
	FString LuaToFString(const char* Bytes, const size_t Length, const int32 MaxChars)
	{
		if (Bytes == nullptr || MaxChars <= 0)
		{
			return FString();
		}

		const int32 Clamped = static_cast<int32>(
			FMath::Min<size_t>(Length, static_cast<size_t>(MaxChars)));
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes), Clamped);
	}

	// --- Binding record access -------------------------------------------------------------------

	void ReleaseSubscriptions(lua_State* L, FModLuaBindingState& Bindings);

	/**
	 * The record for this state, or null once it has been released.
	 *
	 * Looked up through the registry rather than kept in a closure upvalue on purpose: the record's
	 * __gc removes the registry entry *before* destroying the record, so anything that runs later -
	 * and a script can install its own __gc metamethods, which run while the state is closing - sees
	 * a clean null instead of freed memory. Never raises.
	 */
	FModLuaBindingState* FindBindings(lua_State* L)
	{
		if (L == nullptr || lua_checkstack(L, 2) == 0)
		{
			return nullptr;
		}

		lua_rawgetp(L, LUA_REGISTRYINDEX, &BindingStateKey);
		void* const Memory = (lua_type(L, -1) == LUA_TUSERDATA) ? lua_touserdata(L, -1) : nullptr;
		lua_pop(L, 1);
		return static_cast<FModLuaBindingState*>(Memory);
	}

	/**
	 * The mod's context, or null once it has gone. Never raises.
	 *
	 * A context that has died without anyone telling us also releases the subscriptions it left
	 * behind: each one holds a registry reference that would otherwise pin a closure - and with it
	 * every upvalue it captured - for the whole remaining life of the state.
	 */
	UModContext* ResolveContext(lua_State* L)
	{
		FModLuaBindingState* const Bindings = FindBindings(L);
		if (Bindings == nullptr)
		{
			return nullptr;
		}

		UModContext* const Context = Bindings->Context.Get();
		if (Context == nullptr && Bindings->Subscriptions.Num() > 0)
		{
			ReleaseSubscriptions(L, *Bindings);
		}
		return Context;
	}

	/**
	 * Cancels every subscription, drops every relay and returns every registry reference.
	 *
	 * Called from the record's __gc - so, from lua_close - and from ResolveContext once the context
	 * has gone. Never raises: luaL_unref only rewrites keys the registry already has.
	 */
	void ReleaseSubscriptions(lua_State* L, FModLuaBindingState& Bindings)
	{
		UModContext* const Context = Bindings.Context.Get();

		TArray<int> Refs;
		Refs.Reserve(Bindings.Subscriptions.Num());

		for (TPair<int64, FModLuaSubscription>& Pair : Bindings.Subscriptions)
		{
			if (Context != nullptr)
			{
				FModEventHandle Handle;
				Handle.Id = Pair.Key;
				Context->UnsubscribeFromEvent(Handle);
			}

			// Blind the relay before anything else: from here a callback that is already queued
			// finds no state and no reference, and does nothing at all.
			if (UModLuaEventRelay* const Relay = Pair.Value.Relay.Get())
			{
				Relay->LuaState = nullptr;
				Relay->CallbackRef = LUA_NOREF;
			}

			Refs.Add(Pair.Value.CallbackRef);
			Pair.Value.CallbackRef = LUA_NOREF;
		}

		Bindings.Subscriptions.Empty();

		if (lua_checkstack(L, 4) != 0)
		{
			for (const int Ref : Refs)
			{
				luaL_unref(L, LUA_REGISTRYINDEX, Ref);
			}
		}
	}

	// --- Argument checking -----------------------------------------------------------------------

	/**
	 * A non-empty, length-capped name. Raises a Lua error - which the runtime turns into a
	 * diagnostic naming the script and line - rather than reading whatever is on the stack.
	 *
	 * The returned pointer stays valid while the value remains at that stack index, which it does:
	 * nothing in this file pops an argument.
	 */
	const char* CheckName(lua_State* L, const int Arg, size_t& OutLength)
	{
		const char* const Bytes = luaL_checklstring(L, Arg, &OutLength);

		if (OutLength == 0)
		{
			luaL_argerror(L, Arg, "expected a non-empty name");
			return nullptr;
		}

		if (OutLength > static_cast<size_t>(MaxIdentifierChars))
		{
			luaL_argerror(L, Arg, "name is longer than this framework accepts");
			return nullptr;
		}

		return Bytes;
	}

	/** A lua_Integer that fits in the int32 the framework stores. Raises when it does not. */
	int32 CheckInt32(lua_State* L, const int Arg, const lua_Integer Value)
	{
		if (Value < static_cast<lua_Integer>(MIN_int32) || Value > static_cast<lua_Integer>(MAX_int32))
		{
			luaL_argerror(L, Arg, "integer is outside the 32 bit range this framework stores");
			return 0;
		}
		return static_cast<int32>(Value);
	}

	// =============================================================================================
	// The `mod` table. This list IS the sandbox's API surface - see the file header.
	// =============================================================================================

	// --- mod.id ----------------------------------------------------------------------------------

	int LuaModId(lua_State* L)
	{
		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, "mod.id: this mod's context no longer exists.");
		}

		int NumResults = 0;
		{
			const FString ModIdString = Context->GetModId().ToString();
			NumResults = PushText(L, ModIdString) ? 1 : -1;
		}

		// Only now that the FString above is gone is it safe to unwind with longjmp.
		if (NumResults < 0)
		{
			return luaL_error(L, "mod.id: the result could not be allocated within this mod's memory budget.");
		}
		return NumResults;
	}

	// --- mod.log / mod.warn / mod.error ----------------------------------------------------------

	enum class ELuaLogLevel : uint8
	{
		Info,
		Warning,
		Error
	};

	int LuaModLogImpl(lua_State* L, const ELuaLogLevel Level, const char* GoneMessage)
	{
		size_t Length = 0;
		const char* const Bytes = luaL_checklstring(L, 1, &Length);

		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, GoneMessage);
		}

		// Nothing below this line raises, so the FString is safe. UModContext prefixes every line
		// with the mod id and truncates at MaxLogMessageChars itself; capping here as well is about
		// not handing the engine a string the size of the mod's whole memory budget in the first
		// place.
		const FString Message = LuaToFString(Bytes, Length, MaxLogChars);
		switch (Level)
		{
		case ELuaLogLevel::Warning:
			Context->LogWarning(Message);
			break;
		case ELuaLogLevel::Error:
			Context->LogError(Message);
			break;
		case ELuaLogLevel::Info:
		default:
			Context->LogInfo(Message);
			break;
		}

		return 0;
	}

	int LuaModLog(lua_State* L)
	{
		return LuaModLogImpl(L, ELuaLogLevel::Info, "mod.log: this mod's context no longer exists.");
	}

	int LuaModWarn(lua_State* L)
	{
		return LuaModLogImpl(L, ELuaLogLevel::Warning, "mod.warn: this mod's context no longer exists.");
	}

	int LuaModError(lua_State* L)
	{
		return LuaModLogImpl(L, ELuaLogLevel::Error, "mod.error: this mod's context no longer exists.");
	}

	// --- mod.config_get / mod.config_set / mod.config_save ---------------------------------------

	/**
	 * mod.config_get(key, default)
	 *
	 * The DEFAULT'S LUA TYPE SELECTS THE SETTING'S TYPE, because UModContext has four separate typed
	 * getters and no way to ask what type a key holds. `3` reads an integer, `3.0` a float - Lua
	 * distinguishes the two and so does this. A missing or nil default is refused rather than
	 * guessed: guessing would silently read the wrong setting.
	 */
	int LuaModConfigGet(lua_State* L)
	{
		size_t KeyLength = 0;
		const char* const KeyBytes = CheckName(L, 1, KeyLength);

		luaL_checkany(L, 2);
		const int DefaultType = lua_type(L, 2);

		bool bDefaultBool = false;
		int32 DefaultInt = 0;
		lua_Number DefaultNumber = 0.0;
		const char* DefaultStringBytes = nullptr;
		size_t DefaultStringLength = 0;
		bool bWantInteger = false;

		switch (DefaultType)
		{
		case LUA_TBOOLEAN:
			bDefaultBool = lua_toboolean(L, 2) != 0;
			break;

		case LUA_TNUMBER:
			bWantInteger = lua_isinteger(L, 2) != 0;
			if (bWantInteger)
			{
				DefaultInt = CheckInt32(L, 2, lua_tointeger(L, 2));
			}
			else
			{
				DefaultNumber = lua_tonumber(L, 2);
			}
			break;

		case LUA_TSTRING:
			DefaultStringBytes = lua_tolstring(L, 2, &DefaultStringLength);
			if (DefaultStringLength > static_cast<size_t>(MaxConfigStringChars))
			{
				return luaL_argerror(L, 2, "string default is longer than a configuration value may be");
			}
			break;

		default:
			return luaL_argerror(L, 2,
				"expected a boolean, number or string default - its type selects the setting's type");
		}

		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, "mod.config_get: this mod's context no longer exists.");
		}

		int NumResults = 0;
		{
			const FString KeyString = LuaToFString(KeyBytes, KeyLength, MaxIdentifierChars);
			const FName Key(*KeyString);

			switch (DefaultType)
			{
			case LUA_TBOOLEAN:
				lua_pushboolean(L, Context->GetConfigBool(Key, bDefaultBool) ? 1 : 0);
				NumResults = 1;
				break;

			case LUA_TNUMBER:
				if (bWantInteger)
				{
					lua_pushinteger(L, static_cast<lua_Integer>(Context->GetConfigInt(Key, DefaultInt)));
				}
				else
				{
					lua_pushnumber(L, static_cast<lua_Number>(
						Context->GetConfigFloat(Key, static_cast<float>(DefaultNumber))));
				}
				NumResults = 1;
				break;

			case LUA_TSTRING:
			default:
			{
				const FString DefaultString =
					LuaToFString(DefaultStringBytes, DefaultStringLength, MaxConfigStringChars);
				const FString Value = Context->GetConfigString(Key, DefaultString);
				NumResults = PushText(L, Value) ? 1 : -1;
				break;
			}
			}
		}

		if (NumResults < 0)
		{
			return luaL_error(L, "mod.config_get: the result could not be allocated within this mod's memory budget.");
		}
		return NumResults;
	}

	/** mod.config_set(key, value). The value's Lua type selects the setting's type, as above. */
	int LuaModConfigSet(lua_State* L)
	{
		size_t KeyLength = 0;
		const char* const KeyBytes = CheckName(L, 1, KeyLength);

		luaL_checkany(L, 2);
		const int ValueType = lua_type(L, 2);

		bool bValueBool = false;
		int32 ValueInt = 0;
		lua_Number ValueNumber = 0.0;
		const char* ValueStringBytes = nullptr;
		size_t ValueStringLength = 0;
		bool bIsInteger = false;

		switch (ValueType)
		{
		case LUA_TBOOLEAN:
			bValueBool = lua_toboolean(L, 2) != 0;
			break;

		case LUA_TNUMBER:
			bIsInteger = lua_isinteger(L, 2) != 0;
			if (bIsInteger)
			{
				ValueInt = CheckInt32(L, 2, lua_tointeger(L, 2));
			}
			else
			{
				ValueNumber = lua_tonumber(L, 2);
			}
			break;

		case LUA_TSTRING:
			ValueStringBytes = lua_tolstring(L, 2, &ValueStringLength);
			if (ValueStringLength > static_cast<size_t>(MaxConfigStringChars))
			{
				// Refused, not truncated: a silently shortened value is a corrupted setting.
				return luaL_argerror(L, 2, "string is longer than a configuration value may be");
			}
			break;

		default:
			return luaL_argerror(L, 2, "expected a boolean, number or string value");
		}

		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, "mod.config_set: this mod's context no longer exists.");
		}

		{
			const FString KeyString = LuaToFString(KeyBytes, KeyLength, MaxIdentifierChars);
			const FName Key(*KeyString);

			switch (ValueType)
			{
			case LUA_TBOOLEAN:
				Context->SetConfigBool(Key, bValueBool);
				break;

			case LUA_TNUMBER:
				if (bIsInteger)
				{
					Context->SetConfigInt(Key, ValueInt);
				}
				else
				{
					Context->SetConfigFloat(Key, static_cast<float>(ValueNumber));
				}
				break;

			case LUA_TSTRING:
			default:
				Context->SetConfigString(Key,
					LuaToFString(ValueStringBytes, ValueStringLength, MaxConfigStringChars));
				break;
			}
		}

		return 0;
	}

	/** mod.config_save() -> boolean. Shipped defaults are never written back; only changed values are. */
	int LuaModConfigSave(lua_State* L)
	{
		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, "mod.config_save: this mod's context no longer exists.");
		}

		lua_pushboolean(L, Context->SaveConfig() ? 1 : 0);
		return 1;
	}

	// --- mod.save / mod.load ---------------------------------------------------------------------

	/**
	 * mod.save(json_string [, data_version]) -> boolean
	 *
	 * The payload is refused rather than truncated when it is too large: half a JSON document is not
	 * a save, it is a corrupted one that would be handed back to the mod on the next load.
	 */
	int LuaModSave(lua_State* L)
	{
		size_t Length = 0;
		const char* const Bytes = luaL_checklstring(L, 1, &Length);
		const lua_Integer RequestedVersion = luaL_optinteger(L, 2, 1);

		if (static_cast<int64>(Length) > MaxSaveJsonChars)
		{
			return luaL_argerror(L, 1, "save payload is larger than this framework stores");
		}

		if (RequestedVersion < 1)
		{
			return luaL_argerror(L, 2, "data version must be a positive integer");
		}
		const int32 DataVersion = CheckInt32(L, 2, RequestedVersion);

		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, "mod.save: this mod's context no longer exists.");
		}

		{
			// Byte length is an upper bound on character count for UTF-8, so this cap can only ever
			// be more permissive than the manager's own check, never less.
			const FString Json = LuaToFString(Bytes, Length, static_cast<int32>(MaxSaveJsonChars));

			// The save manager enforces "save.modify" and the size cap, and the record is keyed by
			// the context's own mod id - a mod cannot write anybody else's.
			lua_pushboolean(L, Context->SaveJson(Json, DataVersion) ? 1 : 0);
		}

		return 1;
	}

	/** mod.load() -> json_string, data_version, or nil + why. Reading needs no permission. */
	int LuaModLoad(lua_State* L)
	{
		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, "mod.load: this mod's context no longer exists.");
		}

		int NumResults = 0;
		{
			FString Json;
			int32 DataVersion = 0;
			if (!Context->LoadJson(Json, DataVersion))
			{
				NumResults = PushFailure(L, "mod.load: this mod has no save record yet.");
			}
			else if (PushText(L, Json))
			{
				lua_pushinteger(L, static_cast<lua_Integer>(DataVersion));
				NumResults = 2;
			}
			else
			{
				NumResults = -1;
			}
		}

		if (NumResults < 0)
		{
			return luaL_error(L, "mod.load: the save record does not fit in this mod's memory budget.");
		}
		return NumResults;
	}

	// --- mod.has_permission ----------------------------------------------------------------------

	int LuaModHasPermission(lua_State* L)
	{
		size_t Length = 0;
		const char* const Bytes = CheckName(L, 1, Length);

		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, "mod.has_permission: this mod's context no longer exists.");
		}

		{
			const FString PermissionString = LuaToFString(Bytes, Length, MaxIdentifierChars);
			lua_pushboolean(L, Context->HasPermission(FName(*PermissionString)) ? 1 : 0);
		}

		return 1;
	}

	// --- mod.request_api and its handle ----------------------------------------------------------

	FModLuaApiHandle* CheckApiHandle(lua_State* L)
	{
		return static_cast<FModLuaApiHandle*>(luaL_checkudata(L, 1, ApiHandleMetatable));
	}

	/**
	 * The handle owns nothing - the API belongs to the game and is kept alive by the API registry -
	 * so releasing it is a reset, not a destruction. That also makes a handle whose __gc has already
	 * run (which lua_close can do in any order) read as "no longer valid" rather than as garbage.
	 */
	int LuaApiHandleGC(lua_State* L)
	{
		// lua_touserdata, not luaL_testudata: a __gc can only ever be reached through the metatable
		// that names it, so the type is already established, and looking the metatable back up by
		// name is one more thing that has to still work while the state is closing.
		if (lua_type(L, 1) != LUA_TUSERDATA)
		{
			return 0;
		}

		if (FModLuaApiHandle* const Handle = static_cast<FModLuaApiHandle*>(lua_touserdata(L, 1)))
		{
			Handle->Api.Reset();
		}
		return 0;
	}

	int LuaApiHandleValid(lua_State* L)
	{
		FModLuaApiHandle* const Handle = CheckApiHandle(L);
		lua_pushboolean(L, Handle->Api.IsValid() ? 1 : 0);
		return 1;
	}

	int LuaApiHandleId(lua_State* L)
	{
		FModLuaApiHandle* const Handle = CheckApiHandle(L);

		UModAPI* const Api = Handle->Api.Get();
		if (Api == nullptr)
		{
			lua_pushnil(L);
			return 1;
		}

		int NumResults = 0;
		{
			const FString IdString = Api->GetApiId().ToString();
			NumResults = PushText(L, IdString) ? 1 : -1;
		}

		if (NumResults < 0)
		{
			return luaL_error(L, "api:id(): the result could not be allocated within this mod's memory budget.");
		}
		return NumResults;
	}

	int LuaApiHandleVersion(lua_State* L)
	{
		FModLuaApiHandle* const Handle = CheckApiHandle(L);

		UModAPI* const Api = Handle->Api.Get();
		if (Api == nullptr)
		{
			lua_pushnil(L);
			return 1;
		}

		int NumResults = 0;
		{
			const FString VersionString = Api->GetApiVersion().ToString();
			NumResults = PushText(L, VersionString) ? 1 : -1;
		}

		if (NumResults < 0)
		{
			return luaL_error(L, "api:version(): the result could not be allocated within this mod's memory budget.");
		}
		return NumResults;
	}

	int LuaApiHandleToString(lua_State* L)
	{
		FModLuaApiHandle* const Handle = CheckApiHandle(L);

		bool bPushed = false;
		{
			UModAPI* const Api = Handle->Api.Get();
			const FString Text = (Api != nullptr)
				? FString::Printf(TEXT("mod api '%s' %s"), *Api->GetApiId().ToString(),
					*Api->GetApiVersion().ToString())
				: FString(TEXT("mod api (no longer available)"));
			bPushed = PushText(L, Text);
		}

		if (!bPushed)
		{
			return luaL_error(L, "api __tostring: the result could not be allocated within this mod's memory budget.");
		}
		return 1;
	}

	/**
	 * mod.request_api(id [, version_range]) -> handle, or nil + why.
	 *
	 * The requesting mod id is stamped by the context, so the permission gate in the API registry
	 * always runs against THIS mod - a script cannot ask for an API under another mod's name.
	 *
	 * The returned handle is deliberately inert: id(), version() and valid(), and nothing else. It
	 * is not a route into the API's methods, because calling an arbitrary UFUNCTION by name would
	 * mean binding the reflection system, which is exactly the unrestricted reach this sandbox
	 * exists to avoid. A game that wants scripts to drive an API exposes it as its own bound
	 * functions, deliberately, one at a time.
	 */
	int LuaModRequestApi(lua_State* L)
	{
		size_t IdLength = 0;
		const char* const IdBytes = CheckName(L, 1, IdLength);

		size_t RangeLength = 0;
		const char* const RangeBytes = luaL_optlstring(L, 2, "", &RangeLength);
		if (RangeLength > static_cast<size_t>(MaxVersionRangeChars))
		{
			return luaL_argerror(L, 2, "version range expression is longer than this framework accepts");
		}

		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, "mod.request_api: this mod's context no longer exists.");
		}

		if (lua_checkstack(L, 6) == 0)
		{
			return luaL_error(L, "mod.request_api: the Lua stack could not be grown.");
		}

		// Allocate and tag the handle FIRST, while nothing of ours is alive to be skipped by a
		// longjmp. Construction comes last so that a raise in between leaves uninitialised memory
		// with no finaliser attached rather than a finaliser pointed at uninitialised memory.
		void* const Memory = lua_newuserdatauv(L, sizeof(FModLuaApiHandle), 0);
		luaL_setmetatable(L, ApiHandleMetatable);
		FModLuaApiHandle* const Handle = new (Memory) FModLuaApiHandle();

		int NumResults = 0;
		{
			const FString ApiIdString = LuaToFString(IdBytes, IdLength, MaxIdentifierChars);
			const FString VersionRange = LuaToFString(RangeBytes, RangeLength, MaxVersionRangeChars);

			FModDiagnostic Error;

			// A null class skips the type check, which is the only sensible choice here: a script
			// cannot name a UClass and must not be given a way to.
			UModAPI* const Api = Context->RequestAPI(FName(*ApiIdString), nullptr, VersionRange, Error);
			if (Api != nullptr)
			{
				Handle->Api = Api;
				NumResults = 1;  // The handle is already on top of the stack.
			}
			else
			{
				lua_pop(L, 1);
				lua_pushnil(L);

				const FString Message = Error.Message.IsEmpty()
					? FString(TEXT("The API was refused."))
					: Error.Message;
				NumResults = PushText(L, Message) ? 2 : 1;
			}
		}

		return NumResults;
	}

	// --- mod.broadcast ---------------------------------------------------------------------------

	/**
	 * mod.broadcast(event_id) -> boolean
	 *
	 * NO PAYLOAD, ON PURPOSE. An FModEventPayload is a USTRUCT, and letting a script name one would
	 * mean handing it the reflection system to find the type and fill the fields - a general purpose
	 * "construct any struct by name" primitive is not something that can be made safe after the
	 * fact. A game that wants scripted events with data publishes an API for them.
	 *
	 * The source mod id is stamped by the context, so the bus's permission gate and every handler
	 * see the true origin. True means the broadcast was dispatched, not that anyone acted on it.
	 */
	int LuaModBroadcast(lua_State* L)
	{
		size_t Length = 0;
		const char* const Bytes = CheckName(L, 1, Length);

		UModContext* const Context = ResolveContext(L);
		if (Context == nullptr)
		{
			return PushFailure(L, "mod.broadcast: this mod's context no longer exists.");
		}

		{
			const FString EventIdString = LuaToFString(Bytes, Length, MaxIdentifierChars);
			const TInstancedStruct<FModEventPayload> EmptyPayload;
			lua_pushboolean(L, Context->BroadcastEvent(FName(*EventIdString), EmptyPayload) ? 1 : 0);
		}

		return 1;
	}

	// --- Calling back into Lua -------------------------------------------------------------------

	/**
	 * The count hook for a callback that arrives from the game rather than from inside a script.
	 *
	 * The runtime arms its own hook around every entry point it makes; when a broadcast reaches a
	 * handler from outside any script call there is no such entry point, and without this a
	 * subscriber containing `while true do end` would hang the game with no script frame to blame.
	 */
	void LuaCallbackHook(lua_State* L, lua_Debug* /*Debug*/)
	{
		FModLuaBindingState* const Bindings = FindBindings(L);
		if (Bindings == nullptr)
		{
			return;
		}

		Bindings->CallbackInstructions += Bindings->CallbackHookGranularity;

		if (Bindings->MaxCallbackInstructions <= 0)
		{
			return;
		}
		if (!Bindings->bCallbackBudgetHit
			&& Bindings->CallbackInstructions <= Bindings->MaxCallbackInstructions)
		{
			return;
		}

		Bindings->bCallbackBudgetHit = true;

		// Fire on the very next instruction from here on, so a handler that traps the error makes
		// essentially no progress before being aborted again.
		lua_sethook(L, &LuaCallbackHook, LUA_MASKCOUNT, 1);

		// A plain char buffer, not an FString: luaL_error longjmps out and nothing here would be
		// destroyed.
		ANSICHAR Message[192];
		FCStringAnsi::Snprintf(Message, UE_ARRAY_COUNT(Message),
			"instruction budget exhausted in an event handler (limit %lld per call); it was aborted",
			static_cast<long long>(Bindings->MaxCallbackInstructions));
		(void)luaL_error(L, "%s", Message);
	}

	/** Turns the error into a string with a traceback attached, exactly as the runtime's does. */
	int LuaCallbackMessageHandler(lua_State* L)
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

	/** Everything the protected callback needs, held in the dispatching frame that longjmp cannot reach. */
	struct FLuaCallbackInvocation
	{
		int CallbackRef = LUA_NOREF;
		const char* EventId = nullptr;
		size_t EventIdLength = 0;
		const char* SourceModId = nullptr;
		size_t SourceModIdLength = 0;
	};

	/**
	 * The body of one callback, run inside lua_pcall.
	 *
	 * Pushing the arguments allocates and so can raise; doing it here rather than in the caller is
	 * what keeps that error inside a protected call instead of reaching UE's event dispatch.
	 */
	int LuaInvokeCallback(lua_State* L)
	{
		const FLuaCallbackInvocation* const Call =
			static_cast<const FLuaCallbackInvocation*>(lua_touserdata(L, 1));
		if (Call == nullptr)
		{
			return 0;
		}

		if (lua_rawgeti(L, LUA_REGISTRYINDEX, Call->CallbackRef) != LUA_TFUNCTION)
		{
			// The reference was released between the broadcast and here. Not an error.
			return 0;
		}

		lua_pushlstring(L, Call->EventId, Call->EventIdLength);
		lua_pushlstring(L, Call->SourceModId, Call->SourceModIdLength);
		lua_call(L, 2, 0);
		return 0;
	}

	/**
	 * Runs one Lua handler for one broadcast.
	 *
	 * NO ERROR EVER LEAVES THIS FUNCTION. The bus is dispatching to every subscriber of an event
	 * when this runs; letting a mod's mistake propagate would take out the game's own handlers and
	 * any other mod's alongside it. Failures are logged against the owning mod and swallowed.
	 */
	void DispatchLuaEvent(UModLuaEventRelay& Relay, const FModEventContext& EventContext)
	{
		// Read the relay's fields ONCE, up front. A handler is free to call mod.unsubscribe on
		// itself, which drops the last strong reference to this very object.
		lua_State* const L = static_cast<lua_State*>(Relay.LuaState);
		const int CallbackRef = static_cast<int>(Relay.CallbackRef);

		if (L == nullptr || CallbackRef == LUA_NOREF || CallbackRef == LUA_REFNIL)
		{
			return;
		}

		FModLuaBindingState* const Bindings = FindBindings(L);
		if (Bindings == nullptr)
		{
			return;
		}

		if (Bindings->DispatchDepth >= MaxCallbackDepth)
		{
			UE_LOG(LogModLua, Warning,
				TEXT("A Lua event handler for '%s' was refused: handlers are already %d levels deep, ")
				TEXT("which means an event is re-entering its own handler."),
				*EventContext.EventId.ToString(), Bindings->DispatchDepth);
			return;
		}

		if (lua_checkstack(L, 8) == 0)
		{
			UE_LOG(LogModLua, Warning,
				TEXT("A Lua event handler for '%s' was skipped: the Lua stack could not be grown."),
				*EventContext.EventId.ToString());
			return;
		}

		const int BaseTop = lua_gettop(L);

		// Converted in this frame, which lua_pcall never unwinds past, and kept alive across the
		// call because LuaInvokeCallback reads through these pointers.
		const FTCHARToUTF8 EventIdUtf8(*EventContext.EventId.ToString());
		const FTCHARToUTF8 SourceModIdUtf8(*EventContext.SourceModId.ToString());

		FLuaCallbackInvocation Call;
		Call.CallbackRef = CallbackRef;
		Call.EventId = EventIdUtf8.Get();
		Call.EventIdLength = static_cast<size_t>(EventIdUtf8.Length());
		Call.SourceModId = SourceModIdUtf8.Get();
		Call.SourceModIdLength = static_cast<size_t>(SourceModIdUtf8.Length());

		// Only arm a hook when nothing else has: a broadcast raised from inside a script is already
		// running under the runtime's own budget, and overwriting that hook would reset the count
		// the runtime is keeping and hand the script an unmetered stretch of execution.
		const bool bArmHook = (lua_gethook(L) == nullptr) && Bindings->MaxCallbackInstructions > 0;
		if (bArmHook)
		{
			Bindings->CallbackInstructions = 0;
			Bindings->bCallbackBudgetHit = false;
			Bindings->CallbackHookGranularity = static_cast<int32>(FMath::Clamp<int64>(
				Bindings->MaxCallbackInstructions, 1, ModLuaLimits::HookGranularity));
			lua_sethook(L, &LuaCallbackHook, LUA_MASKCOUNT, Bindings->CallbackHookGranularity);
		}

		++Bindings->DispatchDepth;

		lua_pushcfunction(L, &LuaCallbackMessageHandler);
		const int HandlerIndex = lua_gettop(L);
		lua_pushcfunction(L, &LuaInvokeCallback);
		lua_pushlightuserdata(L, &Call);
		const int Status = lua_pcall(L, 1, 0, HandlerIndex);

		--Bindings->DispatchDepth;

		if (bArmHook)
		{
			// Never leave a hook armed between calls: lua_gc can run a __gc metamethod written in
			// Lua, and aborting one of those raises an error from inside the collector.
			lua_sethook(L, nullptr, 0, 0);
		}

		if (Status != LUA_OK)
		{
			FString Message;
			if (lua_type(L, -1) == LUA_TSTRING)
			{
				size_t Length = 0;
				const char* const Bytes = lua_tolstring(L, -1, &Length);
				Message = LuaToFString(Bytes, Length, ModLuaLimits::MaxLuaTextChars);
			}
			if (Message.IsEmpty())
			{
				Message = FString::Printf(TEXT("Lua reported error status %d with no message."), Status);
			}

			const FString Report = FString::Printf(
				TEXT("A script handler for event '%s' failed and was ignored: %s"),
				*EventContext.EventId.ToString(), *Message);

			if (UModContext* const Context = Bindings->Context.Get())
			{
				Context->LogError(Report);
			}
			else
			{
				UE_LOG(LogModLua, Error, TEXT("%s"), *Report);
			}
		}

		lua_settop(L, BaseTop);
	}

	// --- mod.subscribe / mod.unsubscribe ---------------------------------------------------------

	/**
	 * mod.subscribe(event_id, fn [, priority]) -> handle, or nil + why.
	 *
	 * The function is stored in the state's registry with luaL_ref, keyed by the handle the context
	 * issues. Every one of those references is returned by mod.unsubscribe and by the record's
	 * teardown - without that, a subscription leaks a reference and pins its closure, and every
	 * upvalue that closure captured, for the whole life of the state.
	 *
	 * The handler is called as fn(event_id, source_mod_id). Both are plain strings: the payload is
	 * not passed for the same reason mod.broadcast does not accept one.
	 */
	int LuaModSubscribe(lua_State* L)
	{
		size_t EventIdLength = 0;
		const char* const EventIdBytes = CheckName(L, 1, EventIdLength);

		luaL_checktype(L, 2, LUA_TFUNCTION);

		const lua_Integer RequestedPriority = luaL_optinteger(L, 3, 0);
		const int32 Priority = CheckInt32(L, 3, RequestedPriority);

		FModLuaBindingState* const Bindings = FindBindings(L);
		UModContext* const Context = ResolveContext(L);
		if (Bindings == nullptr || Context == nullptr)
		{
			return PushFailure(L, "mod.subscribe: this mod's context no longer exists.");
		}

		if (lua_checkstack(L, 4) == 0)
		{
			return luaL_error(L, "mod.subscribe: the Lua stack could not be grown.");
		}

		// luaL_ref pops the value it is given, so the handler is copied first. This can raise while
		// growing the registry, which is fine: nothing of ours is alive yet.
		lua_pushvalue(L, 2);
		const int CallbackRef = luaL_ref(L, LUA_REGISTRYINDEX);
		if (CallbackRef == LUA_NOREF || CallbackRef == LUA_REFNIL)
		{
			return PushFailure(L, "mod.subscribe: the handler could not be stored.");
		}

		bool bSubscribed = false;
		{
			const FString EventIdString = LuaToFString(EventIdBytes, EventIdLength, MaxIdentifierChars);

			// Outered to the transient package and held alive by the TStrongObjectPtr below: a
			// dynamic delegate stores its object weakly, so the bus does not keep this alive.
			UModLuaEventRelay* const Relay = NewObject<UModLuaEventRelay>();
			Relay->LuaState = L;
			Relay->CallbackRef = CallbackRef;

			FModEventDynamicDelegate Delegate;
			Delegate.BindUFunction(Relay, GET_FUNCTION_NAME_CHECKED(UModLuaEventRelay, HandleModEvent));

			// The context stamps this mod's id on the subscription, caps how many one mod may hold,
			// and cancels every one of them when the mod unloads.
			const FModEventHandle Handle =
				Context->SubscribeToEvent(FName(*EventIdString), Delegate, Priority);

			if (Handle.IsValid())
			{
				FModLuaSubscription& Subscription = Bindings->Subscriptions.Add(Handle.Id);
				Subscription.CallbackRef = CallbackRef;
				Subscription.Relay = TStrongObjectPtr<UModLuaEventRelay>(Relay);

				lua_pushinteger(L, static_cast<lua_Integer>(Handle.Id));
				bSubscribed = true;
			}
			else
			{
				// Blind the relay so a delegate the bus somehow kept can never reach this state.
				Relay->LuaState = nullptr;
				Relay->CallbackRef = LUA_NOREF;
			}
		}

		if (bSubscribed)
		{
			return 1;
		}

		luaL_unref(L, LUA_REGISTRYINDEX, CallbackRef);
		return PushFailure(L,
			"mod.subscribe: the subscription was refused - the event id may be empty, or this mod may "
			"already hold the maximum number of subscriptions.");
	}

	/** mod.unsubscribe(handle) -> boolean. A handle this context did not issue is simply false. */
	int LuaModUnsubscribe(lua_State* L)
	{
		const lua_Integer RawHandle = luaL_checkinteger(L, 1);

		FModLuaBindingState* const Bindings = FindBindings(L);
		UModContext* const Context = ResolveContext(L);
		if (Bindings == nullptr || Context == nullptr)
		{
			return PushFailure(L, "mod.unsubscribe: this mod's context no longer exists.");
		}

		int CallbackRef = LUA_NOREF;
		bool bCancelled = false;
		{
			FModLuaSubscription Subscription;
			if (!Bindings->Subscriptions.RemoveAndCopyValue(static_cast<int64>(RawHandle), Subscription))
			{
				// Not this mod's handle. The context would refuse it anyway; refusing here as well
				// means a guessed handle never even reaches the bus.
				lua_pushboolean(L, 0);
				return 1;
			}

			CallbackRef = Subscription.CallbackRef;

			FModEventHandle Handle;
			Handle.Id = static_cast<int64>(RawHandle);
			bCancelled = Context->UnsubscribeFromEvent(Handle);

			if (UModLuaEventRelay* const Relay = Subscription.Relay.Get())
			{
				Relay->LuaState = nullptr;
				Relay->CallbackRef = LUA_NOREF;
			}
		}

		// Only once the record above is gone - it owns a strong object pointer - is it safe to touch
		// Lua again, even though luaL_unref does not allocate and so cannot raise.
		luaL_unref(L, LUA_REGISTRYINDEX, CallbackRef);

		lua_pushboolean(L, bCancelled ? 1 : 0);
		return 1;
	}

	// --- Teardown --------------------------------------------------------------------------------

	/**
	 * The binding record's finaliser. Runs from lua_close, on the game thread, before Lua frees
	 * anything.
	 */
	int LuaBindingsGC(lua_State* L)
	{
		// lua_touserdata rather than luaL_testudata, for the reason given in LuaApiHandleGC: this is
		// the one release path that MUST work during lua_close, so it depends on as little as
		// possible still being reachable.
		if (lua_type(L, 1) != LUA_TUSERDATA)
		{
			return 0;
		}

		FModLuaBindingState* const Bindings = static_cast<FModLuaBindingState*>(lua_touserdata(L, 1));
		if (Bindings == nullptr)
		{
			return 0;
		}

		// Make the record UNREACHABLE BEFORE DESTROYING IT. A script may install its own __gc
		// metamethods, and one of those can still call a mod.* function while the state is closing;
		// after this line every binding sees "context no longer exists" instead of freed memory.
		lua_pushnil(L);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &BindingStateKey);

		ReleaseSubscriptions(L, *Bindings);
		Bindings->~FModLuaBindingState();
		return 0;
	}

	// --- Installation ----------------------------------------------------------------------------

	/** THE COMPLETE LIST OF WHAT A SCRIPT CAN DO. Read the file header before adding to it. */
	const luaL_Reg ModFunctions[] =
	{
		{ "id",             &LuaModId },
		{ "log",            &LuaModLog },
		{ "warn",           &LuaModWarn },
		{ "error",          &LuaModError },
		{ "config_get",     &LuaModConfigGet },
		{ "config_set",     &LuaModConfigSet },
		{ "config_save",    &LuaModConfigSave },
		{ "save",           &LuaModSave },
		{ "load",           &LuaModLoad },
		{ "has_permission", &LuaModHasPermission },
		{ "request_api",    &LuaModRequestApi },
		{ "broadcast",      &LuaModBroadcast },
		{ "subscribe",      &LuaModSubscribe },
		{ "unsubscribe",    &LuaModUnsubscribe },
		{ nullptr,          nullptr },
	};

	/** Methods on an api handle. Three readers and nothing else - see LuaModRequestApi. */
	const luaL_Reg ApiHandleMethods[] =
	{
		{ "id",      &LuaApiHandleId },
		{ "version", &LuaApiHandleVersion },
		{ "valid",   &LuaApiHandleValid },
		{ nullptr,   nullptr },
	};
}

bool ModLuaBindings::InstallModContext(lua_State* L, UModContext* Context, FModDiagnostic& OutError)
{
	if (L == nullptr)
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Lua.BindingsFailed")),
			TEXT("The mod API cannot be installed into a null Lua state."));
		return false;
	}

	if (Context == nullptr)
	{
		OutError = FModDiagnostic::Error(FName(TEXT("Lua.InvalidContext")),
			TEXT("The mod API cannot be installed without a mod context: the context is the only ")
			TEXT("thing a script is ever given."));
		return false;
	}

	// Raises when the stack cannot be grown, which the runtime's lua_pcall catches. Nothing of ours
	// is alive at this point, so the longjmp costs nothing.
	luaL_checkstack(L, 12, "installing the mod API");

	// 1. The record's metatable. __gc is what guarantees every registry reference and every rooted
	//    relay is released when the state closes, so it is set before any record exists.
	if (luaL_newmetatable(L, BindingStateMetatable) != 0)
	{
		lua_pushcfunction(L, &LuaBindingsGC);
		lua_setfield(L, -2, "__gc");

		lua_pushstring(L, "ModFramework bindings");
		lua_setfield(L, -2, "__name");

		// getmetatable returns false and setmetatable refuses. Not that a sandboxed script can reach
		// this userdata at all - it is only ever in the registry - but a metatable a script cannot
		// rewrite is one less thing to reason about.
		lua_pushboolean(L, 0);
		lua_setfield(L, -2, "__metatable");
	}
	lua_pop(L, 1);

	// 2. The api handle's metatable.
	if (luaL_newmetatable(L, ApiHandleMetatable) != 0)
	{
		lua_pushcfunction(L, &LuaApiHandleGC);
		lua_setfield(L, -2, "__gc");

		lua_pushcfunction(L, &LuaApiHandleToString);
		lua_setfield(L, -2, "__tostring");

		lua_pushstring(L, "mod api");
		lua_setfield(L, -2, "__name");

		luaL_setfuncs(L, ApiHandleMethods, 0);

		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");

		lua_pushboolean(L, 0);
		lua_setfield(L, -2, "__metatable");
	}
	lua_pop(L, 1);

	// 3. The record itself. Tagged with its metatable before it is constructed, so that a raise in
	//    between leaves memory with no finaliser rather than a finaliser over raw memory.
	void* const Memory = lua_newuserdatauv(L, sizeof(FModLuaBindingState), 0);
	luaL_setmetatable(L, BindingStateMetatable);

	FModLuaBindingState* const Bindings = new (Memory) FModLuaBindingState();

	// WEAK. See rule B in the file header: a script can outlive its context.
	Bindings->Context = Context;
	Bindings->MaxCallbackInstructions = ModLuaLimits::DefaultInstructionsPerCall;

	// Anchored under a private address, where no script and no Lua value can name it. This consumes
	// the userdata; the registry is now the only thing holding it, which is exactly right - the
	// record's lifetime is the state's.
	lua_rawsetp(L, LUA_REGISTRYINDEX, &BindingStateKey);

	// 4. The `mod` table.
	luaL_newlibtable(L, ModFunctions);
	luaL_setfuncs(L, ModFunctions, 0);
	lua_setglobal(L, "mod");

	return true;
}

#endif // MODFRAMEWORK_WITH_LUA

// -------------------------------------------------------------------------------------------------
// Defined unconditionally: the class is declared unconditionally so that UnrealHeaderTool, which
// does not evaluate MODFRAMEWORK_WITH_LUA, generates matching reflection code in either build.
// Without an interpreter there is nothing to dispatch to and the relay is never constructed.
// -------------------------------------------------------------------------------------------------
void UModLuaEventRelay::HandleModEvent(const FModEventContext& EventContext)
{
#if MODFRAMEWORK_WITH_LUA
	DispatchLuaEvent(*this, EventContext);
#else
	(void)EventContext;
#endif
}
