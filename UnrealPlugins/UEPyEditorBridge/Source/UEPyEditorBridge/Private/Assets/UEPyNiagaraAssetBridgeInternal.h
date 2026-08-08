#pragma once

#include "Assets/UEPyNiagaraAssetBridge.h"

struct FNiagaraEmitterHandle;
struct FNiagaraVariable;
class UNiagaraNodeFunctionCall;
class UNiagaraSystem;

namespace UEPyNiagaraAssetBridgeInternal
{
FString NormalizeNiagaraObjectPath(const FString& InputPath);

EUEPyNiagaraEditResult Fail(
	EUEPyNiagaraEditResult Result,
	const FString& Message,
	FString& OutError);

FNiagaraEmitterHandle* FindEmitterHandle(
	UNiagaraSystem& System,
	FName EmitterName);

bool FunctionNameMatches(
	const UNiagaraNodeFunctionCall& FunctionCall,
	const FString& RequestedName);

bool SplitModuleInputSelector(
	const FString& Selector,
	FString& OutEmitterName,
	FString& OutModuleName,
	FString& OutInputName);

EUEPyNiagaraEditResult ResolveModuleInput(
	UNiagaraSystem& System,
	const FString& Selector,
	UNiagaraNodeFunctionCall*& OutFunctionCall,
	FNiagaraVariable& OutInputVariable,
	FString& OutError);

bool SaveNiagaraSystem(
	UNiagaraSystem& System,
	FString& OutError);
}
