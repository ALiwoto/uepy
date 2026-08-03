#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimData/BoneMaskFilter.h"

class UEdGraphNode;
class UEdGraphPin;

namespace UEPy::BlueprintGraph
{
enum class EValidatedPatchOperationType : uint8
{
	Connect,
	Disconnect,
	MoveNode,
	AddSaveCachedPose,
	AddUseCachedPose,
	AddSlot,
	AddLayeredBoneBlend,
};

struct FValidatedPatchOperation
{
	EValidatedPatchOperationType Type = EValidatedPatchOperationType::MoveNode;
	UEdGraphNode* Node = nullptr;
	UEdGraphPin* FromPin = nullptr;
	UEdGraphPin* ToPin = nullptr;
	int32 X = 0;
	int32 Y = 0;
	int32 OriginalX = 0;
	int32 OriginalY = 0;
	FString Alias;
	FString Name;
	bool bAlwaysUpdateSourcePose = false;
	bool bMeshSpaceRotationBlend = false;
	bool bMeshSpaceScaleBlend = false;
	float DefaultWeight = 1.0f;
	TArray<FBranchFilter> BranchFilters;
};
}
