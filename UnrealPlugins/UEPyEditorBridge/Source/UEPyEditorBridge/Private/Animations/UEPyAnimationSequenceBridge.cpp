#include "Animations/UEPyAnimationSequenceBridge.h"

#include "Animations/Sequence/UEPyAnimationSequenceEditing.h"

FString UUEPyAnimationSequenceBridge::InspectAnimationSequenceJson(
	const FString& AnimationPath)
{
	return UEPy::AnimationSequence::InspectAnimationSequenceJson(AnimationPath);
}

FString UUEPyAnimationSequenceBridge::ValidatePromoteFrameToStartJson(
	const FString& AnimationPath,
	const int32 FrameIndex,
	const FString& ExpectedFingerprint)
{
	return UEPy::AnimationSequence::RunPromoteFrameToStart(
		AnimationPath, FrameIndex, ExpectedFingerprint, false);
}

FString UUEPyAnimationSequenceBridge::ApplyPromoteFrameToStartJson(
	const FString& AnimationPath,
	const int32 FrameIndex,
	const FString& ExpectedFingerprint)
{
	return UEPy::AnimationSequence::RunPromoteFrameToStart(
		AnimationPath, FrameIndex, ExpectedFingerprint, true);
}

int32 UUEPyAnimationSequenceBridge::GetBridgeProtocolVersion()
{
	return UEPy::AnimationSequence::BridgeProtocolVersion;
}
