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
#include "ObjectTools.h"
#include "OverlappingCorners.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

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

TSet<FName> CaptureDirtyPackageNames()
{
	TSet<FName> DirtyPackageNames;
	for (TObjectIterator<UPackage> PackageIt; PackageIt; ++PackageIt)
	{
		if (PackageIt->IsDirty())
		{
			DirtyPackageNames.Add(PackageIt->GetFName());
		}
	}
	return DirtyPackageNames;
}

void RestorePreviouslyCleanPackages(
	const TSet<FName>& DirtyPackageNamesBeforeReplacement)
{
	for (TObjectIterator<UPackage> PackageIt; PackageIt; ++PackageIt)
	{
		if (PackageIt->IsDirty()
			&& !DirtyPackageNamesBeforeReplacement.Contains(
				PackageIt->GetFName()))
		{
			PackageIt->SetDirtyFlag(false);
		}
	}
}

using FObjectMetadataSnapshot =
	TMap<TWeakObjectPtr<UObject>, TMap<FName, FString>>;

FObjectMetadataSnapshot CaptureObjectMetadata(UObject& RootObject)
{
	TArray<UObject*> Objects = {&RootObject};
	GetObjectsWithOuter(&RootObject, Objects, true);

	FObjectMetadataSnapshot Snapshot;
	for (UObject* Object : Objects)
	{
		if (const TMap<FName, FString>* Values =
			UMetaData::GetMapForObject(Object))
		{
			Snapshot.Add(Object, *Values);
		}
	}
	return Snapshot;
}

void RestoreObjectMetadata(
	UMetaData& Metadata,
	const FObjectMetadataSnapshot& Snapshot)
{
	for (const TPair<TWeakObjectPtr<UObject>, TMap<FName, FString>>& Pair
		: Snapshot)
	{
		if (UObject* Object = Pair.Key.Get())
		{
			Metadata.SetObjectValues(Object, Pair.Value);
		}
	}
}

bool ExpandProxyBoundsToContainSource(
	UStaticMesh& ProxyMesh,
	const UStaticMesh& SourceMesh)
{
	const FBox ProxyBounds = ProxyMesh.GetBoundingBox();
	const FBox SourceBounds = SourceMesh.GetBoundingBox();
	if (!ProxyBounds.IsValid || !SourceBounds.IsValid)
	{
		return false;
	}

	const FVector PositiveExtension(
		FMath::Max(0.0, SourceBounds.Max.X - ProxyBounds.Max.X),
		FMath::Max(0.0, SourceBounds.Max.Y - ProxyBounds.Max.Y),
		FMath::Max(0.0, SourceBounds.Max.Z - ProxyBounds.Max.Z));
	const FVector NegativeExtension(
		FMath::Max(0.0, ProxyBounds.Min.X - SourceBounds.Min.X),
		FMath::Max(0.0, ProxyBounds.Min.Y - SourceBounds.Min.Y),
		FMath::Max(0.0, ProxyBounds.Min.Z - SourceBounds.Min.Z));
	ProxyMesh.SetPositiveBoundsExtension(PositiveExtension);
	ProxyMesh.SetNegativeBoundsExtension(NegativeExtension);
	ProxyMesh.CalculateExtendedBounds();

	const FBox ExpandedProxyBounds = ProxyMesh.GetBoundingBox();
	constexpr double BoundsTolerance = 0.01;
	return ExpandedProxyBounds.Min.X <= SourceBounds.Min.X + BoundsTolerance
		&& ExpandedProxyBounds.Min.Y <= SourceBounds.Min.Y + BoundsTolerance
		&& ExpandedProxyBounds.Min.Z <= SourceBounds.Min.Z + BoundsTolerance
		&& ExpandedProxyBounds.Max.X >= SourceBounds.Max.X - BoundsTolerance
		&& ExpandedProxyBounds.Max.Y >= SourceBounds.Max.Y - BoundsTolerance
		&& ExpandedProxyBounds.Max.Z >= SourceBounds.Max.Z - BoundsTolerance;
}

FPolygonGroupID CollapsePolygonGroupsToOne(FMeshDescription& MeshDescription)
{
	const FPolygonGroupID FirstPolygonGroup =
		MeshDescription.PolygonGroups().GetFirstValidID();
	if (FirstPolygonGroup == INDEX_NONE)
	{
		return INDEX_NONE;
	}

	TMap<FPolygonGroupID, FPolygonGroupID> PolygonGroupRemap;
	for (const FPolygonGroupID PolygonGroupId
		: MeshDescription.PolygonGroups().GetElementIDs())
	{
		PolygonGroupRemap.Add(PolygonGroupId, FirstPolygonGroup);
	}
	MeshDescription.RemapPolygonGroups(PolygonGroupRemap);
	return FirstPolygonGroup;
}

