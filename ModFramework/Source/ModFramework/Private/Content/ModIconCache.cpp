// Copyright (c) 2026. Licensed for use in your own projects.

#include "Content/ModIconCache.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Ticker.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkLog.h"
#include "Core/ModFrameworkTypes.h"
#include "CoreGlobals.h"
#include "CoreTypes.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Logging/LogMacros.h"
#include "Manifest/ModManifest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Packaging/ModPackageFormat.h"
#include "Registry/ModInfo.h"
#include "Registry/ModRegistry.h"
#include "Settings/ModFrameworkSettings.h"
#include "Subsystem/ModSubsystem.h"
#include "Templates/SharedPointer.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectPtr.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

/**
 * File-local helpers.
 *
 * Named rather than anonymous on purpose: this module is built with unity files, where every
 * anonymous namespace in the blob is the same namespace, so a generic helper name here would collide
 * with an identically named helper in a sibling .cpp. Member functions pull these in with a
 * function-scope using-directive, which cannot leak into the rest of the unity file.
 */
namespace ModIconCachePrivate
{
	/** Stable diagnostic codes emitted by the icon cache. Documented on UModIconCache. */
	const TCHAR* const CodeNotFound           = TEXT("Icon.NotFound");
	const TCHAR* const CodeTooLarge           = TEXT("Icon.TooLarge");
	const TCHAR* const CodeUnsupportedFormat  = TEXT("Icon.UnsupportedFormat");
	const TCHAR* const CodeDecodeFailed       = TEXT("Icon.DecodeFailed");
	const TCHAR* const CodeDimensionsExceeded = TEXT("Icon.DimensionsExceeded");
	const TCHAR* const CodeUnsafePath         = TEXT("Icon.UnsafePath");

	/**
	 * Caps used when the settings object cannot be reached at all, which only happens very early in
	 * startup or very late in shutdown. They mirror the defaults declared on UModFrameworkSettings, so
	 * behaviour never changes silently depending on when an icon happened to be asked for.
	 */
	constexpr int64 FallbackMaxIconFileBytes = 2 * 1024 * 1024;
	constexpr int32 FallbackMaxIconDimension = 1024;

	FModDiagnostic MakeIconDiagnostic(const TCHAR* Code, FString Message, const FString& Context, const FModId& ModId)
	{
		FModDiagnostic Diagnostic(EModDiagnosticSeverity::Error, FName(Code), MoveTemp(Message), Context);
		Diagnostic.ModId = ModId;
		return Diagnostic;
	}

	/**
	 * Reports a failed icon load.
	 *
	 * Warning, never Error: an icon that will not load is a cosmetic problem, and nothing about it is
	 * allowed to look like a reason the mod itself did not load. A diagnostic with no code is the
	 * "this mod simply has no icon" case and says nothing at all.
	 */
	void LogIconDiagnostic(const FModDiagnostic& Diagnostic)
	{
		if (Diagnostic.Code.IsNone())
		{
			return;
		}

		UE_LOG(LogModFramework, Warning, TEXT("%s"), *Diagnostic.ToString());
	}

	/** The name of the image format, for diagnostics a mod author has to be able to act on. */
	const TCHAR* DescribeImageFormat(EImageFormat In)
	{
		switch (In)
		{
		case EImageFormat::PNG:            return TEXT("PNG");
		case EImageFormat::JPEG:           return TEXT("JPEG");
		case EImageFormat::GrayscaleJPEG:  return TEXT("grayscale JPEG");
		case EImageFormat::BMP:            return TEXT("BMP");
		case EImageFormat::ICO:            return TEXT("ICO");
		case EImageFormat::EXR:            return TEXT("EXR");
		case EImageFormat::ICNS:           return TEXT("ICNS");
		case EImageFormat::TGA:            return TEXT("TGA");
		case EImageFormat::HDR:            return TEXT("HDR");
		case EImageFormat::TIFF:           return TEXT("TIFF");
		case EImageFormat::DDS:            return TEXT("DDS");
		case EImageFormat::UEJPEG:         return TEXT("UE JPEG");
		case EImageFormat::GrayscaleUEJPEG:return TEXT("grayscale UE JPEG");
		default:                           return TEXT("an unrecognised format");
		}
	}

