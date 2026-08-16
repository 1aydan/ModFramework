// Copyright (c) 2026. Licensed for use in your own projects.

#include "Manifest/ModManifestParser.h"

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "Core/ModFrameworkVersion.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/Platform.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CString.h"
#include "Misc/Char.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonTypes.h"
#include "Serialization/JsonWriter.h"
#include "Templates/SharedPointer.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"
#include "UObject/SoftObjectPath.h"

namespace ModManifestParserPrivate
{
	/**
	 * Stable machine-readable diagnostic codes.
	 *
	 * Kept as string literals rather than FName constants so that nothing constructs an FName during
	 * static initialisation; the FName is built at the point the diagnostic is created.
	 */
	namespace Codes
	{
		constexpr const TCHAR* ReadFailed = TEXT("Manifest.ReadFailed");
		constexpr const TCHAR* InvalidJson = TEXT("Manifest.InvalidJson");
		constexpr const TCHAR* MissingField = TEXT("Manifest.MissingField");
		constexpr const TCHAR* InvalidField = TEXT("Manifest.InvalidField");
		constexpr const TCHAR* InvalidId = TEXT("Manifest.InvalidId");
		constexpr const TCHAR* InvalidVersion = TEXT("Manifest.InvalidVersion");
		constexpr const TCHAR* InvalidVersionRange = TEXT("Manifest.InvalidVersionRange");
		constexpr const TCHAR* UnsupportedManifestVersion = TEXT("Manifest.UnsupportedManifestVersion");
		constexpr const TCHAR* UnknownField = TEXT("Manifest.UnknownField");
		constexpr const TCHAR* DuplicateDependency = TEXT("Manifest.DuplicateDependency");
		constexpr const TCHAR* SelfDependency = TEXT("Manifest.SelfDependency");
		constexpr const TCHAR* InvalidPermission = TEXT("Manifest.InvalidPermission");
		constexpr const TCHAR* InvalidEntryPoint = TEXT("Manifest.InvalidEntryPoint");
		constexpr const TCHAR* InvalidContentRoot = TEXT("Manifest.InvalidContentRoot");
		constexpr const TCHAR* EmptyContent = TEXT("Manifest.EmptyContent");
		constexpr const TCHAR* InvalidClaim = TEXT("Manifest.InvalidClaim");
		constexpr const TCHAR* InvalidIcon = TEXT("Manifest.InvalidIcon");
	}

	/** Canonical JSON key names. Defined once so the reader and the writer can never drift apart. */
	namespace Keys
	{
		constexpr const TCHAR* ManifestVersion = TEXT("manifestVersion");
		constexpr const TCHAR* Id = TEXT("id");
		constexpr const TCHAR* Name = TEXT("name");
		constexpr const TCHAR* Description = TEXT("description");
		constexpr const TCHAR* Author = TEXT("author");
		constexpr const TCHAR* Homepage = TEXT("homepage");
		constexpr const TCHAR* License = TEXT("license");
		constexpr const TCHAR* Icon = TEXT("icon");
		constexpr const TCHAR* Version = TEXT("version");
		constexpr const TCHAR* Game = TEXT("game");
		constexpr const TCHAR* Framework = TEXT("framework");
		constexpr const TCHAR* Sdk = TEXT("sdk");
		constexpr const TCHAR* Dependencies = TEXT("dependencies");
		constexpr const TCHAR* LoadBefore = TEXT("loadBefore");
		constexpr const TCHAR* LoadAfter = TEXT("loadAfter");
		constexpr const TCHAR* Priority = TEXT("priority");
		constexpr const TCHAR* Network = TEXT("network");
		constexpr const TCHAR* Permissions = TEXT("permissions");
		constexpr const TCHAR* EntryPoint = TEXT("entryPoint");
		constexpr const TCHAR* Content = TEXT("content");
		constexpr const TCHAR* Claims = TEXT("claims");
		constexpr const TCHAR* Metadata = TEXT("metadata");

		constexpr const TCHAR* Optional = TEXT("optional");
		constexpr const TCHAR* Reason = TEXT("reason");
		constexpr const TCHAR* Scope = TEXT("scope");
		constexpr const TCHAR* RequiredForNetworkPlay = TEXT("requiredForNetworkPlay");
		constexpr const TCHAR* NativeModule = TEXT("nativeModule");
		constexpr const TCHAR* Class = TEXT("class");
		constexpr const TCHAR* ContentBundles = TEXT("contentBundles");
		constexpr const TCHAR* Path = TEXT("path");
		constexpr const TCHAR* Type = TEXT("type");
		constexpr const TCHAR* MountPoint = TEXT("mountPoint");
		constexpr const TCHAR* MountOrder = TEXT("mountOrder");
		constexpr const TCHAR* ExtensionPoint = TEXT("extensionPoint");
		constexpr const TCHAR* Resource = TEXT("resource");
		constexpr const TCHAR* Policy = TEXT("policy");
	}

	/**
	 * Hard caps applied before any FName or FSoftObjectPath is built.
	 *
	 * FName construction from a string longer than NAME_SIZE (1024) is a fatal error in the engine,
	 * and a mod author controls every one of these strings, so length is checked first and always.
	 * The limits are far above anything a real manifest needs.
	 */
	constexpr int32 MaxModIdLength = 128;
	constexpr int32 MaxNameLength = 256;
	constexpr int32 MaxObjectPathLength = 512;
	constexpr int32 MaxContentPathLength = 512;

	/** Largest manifest file the reader will load, to bound the memory an untrusted file can cost. */
	constexpr int64 MaxManifestFileSize = 4 * 1024 * 1024;

	/** Inclusive bounds on FModManifest::Priority. */
	constexpr int32 MinPriority = -1000;
	constexpr int32 MaxPriority = 1000;

	FString JoinFieldPath(const FString& Prefix, const FString& Key)
	{
		if (Prefix.IsEmpty())
		{
			return Key;
		}

		return FString::Printf(TEXT("%s.%s"), *Prefix, *Key);
	}

	FString MakeIndexedFieldPath(const TCHAR* ArrayKey, int32 Index)
	{
		return FString::Printf(TEXT("%s[%d]"), ArrayKey, Index);
	}

	/** Combines the caller-supplied file label with the field path into FModDiagnostic::Context. */
	FString MakeDiagnosticContext(const FString& ContextPath, const FString& FieldPath)
	{
		if (ContextPath.IsEmpty())
		{
			return FieldPath;
		}

		if (FieldPath.IsEmpty())
		{
			return ContextPath;
		}

		return FString::Printf(TEXT("%s : %s"), *ContextPath, *FieldPath);
	}

	/** Collects diagnostics for one manifest, remembering which file they belong to. */
	struct FContext
	{
		FString ContextPath;
		TArray<FModDiagnostic>* Diagnostics = nullptr;

		void Add(EModDiagnosticSeverity Severity, const TCHAR* Code, const FString& FieldPath, FString Message) const
		{
			if (Diagnostics == nullptr)
			{
				return;
			}

			Diagnostics->Add(FModDiagnostic(Severity, FName(Code), MoveTemp(Message), MakeDiagnosticContext(ContextPath, FieldPath)));
		}

		void Error(const TCHAR* Code, const FString& FieldPath, FString Message) const
		{
			Add(EModDiagnosticSeverity::Error, Code, FieldPath, MoveTemp(Message));
		}

		void Warning(const TCHAR* Code, const FString& FieldPath, FString Message) const
		{
			Add(EModDiagnosticSeverity::Warning, Code, FieldPath, MoveTemp(Message));
		}
	};

	/**
	 * Fills in FModDiagnostic::ModId on the diagnostics added from StartIndex onwards, so callers
	 * scanning a whole mod directory can group messages by mod without re-reading the file.
	 */
	void StampModId(TArray<FModDiagnostic>& InOutDiagnostics, int32 StartIndex, const FModId& ModId)
	{
		if (!ModId.IsValid())
		{
			return;
		}

		for (int32 Index = FMath::Max(StartIndex, 0); Index < InOutDiagnostics.Num(); ++Index)
		{
			if (!InOutDiagnostics[Index].ModId.IsValid())
			{
				InOutDiagnostics[Index].ModId = ModId;
			}
		}
	}

	/** Whether a key was present, present with the wrong JSON type, or absent. */
	enum class EFieldStatus : uint8
	{
		Absent,
		WrongType,
		Present
	};

	const TCHAR* DescribeJsonType(EJson In)
	{
		switch (In)
		{
		case EJson::Null:    return TEXT("null");
		case EJson::String:  return TEXT("a string");
		case EJson::Number:  return TEXT("a number");
		case EJson::Boolean: return TEXT("a boolean");
		case EJson::Array:   return TEXT("an array");
		case EJson::Object:  return TEXT("an object");
		case EJson::None:    return TEXT("missing");
		default:             return TEXT("of an unrecognised type");
		}
	}

