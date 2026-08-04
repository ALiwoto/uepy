#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UEPyStaticMeshAssetBridge.generated.h"

/** Result of baking a reduced static mesh into a standalone asset. */
UENUM(BlueprintType)
enum class EUEPyShadowProxyBakeResult : uint8
{
	Success = 0,
	InvalidTriangleFraction = 1,
	InvalidSourcePath = 2,
	InvalidDestinationPath = 3,
	SourceNotFound = 4,
	SourceMeshUnavailable = 5,
	ReductionUnavailable = 6,
	ReductionFailed = 7,
	DestinationEqualsSource = 8,
	DestinationExists = 9,
	DestinationClassConflict = 10,
	BuildFailed = 11,
	SaveFailed = 12,
};

/** Generic editor-only StaticMesh asset operations exposed to Unreal Python. */
UCLASS()
class UEPYEDITORBRIDGE_API UUEPyStaticMeshAssetBridge final
	: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Reduces source LOD0 and saves the reduced geometry as the destination's
	 * only baked source LOD0. An empty destination creates a sibling named
	 * SourceName_Shadow. Existing destinations require bForce and are updated
	 * in place so hard references remain valid.
	 */
	UFUNCTION(BlueprintCallable, Category="uepy|Assets|Static Mesh")
	static EUEPyShadowProxyBakeResult BakeShadowProxy(
		const FString& SourceObjectPath,
		const FString& DestinationObjectPath,
		float TriangleFraction,
		bool bForce,
		FString& OutDestinationObjectPath,
		int32& OutSourceTriangleCount,
		int32& OutProxyTriangleCount,
		int64& OutSavedPackageBytes,
		FString& OutError);
};