	/** PNG and JPEG only. Anything else a mod ships is refused whatever the file was named. */
	bool IsAllowedIconFormat(EImageFormat In)
	{
		return In == EImageFormat::PNG
			|| In == EImageFormat::JPEG
			|| In == EImageFormat::GrayscaleJPEG;
	}

	/**
	 * Loads an icon's bytes. Runs on a task thread: it touches no UObject and no engine subsystem.
	 *
	 * The size is checked before anything is allocated, from the directory entry for a loose file and
	 * from the table of contents for a packaged one, so a hostile mod cannot make the framework
	 * reserve a gigabyte on the strength of a declared length.
	 */
	bool ReadIconBytes(const FString& FilePath, const FString& EntryPath, bool bFromPackage, int64 MaxFileBytes,
		const FModId& ModId, TArray<uint8>& OutBytes, FModDiagnostic& OutError)
	{
		OutBytes.Reset();

		if (bFromPackage)
		{
			// A reader is created here rather than shared, because FModPackageReader owns a file
			// handle and is explicitly not thread safe.
			FModPackageReader Reader;

			TArray<FModDiagnostic> OpenDiagnostics;
			if (!Reader.Open(FilePath, OpenDiagnostics))
			{
				OutError = MakeIconDiagnostic(CodeNotFound,
					FString::Printf(TEXT("The package holding this mod's icon could not be opened: %s"),
						*ModDiagnostics::Join(OpenDiagnostics, TEXT("; "))),
					FilePath, ModId);
				return false;
			}

			FModPackageEntry Entry;
			if (!Reader.FindEntry(EntryPath, Entry))
			{
				OutError = MakeIconDiagnostic(CodeNotFound,
					FString::Printf(TEXT("The package contains no entry '%s'."), *EntryPath),
					FilePath, ModId);
				return false;
			}

			if (Entry.UncompressedSize > MaxFileBytes)
			{
				OutError = MakeIconDiagnostic(CodeTooLarge,
					FString::Printf(TEXT("Icon '%s' is %lld bytes; the maximum is %lld."),
						*EntryPath, Entry.UncompressedSize, MaxFileBytes),
					FilePath, ModId);
				return false;
			}

			FModDiagnostic ReadError;
			if (!Reader.ReadEntry(EntryPath, OutBytes, ReadError))
			{
				OutError = MakeIconDiagnostic(CodeNotFound,
					FString::Printf(TEXT("Icon '%s' could not be read out of the package: %s"),
						*EntryPath, *ReadError.Message),
					FilePath, ModId);
				OutBytes.Reset();
				return false;
			}
		}
		else
		{
			IFileManager& FileManager = IFileManager::Get();

			const int64 FileSize = FileManager.FileSize(*FilePath);
			if (FileSize < 0)
			{
				OutError = MakeIconDiagnostic(CodeNotFound,
					TEXT("The icon file does not exist or cannot be opened."), FilePath, ModId);
				return false;
			}

			if (FileSize > MaxFileBytes)
			{
				OutError = MakeIconDiagnostic(CodeTooLarge,
					FString::Printf(TEXT("The icon file is %lld bytes; the maximum is %lld."), FileSize, MaxFileBytes),
					FilePath, ModId);
				return false;
			}

			if (!FFileHelper::LoadFileToArray(OutBytes, *FilePath))
			{
				OutError = MakeIconDiagnostic(CodeNotFound,
					TEXT("The icon file could not be read."), FilePath, ModId);
				OutBytes.Reset();
				return false;
			}
		}

		if (OutBytes.Num() == 0)
		{
			OutError = MakeIconDiagnostic(CodeUnsupportedFormat, TEXT("The icon file is empty."), FilePath, ModId);
			return false;
		}

		return true;
	}

