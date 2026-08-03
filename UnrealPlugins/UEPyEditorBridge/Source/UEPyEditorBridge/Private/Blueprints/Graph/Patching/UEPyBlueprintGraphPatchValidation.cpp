#include "Blueprints/Graph/Patching/UEPyBlueprintGraphPatchValidation.h"

#include "Blueprints/Graph/UEPyBlueprintGraphSerialization.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace UEPy::BlueprintGraph
{
namespace
{
constexpr int32 MaximumPatchOperations = 100;
constexpr int32 MaximumBranchFiltersPerNode = 64;

bool TryReadGuidField(
	const FJsonObject& Object,
	const TCHAR* FieldName,
	FGuid& OutGuid)
{
	FString GuidText;
	return Object.TryGetStringField(FieldName, GuidText)
		&& FGuid::Parse(GuidText, OutGuid);
}

UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& NodeGuid)
{
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (IsValid(Node) && Node->NodeGuid == NodeGuid)
		{
			return Node;
		}
	}
	return nullptr;
}

UEdGraphPin* FindPinByGuid(UEdGraphNode* Node, const FGuid& PinGuid)
{
	if (Node == nullptr)
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin != nullptr && Pin->PinId == PinGuid)
		{
			return Pin;
		}
	}
	return nullptr;
}

bool ResolveOperationPin(
	const FJsonObject& OperationJson,
	const TCHAR* NodeField,
	const TCHAR* PinField,
	UEdGraph* Graph,
	UEdGraphPin*& OutPin,
	FString& OutError)
{
	FGuid NodeGuid;
	FGuid PinGuid;
	if (!TryReadGuidField(OperationJson, NodeField, NodeGuid)
		|| !TryReadGuidField(OperationJson, PinField, PinGuid))
	{
		OutError = FString::Printf(
			TEXT("Operation requires valid '%s' and '%s' GUIDs."),
			NodeField,
			PinField);
		return false;
	}

	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid);
	OutPin = FindPinByGuid(Node, PinGuid);
	if (OutPin == nullptr)
	{
		OutError = FString::Printf(
			TEXT("Could not resolve pin '%s' on node '%s'."),
			*PinGuid.ToString(EGuidFormats::DigitsWithHyphens),
			*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		return false;
	}
	return true;
}
}

FString NormalizedPatchName(const FString& Name)
{
	FString Result = Name;
	Result.TrimStartAndEndInline();
	Result.ToLowerInline();
	return Result;
}

namespace
{
bool ReadOperationAlias(
	const FJsonObject& OperationJson,
	const int32 OperationIndex,
	TSet<FString>& Aliases,
	FValidatedPatchOperation& OutOperation,
	FString& OutError)
{
	if (!OperationJson.TryGetStringField(TEXT("alias"), OutOperation.Alias))
	{
		OutError = FString::Printf(
			TEXT("Operation %d requires an 'alias' string."),
			OperationIndex);
		return false;
	}
	OutOperation.Alias.TrimStartAndEndInline();
	const FString NormalizedAlias = NormalizedPatchName(OutOperation.Alias);
	if (NormalizedAlias.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Operation %d requires a non-empty 'alias'."),
			OperationIndex);
		return false;
	}
	if (Aliases.Contains(NormalizedAlias))
	{
		OutError = FString::Printf(
			TEXT("Operation %d reuses alias '%s'."),
			OperationIndex,
			*OutOperation.Alias);
		return false;
	}
	Aliases.Add(NormalizedAlias);
	return true;
}

bool ReadCreationOperationBase(
	const FJsonObject& OperationJson,
	const int32 OperationIndex,
	TSet<FString>& Aliases,
	FValidatedPatchOperation& OutOperation,
	FString& OutError)
{
	if (!ReadOperationAlias(
		OperationJson,
		OperationIndex,
		Aliases,
		OutOperation,
		OutError))
	{
		return false;
	}
	if (!OperationJson.TryGetNumberField(TEXT("x"), OutOperation.X)
		|| !OperationJson.TryGetNumberField(TEXT("y"), OutOperation.Y))
	{
		OutError = FString::Printf(
			TEXT("Operation %d requires integer 'x' and 'y' values."),
			OperationIndex);
		return false;
	}
	return true;
}

