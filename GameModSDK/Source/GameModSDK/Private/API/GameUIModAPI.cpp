// Copyright (c) 2026. Licensed for use in your own projects.

#include "API/GameUIModAPI.h"

#include "API/ModAPI.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "GameModSDKModule.h"
#include "Internationalization/Text.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModVersion.h"
#include "Templates/SubclassOf.h"
#include "UObject/NameTypes.h"

namespace
{
	/** The id this API registers under. Built once, on first use, so no static init order applies. */
	const TCHAR* const GameUIApiIdString = TEXT("game.ui");

	/** The permission a mod must hold before UModAPIRegistry::RequestAPI hands this API over. */
	const TCHAR* const GameUIPermissionString = TEXT("ui.modify");

	/**
	 * Announces that the SDK's own inert body ran instead of a game implementation.
	 *
	 * The first call for a given entry point warns: a game that forgot to register its own
	 * UGameUIModAPI subclass has a real configuration bug, and the mod that called in is about to
	 * behave as though the game refused it. Every later call for the same entry point drops to
	 * Verbose, because a mod polling the API on tick must not be able to flood the log.
	 *
	 * bInOutReported is a latch owned by the call site. Game thread only, like the rest of the
	 * framework.
	 */
	void ReportNoGameImplementation(const UModAPI& Api, const TCHAR* EntryPoint, bool& bInOutReported)
	{
		if (!bInOutReported)
		{
			bInOutReported = true;

			UE_LOG(LogGameModSDK, Warning,
				TEXT("Mod API '%s' has no game implementation of %s. The SDK only declares this surface; the game must register a subclass that overrides it. The call was ignored."),
				*Api.GetApiId().ToString(), EntryPoint);

			return;
		}

		UE_LOG(LogGameModSDK, Verbose,
			TEXT("Mod API '%s': %s is still unimplemented; the call was ignored."),
			*Api.GetApiId().ToString(), EntryPoint);
	}
}

FName UGameUIModAPI::GetStaticApiId()
{
	return FName(GameUIApiIdString);
}

FModVersion UGameUIModAPI::GetStaticApiVersion()
{
	return FModVersion(1, 0, 0);
}

FName UGameUIModAPI::GetStaticRequiredPermission()
{
	return FName(GameUIPermissionString);
}

//////////////////////////////////////////////////////////////////////////
// HUD widgets

// No VerifyServerAuthority anywhere in this file: the HUD is local state, and refusing a UI mod on a
// network client would break exactly the mods this API exists for. See the class comment.

UUserWidget* UGameUIModAPI::AddWidgetToHUD(TSubclassOf<UUserWidget> WidgetClass, int32 ZOrder)
{
	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("AddWidgetToHUD"), bReported);

	return nullptr;
}

bool UGameUIModAPI::RemoveWidgetFromHUD(UUserWidget* Widget)
{
	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("RemoveWidgetFromHUD"), bReported);

	return false;
}

//////////////////////////////////////////////////////////////////////////
// Notifications

void UGameUIModAPI::ShowNotification(const FText& Message, float Duration)
{
	static bool bReported = false;
	ReportNoGameImplementation(*this, TEXT("ShowNotification"), bReported);
}

//////////////////////////////////////////////////////////////////////////
// Identity

FName UGameUIModAPI::NativeGetApiId() const
{
	return GetStaticApiId();
}

FModVersion UGameUIModAPI::NativeGetApiVersion() const
{
	return GetStaticApiVersion();
}

FString UGameUIModAPI::NativeGetApiDescription() const
{
	return TEXT("Adds and removes HUD widgets and shows transient notifications. Client-side only; it never changes replicated state.");
}

bool UGameUIModAPI::NativeGetServerAuthoritative(bool& bOutServerAuthoritative) const
{
	// "Explicitly false", not "unspecified". Returning true here is what stops the resolution chain
	// from falling through to the class metadata, which a cooked Shipping build would have stripped.
	bOutServerAuthoritative = false;
	return true;
}

TArray<FName> UGameUIModAPI::NativeGetRequiredPermissions() const
{
	return TArray<FName>{ GetStaticRequiredPermission() };
}
