#include "Assets/UEPyStaticMeshAssetBridge.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSourceData.h"
#include "HAL/FileManager.h"
#include "IMeshReductionInterfaces.h"
#include "IMeshReductionManagerModule.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "OverlappingCorners.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace
{
constexpr TCHAR DefaultShadowProxySuffix[] = TEXT("_Shadow");
constexpr float MinimumTriangleFraction = 0.0001f;
const FName ShadowProxyMaterialSlotName(TEXT("ShadowProxy"));

FString NormalizeObjectPath(const FString& InputPath)
{
	FString ObjectPath = FPackageName::ExportTextPathToObjectPath(
		InputPath.TrimStartAndEnd());
	if (ObjectPath.StartsWith(TEXT("/All/")))
	{
		ObjectPath.RightChopInline(4, EAllowShrinking::No);
	}

	if (!ObjectPath.Contains(TEXT("."))
		&& FPackageName::IsValidLongPackageName(ObjectPath))
	{
		const FString AssetName =
			FPackageName::GetLongPackageAssetName(ObjectPath);
		ObjectPath = FString::Printf(
			TEXT("%s.%s"),
			*ObjectPath,
			*AssetName);
	}

	return ObjectPath;
}

FString MakeDefaultDestinationObjectPath(const UStaticMesh& SourceMesh)
{
	const FString SourcePackagePath = FPackageName::GetLongPackagePath(
		SourceMesh.GetOutermost()->GetName());
	const FString DestinationAssetName =
		SourceMesh.GetName() + DefaultShadowProxySuffix;
	return FString::Printf(
		TEXT("%s/%s.%s"),
		*SourcePackagePath,
		*DestinationAssetName,
		*DestinationAssetName);
}

EUEPyShadowProxyBakeResult Fail(
	const EUEPyShadowProxyBakeResult Result,
	const FString& Message,
	FString& OutError)
{
	OutError = Message;
	return Result;
}

void CollapseToOneMaterialSection(FMeshDescription& MeshDescription)
{
	const FPolygonGroupID FirstPolygonGroup =
		MeshDescription.PolygonGroups().GetFirstValidID();
	TMap<FPolygonGroupID, FPolygonGroupID> PolygonGroupRemap;
	for (const FPolygonGroupID PolygonGroupId
		: MeshDescription.PolygonGroups().GetElementIDs())
	{
		PolygonGroupRemap.Add(PolygonGroupId, FirstPolygonGroup);
	}
	MeshDescription.RemapPolygonGroups(PolygonGroupRemap);

	FStaticMeshAttributes Attributes(MeshDescription);
	TPolygonGroupAttributesRef<FName> MaterialSlotNames =
		Attributes.GetPolygonGroupMaterialSlotNames();
	MaterialSlotNames[FirstPolygonGroup] = ShadowProxyMaterialSlotName;

	TVertexInstanceAttributesRef<FVector2f> VertexInstanceUvs =
		Attributes.GetVertexInstanceUVs();
	VertexInstanceUvs.SetNumChannels(1);
}

void ConfigureProxyAsset(
	UStaticMesh& ProxyMesh,
	const UStaticMesh& SourceMesh,
	FMeshDescription&& ReducedMeshDescription)
{
	ProxyMesh.Modify();
	ProxyMesh.ClearMeshDescriptions();
	ProxyMesh.ClearHiResMeshDescription();
	ProxyMesh.SetNumSourceModels(0);

	FStaticMeshSourceModel& ProxySourceModel = ProxyMesh.AddSourceModel();
	ProxySourceModel.BuildSettings =
		SourceMesh.GetSourceModel(0).BuildSettings;
	ProxySourceModel.BuildSettings.bRecomputeNormals = false;
	ProxySourceModel.BuildSettings.bRecomputeTangents = false;
	ProxySourceModel.BuildSettings.bComputeWeightedNormals = false;
	ProxySourceModel.BuildSettings.bUseHighPrecisionTangentBasis = false;
	ProxySourceModel.BuildSettings.bUseFullPrecisionUVs = false;
	ProxySourceModel.BuildSettings.bUseBackwardsCompatibleF16TruncUVs = false;
	ProxySourceModel.BuildSettings.bGenerateLightmapUVs = false;
	ProxySourceModel.BuildSettings.bGenerateDistanceFieldAsIfTwoSided = false;
	ProxySourceModel.BuildSettings.bSupportFaceRemap = false;
	ProxySourceModel.BuildSettings.MinLightmapResolution = 4;
	ProxySourceModel.BuildSettings.SrcLightmapIndex = 0;
	ProxySourceModel.BuildSettings.DstLightmapIndex = 0;
	ProxySourceModel.BuildSettings.DistanceFieldResolutionScale = 0.0f;
	ProxySourceModel.BuildSettings.DistanceFieldReplacementMesh = nullptr;
	ProxySourceModel.BuildSettings.MaxLumenMeshCards = 0;
	ProxySourceModel.ReductionSettings = FMeshReductionSettings();
	ProxySourceModel.ScreenSize.Default = 1.0f;

	ProxyMesh.CreateMeshDescription(0, MoveTemp(ReducedMeshDescription));
	UStaticMesh::FCommitMeshDescriptionParams CommitParameters;
	CommitParameters.bUseHashAsGuid = true;
	ProxyMesh.CommitMeshDescription(0, CommitParameters);

	ProxyMesh.SetStaticMaterials({FStaticMaterial(
		UMaterial::GetDefaultMaterial(MD_Surface),
		ShadowProxyMaterialSlotName,
		ShadowProxyMaterialSlotName)});
	ProxyMesh.GetSectionInfoMap().Clear();
	ProxyMesh.GetOriginalSectionInfoMap().Clear();
	FMeshSectionInfo SectionInfo;
	SectionInfo.MaterialIndex = 0;
	SectionInfo.bEnableCollision = false;
	SectionInfo.bCastShadow = true;
	ProxyMesh.GetSectionInfoMap().Set(0, 0, SectionInfo);
	ProxyMesh.GetOriginalSectionInfoMap().Set(0, 0, SectionInfo);

	ProxyMesh.SetLODGroup(NAME_None, false, false);
	ProxyMesh.NaniteSettings = FMeshNaniteSettings();
	ProxyMesh.NaniteSettings.bEnabled = false;
	ProxyMesh.bAllowCPUAccess = false;
	ProxyMesh.bGenerateMeshDistanceField = false;
	ProxyMesh.bSupportUniformlyDistributedSampling = false;
	ProxyMesh.bSupportGpuUniformlyDistributedSampling = false;
	ProxyMesh.bSupportPhysicalMaterialMasks = false;
	ProxyMesh.SetLightMapResolution(4);
	ProxyMesh.SetLightMapCoordinateIndex(0);
	ProxyMesh.SetPositiveBoundsExtension(
		SourceMesh.GetPositiveBoundsExtension());
	ProxyMesh.SetNegativeBoundsExtension(
		SourceMesh.GetNegativeBoundsExtension());
	ProxyMesh.SetLightingGuid();
	ProxyMesh.SetAssetImportData(nullptr);
	ProxyMesh.Sockets.Reset();
	ProxyMesh.ComplexCollisionMesh = nullptr;
	ProxyMesh.MarkAsNotHavingNavigationData();

	ProxyMesh.CreateBodySetup();
	if (UBodySetup* BodySetup = ProxyMesh.GetBodySetup())
	{
		BodySetup->Modify();
		BodySetup->RemoveSimpleCollision();
		BodySetup->DefaultInstance.SetCollisionProfileName(
			UCollisionProfile::NoCollision_ProfileName);
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		BodySetup->InvalidatePhysicsData();
	}

	ProxyMesh.ImportVersion = EImportStaticMeshVersion::LastVersion;
	ProxyMesh.PostEditChange();
}
}

