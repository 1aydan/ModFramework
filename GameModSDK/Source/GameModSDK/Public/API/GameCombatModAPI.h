// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "API/ModAPI.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Manifest/ModVersion.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "GameCombatModAPI.generated.h"

class AActor;
class APawn;

/**
 * The combat surface a mod is allowed to touch: damage, health and the player's pawn.
 *
 * ---------------------------------------------------------------------------------------------
 * THIS CLASS IS A DECLARATION, NOT AN IMPLEMENTATION
 * ---------------------------------------------------------------------------------------------
 * Every method below is a virtual with a deliberately inert body: it logs that no game
 * implementation exists and returns the neutral value (false / 0 / nullptr). Nothing here reaches
 * into the game, because the SDK is compiled and shipped WITHOUT the game's own module - a mod
 * author installs this plugin alone and still has to be able to build against the full surface.
 *
 * The game supplies the behaviour by subclassing this in its own module, overriding the methods it
 * wants to honour, and registering one instance:
 *
 *     UModSubsystem::RegisterGameAPIByClass(UMyCombatAPI::StaticClass());
 *
 * A mod then asks for it by id and gets the game's subclass back:
 *
 *     FModDiagnostic Error;
 *     UGameCombatModAPI* Combat = Cast<UGameCombatModAPI>(
 *         Context->RequestAPI(TEXT("game.combat"), UGameCombatModAPI::StaticClass(), TEXT("^1.0.0"), Error));
 *
 * If the game never registers a subclass, a mod that obtains this base class still runs: it simply
 * observes that every call is refused. That is the whole point of the inert defaults - a missing
 * game implementation must never crash a mod.
 *
 * ---------------------------------------------------------------------------------------------
 * IDENTITY
 * ---------------------------------------------------------------------------------------------
 * The id, version, permissions and authority flag are declared TWICE on purpose:
 *
 *   - as UCLASS metadata, which is what SDK generation and the editor tooling read;
 *   - as the Native* hooks from UModAPI, which are compiled in and survive cooking.
 *
 * UCLASS metadata is editor-only data (WITH_METADATA defaults to WITH_EDITORONLY_DATA), so an API
 * that declared its id only in metadata would register as "game.combat" in the editor and under a
 * class-name derived id in a Shipping build. The Native* hooks are step 1 of UModAPI's resolution
 * chain and cannot be stripped, so they are what actually decides the identity at runtime. Keep the
 * two spellings in agreement; the SDK generator warns when they drift.
 *
 * ---------------------------------------------------------------------------------------------
 * SERVER AUTHORITY
 * ---------------------------------------------------------------------------------------------
 * This API is server-authoritative. Every entry point - reads included - runs
 * UModAPI::VerifyServerAuthority first and bails out on a network client, because the SDK cannot
 * know whether a given override touches authoritative state. A game that wants a cheap replicated
 * read on clients can override the getter and simply not call the guard; the guard belongs to the
 * implementation, not to the declaration.
 *
 * ---------------------------------------------------------------------------------------------
 * UNTRUSTED INPUT
 * ---------------------------------------------------------------------------------------------
 * Everything a mod passes in is untrusted. An override must validate its own arguments - null
 * actors, NaN or negative damage, out-of-range player indices - and refuse them with a return
 * value. Nothing on this API may ever check() or ensure() on a mod-supplied value.
 */
UCLASS(BlueprintType, meta = (
	ModPublic,
	ModApiId = "game.combat",
	ModApiVersion = "1.0.0",
	ModApiPermissions = "gameplay.modify",
	ModApiServerAuthoritative = "true"))
class GAMEMODSDK_API UGameCombatModAPI : public UModAPI
{
	GENERATED_BODY()

public:
	/** "game.combat" - the id a mod passes to UModContext::RequestAPI. */
	static FName GetStaticApiId();

	/** 1.0.0 - the version a mod's range expression is matched against. */
	static FModVersion GetStaticApiVersion();

	/** "gameplay.modify" - the single permission a mod must hold to obtain this API. */
	static FName GetStaticRequiredPermission();

	// --- Damage ----------------------------------------------------------------------------------

	/**
	 * Applies damage to Target through the game's own damage pipeline.
	 *
	 * The default implementation does nothing and returns false. An override must reject a null or
	 * pending-kill Target, a non-finite or negative Amount, and any DamageType it does not know,
	 * rather than trusting the caller.
	 *
	 * @param Target      Actor to damage. May be anything a mod could get hold of, including null.
	 * @param Amount      Damage in the game's own units. Untrusted; clamp it.
	 * @param DamageType  Game-defined damage category, or NAME_None for the game's default.
	 * @return true only when the game actually applied damage.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|Combat", meta = (ModPublic))
	virtual bool ApplyDamage(AActor* Target, float Amount, FName DamageType);

	// --- Health ----------------------------------------------------------------------------------

	/**
	 * Current health of Target.
	 *
	 * The default implementation does nothing and returns 0. A return of 0 therefore means either
	 * "dead" or "the game has no implementation" - callers that need to tell those apart should test
	 * IsAlive as well.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|Combat", meta = (ModPublic))
	virtual float GetHealth(AActor* Target) const;

	/** Maximum health of Target. The default implementation does nothing and returns 0. */
	UFUNCTION(BlueprintCallable, Category = "Game|Combat", meta = (ModPublic))
	virtual float GetMaxHealth(AActor* Target) const;

	/**
	 * Whether Target is alive and can still be damaged.
	 *
	 * The default implementation does nothing and returns false, so a mod written against a game
	 * that has not implemented combat sees an unchanging "nothing is alive" world rather than acting
	 * on made-up state.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|Combat", meta = (ModPublic))
	virtual bool IsAlive(AActor* Target) const;

	// --- Players ---------------------------------------------------------------------------------

	/**
	 * The pawn currently possessed by the player at PlayerIndex, or nullptr.
	 *
	 * PlayerIndex is untrusted: an override must treat a negative or out-of-range index as "no such
	 * player" and return nullptr instead of indexing a player array with it. Index 0 is the first
	 * local player. The default implementation does nothing and returns nullptr.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|Combat", meta = (ModPublic))
	virtual APawn* GetPlayerPawn(int32 PlayerIndex) const;

	//~ Begin UModAPI interface
	// Metadata-independent identity. These are what the API is actually registered under in a cooked
	// build; the UCLASS metadata above only drives editor tooling and SDK generation.
	virtual FName NativeGetApiId() const override;
	virtual FModVersion NativeGetApiVersion() const override;
	virtual FString NativeGetApiDescription() const override;
	virtual bool NativeGetServerAuthoritative(bool& bOutServerAuthoritative) const override;
	virtual TArray<FName> NativeGetRequiredPermissions() const override;
	//~ End UModAPI interface
};
