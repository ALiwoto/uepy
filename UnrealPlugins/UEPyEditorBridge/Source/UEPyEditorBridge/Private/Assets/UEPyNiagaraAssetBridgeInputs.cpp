#include "Assets/UEPyNiagaraAssetBridge.h"
#include "Assets/UEPyNiagaraAssetBridgeInternal.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "INiagaraEditorTypeUtilities.h"
#include "Misc/PackageName.h"
#include "NiagaraEditorModule.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraSystem.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "UObject/UObjectGlobals.h"

using namespace UEPyNiagaraAssetBridgeInternal;

namespace
{
const FName NiagaraParameterMapGetClassName(
	TEXT("NiagaraNodeParameterMapGet"));
const FName NiagaraParameterMapSetClassName(
	TEXT("NiagaraNodeParameterMapSet"));

struct FUEPyResolvedModuleInputEdit
{
	UNiagaraNodeFunctionCall* FunctionCall = nullptr;
	FNiagaraVariable InputVariable;
	FString CanonicalValue;
	FString Selector;
	bool bNeedsChange = false;
};

struct FUEPyResolvedModuleInputLink
{
	UNiagaraNodeFunctionCall* FunctionCall = nullptr;
	FNiagaraVariable InputVariable;
	FNiagaraParameterHandle ParameterHandle;
	FString Selector;
	bool bParameterNeedsCreation = false;
	bool bLinkNeedsCreation = false;
};

UEdGraphPin* FindStackFunctionInputOverridePin(
	UNiagaraNodeFunctionCall& FunctionCall,
	const FNiagaraParameterHandle& AliasedInputHandle)
{
	for (UEdGraphPin* Pin : FunctionCall.Pins)
	{
		if (Pin != nullptr
			&& Pin->Direction == EGPD_Input
			&& Pin->PinName == AliasedInputHandle.GetName())
		{
			return Pin;
		}
	}

	UEdGraphNode* OverrideNode = nullptr;
	for (UEdGraphPin* Pin : FunctionCall.Pins)
	{
		if (Pin == nullptr
			|| Pin->Direction != EGPD_Input
			|| Pin->LinkedTo.Num() != 1)
		{
			continue;
		}

		UEdGraphNode* LinkedNode = Pin->LinkedTo[0] != nullptr
			? Pin->LinkedTo[0]->GetOwningNode()
			: nullptr;
		if (LinkedNode != nullptr
			&& LinkedNode->GetClass()->GetFName()
				== NiagaraParameterMapSetClassName)
		{
			OverrideNode = LinkedNode;
			break;
		}
	}
	if (OverrideNode == nullptr)
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : OverrideNode->Pins)
	{
		if (Pin != nullptr
			&& Pin->Direction == EGPD_Input
			&& Pin->PinName
				== AliasedInputHandle.GetParameterHandleString())
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraphPin* FindLinkedParameterValuePin(const UEdGraphPin& OverridePin)
{
	if (OverridePin.LinkedTo.Num() != 1)
	{
		return nullptr;
	}

	UEdGraphPin* LinkedPin = OverridePin.LinkedTo[0];
	const UEdGraphNode* LinkedNode = LinkedPin != nullptr
		? LinkedPin->GetOwningNode()
		: nullptr;
	const FNiagaraParameterHandle LinkedHandle(
		LinkedPin != nullptr ? LinkedPin->PinName : NAME_None);
	if (LinkedNode == nullptr
		|| LinkedNode->GetClass()->GetFName()
			!= NiagaraParameterMapGetClassName
		|| !LinkedHandle.IsValid()
		|| LinkedHandle.GetNamespace().IsNone())
	{
		return nullptr;
	}
	return LinkedPin;
}

TSet<FNiagaraVariable> GatherUserParameters(UNiagaraSystem& System)
{
	TSet<FNiagaraVariable> Parameters;
	TArray<FNiagaraVariable> UserParameters;
	System.GetExposedParameters().GetUserParameters(UserParameters);
	for (FNiagaraVariable& Parameter : UserParameters)
	{
		FNiagaraUserRedirectionParameterStore::MakeUserVariable(Parameter);
	}
	Parameters.Append(UserParameters);
	return Parameters;
}
}

EUEPyNiagaraEditResult UUEPyNiagaraAssetBridge::SetModuleInputValues(
	const FString& SystemObjectPath,
	const TMap<FString, FString>& ModuleInputValues,
	const bool bSave,
	int32& OutChangedInputCount,
	FString& OutError)
{
	OutChangedInputCount = 0;
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

	TArray<FUEPyResolvedModuleInputEdit> ResolvedEdits;
	ResolvedEdits.Reserve(ModuleInputValues.Num());
	for (const TPair<FString, FString>& RequestedEdit : ModuleInputValues)
	{
		const FString Selector = RequestedEdit.Key.TrimStartAndEnd();
		UNiagaraNodeFunctionCall* FunctionCall = nullptr;
		FNiagaraVariable InputVariable;
		const EUEPyNiagaraEditResult ResolveResult = ResolveModuleInput(
			*System,
			Selector,
			FunctionCall,
			InputVariable,
			OutError);
		if (ResolveResult != EUEPyNiagaraEditResult::Success)
		{
			return ResolveResult;
		}

		FNiagaraVariable ParsedValue(
			InputVariable.GetType(),
			NAME_None);
		ParsedValue.AllocateData();
		const TSharedPtr<INiagaraEditorTypeUtilities, ESPMode::ThreadSafe>
			TypeUtilities = FNiagaraEditorModule::Get().GetTypeUtilities(
				ParsedValue.GetType());
		const FString RequestedValue = RequestedEdit.Value.TrimStartAndEnd();
		bool bParsed = TypeUtilities.IsValid()
			&& TypeUtilities->CanHandlePinDefaults()
			&& TypeUtilities->SetValueFromPinDefaultString(
				RequestedValue,
				ParsedValue);
		if (!bParsed
			&& TypeUtilities.IsValid()
			&& TypeUtilities->CanSetValueFromDisplayName())
		{
			bParsed = TypeUtilities->SetValueFromDisplayName(
				FText::FromString(RequestedValue),
				ParsedValue);
		}
		if (!bParsed)
		{
			return Fail(
				EUEPyNiagaraEditResult::InvalidInputValue,
				FString::Printf(
					TEXT("Value '%s' is invalid for module input selector '%s'."),
					*RequestedValue,
					*Selector),
				OutError);
		}

		ResolvedEdits.Add({
			FunctionCall,
			InputVariable,
			TypeUtilities->GetPinDefaultStringFromValue(ParsedValue),
			Selector});
	}

	bool bHasChanges = false;
	for (FUEPyResolvedModuleInputEdit& Edit : ResolvedEdits)
	{
		const FNiagaraParameterHandle InputHandle(Edit.InputVariable.GetName());
		const FNiagaraParameterHandle AliasedInputHandle =
			FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
				InputHandle,
				Edit.FunctionCall);
		if (const UEdGraphPin* OverridePin =
			FindStackFunctionInputOverridePin(
				*Edit.FunctionCall,
				AliasedInputHandle))
		{
			if (!OverridePin->LinkedTo.IsEmpty())
			{
				return Fail(
					EUEPyNiagaraEditResult::InvalidInputValue,
					FString::Printf(
						TEXT("Module input selector '%s' is linked to another node and cannot be replaced with a local value."),
						*Edit.Selector),
					OutError);
			}
			Edit.bNeedsChange =
				OverridePin->DefaultValue != Edit.CanonicalValue;
		}
		else
		{
			Edit.bNeedsChange = true;
		}
		bHasChanges |= Edit.bNeedsChange;
	}
	if (!bHasChanges)
	{
		return EUEPyNiagaraEditResult::Success;
	}