	/**
	 * Sniffs the format and measures the image without decoding a single pixel. Also runs on a task
	 * thread; Module must have been resolved on the game thread by the caller.
	 *
	 * Reading the dimensions out of the header first is the whole point: a 40 KB PNG can legally
	 * declare 60000x60000 pixels, and finding that out after decoding would be finding it out too
	 * late.
	 */
	bool ValidateIconImage(IImageWrapperModule* Module, const TArray<uint8>& Bytes, int32 MaxDimension,
		const FString& Context, const FModId& ModId, FModDiagnostic& OutError)
	{
		if (Module == nullptr)
		{
			OutError = MakeIconDiagnostic(CodeDecodeFailed,
				TEXT("The ImageWrapper module is unavailable, so the icon cannot be checked before decoding."),
				Context, ModId);
			return false;
		}

		const EImageFormat Format = Module->DetectImageFormat(Bytes.GetData(), Bytes.Num());
		if (!IsAllowedIconFormat(Format))
		{
			OutError = MakeIconDiagnostic(CodeUnsupportedFormat,
				FString::Printf(TEXT("The icon's contents are %s. A mod icon must be a PNG or a JPEG."),
					DescribeImageFormat(Format)),
				Context, ModId);
			return false;
		}

		const TSharedPtr<IImageWrapper> Wrapper = Module->CreateImageWrapper(Format);
		if (!Wrapper.IsValid())
		{
			OutError = MakeIconDiagnostic(CodeDecodeFailed,
				FString::Printf(TEXT("No reader is available for %s images in this build."), DescribeImageFormat(Format)),
				Context, ModId);
			return false;
		}

		if (!Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
		{
			OutError = MakeIconDiagnostic(CodeDecodeFailed,
				TEXT("The icon's image header is malformed."), Context, ModId);
			return false;
		}

		const int64 Width = Wrapper->GetWidth();
		const int64 Height = Wrapper->GetHeight();

		if (Width <= 0 || Height <= 0)
		{
			OutError = MakeIconDiagnostic(CodeDecodeFailed,
				FString::Printf(TEXT("The icon declares a %lldx%lld image, which is not a picture."), Width, Height),
				Context, ModId);
			return false;
		}

		if (Width > static_cast<int64>(MaxDimension) || Height > static_cast<int64>(MaxDimension))
		{
			OutError = MakeIconDiagnostic(CodeDimensionsExceeded,
				FString::Printf(TEXT("The icon is %lldx%lld pixels; neither edge may exceed %d."),
					Width, Height, MaxDimension),
				Context, ModId);
			return false;
		}

		return true;
	}
}

void UModIconCache::Initialize(UModSubsystem* InSubsystem)
{
	Subsystem = InSubsystem;

	// Resolved here, on the game thread, because module loading is game thread only. Every later icon
	// read hands the resulting interface pointer to a worker instead of touching the module manager.
	GetImageWrapperModule();

	EnsureDefaultIcon();
}

void UModIconCache::Shutdown()
{
	// Answer everything still waiting before the fallback is dropped, so no caller is left holding a
	// delegate that will never fire.
	if (PendingRequests.Num() > 0)
	{
		TArray<FModId> Waiting;
		PendingRequests.GetKeys(Waiting);

		UTexture2D* const Fallback = GetDefaultIcon();
		for (const FModId& ModId : Waiting)
		{
			FlushPendingCallbacks(ModId, Fallback);
		}

		PendingRequests.Empty();
	}

	// Bumped after the flush, not before, so that a read a flushed callback managed to start is
	// invalidated too. Anything already in flight compares its captured generation against this one
	// when it lands and quietly stops, so no read can resurrect the cache after it was torn down.
	++Generation;

	Icons.Empty();
	DefaultIcon = nullptr;
	bDefaultIconResolved = false;
	ImageWrapperModule = nullptr;
	Subsystem.Reset();
}

UTexture2D* UModIconCache::FindIcon(FModId ModId) const
{
	const TObjectPtr<UTexture2D>* Found = Icons.Find(ModId);
	return Found != nullptr ? Found->Get() : nullptr;
}

void UModIconCache::RequestIcon(FModId ModId, FOnModIconLoaded OnLoaded)
{
	using namespace ModIconCachePrivate;

	// Everything below touches UObjects and the registry, both of which are game thread only. A caller
	// on a worker is bounced rather than refused, so the delegate still fires exactly once.
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UModIconCache> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, ModId, OnLoaded]()
		{
			if (UModIconCache* Cache = WeakThis.Get())
			{
				Cache->RequestIcon(ModId, OnLoaded);
			}
		});
		return;
	}

	EnsureDefaultIcon();

	if (TObjectPtr<UTexture2D>* Cached = Icons.Find(ModId))
	{
		if (UTexture2D* Texture = Cached->Get())
		{
			DispatchDeferred(ModId, Texture, OnLoaded);
			return;
		}

		// The map is a UPROPERTY, so this should not happen - but a TObjectPtr slot can be nulled by
		// garbage collection at any point, and reading a stale entry back as "cached, but nothing" is
		// worse than reading the file again.
		Icons.Remove(ModId);
	}

	// A read is already running for this mod: join it instead of starting a second one. This is what
	// keeps a mod list of fifty entries from opening the same file fifty times during one layout pass.
	if (TArray<FOnModIconLoaded>* Waiting = PendingRequests.Find(ModId))
	{
		Waiting->Add(OnLoaded);
		return;
	}

	FString FilePath;
	FString EntryPath;
	bool bFromPackage = false;
	FModDiagnostic ResolveError;
	if (!ResolveIconSource(ModId, FilePath, EntryPath, bFromPackage, ResolveError))
	{
		LogIconDiagnostic(ResolveError);
		DispatchDeferred(ModId, GetDefaultIcon(), OnLoaded);
		return;
	}

	TArray<FOnModIconLoaded> Callbacks;
	Callbacks.Add(OnLoaded);
	PendingRequests.Add(ModId, MoveTemp(Callbacks));

	// The caps are snapshotted here so the worker never reads a UObject, not even the settings CDO.
	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	const int64 MaxFileBytes = Settings != nullptr ? Settings->GetEffectiveMaxIconFileBytes() : FallbackMaxIconFileBytes;
	const int32 MaxDimension = Settings != nullptr ? Settings->GetEffectiveMaxIconDimension() : FallbackMaxIconDimension;

	IImageWrapperModule* Module = GetImageWrapperModule();
	const uint64 RequestGeneration = Generation;
	TWeakObjectPtr<UModIconCache> WeakThis(this);

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[WeakThis, ModId, FilePath, EntryPath, bFromPackage, MaxFileBytes, MaxDimension, Module, RequestGeneration]()
		{
			// Deliberately nothing but plain data in this scope: WeakThis is not dereferenced until
			// the continuation below is back on the game thread.
			TArray<uint8> Bytes;
			FModDiagnostic Error;

			bool bSucceeded = ReadIconBytes(FilePath, EntryPath, bFromPackage, MaxFileBytes, ModId, Bytes, Error);
			if (bSucceeded)
			{
				const FString& Context = bFromPackage ? EntryPath : FilePath;
				bSucceeded = ValidateIconImage(Module, Bytes, MaxDimension, Context, ModId, Error);
			}

			if (!bSucceeded)
			{
				Bytes.Reset();
			}

			AsyncTask(ENamedThreads::GameThread,
				[WeakThis, ModId, RequestGeneration, Bytes = MoveTemp(Bytes), Error = MoveTemp(Error), bSucceeded]() mutable
				{
					if (UModIconCache* Cache = WeakThis.Get())
					{
						Cache->FinishAsyncRead(ModId, RequestGeneration, MoveTemp(Bytes), MoveTemp(Error), bSucceeded);
					}
				});
		});
}

