// Copyright (c) 2026. Licensed for use in your own projects.

#include "Validation/GameModValidator.h"

#include "API/GameCombatModAPI.h"
#include "API/GameUIModAPI.h"
#include "API/GameWorldModAPI.h"
#include "Containers/Array.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Core/ModFrameworkTypes.h"
#include "Extensions/GameModSDKExtensionPoints.h"
#include "Extensions/ModExtension.h"
#include "GameModSDKVersion.h"
#include "Manifest/ModManifest.h"
#include "Manifest/ModVersion.h"
#include "Misc/CString.h"
#include "Permissions/ModPermissions.h"
#include "Settings/ModFrameworkSettings.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/NameTypes.h"
#include "UObject/SoftObjectPath.h"

namespace GameModValidationCodes
{
	const FName UnknownExtensionPoint(TEXT("GameMod.UnknownExtensionPoint"));
	const FName ClaimMissingExtensionPoint(TEXT("GameMod.ClaimMissingExtensionPoint"));
	const FName UnknownPermission(TEXT("GameMod.UnknownPermission"));
	const FName MissingPermissionForClaim(TEXT("GameMod.MissingPermissionForClaim"));
	const FName SdkMismatch(TEXT("GameMod.SdkMismatch"));
	const FName SdkVersionRangeInvalid(TEXT("GameMod.SdkVersionRangeInvalid"));
	const FName SdkVersionUnsatisfiable(TEXT("GameMod.SdkVersionUnsatisfiable"));
	const FName SdkNotDeclared(TEXT("GameMod.SdkNotDeclared"));
	const FName SdkIdentityDrift(TEXT("GameMod.SdkIdentityDrift"));
	const FName ContentBundleWithoutEntryPoint(TEXT("GameMod.ContentBundleWithoutEntryPoint"));
	const FName NoRuntimeSurface(TEXT("GameMod.NoRuntimeSurface"));
	const FName NoClaimsDeclared(TEXT("GameMod.NoClaimsDeclared"));

	TArray<FName> GetAllCodes()
	{
		return TArray<FName>({
			UnknownExtensionPoint,
			ClaimMissingExtensionPoint,
			UnknownPermission,
			MissingPermissionForClaim,
			SdkMismatch,
			SdkVersionRangeInvalid,
			SdkVersionUnsatisfiable,
			SdkNotDeclared,
			SdkIdentityDrift,
			ContentBundleWithoutEntryPoint,
			NoRuntimeSurface,
			NoClaimsDeclared });
	}
}

int32 FGameModValidationResult::CountErrors() const
{
	int32 Count = 0;
	for (const FModDiagnostic& Diagnostic : Diagnostics)
	{
		Count += Diagnostic.Severity == EModDiagnosticSeverity::Error ? 1 : 0;
	}

	return Count;
}

int32 FGameModValidationResult::CountWarnings() const
{
	int32 Count = 0;
	for (const FModDiagnostic& Diagnostic : Diagnostics)
	{
		Count += Diagnostic.Severity == EModDiagnosticSeverity::Warning ? 1 : 0;
	}

	return Count;
}

FString FGameModValidationResult::ToDebugString() const
{
	return ModDiagnostics::Join(Diagnostics, TEXT("\n"));
}

FString FGameModValidationResult::ToSummaryString() const
{
	const int32 Errors = CountErrors();
	const int32 Warnings = CountWarnings();

	if (Errors == 0 && Warnings == 0)
	{
		return Diagnostics.Num() > 0
			? FString::Printf(TEXT("no errors or warnings, %d note%s"), Diagnostics.Num(), Diagnostics.Num() == 1 ? TEXT("") : TEXT("s"))
			: FString(TEXT("no findings"));
	}

	return FString::Printf(TEXT("%d error%s, %d warning%s"),
		Errors, Errors == 1 ? TEXT("") : TEXT("s"),
		Warnings, Warnings == 1 ? TEXT("") : TEXT("s"));
}

namespace GameModValidatorPrivate
{
	/**
	 * Collects diagnostics and stamps each one with the manifest's mod id, so a packaging run over a
	 * directory of mods can merge every result into one list and still attribute each line.
	 *
	 * Deliberately not named after anything in Templates/ValueOrError.h: a file-local `MakeError`
	 * collides with the engine's global variadic template and produces an unreadable error at every
	 * call site reached through a using-directive.
	 */
	struct FDiagnosticSink
	{
		FDiagnosticSink(const FModId InModId, TArray<FModDiagnostic>& InOut)
			: ModId(InModId)
			, Out(InOut)
		{
		}

