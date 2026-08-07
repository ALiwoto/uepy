#include "Assets/UEPyNiagaraAssetBridge.h"

#include "EdGraph/EdGraphNode.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
FString NormalizeNiagaraObjectPath(const FString& InputPath)
{
	FString ObjectPath = FPackageName::ExportTextPathToObjectPath(
		InputPath.TrimStartAndEnd());
	if (ObjectPath.StartsWith(TEXT("/All/")))
	{
		ObjectPath.RightChopInline(4, EAllowShrinking::No);
	}

	if (!ObjectPath.Contains(TEXT("."))
		&& FPackageName::IsValidLongPackageName(ObjectPath))
	{
		const FString AssetName =
			FPackageName::GetLongPackageAssetName(ObjectPath);
		ObjectPath = FString::Printf(
			TEXT("%s.%s"),
			*ObjectPath,
			*AssetName);
	}

	return ObjectPath;
}

EUEPyNiagaraEditResult Fail(
	const EUEPyNiagaraEditResult Result,
	const FString& Message,
	FString& OutError)
{
	OutError = Message;
	return Result;
}

FNiagaraEmitterHandle* FindEmitterHandle(
	UNiagaraSystem& System,
	const FName EmitterName)
{
	for (FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		if (Handle.GetName() == EmitterName)
		{
			return &Handle;
		}
	}
	return nullptr;
}

bool FunctionNameMatches(
	const UNiagaraNodeFunctionCall& FunctionCall,
	const FString& RequestedName)
{
	if (FunctionCall.GetFunctionName().Equals(
		RequestedName,
		ESearchCase::IgnoreCase))
	{
		return true;
	}

	return FunctionCall.FunctionScript != nullptr
		&& FunctionCall.FunctionScript->GetName().Equals(
			RequestedName,
			ESearchCase::IgnoreCase);
}

struct FUEPyResolvedLocalSpaceEdit
{
	FVersionedNiagaraEmitter VersionedEmitter;
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	bool bLocalSpace = false;
};

}