UTexture2D* UModIconCache::LoadIconSynchronous(const FModId& ModId, FModDiagnostic& OutError)
{
	using namespace ModIconCachePrivate;

	OutError = FModDiagnostic();

	if (!IsInGameThread())
	{
		OutError = MakeIconDiagnostic(CodeDecodeFailed,
			TEXT("LoadIconSynchronous imports a texture and must be called on the game thread. Use RequestIcon instead."),
			FString(), ModId);
		return GetDefaultIcon();
	}

	EnsureDefaultIcon();

	if (TObjectPtr<UTexture2D>* Cached = Icons.Find(ModId))
	{
		if (UTexture2D* Texture = Cached->Get())
		{
			return Texture;
		}

		Icons.Remove(ModId);
	}

	FString FilePath;
	FString EntryPath;
	bool bFromPackage = false;
	if (!ResolveIconSource(ModId, FilePath, EntryPath, bFromPackage, OutError))
	{
		LogIconDiagnostic(OutError);
		return GetDefaultIcon();
	}

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	const int64 MaxFileBytes = Settings != nullptr ? Settings->GetEffectiveMaxIconFileBytes() : FallbackMaxIconFileBytes;
	const int32 MaxDimension = Settings != nullptr ? Settings->GetEffectiveMaxIconDimension() : FallbackMaxIconDimension;

	TArray<uint8> Bytes;
	if (!ReadIconBytes(FilePath, EntryPath, bFromPackage, MaxFileBytes, ModId, Bytes, OutError))
	{
		LogIconDiagnostic(OutError);
		return GetDefaultIcon();
	}

	const FString& Context = bFromPackage ? EntryPath : FilePath;
	if (!ValidateIconImage(GetImageWrapperModule(), Bytes, MaxDimension, Context, ModId, OutError))
	{
		LogIconDiagnostic(OutError);
		return GetDefaultIcon();
	}

	UTexture2D* Icon = ImportIconBytes(ModId, Bytes, MaxDimension, OutError);
	if (Icon == nullptr)
	{
		LogIconDiagnostic(OutError);
		return GetDefaultIcon();
	}

	Icons.Add(ModId, Icon);

	// Requests already in flight for this mod are left alone on purpose: their completion finds this
	// entry in the cache and hands out the very same texture, without a delegate firing re-entrantly
	// from inside this call.
	return Icon;
}

