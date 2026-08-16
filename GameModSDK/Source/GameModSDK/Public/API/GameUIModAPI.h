// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "API/ModAPI.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Internationalization/Text.h"
#include "Manifest/ModVersion.h"
#include "Templates/SubclassOf.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include "GameUIModAPI.generated.h"

class UUserWidget;

/**
 * The HUD surface a mod is allowed to touch: adding widgets and showing notifications.
 *
 * ---------------------------------------------------------------------------------------------
 * THIS CLASS IS A DECLARATION, NOT AN IMPLEMENTATION
 * ---------------------------------------------------------------------------------------------
 * Every method below is a virtual with a deliberately inert body: it logs that no game
 * implementation exists and returns the neutral value (nullptr / false / nothing). Nothing here
 * reaches into the game, because the SDK is compiled and shipped WITHOUT the game's own module - a
 * mod author installs this plugin alone and still has to be able to build against the full surface.
 *
 * The game supplies the behaviour by subclassing this in its own module, overriding the methods it
 * wants to honour, and registering one instance:
 *
 *     UModSubsystem::RegisterGameAPIByClass(UMyUIAPI::StaticClass());
 *
 * A mod then asks for it by id and gets the game's subclass back:
 *
 *     FModDiagnostic Error;
 *     UGameUIModAPI* UI = Cast<UGameUIModAPI>(
 *         Context->RequestAPI(TEXT("game.ui"), UGameUIModAPI::StaticClass(), TEXT("^1.0.0"), Error));
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
 * declared its id only in metadata would register as "game.ui" in the editor and under a class-name
 * derived id in a Shipping build. Keep the two spellings in agreement.
 *
 * ---------------------------------------------------------------------------------------------
 * SERVER AUTHORITY
 * ---------------------------------------------------------------------------------------------
 * This API is deliberately NOT server-authoritative: the HUD is a purely local concern and a
 * client-side UI mod is exactly the kind of mod that should keep working in multiplayer. No method
 * here calls VerifyServerAuthority, and none of them may ever change replicated state. A game
 * override that needs to touch gameplay from a widget must route that through UGameCombatModAPI or
 * UGameWorldModAPI, which are guarded.
 *
 * ---------------------------------------------------------------------------------------------
 * UNTRUSTED INPUT
 * ---------------------------------------------------------------------------------------------
 * Everything a mod passes in is untrusted. An override must reject a null or abstract WidgetClass,
 * a widget it did not create, a non-finite Duration and an over-long notification text, and should
 * cap how many widgets a single mod may hold on the HUD at once - an unbounded AddWidgetToHUD loop
 * is the easiest way for one mod to make a game unplayable.
 */
UCLASS(BlueprintType, meta = (
	ModPublic,
	ModApiId = "game.ui",
	ModApiVersion = "1.0.0",
	ModApiPermissions = "ui.modify",
	ModApiServerAuthoritative = "false"))
class GAMEMODSDK_API UGameUIModAPI : public UModAPI
{
	GENERATED_BODY()

public:
	/** "game.ui" - the id a mod passes to UModContext::RequestAPI. */
	static FName GetStaticApiId();

	/** 1.0.0 - the version a mod's range expression is matched against. */
	static FModVersion GetStaticApiVersion();

	/** "ui.modify" - the single permission a mod must hold to obtain this API. */
	static FName GetStaticRequiredPermission();

	// --- HUD widgets -----------------------------------------------------------------------------

	/**
	 * Creates one instance of WidgetClass and puts it on the player's HUD.
	 *
	 * The returned widget is owned by the game, not by the mod: the game is expected to take it down
	 * when the owning mod deactivates, so a mod that forgets to call RemoveWidgetFromHUD cannot leave
	 * orphaned UI behind. Hold the result weakly.
	 *
	 * The default implementation does nothing and returns nullptr.
	 *
	 * @param WidgetClass  Widget to create. Untrusted: may be null, abstract or stale.
	 * @param ZOrder       Draw order within the HUD; higher draws on top. Untrusted, so an override
	 *                     should clamp it into whatever band it reserves for mods rather than letting
	 *                     a mod paint over the game's own critical UI.
	 * @return The new widget, or nullptr when the game refused to create it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|UI", meta = (ModPublic))
	virtual UUserWidget* AddWidgetToHUD(TSubclassOf<UUserWidget> WidgetClass, int32 ZOrder);

	/**
	 * Takes a widget off the HUD and releases it.
	 *
	 * An override must verify that Widget is one it handed out through AddWidgetToHUD before removing
	 * it: a mod can pass any widget it can reach, including the game's own HUD.
	 *
	 * The default implementation does nothing and returns false.
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|UI", meta = (ModPublic))
	virtual bool RemoveWidgetFromHUD(UUserWidget* Widget);

	// --- Notifications ---------------------------------------------------------------------------

	/**
	 * Shows a short transient message using the game's own notification style.
	 *
	 * There is no return value on purpose: a notification is advisory, and a mod must not be able to
	 * tell whether the game decided to suppress, queue or coalesce it.
	 *
	 * The default implementation does nothing.
	 *
	 * @param Message   Text to show. Untrusted: an override should cap its length and must never
	 *                  interpret it as markup or a format string.
	 * @param Duration  Seconds to keep it up. Untrusted: clamp it, and treat a non-finite or
	 *                  non-positive value as "use the game's default".
	 */
	UFUNCTION(BlueprintCallable, Category = "Game|UI", meta = (ModPublic))
	virtual void ShowNotification(const FText& Message, float Duration);

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
