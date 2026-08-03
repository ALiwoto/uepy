#include "Blueprints/UEPyBlueprintGraphBridge.h"

#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "AnimationStateMachineGraph.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace
{
constexpr int32 BridgeProtocolVersion = 2;
constexpr int32 BlueprintPatchFormatVersion = 1;
constexpr int32 MaximumPatchOperations = 100;

using FJsonObjectPtr = TSharedPtr<FJsonObject>;
using FJsonValuePtr = TSharedPtr<FJsonValue>;

FString SerializeJson(const FJsonObjectPtr& Object)
{
	FString Result;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Result;
}

FJsonObjectPtr MakeErrorJson(const FString& BlueprintPath, const FString& Error)
{
	FJsonObjectPtr Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("found"), false);
	Result->SetStringField(TEXT("requested_path"), BlueprintPath);
	Result->SetStringField(TEXT("error"), Error);
	return Result;
}

FString ObjectPath(const UObject* Object)
{
	return IsValid(Object) ? Object->GetPathName() : FString();
}

FString PinDirectionName(const EEdGraphPinDirection Direction)
{
	return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
}

FString PinContainerName(const EPinContainerType ContainerType)
{
	switch (ContainerType)
	{
	case EPinContainerType::Array:
		return TEXT("array");
	case EPinContainerType::Set:
		return TEXT("set");
	case EPinContainerType::Map:
		return TEXT("map");
	default:
		return TEXT("none");
	}
}

FString CleanTitle(const UEdGraphNode* Node)
{
	if (!IsValid(Node))
	{
		return FString();
	}

	FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Title.ReplaceInline(TEXT("\r"), TEXT(" "));
	Title.ReplaceInline(TEXT("\n"), TEXT(" "));
	return Title;
}