		void Add(const EModDiagnosticSeverity Severity, const FName Code, FString Message, FString Context) const
		{
			FModDiagnostic Diagnostic(Severity, Code, MoveTemp(Message), MoveTemp(Context));
			Diagnostic.ModId = ModId;
			Out.Add(MoveTemp(Diagnostic));
		}

		void Error(const FName Code, FString Message, FString Context) const
		{
			Add(EModDiagnosticSeverity::Error, Code, MoveTemp(Message), MoveTemp(Context));
		}

		void Warning(const FName Code, FString Message, FString Context) const
		{
			Add(EModDiagnosticSeverity::Warning, Code, MoveTemp(Message), MoveTemp(Context));
		}

		void Info(const FName Code, FString Message, FString Context) const
		{
			Add(EModDiagnosticSeverity::Info, Code, MoveTemp(Message), MoveTemp(Context));
		}

		FModId ModId;
		TArray<FModDiagnostic>& Out;
	};

	/** "game.weapon, game.enemy, game.gamerule, game.ui". Empty string for an empty list. */
	FString JoinNames(const TArray<FName>& In)
	{
		TArray<FString> Strings;
		Strings.Reserve(In.Num());
		for (const FName Name : In)
		{
			Strings.Add(Name.ToString());
		}

		return FString::Join(Strings, TEXT(", "));
	}

	/** The mod's own name for a message, falling back to its id, then to a placeholder. */
	FString DescribeMod(const FModManifest& Manifest)
	{
		const FString Name = Manifest.GetDisplayNameOrId();
		return Name.IsEmpty() ? FString(TEXT("This mod")) : FString::Printf(TEXT("'%s'"), *Name);
	}

	/** The raw range text, with the empty expression spelled the way a manifest would spell it. */
	FString DescribeRange(const FModVersionRange& In)
	{
		return In.Expression.IsEmpty() ? FString(TEXT("*")) : In.Expression;
	}

	/** "claims[2].extensionPoint" - the same field-path form FModManifestParser uses for context. */
	FString MakeFieldPath(const TCHAR* Field, const int32 Index, const TCHAR* Leaf)
	{
		return Leaf != nullptr
			? FString::Printf(TEXT("%s[%d].%s"), Field, Index, Leaf)
			: FString::Printf(TEXT("%s[%d]"), Field, Index);
	}

	/**
	 * Every permission id this SDK's surface can require, on top of the framework builtins.
	 *
	 * Derived from the extension point descriptors and from each UModAPI's own static accessor rather
	 * than hand-listed, so the known set cannot drift away from what the SDK actually enforces.
	 */
	void AppendSdkPermissions(const TArray<FModExtensionPointDescriptor>& Points, TArray<FName>& InOut)
	{
		for (const FModExtensionPointDescriptor& Point : Points)
		{
			for (const FName Permission : Point.RequiredPermissions)
			{
				if (!Permission.IsNone())
				{
					InOut.AddUnique(Permission);
				}
			}
		}

		// The three APIs gate themselves with a permission apiece; a mod that never asks for the
		// permission never receives the API from UModContext::RequestAPI.
		InOut.AddUnique(UGameCombatModAPI::GetStaticRequiredPermission());
		InOut.AddUnique(UGameWorldModAPI::GetStaticRequiredPermission());
		InOut.AddUnique(UGameUIModAPI::GetStaticRequiredPermission());

		InOut.RemoveAll([](const FName In) { return In.IsNone(); });
		InOut.Sort([](const FName A, const FName B) { return A.LexicalLess(B); });
	}

	//~ ---------------------------------------------------------------------------------------------
	//~ The checks.
	//~ ---------------------------------------------------------------------------------------------

