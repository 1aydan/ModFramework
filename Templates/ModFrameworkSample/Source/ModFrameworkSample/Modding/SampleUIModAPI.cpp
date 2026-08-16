// Copyright (c) 2026. Licensed for use in your own projects.

#include "SampleUIModAPI.h"

#include "Blueprint/UserWidget.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogSampleUIModding, Log, All);

UUserWidget* USampleUIModAPI::AddWidgetToHUD(TSubclassOf<UUserWidget> WidgetClass, int32 ZOrder)
{
	if (WidgetClass == nullptr)
	{
		return nullptr;
	}

	// Prune dead entries before enforcing the cap, so widgets a mod already removed do not count
	// against it.
	ModWidgets.RemoveAll([](const TWeakObjectPtr<UUserWidget>& Entry)
	{
		return !Entry.IsValid();
	});

	if (ModWidgets.Num() >= MaxModWidgets)
	{
		UE_LOG(LogSampleUIModding, Warning,
			TEXT("AddWidgetToHUD refused: %d mod widgets are already on screen (cap %d)."),
			ModWidgets.Num(), MaxModWidgets);
		return nullptr;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	APlayerController* Controller = UGameplayStatics::GetPlayerController(World, 0);
	if (Controller == nullptr)
	{
		return nullptr;
	}

	UUserWidget* Widget = CreateWidget<UUserWidget>(Controller, WidgetClass);
	if (Widget == nullptr)
	{
		return nullptr;
	}

	Widget->AddToViewport(ZOrder);
	ModWidgets.Add(Widget);
	return Widget;
}

bool USampleUIModAPI::RemoveWidgetFromHUD(UUserWidget* Widget)
{
	if (!IsValid(Widget))
	{
		return false;
	}

	// Only remove widgets this API created. Otherwise a mod could tear down the game's own HUD by
	// passing any widget it can reach.
	const int32 Index = ModWidgets.IndexOfByPredicate([Widget](const TWeakObjectPtr<UUserWidget>& Entry)
	{
		return Entry.Get() == Widget;
	});

	if (Index == INDEX_NONE)
	{
		UE_LOG(LogSampleUIModding, Verbose,
			TEXT("RemoveWidgetFromHUD refused: '%s' was not created through this API."),
			*Widget->GetName());
		return false;
	}

	Widget->RemoveFromParent();
	ModWidgets.RemoveAt(Index);
	return true;
}

void USampleUIModAPI::ShowNotification(const FText& Message, float Duration)
{
	// This sample has no notification system. Logging is the honest implementation: a mod author
	// sees their call worked and where it went, rather than it silently doing nothing. A real game
	// would route this into its own toast/message widget.
	UE_LOG(LogSampleUIModding, Log, TEXT("[mod notification, %.1fs] %s"),
		Duration, *Message.ToString());
}

void USampleUIModAPI::OnAPIUnregistered()
{
	for (const TWeakObjectPtr<UUserWidget>& Entry : ModWidgets)
	{
		if (UUserWidget* Widget = Entry.Get())
		{
			Widget->RemoveFromParent();
		}
	}

	ModWidgets.Reset();

	Super::OnAPIUnregistered();
}