FJsonObjectPtr MakePinJson(const UEdGraphPin* Pin)
{
	FJsonObjectPtr Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("name"), Pin->PinName.ToString());
	Result->SetStringField(TEXT("friendly_name"), Pin->PinFriendlyName.ToString());
	Result->SetStringField(TEXT("direction"), PinDirectionName(Pin->Direction));
	Result->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
	Result->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
	Result->SetStringField(
		TEXT("subcategory_object"),
		ObjectPath(Pin->PinType.PinSubCategoryObject.Get()));
	Result->SetStringField(TEXT("container"), PinContainerName(Pin->PinType.ContainerType));
	Result->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
	Result->SetBoolField(TEXT("is_const"), Pin->PinType.bIsConst);
	Result->SetBoolField(TEXT("hidden"), Pin->bHidden);
	Result->SetBoolField(TEXT("advanced"), Pin->bAdvancedView);
	Result->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);
	Result->SetBoolField(TEXT("not_connectable"), Pin->bNotConnectable);
	Result->SetStringField(TEXT("default_value"), Pin->DefaultValue);
	Result->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
	Result->SetStringField(TEXT("default_object"), ObjectPath(Pin->DefaultObject));

	TArray<const UEdGraphPin*> LinkedPins;
	LinkedPins.Reserve(Pin->LinkedTo.Num());
	for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
	{
		if (LinkedPin != nullptr && IsValid(LinkedPin->GetOwningNode()))
		{
			LinkedPins.Add(LinkedPin);
		}
	}
	LinkedPins.Sort([](const UEdGraphPin& Left, const UEdGraphPin& Right)
	{
		const UEdGraphNode* LeftNode = Left.GetOwningNode();
		const UEdGraphNode* RightNode = Right.GetOwningNode();
		const FString LeftKey = FString::Printf(
			TEXT("%s|%s|%s"),
			*LeftNode->NodeGuid.ToString(),
			*Left.PinName.ToString(),
			*Left.PinId.ToString());
		const FString RightKey = FString::Printf(
			TEXT("%s|%s|%s"),
			*RightNode->NodeGuid.ToString(),
			*Right.PinName.ToString(),
			*Right.PinId.ToString());
		return LeftKey < RightKey;
	});

	TArray<FJsonValuePtr> Links;
	for (const UEdGraphPin* LinkedPin : LinkedPins)
	{
		const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
		FJsonObjectPtr Link = MakeShared<FJsonObject>();
		Link->SetStringField(
			TEXT("node_id"),
			LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Link->SetStringField(TEXT("node_title"), CleanTitle(LinkedNode));
		Link->SetStringField(
			TEXT("pin_id"),
			LinkedPin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
		Link->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
		Links.Add(MakeShared<FJsonValueObject>(Link));
	}
	Result->SetArrayField(TEXT("links"), Links);
	return Result;
}

void AddSequencePlayerDetails(
	const UAnimGraphNode_SequencePlayer& SequencePlayer,
	const FJsonObjectPtr& Details)
{
	const UAnimSequenceBase* Sequence = SequencePlayer.Node.GetSequence();
	Details->SetStringField(TEXT("kind"), TEXT("sequence_player"));
	Details->SetStringField(TEXT("animation"), ObjectPath(Sequence));
	Details->SetBoolField(TEXT("loop"), SequencePlayer.Node.IsLooping());
	Details->SetNumberField(TEXT("play_rate"), SequencePlayer.Node.GetPlayRate());
	Details->SetNumberField(TEXT("start_position"), SequencePlayer.Node.GetStartPosition());
}

void AddSlotDetails(
	const UAnimGraphNode_Slot& Slot,
	const UAnimBlueprint* AnimBlueprint,
	const FJsonObjectPtr& Details)
{
	Details->SetStringField(TEXT("kind"), TEXT("slot"));
	Details->SetStringField(TEXT("slot_name"), Slot.Node.SlotName.ToString());
	Details->SetBoolField(
		TEXT("always_update_source_pose"),
		Slot.Node.bAlwaysUpdateSourcePose);
	const USkeleton* Skeleton = AnimBlueprint != nullptr
		? AnimBlueprint->TargetSkeleton
		: nullptr;
	Details->SetStringField(
		TEXT("slot_group"),
		Skeleton != nullptr
			? Skeleton->GetSlotGroupName(Slot.Node.SlotName).ToString()
			: FString());
}

void AddLayeredBlendDetails(
	const UAnimGraphNode_LayeredBoneBlend& LayeredBlend,
	const FJsonObjectPtr& Details)
{
	Details->SetStringField(TEXT("kind"), TEXT("layered_bone_blend"));
	const UEnum* BlendModeEnum = StaticEnum<ELayeredBoneBlendMode>();
	Details->SetStringField(
		TEXT("blend_mode"),
		BlendModeEnum != nullptr
			? BlendModeEnum->GetNameStringByValue(
				static_cast<int64>(LayeredBlend.Node.BlendMode))
			: FString());
	Details->SetBoolField(
		TEXT("mesh_space_rotation_blend"),
		LayeredBlend.Node.bMeshSpaceRotationBlend);
	Details->SetBoolField(
		TEXT("mesh_space_scale_blend"),
		LayeredBlend.Node.bMeshSpaceScaleBlend);
	Details->SetBoolField(
		TEXT("blend_root_motion_based_on_root_bone"),
		LayeredBlend.Node.bBlendRootMotionBasedOnRootBone);
	Details->SetNumberField(TEXT("lod_threshold"), LayeredBlend.Node.LODThreshold);

	TArray<FJsonValuePtr> Layers;
	const int32 LayerCount = FMath::Max(
		LayeredBlend.Node.BlendPoses.Num(),
		FMath::Max(
			LayeredBlend.Node.LayerSetup.Num(),
			LayeredBlend.Node.BlendMasks.Num()));
	for (int32 LayerIndex = 0; LayerIndex < LayerCount; ++LayerIndex)
	{
		FJsonObjectPtr Layer = MakeShared<FJsonObject>();
		Layer->SetNumberField(TEXT("index"), LayerIndex);
		Layer->SetNumberField(
			TEXT("default_weight"),
			LayeredBlend.Node.BlendWeights.IsValidIndex(LayerIndex)
				? LayeredBlend.Node.BlendWeights[LayerIndex]
				: 0.0f);
		Layer->SetStringField(
			TEXT("blend_mask"),
			LayeredBlend.Node.BlendMasks.IsValidIndex(LayerIndex)
				? ObjectPath(LayeredBlend.Node.BlendMasks[LayerIndex])
				: FString());

		TArray<FJsonValuePtr> BranchFilters;
		if (LayeredBlend.Node.LayerSetup.IsValidIndex(LayerIndex))
		{
			for (const FBranchFilter& Filter
				: LayeredBlend.Node.LayerSetup[LayerIndex].BranchFilters)
			{
				FJsonObjectPtr FilterJson = MakeShared<FJsonObject>();
				FilterJson->SetStringField(TEXT("bone"), Filter.BoneName.ToString());
				FilterJson->SetNumberField(TEXT("blend_depth"), Filter.BlendDepth);
				BranchFilters.Add(MakeShared<FJsonValueObject>(FilterJson));
			}
		}
		Layer->SetArrayField(TEXT("branch_filters"), BranchFilters);
		Layers.Add(MakeShared<FJsonValueObject>(Layer));
	}
	Details->SetArrayField(TEXT("layers"), Layers);
}

FJsonObjectPtr MakeNodeDetails(
	UEdGraphNode* Node,
	const UAnimBlueprint* AnimBlueprint)
{
	FJsonObjectPtr Details = MakeShared<FJsonObject>();
	if (const UAnimGraphNode_SequencePlayer* SequencePlayer =
		Cast<UAnimGraphNode_SequencePlayer>(Node))
	{
		AddSequencePlayerDetails(*SequencePlayer, Details);
	}
	else if (const UAnimGraphNode_Slot* Slot = Cast<UAnimGraphNode_Slot>(Node))
	{
		AddSlotDetails(*Slot, AnimBlueprint, Details);
	}
	else if (const UAnimGraphNode_LayeredBoneBlend* LayeredBlend =
		Cast<UAnimGraphNode_LayeredBoneBlend>(Node))
	{
		AddLayeredBlendDetails(*LayeredBlend, Details);
	}
	else if (const UAnimGraphNode_SaveCachedPose* SaveCachedPose =
		Cast<UAnimGraphNode_SaveCachedPose>(Node))
	{
		Details->SetStringField(TEXT("kind"), TEXT("save_cached_pose"));
		Details->SetStringField(TEXT("cache_name"), SaveCachedPose->CacheName);
	}
	else if (const UAnimGraphNode_UseCachedPose* UseCachedPose =
		Cast<UAnimGraphNode_UseCachedPose>(Node))
	{
		Details->SetStringField(TEXT("kind"), TEXT("use_cached_pose"));
		const UAnimGraphNode_SaveCachedPose* ResolvedSaveCachedPose =
			UseCachedPose->SaveCachedPoseNode.Get();
		Details->SetStringField(
			TEXT("cache_name"),
			ResolvedSaveCachedPose != nullptr
				? ResolvedSaveCachedPose->CacheName
				: FString());
	}
	else if (UAnimGraphNode_StateMachineBase* StateMachine =
		Cast<UAnimGraphNode_StateMachineBase>(Node))
	{
		Details->SetStringField(TEXT("kind"), TEXT("state_machine"));
		Details->SetStringField(
			TEXT("state_machine_name"),
			StateMachine->GetStateMachineName());
		Details->SetStringField(
			TEXT("state_machine_graph"),
			ObjectPath(StateMachine->EditorStateMachineGraph));
	}
	return Details;
}

FJsonObjectPtr MakeNodeJson(
	UEdGraphNode* Node,
	const UAnimBlueprint* AnimBlueprint)
{
	FJsonObjectPtr Result = MakeShared<FJsonObject>();
	Result->SetStringField(
		TEXT("id"),
		Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("title"), CleanTitle(Node));
	Result->SetStringField(TEXT("name"), Node->GetName());
	Result->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
	Result->SetNumberField(TEXT("x"), Node->NodePosX);
	Result->SetNumberField(TEXT("y"), Node->NodePosY);
	Result->SetStringField(TEXT("comment"), Node->NodeComment);
	Result->SetBoolField(TEXT("comment_visible"), Node->bCommentBubbleVisible);

	TArray<FString> SubGraphPaths;
	for (const UEdGraph* SubGraph : Node->GetSubGraphs())
	{
		if (IsValid(SubGraph))
		{
			SubGraphPaths.Add(SubGraph->GetPathName());
		}
	}
	SubGraphPaths.Sort();
	TArray<FJsonValuePtr> SubGraphs;
	for (const FString& SubGraphPath : SubGraphPaths)
	{
		SubGraphs.Add(MakeShared<FJsonValueString>(SubGraphPath));
	}
	Result->SetArrayField(TEXT("subgraphs"), SubGraphs);

	TArray<const UEdGraphPin*> Pins;
	Pins.Reserve(Node->Pins.Num());
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin != nullptr)
		{
			Pins.Add(Pin);
		}
	}
	Pins.Sort([](const UEdGraphPin& Left, const UEdGraphPin& Right)
	{
		const FString LeftKey = FString::Printf(
			TEXT("%d|%s|%s"),
			static_cast<int32>(Left.Direction),
			*Left.PinName.ToString(),
			*Left.PinId.ToString());
		const FString RightKey = FString::Printf(
			TEXT("%d|%s|%s"),
			static_cast<int32>(Right.Direction),
			*Right.PinName.ToString(),
			*Right.PinId.ToString());
		return LeftKey < RightKey;
	});

	TArray<FJsonValuePtr> PinValues;
	for (const UEdGraphPin* Pin : Pins)
	{
		PinValues.Add(MakeShared<FJsonValueObject>(MakePinJson(Pin)));
	}
	Result->SetArrayField(TEXT("pins"), PinValues);

	const FJsonObjectPtr Details = MakeNodeDetails(Node, AnimBlueprint);
	if (Details->Values.Num() > 0)
	{
		Result->SetObjectField(TEXT("details"), Details);
	}
	return Result;
}