	/**
	 * `sdk.id` must be this SDK, and `sdk.version` must be a range this SDK's version can satisfy.
	 *
	 * Mirrors FModDependencyResolver's SDK block deliberately, with one difference in severity: the
	 * resolver only *warns* about a malformed range and then treats it as "any version", because at
	 * load time refusing a mod over punctuation would be worse than running it. At package time the
	 * author is right there and the constraint they wrote is being silently discarded, so it is an
	 * error here.
	 */
	void ValidateSdkRequirement(const FModManifest& Manifest, const FDiagnosticSink& Sink)
	{
		const FModSdkRequirement& Sdk = Manifest.Sdk;
		const FString SdkId = GameModSDK::GetSdkId();
		const FModVersion SdkVersion = GameModSDK::GetSdkVersion();
		const FString Who = DescribeMod(Manifest);

		if (Sdk.SdkId.IsEmpty())
		{
			// An empty id is legal - the resolver reads it as "this mod does not care which SDK is
			// present" and skips the SDK check entirely. That is fine for a mod that only replaces
			// assets, and a trap for one that plugs into this game's surface: nothing will stop it
			// installing against a future SDK that removed what it uses.
			const bool bUsesSdkSurface =
				Manifest.Claims.Num() > 0 ||
				Manifest.RequestedPermissions.Num() > 0 ||
				!Manifest.EntryPoint.EntryClass.IsNull() ||
				Manifest.EntryPoint.ContentBundles.Num() > 0;

			FString Message = FString::Printf(
				TEXT("%s declares no 'sdk.id'. Nothing then checks it against this game's modding surface, in this build or in any later one. Add \"sdk\": { \"id\": \"%s\", \"version\": \"^%s\" }."),
				*Who, *SdkId, *SdkVersion.ToString());

			if (!Sdk.VersionRange.Expression.IsEmpty())
			{
				// Same reading as the resolver: a version with no id to hang it on is ignored outright.
				Message = FString::Printf(
					TEXT("%s requires SDK version %s but names no 'sdk.id', so the requirement is ignored entirely. Add \"sdk\": { \"id\": \"%s\", \"version\": \"%s\" }."),
					*Who, *DescribeRange(Sdk.VersionRange), *SdkId, *DescribeRange(Sdk.VersionRange));
			}

			Sink.Add(bUsesSdkSurface || !Sdk.VersionRange.Expression.IsEmpty()
					? EModDiagnosticSeverity::Warning
					: EModDiagnosticSeverity::Info,
				GameModValidationCodes::SdkNotDeclared, MoveTemp(Message), TEXT("sdk.id"));
			return;
		}

		if (!Sdk.SdkId.Equals(SdkId, ESearchCase::IgnoreCase))
		{
			Sink.Error(GameModValidationCodes::SdkMismatch, FString::Printf(
				TEXT("%s requires the modding SDK '%s', but this is '%s'. A mod built against another game's SDK is rejected at load with IncompatibleSdk."),
				*Who, *Sdk.SdkId, *SdkId), TEXT("sdk.id"));
			return;
		}

		if (!Sdk.VersionRange.IsValid())
		{
			Sink.Error(GameModValidationCodes::SdkVersionRangeInvalid, FString::Printf(
				TEXT("'%s' is not a valid version range for 'sdk.version', so the constraint is discarded and the mod is accepted against every SDK version. Use a form like \"^%s\", \"~%s\", \">=%s <1.0.0\" or \"*\"."),
				*Sdk.VersionRange.Expression, *SdkVersion.ToString(), *SdkVersion.ToString(), *SdkVersion.ToString()),
				TEXT("sdk.version"));
			return;
		}

		if (!Sdk.VersionRange.Satisfies(SdkVersion))
		{
			FString Message = FString::Printf(
				TEXT("%s requires SDK version %s (%s), but this SDK is '%s' %s. No build of this mod can ever load here."),
				*Who, *DescribeRange(Sdk.VersionRange), *Sdk.VersionRange.Describe(), *SdkId, *SdkVersion.ToString());

			if (SdkVersion.Major == 0)
			{
				// The commonest way to land here: an author writes "^1.0.0" or "^2.0.0" out of habit
				// against a pre-1.0 SDK, where caret ranges are pinned to the minor rather than the
				// major and every one of those admits nothing this SDK will ever publish before 1.0.
				Message += FString::Printf(
					TEXT(" This SDK is still pre-1.0, where \"^MAJOR.MINOR.PATCH\" admits only that same minor; pin \"^%d.%d.%d\"."),
					SdkVersion.Major, SdkVersion.Minor, SdkVersion.Patch);
			}

			Sink.Error(GameModValidationCodes::SdkVersionUnsatisfiable, MoveTemp(Message), TEXT("sdk.version"));
		}
	}