bool ReadRequiredCreationName(
	const FJsonObject& OperationJson,
	const TCHAR* FieldName,
	const int32 OperationIndex,
	FString& OutName,
	FString& OutError)
{
	if (!OperationJson.TryGetStringField(FieldName, OutName))
	{
		OutError = FString::Printf(
			TEXT("Operation %d requires a '%s' string."),
			OperationIndex,
			FieldName);
		return false;
	}
	OutName.TrimStartAndEndInline();
	if (OutName.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Operation %d requires a non-empty '%s'."),
			OperationIndex,
			FieldName);
		return false;
	}
	return true;
}

bool ReadLayerDefaultWeight(
	const FJsonObject& OperationJson,
	const int32 OperationIndex,
	FValidatedPatchOperation& OutOperation,
	FString& OutError)
{
	double DefaultWeight = OutOperation.DefaultWeight;
	if (OperationJson.HasField(TEXT("default_weight"))
		&& (!OperationJson.TryGetNumberField(
			TEXT("default_weight"),
			DefaultWeight)
			|| !FMath::IsFinite(DefaultWeight)
			|| DefaultWeight < 0.0
			|| DefaultWeight > 1.0))
	{
		OutError = FString::Printf(
			TEXT("Operation %d field 'default_weight' must be a finite number from 0 to 1."),
			OperationIndex);
		return false;
	}
	OutOperation.DefaultWeight = static_cast<float>(DefaultWeight);
	return true;
}

bool ReadBranchFilters(
	const FJsonObject& OperationJson,
	const int32 OperationIndex,
	const USkeleton& TargetSkeleton,
	FValidatedPatchOperation& OutOperation,
	FString& OutError)
{
	const TArray<FJsonValuePtr>* BranchFilterValues = nullptr;
	if (!OperationJson.TryGetArrayField(
		TEXT("branch_filters"),
		BranchFilterValues)
		|| BranchFilterValues == nullptr
		|| BranchFilterValues->IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Operation %d requires a non-empty 'branch_filters' array."),
			OperationIndex);
		return false;
	}
	if (BranchFilterValues->Num() > MaximumBranchFiltersPerNode)
	{
		OutError = FString::Printf(
			TEXT("Operation %d exceeds the %d-branch-filter safety limit."),
			OperationIndex,
			MaximumBranchFiltersPerNode);
		return false;
	}

	TSet<FString> FilterBones;
	for (int32 FilterIndex = 0;
		FilterIndex < BranchFilterValues->Num();
		++FilterIndex)
	{
		const FJsonValuePtr& FilterValue = (*BranchFilterValues)[FilterIndex];
		if (!FilterValue.IsValid() || FilterValue->Type != EJson::Object)
		{
			OutError = FString::Printf(
				TEXT("Operation %d branch filter %d must be a JSON object."),
				OperationIndex,
				FilterIndex);
			return false;
		}

		const FJsonObjectPtr FilterJson = FilterValue->AsObject();
		FString BoneName;
		int32 BlendDepth = 0;
		if (!FilterJson.IsValid()
			|| !ReadRequiredCreationName(
				*FilterJson,
				TEXT("bone"),
				OperationIndex,
				BoneName,
				OutError)
			|| !FilterJson->TryGetNumberField(
				TEXT("blend_depth"),
				BlendDepth))
		{
			OutError = FString::Printf(
				TEXT("Operation %d branch filter %d requires string 'bone' and integer 'blend_depth'."),
				OperationIndex,
				FilterIndex);
			return false;
		}

		const FString NormalizedBone = NormalizedPatchName(BoneName);
		if (FilterBones.Contains(NormalizedBone))
		{
			OutError = FString::Printf(
				TEXT("Operation %d repeats branch-filter bone '%s'."),
				OperationIndex,
				*BoneName);
			return false;
		}
		if (TargetSkeleton.GetReferenceSkeleton().FindBoneIndex(
			FName(*BoneName)) == INDEX_NONE)
		{
			OutError = FString::Printf(
				TEXT("Operation %d branch-filter bone '%s' does not exist on the target skeleton."),
				OperationIndex,
				*BoneName);
			return false;
		}

		FilterBones.Add(NormalizedBone);
		FBranchFilter& BranchFilter =
			OutOperation.BranchFilters.AddDefaulted_GetRef();
		BranchFilter.BoneName = FName(*BoneName);
		BranchFilter.BlendDepth = BlendDepth;
	}
	return true;
}
}

