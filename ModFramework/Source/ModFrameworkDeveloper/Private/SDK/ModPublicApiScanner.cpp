// Copyright (c) 2026. Licensed for use in your own projects.

#include "SDK/ModPublicApiScanner.h"

#include "Algo/Sort.h"
#include "API/ModAPI.h"
#include "Containers/Map.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Extensions/ModExtension.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Manifest/ModVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModFrameworkDeveloperModule.h"
#include "ModuleDescriptor.h"
#include "Modules/ModuleManager.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "ProjectDescriptor.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Templates/Casts.h"
#include "Templates/SharedPointer.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace ModPublicApiMetadata
{
	const TCHAR* const ModPublic = TEXT("ModPublic");
	const TCHAR* const ModApiId = TEXT("ModApiId");
	const TCHAR* const ModApiVersion = TEXT("ModApiVersion");
	const TCHAR* const ModApiPermissions = TEXT("ModApiPermissions");
	const TCHAR* const ModApiServerAuthoritative = TEXT("ModApiServerAuthoritative");
	const TCHAR* const ModExtensionPoint = TEXT("ModExtensionPoint");
	const TCHAR* const ModSince = TEXT("ModSince");
	const TCHAR* const ModDeprecated = TEXT("ModDeprecated");
	const TCHAR* const ModuleRelativePath = TEXT("ModuleRelativePath");
	const TCHAR* const IncludePath = TEXT("IncludePath");
}

namespace ModPublicApiCodes
{
	const TCHAR* const MetadataUnavailable = TEXT("Sdk.MetadataUnavailable");
	const TCHAR* const ApiMissingId = TEXT("Sdk.ApiMissingId");
	const TCHAR* const ApiIdentityMismatch = TEXT("Sdk.ApiIdentityMismatch");
	const TCHAR* const ApiIdentityNotNative = TEXT("Sdk.ApiIdentityNotNative");
	const TCHAR* const InvalidVersion = TEXT("Sdk.InvalidVersion");
	const TCHAR* const ExtensionMissingPoint = TEXT("Sdk.ExtensionMissingPoint");
	const TCHAR* const NoMarkedMembers = TEXT("Sdk.NoMarkedMembers");
	const TCHAR* const DuplicateApiId = TEXT("Sdk.DuplicateApiId");
	const TCHAR* const UnmarkedTypeLeak = TEXT("Sdk.UnmarkedTypeLeak");
	const TCHAR* const ReportWriteFailed = TEXT("Sdk.ReportWriteFailed");
}

namespace ModPublicApiScannerPrivate
{
	/** The framework plugin always ships inside a generated bundle, so its types are never a leak. */
	const TCHAR* const FrameworkPluginName = TEXT("ModFramework");

	/** Guard against a pathological delegate signature that refers back into itself. */
	constexpr int32 MaxReferenceDepth = 8;

	/**
	 * Deliberately not named MakeError. Templates/ValueOrError.h declares a global variadic
	 * MakeError(ArgTypes&&...) that is an exact match for any argument list, and at any call site
	 * reached through a using-directive the engine template wins overload resolution and produces an
	 * unreadable TValueOrError_ErrorProxy diagnostic.
	 */
	FModDiagnostic MakeScanDiagnostic(EModDiagnosticSeverity InSeverity, const TCHAR* InCode,
		FString InMessage, FString InContext = FString())
	{
		return FModDiagnostic(InSeverity, FName(InCode), MoveTemp(InMessage), MoveTemp(InContext));
	}