EUEPyNiagaraEditResult UUEPyNiagaraAssetBridge::EditSystem(
	const FString& SystemObjectPath,
	const TArray<FName>& EmittersToDisable,
	const TArray<FName>& EmittersToEnable,
	const TArray<FName>& EmittersToSetWorldSpace,
	const TArray<FName>& EmittersToSetLocalSpace,
	const TArray<FString>& ModulesToDisable,
	const TArray<FString>& ModulesToEnable,
	const bool bSave,
	int32& OutChangedEmitterCount,
	int32& OutChangedModuleCount,
	FString& OutError)
{
	OutChangedEmitterCount = 0;
	OutChangedModuleCount = 0;
	OutError.Reset();
	if (!IsInGameThread())
	{
		return Fail(
			EUEPyNiagaraEditResult::WrongThread,
			TEXT("Niagara asset editing must run on Unreal's game thread."),
			OutError);
	}

	const FString NormalizedSystemPath =
		NormalizeNiagaraObjectPath(SystemObjectPath);
	if (!FPackageName::IsValidObjectPath(NormalizedSystemPath))
	{
		return Fail(
			EUEPyNiagaraEditResult::InvalidSystemPath,
			TEXT("System must be a valid mounted Unreal object or package path."),
			OutError);
	}

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(
		nullptr,
		*NormalizedSystemPath,
		nullptr,
		LOAD_NoWarn | LOAD_Quiet);
	if (System == nullptr)
	{
		return Fail(
			EUEPyNiagaraEditResult::SystemNotFound,
			TEXT("Asset was not found or is not a Niagara system."),
			OutError);
	}
	System->WaitForCompilationComplete(false, false);

	TMap<FName, bool> EmitterEdits;
	auto AddEmitterEdits = [&](const TArray<FName>& EmitterNames,
		const bool bEnabled) -> EUEPyNiagaraEditResult
	{
		for (const FName EmitterName : EmitterNames)
		{
			if (EmitterName.IsNone())
			{
				return Fail(
					EUEPyNiagaraEditResult::EmitterNotFound,
					TEXT("Emitter names cannot be empty."),
					OutError);
			}
			if (const bool* ExistingState = EmitterEdits.Find(EmitterName))
			{
				if (*ExistingState != bEnabled)
				{
					return Fail(
						EUEPyNiagaraEditResult::ConflictingEdit,
						FString::Printf(
							TEXT("Emitter '%s' was requested both enabled and disabled."),
							*EmitterName.ToString()),
						OutError);
				}
				continue;
			}
			EmitterEdits.Add(EmitterName, bEnabled);
		}
		return EUEPyNiagaraEditResult::Success;
	};

	EUEPyNiagaraEditResult Result = AddEmitterEdits(
		EmittersToDisable,
		false);
	if (Result != EUEPyNiagaraEditResult::Success)
	{
		return Result;
	}
	Result = AddEmitterEdits(EmittersToEnable, true);
	if (Result != EUEPyNiagaraEditResult::Success)
	{
		return Result;
	}

	TMap<FNiagaraEmitterHandle*, bool> ResolvedEmitterEdits;
	for (const TPair<FName, bool>& Edit : EmitterEdits)
	{
		FNiagaraEmitterHandle* Handle = FindEmitterHandle(*System, Edit.Key);
		if (Handle == nullptr)
		{
			return Fail(
				EUEPyNiagaraEditResult::EmitterNotFound,
				FString::Printf(
					TEXT("Emitter '%s' was not found in '%s'."),
					*Edit.Key.ToString(),
					*NormalizedSystemPath),
				OutError);
		}
		ResolvedEmitterEdits.Add(Handle, Edit.Value);
	}

	TMap<FName, bool> LocalSpaceEdits;
	auto AddLocalSpaceEdits = [&](const TArray<FName>& EmitterNames,
		const bool bLocalSpace) -> EUEPyNiagaraEditResult
	{
		for (const FName EmitterName : EmitterNames)
		{
			if (EmitterName.IsNone())
			{
				return Fail(
					EUEPyNiagaraEditResult::EmitterNotFound,
					TEXT("Emitter names cannot be empty."),
					OutError);
			}
			if (const bool* ExistingSpace = LocalSpaceEdits.Find(EmitterName))
			{
				if (*ExistingSpace != bLocalSpace)
				{
					return Fail(
						EUEPyNiagaraEditResult::ConflictingEdit,
						FString::Printf(
							TEXT("Emitter '%s' was requested as both world-space and local-space."),
							*EmitterName.ToString()),
						OutError);
				}
				continue;
			}
			LocalSpaceEdits.Add(EmitterName, bLocalSpace);
		}
		return EUEPyNiagaraEditResult::Success;
	};

	Result = AddLocalSpaceEdits(EmittersToSetWorldSpace, false);
	if (Result != EUEPyNiagaraEditResult::Success)
	{
		return Result;
	}
	Result = AddLocalSpaceEdits(EmittersToSetLocalSpace, true);
	if (Result != EUEPyNiagaraEditResult::Success)
	{
		return Result;
	}

	TArray<FUEPyResolvedLocalSpaceEdit> ResolvedLocalSpaceEdits;
	for (const TPair<FName, bool>& Edit : LocalSpaceEdits)
	{
		FNiagaraEmitterHandle* Handle = FindEmitterHandle(*System, Edit.Key);
		if (Handle == nullptr)
		{
			return Fail(
				EUEPyNiagaraEditResult::EmitterNotFound,
				FString::Printf(
					TEXT("Emitter '%s' was not found in '%s'."),
					*Edit.Key.ToString(),
					*NormalizedSystemPath),
				OutError);
		}
		if (Handle->GetEmitterMode() != ENiagaraEmitterMode::Standard)
		{
			return Fail(
				EUEPyNiagaraEditResult::UnsupportedEmitter,
				FString::Printf(
					TEXT("Emitter '%s' is stateless and has no local-space setting."),
					*Edit.Key.ToString()),
				OutError);
		}

		const FVersionedNiagaraEmitter VersionedEmitter =
			Handle->GetInstance();
		FVersionedNiagaraEmitterData* EmitterData =
			VersionedEmitter.GetEmitterData();
		if (VersionedEmitter.Emitter == nullptr || EmitterData == nullptr)
		{
			return Fail(
				EUEPyNiagaraEditResult::UnsupportedEmitter,
				FString::Printf(
					TEXT("Emitter '%s' has no editable version data."),
					*Edit.Key.ToString()),
				OutError);
		}

		ResolvedLocalSpaceEdits.Add({
			VersionedEmitter,
			EmitterData,
			Edit.Value});
	}

	TMap<UNiagaraNodeFunctionCall*, bool> ResolvedModuleEdits;
	auto ResolveModuleEdits = [&](const TArray<FString>& Selectors,
		const bool bEnabled) -> EUEPyNiagaraEditResult
	{
		for (const FString& UntrimmedSelector : Selectors)
		{
			const FString Selector = UntrimmedSelector.TrimStartAndEnd();
			FString EmitterNameString;
			FString ModuleName;
			if (!Selector.Split(
				TEXT(":"),
				&EmitterNameString,
				&ModuleName,
				ESearchCase::CaseSensitive,
				ESearchDir::FromStart))
			{
				return Fail(
					EUEPyNiagaraEditResult::InvalidModuleSelector,
					FString::Printf(
						TEXT("Module selector '%s' must use 'EmitterName:ModuleName'."),
						*Selector),
					OutError);
			}
			EmitterNameString.TrimStartAndEndInline();
			ModuleName.TrimStartAndEndInline();
			if (EmitterNameString.IsEmpty() || ModuleName.IsEmpty())
			{
				return Fail(
					EUEPyNiagaraEditResult::InvalidModuleSelector,
					FString::Printf(
						TEXT("Module selector '%s' contains an empty name."),
						*Selector),
					OutError);
			}

			FNiagaraEmitterHandle* Handle = FindEmitterHandle(
				*System,
				FName(*EmitterNameString));
			if (Handle == nullptr)
			{
				return Fail(
					EUEPyNiagaraEditResult::EmitterNotFound,
					FString::Printf(
						TEXT("Emitter '%s' from selector '%s' was not found."),
						*EmitterNameString,
						*Selector),
					OutError);
			}
			if (Handle->GetEmitterMode() != ENiagaraEmitterMode::Standard)
			{
				return Fail(
					EUEPyNiagaraEditResult::UnsupportedEmitter,
					FString::Printf(
						TEXT("Emitter '%s' is stateless and has no editable Niagara graph."),
						*EmitterNameString),
					OutError);
			}

			FVersionedNiagaraEmitterData* EmitterData =
				Handle->GetEmitterData();
			UNiagaraScriptSource* ScriptSource = EmitterData != nullptr
				? Cast<UNiagaraScriptSource>(EmitterData->GraphSource)
				: nullptr;
			UNiagaraGraph* Graph = ScriptSource != nullptr
				? ScriptSource->NodeGraph
				: nullptr;
			if (Graph == nullptr)
			{
				return Fail(
					EUEPyNiagaraEditResult::UnsupportedEmitter,
					FString::Printf(
						TEXT("Emitter '%s' has no editable Niagara graph."),
						*EmitterNameString),
					OutError);
			}

			TArray<UNiagaraNodeFunctionCall*> Matches;
			Graph->GetNodesOfClass(Matches);
			Matches.RemoveAll([&](const UNiagaraNodeFunctionCall* FunctionCall)
			{
				return FunctionCall == nullptr
					|| !FunctionNameMatches(*FunctionCall, ModuleName);
			});
			if (Matches.IsEmpty())
			{
				return Fail(
					EUEPyNiagaraEditResult::ModuleNotFound,
					FString::Printf(
						TEXT("Module '%s' was not found in emitter '%s'."),
						*ModuleName,
						*EmitterNameString),
					OutError);
			}
			if (Matches.Num() > 1)
			{
				return Fail(
					EUEPyNiagaraEditResult::ModuleAmbiguous,
					FString::Printf(
						TEXT("Module '%s' occurs %d times in emitter '%s'."),
						*ModuleName,
						Matches.Num(),
						*EmitterNameString),
					OutError);
			}

			UNiagaraNodeFunctionCall* FunctionCall = Matches[0];
			if (const bool* ExistingState =
				ResolvedModuleEdits.Find(FunctionCall))
			{
				if (*ExistingState != bEnabled)
				{
					return Fail(
						EUEPyNiagaraEditResult::ConflictingEdit,
						FString::Printf(
							TEXT("Module selector '%s' was requested both enabled and disabled."),
							*Selector),
						OutError);
				}
				continue;
			}
			ResolvedModuleEdits.Add(FunctionCall, bEnabled);
		}
		return EUEPyNiagaraEditResult::Success;
	};

	Result = ResolveModuleEdits(ModulesToDisable, false);
	if (Result != EUEPyNiagaraEditResult::Success)
	{
		return Result;
	}
	Result = ResolveModuleEdits(ModulesToEnable, true);
	if (Result != EUEPyNiagaraEditResult::Success)
	{
		return Result;
	}

	bool bHasChanges = false;
	for (const TPair<FNiagaraEmitterHandle*, bool>& Edit
		: ResolvedEmitterEdits)
	{
		bHasChanges |= Edit.Key->GetIsEnabled() != Edit.Value;
	}
	for (const TPair<UNiagaraNodeFunctionCall*, bool>& Edit
		: ResolvedModuleEdits)
	{
		const ENodeEnabledState DesiredState = Edit.Value
			? ENodeEnabledState::Enabled
			: ENodeEnabledState::Disabled;
		bHasChanges |= Edit.Key->GetDesiredEnabledState() != DesiredState;
	}
	for (const FUEPyResolvedLocalSpaceEdit& Edit : ResolvedLocalSpaceEdits)
	{
		bHasChanges |= Edit.EmitterData->bLocalSpace != Edit.bLocalSpace;
	}
	if (!bHasChanges)
	{
		return EUEPyNiagaraEditResult::Success;
	}

	System->Modify();
	for (const TPair<FNiagaraEmitterHandle*, bool>& Edit
		: ResolvedEmitterEdits)
	{
		if (Edit.Key->SetIsEnabled(Edit.Value, *System, false))
		{
			++OutChangedEmitterCount;
		}
	}

	TArray<FUEPyResolvedLocalSpaceEdit*> ChangedLocalSpaceEdits;
	for (FUEPyResolvedLocalSpaceEdit& Edit : ResolvedLocalSpaceEdits)
	{
		if (Edit.EmitterData->bLocalSpace == Edit.bLocalSpace)
		{
			continue;
		}
		Edit.VersionedEmitter.Emitter->Modify();
		Edit.EmitterData->bLocalSpace = Edit.bLocalSpace;
		ChangedLocalSpaceEdits.Add(&Edit);
		++OutChangedEmitterCount;
	}
	if (!ChangedLocalSpaceEdits.IsEmpty())
	{
		FProperty* LocalSpaceProperty = FindFProperty<FProperty>(
			FVersionedNiagaraEmitterData::StaticStruct(),
			GET_MEMBER_NAME_CHECKED(
				FVersionedNiagaraEmitterData,
				bLocalSpace));
		check(LocalSpaceProperty != nullptr);
		for (FUEPyResolvedLocalSpaceEdit* Edit : ChangedLocalSpaceEdits)
		{
			FPropertyChangedEvent PropertyChangedEvent(
				LocalSpaceProperty,
				EPropertyChangeType::ValueSet);
			Edit->VersionedEmitter.Emitter->PostEditChangeVersionedProperty(
				PropertyChangedEvent,
				Edit->VersionedEmitter.Version);
		}
	}

	for (const TPair<UNiagaraNodeFunctionCall*, bool>& Edit
		: ResolvedModuleEdits)
	{
		const ENodeEnabledState DesiredState = Edit.Value
			? ENodeEnabledState::Enabled
			: ENodeEnabledState::Disabled;
		if (Edit.Key->GetDesiredEnabledState() == DesiredState)
		{
			continue;
		}
		Edit.Key->Modify();
		Edit.Key->SetEnabledState(DesiredState, true);
		Edit.Key->MarkNodeRequiresSynchronization(
			TEXT("uepy Niagara asset edit"),
			true);
		++OutChangedModuleCount;
	}

	if (OutChangedEmitterCount > 0 || OutChangedModuleCount > 0)
	{
		System->MarkPackageDirty();
		System->RequestCompile(false);
	}

	if (!bSave)
	{
		return EUEPyNiagaraEditResult::Success;
	}
	System->WaitForCompilationComplete(false, false);

	UPackage* Package = System->GetOutermost();
	const FString PackageName = Package->GetName();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArguments;
	SaveArguments.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArguments.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(
		Package,
		System,
		*PackageFilename,
		SaveArguments))
	{
		return Fail(
			EUEPyNiagaraEditResult::SaveFailed,
			FString::Printf(
				TEXT("Niagara system package '%s' could not be saved."),
				*PackageName),
			OutError);
	}

	return EUEPyNiagaraEditResult::Success;
}
