// Copyright (c) 2026. Licensed for use in your own projects.

#pragma once

#include "CoreMinimal.h"
#include "API/GameUIModAPI.h"
#include "SampleUIModAPI.generated.h"

class UUserWidget;

/**
 * The sample game's implementation of the UI modding API.
 *
 * This is the one API in the SDK that is NOT server-authoritative: HUD content is a purely local
 * presentation concern, so a client adding a widget is legitimate.
 *
 * Widgets created here are tracked so they can be torn down when the API is unregistered. Without
 * that, unloading a mod would leave its HUD elements on screen with no owner - a leak the player
 * can see.
 */
UCLASS(NotBlueprintable)
class USampleUIModAPI : public UGameUIModAPI
{
	GENERATED_BODY()

public:
	//~ Begin UGameUIModAPI
	virtual UUserWidget* AddWidgetToHUD(TSubclassOf<UUserWidget> WidgetClass, int32 ZOrder) override;
	virtual bool RemoveWidgetFromHUD(UUserWidget* Widget) override;
	virtual void ShowNotification(const FText& Message, float Duration) override;
	//~ End UGameUIModAPI

	//~ Begin UModAPI
	virtual void OnAPIUnregistered() override;
	//~ End UModAPI

private:
	/** Widgets this API created, so they can be removed if the API goes away. */
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UUserWidget>> ModWidgets;

	/** A mod that adds widgets in a loop should not be able to bury the game's own HUD. */
	static constexpr int32 MaxModWidgets = 64;
};