	const UEdGraphSchema_Niagara* NiagaraSchema =
		GetDefault<UEdGraphSchema_Niagara>();
	System->Modify();
	for (FUEPyResolvedModuleInputEdit& Edit : ResolvedEdits)
	{
		if (!Edit.bNeedsChange)
		{
			continue;
		}
		const FNiagaraParameterHandle InputHandle(
			Edit.InputVariable.GetName());
		const FNiagaraParameterHandle AliasedInputHandle =
			FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
				InputHandle,
				Edit.FunctionCall);
		Edit.FunctionCall->Modify();
		Edit.FunctionCall->GetNiagaraGraph()->Modify();
		UEdGraphPin* OverridePin = &FNiagaraStackGraphUtilities::
			GetOrCreateStackFunctionInputOverridePin(
				*Edit.FunctionCall,
				AliasedInputHandle,
				Edit.InputVariable.GetType(),
				FGuid(),
				FGuid());
		NiagaraSchema->TrySetDefaultValue(
			*OverridePin,
			Edit.CanonicalValue,
			true);
		Edit.FunctionCall->MarkNodeRequiresSynchronization(
			TEXT("uepy Niagara module input edit"),
			true);
		++OutChangedInputCount;
	}

	if (OutChangedInputCount == 0)
	{
		return EUEPyNiagaraEditResult::Success;
	}

	System->MarkPackageDirty();
	System->RequestCompile(false);
	if (!bSave)
	{
		return EUEPyNiagaraEditResult::Success;
	}

	if (!SaveNiagaraSystem(*System, OutError))
	{
		return EUEPyNiagaraEditResult::SaveFailed;
	}

	return EUEPyNiagaraEditResult::Success;
}