	/** Looks a key up, treating an explicit JSON null exactly like an absent key. */
	TSharedPtr<FJsonValue> FindField(const FJsonObject& Object, const TCHAR* Key)
	{
		TSharedPtr<FJsonValue> Value = Object.TryGetField(Key);
		if (Value.IsValid() && Value->Type == EJson::Null)
		{
			return TSharedPtr<FJsonValue>();
		}

		return Value;
	}

	void ReportWrongType(const FContext& Ctx, const FString& FieldPath, const TCHAR* Expected, EJson Actual)
	{
		Ctx.Error(Codes::InvalidField, FieldPath,
			FString::Printf(TEXT("Field '%s' must be %s but is %s."), *FieldPath, Expected, DescribeJsonType(Actual)));
	}

	EFieldStatus ReadStringField(const FJsonObject& Object, const TCHAR* Key, const FString& FieldPath, const FContext& Ctx, FString& Out)
	{
		const TSharedPtr<FJsonValue> Value = FindField(Object, Key);
		if (!Value.IsValid())
		{
			return EFieldStatus::Absent;
		}

		if (Value->Type != EJson::String)
		{
			ReportWrongType(Ctx, FieldPath, TEXT("a string"), Value->Type);
			return EFieldStatus::WrongType;
		}

		Value->TryGetString(Out);
		return EFieldStatus::Present;
	}

	EFieldStatus ReadBoolField(const FJsonObject& Object, const TCHAR* Key, const FString& FieldPath, const FContext& Ctx, bool& Out)
	{
		const TSharedPtr<FJsonValue> Value = FindField(Object, Key);
		if (!Value.IsValid())
		{
			return EFieldStatus::Absent;
		}

		if (Value->Type != EJson::Boolean)
		{
			ReportWrongType(Ctx, FieldPath, TEXT("a boolean"), Value->Type);
			return EFieldStatus::WrongType;
		}

		Value->TryGetBool(Out);
		return EFieldStatus::Present;
	}

	EFieldStatus ReadIntField(const FJsonObject& Object, const TCHAR* Key, const FString& FieldPath, const FContext& Ctx, int32& Out)
	{
		const TSharedPtr<FJsonValue> Value = FindField(Object, Key);
		if (!Value.IsValid())
		{
			return EFieldStatus::Absent;
		}

		if (Value->Type != EJson::Number)
		{
			ReportWrongType(Ctx, FieldPath, TEXT("a whole number"), Value->Type);
			return EFieldStatus::WrongType;
		}

		double Number = 0.0;
		if (!Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
		{
			Ctx.Error(Codes::InvalidField, FieldPath,
				FString::Printf(TEXT("Field '%s' is not a finite number."), *FieldPath));
			return EFieldStatus::WrongType;
		}

		if (Number != FMath::TruncToDouble(Number))
		{
			Ctx.Error(Codes::InvalidField, FieldPath,
				FString::Printf(TEXT("Field '%s' must be a whole number but is %f."), *FieldPath, Number));
			return EFieldStatus::WrongType;
		}

		if (Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
		{
			Ctx.Error(Codes::InvalidField, FieldPath,
				FString::Printf(TEXT("Field '%s' is outside the range of a 32 bit integer."), *FieldPath));
			return EFieldStatus::WrongType;
		}

		Out = static_cast<int32>(Number);
		return EFieldStatus::Present;
	}

	EFieldStatus ReadObjectField(const FJsonObject& Object, const TCHAR* Key, const FString& FieldPath, const FContext& Ctx, TSharedPtr<FJsonObject>& Out)
	{
		const TSharedPtr<FJsonValue> Value = FindField(Object, Key);
		if (!Value.IsValid())
		{
			return EFieldStatus::Absent;
		}

		if (Value->Type != EJson::Object)
		{
			ReportWrongType(Ctx, FieldPath, TEXT("an object"), Value->Type);
			return EFieldStatus::WrongType;
		}

		Out = Value->AsObject();
		if (!Out.IsValid())
		{
			ReportWrongType(Ctx, FieldPath, TEXT("an object"), Value->Type);
			return EFieldStatus::WrongType;
		}

		return EFieldStatus::Present;
	}

	/**
	 * Reads an array field. OutValues is only valid while Object is alive, which is always the case
	 * here because the whole parse happens inside one call.
	 */
	EFieldStatus ReadArrayField(const FJsonObject& Object, const TCHAR* Key, const FString& FieldPath, const FContext& Ctx, const TArray<TSharedPtr<FJsonValue>>*& OutValues)
	{
		const TSharedPtr<FJsonValue> Value = FindField(Object, Key);
		if (!Value.IsValid())
		{
			return EFieldStatus::Absent;
		}

		if (Value->Type != EJson::Array)
		{
			ReportWrongType(Ctx, FieldPath, TEXT("an array"), Value->Type);
			return EFieldStatus::WrongType;
		}

		OutValues = &Value->AsArray();
		return EFieldStatus::Present;
	}

	/**
	 * Reads one array element as a string. Returns false, after reporting the wrong type, when the
	 * element is not a string; callers skip it and keep their own index so diagnostics always name
	 * the position the element really has in the file.
	 */
	bool TryGetStringElement(const TArray<TSharedPtr<FJsonValue>>& Values, int32 Index, const FString& ElementPath, const FContext& Ctx, FString& Out)
	{
		const TSharedPtr<FJsonValue>& Element = Values[Index];
		if (!Element.IsValid() || Element->Type != EJson::String)
		{
			ReportWrongType(Ctx, ElementPath, TEXT("a string"), Element.IsValid() ? Element->Type : EJson::None);
			return false;
		}

		Element->TryGetString(Out);
		return true;
	}

	/** Emits a Manifest.UnknownField warning for every key the schema does not define at this level. */
	void WarnUnknownFields(const FJsonObject& Object, TConstArrayView<const TCHAR*> KnownKeys, const FString& FieldPathPrefix, const FContext& Ctx)
	{
		TArray<FString> UnknownKeys;

		// The key type of FJsonObject::Values differs between the shared-string and legacy string
		// builds of the Json module, so the pair type is deduced rather than spelled out.
		for (const auto& Pair : Object.Values)
		{
			const FString Key(*Pair.Key);

			bool bKnown = false;
			for (const TCHAR* KnownKey : KnownKeys)
			{
				// FJsonObject looks fields up case insensitively, so the same rule has to apply here
				// or a manifest writing "ID" would be both read and reported as unknown.
				if (FCString::Stricmp(*Key, KnownKey) == 0)
				{
					bKnown = true;
					break;
				}
			}

			if (!bKnown)
			{
				UnknownKeys.Add(Key);
			}
		}

		// Sorted so the diagnostics come out in the same order on every run and every platform.
		UnknownKeys.Sort();

		for (const FString& Key : UnknownKeys)
		{
			const FString FieldPath = JoinFieldPath(FieldPathPrefix, Key);
			Ctx.Warning(Codes::UnknownField, FieldPath,
				FString::Printf(TEXT("Unknown field '%s' was ignored."), *FieldPath));
		}
	}

	/**
	 * Turns manifest text into an FModId without validating it.
	 *
	 * Only the length is enforced here, because building an FName out of an over-long string is a
	 * fatal engine error. Everything else about the id is left in place so that
	 * FModManifestParser::ValidateManifest can report exactly what is wrong with it, once.
	 */
	bool TryMakeModId(const FString& Raw, const FString& FieldPath, const FContext& Ctx, FModId& OutId)
	{
		if (Raw.Len() > MaxModIdLength)
		{
			Ctx.Error(Codes::InvalidId, FieldPath,
				FString::Printf(TEXT("Mod id in '%s' is %d characters long; the maximum is %d."),
					*FieldPath, Raw.Len(), MaxModIdLength));
			return false;
		}

		OutId = FModId::FromString(Raw);
		return true;
	}

	/** Same length-first reasoning as TryMakeModId, for the FName fields of permissions and claims. */
	bool TryMakeName(const FString& Raw, const FString& FieldPath, const TCHAR* Code, const FContext& Ctx, FName& OutName)
	{
		if (Raw.Len() > MaxNameLength)
		{
			Ctx.Error(Code, FieldPath,
				FString::Printf(TEXT("Value of '%s' is %d characters long; the maximum is %d."),
					*FieldPath, Raw.Len(), MaxNameLength));
			return false;
		}

		OutName = FName(*Raw);
		return true;
	}

	/**
	 * Parses an object path. FSoftObjectPath silently resets itself when handed something that is
	 * not a package path, so a non-empty string that produces a null path is reported here - the
	 * parsed struct alone could not tell "absent" from "malformed" later on.
	 */
	bool TryMakeObjectPath(const FString& Raw, const FString& FieldPath, const FContext& Ctx, FSoftObjectPath& OutPath)
	{
		const FString Trimmed = Raw.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutPath.Reset();
			return true;
		}

		if (Trimmed.Len() > MaxObjectPathLength)
		{
			Ctx.Error(Codes::InvalidEntryPoint, FieldPath,
				FString::Printf(TEXT("Object path in '%s' is %d characters long; the maximum is %d."),
					*FieldPath, Trimmed.Len(), MaxObjectPathLength));
			return false;
		}

		OutPath = FSoftObjectPath(Trimmed);
		if (OutPath.IsNull())
		{
			Ctx.Error(Codes::InvalidEntryPoint, FieldPath,
				FString::Printf(TEXT("'%s' is not a valid object path in field '%s'. Expected something like \"/MyMod/BP_Entry.BP_Entry_C\"."),
					*Trimmed, *FieldPath));
			return false;
		}

		return true;
	}

	/** True when every character of In is a letter, a digit or an underscore, and In starts a C identifier. */
	bool IsIdentifier(const FString& In)
	{
		if (In.IsEmpty())
		{
			return false;
		}

		if (!FChar::IsAlpha(In[0]) && In[0] != TEXT('_'))
		{
			return false;
		}

		for (const TCHAR Character : In)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
			{
				return false;
			}
		}

		return true;
	}