	/** "true"/"yes"/"1" - the spellings UHT lets an author write into a metadata value. */
	bool ParseMetadataBool(const FString& InValue)
	{
		const FString Trimmed = InValue.TrimStartAndEnd();
		return Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("1"));
	}

	/** Splits a comma separated metadata list, trimming and dropping empties. */
	TArray<FString> SplitMetadataList(const FString& InValue)
	{
		TArray<FString> Raw;
		InValue.ParseIntoArray(Raw, TEXT(","), /*InCullEmpty*/ true);

		TArray<FString> Result;
		Result.Reserve(Raw.Num());
		for (FString& Entry : Raw)
		{
			Entry.TrimStartAndEndInline();
			if (!Entry.IsEmpty())
			{
				Result.Add(MoveTemp(Entry));
			}
		}
		return Result;
	}

	/** The full C++ spelling of a property's type, template arguments included. */
	FString RenderCppType(const FProperty* InProperty)
	{
		if (!InProperty)
		{
			return FString();
		}

		FString Extended;
		FString Base = InProperty->GetCPPType(&Extended, /*CPPExportFlags*/ 0);
		return Base + Extended;
	}

	void CollectReferencedFields(const FProperty* InProperty, TArray<const UField*>& OutFields, int32 InDepth);

	/** Every reflected type named by a function's parameter list, including its return value. */
	void CollectFunctionReferencedFields(const UFunction* InFunction, TArray<const UField*>& OutFields, int32 InDepth)
	{
		if (!InFunction || InDepth > MaxReferenceDepth)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(InFunction, EFieldIterationFlags::None); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm))
			{
				CollectReferencedFields(*It, OutFields, InDepth + 1);
			}
		}
	}

	/**
	 * Every reflected type named anywhere in this property's type, containers unwrapped.
	 *
	 * Order matters. FClassProperty and FSoftClassProperty both derive from FObjectPropertyBase, so
	 * an FObjectPropertyBase test placed first would swallow them and report UClass instead of the
	 * metaclass - and the metaclass is the type a mod author actually needs a header for.
	 */
	void CollectReferencedFields(const FProperty* InProperty, TArray<const UField*>& OutFields, int32 InDepth)
	{
		if (!InProperty || InDepth > MaxReferenceDepth)
		{
			return;
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(InProperty))
		{
			CollectReferencedFields(ArrayProperty->Inner, OutFields, InDepth + 1);
			return;
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(InProperty))
		{
			CollectReferencedFields(SetProperty->ElementProp, OutFields, InDepth + 1);
			return;
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(InProperty))
		{
			CollectReferencedFields(MapProperty->KeyProp, OutFields, InDepth + 1);
			CollectReferencedFields(MapProperty->ValueProp, OutFields, InDepth + 1);
			return;
		}
		if (const FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(InProperty))
		{
			CollectReferencedFields(OptionalProperty->GetValueProperty(), OutFields, InDepth + 1);
			return;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(InProperty))
		{
			OutFields.Add(StructProperty->Struct);
			return;
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(InProperty))
		{
			OutFields.Add(EnumProperty->GetEnum());
			return;
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(InProperty))
		{
			if (ByteProperty->Enum)
			{
				OutFields.Add(ByteProperty->Enum);
			}
			return;
		}
		if (const FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(InProperty))
		{
			OutFields.Add(InterfaceProperty->InterfaceClass);
			return;
		}
		if (const FClassProperty* ClassProperty = CastField<FClassProperty>(InProperty))
		{
			OutFields.Add(ClassProperty->MetaClass);
			return;
		}
		if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(InProperty))
		{
			OutFields.Add(SoftClassProperty->MetaClass);
			return;
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(InProperty))
		{
			OutFields.Add(ObjectProperty->PropertyClass);
			return;
		}
		if (const FDelegateProperty* DelegateProperty = CastField<FDelegateProperty>(InProperty))
		{
			CollectFunctionReferencedFields(DelegateProperty->SignatureFunction, OutFields, InDepth + 1);
			return;
		}
		if (const FMulticastDelegateProperty* MulticastProperty = CastField<FMulticastDelegateProperty>(InProperty))
		{
			CollectFunctionReferencedFields(MulticastProperty->SignatureFunction, OutFields, InDepth + 1);
			return;
		}

		// Everything else is a primitive - bool, numerics, FString, FName, FText - and needs no header.
	}

	/** True when a class is reflection scaffolding rather than something an author wrote. */
	bool IsSkeletonOrTrashClass(const UClass* InClass)
	{
		if (!InClass)
		{
			return true;
		}

		if (InClass->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			return true;
		}

		const FString ClassName = InClass->GetName();
		return ClassName.StartsWith(TEXT("SKEL_"))
			|| ClassName.StartsWith(TEXT("REINST_"))
			|| ClassName.StartsWith(TEXT("TRASHCLASS_"))
			|| ClassName.StartsWith(TEXT("PLACEHOLDER-"))
			|| ClassName.StartsWith(TEXT("HOTRELOADED_"));
	}

	/** Applies the allow/deny lists. An empty allow list means "every module". */
	bool IsModulePermitted(const FString& InModuleName, const FModPublicApiScanOptions& InOptions)
	{
		if (InOptions.ModuleDenyList.Contains(InModuleName))
		{
			return false;
		}
		return InOptions.ModuleAllowList.IsEmpty() || InOptions.ModuleAllowList.Contains(InModuleName);
	}

	/** True when the current project's descriptor declares this module as one of its own. */
	bool IsProjectOwnedModule(const FString& InModuleName)
	{
		const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
		if (!Project)
		{
			return false;
		}

		for (const FModuleDescriptor& Module : Project->Modules)
		{
			if (Module.Name.ToString().Equals(InModuleName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** Warns when a string that is supposed to be semver is not. */
	void ValidateVersionString(const FString& InValue, const TCHAR* InKeyName, const FString& InContext,
		TArray<FModDiagnostic>& OutDiagnostics)
	{
		if (InValue.IsEmpty())
		{
			return;
		}

		FModVersion Parsed;
		FString ParseError;
		if (!FModVersion::Parse(InValue, Parsed, &ParseError, /*bAllowPartial*/ true))
		{
			OutDiagnostics.Add(MakeScanDiagnostic(EModDiagnosticSeverity::Warning,
				ModPublicApiCodes::InvalidVersion,
				FString::Printf(TEXT("%s=\"%s\" is not a valid semantic version (%s). It is copied into the SDK index verbatim."),
					InKeyName, *InValue, *ParseError),
				InContext));
		}
	}

	const TCHAR* KindToString(EModPublicSymbolKind InKind)
	{
		switch (InKind)
		{
		case EModPublicSymbolKind::Class:     return TEXT("class");
		case EModPublicSymbolKind::Interface: return TEXT("interface");
		case EModPublicSymbolKind::Struct:    return TEXT("struct");
		case EModPublicSymbolKind::Enum:      return TEXT("enum");
		default:                              return TEXT("unknown");
		}
	}

	void AddOptionalString(const TSharedRef<FJsonObject>& InObject, const TCHAR* InKey, const FString& InValue)
	{
		if (!InValue.IsEmpty())
		{
			InObject->SetStringField(InKey, InValue);
		}
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& InValues)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(InValues.Num());
		for (const FString& Value : InValues)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	void WriteVersionMetadata(const TSharedRef<FJsonObject>& InObject, const FModPublicMetadata& InMetadata)
	{
		AddOptionalString(InObject, TEXT("since"), InMetadata.Since);
		AddOptionalString(InObject, TEXT("deprecated"), InMetadata.Deprecated);
	}

	TSharedRef<FJsonObject> MakeTypeJson(const FModPublicTypeInfo& InType)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("kind"), KindToString(InType.Kind));
		Object->SetStringField(TEXT("name"), InType.Name);
		Object->SetStringField(TEXT("path"), InType.PathName);
		Object->SetStringField(TEXT("module"), InType.ModuleName);
		AddOptionalString(Object, TEXT("header"), InType.HeaderPath);
		AddOptionalString(Object, TEXT("include"), InType.IncludePath);
		AddOptionalString(Object, TEXT("super"), InType.SuperName);
		AddOptionalString(Object, TEXT("apiId"), InType.Metadata.ApiId);
		AddOptionalString(Object, TEXT("apiVersion"), InType.Metadata.ApiVersion);
		AddOptionalString(Object, TEXT("extensionPoint"), InType.Metadata.ExtensionPointId);
		WriteVersionMetadata(Object, InType.Metadata);

		if (InType.bIsAbstract)
		{
			Object->SetBoolField(TEXT("abstract"), true);
		}
		if (InType.bIsModAPI)
		{
			Object->SetBoolField(TEXT("isModApi"), true);
		}
		if (InType.bIsModExtension)
		{
			Object->SetBoolField(TEXT("isModExtension"), true);
		}
		if (!InType.bIsNative)
		{
			Object->SetBoolField(TEXT("blueprintGenerated"), true);
		}

		if (InType.Enumerators.Num() > 0)
		{
			Object->SetArrayField(TEXT("enumerators"), MakeStringArray(InType.Enumerators));
		}

		TArray<TSharedPtr<FJsonValue>> FunctionValues;
		for (const FModPublicFunctionInfo& Function : InType.Functions)
		{
			if (!Function.bMarkedPublic)
			{
				continue;
			}

			const TSharedRef<FJsonObject> FunctionObject = MakeShared<FJsonObject>();
			FunctionObject->SetStringField(TEXT("name"), Function.Name);
			FunctionObject->SetStringField(TEXT("signature"), Function.Signature);
			FunctionObject->SetStringField(TEXT("returnType"), Function.ReturnCppType);
			FunctionObject->SetBoolField(TEXT("blueprintCallable"), Function.bBlueprintCallable);
			if (Function.bBlueprintPure)
			{
				FunctionObject->SetBoolField(TEXT("blueprintPure"), true);
			}
			if (Function.bBlueprintImplementable)
			{
				FunctionObject->SetBoolField(TEXT("blueprintImplementable"), true);
			}
			if (Function.bStatic)
			{
				FunctionObject->SetBoolField(TEXT("static"), true);
			}
			WriteVersionMetadata(FunctionObject, Function.Metadata);

			TArray<TSharedPtr<FJsonValue>> ParameterValues;
			for (const FModPublicParameterInfo& Parameter : Function.Parameters)
			{
				if (Parameter.bReturnValue)
				{
					continue;
				}

				const TSharedRef<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
				ParameterObject->SetStringField(TEXT("name"), Parameter.Name);
				ParameterObject->SetStringField(TEXT("type"), Parameter.CppType);
				if (Parameter.bOutParameter)
				{
					ParameterObject->SetBoolField(TEXT("out"), true);
				}
				ParameterValues.Add(MakeShared<FJsonValueObject>(ParameterObject));
			}
			if (ParameterValues.Num() > 0)
			{
				FunctionObject->SetArrayField(TEXT("parameters"), ParameterValues);
			}

			FunctionValues.Add(MakeShared<FJsonValueObject>(FunctionObject));
		}
		if (FunctionValues.Num() > 0)
		{
			Object->SetArrayField(TEXT("functions"), FunctionValues);
		}

		TArray<TSharedPtr<FJsonValue>> PropertyValues;
		for (const FModPublicPropertyInfo& Property : InType.Properties)
		{
			if (!Property.bMarkedPublic)
			{
				continue;
			}

			const TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
			PropertyObject->SetStringField(TEXT("name"), Property.Name);
			PropertyObject->SetStringField(TEXT("type"), Property.CppType);
			if (Property.bBlueprintReadOnly)
			{
				PropertyObject->SetBoolField(TEXT("readOnly"), true);
			}
			if (Property.bEditable)
			{
				PropertyObject->SetBoolField(TEXT("editable"), true);
			}
			WriteVersionMetadata(PropertyObject, Property.Metadata);
			PropertyValues.Add(MakeShared<FJsonValueObject>(PropertyObject));
		}
		if (PropertyValues.Num() > 0)
		{
			Object->SetArrayField(TEXT("properties"), PropertyValues);
		}

		return Object;
	}
}

//////////////////////////////////////////////////////////////////////////
// FModPublicMetadata

bool FModPublicMetadata::IsEmpty() const
{
	return ApiId.IsEmpty()
		&& ApiVersion.IsEmpty()
		&& ApiPermissions.Num() == 0
		&& !bHasServerAuthoritative
		&& ExtensionPointId.IsEmpty()
		&& Since.IsEmpty()
		&& Deprecated.IsEmpty();
}

//////////////////////////////////////////////////////////////////////////
// FModPublicTypeInfo

int32 FModPublicTypeInfo::CountMarkedFunctions() const
{
	int32 Count = 0;
	for (const FModPublicFunctionInfo& Function : Functions)
	{
		Count += Function.bMarkedPublic ? 1 : 0;
	}
	return Count;
}

int32 FModPublicTypeInfo::CountMarkedProperties() const
{
	int32 Count = 0;
	for (const FModPublicPropertyInfo& Property : Properties)
	{
		Count += Property.bMarkedPublic ? 1 : 0;
	}
	return Count;
}

//////////////////////////////////////////////////////////////////////////
// FModPublicApiReport

int32 FModPublicApiReport::GetTypeCount() const
{
	return Classes.Num() + Interfaces.Num() + Structs.Num() + Enums.Num();
}

int32 FModPublicApiReport::GetFunctionCount() const
{
	const TArray<FModPublicTypeInfo>* const Buckets[] = { &Classes, &Interfaces, &Structs, &Enums };

	int32 Count = 0;
	for (const TArray<FModPublicTypeInfo>* Bucket : Buckets)
	{
		for (const FModPublicTypeInfo& Type : *Bucket)
		{
			Count += Type.CountMarkedFunctions();
		}
	}
	return Count;
}

int32 FModPublicApiReport::GetPropertyCount() const
{
	const TArray<FModPublicTypeInfo>* const Buckets[] = { &Classes, &Interfaces, &Structs, &Enums };

	int32 Count = 0;
	for (const TArray<FModPublicTypeInfo>* Bucket : Buckets)
	{
		for (const FModPublicTypeInfo& Type : *Bucket)
		{
			Count += Type.CountMarkedProperties();
		}
	}
	return Count;
}

bool FModPublicApiReport::HasErrors() const
{
	return ModDiagnostics::HasErrors(Diagnostics);
}

bool FModPublicApiReport::HasWarnings() const
{
	for (const FModDiagnostic& Diagnostic : Diagnostics)
	{
		if (Diagnostic.Severity == EModDiagnosticSeverity::Warning)
		{
			return true;
		}
	}
	return false;
}

const FModPublicTypeInfo* FModPublicApiReport::FindType(const FString& InPathName) const
{
	const TArray<FModPublicTypeInfo>* const Buckets[] = { &Classes, &Interfaces, &Structs, &Enums };

	for (const TArray<FModPublicTypeInfo>* Bucket : Buckets)
	{
		for (const FModPublicTypeInfo& Type : *Bucket)
		{
			if (Type.PathName == InPathName)
			{
				return &Type;
			}
		}
	}
	return nullptr;
}

FString FModPublicApiReport::DescribeCounts() const
{
	return FString::Printf(
		TEXT("%d class(es), %d interface(s), %d struct(s), %d enum(s), %d function(s), %d property(ies), %d API(s), %d extension point(s) across %d module(s)"),
		Classes.Num(), Interfaces.Num(), Structs.Num(), Enums.Num(),
		GetFunctionCount(), GetPropertyCount(), Apis.Num(), ExtensionPoints.Num(), Modules.Num());
}

//////////////////////////////////////////////////////////////////////////
// FModPublicApiScanner - metadata access

bool FModPublicApiScanner::IsMetadataAvailable()
{
#if WITH_METADATA
	return true;
#else
	return false;
#endif
}

bool FModPublicApiScanner::HasFieldMetadata(const UField* InField, const TCHAR* InKey)
{
#if WITH_METADATA
	// UEnum declares its own HasMetaData overload that hides UField's, but both read the same
	// per-package FMetaData map keyed by the object (Class.cpp UField::FindMetaData, Enum.cpp
	// UEnum::HasMetaData), so going through the UField pointer is correct for enums too.
	return InField != nullptr && InKey != nullptr && InField->HasMetaData(InKey);
#else
	(void)InField;
	(void)InKey;
	return false;
#endif
}

FString FModPublicApiScanner::GetFieldMetadata(const UField* InField, const TCHAR* InKey)
{
#if WITH_METADATA
	if (InField && InKey)
	{
		if (const FString* Value = InField->FindMetaData(InKey))
		{
			return *Value;
		}
	}
	return FString();
#else
	(void)InField;
	(void)InKey;
	return FString();
#endif
}

bool FModPublicApiScanner::HasPropertyMetadata(const FProperty* InProperty, const TCHAR* InKey)
{
#if WITH_METADATA
	return InProperty != nullptr && InKey != nullptr && InProperty->HasMetaData(InKey);
#else
	(void)InProperty;
	(void)InKey;
	return false;
#endif
}

FString FModPublicApiScanner::GetPropertyMetadata(const FProperty* InProperty, const TCHAR* InKey)
{
#if WITH_METADATA
	if (InProperty && InKey)
	{
		if (const FString* Value = InProperty->FindMetaData(InKey))
		{
			return *Value;
		}
	}
	return FString();
#else
	(void)InProperty;
	(void)InKey;
	return FString();
#endif
}

bool FModPublicApiScanner::IsMarkedPublic(const UField* InField)
{
	return HasFieldMetadata(InField, ModPublicApiMetadata::ModPublic);
}

bool FModPublicApiScanner::IsMarkedPublic(const FProperty* InProperty)
{
	return HasPropertyMetadata(InProperty, ModPublicApiMetadata::ModPublic);
}

FModPublicMetadata FModPublicApiScanner::ReadMetadata(const UField* InField)
{
	using namespace ModPublicApiScannerPrivate;

	FModPublicMetadata Result;
	if (!InField)
	{
		return Result;
	}

	Result.ApiId = GetFieldMetadata(InField, ModPublicApiMetadata::ModApiId).TrimStartAndEnd();
	Result.ApiVersion = GetFieldMetadata(InField, ModPublicApiMetadata::ModApiVersion).TrimStartAndEnd();
	Result.ApiPermissions = SplitMetadataList(GetFieldMetadata(InField, ModPublicApiMetadata::ModApiPermissions));
	Result.ExtensionPointId = GetFieldMetadata(InField, ModPublicApiMetadata::ModExtensionPoint).TrimStartAndEnd();
	Result.Since = GetFieldMetadata(InField, ModPublicApiMetadata::ModSince).TrimStartAndEnd();
	Result.Deprecated = GetFieldMetadata(InField, ModPublicApiMetadata::ModDeprecated).TrimStartAndEnd();

	Result.bHasServerAuthoritative = HasFieldMetadata(InField, ModPublicApiMetadata::ModApiServerAuthoritative);
	if (Result.bHasServerAuthoritative)
	{
		Result.bServerAuthoritative =
			ParseMetadataBool(GetFieldMetadata(InField, ModPublicApiMetadata::ModApiServerAuthoritative));
	}

	return Result;
}

FModPublicMetadata FModPublicApiScanner::ReadMetadata(const FProperty* InProperty)
{
	FModPublicMetadata Result;
	if (!InProperty)
	{
		return Result;
	}

	Result.Since = GetPropertyMetadata(InProperty, ModPublicApiMetadata::ModSince).TrimStartAndEnd();
	Result.Deprecated = GetPropertyMetadata(InProperty, ModPublicApiMetadata::ModDeprecated).TrimStartAndEnd();
	return Result;
}

//////////////////////////////////////////////////////////////////////////
// FModPublicApiScanner - helpers

FString FModPublicApiScanner::GetOwningModuleName(const UField* InField)
{
	if (!InField)
	{
		return FString();
	}

	const UPackage* Package = InField->GetOutermost();
	if (!Package)
	{
		return FString();
	}

	const FString PackageName = Package->GetName();
	static const FString ScriptPrefix(TEXT("/Script/"));
	if (PackageName.StartsWith(ScriptPrefix, ESearchCase::CaseSensitive))
	{
		return PackageName.RightChop(ScriptPrefix.Len());
	}

	// A Blueprint-generated type lives in a content package; there is no module to name, so the
	// package path is reported instead and callers tell the two apart by the leading slash.
	return PackageName;
}

TArray<FString> FModPublicApiScanner::GetPluginModuleNames(const FString& InPluginName)
{
	TArray<FString> Result;
	if (InPluginName.IsEmpty())
	{
		return Result;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(InPluginName);
	if (!Plugin.IsValid())
	{
		return Result;
	}

	for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
	{
		Result.AddUnique(Module.Name.ToString());
	}
	return Result;
}

bool FModPublicApiScanner::IsModuleAvailableToModAuthors(const FString& InModuleName,
	const TSet<FString>& InShippingModules)
{
	using namespace ModPublicApiScannerPrivate;

	// An unknown module cannot be judged. Staying quiet beats crying wolf on every scan.
	if (InModuleName.IsEmpty())
	{
		return true;
	}

	if (InShippingModules.Contains(InModuleName))
	{
		return true;
	}

	// A content package path, i.e. a Blueprint-generated type. Never available as a header.
	if (InModuleName.StartsWith(TEXT("/")))
	{
		return false;
	}

	const TSharedPtr<IPlugin> OwningPlugin = IPluginManager::Get().GetModuleOwnerPlugin(FName(*InModuleName));
	if (OwningPlugin.IsValid())
	{
		// An engine plugin exists in every install of the same engine version, so a mod author
		// building against that engine already has its headers.
		return OwningPlugin->GetLoadedFrom() == EPluginLoadedFrom::Engine;
	}

	// No owning plugin: either a base engine module or one of the project's own modules.
	const FString ModuleFilename = FModuleManager::Get().GetModuleFilename(FName(*InModuleName));
	if (!ModuleFilename.IsEmpty())
	{
		const FString EngineDirectory = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
		return FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(ModuleFilename), EngineDirectory);
	}

	// Monolithic build, or a module linked straight into the executable: there is no DLL to locate.
	// Fall back to the project descriptor, which is authoritative about the game's own modules;
	// anything it does not claim arrived with the engine.
	return !IsProjectOwnedModule(InModuleName);
}

//////////////////////////////////////////////////////////////////////////
// FModPublicApiScanner - scan

FModPublicApiReport FModPublicApiScanner::Scan()
{
	return Scan(FModPublicApiScanOptions());
}

FModPublicApiReport FModPublicApiScanner::Scan(const FModPublicApiScanOptions& InOptions)
{
	using namespace ModPublicApiScannerPrivate;

	FModPublicApiReport Report;

	const double StartSeconds = FPlatformTime::Seconds();

	if (!IsMetadataAvailable())
	{
		Report.bMetadataAvailable = false;
		Report.Diagnostics.Add(MakeScanDiagnostic(EModDiagnosticSeverity::Error,
			ModPublicApiCodes::MetadataUnavailable,
			TEXT("Reflection metadata is compiled out of this build (WITH_METADATA is 0), so no ModPublic symbol can be seen. ")
			TEXT("Run SDK generation from an editor build or an editor commandlet.")));
		Report.ScanSeconds = FPlatformTime::Seconds() - StartSeconds;
		return Report;
	}

	// Modules whose source is copied into the bundle verbatim. An unmarked type from one of these
	// still compiles for the mod author, so it must not be reported as a leak.
	TSet<FString> ShippingModules;
	{
		TArray<FString> PluginNames = InOptions.ShippingPluginNames;
		PluginNames.AddUnique(FrameworkPluginName);

		for (const FString& PluginName : PluginNames)
		{
			for (const FString& ModuleName : GetPluginModuleNames(PluginName))
			{
				ShippingModules.Add(ModuleName);
			}
		}
	}

	const UClass* const ModApiClass = UModAPI::StaticClass();
	const UClass* const ModExtensionClass = UModExtension::StaticClass();

	TSet<FString> ModulesSeen;

	// Path name -> the live reflection object, so pass 2 never has to look a type up again. Local to
	// this function and never published: the report itself holds no raw UObject pointers, so nothing
	// in it can dangle after a GC.
	TMap<FString, const UStruct*> ResolvedStructs;

	//~ Pass 1: collect every marked type ----------------------------------------------------------

	auto ReadCommonTypeInfo = [](const UField* InField, FModPublicTypeInfo& OutType)
	{
		OutType.Name = InField->GetName();
		OutType.PathName = InField->GetPathName();
		OutType.ModuleName = GetOwningModuleName(InField);
		OutType.HeaderPath = GetFieldMetadata(InField, ModPublicApiMetadata::ModuleRelativePath);
		OutType.IncludePath = GetFieldMetadata(InField, ModPublicApiMetadata::IncludePath);
		OutType.Metadata = ReadMetadata(InField);
	};

	auto CollectMembers = [](const UStruct* InStruct, FModPublicTypeInfo& OutType)
	{
		for (TFieldIterator<UFunction> FunctionIt(InStruct, EFieldIterationFlags::None); FunctionIt; ++FunctionIt)
		{
			const UFunction* Function = *FunctionIt;
			if (!Function || Function->HasAnyFunctionFlags(FUNC_Delegate))
			{
				continue;
			}

			FModPublicFunctionInfo Info;
			Info.Name = Function->GetName();
			Info.OwnerPath = InStruct->GetPathName();
			Info.bMarkedPublic = IsMarkedPublic(Function);
			Info.bBlueprintCallable = Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
			Info.bBlueprintPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
			Info.bBlueprintEvent = Function->HasAnyFunctionFlags(FUNC_BlueprintEvent);
			Info.bStatic = Function->HasAnyFunctionFlags(FUNC_Static);
			Info.bNative = Function->HasAnyFunctionFlags(FUNC_Native);
			Info.bBlueprintImplementable = Info.bBlueprintEvent && !Info.bNative;
			Info.HeaderPath = GetFieldMetadata(Function, ModPublicApiMetadata::ModuleRelativePath);
			Info.Metadata = ReadMetadata(Function);

			FString ParameterText;
			for (TFieldIterator<FProperty> ParamIt(Function, EFieldIterationFlags::None); ParamIt; ++ParamIt)
			{
				const FProperty* Parameter = *ParamIt;
				if (!Parameter || !Parameter->HasAnyPropertyFlags(CPF_Parm))
				{
					continue;
				}

				FModPublicParameterInfo ParameterInfo;
				ParameterInfo.Name = Parameter->GetName();
				ParameterInfo.CppType = RenderCppType(Parameter);
				ParameterInfo.bReturnValue = Parameter->HasAnyPropertyFlags(CPF_ReturnParm);
				ParameterInfo.bOutParameter = Parameter->HasAnyPropertyFlags(CPF_OutParm) && !ParameterInfo.bReturnValue;
				ParameterInfo.bConstReference = Parameter->HasAllPropertyFlags(CPF_ConstParm | CPF_ReferenceParm);

				if (ParameterInfo.bReturnValue)
				{
					Info.ReturnCppType = ParameterInfo.CppType;
				}
				else
				{
					if (!ParameterText.IsEmpty())
					{
						ParameterText += TEXT(", ");
					}
					if (ParameterInfo.bConstReference)
					{
						ParameterText += TEXT("const ");
					}
					ParameterText += ParameterInfo.CppType;
					if (ParameterInfo.bConstReference || ParameterInfo.bOutParameter)
					{
						ParameterText += TEXT("&");
					}
					ParameterText += TEXT(" ");
					ParameterText += ParameterInfo.Name;
				}

				Info.Parameters.Add(MoveTemp(ParameterInfo));
			}

			if (Info.ReturnCppType.IsEmpty())
			{
				Info.ReturnCppType = TEXT("void");
			}

			Info.Signature = FString::Printf(TEXT("%s%s %s(%s)"),
				Info.bStatic ? TEXT("static ") : TEXT(""),
				*Info.ReturnCppType, *Info.Name, *ParameterText);

			OutType.Functions.Add(MoveTemp(Info));
		}

		for (TFieldIterator<FProperty> PropertyIt(InStruct, EFieldIterationFlags::None); PropertyIt; ++PropertyIt)
		{
			const FProperty* Property = *PropertyIt;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}

			FModPublicPropertyInfo Info;
			Info.Name = Property->GetName();
			Info.OwnerPath = InStruct->GetPathName();
			Info.CppType = RenderCppType(Property);
			Info.bMarkedPublic = IsMarkedPublic(Property);
			Info.bBlueprintVisible = Property->HasAnyPropertyFlags(CPF_BlueprintVisible);
			Info.bBlueprintReadOnly = Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
			Info.bEditable = Property->HasAnyPropertyFlags(CPF_Edit);
			Info.bConfig = Property->HasAnyPropertyFlags(CPF_Config);
			Info.HeaderPath = GetPropertyMetadata(Property, ModPublicApiMetadata::ModuleRelativePath);
			Info.Metadata = ReadMetadata(Property);

			OutType.Properties.Add(MoveTemp(Info));
		}
	};

	for (UClass* Class : TObjectRange<UClass>())
	{
		if (IsSkeletonOrTrashClass(Class))
		{
			continue;
		}

		++Report.TypesExamined;

		const bool bNative = Class->HasAnyClassFlags(CLASS_Native);
		if (!bNative && !InOptions.bIncludeBlueprintGeneratedTypes)
		{
			continue;
		}

		if (!IsMarkedPublic(Class))
		{
			continue;
		}

		const FString ModuleName = GetOwningModuleName(Class);
		if (!IsModulePermitted(ModuleName, InOptions))
		{
			continue;
		}

		FModPublicTypeInfo Type;
		Type.Kind = Class->HasAnyClassFlags(CLASS_Interface)
			? EModPublicSymbolKind::Interface
			: EModPublicSymbolKind::Class;
		ReadCommonTypeInfo(Class, Type);
		Type.bIsNative = bNative;
		Type.bIsAbstract = Class->HasAnyClassFlags(CLASS_Abstract);
		Type.bIsBlueprintType = ParseMetadataBool(GetFieldMetadata(Class, TEXT("BlueprintType")));
		Type.bIsBlueprintable = ParseMetadataBool(GetFieldMetadata(Class, TEXT("IsBlueprintBase")));
		Type.bIsModAPI = ModApiClass && Class->IsChildOf(ModApiClass);
		Type.bIsModExtension = ModExtensionClass && Class->IsChildOf(ModExtensionClass);

		if (const UClass* Super = Class->GetSuperClass())
		{
			Type.SuperName = Super->GetName();
			Type.SuperPath = Super->GetPathName();
		}

		CollectMembers(Class, Type);
		ModulesSeen.Add(ModuleName);
		ResolvedStructs.Add(Type.PathName, Class);

		if (Type.Kind == EModPublicSymbolKind::Interface)
		{
			Report.Interfaces.Add(MoveTemp(Type));
		}
		else
		{
			Report.Classes.Add(MoveTemp(Type));
		}
	}

	for (UScriptStruct* Struct : TObjectRange<UScriptStruct>())
	{
		++Report.TypesExamined;

		if (!IsMarkedPublic(Struct))
		{
			continue;
		}

		const FString ModuleName = GetOwningModuleName(Struct);
		if (!IsModulePermitted(ModuleName, InOptions))
		{
			continue;
		}

		FModPublicTypeInfo Type;
		Type.Kind = EModPublicSymbolKind::Struct;
		ReadCommonTypeInfo(Struct, Type);
		Type.bIsBlueprintType = ParseMetadataBool(GetFieldMetadata(Struct, TEXT("BlueprintType")));

		if (const UStruct* Super = Struct->GetSuperStruct())
		{
			Type.SuperName = Super->GetName();
			Type.SuperPath = Super->GetPathName();
		}

		CollectMembers(Struct, Type);
		ModulesSeen.Add(ModuleName);
		ResolvedStructs.Add(Type.PathName, Struct);
		Report.Structs.Add(MoveTemp(Type));
	}

	for (UEnum* Enum : TObjectRange<UEnum>())
	{
		++Report.TypesExamined;

		if (!IsMarkedPublic(Enum))
		{
			continue;
		}

		const FString ModuleName = GetOwningModuleName(Enum);
		if (!IsModulePermitted(ModuleName, InOptions))
		{
			continue;
		}

		FModPublicTypeInfo Type;
		Type.Kind = EModPublicSymbolKind::Enum;
		ReadCommonTypeInfo(Enum, Type);
		Type.bIsBlueprintType = ParseMetadataBool(GetFieldMetadata(Enum, TEXT("BlueprintType")));

		const int32 NumEnums = Enum->NumEnums();
		for (int32 Index = 0; Index < NumEnums; ++Index)
		{
			const FString EnumeratorName = Enum->GetNameStringByIndex(Index);
			// UHT appends a synthetic <Enum>_MAX sentinel that is not part of the authored surface.
			if (EnumeratorName.IsEmpty() || EnumeratorName.EndsWith(TEXT("_MAX"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			Type.Enumerators.Add(EnumeratorName);
		}

		ModulesSeen.Add(ModuleName);
		Report.Enums.Add(MoveTemp(Type));
	}

	//~ Deterministic ordering ----------------------------------------------------------------------
	// TObjectRange order follows UObject allocation order, which differs between runs. An SDK index
	// that reshuffled itself would make every regeneration look like a change.

	auto SortTypes = [](TArray<FModPublicTypeInfo>& InTypes)
	{
		Algo::Sort(InTypes, [](const FModPublicTypeInfo& A, const FModPublicTypeInfo& B)
		{
			if (A.ModuleName != B.ModuleName)
			{
				return A.ModuleName < B.ModuleName;
			}
			return A.PathName < B.PathName;
		});
	};

	SortTypes(Report.Classes);
	SortTypes(Report.Interfaces);
	SortTypes(Report.Structs);
	SortTypes(Report.Enums);

	Report.Modules = ModulesSeen.Array();
	Report.Modules.Sort();

	//~ Pass 2: indices and validation --------------------------------------------------------------

	TSet<FString> MarkedTypePaths;
	MarkedTypePaths.Reserve(Report.GetTypeCount());
	{
		const TArray<FModPublicTypeInfo>* const Buckets[] =
			{ &Report.Classes, &Report.Interfaces, &Report.Structs, &Report.Enums };
		for (const TArray<FModPublicTypeInfo>* Bucket : Buckets)
		{
			for (const FModPublicTypeInfo& Type : *Bucket)
			{
				MarkedTypePaths.Add(Type.PathName);
			}
		}
	}

	TMap<FString, FString> ApiIdOwners;

	for (const FModPublicTypeInfo& Type : Report.Classes)
	{
		const FString& Context = Type.PathName;

		ValidateVersionString(Type.Metadata.ApiVersion, ModPublicApiMetadata::ModApiVersion, Context, Report.Diagnostics);
		ValidateVersionString(Type.Metadata.Since, ModPublicApiMetadata::ModSince, Context, Report.Diagnostics);
		ValidateVersionString(Type.Metadata.Deprecated, ModPublicApiMetadata::ModDeprecated, Context, Report.Diagnostics);

		if (!Type.bIsModAPI)
		{
			continue;
		}

		const UStruct* const* ResolvedStruct = ResolvedStructs.Find(Type.PathName);
		const UClass* ApiClass = ResolvedStruct ? Cast<UClass>(*ResolvedStruct) : nullptr;

		// Resolve what a SHIPPED build would use. Every native class already has its CDO by the time
		// reflection is walked, so passing false here creates nothing.
		FString NativeApiId;
		FString NativeApiVersion;
		if (ApiClass)
		{
			if (const UModAPI* ApiDefaults = Cast<UModAPI>(ApiClass->GetDefaultObject(/*bCreateIfNeeded*/ false)))
			{
				const FName ResolvedNativeId = ApiDefaults->NativeGetApiId();
				if (!ResolvedNativeId.IsNone())
				{
					NativeApiId = ResolvedNativeId.ToString();
				}

				const FModVersion ResolvedNativeVersion = ApiDefaults->NativeGetApiVersion();
				if (!ResolvedNativeVersion.IsZero())
				{
					NativeApiVersion = ResolvedNativeVersion.ToString();
				}
			}
		}

		FModPublicApiEntry Entry;
		Entry.ApiId = Type.Metadata.ApiId.IsEmpty() ? NativeApiId : Type.Metadata.ApiId;
		Entry.Version = Type.Metadata.ApiVersion.IsEmpty() ? NativeApiVersion : Type.Metadata.ApiVersion;
		Entry.RequiredPermissions = Type.Metadata.ApiPermissions;
		Entry.bServerAuthoritative = Type.Metadata.bServerAuthoritative;
		Entry.ClassName = Type.Name;
		Entry.ClassPath = Type.PathName;
		Entry.ModuleName = Type.ModuleName;
		Entry.HeaderPath = Type.HeaderPath;
		Entry.NativeApiId = NativeApiId;
		Entry.bNativeIdentity = !NativeApiId.IsEmpty();

		if (InOptions.bWarnOnMissingIdentity && Entry.ApiId.IsEmpty() && !Type.bIsAbstract)
		{
			Report.Diagnostics.Add(MakeScanDiagnostic(EModDiagnosticSeverity::Warning,
				ModPublicApiCodes::ApiMissingId,
				FString::Printf(TEXT("%s derives from UModAPI but declares no ModApiId and overrides no NativeGetApiId. ")
					TEXT("It will register under the name-derived fallback id \"%s\", which silently changes whenever the class is renamed."),
					*Type.Name, *UModAPI::MakeFallbackApiId(ApiClass).ToString()),
				Context));
		}

		if (InOptions.bWarnOnNonNativeIdentity && !Type.bIsAbstract && !Type.Metadata.ApiId.IsEmpty())
		{
			if (NativeApiId.IsEmpty())
			{
				Report.Diagnostics.Add(MakeScanDiagnostic(EModDiagnosticSeverity::Warning,
					ModPublicApiCodes::ApiIdentityNotNative,
					FString::Printf(TEXT("%s declares ModApiId=\"%s\" but does not override NativeGetApiId. ")
						TEXT("Class metadata is stripped from cooked builds, so the shipped game registers this API under a different id than the editor does."),
						*Type.Name, *Type.Metadata.ApiId),
					Context));
			}
			else if (!Type.Metadata.ApiId.Equals(NativeApiId, ESearchCase::IgnoreCase))
			{
				Report.Diagnostics.Add(MakeScanDiagnostic(EModDiagnosticSeverity::Error,
					ModPublicApiCodes::ApiIdentityMismatch,
					FString::Printf(TEXT("%s declares ModApiId=\"%s\" but NativeGetApiId returns \"%s\". ")
						TEXT("The editor and the shipped game would publish two different APIs."),
						*Type.Name, *Type.Metadata.ApiId, *NativeApiId),
					Context));
			}
		}

		if (!Entry.ApiId.IsEmpty())
		{
			if (const FString* ExistingOwner = ApiIdOwners.Find(Entry.ApiId))
			{
				Report.Diagnostics.Add(MakeScanDiagnostic(EModDiagnosticSeverity::Error,
					ModPublicApiCodes::DuplicateApiId,
					FString::Printf(TEXT("API id \"%s\" is claimed by both %s and %s. Only one of them can ever be registered."),
						*Entry.ApiId, **ExistingOwner, *Type.Name),
					Context));
			}
			else
			{
				ApiIdOwners.Add(Entry.ApiId, Type.Name);
			}
		}

		Report.Apis.Add(MoveTemp(Entry));
	}

	Algo::Sort(Report.Apis, [](const FModPublicApiEntry& A, const FModPublicApiEntry& B)
	{
		return A.ApiId != B.ApiId ? A.ApiId < B.ApiId : A.ClassPath < B.ClassPath;
	});

	for (const FModPublicTypeInfo& Type : Report.Classes)
	{
		if (!Type.bIsModExtension)
		{
			continue;
		}

		if (Type.Metadata.ExtensionPointId.IsEmpty())
		{
			if (InOptions.bWarnOnMissingIdentity)
			{
				Report.Diagnostics.Add(MakeScanDiagnostic(EModDiagnosticSeverity::Warning,
					ModPublicApiCodes::ExtensionMissingPoint,
					FString::Printf(TEXT("%s derives from UModExtension but declares no ModExtensionPoint, so the generated SDK cannot tell a mod author which point it plugs into."),
						*Type.Name),
					Type.PathName));
			}
			continue;
		}

		FModPublicExtensionPointEntry Entry;
		Entry.ExtensionPointId = Type.Metadata.ExtensionPointId;
		Entry.BaseClassName = Type.Name;
		Entry.BaseClassPath = Type.PathName;
		Entry.ModuleName = Type.ModuleName;
		Entry.HeaderPath = Type.HeaderPath;
		Entry.bAbstract = Type.bIsAbstract;
		Entry.bBlueprintable = Type.bIsBlueprintable;
		Report.ExtensionPoints.Add(MoveTemp(Entry));
	}

	Algo::Sort(Report.ExtensionPoints, [](const FModPublicExtensionPointEntry& A, const FModPublicExtensionPointEntry& B)
	{
		return A.ExtensionPointId != B.ExtensionPointId
			? A.ExtensionPointId < B.ExtensionPointId
			: A.BaseClassPath < B.BaseClassPath;
	});

	//~ "Marked the class, forgot the members" ------------------------------------------------------

	if (InOptions.bWarnOnUnmarkedMembers)
	{
		const TArray<FModPublicTypeInfo>* const Buckets[] = { &Report.Classes, &Report.Interfaces };
		for (const TArray<FModPublicTypeInfo>* Bucket : Buckets)
		{
			for (const FModPublicTypeInfo& Type : *Bucket)
			{
				int32 CallableCount = 0;
				int32 MarkedCallableCount = 0;
				for (const FModPublicFunctionInfo& Function : Type.Functions)
				{
					if (Function.bBlueprintCallable || Function.bBlueprintPure)
					{
						++CallableCount;
						MarkedCallableCount += Function.bMarkedPublic ? 1 : 0;
					}
				}

				if (CallableCount > 0 && MarkedCallableCount == 0 && Type.CountMarkedProperties() == 0)
				{
					Report.Diagnostics.Add(MakeScanDiagnostic(EModDiagnosticSeverity::Warning,
						ModPublicApiCodes::NoMarkedMembers,
						FString::Printf(TEXT("%s is marked ModPublic and declares %d Blueprint-callable function(s), but not one of them - and none of its properties - carries ModPublic. ")
							TEXT("Marking a class does not mark its members, so the generated SDK would document an empty type."),
							*Type.Name, CallableCount),
						Type.PathName));
				}
			}
		}
	}

	//~ The leak that breaks a generated SDK's compile ----------------------------------------------

	if (InOptions.bDetectUnmarkedTypeLeaks)
	{
		// One report per (marked type, leaked type) pair: a struct with six fields of the same
		// game-internal type is one mistake, not six.
		TSet<FString> AlreadyReported;

		auto ReportLeak = [&Report, &AlreadyReported]
			(const FModPublicTypeInfo& InOwner, const FString& InMemberDescription, const UField* InReferenced)
		{
			if (!InReferenced)
			{
				return;
			}

			const FString ReferencedPath = InReferenced->GetPathName();
			const FString Key = InOwner.PathName + TEXT("|") + ReferencedPath;
			if (AlreadyReported.Contains(Key))
			{
				return;
			}
			AlreadyReported.Add(Key);

			const FString ReferencedName = InReferenced->GetName();
			const FString ReferencedModule = FModPublicApiScanner::GetOwningModuleName(InReferenced);

			Report.Diagnostics.Add(ModPublicApiScannerPrivate::MakeScanDiagnostic(EModDiagnosticSeverity::Error,
				ModPublicApiCodes::UnmarkedTypeLeak,
				FString::Printf(
					TEXT("%s exposes %s, whose type %s lives in \"%s\". That module is not shipped inside the SDK bundle and %s is not marked ModPublic, ")
					TEXT("so the generated SDK will not compile in a mod author's project. Mark %s ModPublic, or replace it with a flat mod-facing type."),
					*InOwner.Name, *InMemberDescription, *ReferencedName,
					ReferencedModule.IsEmpty() ? TEXT("<unknown>") : *ReferencedModule,
					*ReferencedName, *ReferencedName),
				InOwner.PathName));
		};

		auto IsReferenceSatisfied = [&MarkedTypePaths, &ShippingModules](const UField* InReferenced) -> bool
		{
			if (!InReferenced)
			{
				return true;
			}
			if (MarkedTypePaths.Contains(InReferenced->GetPathName()))
			{
				return true;
			}
			return FModPublicApiScanner::IsModuleAvailableToModAuthors(
				FModPublicApiScanner::GetOwningModuleName(InReferenced), ShippingModules);
		};

		const TArray<FModPublicTypeInfo>* const Buckets[] = { &Report.Classes, &Report.Interfaces, &Report.Structs };
		for (const TArray<FModPublicTypeInfo>* Bucket : Buckets)
		{
			for (const FModPublicTypeInfo& Type : *Bucket)
			{
				const UStruct* const* Found = ResolvedStructs.Find(Type.PathName);
				if (!Found || !*Found)
				{
					continue;
				}
				const UStruct* Struct = *Found;

				// A marked type's own base class has to exist for the declaration to compile at all.
				if (const UStruct* Super = Struct->GetSuperStruct())
				{
					if (!IsReferenceSatisfied(Super))
					{
						ReportLeak(Type, TEXT("its base class"), Super);
					}
				}

				TArray<const UField*> Referenced;

				for (TFieldIterator<UFunction> FunctionIt(Struct, EFieldIterationFlags::None); FunctionIt; ++FunctionIt)
				{
					const UFunction* Function = *FunctionIt;
					if (!Function || Function->HasAnyFunctionFlags(FUNC_Delegate) || !IsMarkedPublic(Function))
					{
						continue;
					}

					Referenced.Reset();
					CollectFunctionReferencedFields(Function, Referenced, 0);
					for (const UField* Field : Referenced)
					{
						if (!IsReferenceSatisfied(Field))
						{
							ReportLeak(Type, FString::Printf(TEXT("the signature of %s()"), *Function->GetName()), Field);
						}
					}
				}

				for (TFieldIterator<FProperty> PropertyIt(Struct, EFieldIterationFlags::None); PropertyIt; ++PropertyIt)
				{
					const FProperty* Property = *PropertyIt;
					if (!Property || Property->HasAnyPropertyFlags(CPF_Parm) || !IsMarkedPublic(Property))
					{
						continue;
					}

					Referenced.Reset();
					CollectReferencedFields(Property, Referenced, 0);
					for (const UField* Field : Referenced)
					{
						if (!IsReferenceSatisfied(Field))
						{
							ReportLeak(Type, FString::Printf(TEXT("the property %s"), *Property->GetName()), Field);
						}
					}
				}
			}
		}
	}

	Report.ScanSeconds = FPlatformTime::Seconds() - StartSeconds;

	UE_LOG(LogModFrameworkDeveloper, Log,
		TEXT("Public API scan finished in %.3fs: %s (%d type(s) examined, %d diagnostic(s))."),
		Report.ScanSeconds, *Report.DescribeCounts(), Report.TypesExamined, Report.Diagnostics.Num());

	return Report;
}

//////////////////////////////////////////////////////////////////////////
// FModPublicApiScanner - JSON

TSharedRef<FJsonObject> FModPublicApiScanner::BuildApiIndexJson(const FModPublicApiReport& InReport)
{
	using namespace ModPublicApiScannerPrivate;

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	const TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("classes"), InReport.Classes.Num());
	Counts->SetNumberField(TEXT("interfaces"), InReport.Interfaces.Num());
	Counts->SetNumberField(TEXT("structs"), InReport.Structs.Num());
	Counts->SetNumberField(TEXT("enums"), InReport.Enums.Num());
	Counts->SetNumberField(TEXT("functions"), InReport.GetFunctionCount());
	Counts->SetNumberField(TEXT("properties"), InReport.GetPropertyCount());
	Root->SetObjectField(TEXT("counts"), Counts);

	Root->SetArrayField(TEXT("modules"), MakeStringArray(InReport.Modules));

	TArray<TSharedPtr<FJsonValue>> ApiValues;
	for (const FModPublicApiEntry& Api : InReport.Apis)
	{
		const TSharedRef<FJsonObject> ApiObject = MakeShared<FJsonObject>();
		ApiObject->SetStringField(TEXT("id"), Api.ApiId);
		ApiObject->SetStringField(TEXT("version"), Api.Version);
		ApiObject->SetStringField(TEXT("class"), Api.ClassName);
		ApiObject->SetStringField(TEXT("classPath"), Api.ClassPath);
		ApiObject->SetStringField(TEXT("module"), Api.ModuleName);
		AddOptionalString(ApiObject, TEXT("header"), Api.HeaderPath);
		ApiObject->SetBoolField(TEXT("serverAuthoritative"), Api.bServerAuthoritative);
		ApiObject->SetArrayField(TEXT("permissions"), MakeStringArray(Api.RequiredPermissions));
		// Recorded so a mod author can see which ids are guaranteed to survive cooking.
		ApiObject->SetBoolField(TEXT("nativeIdentity"), Api.bNativeIdentity);
		AddOptionalString(ApiObject, TEXT("nativeId"), Api.NativeApiId);
		ApiValues.Add(MakeShared<FJsonValueObject>(ApiObject));
	}
	Root->SetArrayField(TEXT("apis"), ApiValues);

	TArray<TSharedPtr<FJsonValue>> PointValues;
	for (const FModPublicExtensionPointEntry& Point : InReport.ExtensionPoints)
	{
		const TSharedRef<FJsonObject> PointObject = MakeShared<FJsonObject>();
		PointObject->SetStringField(TEXT("id"), Point.ExtensionPointId);
		PointObject->SetStringField(TEXT("baseClass"), Point.BaseClassName);
		PointObject->SetStringField(TEXT("baseClassPath"), Point.BaseClassPath);
		PointObject->SetStringField(TEXT("module"), Point.ModuleName);
		AddOptionalString(PointObject, TEXT("header"), Point.HeaderPath);
		PointObject->SetBoolField(TEXT("abstract"), Point.bAbstract);
		PointValues.Add(MakeShared<FJsonValueObject>(PointObject));
	}
	Root->SetArrayField(TEXT("extensionPoints"), PointValues);

	TArray<TSharedPtr<FJsonValue>> TypeValues;
	{
		const TArray<FModPublicTypeInfo>* const Buckets[] =
			{ &InReport.Classes, &InReport.Interfaces, &InReport.Structs, &InReport.Enums };
		for (const TArray<FModPublicTypeInfo>* Bucket : Buckets)
		{
			for (const FModPublicTypeInfo& Type : *Bucket)
			{
				TypeValues.Add(MakeShared<FJsonValueObject>(MakeTypeJson(Type)));
			}
		}
	}
	Root->SetArrayField(TEXT("types"), TypeValues);

	return Root;
}

bool FModPublicApiScanner::SerializeReportToString(const FModPublicApiReport& InReport, FString& OutJson)
{
	const TSharedRef<FJsonObject> Root = BuildApiIndexJson(InReport);

	Root->SetBoolField(TEXT("metadataAvailable"), InReport.bMetadataAvailable);
	Root->SetNumberField(TEXT("typesExamined"), InReport.TypesExamined);

	TArray<TSharedPtr<FJsonValue>> DiagnosticValues;
	for (const FModDiagnostic& Diagnostic : InReport.Diagnostics)
	{
		const TSharedRef<FJsonObject> DiagnosticObject = MakeShared<FJsonObject>();
		DiagnosticObject->SetStringField(TEXT("severity"), ModFrameworkEnums::ToString(Diagnostic.Severity));
		DiagnosticObject->SetStringField(TEXT("code"), Diagnostic.Code.ToString());
		DiagnosticObject->SetStringField(TEXT("message"), Diagnostic.Message);
		if (!Diagnostic.Context.IsEmpty())
		{
			DiagnosticObject->SetStringField(TEXT("context"), Diagnostic.Context);
		}
		DiagnosticValues.Add(MakeShared<FJsonValueObject>(DiagnosticObject));
	}
	Root->SetArrayField(TEXT("diagnostics"), DiagnosticValues);

	OutJson.Reset();

	// FJsonSerializer::Serialize takes Policy::FMapOfValues, which is exactly
	// TSharedPtr<FJsonObject>, and closes the writer itself (bCloseWriter defaults to true).
	const TSharedPtr<FJsonObject> Object = Root;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);

	return FJsonSerializer::Serialize(Object, Writer);
}

bool FModPublicApiScanner::WriteReportToFile(const FModPublicApiReport& InReport, const FString& InAbsolutePath,
	FModDiagnostic& OutError)
{
	using namespace ModPublicApiScannerPrivate;

	if (InAbsolutePath.IsEmpty())
	{
		OutError = MakeScanDiagnostic(EModDiagnosticSeverity::Error, ModPublicApiCodes::ReportWriteFailed,
			TEXT("No destination path was supplied for the API report."));
		return false;
	}

	FString Json;
	if (!SerializeReportToString(InReport, Json))
	{
		OutError = MakeScanDiagnostic(EModDiagnosticSeverity::Error, ModPublicApiCodes::ReportWriteFailed,
			TEXT("The API report could not be serialised to JSON."), InAbsolutePath);
		return false;
	}

	const FString Directory = FPaths::GetPath(InAbsolutePath);
	if (!Directory.IsEmpty() && !IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true))
	{
		OutError = MakeScanDiagnostic(EModDiagnosticSeverity::Error, ModPublicApiCodes::ReportWriteFailed,
			TEXT("Could not create the directory for the API report."), Directory);
		return false;
	}

	if (!FFileHelper::SaveStringToFile(Json, *InAbsolutePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = MakeScanDiagnostic(EModDiagnosticSeverity::Error, ModPublicApiCodes::ReportWriteFailed,
			TEXT("Could not write the API report."), InAbsolutePath);
		return false;
	}

	return true;
}