void UModIconCache::ReleaseIcon(FModId ModId)
{
	Icons.Remove(ModId);
}

void UModIconCache::ReleaseAll()
{
	Icons.Empty();
}

UTexture2D* UModIconCache::GetDefaultIcon() const
{
	return DefaultIcon;
}

bool UModIconCache::HasIcon(FModId ModId) const
{
	FString FilePath;
	FString EntryPath;
	bool bFromPackage = false;
	FModDiagnostic Error;

	return ResolveIconSource(ModId, FilePath, EntryPath, bFromPackage, Error);
}

void UModIconCache::EnsureDefaultIcon()
{
	if (bDefaultIconResolved)
	{
		return;
	}

	bDefaultIconResolved = true;

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	if (Settings == nullptr || Settings->DefaultModIcon.IsNull())
	{
		return;
	}

	// Synchronous on purpose. This is one small asset the project itself configured, it is resolved
	// exactly once, and every icon request downstream needs a fallback ready to hand back.
	DefaultIcon = Settings->DefaultModIcon.LoadSynchronous();

	if (DefaultIcon == nullptr)
	{
		UE_LOG(LogModFramework, Warning,
			TEXT("The default mod icon '%s' configured in project settings could not be loaded. Mods without a usable icon will be reported with no icon at all."),
			*Settings->DefaultModIcon.ToString());
	}
}

