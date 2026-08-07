#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UEPyAudioAssetBridge.generated.h"

/** Result of extracting embedded WAVE source data from Unreal package files. */
UENUM(BlueprintType)
enum class EUEPyEmbeddedAudioExtractionResult : uint8
{
	Success = 0,
	InvalidSourcePath = 1,
	InvalidDestinationPath = 2,
	DestinationExists = 3,
	NoWavePayload = 4,
	ExtractionFailed = 5,
};

/** Generic editor-only audio package operations exposed to Unreal Python. */
UCLASS()
class UEPYEDITORBRIDGE_API UUEPyAudioAssetBridge final
	: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Extracts RIFF/WAVE source payloads stored in Unreal compressed buffers.
	 * SourceFilesystemPath may name one .uasset or a directory, which is
	 * searched recursively. Directory structure is preserved below
	 * DestinationDirectory. Existing output files require bForce.
	 */
	UFUNCTION(BlueprintCallable, Category="uepy|Assets|Audio")
	static EUEPyEmbeddedAudioExtractionResult ExtractEmbeddedWaveAudio(
		const FString& SourceFilesystemPath,
		const FString& DestinationDirectory,
		bool bForce,
		int32& OutScannedPackageCount,
		int32& OutExtractedWaveCount,
		int32& OutSkippedPackageCount,
		int32& OutFailedPackageCount,
		TArray<FString>& OutWrittenFiles,
		TArray<FString>& OutErrors);
};