	bool ContainsWhitespace(const FString& In)
	{
		for (const TCHAR Character : In)
		{
			if (FChar::IsWhitespace(Character))
			{
				return true;
			}
		}

		return false;
	}

	/**
	 * SECURITY: decides whether a content root path stays inside the mod directory.
	 *
	 * A mod author writes this string, so it is rejected unless it is unambiguously relative:
	 * no drive letter or alternate data stream, no leading separator, no home-directory shorthand,
	 * no ".." segment and no control characters. Both separator styles are considered, because a
	 * manifest authored on Windows may well use backslashes.
	 */
	bool IsSafeRelativeContentPath(const FString& InPath, FString& OutReason)
	{
		if (InPath.Len() > MaxContentPathLength)
		{
			OutReason = FString::Printf(TEXT("it is %d characters long and the maximum is %d"), InPath.Len(), MaxContentPathLength);
			return false;
		}

		for (const TCHAR Character : InPath)
		{
			if (Character < TEXT(' ') || Character == TCHAR(0x7F))
			{
				OutReason = TEXT("it contains a control character");
				return false;
			}
		}

		FString Normalized = InPath;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);

		if (Normalized.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
		{
			OutReason = TEXT("it is an absolute path");
			return false;
		}

		if (Normalized.Contains(TEXT(":"), ESearchCase::CaseSensitive))
		{
			OutReason = TEXT("it names a drive or a stream");
			return false;
		}

		if (Normalized.StartsWith(TEXT("~"), ESearchCase::CaseSensitive))
		{
			OutReason = TEXT("it is relative to a home directory");
			return false;
		}

		TArray<FString> Segments;
		Normalized.ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty=*/false);
		for (const FString& Segment : Segments)
		{
			if (Segment == TEXT(".."))
			{
				OutReason = TEXT("it contains a \"..\" segment, which would escape the mod directory");
				return false;
			}
		}