struct FGraphConnection
{
	const UEdGraphNode* FromNode = nullptr;
	const UEdGraphPin* FromPin = nullptr;
	const UEdGraphNode* ToNode = nullptr;
	const UEdGraphPin* ToPin = nullptr;
};

FString ConnectionKey(const FGraphConnection& Connection)
{
	return FString::Printf(
		TEXT("%s|%s|%s|%s"),
		*Connection.FromNode->NodeGuid.ToString(),
		*Connection.FromPin->PinId.ToString(),
		*Connection.ToNode->NodeGuid.ToString(),
		*Connection.ToPin->PinId.ToString());
}

FJsonObjectPtr MakeConnectionJson(const FGraphConnection& Connection)
{
	FJsonObjectPtr Result = MakeShared<FJsonObject>();
	Result->SetStringField(
		TEXT("from_node_id"),
		Connection.FromNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("from_node_title"), CleanTitle(Connection.FromNode));
	Result->SetStringField(TEXT("from_pin"), Connection.FromPin->PinName.ToString());
	Result->SetStringField(
		TEXT("to_node_id"),
		Connection.ToNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("to_node_title"), CleanTitle(Connection.ToNode));
	Result->SetStringField(TEXT("to_pin"), Connection.ToPin->PinName.ToString());
	Result->SetStringField(
		TEXT("flow"),
		FString::Printf(
			TEXT("%s.%s -> %s.%s"),
			*CleanTitle(Connection.FromNode),
			*Connection.FromPin->PinName.ToString(),
			*CleanTitle(Connection.ToNode),
			*Connection.ToPin->PinName.ToString()));
	return Result;
}