	/** Every claim must name an extension point this game actually opens. */
	void ValidateClaims(const FModManifest& Manifest, const FDiagnosticSink& Sink)
	{
		const TArray<FName> KnownPoints = GameModExtensionPoints::GetAllPointIds();
		const FString KnownList = JoinNames(KnownPoints);

		for (int32 Index = 0; Index < Manifest.Claims.Num(); ++Index)
		{
			const FName PointId = Manifest.Claims[Index].ExtensionPointId;

			if (PointId.IsNone())
			{
				Sink.Error(GameModValidationCodes::ClaimMissingExtensionPoint, FString::Printf(
					TEXT("Claim %d names no extension point, so it can never match anything and contributes nothing to conflict detection. This game opens: %s."),
					Index, *KnownList),
					MakeFieldPath(TEXT("claims"), Index, TEXT("extensionPoint")));
				continue;
			}

			if (!KnownPoints.Contains(PointId))
			{
				Sink.Error(GameModValidationCodes::UnknownExtensionPoint, FString::Printf(
					TEXT("Claim %d names the extension point '%s', which this game does not open. An extension registered against it is refused with Extension.PointNotFound, and the claim itself is inert. This game opens: %s."),
					Index, *PointId.ToString(), *KnownList),
					MakeFieldPath(TEXT("claims"), Index, TEXT("extensionPoint")));
			}
		}
	}

	/**
	 * Every requested permission should be one somebody actually registers, and every point a mod
	 * claims should have its permission requested.
	 *
	 * Both are warnings rather than errors because a game may register further permissions of its own
	 * at runtime, which is invisible from a mod author's project, and because a claim is a
	 * declaration of intent rather than a registration.
	 */
	void ValidatePermissions(
		const FModManifest& Manifest,
		const TArray<FModExtensionPointDescriptor>& Points,
		const TArray<FName>& KnownPermissions,
		const FDiagnosticSink& Sink)
	{
		const FString Who = DescribeMod(Manifest);
		const FString KnownList = JoinNames(KnownPermissions);

		TSet<FName> Requested;
		Requested.Reserve(Manifest.RequestedPermissions.Num());

		for (int32 Index = 0; Index < Manifest.RequestedPermissions.Num(); ++Index)
		{
			const FName Permission = Manifest.RequestedPermissions[Index];
			if (Permission.IsNone())
			{
				// An empty permission name is a shape problem, and FModManifestParser already reports
				// it as Manifest.InvalidPermission. Repeating it here would only add noise.
				continue;
			}

			Requested.Add(Permission);

			if (!KnownPermissions.Contains(Permission))
			{
				Sink.Warning(GameModValidationCodes::UnknownPermission, FString::Printf(
					TEXT("'%s' is not a permission this game defines, so it can never be granted: UModPermissionRegistry leaves an unregistered permission Pending, or denies it outright when bDenyUnknownPermissions is set. A typo here fails silently at run time. Known permissions: %s."),
					*Permission.ToString(), *KnownList),
					MakeFieldPath(TEXT("permissions"), Index, nullptr));
			}
		}

		// A claim says "I intend to modify this point". Registering an extension there needs the
		// point's RequiredPermissions, which UModExtensionRegistry checks through the permission
		// callback UModSubsystem binds for it - so a mod that claims a point and never asks for its
		// permission gets Extension.PermissionDenied and, from the player's side, nothing at all.
		TSet<FName> ClaimedPoints;
		ClaimedPoints.Reserve(Manifest.Claims.Num());
		for (const FModResourceClaimDeclaration& Claim : Manifest.Claims)
		{
			if (!Claim.ExtensionPointId.IsNone())
			{
				ClaimedPoints.Add(Claim.ExtensionPointId);
			}
		}

		// Iterated over the descriptors rather than over the claims, so the order of the findings is
		// the SDK's declaration order and one point produces one finding however many times it is
		// claimed.
		for (const FModExtensionPointDescriptor& Point : Points)
		{
			if (!ClaimedPoints.Contains(Point.ExtensionPointId))
			{
				continue;
			}

			for (const FName Required : Point.RequiredPermissions)
			{
				if (Required.IsNone() || Requested.Contains(Required))
				{
					continue;
				}

				Sink.Warning(GameModValidationCodes::MissingPermissionForClaim, FString::Printf(
					TEXT("%s claims the extension point '%s', which requires the permission '%s', but 'permissions' does not request it. Any extension the mod registers there is refused with Extension.PermissionDenied."),
					*Who, *Point.ExtensionPointId.ToString(), *Required.ToString()),
					TEXT("permissions"));
			}
		}
	}

