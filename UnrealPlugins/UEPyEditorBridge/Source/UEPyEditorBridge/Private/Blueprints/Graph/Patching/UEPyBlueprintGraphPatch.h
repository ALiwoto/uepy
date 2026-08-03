#pragma once

#include "CoreMinimal.h"

namespace UEPy::BlueprintGraph
{
FString RunBlueprintGraphPatch(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& PatchJson,
	bool bApply);
}
