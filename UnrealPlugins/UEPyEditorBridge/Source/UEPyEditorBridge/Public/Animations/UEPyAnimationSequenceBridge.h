#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UEPyAnimationSequenceBridge.generated.h"

/**
 * Reflected access to animation-sequence data and narrowly reviewed edits that
 * Unreal Python does not expose precisely.
 *
 * This editor-only bridge never saves an asset. Mutations require an expected
 * fingerprint and are wrapped in one editor Undo transaction.
 */
UCLASS()
class UEPYEDITORBRIDGE_API UUEPyAnimationSequenceBridge final
	: public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Returns a deterministic JSON description of an Animation Sequence. */
	UFUNCTION(BlueprintPure, Category="UEPy|Editor|Animation Sequence")
	static FString InspectAnimationSequenceJson(const FString& AnimationPath);

	/** Validates promotion of one sampled frame to the start of the sequence. */
	UFUNCTION(BlueprintPure, Category="UEPy|Editor|Animation Sequence")
	static FString ValidatePromoteFrameToStartJson(
		const FString& AnimationPath,
		int32 FrameIndex,
		const FString& ExpectedFingerprint);

	/**
	 * Removes every sampled frame before FrameIndex. If that would leave only
	 * one key, the retained pose is duplicated so Unreal keeps a valid static
	 * one-frame sequence.
	 */
	UFUNCTION(BlueprintCallable, Category="UEPy|Editor|Animation Sequence")
	static FString ApplyPromoteFrameToStartJson(
		const FString& AnimationPath,
		int32 FrameIndex,
		const FString& ExpectedFingerprint);

	UFUNCTION(BlueprintPure, Category="UEPy|Editor|Animation Sequence")
	static int32 GetBridgeProtocolVersion();
};
