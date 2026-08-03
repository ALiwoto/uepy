#include "Blueprints/Graph/Patching/UEPyBlueprintGraphPatch.h"

#include "Blueprints/Graph/UEPyBlueprintGraphSerialization.h"
#include "Blueprints/Graph/Patching/UEPyBlueprintGraphPatchTypes.h"
#include "Blueprints/Graph/Patching/UEPyBlueprintGraphPatchValidation.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

namespace UEPy::BlueprintGraph
{
namespace
{
FJsonObjectPtr MakePatchResult(
	const FString& BlueprintPath,
	const FString& GraphName)
{
	FJsonObjectPtr Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("bridge_protocol_version"), BridgeProtocolVersion);
	Result->SetNumberField(TEXT("patch_format_version"), BlueprintPatchFormatVersion);
	Result->SetStringField(TEXT("requested_path"), BlueprintPath);
	Result->SetStringField(TEXT("requested_graph"), GraphName);
	Result->SetBoolField(TEXT("valid"), false);
	Result->SetBoolField(TEXT("applied"), false);
	Result->SetBoolField(TEXT("changed"), false);
	return Result;
}

FString FinishPatchError(
	const FJsonObjectPtr& Result,
	const FString& Error)
{
	Result->SetStringField(TEXT("error"), Error);
	return SerializeJson(Result);
}

void FinalizeCreatedGraphNode(
	UEdGraph* Graph,
	UEdGraphNode* Node,
	const int32 X,
	const int32 Y)
{
	Node->CreateNewGuid();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	Node->SetFlags(RF_Transactional);
	Node->AllocateDefaultPins();
	Node->PostPlacedNewNode();
	Graph->AddNode(Node, false, false);
}

void RollBackCreatedGraphNodes(
	const TArray<TPair<FString, UEdGraphNode*>>& CreatedNodes,
	UEdGraph* Graph,
	UPackage* Package,
	FScopedTransaction& Transaction)
{
	for (int32 NodeIndex = CreatedNodes.Num() - 1;
		NodeIndex >= 0;
		--NodeIndex)
	{
		UEdGraphNode* Node = CreatedNodes[NodeIndex].Value;
		if (IsValid(Node))
		{
			Node->DestroyNode();
		}
	}
	Transaction.Cancel();
	Graph->NotifyGraphChanged();
	if (Package != nullptr)
	{
		Package->SetDirtyFlag(false);
	}
}
}

