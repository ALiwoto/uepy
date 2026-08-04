using UnrealBuildTool;

public class UEPyEditorBridge : ModuleRules
{
	public UEPyEditorBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AnimGraph",
			"AnimGraphRuntime",
			"AssetRegistry",
			"Json",
			"MeshDescription",
			"MeshReductionInterface",
			"MeshUtilitiesCommon",
			"StaticMeshDescription",
			"UnrealEd"
		});
	}
}
