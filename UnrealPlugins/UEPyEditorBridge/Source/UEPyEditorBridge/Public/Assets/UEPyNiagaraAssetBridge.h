#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UEPyNiagaraAssetBridge.generated.h"

/** Result of applying a batch of generic Niagara-system graph edits. */
UENUM(BlueprintType)
enum class EUEPyNiagaraEditResult : uint8
{
	Success = 0,
	WrongThread = 1,
	InvalidSystemPath = 2,
	SystemNotFound = 3,
	ConflictingEdit = 4,
	EmitterNotFound = 5,
	UnsupportedEmitter = 6,
	InvalidModuleSelector = 7,
	ModuleNotFound = 8,
	ModuleAmbiguous = 9,
	SaveFailed = 10,
	InvalidInputSelector = 11,
	InputNotFound = 12,
	InputAmbiguous = 13,
	InvalidInputValue = 14,
	InvalidParameterName = 15,
	ParameterTypeConflict = 16,
};

/** Generic editor-only Niagara asset operations exposed to Unreal Python. */
UCLASS()
class UEPYEDITORBRIDGE_API UUEPyNiagaraAssetBridge final
	: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Edits named emitter enabled/local-space states and function-call nodes in
	 * an existing Niagara system. Module selectors use
	 * "EmitterName:ModuleName". All selectors are validated before mutation.
	 */
	UFUNCTION(BlueprintCallable, Category="uepy|Assets|Niagara")
	static EUEPyNiagaraEditResult EditSystem(
		const FString& SystemObjectPath,
		const TArray<FName>& EmittersToDisable,
		const TArray<FName>& EmittersToEnable,
		const TArray<FName>& EmittersToSetWorldSpace,
		const TArray<FName>& EmittersToSetLocalSpace,
		const TArray<FString>& ModulesToDisable,
		const TArray<FString>& ModulesToEnable,
		bool bSave,
		int32& OutChangedEmitterCount,
		int32& OutChangedModuleCount,
		FString& OutError);

	/**
	 * Sets local Niagara module-input values using selectors formatted as
	 * "EmitterName:ModuleName:InputName". Values may use either Unreal's pin
	 * default representation or the type's editor display name (for example,
	 * an enum entry such as "Once"). A fully qualified input name such as
	 * "Module.Color" can disambiguate inputs with the same leaf name. All
	 * selectors and values are validated before mutation.
	 */
	UFUNCTION(BlueprintCallable, Category="uepy|Assets|Niagara")
	static EUEPyNiagaraEditResult SetModuleInputValues(
		const FString& SystemObjectPath,
		const TMap<FString, FString>& ModuleInputValues,
		bool bSave,
		int32& OutChangedInputCount,
		FString& OutError);

	/**
	 * Links Niagara module inputs to exposed user parameters. Map keys use
	 * "EmitterName:ModuleName:InputName" selectors and values use fully
	 * qualified user parameter names such as "User.Tint". Missing user
	 * parameters are created with the module input's type. All links are
	 * validated before mutation.
	 */
	UFUNCTION(BlueprintCallable, Category="uepy|Assets|Niagara")
	static EUEPyNiagaraEditResult LinkModuleInputsToUserParameters(
		const FString& SystemObjectPath,
		const TMap<FString, FString>& ModuleInputParameterLinks,
		bool bSave,
		int32& OutChangedInputCount,
		FString& OutError);
};
