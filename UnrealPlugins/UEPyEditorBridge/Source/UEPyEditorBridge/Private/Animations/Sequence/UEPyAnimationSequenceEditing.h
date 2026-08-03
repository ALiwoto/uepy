#pragma once

#include "CoreMinimal.h"

namespace UEPy::AnimationSequence
{
inline constexpr int32 BridgeProtocolVersion = 1;

FString InspectAnimationSequenceJson(const FString& AnimationPath);

FString RunPromoteFrameToStart(
	const FString& AnimationPath,
	int32 FrameIndex,
	const FString& ExpectedFingerprint,
	bool bApply);
}
