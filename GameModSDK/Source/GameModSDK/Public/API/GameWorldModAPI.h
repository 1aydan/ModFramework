// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "API/ModAPI.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Manifest/ModVersion.h"
#include "Math/Transform.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "GameWorldModAPI.generated.h"

class AActor;

/**
 * The world surface a mod is allowed to touch: spawning enemies and reading wave progress.
 *
 * ---------------------------------------------------------------------------------------------
 * THIS CLASS IS A DECLARATION, NOT AN IMPLEMENTATION
 * ---------------------------------------------------------------------------------------------
 * Every method below is a virtual with a deliberately inert body: it logs that no game
 * implementation exists and returns the neutral value (nullptr / 0 / false). Nothing here reaches
 * into the game, because the SDK is compiled and shipped WITHOUT the game's own module - a mod
 * author installs this plugin alone and still has to be able to build against the full surface.
 *
 * The game supplies the behaviour by subclassing this in its own module, overriding the methods it
 * wants to honour, and registering one instance:
 *
 *     UModSubsystem::RegisterGameAPIByClass(UMyWorldAPI::StaticClass());
 *
 * A mod then asks for it by id and gets the game's subclass back:
 *
 *     FModDiagnostic Error;
 *     UGameWorldModAPI* World = Cast<UGameWorldModAPI>(
 *         Context->RequestAPI(TEXT("game.world"), UGameWorldModAPI::StaticClass(), TEXT("^1.0.0"), Error));
 *
 * If the game never registers a subclass, a mod that obtains this base class still runs: it simply
 * observes that every call is refused. That is the whole point of the inert defaults - a missing
 * game implementation must never crash a mod.
 *
 * ---------------------------------------------------------------------------------------------
 * IDENTITY
 * ---------------------------------------------------------------------------------------------
 * The id, version, permissions and authority flag are declared TWICE on purpose: as UCLASS
 * metadata, which SDK generation and editor tooling read, and as the Native* hooks from UModAPI,
 * which are compiled in and survive cooking. UCLASS metadata is editor-only data, so an API that
 * declared its id only in metadata would register as "game.world" in the editor and under a
 * class-name derived id in a Shipping build. Keep the two spellings in agreement.
 *
 * ---------------------------------------------------------------------------------------------
 * SERVER AUTHORITY
 * ---------------------------------------------------------------------------------------------
 * This API is server-authoritative - spawning and despawning actors on a client would desynchronise
 * the session immediately. Every entry point, reads included, runs UModAPI::VerifyServerAuthority
 * first and bails out on a network client. A game that wants a cheap replicated read on clients can
 * override the getter and not call the guard; the guard belongs to the implementation.
 *
 * ---------------------------------------------------------------------------------------------
 * UNTRUSTED INPUT
 * ---------------------------------------------------------------------------------------------
 * Everything a mod passes in is untrusted. An override must reject an unknown EnemyId, a transform
 * carrying NaN or an absurd scale, and an actor it does not own, with a return value rather than an
 * assertion. A game should also rate-limit SpawnEnemy: an unbounded spawn loop in a mod is the
 * easiest way for one bad mod to take a server down.
 */
UCLASS(BlueprintType, meta = (
	ModPublic,
	ModApiId = "game.world",
	ModApiVersion = "1.0.0",
	ModApiPermissions = "gameplay.modify",
	ModApiServerAuthoritative = "true"))
class GAMEMODSDK_API UGameWorldModAPI : public UModAPI
{
	GENERATED_BODY()

public:
	/** "game.world" - the id a mod passes to UModContext::RequestAPI. */
	static FName GetStaticApiId();

	/** 1.0.0 - the version a mod's range expression is matched against. */
	static FModVersion GetStaticApiVersion();

	/** "gameplay.modify" - the single permission a mod must hold to obtain this API. */
	static FName GetStaticRequiredPermission();

	// --- Spawning --------------------------------------------------------------------------------

	/**
	 * Spawns the enemy the game files under EnemyId and returns the new actor.
	 *
	 * EnemyId names a UGameEnemyDefinition - either one of the game's own or one a mod shipped - not
	 * a class path, so a mod can never use this to instantiate an arbitrary UClass. An override must
	 * treat an unknown id as a refusal and return nullptr.
	 *
	 * The default implementation does nothing and returns nullptr.
	 *
	 * @param EnemyId         Primary asset name of a UGameEnemyDefinition. Untrusted.
	 * @param SpawnTransform  Where to place it. Untrusted: validate it is finite before use.
	 * @return The spawned actor, or nullptr when the game refused the spawn.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|World", meta = (ModPublic))
	virtual AActor* SpawnEnemy(FName EnemyId, const FTransform& SpawnTransform);

	/**
	 * Removes an enemy the game is tracking.
	 *
	 * An override must verify that Enemy really is one of its own spawned enemies before destroying
	 * it: a mod can pass any actor it can reach, including the player's pawn.
	 *
	 * The default implementation does nothing and returns false.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|World", meta = (ModPublic))
	virtual bool DespawnEnemy(AActor* Enemy);

	// --- Progress --------------------------------------------------------------------------------

	/**
	 * How many enemies are alive right now. The default implementation does nothing and returns 0.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|World", meta = (ModPublic))
	virtual int32 GetActiveEnemyCount() const;

	/**
	 * The wave currently running, using the same numbering as FGameWaveStartedEvent::WaveIndex.
	 *
	 * The default implementation does nothing and returns 0. A game with no wave concept should keep
	 * returning 0 rather than inventing a number.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|World", meta = (ModPublic))
	virtual int32 GetCurrentWave() const;

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
