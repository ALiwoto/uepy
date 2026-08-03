#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UEPyBlueprintGraphBridge.generated.h"

/**
 * Read-only reflected access to Blueprint graph data that Unreal Python does
 * not expose, such as UEdGraphNode pins and their links.
 *
 * This class belongs to the editor-only module and is absent from packaged
 * builds. Inspection never changes, compiles, dirties, or saves the asset.
 */
UCLASS()
class UEPYEDITORBRIDGE_API UUEPyBlueprintGraphBridge final
	: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Returns a deterministic JSON description of one graph in a Blueprint. */
	UFUNCTION(BlueprintPure, Category="UEPy|Editor|Blueprint Inspection")
	static FString InspectBlueprintGraphJson(
		const FString& BlueprintPath,
		const FString& GraphName);

	/** Protocol used by the Python client to reject incompatible bridge builds. */
	UFUNCTION(BlueprintPure, Category="UEPy|Editor|Blueprint Inspection")
	static int32 GetBridgeProtocolVersion();
};