void CollapseToOneMaterialSection(FMeshDescription& MeshDescription)
{
	const FPolygonGroupID FirstPolygonGroup =
		CollapsePolygonGroupsToOne(MeshDescription);
	check(FirstPolygonGroup != INDEX_NONE);

	FStaticMeshAttributes Attributes(MeshDescription);
	TPolygonGroupAttributesRef<FName> MaterialSlotNames =
		Attributes.GetPolygonGroupMaterialSlotNames();
	MaterialSlotNames[FirstPolygonGroup] = ShadowProxyMaterialSlotName;

	TVertexInstanceAttributesRef<FVector2f> VertexInstanceUvs =
		Attributes.GetVertexInstanceUVs();
	VertexInstanceUvs.SetNumChannels(1);

	TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors =
		Attributes.GetVertexInstanceColors();
	for (const FVertexInstanceID VertexInstanceId
		: MeshDescription.VertexInstances().GetElementIDs())
	{
		VertexInstanceColors[VertexInstanceId] = FVector4f(1.0f);
	}
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
	ProxySourceModel.BuildSettings.bBuildReversedIndexBuffer = false;
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
	ProxyMesh.SetPositiveBoundsExtension(FVector::ZeroVector);
	ProxyMesh.SetNegativeBoundsExtension(FVector::ZeroVector);
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
	if (!IsInGameThread())
	{
		return Fail(
			EUEPyShadowProxyBakeResult::WrongThread,
			TEXT("Shadow-proxy baking must run on Unreal's game thread."),
			OutError);
	}

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
	if (CollapsePolygonGroupsToOne(SourceMeshDescription) == INDEX_NONE)
	{
		return Fail(
			EUEPyShadowProxyBakeResult::SourceMeshUnavailable,
			TEXT("Source mesh LOD0 contains no polygon groups."),
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
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor != nullptr
		? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
		: nullptr;
	const bool bReopenAssetEditor = AssetEditorSubsystem != nullptr
		&& ExistingDestination != nullptr
		&& AssetEditorSubsystem->FindEditorForAsset(
			ExistingDestination,
			false) != nullptr;

	const FName TemporaryProxyName = MakeUniqueObjectName(
		GetTransientPackage(),
		UStaticMesh::StaticClass(),
		FName(*FString::Printf(
			TEXT("%s_UEPyTemp"),
			*DestinationAssetName)));
	UStaticMesh* ProxyMesh = NewObject<UStaticMesh>(
		GetTransientPackage(),
		TemporaryProxyName,
		RF_Transactional);
	ProxyMesh->InitResources();

	ConfigureProxyAsset(
		*ProxyMesh,
		*SourceMesh,
		MoveTemp(ReducedMeshDescription));
	FStaticMeshCompilingManager::Get().FinishCompilation({ProxyMesh});
	if (ProxyMesh->GetRenderData() == nullptr
		|| ProxyMesh->GetRenderData()->LODResources.Num() != 1)
	{
		ProxyMesh->MarkAsGarbage();
		return Fail(
			EUEPyShadowProxyBakeResult::BuildFailed,
			TEXT("The reduced source geometry could not be built as one render LOD."),
			OutError);
	}
	if (!ExpandProxyBoundsToContainSource(*ProxyMesh, *SourceMesh))
	{
		ProxyMesh->MarkAsGarbage();
		return Fail(
			EUEPyShadowProxyBakeResult::BuildFailed,
			TEXT("The shadow proxy's bounds could not be expanded to contain the source bounds."),
			OutError);
	}

	if (bReopenAssetEditor)
	{
		AssetEditorSubsystem->CloseAllEditorsForAsset(ExistingDestination);
	}

	constexpr ERenameFlags SwapRenameFlags =
		REN_DontCreateRedirectors | REN_NonTransactional;
	const bool bDestinationPackageWasDirty = DestinationPackage->IsDirty();
	UMetaData* DestinationMetadata = DestinationPackage->GetMetaData();
	const FObjectMetadataSnapshot ExistingDestinationMetadata =
		ExistingDestination != nullptr
			? CaptureObjectMetadata(*ExistingDestination)
			: FObjectMetadataSnapshot();
	const bool bExistingWasPublic = ExistingDestination != nullptr
		&& ExistingDestination->HasAnyFlags(RF_Public);
	const bool bExistingWasStandalone = ExistingDestination != nullptr
		&& ExistingDestination->HasAnyFlags(RF_Standalone);
	bool bExistingMovedToTransient = false;

	if (ExistingDestination != nullptr)
	{
		FAssetRegistryModule::AssetDeleted(ExistingDestination);
		ExistingDestination->ClearFlags(RF_Public | RF_Standalone);
		const FName PreviousAssetTemporaryName = MakeUniqueObjectName(
			GetTransientPackage(),
			UStaticMesh::StaticClass(),
			FName(*FString::Printf(
				TEXT("%s_UEPyPrevious"),
				*DestinationAssetName)));
		bExistingMovedToTransient = ExistingDestination->Rename(
			*PreviousAssetTemporaryName.ToString(),
			GetTransientPackage(),
			SwapRenameFlags);
		if (!bExistingMovedToTransient)
		{
			if (bExistingWasPublic)
			{
				ExistingDestination->SetFlags(RF_Public);
			}
			if (bExistingWasStandalone)
			{
				ExistingDestination->SetFlags(RF_Standalone);
			}
			FAssetRegistryModule::AssetCreated(ExistingDestination);
			DestinationPackage->SetDirtyFlag(bDestinationPackageWasDirty);
			ProxyMesh->MarkAsGarbage();
			if (bReopenAssetEditor)
			{
				AssetEditorSubsystem->OpenEditorForAsset(ExistingDestination);
			}
			return Fail(
				EUEPyShadowProxyBakeResult::DestinationReplaceFailed,
				TEXT("The existing destination could not be moved aside for replacement."),
				OutError);
		}
	}

	const bool bProxyMovedToDestination = ProxyMesh->Rename(
		*DestinationAssetName,
		DestinationPackage,
		SwapRenameFlags);
	if (!bProxyMovedToDestination)
	{
		bool bExistingRestored = ExistingDestination == nullptr;
		if (ExistingDestination != nullptr && bExistingMovedToTransient)
		{
			bExistingRestored = ExistingDestination->Rename(
				*DestinationAssetName,
				DestinationPackage,
				SwapRenameFlags);
			if (bExistingRestored)
			{
				if (bExistingWasPublic)
				{
					ExistingDestination->SetFlags(RF_Public);
				}
				if (bExistingWasStandalone)
				{
					ExistingDestination->SetFlags(RF_Standalone);
				}
				FAssetRegistryModule::AssetCreated(ExistingDestination);
			}
		}
		DestinationPackage->SetDirtyFlag(bDestinationPackageWasDirty);
		ProxyMesh->MarkAsGarbage();
		if (bReopenAssetEditor && bExistingRestored)
		{
			AssetEditorSubsystem->OpenEditorForAsset(ExistingDestination);
		}
		return Fail(
			EUEPyShadowProxyBakeResult::DestinationReplaceFailed,
			bExistingRestored
				? TEXT("The fresh shadow proxy could not be moved to the destination.")
				: TEXT("The fresh shadow proxy could not be moved to the destination, and the previous asset could not be restored."),
			OutError);
	}

	ProxyMesh->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(ProxyMesh);

	DestinationMetadata->RemoveMetaDataOutsidePackage();
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
	if (!bSaved)
	{
		FAssetRegistryModule::AssetDeleted(ProxyMesh);
		ProxyMesh->ClearFlags(RF_Public | RF_Standalone);
		const FName FailedProxyTemporaryName = MakeUniqueObjectName(
			GetTransientPackage(),
			UStaticMesh::StaticClass(),
			FName(*FString::Printf(
				TEXT("%s_UEPyFailed"),
				*DestinationAssetName)));
		const bool bFailedProxyMovedToTransient = ProxyMesh->Rename(
			*FailedProxyTemporaryName.ToString(),
			GetTransientPackage(),
			SwapRenameFlags);
		bool bExistingRestored = false;
		if (ExistingDestination != nullptr
			&& bExistingMovedToTransient
			&& bFailedProxyMovedToTransient)
		{
			bExistingRestored = ExistingDestination->Rename(
				*DestinationAssetName,
				DestinationPackage,
				SwapRenameFlags);
			if (bExistingRestored)
			{
				if (bExistingWasPublic)
				{
					ExistingDestination->SetFlags(RF_Public);
				}
				if (bExistingWasStandalone)
				{
					ExistingDestination->SetFlags(RF_Standalone);
				}
				FAssetRegistryModule::AssetCreated(ExistingDestination);
				RestoreObjectMetadata(
					*DestinationMetadata,
					ExistingDestinationMetadata);
			}
		}
		const bool bRollbackSucceeded = bFailedProxyMovedToTransient
			&& (ExistingDestination == nullptr || bExistingRestored);
		DestinationPackage->SetDirtyFlag(bDestinationPackageWasDirty);
		if (bFailedProxyMovedToTransient)
		{
			ProxyMesh->MarkAsGarbage();
		}
		if (bReopenAssetEditor && bExistingRestored)
		{
			AssetEditorSubsystem->OpenEditorForAsset(ExistingDestination);
		}
		return Fail(
			EUEPyShadowProxyBakeResult::SaveFailed,
			bRollbackSucceeded
				? (ExistingDestination != nullptr
					? TEXT("The shadow-proxy package could not be saved; the previous destination was restored.")
					: TEXT("The shadow-proxy package could not be saved; the unsaved destination was discarded."))
				: TEXT("The shadow-proxy package could not be saved, and its in-memory state could not be rolled back."),
			OutError);
	}

	if (ExistingDestination != nullptr)
	{
		const TSet<FName> DirtyPackageNamesBeforeReplacement =
			CaptureDirtyPackageNames();
		TArray<UObject*> ObjectsToReplace = {ExistingDestination};
		ObjectTools::ForceReplaceReferences(ProxyMesh, ObjectsToReplace);
		RestorePreviouslyCleanPackages(
			DirtyPackageNamesBeforeReplacement);
		ExistingDestination->MarkAsGarbage();
	}
	if (bReopenAssetEditor)
	{
		AssetEditorSubsystem->OpenEditorForAsset(ProxyMesh);
	}

	OutSavedPackageBytes = FMath::Max<int64>(
		0,
		IFileManager::Get().FileSize(*PackageFilename));
	return EUEPyShadowProxyBakeResult::Success;
}
