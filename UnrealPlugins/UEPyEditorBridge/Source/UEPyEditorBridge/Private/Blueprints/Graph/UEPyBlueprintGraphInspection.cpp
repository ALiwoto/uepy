#include "Blueprints/Graph/UEPyBlueprintGraphInspection.h"

#include "Blueprints/Graph/UEPyBlueprintGraphSerialization.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"

namespace UEPy::BlueprintGraph
{
FString InspectBlueprintGraphJson(
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
}