bool UModIconCache::ResolveIconSource(const FModId& ModId, FString& OutFilePath, FString& OutEntryPath, bool& bOutFromPackage, FModDiagnostic& OutError) const
{
	using namespace ModIconCachePrivate;

	OutFilePath.Reset();
	OutEntryPath.Reset();
	bOutFromPackage = false;
	OutError = FModDiagnostic();

	if (!ModId.IsValid())
	{
		OutError = MakeIconDiagnostic(CodeNotFound, TEXT("An icon was requested for an empty mod id."), FString(), ModId);
		return false;
	}

	UModSubsystem* OwningSubsystem = Subsystem.Get();
	const UModRegistry* Registry = OwningSubsystem != nullptr ? OwningSubsystem->GetRegistry() : nullptr;
	if (Registry == nullptr)
	{
		OutError = MakeIconDiagnostic(CodeNotFound,
			TEXT("The mod registry is not available, so no icon can be located."), FString(), ModId);
		return false;
	}

	const FModInfo* Info = Registry->FindMod(ModId);
	if (Info == nullptr)
	{
		OutError = MakeIconDiagnostic(CodeNotFound,
			FString::Printf(TEXT("Mod '%s' is not registered."), *ModId.ToString()), FString(), ModId);
		return false;
	}

	const FString IconPath = Info->Manifest.IconPath.TrimStartAndEnd();
	if (IconPath.IsEmpty())
	{
		// The ordinary case for most mods, and not a problem. Leaving OutError's code empty is what
		// tells the caller to stay quiet about it.
		return false;
	}

	// Defence in depth. FModManifestParser::ValidateManifest already rejected an unsafe path, but an
	// FModInfo can also be built in code or arrive from a provider that never went through the
	// validator, and this string is about to be used as a filesystem path and as a package key.
	FString PathError;
	if (!ModPackage::IsSafeRelativePath(IconPath, PathError))
	{
		OutError = MakeIconDiagnostic(CodeUnsafePath,
			FString::Printf(TEXT("Icon path '%s' was rejected: %s"), *IconPath, *PathError), Info->RootPath, ModId);
		return false;
	}

	if (Info->RootPath.IsEmpty())
	{
		OutError = MakeIconDiagnostic(CodeNotFound,
			FString::Printf(TEXT("Mod '%s' declares an icon but has no root path."), *ModId.ToString()), FString(), ModId);
		return false;
	}

	// A mod discovered as a `.mod` container has the container itself as its root path, and its icon
	// is read straight out of the table of contents - no extraction and no mount required, which is
	// the whole reason a mod list can draw icons for mods that were never installed.
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*Info->RootPath)
		&& FPaths::GetExtension(Info->RootPath, /*bIncludeDot=*/true).Equals(ModPackage::GetFileExtension(), ESearchCase::IgnoreCase))
	{
		bOutFromPackage = true;
		OutFilePath = Info->RootPath;
		OutEntryPath = IconPath;
		return true;
	}

	// An unpacked mod. Discovery normally fills ResolvedIconPath in; recomputing it when it is empty
	// keeps the cache working for records built by anything that did not.
	OutFilePath = Info->ResolvedIconPath.IsEmpty() ? (Info->RootPath / IconPath) : Info->ResolvedIconPath;
	return true;
}

void UModIconCache::DispatchDeferred(const FModId& ModId, UTexture2D* Icon, const FOnModIconLoaded& OnLoaded) const
{
	if (!OnLoaded.IsBound())
	{
		return;
	}

	TWeakObjectPtr<const UModIconCache> WeakThis(this);
	TWeakObjectPtr<UTexture2D> WeakIcon(Icon);
	FOnModIconLoaded Callback = OnLoaded;

	// One shot: returning false unregisters the ticker. The hop through a tick is the point. A caller
	// must never see the delegate fire inside its own RequestIcon call for a warm cache and on a later
	// frame for a cold one, because code written against the first ordering breaks on the second.
	FTSTicker::GetCoreTicker().AddTicker(TEXT("ModIconCache.Dispatch"), 0.0f,
		[WeakThis, WeakIcon, Callback = MoveTemp(Callback), ModId](float) -> bool
		{
			UTexture2D* Resolved = WeakIcon.Get();
			if (Resolved == nullptr && WeakThis.IsValid())
			{
				Resolved = WeakThis->GetDefaultIcon();
			}

			Callback.ExecuteIfBound(ModId, Resolved);
			return false;
		});
}