		return true;
	}

	/**
	 * SECURITY: decides whether the manifest's `icon` names an image file inside the mod directory.
	 *
	 * The containment rules are exactly the ones a content root gets - the icon is a mod-authored path
	 * and deserves no more trust than any other - plus two restrictions of its own: the spelling must
	 * be the forward-slash one, and the file must be a format the framework is prepared to decode.
	 */
	bool IsSafeIconPath(const FString& InPath, FString& OutReason)
	{
		// IsSafeRelativeContentPath folds backslashes into '/' before it checks anything, which is the
		// right call for a content root authored on Windows. An icon path is used both as a filesystem
		// path and as a `.mod` table-of-contents lookup key, and those two spellings must not be
		// allowed to diverge, so the backslash form is refused outright rather than rewritten.
		if (InPath.Contains(TEXT("\\"), ESearchCase::CaseSensitive))
		{
			OutReason = TEXT("it contains a backslash; icon paths are written with forward slashes");
			return false;
		}

		if (!IsSafeRelativeContentPath(InPath, OutReason))
		{
			return false;
		}

		if (FPaths::GetBaseFilename(InPath).IsEmpty())
		{
			OutReason = TEXT("it does not name a file");
			return false;
		}

		// Only the two formats IImageWrapperModule::DetectImageFormat is allowed to report back for a
		// mod icon. The extension is a first filter for the author's benefit; the bytes are sniffed
		// again before anything is decoded, because an extension proves nothing.
		const FString Extension = FPaths::GetExtension(InPath).ToLower();
		if (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg"))
		{
			OutReason = Extension.IsEmpty()
				? FString(TEXT("it has no file extension; an icon must be a .png, .jpg or .jpeg file"))
				: FString::Printf(TEXT("'.%s' is not a supported icon format; use .png, .jpg or .jpeg"), *Extension);
			return false;
		}

		return true;
	}

	/** A mount point must look like "/Word/" so it can serve as an Unreal package root. */
	bool IsValidMountPoint(const FString& In, FString& OutReason)
	{
		if (In.Len() < 3)
		{
			OutReason = TEXT("it is too short");
			return false;
		}

		if (In[0] != TEXT('/') || In[In.Len() - 1] != TEXT('/'))
		{
			OutReason = TEXT("it must start and end with '/'");
			return false;
		}

		const FString Inner = In.Mid(1, In.Len() - 2);
		if (Inner.IsEmpty())
		{
			OutReason = TEXT("it has no root name");
			return false;
		}

		if (!FChar::IsAlpha(Inner[0]) && Inner[0] != TEXT('_'))
		{
			OutReason = TEXT("the root name must start with a letter or an underscore");
			return false;
		}

		for (const TCHAR Character : Inner)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
			{
				OutReason = TEXT("the root name may only contain letters, digits and underscores");
				return false;
			}
		}

		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	// Readers for the nested objects of the schema.

	/**
	 * Reads a version constraint.
	 *
	 * A malformed expression is deliberately NOT reported here: FModVersionRange keeps the author's
	 * text, so ValidateManifest can report it once with the exact field name whether the manifest
	 * came from JSON or was built in code.
	 */
	void ReadVersionRange(const FJsonObject& Object, const TCHAR* Key, const FString& FieldPath, const FContext& Ctx, FModVersionRange& Out)
	{
		FString Text;
		if (ReadStringField(Object, Key, FieldPath, Ctx, Text) != EFieldStatus::Present)
		{
			Out = FModVersionRange::Any();
			return;
		}

		FModVersionRange::Parse(Text, Out);
	}

	void ReadGame(const FJsonObject& Root, const FContext& Ctx, FModGameRequirement& Out)
	{
		Out.VersionRange = FModVersionRange::Any();

		TSharedPtr<FJsonObject> Game;
		if (ReadObjectField(Root, Keys::Game, Keys::Game, Ctx, Game) != EFieldStatus::Present)
		{
			return;
		}

		ReadStringField(*Game, Keys::Id, JoinFieldPath(Keys::Game, Keys::Id), Ctx, Out.GameId);
		ReadVersionRange(*Game, Keys::Version, JoinFieldPath(Keys::Game, Keys::Version), Ctx, Out.VersionRange);

		static const TCHAR* const KnownKeys[] = { Keys::Id, Keys::Version };
		WarnUnknownFields(*Game, MakeArrayView(KnownKeys), Keys::Game, Ctx);
	}

	void ReadFramework(const FJsonObject& Root, const FContext& Ctx, FModVersionRange& Out)
	{
		Out = FModVersionRange::Any();

		TSharedPtr<FJsonObject> Framework;
		if (ReadObjectField(Root, Keys::Framework, Keys::Framework, Ctx, Framework) != EFieldStatus::Present)
		{
			return;
		}

		ReadVersionRange(*Framework, Keys::Version, JoinFieldPath(Keys::Framework, Keys::Version), Ctx, Out);

		static const TCHAR* const KnownKeys[] = { Keys::Version };
		WarnUnknownFields(*Framework, MakeArrayView(KnownKeys), Keys::Framework, Ctx);
	}

	void ReadSdk(const FJsonObject& Root, const FContext& Ctx, FModSdkRequirement& Out)
	{
		Out.VersionRange = FModVersionRange::Any();

		TSharedPtr<FJsonObject> Sdk;
		if (ReadObjectField(Root, Keys::Sdk, Keys::Sdk, Ctx, Sdk) != EFieldStatus::Present)
		{
			return;
		}

		ReadStringField(*Sdk, Keys::Id, JoinFieldPath(Keys::Sdk, Keys::Id), Ctx, Out.SdkId);
		ReadVersionRange(*Sdk, Keys::Version, JoinFieldPath(Keys::Sdk, Keys::Version), Ctx, Out.VersionRange);

		static const TCHAR* const KnownKeys[] = { Keys::Id, Keys::Version };
		WarnUnknownFields(*Sdk, MakeArrayView(KnownKeys), Keys::Sdk, Ctx);
	}

	void ReadNetwork(const FJsonObject& Root, const FContext& Ctx, EModNetworkScope& OutScope, bool& bOutRequired)
	{
		TSharedPtr<FJsonObject> Network;
		if (ReadObjectField(Root, Keys::Network, Keys::Network, Ctx, Network) != EFieldStatus::Present)
		{
			return;
		}

		const FString ScopePath = JoinFieldPath(Keys::Network, Keys::Scope);
		FString ScopeText;
		if (ReadStringField(*Network, Keys::Scope, ScopePath, Ctx, ScopeText) == EFieldStatus::Present)
		{
			EModNetworkScope Scope = EModNetworkScope::ClientAndServer;
			if (ModFrameworkEnums::ParseNetworkScope(ScopeText, Scope))
			{
				OutScope = Scope;
			}
			else
			{
				Ctx.Error(Codes::InvalidField, ScopePath,
					FString::Printf(TEXT("'%s' is not a network scope. Expected ClientAndServer, ClientOnly or ServerOnly."), *ScopeText));
			}
		}

		ReadBoolField(*Network, Keys::RequiredForNetworkPlay, JoinFieldPath(Keys::Network, Keys::RequiredForNetworkPlay), Ctx, bOutRequired);

		static const TCHAR* const KnownKeys[] = { Keys::Scope, Keys::RequiredForNetworkPlay };
		WarnUnknownFields(*Network, MakeArrayView(KnownKeys), Keys::Network, Ctx);
	}

	void ReadEntryPoint(const FJsonObject& Root, const FContext& Ctx, FModEntryPoint& Out)
	{
		TSharedPtr<FJsonObject> EntryPoint;
		if (ReadObjectField(Root, Keys::EntryPoint, Keys::EntryPoint, Ctx, EntryPoint) != EFieldStatus::Present)
		{
			return;
		}

		ReadStringField(*EntryPoint, Keys::NativeModule, JoinFieldPath(Keys::EntryPoint, Keys::NativeModule), Ctx, Out.NativeModule);

		const FString ClassPath = JoinFieldPath(Keys::EntryPoint, Keys::Class);
		FString ClassText;
		if (ReadStringField(*EntryPoint, Keys::Class, ClassPath, Ctx, ClassText) == EFieldStatus::Present)
		{
			FSoftObjectPath Parsed;
			if (TryMakeObjectPath(ClassText, ClassPath, Ctx, Parsed))
			{
				Out.EntryClass = FSoftClassPath(Parsed.ToString());
			}
		}

		const FString BundlesPath = JoinFieldPath(Keys::EntryPoint, Keys::ContentBundles);
		const TArray<TSharedPtr<FJsonValue>>* BundleValues = nullptr;
		if (ReadArrayField(*EntryPoint, Keys::ContentBundles, BundlesPath, Ctx, BundleValues) == EFieldStatus::Present)
		{
			Out.ContentBundles.Reset();
			Out.ContentBundles.Reserve(BundleValues->Num());

			for (int32 Index = 0; Index < BundleValues->Num(); ++Index)
			{
				const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *BundlesPath, Index);

				FString BundleText;
				if (!TryGetStringElement(*BundleValues, Index, ElementPath, Ctx, BundleText))
				{
					continue;
				}

				if (BundleText.TrimStartAndEnd().IsEmpty())
				{
					Ctx.Error(Codes::InvalidEntryPoint, ElementPath,
						FString::Printf(TEXT("Content bundle '%s' is empty."), *ElementPath));
					continue;
				}

				FSoftObjectPath Bundle;
				if (TryMakeObjectPath(BundleText, ElementPath, Ctx, Bundle))
				{
					Out.ContentBundles.Add(Bundle);
				}
			}
		}

		static const TCHAR* const KnownKeys[] = { Keys::NativeModule, Keys::Class, Keys::ContentBundles };
		WarnUnknownFields(*EntryPoint, MakeArrayView(KnownKeys), Keys::EntryPoint, Ctx);
	}

	void ReadDependencies(const FJsonObject& Root, const FContext& Ctx, TArray<FModDependency>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (ReadArrayField(Root, Keys::Dependencies, Keys::Dependencies, Ctx, Values) != EFieldStatus::Present)
		{
			return;
		}

		Out.Reset();
		Out.Reserve(Values->Num());

		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Element = (*Values)[Index];
			const FString ElementPath = MakeIndexedFieldPath(Keys::Dependencies, Index);

			if (!Element.IsValid() || Element->Type != EJson::Object)
			{
				ReportWrongType(Ctx, ElementPath, TEXT("an object"), Element.IsValid() ? Element->Type : EJson::None);
				continue;
			}

			const TSharedPtr<FJsonObject> Entry = Element->AsObject();
			if (!Entry.IsValid())
			{
				ReportWrongType(Ctx, ElementPath, TEXT("an object"), EJson::None);
				continue;
			}

			FModDependency Dependency;
			Dependency.VersionRange = FModVersionRange::Any();

			const FString IdPath = JoinFieldPath(ElementPath, Keys::Id);
			FString IdText;
			if (ReadStringField(*Entry, Keys::Id, IdPath, Ctx, IdText) == EFieldStatus::Present)
			{
				TryMakeModId(IdText, IdPath, Ctx, Dependency.Id);
			}
			else if (!Entry->HasField(Keys::Id))
			{
				Ctx.Error(Codes::MissingField, IdPath,
					FString::Printf(TEXT("Dependency '%s' has no 'id'."), *ElementPath));
			}

			ReadVersionRange(*Entry, Keys::Version, JoinFieldPath(ElementPath, Keys::Version), Ctx, Dependency.VersionRange);
			ReadBoolField(*Entry, Keys::Optional, JoinFieldPath(ElementPath, Keys::Optional), Ctx, Dependency.bOptional);
			ReadStringField(*Entry, Keys::Reason, JoinFieldPath(ElementPath, Keys::Reason), Ctx, Dependency.Reason);

			static const TCHAR* const KnownKeys[] = { Keys::Id, Keys::Version, Keys::Optional, Keys::Reason };
			WarnUnknownFields(*Entry, MakeArrayView(KnownKeys), ElementPath, Ctx);

			Out.Add(MoveTemp(Dependency));
		}
	}

	void ReadModIdArray(const FJsonObject& Root, const TCHAR* Key, const FContext& Ctx, TArray<FModId>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (ReadArrayField(Root, Key, Key, Ctx, Values) != EFieldStatus::Present)
		{
			return;
		}

		Out.Reset();
		Out.Reserve(Values->Num());

		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const FString ElementPath = MakeIndexedFieldPath(Key, Index);

			FString Text;
			if (!TryGetStringElement(*Values, Index, ElementPath, Ctx, Text))
			{
				continue;
			}

			FModId Id;
			if (TryMakeModId(Text, ElementPath, Ctx, Id))
			{
				Out.Add(Id);
			}
		}
	}

	void ReadPermissions(const FJsonObject& Root, const FContext& Ctx, TArray<FName>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (ReadArrayField(Root, Keys::Permissions, Keys::Permissions, Ctx, Values) != EFieldStatus::Present)
		{
			return;
		}

		Out.Reset();
		Out.Reserve(Values->Num());

		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const FString ElementPath = MakeIndexedFieldPath(Keys::Permissions, Index);

			FString Text;
			if (!TryGetStringElement(*Values, Index, ElementPath, Ctx, Text))
			{
				continue;
			}

			FName Permission;
			if (TryMakeName(Text, ElementPath, Codes::InvalidPermission, Ctx, Permission))
			{
				Out.Add(Permission);
			}
		}
	}

	void ReadContentRoots(const FJsonObject& Root, const FContext& Ctx, TArray<FModContentRoot>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (ReadArrayField(Root, Keys::Content, Keys::Content, Ctx, Values) != EFieldStatus::Present)
		{
			return;
		}

		Out.Reset();
		Out.Reserve(Values->Num());

		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Element = (*Values)[Index];
			const FString ElementPath = MakeIndexedFieldPath(Keys::Content, Index);

			if (!Element.IsValid() || Element->Type != EJson::Object)
			{
				ReportWrongType(Ctx, ElementPath, TEXT("an object"), Element.IsValid() ? Element->Type : EJson::None);
				continue;
			}

			const TSharedPtr<FJsonObject> Entry = Element->AsObject();
			if (!Entry.IsValid())
			{
				ReportWrongType(Ctx, ElementPath, TEXT("an object"), EJson::None);
				continue;
			}

			FModContentRoot ContentRoot;

			const FString PathFieldPath = JoinFieldPath(ElementPath, Keys::Path);
			if (ReadStringField(*Entry, Keys::Path, PathFieldPath, Ctx, ContentRoot.RelativePath) != EFieldStatus::Present
				&& !Entry->HasField(Keys::Path))
			{
				Ctx.Error(Codes::MissingField, PathFieldPath,
					FString::Printf(TEXT("Content root '%s' has no 'path'."), *ElementPath));
			}

			const FString TypePath = JoinFieldPath(ElementPath, Keys::Type);
			FString TypeText;
			if (ReadStringField(*Entry, Keys::Type, TypePath, Ctx, TypeText) == EFieldStatus::Present)
			{
				EModContentRootType Type = EModContentRootType::Pak;
				if (ModFrameworkEnums::ParseContentRootType(TypeText, Type))
				{
					ContentRoot.Type = Type;
				}
				else
				{
					Ctx.Error(Codes::InvalidField, TypePath,
						FString::Printf(TEXT("'%s' is not a content root type. Expected Pak, IoStore or LooseDirectory."), *TypeText));
				}
			}

			ReadStringField(*Entry, Keys::MountPoint, JoinFieldPath(ElementPath, Keys::MountPoint), Ctx, ContentRoot.MountPoint);
			ReadIntField(*Entry, Keys::MountOrder, JoinFieldPath(ElementPath, Keys::MountOrder), Ctx, ContentRoot.MountOrder);

			static const TCHAR* const KnownKeys[] = { Keys::Path, Keys::Type, Keys::MountPoint, Keys::MountOrder };
			WarnUnknownFields(*Entry, MakeArrayView(KnownKeys), ElementPath, Ctx);

			Out.Add(MoveTemp(ContentRoot));
		}
	}

	void ReadClaims(const FJsonObject& Root, const FContext& Ctx, TArray<FModResourceClaimDeclaration>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (ReadArrayField(Root, Keys::Claims, Keys::Claims, Ctx, Values) != EFieldStatus::Present)
		{
			return;
		}

		Out.Reset();
		Out.Reserve(Values->Num());

		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Element = (*Values)[Index];
			const FString ElementPath = MakeIndexedFieldPath(Keys::Claims, Index);

			if (!Element.IsValid() || Element->Type != EJson::Object)
			{
				ReportWrongType(Ctx, ElementPath, TEXT("an object"), Element.IsValid() ? Element->Type : EJson::None);
				continue;
			}

			const TSharedPtr<FJsonObject> Entry = Element->AsObject();
			if (!Entry.IsValid())
			{
				ReportWrongType(Ctx, ElementPath, TEXT("an object"), EJson::None);
				continue;
			}

			FModResourceClaimDeclaration Claim;

			const FString ExtensionPointPath = JoinFieldPath(ElementPath, Keys::ExtensionPoint);
			FString ExtensionPointText;
			if (ReadStringField(*Entry, Keys::ExtensionPoint, ExtensionPointPath, Ctx, ExtensionPointText) == EFieldStatus::Present)
			{
				TryMakeName(ExtensionPointText, ExtensionPointPath, Codes::InvalidClaim, Ctx, Claim.ExtensionPointId);
			}
			else if (!Entry->HasField(Keys::ExtensionPoint))
			{
				Ctx.Error(Codes::MissingField, ExtensionPointPath,
					FString::Printf(TEXT("Claim '%s' has no 'extensionPoint'."), *ElementPath));
			}

			const FString ResourcePath = JoinFieldPath(ElementPath, Keys::Resource);
			FString ResourceText;
			if (ReadStringField(*Entry, Keys::Resource, ResourcePath, Ctx, ResourceText) == EFieldStatus::Present)
			{
				TryMakeName(ResourceText, ResourcePath, Codes::InvalidClaim, Ctx, Claim.ResourceId);
			}
			else if (!Entry->HasField(Keys::Resource))
			{
				Ctx.Error(Codes::MissingField, ResourcePath,
					FString::Printf(TEXT("Claim '%s' has no 'resource'."), *ElementPath));
			}

			const FString PolicyPath = JoinFieldPath(ElementPath, Keys::Policy);
			FString PolicyText;
			if (ReadStringField(*Entry, Keys::Policy, PolicyPath, Ctx, PolicyText) == EFieldStatus::Present)
			{
				EModConflictPolicy Policy = EModConflictPolicy::Error;
				if (ModFrameworkEnums::ParseConflictPolicy(PolicyText, Policy))
				{
					Claim.PreferredPolicy = Policy;
				}
				else
				{
					Ctx.Error(Codes::InvalidField, PolicyPath,
						FString::Printf(TEXT("'%s' is not a conflict policy. Expected Error, FirstWins, LastWins, Priority or Merge."), *PolicyText));
				}
			}

			static const TCHAR* const KnownKeys[] = { Keys::ExtensionPoint, Keys::Resource, Keys::Policy };
			WarnUnknownFields(*Entry, MakeArrayView(KnownKeys), ElementPath, Ctx);

			Out.Add(MoveTemp(Claim));
		}
	}

	void ReadMetadata(const FJsonObject& Root, const FContext& Ctx, TMap<FString, FString>& Out)
	{
		TSharedPtr<FJsonObject> Metadata;
		if (ReadObjectField(Root, Keys::Metadata, Keys::Metadata, Ctx, Metadata) != EFieldStatus::Present)
		{
			return;
		}

		Out.Reset();

		// Metadata is deliberately free form, so no key is ever "unknown" here; only the value type
		// is constrained.
		for (const auto& Pair : Metadata->Values)
		{
			const FString Key(*Pair.Key);
			const FString FieldPath = JoinFieldPath(Keys::Metadata, Key);

			if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Null)
			{
				continue;
			}

			if (Pair.Value->Type != EJson::String)
			{
				ReportWrongType(Ctx, FieldPath, TEXT("a string"), Pair.Value->Type);
				continue;
			}

			FString Value;
			Pair.Value->TryGetString(Value);
			Out.Add(Key, MoveTemp(Value));
		}
	}

	void ReadManifestObject(const FJsonObject& Root, const FContext& Ctx, FModManifest& Out)
	{
		ReadIntField(Root, Keys::ManifestVersion, Keys::ManifestVersion, Ctx, Out.ManifestVersion);

		const FString IdPath(Keys::Id);
		FString IdText;
		if (ReadStringField(Root, Keys::Id, IdPath, Ctx, IdText) == EFieldStatus::Present)
		{
			TryMakeModId(IdText, IdPath, Ctx, Out.Id);
		}

		ReadStringField(Root, Keys::Name, Keys::Name, Ctx, Out.DisplayName);
		ReadStringField(Root, Keys::Description, Keys::Description, Ctx, Out.Description);
		ReadStringField(Root, Keys::Author, Keys::Author, Ctx, Out.Author);
		ReadStringField(Root, Keys::Homepage, Keys::Homepage, Ctx, Out.Homepage);
		ReadStringField(Root, Keys::License, Keys::License, Ctx, Out.License);
		ReadStringField(Root, Keys::Icon, Keys::Icon, Ctx, Out.IconPath);

		const FString VersionPath(Keys::Version);
		FString VersionText;
		if (ReadStringField(Root, Keys::Version, VersionPath, Ctx, VersionText) == EFieldStatus::Present)
		{
			FString Error;
			FModVersion Version;

			// The mod's own version is the number it publishes under, so partial forms such as "2.1"
			// are refused here even though version *constraints* still accept them.
			if (FModVersion::Parse(VersionText, Version, &Error, /*bAllowPartial=*/false))
			{
				Out.Version = Version;
			}
			else
			{
				Ctx.Error(Codes::InvalidVersion, VersionPath,
					FString::Printf(TEXT("'%s' is not a semantic version: %s"), *VersionText, *Error));
			}
		}

		ReadGame(Root, Ctx, Out.Game);
		ReadFramework(Root, Ctx, Out.FrameworkVersionRange);
		ReadSdk(Root, Ctx, Out.Sdk);
		ReadDependencies(Root, Ctx, Out.Dependencies);
		ReadModIdArray(Root, Keys::LoadBefore, Ctx, Out.LoadBefore);
		ReadModIdArray(Root, Keys::LoadAfter, Ctx, Out.LoadAfter);
		ReadIntField(Root, Keys::Priority, Keys::Priority, Ctx, Out.Priority);
		ReadNetwork(Root, Ctx, Out.NetworkScope, Out.bRequiredForNetworkPlay);
		ReadPermissions(Root, Ctx, Out.RequestedPermissions);
		ReadEntryPoint(Root, Ctx, Out.EntryPoint);
		ReadContentRoots(Root, Ctx, Out.ContentRoots);
		ReadClaims(Root, Ctx, Out.Claims);
		ReadMetadata(Root, Ctx, Out.Metadata);

		static const TCHAR* const KnownKeys[] =
		{
			Keys::ManifestVersion, Keys::Id, Keys::Name, Keys::Description, Keys::Author,
			Keys::Homepage, Keys::License, Keys::Icon, Keys::Version, Keys::Game, Keys::Framework,
			Keys::Sdk, Keys::Dependencies, Keys::LoadBefore, Keys::LoadAfter, Keys::Priority,
			Keys::Network, Keys::Permissions, Keys::EntryPoint, Keys::Content, Keys::Claims,
			Keys::Metadata
		};
		WarnUnknownFields(Root, MakeArrayView(KnownKeys), FString(), Ctx);
	}

	//////////////////////////////////////////////////////////////////////////
	// Writer.

	TSharedPtr<FJsonObject> BuildJsonObject(const FModManifest& In)
	{
		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

		// Always written: it is the schema discriminator a reader needs before anything else.
		Root->SetNumberField(Keys::ManifestVersion, static_cast<double>(In.ManifestVersion));

		// The four required fields are always written, even when empty, so a broken manifest still
		// round-trips into the same broken manifest instead of silently changing shape.
		Root->SetStringField(Keys::Id, In.Id.ToString());
		Root->SetStringField(Keys::Name, In.DisplayName);

		if (!In.Description.IsEmpty())
		{
			Root->SetStringField(Keys::Description, In.Description);
		}
		if (!In.Author.IsEmpty())
		{
			Root->SetStringField(Keys::Author, In.Author);
		}
		if (!In.Homepage.IsEmpty())
		{
			Root->SetStringField(Keys::Homepage, In.Homepage);
		}
		if (!In.License.IsEmpty())
		{
			Root->SetStringField(Keys::License, In.License);
		}
		if (!In.IconPath.IsEmpty())
		{
			Root->SetStringField(Keys::Icon, In.IconPath);
		}

		Root->SetStringField(Keys::Version, In.Version.ToString());

		{
			const TSharedPtr<FJsonObject> Game = MakeShared<FJsonObject>();
			Game->SetStringField(Keys::Id, In.Game.GameId);
			if (!In.Game.VersionRange.IsAny())
			{
				Game->SetStringField(Keys::Version, In.Game.VersionRange.Expression);
			}
			Root->SetObjectField(Keys::Game, Game);
		}

		if (!In.FrameworkVersionRange.IsAny())
		{
			const TSharedPtr<FJsonObject> Framework = MakeShared<FJsonObject>();
			Framework->SetStringField(Keys::Version, In.FrameworkVersionRange.Expression);
			Root->SetObjectField(Keys::Framework, Framework);
		}

		if (!In.Sdk.SdkId.IsEmpty() || !In.Sdk.VersionRange.IsAny())
		{
			const TSharedPtr<FJsonObject> Sdk = MakeShared<FJsonObject>();
			if (!In.Sdk.SdkId.IsEmpty())
			{
				Sdk->SetStringField(Keys::Id, In.Sdk.SdkId);
			}
			if (!In.Sdk.VersionRange.IsAny())
			{
				Sdk->SetStringField(Keys::Version, In.Sdk.VersionRange.Expression);
			}
			Root->SetObjectField(Keys::Sdk, Sdk);
		}

		if (In.Dependencies.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(In.Dependencies.Num());

			for (const FModDependency& Dependency : In.Dependencies)
			{
				const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(Keys::Id, Dependency.Id.ToString());

				// A dependency's constraint is the load-bearing part of the entry, so it is written
				// even when it accepts everything.
				Entry->SetStringField(Keys::Version, Dependency.VersionRange.Expression);

				if (Dependency.bOptional)
				{
					Entry->SetBoolField(Keys::Optional, true);
				}
				if (!Dependency.Reason.IsEmpty())
				{
					Entry->SetStringField(Keys::Reason, Dependency.Reason);
				}

				Values.Add(MakeShared<FJsonValueObject>(Entry));
			}

			Root->SetArrayField(Keys::Dependencies, MoveTemp(Values));
		}

		if (In.LoadBefore.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(In.LoadBefore.Num());
			for (const FModId& Id : In.LoadBefore)
			{
				Values.Add(MakeShared<FJsonValueString>(Id.ToString()));
			}
			Root->SetArrayField(Keys::LoadBefore, MoveTemp(Values));
		}

		if (In.LoadAfter.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(In.LoadAfter.Num());
			for (const FModId& Id : In.LoadAfter)
			{
				Values.Add(MakeShared<FJsonValueString>(Id.ToString()));
			}
			Root->SetArrayField(Keys::LoadAfter, MoveTemp(Values));
		}

		if (In.Priority != 0)
		{
			Root->SetNumberField(Keys::Priority, static_cast<double>(In.Priority));
		}

		if (In.NetworkScope != EModNetworkScope::ClientAndServer || In.bRequiredForNetworkPlay)
		{
			const TSharedPtr<FJsonObject> Network = MakeShared<FJsonObject>();
			if (In.NetworkScope != EModNetworkScope::ClientAndServer)
			{
				Network->SetStringField(Keys::Scope, ModFrameworkEnums::ToString(In.NetworkScope));
			}
			if (In.bRequiredForNetworkPlay)
			{
				Network->SetBoolField(Keys::RequiredForNetworkPlay, true);
			}
			Root->SetObjectField(Keys::Network, Network);
		}

		if (In.RequestedPermissions.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(In.RequestedPermissions.Num());
			for (const FName& Permission : In.RequestedPermissions)
			{
				Values.Add(MakeShared<FJsonValueString>(Permission.ToString()));
			}
			Root->SetArrayField(Keys::Permissions, MoveTemp(Values));
		}

		if (!In.EntryPoint.NativeModule.IsEmpty() || !In.EntryPoint.EntryClass.IsNull() || In.EntryPoint.ContentBundles.Num() > 0)
		{
			const TSharedPtr<FJsonObject> EntryPoint = MakeShared<FJsonObject>();

			if (!In.EntryPoint.NativeModule.IsEmpty())
			{
				EntryPoint->SetStringField(Keys::NativeModule, In.EntryPoint.NativeModule);
			}
			if (!In.EntryPoint.EntryClass.IsNull())
			{
				EntryPoint->SetStringField(Keys::Class, In.EntryPoint.EntryClass.ToString());
			}
			if (In.EntryPoint.ContentBundles.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				Values.Reserve(In.EntryPoint.ContentBundles.Num());
				for (const FSoftObjectPath& Bundle : In.EntryPoint.ContentBundles)
				{
					Values.Add(MakeShared<FJsonValueString>(Bundle.ToString()));
				}
				EntryPoint->SetArrayField(Keys::ContentBundles, MoveTemp(Values));
			}

			Root->SetObjectField(Keys::EntryPoint, EntryPoint);
		}

		if (In.ContentRoots.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(In.ContentRoots.Num());

			for (const FModContentRoot& ContentRoot : In.ContentRoots)
			{
				const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(Keys::Path, ContentRoot.RelativePath);

				if (ContentRoot.Type != EModContentRootType::Pak)
				{
					Entry->SetStringField(Keys::Type, ModFrameworkEnums::ToString(ContentRoot.Type));
				}
				if (!ContentRoot.MountPoint.IsEmpty())
				{
					Entry->SetStringField(Keys::MountPoint, ContentRoot.MountPoint);
				}
				if (ContentRoot.MountOrder != 0)
				{
					Entry->SetNumberField(Keys::MountOrder, static_cast<double>(ContentRoot.MountOrder));
				}

				Values.Add(MakeShared<FJsonValueObject>(Entry));
			}

			Root->SetArrayField(Keys::Content, MoveTemp(Values));
		}

		if (In.Claims.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(In.Claims.Num());

			for (const FModResourceClaimDeclaration& Claim : In.Claims)
			{
				const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(Keys::ExtensionPoint, Claim.ExtensionPointId.ToString());
				Entry->SetStringField(Keys::Resource, Claim.ResourceId.ToString());

				if (Claim.PreferredPolicy != EModConflictPolicy::Error)
				{
					Entry->SetStringField(Keys::Policy, ModFrameworkEnums::ToString(Claim.PreferredPolicy));
				}

				Values.Add(MakeShared<FJsonValueObject>(Entry));
			}

			Root->SetArrayField(Keys::Claims, MoveTemp(Values));
		}

		if (In.Metadata.Num() > 0)
		{
			const TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();

			// Sorted so two runs over the same manifest produce byte-identical output.
			TArray<FString> MetadataKeys;
			In.Metadata.GetKeys(MetadataKeys);
			MetadataKeys.Sort();

			for (const FString& Key : MetadataKeys)
			{
				Metadata->SetStringField(Key, In.Metadata[Key]);
			}

			Root->SetObjectField(Keys::Metadata, Metadata);
		}

		return Root;
	}
}