TArray<UEdGraphNode*> GetSortedGraphNodes(UEdGraph* Graph)
{
	TArray<UEdGraphNode*> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (IsValid(Node))
		{
			Nodes.Add(Node);
		}
	}
	Nodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
	{
		return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
	});
	return Nodes;
}

FString BuildGraphFingerprint(
	UEdGraph* Graph,
	const UAnimBlueprint* AnimBlueprint)
{
	FString CanonicalGraph;
	for (UEdGraphNode* Node : GetSortedGraphNodes(Graph))
	{
		CanonicalGraph += SerializeJson(MakeNodeJson(Node, AnimBlueprint));
		CanonicalGraph += TEXT("\n");
	}

	const FTCHARToUTF8 EncodedGraph(*CanonicalGraph);
	return LexToString(FSHA1::HashBuffer(
		EncodedGraph.Get(),
		EncodedGraph.Length()));
}

bool ResolveBlueprintGraph(
	const FString& BlueprintPath,
	const FString& GraphName,
	UBlueprint*& OutBlueprint,
	UEdGraph*& OutGraph,
	FString& OutError)
{
	OutBlueprint = Cast<UBlueprint>(LoadObject<UObject>(nullptr, *BlueprintPath));
	if (OutBlueprint == nullptr)
	{
		OutError = TEXT("Asset was not found or is not a Blueprint.");
		return false;
	}

	TArray<UEdGraph*> Graphs;
	OutBlueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (IsValid(Graph)
			&& Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			OutGraph = Graph;
			return true;
		}
	}

	OutError = TEXT("Requested graph was not found.");
	return false;
}