void UModIconCache::FinishAsyncRead(const FModId& ModId, uint64 InGeneration, TArray<uint8> Bytes, FModDiagnostic Error, bool bReadSucceeded)
{
	using namespace ModIconCachePrivate;

	// Shutdown ran while this read was in flight. Its callbacks were already answered there.
	if (InGeneration != Generation)
	{
		return;
	}

	UTexture2D* Icon = nullptr;

	// LoadIconSynchronous may have imported this very icon while the read was running.
	if (TObjectPtr<UTexture2D>* Cached = Icons.Find(ModId))
	{
		Icon = Cached->Get();
	}

	if (Icon == nullptr)
	{
		if (bReadSucceeded)
		{
			const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
			const int32 MaxDimension = Settings != nullptr ? Settings->GetEffectiveMaxIconDimension() : FallbackMaxIconDimension;

			FModDiagnostic ImportError;
			Icon = ImportIconBytes(ModId, Bytes, MaxDimension, ImportError);
			if (Icon != nullptr)
			{
				Icons.Add(ModId, Icon);
			}
			else
			{
				LogIconDiagnostic(ImportError);
			}
		}
		else
		{
			LogIconDiagnostic(Error);
		}
	}

	FlushPendingCallbacks(ModId, Icon != nullptr ? Icon : GetDefaultIcon());
}

UTexture2D* UModIconCache::ImportIconBytes(const FModId& ModId, const TArray<uint8>& Bytes, int32 MaxDimension, FModDiagnostic& OutError) const
{
	using namespace ModIconCachePrivate;

	OutError = FModDiagnostic();

	if (Bytes.Num() == 0)
	{
		OutError = MakeIconDiagnostic(CodeDecodeFailed, TEXT("There are no icon bytes to decode."), FString(), ModId);
		return nullptr;
	}

	// Creates a transient, platform-data-only texture, which is exactly what a UI thumbnail wants and
	// why this has to happen on the game thread.
	UTexture2D* Texture = FImageUtils::ImportBufferAsTexture2D(Bytes);
	if (Texture == nullptr)
	{
		OutError = MakeIconDiagnostic(CodeDecodeFailed,
			TEXT("The icon's pixels could not be decoded."), FString(), ModId);
		return nullptr;
	}

	// The header was measured before the decode; this re-check costs nothing and closes the gap where
	// a file's header and its payload disagree about how big the picture is.
	if (Texture->GetSizeX() > MaxDimension || Texture->GetSizeY() > MaxDimension)
	{
		OutError = MakeIconDiagnostic(CodeDimensionsExceeded,
			FString::Printf(TEXT("The decoded icon is %dx%d pixels; neither edge may exceed %d."),
				Texture->GetSizeX(), Texture->GetSizeY(), MaxDimension),
			FString(), ModId);
		return nullptr;
	}

	return Texture;
}

void UModIconCache::FlushPendingCallbacks(const FModId& ModId, UTexture2D* Icon)
{
	TArray<FOnModIconLoaded> Callbacks;
	if (!PendingRequests.RemoveAndCopyValue(ModId, Callbacks))
	{
		return;
	}

	// The entry is gone before anything is called, so a callback that asks for another icon - or for
	// this one again - re-enters cleanly.
	for (const FOnModIconLoaded& Callback : Callbacks)
	{
		Callback.ExecuteIfBound(ModId, Icon);
	}
}

IImageWrapperModule* UModIconCache::GetImageWrapperModule() const
{
	if (ImageWrapperModule == nullptr && IsInGameThread())
	{
		static const FName ImageWrapperModuleName(TEXT("ImageWrapper"));

		ImageWrapperModule = FModuleManager::Get().LoadModulePtr<IImageWrapperModule>(ImageWrapperModuleName);
		if (ImageWrapperModule == nullptr)
		{
			UE_LOG(LogModFramework, Warning,
				TEXT("The ImageWrapper module is unavailable. Mod icons cannot be validated, so every mod will use the default icon."));
		}
	}

	return ImageWrapperModule;
}