FString RunBlueprintGraphPatch(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& PatchJson,
	const bool bApply)
{
	FJsonObjectPtr Result = MakePatchResult(BlueprintPath, GraphName);
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	FString Error;
	if (!ResolveBlueprintGraph(
		BlueprintPath,
		GraphName,
		Blueprint,
		Graph,
		Error))
	{
		return FinishPatchError(Result, Error);
	}

	const UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint);
	const FString CurrentFingerprint = BuildGraphFingerprint(Graph, AnimBlueprint);
	Result->SetStringField(TEXT("current_fingerprint"), CurrentFingerprint);
	Result->SetBoolField(
		TEXT("package_dirty"),
		Blueprint->GetOutermost() != nullptr
			&& Blueprint->GetOutermost()->IsDirty());

	FJsonObjectPtr Patch;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PatchJson);
	if (!FJsonSerializer::Deserialize(Reader, Patch) || !Patch.IsValid())
	{
		return FinishPatchError(Result, TEXT("Patch is not valid JSON."));
	}

	int32 PatchVersion = 0;
	if (!Patch->TryGetNumberField(TEXT("version"), PatchVersion)
		|| PatchVersion != BlueprintPatchFormatVersion)
	{
		return FinishPatchError(
			Result,
			FString::Printf(
				TEXT("Patch 'version' must be %d."),
				BlueprintPatchFormatVersion));
	}

	FString ExpectedFingerprint;
	if (!Patch->TryGetStringField(
		TEXT("expected_fingerprint"),
		ExpectedFingerprint)
		|| !ExpectedFingerprint.Equals(
			CurrentFingerprint,
			ESearchCase::IgnoreCase))
	{
		return FinishPatchError(
			Result,
			TEXT("Patch fingerprint does not match the current graph. Inspect it again before patching."));
	}

	TArray<FValidatedPatchOperation> Operations;
	if (!ValidatePatchOperations(*Patch, Blueprint, Graph, Operations, Error))
	{
		return FinishPatchError(Result, Error);
	}
	Result->SetNumberField(TEXT("operation_count"), Operations.Num());
	Result->SetBoolField(TEXT("valid"), true);
	if (!bApply)
	{
		Result->SetStringField(TEXT("resulting_fingerprint"), CurrentFingerprint);
		return SerializeJson(Result);
	}

	UPackage* Package = Blueprint->GetOutermost();
	if (Package != nullptr && Package->IsDirty())
	{
		Result->SetBoolField(TEXT("valid"), false);
		return FinishPatchError(
			Result,
			TEXT("Refusing to patch a dirty Blueprint package. Save or revert it first."));
	}

	const UEdGraphSchema* Schema = Graph->GetSchema();
	FScopedTransaction Transaction(
		FText::FromString(TEXT("Apply uepy Blueprint graph patch")));
	Blueprint->Modify();
	Graph->Modify();

	if (!Operations.IsEmpty()
		&& Operations[0].Type
			== EValidatedPatchOperationType::AddLayeredBoneBlendPose)
	{
		TArray<FJsonValuePtr> UpdatedNodeValues;
		for (const FValidatedPatchOperation& Operation : Operations)
		{
			UAnimGraphNode_LayeredBoneBlend* LayeredBlend =
				CastChecked<UAnimGraphNode_LayeredBoneBlend>(Operation.Node);
			LayeredBlend->Modify();
			const int32 NewPoseIndex = LayeredBlend->Node.BlendPoses.Num();
			LayeredBlend->Node.AddPose();
			LayeredBlend->Node.LayerSetup[NewPoseIndex].BranchFilters =
				Operation.BranchFilters;
			LayeredBlend->Node.BlendWeights[NewPoseIndex] =
				Operation.DefaultWeight;
			LayeredBlend->Node.InvalidatePerBoneBlendWeights();
			LayeredBlend->ReconstructNode();

			FJsonObjectPtr UpdatedNodeJson =
				MakeNodeJson(LayeredBlend, AnimBlueprint);
			UpdatedNodeJson->SetStringField(TEXT("alias"), Operation.Alias);
			UpdatedNodeJson->SetNumberField(
				TEXT("added_pose_index"),
				NewPoseIndex);
			UpdatedNodeValues.Add(
				MakeShared<FJsonValueObject>(UpdatedNodeJson));
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Result->SetArrayField(TEXT("updated_nodes"), UpdatedNodeValues);
		Result->SetBoolField(TEXT("applied"), true);
		Result->SetBoolField(TEXT("changed"), true);
		Result->SetBoolField(
			TEXT("package_dirty"),
			Package != nullptr && Package->IsDirty());
		Result->SetStringField(
			TEXT("resulting_fingerprint"),
			BuildGraphFingerprint(Graph, AnimBlueprint));
		return SerializeJson(Result);
	}

	if (!Operations.IsEmpty() && IsCreationOperation(Operations[0].Type))
	{
		TArray<UAnimGraphNode_SaveCachedPose*> ExistingCachedPoseNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(
			Blueprint,
			ExistingCachedPoseNodes);
		TMap<FString, UAnimGraphNode_SaveCachedPose*> CachedPoseNodes;
		for (UAnimGraphNode_SaveCachedPose* ExistingCachedPose
			: ExistingCachedPoseNodes)
		{
			if (IsValid(ExistingCachedPose))
			{
				CachedPoseNodes.Add(
					NormalizedPatchName(ExistingCachedPose->CacheName),
					ExistingCachedPose);
			}
		}

		TArray<TPair<FString, UEdGraphNode*>> CreatedNodes;
		for (const FValidatedPatchOperation& Operation : Operations)
		{
			UEdGraphNode* CreatedNode = nullptr;
			switch (Operation.Type)
			{
			case EValidatedPatchOperationType::AddSaveCachedPose:
			{
				UAnimGraphNode_SaveCachedPose* SaveCachedPose =
					NewObject<UAnimGraphNode_SaveCachedPose>(Graph);
				if (SaveCachedPose != nullptr)
				{
					SaveCachedPose->CacheName = Operation.Name;
					FinalizeCreatedGraphNode(
						Graph,
						SaveCachedPose,
						Operation.X,
						Operation.Y);
					CachedPoseNodes.Add(
						NormalizedPatchName(Operation.Name),
						SaveCachedPose);
					CreatedNode = SaveCachedPose;
				}
				break;
			}

			case EValidatedPatchOperationType::AddUseCachedPose:
			{
				UAnimGraphNode_SaveCachedPose* const* SaveCachedPose =
					CachedPoseNodes.Find(NormalizedPatchName(Operation.Name));
				if (SaveCachedPose != nullptr && IsValid(*SaveCachedPose))
				{
					UAnimGraphNode_UseCachedPose* UseCachedPose =
						NewObject<UAnimGraphNode_UseCachedPose>(Graph);
					if (UseCachedPose != nullptr)
					{
						UseCachedPose->SaveCachedPoseNode = *SaveCachedPose;
						FinalizeCreatedGraphNode(
							Graph,
							UseCachedPose,
							Operation.X,
							Operation.Y);
						CreatedNode = UseCachedPose;
					}
				}
				break;
			}

			case EValidatedPatchOperationType::AddSlot:
			{
				UAnimGraphNode_Slot* Slot = NewObject<UAnimGraphNode_Slot>(Graph);
				if (Slot != nullptr)
				{
					Slot->Node.SlotName = FName(*Operation.Name);
					Slot->Node.bAlwaysUpdateSourcePose =
						Operation.bAlwaysUpdateSourcePose;
					FinalizeCreatedGraphNode(
						Graph,
						Slot,
						Operation.X,
						Operation.Y);
					CreatedNode = Slot;
				}
				break;
			}

			case EValidatedPatchOperationType::AddLayeredBoneBlend:
			{
				UAnimGraphNode_LayeredBoneBlend* LayeredBlend =
					NewObject<UAnimGraphNode_LayeredBoneBlend>(Graph);
				if (LayeredBlend != nullptr)
				{
					LayeredBlend->Node.BlendMode =
						ELayeredBoneBlendMode::BranchFilter;
					if (LayeredBlend->Node.LayerSetup.IsEmpty())
					{
						LayeredBlend->Node.AddPose();
					}
					LayeredBlend->Node.LayerSetup[0].BranchFilters =
						Operation.BranchFilters;
					LayeredBlend->Node.BlendWeights[0] = Operation.DefaultWeight;
					LayeredBlend->Node.bMeshSpaceRotationBlend =
						Operation.bMeshSpaceRotationBlend;
					LayeredBlend->Node.bMeshSpaceScaleBlend =
						Operation.bMeshSpaceScaleBlend;
					FinalizeCreatedGraphNode(
						Graph,
						LayeredBlend,
						Operation.X,
						Operation.Y);
					CreatedNode = LayeredBlend;
				}
				break;
			}

			case EValidatedPatchOperationType::Connect:
			case EValidatedPatchOperationType::Disconnect:
			case EValidatedPatchOperationType::MoveNode:
			case EValidatedPatchOperationType::AddLayeredBoneBlendPose:
				break;
			}

			if (CreatedNode == nullptr)
			{
				RollBackCreatedGraphNodes(
					CreatedNodes,
					Graph,
					Package,
					Transaction);
				return FinishPatchError(
					Result,
					FString::Printf(
						TEXT("Failed to create node for alias '%s'; all nodes created by this patch were rolled back."),
						*Operation.Alias));
			}
			CreatedNodes.Emplace(Operation.Alias, CreatedNode);
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		const FString ResultingFingerprint =
			BuildGraphFingerprint(Graph, AnimBlueprint);
		TArray<FJsonValuePtr> CreatedNodeValues;
		for (const TPair<FString, UEdGraphNode*>& CreatedNode : CreatedNodes)
		{
			FJsonObjectPtr CreatedNodeJson =
				MakeNodeJson(CreatedNode.Value, AnimBlueprint);
			CreatedNodeJson->SetStringField(TEXT("alias"), CreatedNode.Key);
			CreatedNodeValues.Add(
				MakeShared<FJsonValueObject>(CreatedNodeJson));
		}
		Result->SetArrayField(TEXT("created_nodes"), CreatedNodeValues);
		Result->SetBoolField(TEXT("applied"), true);
		Result->SetBoolField(TEXT("changed"), true);
		Result->SetBoolField(
			TEXT("package_dirty"),
			Package != nullptr && Package->IsDirty());
		Result->SetStringField(
			TEXT("resulting_fingerprint"),
			ResultingFingerprint);
		return SerializeJson(Result);
	}

	int32 AppliedOperationCount = 0;
	for (FValidatedPatchOperation& Operation : Operations)
	{
		switch (Operation.Type)
		{
		case EValidatedPatchOperationType::Connect:
			Operation.FromPin->GetOwningNode()->Modify();
			Operation.ToPin->GetOwningNode()->Modify();
			if (!Schema->TryCreateConnection(Operation.FromPin, Operation.ToPin))
			{
				for (int32 RollbackIndex = AppliedOperationCount - 1;
					RollbackIndex >= 0;
					--RollbackIndex)
				{
					const FValidatedPatchOperation& AppliedOperation =
						Operations[RollbackIndex];
					switch (AppliedOperation.Type)
					{
					case EValidatedPatchOperationType::Connect:
						Schema->BreakSinglePinLink(
							AppliedOperation.FromPin,
							AppliedOperation.ToPin);
						break;
					case EValidatedPatchOperationType::Disconnect:
						Schema->TryCreateConnection(
							AppliedOperation.FromPin,
							AppliedOperation.ToPin);
						break;
					case EValidatedPatchOperationType::MoveNode:
						AppliedOperation.Node->NodePosX = AppliedOperation.OriginalX;
						AppliedOperation.Node->NodePosY = AppliedOperation.OriginalY;
						break;
					case EValidatedPatchOperationType::AddSaveCachedPose:
					case EValidatedPatchOperationType::AddUseCachedPose:
					case EValidatedPatchOperationType::AddSlot:
					case EValidatedPatchOperationType::AddLayeredBoneBlend:
					case EValidatedPatchOperationType::AddLayeredBoneBlendPose:
						break;
					}
				}
				Transaction.Cancel();
				Graph->NotifyGraphChanged();
				if (Package != nullptr)
				{
					Package->SetDirtyFlag(false);
				}
				return FinishPatchError(
					Result,
					TEXT("A prevalidated connection unexpectedly failed; earlier operations were rolled back."));
			}
			break;

		case EValidatedPatchOperationType::Disconnect:
			Operation.FromPin->GetOwningNode()->Modify();
			Operation.ToPin->GetOwningNode()->Modify();
			Schema->BreakSinglePinLink(Operation.FromPin, Operation.ToPin);
			break;

		case EValidatedPatchOperationType::MoveNode:
			Operation.Node->Modify();
			Operation.Node->NodePosX = Operation.X;
			Operation.Node->NodePosY = Operation.Y;
			break;

		case EValidatedPatchOperationType::AddSaveCachedPose:
		case EValidatedPatchOperationType::AddUseCachedPose:
		case EValidatedPatchOperationType::AddSlot:
		case EValidatedPatchOperationType::AddLayeredBoneBlend:
		case EValidatedPatchOperationType::AddLayeredBoneBlendPose:
			break;
		}
		++AppliedOperationCount;
	}

	Graph->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	const FString ResultingFingerprint =
		BuildGraphFingerprint(Graph, AnimBlueprint);
	Result->SetBoolField(TEXT("applied"), true);
	Result->SetBoolField(TEXT("changed"), true);
	Result->SetBoolField(
		TEXT("package_dirty"),
		Package != nullptr && Package->IsDirty());
	Result->SetStringField(TEXT("resulting_fingerprint"), ResultingFingerprint);
	return SerializeJson(Result);
}
}