//////////////////////////////////////////////////////////////////////////
// FModManifestParser

const TCHAR* FModManifestParser::GetManifestFileName()
{
	return TEXT("mod.json");
}

FModManifestParseResult FModManifestParser::ParseFromString(const FString& JsonText, const FString& ContextPath)
{
	using namespace ModManifestParserPrivate;

	FModManifestParseResult Result;

	FContext Ctx;
	Ctx.ContextPath = ContextPath;
	Ctx.Diagnostics = &Result.Diagnostics;

	if (JsonText.TrimStartAndEnd().IsEmpty())
	{
		Ctx.Error(Codes::InvalidJson, FString(), TEXT("The manifest is empty."));
		Result.bSuccess = false;
		return Result;
	}

	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		const FString ReaderError = Reader->GetErrorMessage();
		Ctx.Error(Codes::InvalidJson, FString(),
			ReaderError.IsEmpty()
				? FString(TEXT("The manifest is not a JSON object."))
				: FString::Printf(TEXT("The manifest is not valid JSON: %s"), *ReaderError));
		Result.bSuccess = false;
		return Result;
	}

	ReadManifestObject(*Root, Ctx, Result.Manifest);
	ValidateManifest(Result.Manifest, ContextPath, Result.Diagnostics);

	// The mod id is only known once the whole file has been read, so it is stamped onto the
	// diagnostics at the end rather than at the point each one was raised.
	StampModId(Result.Diagnostics, 0, Result.Manifest.Id);

	Result.bSuccess = !ModDiagnostics::HasErrors(Result.Diagnostics);
	return Result;
}

