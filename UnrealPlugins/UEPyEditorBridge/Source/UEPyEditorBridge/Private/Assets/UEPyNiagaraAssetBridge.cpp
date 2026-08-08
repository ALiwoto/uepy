#include "Assets/UEPyNiagaraAssetBridge.h"
#include "Assets/UEPyNiagaraAssetBridgeInternal.h"

#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace UEPyNiagaraAssetBridgeInternal
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

bool SplitModuleInputSelector(
	const FString& Selector,
	FString& OutEmitterName,
	FString& OutModuleName,
	FString& OutInputName)
{
	FString Remainder;
	if (!Selector.Split(
		TEXT(":"),
		&OutEmitterName,
		&Remainder,
		ESearchCase::CaseSensitive,
		ESearchDir::FromStart)
		|| !Remainder.Split(
			TEXT(":"),
			&OutModuleName,
			&OutInputName,
			ESearchCase::CaseSensitive,
			ESearchDir::FromStart))
	{
		return false;
	}

	OutEmitterName.TrimStartAndEndInline();
	OutModuleName.TrimStartAndEndInline();
	OutInputName.TrimStartAndEndInline();
	return !OutEmitterName.IsEmpty()
		&& !OutModuleName.IsEmpty()
		&& !OutInputName.IsEmpty();
}

EUEPyNiagaraEditResult ResolveModuleInput(
	UNiagaraSystem& System,
	const FString& Selector,
	UNiagaraNodeFunctionCall*& OutFunctionCall,
	FNiagaraVariable& OutInputVariable,
	FString& OutError)
{
	FString EmitterName;
	FString ModuleName;
	FString InputName;
	if (!SplitModuleInputSelector(
		Selector,
		EmitterName,
		ModuleName,
		InputName))
	{
		return Fail(
			EUEPyNiagaraEditResult::InvalidInputSelector,
			FString::Printf(
				TEXT("Module input selector '%s' must use 'EmitterName:ModuleName:InputName'."),
				*Selector),
			OutError);
	}

	FNiagaraEmitterHandle* Handle = FindEmitterHandle(
		System,
		FName(*EmitterName));
	if (Handle == nullptr)
	{
		return Fail(
			EUEPyNiagaraEditResult::EmitterNotFound,
			FString::Printf(
				TEXT("Emitter '%s' from selector '%s' was not found."),
				*EmitterName,
				*Selector),
			OutError);
	}
	if (Handle->GetEmitterMode() != ENiagaraEmitterMode::Standard)
	{
		return Fail(
			EUEPyNiagaraEditResult::UnsupportedEmitter,
			FString::Printf(
				TEXT("Emitter '%s' is stateless and has no editable Niagara graph."),
				*EmitterName),
			OutError);
	}

	FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
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
				*EmitterName),
			OutError);
	}

	TArray<UNiagaraNodeFunctionCall*> ModuleMatches;
	Graph->GetNodesOfClass(ModuleMatches);
	ModuleMatches.RemoveAll(
		[&](const UNiagaraNodeFunctionCall* FunctionCall)
		{
			return FunctionCall == nullptr
				|| !FunctionNameMatches(*FunctionCall, ModuleName);
		});
	if (ModuleMatches.IsEmpty())
	{
		return Fail(
			EUEPyNiagaraEditResult::ModuleNotFound,
			FString::Printf(
				TEXT("Module '%s' was not found in emitter '%s'."),
				*ModuleName,
				*EmitterName),
			OutError);
	}
	if (ModuleMatches.Num() > 1)
	{
		return Fail(
			EUEPyNiagaraEditResult::ModuleAmbiguous,
			FString::Printf(
				TEXT("Module '%s' occurs %d times in emitter '%s'."),
				*ModuleName,
				ModuleMatches.Num(),
				*EmitterName),
			OutError);
	}

	UNiagaraNodeFunctionCall* FunctionCall = ModuleMatches[0];
	UNiagaraGraph* CalledGraph = FunctionCall->GetCalledGraph();
	if (CalledGraph == nullptr)
	{
		return Fail(
			EUEPyNiagaraEditResult::UnsupportedEmitter,
			FString::Printf(
				TEXT("Module '%s' in emitter '%s' has no called graph."),
				*ModuleName,
				*EmitterName),
			OutError);
	}

	const bool bMatchFullyQualifiedInput = InputName.Contains(TEXT("."));
	TArray<FNiagaraVariable> InputMatches;
	for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>&
		MetadataEntry : CalledGraph->GetAllMetaData())
	{
		const FString FullInputName = MetadataEntry.Key.GetName().ToString();
		const FNiagaraParameterHandle InputHandle(MetadataEntry.Key.GetName());
		const bool bMatches = bMatchFullyQualifiedInput
			? FullInputName.Equals(InputName, ESearchCase::IgnoreCase)
			: InputHandle.GetName().ToString().Equals(
				InputName,
				ESearchCase::IgnoreCase);
		if (bMatches)
		{
			InputMatches.Add(MetadataEntry.Key);
		}
	}
	if (InputMatches.IsEmpty())
	{
		return Fail(
			EUEPyNiagaraEditResult::InputNotFound,
			FString::Printf(
				TEXT("Input '%s' was not found on module '%s' in emitter '%s'."),
				*InputName,
				*ModuleName,
				*EmitterName),
			OutError);
	}
	if (InputMatches.Num() > 1)
	{
		return Fail(
			EUEPyNiagaraEditResult::InputAmbiguous,
			FString::Printf(
				TEXT("Input '%s' occurs %d times on module '%s' in emitter '%s'. Use its fully qualified name to disambiguate it."),
				*InputName,
				InputMatches.Num(),
				*ModuleName,
				*EmitterName),
			OutError);
	}

	OutFunctionCall = FunctionCall;
	OutInputVariable = InputMatches[0];
	return EUEPyNiagaraEditResult::Success;
}

bool SaveNiagaraSystem(
	UNiagaraSystem& System,
	FString& OutError)
{
	System.WaitForCompilationComplete(false, false);
	UPackage* Package = System.GetOutermost();
	const FString PackageName = Package->GetName();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArguments;
	SaveArguments.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArguments.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(
		Package,
		&System,
		*PackageFilename,
		SaveArguments))
	{
		OutError = FString::Printf(
			TEXT("Niagara system package '%s' could not be saved."),
			*PackageName);
		return false;
	}
	return true;
}

}
