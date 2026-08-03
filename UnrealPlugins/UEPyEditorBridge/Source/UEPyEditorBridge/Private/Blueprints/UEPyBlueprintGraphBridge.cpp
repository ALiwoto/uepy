#include "Blueprints/UEPyBlueprintGraphBridge.h"

#include "Blueprints/Graph/UEPyBlueprintGraphSerialization.h"
#include "Blueprints/Graph/UEPyBlueprintGraphInspection.h"
#include "Blueprints/Graph/Patching/UEPyBlueprintGraphPatch.h"

FString UUEPyBlueprintGraphBridge::InspectBlueprintGraphJson(
	const FString& BlueprintPath,
	const FString& GraphName)
{
	return UEPy::BlueprintGraph::InspectBlueprintGraphJson(BlueprintPath, GraphName);
}

FString UUEPyBlueprintGraphBridge::ValidateBlueprintGraphPatchJson(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& PatchJson)
{
	return UEPy::BlueprintGraph::RunBlueprintGraphPatch(
		BlueprintPath, GraphName, PatchJson, false);
}

FString UUEPyBlueprintGraphBridge::ApplyBlueprintGraphPatchJson(
	const FString& BlueprintPath,
	const FString& GraphName,
	const FString& PatchJson)
{
	return UEPy::BlueprintGraph::RunBlueprintGraphPatch(
		BlueprintPath, GraphName, PatchJson, true);
}

int32 UUEPyBlueprintGraphBridge::GetBridgeProtocolVersion()
{
	return UEPy::BlueprintGraph::BridgeProtocolVersion;
}