EUEPyNiagaraEditResult
UUEPyNiagaraAssetBridge::LinkModuleInputsToUserParameters(
	const FString& SystemObjectPath,
	const TMap<FString, FString>& ModuleInputParameterLinks,
	const bool bSave,
	int32& OutChangedInputCount,
	FString& OutError)
{
	OutChangedInputCount = 0;
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

	TArray<FNiagaraVariable> ExistingUserParameters;
	System->GetExposedParameters().GetUserParameters(
		ExistingUserParameters);
	for (FNiagaraVariable& Parameter : ExistingUserParameters)
	{
		FNiagaraUserRedirectionParameterStore::MakeUserVariable(Parameter);
	}

	TArray<FUEPyResolvedModuleInputLink> ResolvedLinks;
	ResolvedLinks.Reserve(ModuleInputParameterLinks.Num());
	TMap<FName, FNiagaraTypeDefinition> RequestedParameterTypes;
	TMap<FName, FName> RequestedInputLinks;
	for (const TPair<FString, FString>& RequestedLink
		: ModuleInputParameterLinks)
	{
		const FString Selector = RequestedLink.Key.TrimStartAndEnd();
		UNiagaraNodeFunctionCall* FunctionCall = nullptr;
		FNiagaraVariable InputVariable;
		const EUEPyNiagaraEditResult ResolveResult = ResolveModuleInput(
			*System,
			Selector,
			FunctionCall,
			InputVariable,
			OutError);
		if (ResolveResult != EUEPyNiagaraEditResult::Success)
		{
			return ResolveResult;
		}

		const FString ParameterName =
			RequestedLink.Value.TrimStartAndEnd();
		const FNiagaraParameterHandle ParameterHandle{
			FName(*ParameterName)};
		if (!ParameterHandle.IsValid() || !ParameterHandle.IsUserHandle())
		{
			return Fail(
				EUEPyNiagaraEditResult::InvalidParameterName,
				FString::Printf(
					TEXT("Parameter '%s' for selector '%s' must be a fully qualified User parameter."),
					*ParameterName,
					*Selector),
				OutError);
		}
		if (const FNiagaraTypeDefinition* RequestedType =
			RequestedParameterTypes.Find(
				ParameterHandle.GetParameterHandleString()))
		{
			if (*RequestedType != InputVariable.GetType())
			{
				return Fail(
					EUEPyNiagaraEditResult::ParameterTypeConflict,
					FString::Printf(
						TEXT("User parameter '%s' was requested for incompatible input types in the same edit."),
						*ParameterName),
					OutError);
			}
		}
		else
		{
			RequestedParameterTypes.Add(
				ParameterHandle.GetParameterHandleString(),
				InputVariable.GetType());
		}

		const FName InputIdentity(*FString::Printf(
			TEXT("%s:%s"),
			*FunctionCall->GetPathName(),
			*InputVariable.GetName().ToString()));
		if (const FName* RequestedParameter =
			RequestedInputLinks.Find(InputIdentity))
		{
			if (*RequestedParameter
				!= ParameterHandle.GetParameterHandleString())
			{
				return Fail(
					EUEPyNiagaraEditResult::ConflictingEdit,
					FString::Printf(
						TEXT("Module input selector '%s' was requested with multiple user parameters in the same edit."),
						*Selector),
					OutError);
			}
		}
		else
		{
			RequestedInputLinks.Add(
				InputIdentity,
				ParameterHandle.GetParameterHandleString());
		}

		bool bParameterNeedsCreation = true;
		for (const FNiagaraVariable& ExistingParameter
			: ExistingUserParameters)
		{
			if (ExistingParameter.GetName()
				!= ParameterHandle.GetParameterHandleString())
			{
				continue;
			}
			if (ExistingParameter.GetType() != InputVariable.GetType())
			{
				return Fail(
					EUEPyNiagaraEditResult::ParameterTypeConflict,
					FString::Printf(
						TEXT("User parameter '%s' already exists with a type incompatible with selector '%s'."),
						*ParameterName,
						*Selector),
					OutError);
			}
			bParameterNeedsCreation = false;
			break;
		}

		const FNiagaraParameterHandle InputHandle(
			InputVariable.GetName());
		const FNiagaraParameterHandle AliasedInputHandle =
			FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
				InputHandle,
				FunctionCall);
		bool bLinkNeedsCreation = true;
		if (const UEdGraphPin* OverridePin =
			FindStackFunctionInputOverridePin(
				*FunctionCall,
				AliasedInputHandle))
		{
			if (!OverridePin->LinkedTo.IsEmpty())
			{
				const UEdGraphPin* LinkedValuePin =
					FindLinkedParameterValuePin(*OverridePin);
				if (LinkedValuePin == nullptr)
				{
					return Fail(
						EUEPyNiagaraEditResult::ConflictingEdit,
						FString::Printf(
							TEXT("Module input selector '%s' is already linked to a non-parameter value."),
							*Selector),
						OutError);
				}

				const FNiagaraParameterHandle ExistingLinkedHandle(
					LinkedValuePin->PinName);
				if (ExistingLinkedHandle != ParameterHandle)
				{
					return Fail(
						EUEPyNiagaraEditResult::ConflictingEdit,
						FString::Printf(
							TEXT("Module input selector '%s' is already linked to '%s'."),
							*Selector,
							*ExistingLinkedHandle.GetParameterHandleString().ToString()),
						OutError);
				}
				bLinkNeedsCreation = false;
			}
		}

		ResolvedLinks.Add({
			FunctionCall,
			InputVariable,
			ParameterHandle,
			Selector,
			bParameterNeedsCreation,
			bLinkNeedsCreation});
	}

	bool bHasChanges = false;
	for (const FUEPyResolvedModuleInputLink& Link : ResolvedLinks)
	{
		bHasChanges |= Link.bParameterNeedsCreation
			|| Link.bLinkNeedsCreation;
	}
	if (!bHasChanges)
	{
		return EUEPyNiagaraEditResult::Success;
	}

	System->Modify();
	for (FUEPyResolvedModuleInputLink& Link : ResolvedLinks)
	{
		if (!Link.bParameterNeedsCreation && !Link.bLinkNeedsCreation)
		{
			continue;
		}

		const FNiagaraParameterHandle InputHandle(
			Link.InputVariable.GetName());
		const FNiagaraParameterHandle AliasedInputHandle =
			FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
				InputHandle,
				Link.FunctionCall);
		Link.FunctionCall->Modify();
		UNiagaraGraph* NiagaraGraph =
			Link.FunctionCall->GetNiagaraGraph();
		NiagaraGraph->Modify();
		UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::
			GetOrCreateStackFunctionInputOverridePin(
				*Link.FunctionCall,
				AliasedInputHandle,
				Link.InputVariable.GetType(),
				FGuid(),
				FGuid());

		if (Link.bParameterNeedsCreation)
		{
			FNiagaraVariable UserParameter(
				Link.InputVariable.GetType(),
				Link.ParameterHandle.GetParameterHandleString());
			FNiagaraVariable InitialValue = UserParameter;
			InitialValue.AllocateData();
			const TSharedPtr<INiagaraEditorTypeUtilities,
				ESPMode::ThreadSafe> TypeUtilities =
				FNiagaraEditorModule::Get().GetTypeUtilities(
					InitialValue.GetType());
			const bool bHasLocalDefault = OverridePin.LinkedTo.IsEmpty()
				&& TypeUtilities.IsValid()
				&& TypeUtilities->CanHandlePinDefaults()
				&& TypeUtilities->SetValueFromPinDefaultString(
					OverridePin.DefaultValue,
					InitialValue);

			FNiagaraUserRedirectionParameterStore& ExposedParameters =
				System->GetExposedParameters();
			ExposedParameters.AddParameter(UserParameter, true, true);
			if (bHasLocalDefault)
			{
				ExposedParameters.SetParameterData(
					InitialValue.GetData(),
					UserParameter,
					false);
			}
		}

		if (Link.bLinkNeedsCreation)
		{
			const TSet<FNiagaraVariable> KnownParameters =
				GatherUserParameters(*System);
			FNiagaraStackGraphUtilities::
				SetLinkedValueHandleForFunctionInput(
					OverridePin,
					Link.ParameterHandle,
					KnownParameters);
		}

		Link.FunctionCall->MarkNodeRequiresSynchronization(
			TEXT("uepy Niagara module input user-parameter link"),
			true);
		++OutChangedInputCount;
	}

	System->MarkPackageDirty();
	System->RequestCompile(false);
	if (!bSave)
	{
		return EUEPyNiagaraEditResult::Success;
	}
	if (!SaveNiagaraSystem(*System, OutError))
	{
		return EUEPyNiagaraEditResult::SaveFailed;
	}

	return EUEPyNiagaraEditResult::Success;
}
