#pragma once

// Deterministic Blueprint graph snapshots shared by inspection and patching.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UAnimBlueprint;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

namespace UEPy::BlueprintGraph
{
inline constexpr int32 BridgeProtocolVersion = 2;
inline constexpr int32 BlueprintPatchFormatVersion = 1;

using FJsonObjectPtr = TSharedPtr<FJsonObject>;
using FJsonValuePtr = TSharedPtr<FJsonValue>;

struct FGraphConnection
{
	const UEdGraphNode* FromNode = nullptr;
	const UEdGraphPin* FromPin = nullptr;
	const UEdGraphNode* ToNode = nullptr;
	const UEdGraphPin* ToPin = nullptr;
};

FString SerializeJson(const FJsonObjectPtr& Object);
FJsonObjectPtr MakeErrorJson(const FString& BlueprintPath, const FString& Error);
FString ObjectPath(const UObject* Object);
FJsonObjectPtr MakeNodeJson(UEdGraphNode* Node, const UAnimBlueprint* AnimBlueprint);
FString ConnectionKey(const FGraphConnection& Connection);
FJsonObjectPtr MakeConnectionJson(const FGraphConnection& Connection);
FString BuildGraphFingerprint(UEdGraph* Graph, const UAnimBlueprint* AnimBlueprint);
bool ResolveBlueprintGraph(
	const FString& BlueprintPath,
	const FString& GraphName,
	UBlueprint*& OutBlueprint,
	UEdGraph*& OutGraph,
	FString& OutError);
}