enum class EValidatedPatchOperationType : uint8
{
	Connect,
	Disconnect,
	MoveNode,
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
};

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

bool ValidatePatchOperations(
	const FJsonObject& Patch,
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
	return true;
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
	if (!ValidatePatchOperations(*Patch, Graph, Operations, Error))
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

FString UUEPyBlueprintGraphBridge::InspectBlueprintGraphJson(
	const FString& BlueprintPath,
	const FString& GraphName)
{
	UObject* Asset = LoadObject<UObject>(nullptr, *BlueprintPath);
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	if (Blueprint == nullptr)
	{
		return SerializeJson(MakeErrorJson(
			BlueprintPath,
			Asset == nullptr
				? TEXT("Asset was not found.")
				: TEXT("Asset is not a Blueprint.")));
	}

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	Graphs.RemoveAll([](const UEdGraph* Graph)
	{
		return !IsValid(Graph);
	});
	Graphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});

	FJsonObjectPtr Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("bridge_protocol_version"), BridgeProtocolVersion);
	Result->SetBoolField(TEXT("found"), true);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("class"), Blueprint->GetClass()->GetPathName());
	Result->SetStringField(TEXT("parent_class"), ObjectPath(Blueprint->ParentClass));
	const UClass* NativeParentClass = Blueprint->ParentClass;
	while (NativeParentClass != nullptr
		&& !NativeParentClass->HasAnyClassFlags(CLASS_Native))
	{
		NativeParentClass = NativeParentClass->GetSuperClass();
	}
	Result->SetStringField(TEXT("native_parent"), ObjectPath(NativeParentClass));
	Result->SetStringField(TEXT("generated_class"), ObjectPath(Blueprint->GeneratedClass));
	Result->SetBoolField(
		TEXT("package_dirty"),
		Blueprint->GetOutermost() != nullptr && Blueprint->GetOutermost()->IsDirty());

	TArray<FJsonValuePtr> GraphSummaries;
	UEdGraph* RequestedGraph = nullptr;
	for (UEdGraph* Graph : Graphs)
	{
		FJsonObjectPtr Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("name"), Graph->GetName());
		Summary->SetStringField(TEXT("path"), Graph->GetPathName());
		Summary->SetStringField(TEXT("class"), Graph->GetClass()->GetPathName());
		Summary->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
		GraphSummaries.Add(MakeShared<FJsonValueObject>(Summary));

		if (RequestedGraph == nullptr
			&& Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			RequestedGraph = Graph;
		}
	}
	Result->SetArrayField(TEXT("graphs"), GraphSummaries);

	if (RequestedGraph == nullptr)
	{
		Result->SetBoolField(TEXT("graph_found"), false);
		Result->SetStringField(TEXT("requested_graph"), GraphName);
		Result->SetStringField(TEXT("error"), TEXT("Requested graph was not found."));
		return SerializeJson(Result);
	}

	Result->SetBoolField(TEXT("graph_found"), true);
	FJsonObjectPtr GraphJson = MakeShared<FJsonObject>();
	GraphJson->SetStringField(TEXT("name"), RequestedGraph->GetName());
	GraphJson->SetStringField(TEXT("path"), RequestedGraph->GetPathName());
	GraphJson->SetStringField(TEXT("class"), RequestedGraph->GetClass()->GetPathName());
	GraphJson->SetStringField(
		TEXT("schema"),
		RequestedGraph->GetSchema() != nullptr
			? RequestedGraph->GetSchema()->GetClass()->GetPathName()
			: FString());

	TArray<UEdGraphNode*> Nodes;
	for (UEdGraphNode* Node : RequestedGraph->Nodes)
	{
		if (IsValid(Node))
		{
			Nodes.Add(Node);
		}
	}
	Nodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
	{
		if (Left.NodePosX != Right.NodePosX)
		{
			return Left.NodePosX < Right.NodePosX;
		}
		if (Left.NodePosY != Right.NodePosY)
		{
			return Left.NodePosY < Right.NodePosY;
		}
		return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
	});

	const UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint);
	GraphJson->SetStringField(
		TEXT("fingerprint"),
		BuildGraphFingerprint(RequestedGraph, AnimBlueprint));
	TArray<FJsonValuePtr> NodeValues;
	for (UEdGraphNode* Node : Nodes)
	{
		NodeValues.Add(MakeShared<FJsonValueObject>(
			MakeNodeJson(Node, AnimBlueprint)));
	}
	GraphJson->SetArrayField(TEXT("nodes"), NodeValues);

	TArray<FGraphConnection> Connections;
	for (const UEdGraphNode* Node : Nodes)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				const UEdGraphNode* LinkedNode = LinkedPin != nullptr
					? LinkedPin->GetOwningNode()
					: nullptr;
				if (IsValid(LinkedNode))
				{
					Connections.Add({ Node, Pin, LinkedNode, LinkedPin });
				}
			}
		}
	}
	Connections.Sort([](const FGraphConnection& Left, const FGraphConnection& Right)
	{
		return ConnectionKey(Left) < ConnectionKey(Right);
	});

	TArray<FJsonValuePtr> ConnectionValues;
	for (const FGraphConnection& Connection : Connections)
	{
		ConnectionValues.Add(MakeShared<FJsonValueObject>(
			MakeConnectionJson(Connection)));
	}
	GraphJson->SetArrayField(TEXT("connections"), ConnectionValues);
	Result->SetObjectField(TEXT("graph"), GraphJson);
	return SerializeJson(Result);
}

FString UUEPyBlueprintGraphBridge::ValidateBlueprintGraphPatchJson(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& PatchJson)
{
	return RunBlueprintGraphPatch(
		BlueprintPath,
		GraphName,
		PatchJson,
		false);
}

FString UUEPyBlueprintGraphBridge::ApplyBlueprintGraphPatchJson(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& PatchJson)
{
	return RunBlueprintGraphPatch(
		BlueprintPath,
		GraphName,
		PatchJson,
		true);
}

int32 UUEPyBlueprintGraphBridge::GetBridgeProtocolVersion()
{
	return BridgeProtocolVersion;
}