EUEPyShadowProxyBakeResult UUEPyStaticMeshAssetBridge::BakeShadowProxy(
	const FString& SourceObjectPath,
	const FString& DestinationObjectPath,
	const float TriangleFraction,
	const bool bForce,
	FString& OutDestinationObjectPath,
	int32& OutSourceTriangleCount,
	int32& OutProxyTriangleCount,
	int64& OutSavedPackageBytes,
	FString& OutError)
{
	OutDestinationObjectPath.Reset();
	OutSourceTriangleCount = 0;
	OutProxyTriangleCount = 0;
	OutSavedPackageBytes = 0;
	OutError.Reset();

	if (!FMath::IsFinite(TriangleFraction)
		|| TriangleFraction < MinimumTriangleFraction
		|| TriangleFraction >= 1.0f)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::InvalidTriangleFraction,
			TEXT("Triangle fraction must be at least 0.0001 and less than 1.0."),
			OutError);
	}

	const FString NormalizedSourcePath = NormalizeObjectPath(SourceObjectPath);
	if (!FPackageName::IsValidObjectPath(NormalizedSourcePath))
	{
		return Fail(
			EUEPyShadowProxyBakeResult::InvalidSourcePath,
			TEXT("Source must be a valid mounted Unreal object or package path."),
			OutError);
	}

	UStaticMesh* SourceMesh = Cast<UStaticMesh>(LoadObject<UObject>(
		nullptr,
		*NormalizedSourcePath,
		nullptr,
		LOAD_NoWarn | LOAD_Quiet));
	if (SourceMesh == nullptr)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::SourceNotFound,
			TEXT("Source asset was not found or is not a StaticMesh."),
			OutError);
	}

	OutDestinationObjectPath = DestinationObjectPath.IsEmpty()
		? MakeDefaultDestinationObjectPath(*SourceMesh)
		: NormalizeObjectPath(DestinationObjectPath);
	if (!FPackageName::IsValidObjectPath(OutDestinationObjectPath))
	{
		return Fail(
			EUEPyShadowProxyBakeResult::InvalidDestinationPath,
			TEXT("Destination must be a valid mounted Unreal object or package path."),
			OutError);
	}
	if (OutDestinationObjectPath.Equals(
		SourceMesh->GetPathName(),
		ESearchCase::IgnoreCase))
	{
		return Fail(
			EUEPyShadowProxyBakeResult::DestinationEqualsSource,
			TEXT("Source and destination must be different assets."),
			OutError);
	}

	const FString DestinationPackageName =
		FPackageName::ObjectPathToPackageName(OutDestinationObjectPath);
	const FString DestinationAssetName =
		FPackageName::ObjectPathToObjectName(OutDestinationObjectPath);
	if (FPackageName::GetLongPackageAssetName(DestinationPackageName)
		!= DestinationAssetName)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::InvalidDestinationPath,
			TEXT("Destination object name must match its package asset name."),
			OutError);
	}

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry"));
	const FAssetData ExistingDestinationData =
		AssetRegistryModule.Get().GetAssetByObjectPath(
			FSoftObjectPath(OutDestinationObjectPath));
	UObject* ExistingInMemoryObject = ExistingDestinationData.IsValid()
		? nullptr
		: StaticFindObject(
			UObject::StaticClass(),
			nullptr,
			*OutDestinationObjectPath);
	const bool bDestinationExists =
		ExistingDestinationData.IsValid() || ExistingInMemoryObject != nullptr;
	if (bDestinationExists && !bForce)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::DestinationExists,
			TEXT("Destination already exists; pass --force to rebuild it."),
			OutError);
	}
	if (ExistingDestinationData.IsValid()
		&& ExistingDestinationData.AssetClassPath
			!= UStaticMesh::StaticClass()->GetClassPathName())
	{
		return Fail(
			EUEPyShadowProxyBakeResult::DestinationClassConflict,
			TEXT("Destination exists but is not a StaticMesh."),
			OutError);
	}
	if (ExistingInMemoryObject != nullptr
		&& !ExistingInMemoryObject->IsA<UStaticMesh>())
	{
		return Fail(
			EUEPyShadowProxyBakeResult::DestinationClassConflict,
			TEXT("Destination exists in memory but is not a StaticMesh."),
			OutError);
	}
	UStaticMesh* ExistingDestination = ExistingDestinationData.IsValid()
		? Cast<UStaticMesh>(ExistingDestinationData.GetAsset())
		: Cast<UStaticMesh>(ExistingInMemoryObject);
	if (bDestinationExists && ExistingDestination == nullptr)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::DestinationClassConflict,
			TEXT("Existing destination StaticMesh could not be loaded."),
			OutError);
	}
	if (ExistingDestination != nullptr)
	{
		FStaticMeshCompilingManager::Get().FinishCompilation(
			{ExistingDestination});
	}

	FStaticMeshCompilingManager::Get().FinishCompilation({SourceMesh});
	if (SourceMesh->GetNumSourceModels() == 0)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::SourceMeshUnavailable,
			TEXT("Source mesh has no source LOD0."),
			OutError);
	}

	FMeshDescription SourceMeshDescription;
	if (!SourceMesh->CloneMeshDescription(0, SourceMeshDescription))
	{
		return Fail(
			EUEPyShadowProxyBakeResult::SourceMeshUnavailable,
			TEXT("Source mesh LOD0 has no editable MeshDescription."),
			OutError);
	}
	OutSourceTriangleCount = SourceMeshDescription.Triangles().Num();
	if (OutSourceTriangleCount <= 0)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::SourceMeshUnavailable,
			TEXT("Source mesh LOD0 contains no triangles."),
			OutError);
	}

	IMeshReductionManagerModule* ReductionManager =
		FModuleManager::LoadModulePtr<IMeshReductionManagerModule>(
			TEXT("MeshReductionInterface"));
	IMeshReduction* MeshReduction = ReductionManager != nullptr
		? ReductionManager->GetStaticMeshReductionInterface()
		: nullptr;
	if (MeshReduction == nullptr || !MeshReduction->IsSupported())
	{
		return Fail(
			EUEPyShadowProxyBakeResult::ReductionUnavailable,
			TEXT("No supported static-mesh reduction implementation is available."),
			OutError);
	}

	FOverlappingCorners OverlappingCorners;
	const float OverlapThreshold =
		SourceMesh->GetSourceModel(0).BuildSettings.bRemoveDegenerates
			? THRESH_POINTS_ARE_SAME
			: 0.0f;
	FStaticMeshOperations::FindOverlappingCorners(
		OverlappingCorners,
		SourceMeshDescription,
		OverlapThreshold);

	FMeshReductionSettings ReductionSettings;
	ReductionSettings.PercentTriangles = TriangleFraction;
	ReductionSettings.PercentVertices = 1.0f;
	ReductionSettings.BaseLODModel = 0;
	ReductionSettings.TerminationCriterion =
		EStaticMeshReductionTerimationCriterion::Triangles;
	ReductionSettings.SilhouetteImportance = EMeshFeatureImportance::Highest;
	ReductionSettings.TextureImportance = EMeshFeatureImportance::Off;
	ReductionSettings.ShadingImportance = EMeshFeatureImportance::Lowest;
	ReductionSettings.VertexColorImportance = EMeshFeatureImportance::Off;

	FMeshDescription ReducedMeshDescription;
	FStaticMeshAttributes(ReducedMeshDescription).Register();
	float MaximumDeviation = 0.0f;
	MeshReduction->ReduceMeshDescription(
		ReducedMeshDescription,
		MaximumDeviation,
		SourceMeshDescription,
		OverlappingCorners,
		ReductionSettings);
	OutProxyTriangleCount = ReducedMeshDescription.Triangles().Num();
	if (OutProxyTriangleCount <= 0
		|| OutProxyTriangleCount > OutSourceTriangleCount
		|| ReducedMeshDescription.PolygonGroups().Num() == 0)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::ReductionFailed,
			TEXT("Mesh reduction produced no valid shadow-proxy geometry."),
			OutError);
	}
	CollapseToOneMaterialSection(ReducedMeshDescription);

	UPackage* DestinationPackage = CreatePackage(*DestinationPackageName);
	DestinationPackage->FullyLoad();
	UStaticMesh* ProxyMesh = ExistingDestination;
	if (ProxyMesh == nullptr)
	{
		ProxyMesh = NewObject<UStaticMesh>(
			DestinationPackage,
			*DestinationAssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		ProxyMesh->InitResources();
		FAssetRegistryModule::AssetCreated(ProxyMesh);
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor != nullptr
		? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
		: nullptr;
	const bool bReopenAssetEditor = AssetEditorSubsystem != nullptr
		&& AssetEditorSubsystem->FindEditorForAsset(ProxyMesh, false) != nullptr;
	if (bReopenAssetEditor)
	{
		AssetEditorSubsystem->CloseAllEditorsForAsset(ProxyMesh);
	}

	ConfigureProxyAsset(
		*ProxyMesh,
		*SourceMesh,
		MoveTemp(ReducedMeshDescription));
	FStaticMeshCompilingManager::Get().FinishCompilation({ProxyMesh});
	if (ProxyMesh->GetRenderData() == nullptr
		|| ProxyMesh->GetRenderData()->LODResources.Num() != 1)
	{
		if (bReopenAssetEditor)
		{
			AssetEditorSubsystem->OpenEditorForAsset(ProxyMesh);
		}
		return Fail(
			EUEPyShadowProxyBakeResult::BuildFailed,
			TEXT("The reduced source geometry could not be built as one render LOD."),
			OutError);
	}

	DestinationPackage->GetMetaData();
	DestinationPackage->MarkPackageDirty();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		DestinationPackageName,
		FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArguments;
	SaveArguments.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArguments.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(
		DestinationPackage,
		ProxyMesh,
		*PackageFilename,
		SaveArguments);
	if (bReopenAssetEditor)
	{
		AssetEditorSubsystem->OpenEditorForAsset(ProxyMesh);
	}
	if (!bSaved)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::SaveFailed,
			TEXT("The shadow-proxy package could not be saved."),
			OutError);
	}

	OutSavedPackageBytes = FMath::Max<int64>(
		0,
		IFileManager::Get().FileSize(*PackageFilename));
	return EUEPyShadowProxyBakeResult::Success;
}
