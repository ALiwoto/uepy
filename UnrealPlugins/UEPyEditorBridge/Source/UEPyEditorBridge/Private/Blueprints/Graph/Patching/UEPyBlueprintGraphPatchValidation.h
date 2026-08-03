#pragma once

#include "CoreMinimal.h"
#include "Blueprints/Graph/Patching/UEPyBlueprintGraphPatchTypes.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;

namespace UEPy::BlueprintGraph
{
FString NormalizedPatchName(const FString& Name);
bool IsCreationOperation(EValidatedPatchOperationType Type);
bool ValidatePatchOperations(
	const FJsonObject& Patch,
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	TArray<FValidatedPatchOperation>& OutOperations,
	FString& OutError);
}
