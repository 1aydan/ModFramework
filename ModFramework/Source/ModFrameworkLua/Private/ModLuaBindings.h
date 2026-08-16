// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "Core/ModFrameworkTypes.h"
#include "CoreTypes.h"
#include "Events/ModEventTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "ModLuaBindings.generated.h"

class UModContext;

/**
 * One Lua subscriber, seen from the C++ side of the event bus.
 *
 * WHY A UOBJECT EXISTS AT ALL IN A FILE THAT IS OTHERWISE PURE C API GLUE: UModContext's only
 * subscription entry point takes an FModEventDynamicDelegate, and a dynamic delegate can be bound
 * to exactly one thing - a UFUNCTION on a UObject. There is no non-UObject route, and reaching past
 * the context to the event bus's native delegate would step outside the one class this sandbox is
 * allowed to see. So each Lua subscription gets one of these: the smallest possible object whose
 * only job is to turn a bus callback back into a call into the right Lua function.
 *
 * ONE RELAY PER SUBSCRIPTION, NOT ONE PER STATE. A dynamic delegate is identified by (object,
 * function name), so several subscriptions sharing one relay would be indistinguishable when the
 * callback arrives and there would be no way to know which Lua function to invoke.
 *
 * The relay holds no strong reference to anything and no ownership. LuaState is deliberately void*:
 * Lua's headers live under Private/ThirdParty and must not leak into a header, and nothing here
 * needs to know what a lua_State looks like. It is cleared - together with CallbackRef - the moment
 * the subscription is cancelled or the owning state is closed, which is what makes a callback that
 * arrives late a no-op instead of a call into freed memory.
 *
 * Declared unconditionally, outside MODFRAMEWORK_WITH_LUA, on purpose: UnrealHeaderTool does not
 * evaluate that macro, so a class hidden behind it would still have reflection code generated for
 * it and a build without the Lua sources would fail to link the class it never compiled. The
 * dispatch body is what is conditional, not the declaration.
 */
UCLASS()
class UModLuaEventRelay : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Bound to the event bus through UModContext::SubscribeToEvent.
	 *
	 * Runs the Lua handler inside a protected call with a traceback handler and an instruction
	 * budget, so neither an error nor a runaway loop in a mod's script can escape into UE's event
	 * dispatch. A relay whose state or reference has been cleared simply does nothing.
	 */
	UFUNCTION()
	void HandleModEvent(const FModEventContext& EventContext);

	/**
	 * The lua_State to call into, as void*, or null once that state has gone away. Never
	 * dereferenced without being tested first.
	 */
	void* LuaState = nullptr;

	/**
	 * luaL_ref index of the Lua function in that state's registry, or LUA_NOREF (-2) when there is
	 * none. Spelled as a literal so this header needs nothing from lauxlib.h.
	 */
	int32 CallbackRef = -2;
};

#if MODFRAMEWORK_WITH_LUA

/**
 * Lua's own headers live under Private/ThirdParty and are on this module's private include path
 * only, so they must not be pulled into a header that other files include casually. Naming the
 * state by its C struct tag is enough: in C++ `struct lua_State` and lua.h's
 * `typedef struct lua_State lua_State` denote the same type, so this declaration and that typedef
 * coexist in either include order.
 */
struct lua_State;

/**
 * The binding layer: everything a script is allowed to reach.
 *
 * SPLIT FROM THE RUNTIME ON PURPOSE. FModLuaRuntime owns the *shape* of the sandbox - which
 * standard libraries exist, what the budgets are, how errors come back - and knows nothing about
 * the framework's mod-facing API. This namespace owns the opposite half: it is the single place
 * where UModContext's surface becomes callable from Lua. Auditing "what can a mod do?" therefore
 * means reading one file, which is the whole point of UModContext being the only bound object.
 */
namespace ModLuaBindings
{
	/**
	 * Installs the `mod` table into L's globals, wrapping Context.
	 *
	 * Called by FModLuaRuntime immediately after the sandbox is built and before any mod script is
	 * loaded, so a script sees a complete environment or none at all. The call is made from inside a
	 * lua_pcall, so raising a Lua error here is caught rather than escaping into UE - but the C++
	 * frame is left by longjmp, so do not hold a non-trivially-destructible local (FString, TArray,
	 * FModDiagnostic) across anything that can raise.
	 *
	 * Context is guaranteed non-null and its state guaranteed freshly sandboxed. Return false with
	 * OutError filled rather than raising when the failure is a C++-side one; the runtime turns a
	 * false return into a failed CreateContext and destroys the state.
	 *
	 * IMPLEMENTED ELSEWHERE - see ModLuaBindings.cpp. Deliberately only declared here.
	 */
	bool InstallModContext(lua_State* L, UModContext* Context, FModDiagnostic& OutError);
}

#endif // MODFRAMEWORK_WITH_LUA