FModManifestParseResult FModManifestParser::ParseFromFile(const FString& AbsoluteFilePath)
{
	using namespace ModManifestParserPrivate;

	FModManifestParseResult Result;

	FContext Ctx;
	Ctx.ContextPath = AbsoluteFilePath;
	Ctx.Diagnostics = &Result.Diagnostics;

	if (AbsoluteFilePath.IsEmpty())
	{
		Ctx.Error(Codes::ReadFailed, FString(), TEXT("No manifest path was supplied."));
		return Result;
	}

	IFileManager& FileManager = IFileManager::Get();

	const int64 FileSize = FileManager.FileSize(*AbsoluteFilePath);
	if (FileSize < 0)
	{
		Ctx.Error(Codes::ReadFailed, FString(),
			FString::Printf(TEXT("Manifest file '%s' does not exist or cannot be opened."), *AbsoluteFilePath));
		return Result;
	}

	if (FileSize > MaxManifestFileSize)
	{
		Ctx.Error(Codes::ReadFailed, FString(),
			FString::Printf(TEXT("Manifest file '%s' is %lld bytes; the maximum is %lld."),
				*AbsoluteFilePath, FileSize, MaxManifestFileSize));
		return Result;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *AbsoluteFilePath))
	{
		Ctx.Error(Codes::ReadFailed, FString(),
			FString::Printf(TEXT("Manifest file '%s' could not be read."), *AbsoluteFilePath));
		return Result;
	}

	return ParseFromString(JsonText, AbsoluteFilePath);
}

