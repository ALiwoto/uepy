#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UEPyBlueprintGraphBridge.generated.h"

/**
 * Reflected access to Blueprint graph data that Unreal Python does not expose,
 * such as UEdGraphNode pins and their links.
 *
 * This class belongs to the editor-only module and is absent from packaged
 * builds. Inspection is read-only; explicit patch application is transactional
 * and never compiles or saves the asset.
 */
UCLASS()
class UEPYEDITORBRIDGE_API UUEPyBlueprintGraphBridge final
	: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Returns a deterministic JSON description of one graph in a Blueprint. */
	UFUNCTION(BlueprintPure, Category="UEPy|Editor|Blueprint Graph")
	static FString InspectBlueprintGraphJson(
		const FString& BlueprintPath,
		const FString& GraphName);

	/** Validates a patch without changing the Blueprint. */
	UFUNCTION(BlueprintPure, Category="UEPy|Editor|Blueprint Graph")
	static FString ValidateBlueprintGraphPatchJson(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& PatchJson);

	/** Applies a validated patch transactionally without saving the Blueprint. */
	UFUNCTION(BlueprintCallable, Category="UEPy|Editor|Blueprint Graph")
	static FString ApplyBlueprintGraphPatchJson(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& PatchJson);

	/** Protocol used by the Python client to reject incompatible bridge builds. */
	UFUNCTION(BlueprintPure, Category="UEPy|Editor|Blueprint Graph")
	static int32 GetBridgeProtocolVersion();
};
