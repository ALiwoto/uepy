#include "Blueprints/Graph/UEPyBlueprintGraphSerialization.h"

#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "AnimationStateMachineGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UEPy::BlueprintGraph
{
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

namespace
{
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

namespace
{
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
}