bool FModManifestParser::SerializeToString(const FModManifest& In, FString& OutJson, bool bPrettyPrint)
{
	using namespace ModManifestParserPrivate;

	OutJson.Reset();

	const TSharedPtr<FJsonObject> Root = BuildJsonObject(In);

	if (bPrettyPrint)
	{
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Root, Writer);
	}

	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	return FJsonSerializer::Serialize(Root, Writer);
}

bool FModManifestParser::SerializeToFile(const FModManifest& In, const FString& AbsoluteFilePath)
{
	if (AbsoluteFilePath.IsEmpty())
	{
		return false;
	}

	FString Json;
	if (!SerializeToString(In, Json, /*bPrettyPrint=*/true))
	{
		return false;
	}

	const FString Directory = FPaths::GetPath(AbsoluteFilePath);
	if (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
	{
		IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);
	}

	// UTF-8 without a BOM: FFileHelper::LoadFileToString decodes plain UTF-8, and a BOM would be the
	// first character the JSON reader saw.
	return FFileHelper::SaveStringToFile(Json, *AbsoluteFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FModManifestParser::ValidateManifest(const FModManifest& In, const FString& ContextPath, TArray<FModDiagnostic>& OutDiagnostics)
{
	using namespace ModManifestParserPrivate;

	FContext Ctx;
	Ctx.ContextPath = ContextPath;
	Ctx.Diagnostics = &OutDiagnostics;

	// Diagnostics already in the array belong to the caller and are left untouched.
	const int32 FirstOwnDiagnostic = OutDiagnostics.Num();

	// --- Schema version ---------------------------------------------------
	if (In.ManifestVersion < 1)
	{
		Ctx.Error(Codes::InvalidField, Keys::ManifestVersion,
			FString::Printf(TEXT("'manifestVersion' is %d; it must be 1 or greater."), In.ManifestVersion));
	}
	else if (In.ManifestVersion > MODFRAMEWORK_MANIFEST_VERSION)
	{
		Ctx.Error(Codes::UnsupportedManifestVersion, Keys::ManifestVersion,
			FString::Printf(TEXT("This mod declares manifest version %d but this framework only understands up to %d. Update the game or the framework to load it."),
				In.ManifestVersion, MODFRAMEWORK_MANIFEST_VERSION));
	}

	// --- Identity ---------------------------------------------------------
	if (!In.Id.IsValid())
	{
		Ctx.Error(Codes::MissingField, Keys::Id, TEXT("The manifest has no usable 'id'."));
	}
	else
	{
		FString IdError;
		if (!FModId::IsValidIdString(In.Id.ToString(), IdError))
		{
			Ctx.Error(Codes::InvalidId, Keys::Id,
				FString::Printf(TEXT("Field 'id' is not a valid mod id: %s"), *IdError));
		}
	}

	if (In.DisplayName.TrimStartAndEnd().IsEmpty())
	{
		Ctx.Error(Codes::MissingField, Keys::Name, TEXT("Field 'name' is required and must not be empty."));
	}

	if (In.Version.IsZero())
	{
		Ctx.Error(Codes::InvalidVersion, Keys::Version,
			TEXT("Field 'version' is missing or is 0.0.0; a mod must publish a non-zero semantic version."));
	}

	// --- Environment requirements ----------------------------------------
	if (In.Game.GameId.TrimStartAndEnd().IsEmpty())
	{
		Ctx.Error(Codes::MissingField, JoinFieldPath(Keys::Game, Keys::Id),
			TEXT("Field 'game.id' is required: a mod must name the game it targets."));
	}

	if (!In.Game.VersionRange.IsValid())
	{
		Ctx.Error(Codes::InvalidVersionRange, JoinFieldPath(Keys::Game, Keys::Version),
			FString::Printf(TEXT("'%s' is not a valid version range."), *In.Game.VersionRange.Expression));
	}

	if (!In.FrameworkVersionRange.IsValid())
	{
		Ctx.Error(Codes::InvalidVersionRange, JoinFieldPath(Keys::Framework, Keys::Version),
			FString::Printf(TEXT("'%s' is not a valid version range."), *In.FrameworkVersionRange.Expression));
	}

	if (!In.Sdk.VersionRange.IsValid())
	{
		Ctx.Error(Codes::InvalidVersionRange, JoinFieldPath(Keys::Sdk, Keys::Version),
			FString::Printf(TEXT("'%s' is not a valid version range."), *In.Sdk.VersionRange.Expression));
	}

	// --- Dependencies -----------------------------------------------------
	{
		TSet<FModId> SeenDependencies;
		SeenDependencies.Reserve(In.Dependencies.Num());

		for (int32 Index = 0; Index < In.Dependencies.Num(); ++Index)
		{
			const FModDependency& Dependency = In.Dependencies[Index];
			const FString ElementPath = MakeIndexedFieldPath(Keys::Dependencies, Index);
			const FString IdPath = JoinFieldPath(ElementPath, Keys::Id);

			if (!Dependency.Id.IsValid())
			{
				Ctx.Error(Codes::InvalidId, IdPath,
					FString::Printf(TEXT("Dependency '%s' has no usable 'id'."), *ElementPath));
			}
			else
			{
				FString IdError;
				if (!FModId::IsValidIdString(Dependency.Id.ToString(), IdError))
				{
					Ctx.Error(Codes::InvalidId, IdPath,
						FString::Printf(TEXT("Dependency id '%s' is not valid: %s"), *Dependency.Id.ToString(), *IdError));
				}

				if (In.Id.IsValid() && Dependency.Id == In.Id)
				{
					Ctx.Error(Codes::SelfDependency, IdPath,
						FString::Printf(TEXT("Mod '%s' declares a dependency on itself."), *In.Id.ToString()));
				}
				else if (SeenDependencies.Contains(Dependency.Id))
				{
					Ctx.Error(Codes::DuplicateDependency, IdPath,
						FString::Printf(TEXT("Dependency '%s' is declared more than once."), *Dependency.Id.ToString()));
				}
				else
				{
					SeenDependencies.Add(Dependency.Id);
				}
			}

			if (!Dependency.VersionRange.IsValid())
			{
				Ctx.Error(Codes::InvalidVersionRange, JoinFieldPath(ElementPath, Keys::Version),
					FString::Printf(TEXT("'%s' is not a valid version range."), *Dependency.VersionRange.Expression));
			}
		}
	}

	// --- Ordering hints ---------------------------------------------------
	{
		const TCHAR* const OrderingKeys[] = { Keys::LoadBefore, Keys::LoadAfter };
		const TArray<FModId>* const OrderingArrays[] = { &In.LoadBefore, &In.LoadAfter };
		constexpr int32 NumOrderingArrays = 2;

		for (int32 ArrayIndex = 0; ArrayIndex < NumOrderingArrays; ++ArrayIndex)
		{
			const TCHAR* Key = OrderingKeys[ArrayIndex];
			const TArray<FModId>& Ids = *OrderingArrays[ArrayIndex];

			for (int32 Index = 0; Index < Ids.Num(); ++Index)
			{
				const FString ElementPath = MakeIndexedFieldPath(Key, Index);

				if (!Ids[Index].IsValid())
				{
					Ctx.Error(Codes::InvalidId, ElementPath,
						FString::Printf(TEXT("'%s' is empty; it must name a mod."), *ElementPath));
					continue;
				}

				FString IdError;
				if (!FModId::IsValidIdString(Ids[Index].ToString(), IdError))
				{
					Ctx.Error(Codes::InvalidId, ElementPath,
						FString::Printf(TEXT("'%s' is not a valid mod id: %s"), *Ids[Index].ToString(), *IdError));
				}

				if (In.Id.IsValid() && Ids[Index] == In.Id)
				{
					Ctx.Error(Codes::SelfDependency, ElementPath,
						FString::Printf(TEXT("Mod '%s' orders itself against itself in '%s'."), *In.Id.ToString(), Key));
				}
			}
		}
	}

	// --- Load order priority ---------------------------------------------
	if (In.Priority < MinPriority || In.Priority > MaxPriority)
	{
		Ctx.Error(Codes::InvalidField, Keys::Priority,
			FString::Printf(TEXT("'priority' is %d; it must be between %d and %d."), In.Priority, MinPriority, MaxPriority));
	}

	// --- Permissions ------------------------------------------------------
	for (int32 Index = 0; Index < In.RequestedPermissions.Num(); ++Index)
	{
		const FString ElementPath = MakeIndexedFieldPath(Keys::Permissions, Index);
		const FString Permission = In.RequestedPermissions[Index].IsNone()
			? FString()
			: In.RequestedPermissions[Index].ToString();

		if (Permission.IsEmpty())
		{
			Ctx.Error(Codes::InvalidPermission, ElementPath,
				FString::Printf(TEXT("'%s' is an empty permission name."), *ElementPath));
			continue;
		}

		if (ContainsWhitespace(Permission))
		{
			Ctx.Error(Codes::InvalidPermission, ElementPath,
				FString::Printf(TEXT("Permission '%s' contains whitespace; permission ids are dotted identifiers such as \"gameplay.modify\"."), *Permission));
		}
	}

	// --- Entry point ------------------------------------------------------
	if (!In.EntryPoint.NativeModule.IsEmpty() && !IsIdentifier(In.EntryPoint.NativeModule))
	{
		Ctx.Error(Codes::InvalidEntryPoint, JoinFieldPath(Keys::EntryPoint, Keys::NativeModule),
			FString::Printf(TEXT("'%s' is not a valid module name; expected letters, digits and underscores only."), *In.EntryPoint.NativeModule));
	}

	if (!In.EntryPoint.EntryClass.IsNull() && In.EntryPoint.EntryClass.GetAssetName().IsEmpty())
	{
		Ctx.Error(Codes::InvalidEntryPoint, JoinFieldPath(Keys::EntryPoint, Keys::Class),
			FString::Printf(TEXT("'%s' names a package but no class. Expected something like \"/MyMod/BP_Entry.BP_Entry_C\"."),
				*In.EntryPoint.EntryClass.ToString()));
	}

	{
		const FString BundlesPath = JoinFieldPath(Keys::EntryPoint, Keys::ContentBundles);
		for (int32 Index = 0; Index < In.EntryPoint.ContentBundles.Num(); ++Index)
		{
			const FSoftObjectPath& Bundle = In.EntryPoint.ContentBundles[Index];
			const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *BundlesPath, Index);

			if (Bundle.IsNull())
			{
				Ctx.Error(Codes::InvalidEntryPoint, ElementPath,
					FString::Printf(TEXT("'%s' is not a valid content bundle path."), *ElementPath));
			}
			else if (Bundle.GetAssetName().IsEmpty())
			{
				Ctx.Error(Codes::InvalidEntryPoint, ElementPath,
					FString::Printf(TEXT("'%s' names a package but no asset."), *Bundle.ToString()));
			}
		}
	}

	// --- Content roots ----------------------------------------------------
	for (int32 Index = 0; Index < In.ContentRoots.Num(); ++Index)
	{
		const FModContentRoot& ContentRoot = In.ContentRoots[Index];
		const FString ElementPath = MakeIndexedFieldPath(Keys::Content, Index);

		if (ContentRoot.RelativePath.TrimStartAndEnd().IsEmpty())
		{
			Ctx.Error(Codes::EmptyContent, JoinFieldPath(ElementPath, Keys::Path),
				FString::Printf(TEXT("Content root '%s' declares an empty path."), *ElementPath));
		}
		else
		{
			FString Reason;
			if (!IsSafeRelativeContentPath(ContentRoot.RelativePath, Reason))
			{
				Ctx.Error(Codes::InvalidContentRoot, JoinFieldPath(ElementPath, Keys::Path),
					FString::Printf(TEXT("Content path '%s' was rejected because %s. Content paths must stay inside the mod directory."),
						*ContentRoot.RelativePath, *Reason));
			}
		}

		if (!ContentRoot.MountPoint.IsEmpty())
		{
			FString Reason;
			if (!IsValidMountPoint(ContentRoot.MountPoint, Reason))
			{
				Ctx.Error(Codes::InvalidContentRoot, JoinFieldPath(ElementPath, Keys::MountPoint),
					FString::Printf(TEXT("Mount point '%s' was rejected because %s. It must have the form \"/MyMod/\"."),
						*ContentRoot.MountPoint, *Reason));
			}
		}
	}

	// --- Icon -------------------------------------------------------------
	// An absent or empty 'icon' is the normal case for a mod that ships none, and is silent. A bad
	// one is an error on the field, never on the mod: UModIconCache falls back to the default icon
	// and the mod still loads.
	if (!In.IconPath.IsEmpty())
	{
		const FString Icon = In.IconPath.TrimStartAndEnd();

		if (Icon.IsEmpty())
		{
			Ctx.Error(Codes::InvalidIcon, Keys::Icon,
				TEXT("Field 'icon' is present but blank. Leave it out entirely when the mod ships no icon."));
		}
		else
		{
			FString Reason;
			if (!IsSafeIconPath(Icon, Reason))
			{
				Ctx.Error(Codes::InvalidIcon, Keys::Icon,
					FString::Printf(TEXT("Icon path '%s' was rejected because %s. An icon is a .png, .jpg or .jpeg file inside the mod directory, such as \"Icon.png\"."),
						*In.IconPath, *Reason));
			}
		}
	}

	// --- Resource claims --------------------------------------------------
	for (int32 Index = 0; Index < In.Claims.Num(); ++Index)
	{
		const FModResourceClaimDeclaration& Claim = In.Claims[Index];
		const FString ElementPath = MakeIndexedFieldPath(Keys::Claims, Index);

		const FString ExtensionPoint = Claim.ExtensionPointId.IsNone() ? FString() : Claim.ExtensionPointId.ToString();
		if (ExtensionPoint.IsEmpty())
		{
			Ctx.Error(Codes::InvalidClaim, JoinFieldPath(ElementPath, Keys::ExtensionPoint),
				FString::Printf(TEXT("Claim '%s' has an empty 'extensionPoint'."), *ElementPath));
		}
		else if (ContainsWhitespace(ExtensionPoint))
		{
			Ctx.Error(Codes::InvalidClaim, JoinFieldPath(ElementPath, Keys::ExtensionPoint),
				FString::Printf(TEXT("Extension point id '%s' contains whitespace."), *ExtensionPoint));
		}

		const FString Resource = Claim.ResourceId.IsNone() ? FString() : Claim.ResourceId.ToString();
		if (Resource.IsEmpty())
		{
			Ctx.Error(Codes::InvalidClaim, JoinFieldPath(ElementPath, Keys::Resource),
				FString::Printf(TEXT("Claim '%s' has an empty 'resource'."), *ElementPath));
		}
		else if (ContainsWhitespace(Resource))
		{
			Ctx.Error(Codes::InvalidClaim, JoinFieldPath(ElementPath, Keys::Resource),
				FString::Printf(TEXT("Resource id '%s' contains whitespace."), *Resource));
		}
	}

	StampModId(OutDiagnostics, FirstOwnDiagnostic, In.Id);
}