	/**
	 * Whether the mod has anything that can run, and whether conflict detection will be able to see
	 * what it does.
	 */
	void ValidateEntryPoint(const FModManifest& Manifest, const FDiagnosticSink& Sink)
	{
		const FModEntryPoint& EntryPoint = Manifest.EntryPoint;
		const FString Who = DescribeMod(Manifest);

		const bool bHasEntryClass = !EntryPoint.EntryClass.IsNull();
		const bool bHasBundles = EntryPoint.ContentBundles.Num() > 0;
		const bool bHasNativeModule = !EntryPoint.NativeModule.IsEmpty();
		const bool bHasClaims = Manifest.Claims.Num() > 0;
		const bool bHasPermissions = Manifest.RequestedPermissions.Num() > 0;

		// The brief for this validator expected "content bundles with no entryPoint.class" to be an
		// error - a bundle with nothing to register it. The framework says otherwise, and the
		// framework wins: UModSubsystem::LoadMod calls a missing entry class "entirely legal, and the
		// common case for a content-only mod", and UModSubsystem::ApplyContentBundles instantiates and
		// registers every class a bundle names without an entry point being involved at all.
		// UModContentBundle's own header calls itself the "no C++ required" path, explicitly "without
		// shipping a single line of code or even an entry point class". So this is a note, not a
		// defect: it tells an author who expected to write code that they have not, and blocks nothing.
		if (bHasBundles && !bHasEntryClass)
		{
			Sink.Info(GameModValidationCodes::ContentBundleWithoutEntryPoint, FString::Printf(
				TEXT("%s declares %d content bundle%s and no 'entryPoint.class'. That is the supported content-only shape - the framework instantiates and registers everything the bundles name by itself - so this is only worth checking if the mod was meant to run code of its own."),
				*Who, EntryPoint.ContentBundles.Num(), EntryPoint.ContentBundles.Num() == 1 ? TEXT("") : TEXT("s")),
				TEXT("entryPoint.class"));
		}

		// The genuinely dead shape: nothing to load, nothing to register, yet the manifest declares
		// intent. Native modules count as a surface even though the MVP does not load one, because a
		// manifest naming a native module has told the truth about what it ships.
		if (!bHasEntryClass && !bHasBundles && !bHasNativeModule && (bHasClaims || bHasPermissions))
		{
			Sink.Warning(GameModValidationCodes::NoRuntimeSurface, FString::Printf(
				TEXT("%s declares %s but has no 'entryPoint.class', no 'entryPoint.contentBundles' and no 'entryPoint.nativeModule', so it can never register anything. It will load, do nothing, and report no error."),
				*Who,
				bHasClaims && bHasPermissions ? TEXT("claims and permissions")
					: bHasClaims ? TEXT("claims") : TEXT("permissions")),
				TEXT("entryPoint"));
		}

		// Conflict detection runs over declared claims. UModSubsystem also folds in the claims of
		// already-registered extensions, but those only exist once a mod has activated - during the
		// cold pass that decides load order and rejects conflicting mods, the manifest's claims are
		// all there is. A mod that declares none is invisible to it until it is already running.
		if (!bHasClaims && (bHasEntryClass || bHasBundles))
		{
			Sink.Warning(GameModValidationCodes::NoClaimsDeclared, FString::Printf(
				TEXT("%s will register extensions but declares no 'claims', so conflict detection cannot see what it touches: two mods retuning the same resource both apply silently and the player is never told. Declare a claim per resource the mod replaces. A mod that only adds new resources has nothing to declare and can ignore this."),
				*Who),
				TEXT("claims"));
		}
	}
}

FGameModValidationResult UGameModValidator::ValidateManifest(const FModManifest& Manifest)
{
	using namespace GameModValidatorPrivate;

	FGameModValidationResult Result;
	const FDiagnosticSink Sink(Manifest.Id, Result.Diagnostics);

	// Built once and shared: the descriptors are the single source of truth for which permissions a
	// point requires, and rebuilding them per check would be the start of two answers to one question.
	const TArray<FModExtensionPointDescriptor> Points = BuildGameExtensionPointDescriptors();

	TArray<FName> KnownPermissions = ModPermissions::GetBuiltinPermissions();
	AppendSdkPermissions(Points, KnownPermissions);

	ValidateSdkRequirement(Manifest, Sink);
	ValidateClaims(Manifest, Sink);
	ValidatePermissions(Manifest, Points, KnownPermissions, Sink);
	ValidateEntryPoint(Manifest, Sink);

	Result.bValid = !ModDiagnostics::HasErrors(Result.Diagnostics);
	return Result;
}