bool IsCreationOperation(const EValidatedPatchOperationType Type)
{
	return Type == EValidatedPatchOperationType::AddSaveCachedPose
		|| Type == EValidatedPatchOperationType::AddUseCachedPose
		|| Type == EValidatedPatchOperationType::AddSlot
		|| Type == EValidatedPatchOperationType::AddLayeredBoneBlend;
}

bool ValidatePatchOperations(
	const FJsonObject& Patch,
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	TArray<FValidatedPatchOperation>& OutOperations,
	FString& OutError)
{
	const TArray<FJsonValuePtr>* OperationValues = nullptr;
	if (!Patch.TryGetArrayField(TEXT("operations"), OperationValues)
		|| OperationValues == nullptr
		|| OperationValues->IsEmpty())
	{
		OutError = TEXT("Patch requires a non-empty 'operations' array.");
		return false;
	}
	if (OperationValues->Num() > MaximumPatchOperations)
	{
		OutError = FString::Printf(
			TEXT("Patch exceeds the %d-operation safety limit."),
			MaximumPatchOperations);
		return false;
	}

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema == nullptr)
	{
		OutError = TEXT("Graph has no schema.");
		return false;
	}

	TSet<FGuid> TouchedPins;
	TSet<FGuid> MovedNodes;
	TSet<FGuid> LayeredBlendNodesWithAddedPose;
	TSet<FString> Aliases;
	TSet<FString> AvailableCachedPoseNames;
	TArray<UAnimGraphNode_SaveCachedPose*> ExistingCachedPoseNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(
		Blueprint,
		ExistingCachedPoseNodes);
	for (const UAnimGraphNode_SaveCachedPose* ExistingCachedPose
		: ExistingCachedPoseNodes)
	{
		if (IsValid(ExistingCachedPose))
		{
			AvailableCachedPoseNames.Add(
				NormalizedPatchName(ExistingCachedPose->CacheName));
		}
	}

	UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint);
	USkeleton* TargetSkeleton = AnimBlueprint != nullptr
		? AnimBlueprint->TargetSkeleton
		: nullptr;
	const bool bIsAnimationGraph = Graph->IsA<UAnimationGraph>()
		&& Graph->GetOuter() == AnimBlueprint;
	bool bHasCreationOperation = false;
	bool bHasLayerPoseOperation = false;
	bool bHasExistingNodeOperation = false;
	for (int32 OperationIndex = 0;
		OperationIndex < OperationValues->Num();
		++OperationIndex)
	{
		const FJsonValuePtr& OperationValue = (*OperationValues)[OperationIndex];
		if (!OperationValue.IsValid() || OperationValue->Type != EJson::Object)
		{
			OutError = FString::Printf(
				TEXT("Operation %d must be a JSON object."),
				OperationIndex);
			return false;
		}

		const FJsonObjectPtr OperationJson = OperationValue->AsObject();
		FString OperationName;
		if (!OperationJson.IsValid()
			|| !OperationJson->TryGetStringField(TEXT("op"), OperationName))
		{
			OutError = FString::Printf(
				TEXT("Operation %d requires an 'op' string."),
				OperationIndex);
			return false;
		}

		FValidatedPatchOperation Operation;
		if (OperationName.Equals(TEXT("connect"), ESearchCase::IgnoreCase)
			|| OperationName.Equals(TEXT("disconnect"), ESearchCase::IgnoreCase))
		{
			bHasExistingNodeOperation = true;
			if (!ResolveOperationPin(
				*OperationJson,
				TEXT("from_node_id"),
				TEXT("from_pin_id"),
				Graph,
				Operation.FromPin,
				OutError)
				|| !ResolveOperationPin(
					*OperationJson,
					TEXT("to_node_id"),
					TEXT("to_pin_id"),
					Graph,
					Operation.ToPin,
					OutError))
			{
				OutError = FString::Printf(
					TEXT("Operation %d: %s"),
					OperationIndex,
					*OutError);
				return false;
			}

			if (Operation.FromPin->Direction != EGPD_Output
				|| Operation.ToPin->Direction != EGPD_Input)
			{
				OutError = FString::Printf(
					TEXT("Operation %d must identify an output pin followed by an input pin."),
					OperationIndex);
				return false;
			}
			if (TouchedPins.Contains(Operation.FromPin->PinId)
				|| TouchedPins.Contains(Operation.ToPin->PinId))
			{
				OutError = FString::Printf(
					TEXT("Operation %d reuses a pin already touched by this patch."),
					OperationIndex);
				return false;
			}
			TouchedPins.Add(Operation.FromPin->PinId);
			TouchedPins.Add(Operation.ToPin->PinId);

			if (OperationName.Equals(TEXT("connect"), ESearchCase::IgnoreCase))
			{
				if (Operation.FromPin->LinkedTo.Contains(Operation.ToPin))
				{
					OutError = FString::Printf(
						TEXT("Operation %d requests a connection that already exists."),
						OperationIndex);
					return false;
				}
				const FPinConnectionResponse Response =
					Schema->CanCreateConnection(Operation.FromPin, Operation.ToPin);
				if (Response.Response != CONNECT_RESPONSE_MAKE)
				{
					OutError = FString::Printf(
						TEXT("Operation %d cannot create a side-effect-free connection: %s"),
						OperationIndex,
						*Response.Message.ToString());
					return false;
				}
				Operation.Type = EValidatedPatchOperationType::Connect;
			}
			else
			{
				if (!Operation.FromPin->LinkedTo.Contains(Operation.ToPin))
				{
					OutError = FString::Printf(
						TEXT("Operation %d requests a connection that does not exist."),
						OperationIndex);
					return false;
				}
				Operation.Type = EValidatedPatchOperationType::Disconnect;
			}
		}
		else if (OperationName.Equals(TEXT("move_node"), ESearchCase::IgnoreCase))
		{
			bHasExistingNodeOperation = true;
			FGuid NodeGuid;
			if (!TryReadGuidField(*OperationJson, TEXT("node_id"), NodeGuid))
			{
				OutError = FString::Printf(
					TEXT("Operation %d requires a valid 'node_id' GUID."),
					OperationIndex);
				return false;
			}
			Operation.Node = FindNodeByGuid(Graph, NodeGuid);
			if (Operation.Node == nullptr)
			{
				OutError = FString::Printf(
					TEXT("Operation %d could not resolve node '%s'."),
					OperationIndex,
					*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				return false;
			}
			if (MovedNodes.Contains(NodeGuid))
			{
				OutError = FString::Printf(
					TEXT("Operation %d moves a node already moved by this patch."),
					OperationIndex);
				return false;
			}
			if (!OperationJson->TryGetNumberField(TEXT("x"), Operation.X)
				|| !OperationJson->TryGetNumberField(TEXT("y"), Operation.Y))
			{
				OutError = FString::Printf(
					TEXT("Operation %d requires integer 'x' and 'y' values."),
					OperationIndex);
				return false;
			}
			MovedNodes.Add(NodeGuid);
			Operation.Type = EValidatedPatchOperationType::MoveNode;
			Operation.OriginalX = Operation.Node->NodePosX;
			Operation.OriginalY = Operation.Node->NodePosY;
		}
		else if (OperationName.Equals(
			TEXT("add_save_cached_pose"),
			ESearchCase::IgnoreCase))
		{
			bHasCreationOperation = true;
			if (!bIsAnimationGraph || AnimBlueprint == nullptr)
			{
				OutError = FString::Printf(
					TEXT("Operation %d can only create nodes in an Animation Blueprint's AnimationGraph."),
					OperationIndex);
				return false;
			}
			if (!ReadCreationOperationBase(
				*OperationJson,
				OperationIndex,
				Aliases,
				Operation,
				OutError)
				|| !ReadRequiredCreationName(
					*OperationJson,
					TEXT("cache_name"),
					OperationIndex,
					Operation.Name,
					OutError))
			{
				return false;
			}

			const FString NormalizedCacheName =
				NormalizedPatchName(Operation.Name);
			if (AvailableCachedPoseNames.Contains(NormalizedCacheName))
			{
				OutError = FString::Printf(
					TEXT("Operation %d reuses cached-pose name '%s'."),
					OperationIndex,
					*Operation.Name);
				return false;
			}
			AvailableCachedPoseNames.Add(NormalizedCacheName);
			Operation.Type = EValidatedPatchOperationType::AddSaveCachedPose;
		}
		else if (OperationName.Equals(
			TEXT("add_use_cached_pose"),
			ESearchCase::IgnoreCase))
		{
			bHasCreationOperation = true;
			if (!bIsAnimationGraph || AnimBlueprint == nullptr)
			{
				OutError = FString::Printf(
					TEXT("Operation %d can only create nodes in an Animation Blueprint's AnimationGraph."),
					OperationIndex);
				return false;
			}
			if (!ReadCreationOperationBase(
				*OperationJson,
				OperationIndex,
				Aliases,
				Operation,
				OutError)
				|| !ReadRequiredCreationName(
					*OperationJson,
					TEXT("cache_name"),
					OperationIndex,
					Operation.Name,
					OutError))
			{
				return false;
			}
			if (!AvailableCachedPoseNames.Contains(
				NormalizedPatchName(Operation.Name)))
			{
				OutError = FString::Printf(
					TEXT("Operation %d references unknown cached pose '%s'. Add its save node earlier in this patch or create it first."),
					OperationIndex,
					*Operation.Name);
				return false;
			}
			Operation.Type = EValidatedPatchOperationType::AddUseCachedPose;
		}
		else if (OperationName.Equals(TEXT("add_slot"), ESearchCase::IgnoreCase))
		{
			bHasCreationOperation = true;
			if (!bIsAnimationGraph || AnimBlueprint == nullptr
				|| TargetSkeleton == nullptr)
			{
				OutError = FString::Printf(
					TEXT("Operation %d requires an Animation Blueprint with a target skeleton and AnimationGraph."),
					OperationIndex);
				return false;
			}
			if (!ReadCreationOperationBase(
				*OperationJson,
				OperationIndex,
				Aliases,
				Operation,
				OutError)
				|| !ReadRequiredCreationName(
					*OperationJson,
					TEXT("slot_name"),
					OperationIndex,
					Operation.Name,
					OutError))
			{
				return false;
			}
			if (!TargetSkeleton->ContainsSlotName(FName(*Operation.Name)))
			{
				OutError = FString::Printf(
					TEXT("Operation %d references slot '%s', which is not registered on the target skeleton."),
					OperationIndex,
					*Operation.Name);
				return false;
			}
			if (OperationJson->HasField(TEXT("always_update_source_pose"))
				&& !OperationJson->TryGetBoolField(
					TEXT("always_update_source_pose"),
					Operation.bAlwaysUpdateSourcePose))
			{
				OutError = FString::Printf(
					TEXT("Operation %d field 'always_update_source_pose' must be boolean."),
					OperationIndex);
				return false;
			}
			Operation.Type = EValidatedPatchOperationType::AddSlot;
		}
		else if (OperationName.Equals(
			TEXT("add_layered_bone_blend_pose"),
			ESearchCase::IgnoreCase))
		{
			bHasLayerPoseOperation = true;
			if (!bIsAnimationGraph || AnimBlueprint == nullptr
				|| TargetSkeleton == nullptr)
			{
				OutError = FString::Printf(
					TEXT("Operation %d requires an Animation Blueprint with a target skeleton and AnimationGraph."),
					OperationIndex);
				return false;
			}
			if (!ReadOperationAlias(
				*OperationJson,
				OperationIndex,
				Aliases,
				Operation,
				OutError))
			{
				return false;
			}

			FGuid NodeGuid;
			if (!TryReadGuidField(*OperationJson, TEXT("node_id"), NodeGuid))
			{
				OutError = FString::Printf(
					TEXT("Operation %d requires a valid 'node_id' GUID."),
					OperationIndex);
				return false;
			}
			UAnimGraphNode_LayeredBoneBlend* LayeredBlend =
				Cast<UAnimGraphNode_LayeredBoneBlend>(FindNodeByGuid(Graph, NodeGuid));
			if (LayeredBlend == nullptr)
			{
				OutError = FString::Printf(
					TEXT("Operation %d could not resolve layered-blend node '%s'."),
					OperationIndex,
					*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				return false;
			}
			if (LayeredBlendNodesWithAddedPose.Contains(NodeGuid))
			{
				OutError = FString::Printf(
					TEXT("Operation %d adds more than one pose to layered-blend node '%s'. Use separate reviewed patches."),
					OperationIndex,
					*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				return false;
			}
			if (LayeredBlend->Node.BlendMode
				!= ELayeredBoneBlendMode::BranchFilter)
			{
				OutError = FString::Printf(
					TEXT("Operation %d requires a layered-blend node using BranchFilter mode."),
					OperationIndex);
				return false;
			}
			const int32 PoseCount = LayeredBlend->Node.BlendPoses.Num();
			if (LayeredBlend->Node.LayerSetup.Num() != PoseCount
				|| LayeredBlend->Node.BlendWeights.Num() != PoseCount)
			{
				OutError = FString::Printf(
					TEXT("Operation %d found inconsistent pose arrays on layered-blend node '%s'."),
					OperationIndex,
					*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				return false;
			}
			if (!ReadLayerDefaultWeight(
				*OperationJson,
				OperationIndex,
				Operation,
				OutError)
				|| !ReadBranchFilters(
					*OperationJson,
					OperationIndex,
					*TargetSkeleton,
					Operation,
					OutError))
			{
				return false;
			}

			LayeredBlendNodesWithAddedPose.Add(NodeGuid);
			Operation.Node = LayeredBlend;
			Operation.Type =
				EValidatedPatchOperationType::AddLayeredBoneBlendPose;
		}
		else if (OperationName.Equals(
			TEXT("add_layered_bone_blend"),
			ESearchCase::IgnoreCase))
		{
			bHasCreationOperation = true;
			if (!bIsAnimationGraph || AnimBlueprint == nullptr
				|| TargetSkeleton == nullptr)
			{
				OutError = FString::Printf(
					TEXT("Operation %d requires an Animation Blueprint with a target skeleton and AnimationGraph."),
					OperationIndex);
				return false;
			}
			if (!ReadCreationOperationBase(
				*OperationJson,
				OperationIndex,
				Aliases,
				Operation,
				OutError))
			{
				return false;
			}

			if (!ReadLayerDefaultWeight(
				*OperationJson,
				OperationIndex,
				Operation,
				OutError))
			{
				return false;
			}
			if (OperationJson->HasField(TEXT("mesh_space_rotation_blend"))
				&& !OperationJson->TryGetBoolField(
					TEXT("mesh_space_rotation_blend"),
					Operation.bMeshSpaceRotationBlend))
			{
				OutError = FString::Printf(
					TEXT("Operation %d field 'mesh_space_rotation_blend' must be boolean."),
					OperationIndex);
				return false;
			}
			if (OperationJson->HasField(TEXT("mesh_space_scale_blend"))
				&& !OperationJson->TryGetBoolField(
					TEXT("mesh_space_scale_blend"),
					Operation.bMeshSpaceScaleBlend))
			{
				OutError = FString::Printf(
					TEXT("Operation %d field 'mesh_space_scale_blend' must be boolean."),
					OperationIndex);
				return false;
			}

			if (!ReadBranchFilters(
				*OperationJson,
				OperationIndex,
				*TargetSkeleton,
				Operation,
				OutError))
			{
				return false;
			}
			Operation.Type = EValidatedPatchOperationType::AddLayeredBoneBlend;
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Operation %d uses unsupported op '%s'."),
				OperationIndex,
				*OperationName);
			return false;
		}

		OutOperations.Add(Operation);
	}
	const int32 OperationCategoryCount =
		(bHasCreationOperation ? 1 : 0)
		+ (bHasLayerPoseOperation ? 1 : 0)
		+ (bHasExistingNodeOperation ? 1 : 0);
	if (OperationCategoryCount > 1)
	{
		OutError = TEXT("Node creation, layered-blend pose addition, and existing-pin edits must use separate patches. Apply one structural step, inspect its new pins, then connect them.");
		return false;
	}
	return true;
}
}