FGameModValidationResult UGameModValidator::ValidateSdkEnvironment()
{
	using namespace GameModValidatorPrivate;

	FGameModValidationResult Result;
	const FDiagnosticSink Sink(FModId(), Result.Diagnostics);

	const FString SdkId = GameModSDK::GetSdkId();
	const FModVersion SdkVersion = GameModSDK::GetSdkVersion();

	const UModFrameworkSettings* Settings = UModFrameworkSettings::Get();
	if (Settings == nullptr)
	{
		Sink.Info(GameModValidationCodes::SdkIdentityDrift, FString::Printf(
			TEXT("This build ships the modding SDK '%s' %s. The mod framework settings were not available, so the project's declared SDK identity could not be compared against it."),
			*SdkId, *SdkVersion.ToString()),
			TEXT("ModFrameworkSettings"));

		Result.bValid = !ModDiagnostics::HasErrors(Result.Diagnostics);
		return Result;
	}

	if (Settings->SdkId.IsEmpty())
	{
		Sink.Warning(GameModValidationCodes::SdkIdentityDrift, FString::Printf(
			TEXT("This build ships the modding SDK '%s' %s, but the project configures no SdkId under Project Settings > Plugins > Mod Framework. FModEnvironment::FromSettings then reports no SDK at all, and every mod that pins one is rejected at load with IncompatibleSdk."),
			*SdkId, *SdkVersion.ToString()),
			TEXT("ModFrameworkSettings.SdkId"));
	}
	else if (!Settings->SdkId.Equals(SdkId, ESearchCase::IgnoreCase))
	{
		Sink.Warning(GameModValidationCodes::SdkIdentityDrift, FString::Printf(
			TEXT("The project declares SdkId '%s' but this build ships the SDK '%s'. Mods validated here against '%s' are matched at load time against '%s', so packaging and loading will disagree."),
			*Settings->SdkId, *SdkId, *SdkId, *Settings->SdkId),
			TEXT("ModFrameworkSettings.SdkId"));
	}

	if (Settings->SdkVersion.IsEmpty())
	{
		Sink.Warning(GameModValidationCodes::SdkIdentityDrift, FString::Printf(
			TEXT("The project configures no SdkVersion, so mods pinning an SDK version range are accepted at load without being checked, whatever they pinned. This SDK is %s."),
			*SdkVersion.ToString()),
			TEXT("ModFrameworkSettings.SdkVersion"));
	}
	else
	{
		FModVersion Configured;
		FString ParseError;
		if (!FModVersion::Parse(Settings->SdkVersion, Configured, &ParseError))
		{
			Sink.Warning(GameModValidationCodes::SdkIdentityDrift, FString::Printf(
				TEXT("The project's SdkVersion '%s' is not a valid semantic version (%s), so SDK version requirements are not enforced at load. This SDK is %s."),
				*Settings->SdkVersion, *ParseError, *SdkVersion.ToString()),
				TEXT("ModFrameworkSettings.SdkVersion"));
		}
		else if (Configured != SdkVersion)
		{
			Sink.Warning(GameModValidationCodes::SdkIdentityDrift, FString::Printf(
				TEXT("The project declares SdkVersion %s but this build ships SDK %s. A mod whose range this validator accepts against %s can still be refused at load against %s."),
				*Configured.ToString(), *SdkVersion.ToString(), *SdkVersion.ToString(), *Configured.ToString()),
				TEXT("ModFrameworkSettings.SdkVersion"));
		}
	}

	// Nothing above is ever an Error: a misconfigured project is the game developer's problem to fix,
	// not a reason to refuse to package somebody's mod. Computed rather than assigned true, so a
	// future check that does raise an error cannot leave this saying "valid" by accident.
	Result.bValid = !ModDiagnostics::HasErrors(Result.Diagnostics);
	return Result;
}

bool UGameModValidator::IsKnownExtensionPoint(const FName ExtensionPointId)
{
	if (ExtensionPointId.IsNone())
	{
		return false;
	}

	return GameModExtensionPoints::GetAllPointIds().Contains(ExtensionPointId);
}

TArray<FName> UGameModValidator::GetKnownPermissionIds()
{
	using namespace GameModValidatorPrivate;

	TArray<FName> Known = ModPermissions::GetBuiltinPermissions();
	AppendSdkPermissions(BuildGameExtensionPointDescriptors(), Known);

	return Known;
}
