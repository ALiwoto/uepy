#include "Animations/Sequence/UEPyAnimationSequenceEditing.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceHelpers.h"
#include "Dom/JsonObject.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "UEPyAnimationSequenceEditing"

namespace UEPy::AnimationSequence
{
namespace
{
using FJsonObjectPtr = TSharedPtr<FJsonObject>;

FString SerializeJson(const FJsonObjectPtr& Object)
{
	FString Result;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Result;
}

UAnimSequence* ResolveAnimationSequence(
	const FString& AnimationPath,
	FString& OutError)
{
	UAnimSequence* Sequence = Cast<UAnimSequence>(
		LoadObject<UObject>(nullptr, *AnimationPath));
	if (Sequence == nullptr)
	{
		OutError = TEXT("Asset was not found or is not an Animation Sequence.");
	}
	return Sequence;
}

FString BuildFingerprint(const UAnimSequence* Sequence)
{
	const IAnimationDataModel* Model = Sequence->GetDataModel();
	FString Canonical;
	Canonical += Sequence->GetPathName();
	Canonical += FString::Printf(
		TEXT("|%d|%d|%d/%d"),
		Model->GetNumberOfFrames(),
		Model->GetNumberOfKeys(),
		Model->GetFrameRate().Numerator,
		Model->GetFrameRate().Denominator);

	TArray<FName> TrackNames;
	Model->GetBoneTrackNames(TrackNames);
	for (const FName TrackName : TrackNames)
	{
		Canonical += TEXT("|");
		Canonical += TrackName.ToString();

		TArray<FTransform> Transforms;
		Model->GetBoneTrackTransforms(TrackName, Transforms);
		for (const FTransform& Transform : Transforms)
		{
			const FVector Translation = Transform.GetTranslation();
			const FQuat Rotation = Transform.GetRotation();
			const FVector Scale = Transform.GetScale3D();
			Canonical += FString::Printf(
				TEXT("|%.17g,%.17g,%.17g;%.17g,%.17g,%.17g,%.17g;%.17g,%.17g,%.17g"),
				Translation.X, Translation.Y, Translation.Z,
				Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
				Scale.X, Scale.Y, Scale.Z);
		}
	}

	for (const FTransformCurve& Curve : Model->GetTransformCurves())
	{
		Canonical += TEXT("|transform_curve:");
		Canonical += Curve.GetName().ToString();

		TArray<float> Times;
		TArray<FTransform> Values;
		Curve.GetKeys(Times, Values);
		for (int32 KeyIndex = 0; KeyIndex < Values.Num(); ++KeyIndex)
		{
			const FTransform& Transform = Values[KeyIndex];
			const FVector Translation = Transform.GetTranslation();
			const FQuat Rotation = Transform.GetRotation();
			const FVector Scale = Transform.GetScale3D();
			Canonical += FString::Printf(
				TEXT("|%.9g:%.17g,%.17g,%.17g;%.17g,%.17g,%.17g,%.17g;%.17g,%.17g,%.17g"),
				Times.IsValidIndex(KeyIndex) ? Times[KeyIndex] : -1.0f,
				Translation.X, Translation.Y, Translation.Z,
				Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
				Scale.X, Scale.Y, Scale.Z);
		}
	}

	const FTCHARToUTF8 Encoded(*Canonical);
	return LexToString(FSHA1::HashBuffer(Encoded.Get(), Encoded.Length()));
}

FJsonObjectPtr MakeSequenceJson(UAnimSequence* Sequence)
{
	const IAnimationDataModel* Model = Sequence->GetDataModel();
	const FFrameRate FrameRate = Model->GetFrameRate();

	FJsonObjectPtr Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("found"), true);
	Result->SetStringField(TEXT("path"), Sequence->GetPathName());
	Result->SetStringField(TEXT("name"), Sequence->GetName());
	Result->SetBoolField(TEXT("package_dirty"), Sequence->GetOutermost()->IsDirty());
	Result->SetNumberField(TEXT("number_of_frames"), Model->GetNumberOfFrames());
	Result->SetNumberField(TEXT("number_of_keys"), Model->GetNumberOfKeys());
	Result->SetNumberField(
		TEXT("number_of_transform_curves"),
		Model->GetNumberOfTransformCurves());
	Result->SetNumberField(TEXT("sequence_length"), Sequence->GetPlayLength());
	Result->SetStringField(
		TEXT("frame_rate"),
		FString::Printf(TEXT("%d/%d"), FrameRate.Numerator, FrameRate.Denominator));
	Result->SetStringField(TEXT("fingerprint"), BuildFingerprint(Sequence));
	return Result;
}

FJsonObjectPtr MakeErrorJson(const FString& AnimationPath, const FString& Error)
{
	FJsonObjectPtr Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("found"), false);
	Result->SetStringField(TEXT("requested_path"), AnimationPath);
	Result->SetStringField(TEXT("error"), Error);
	return Result;
}

bool ValidatePromote(
	UAnimSequence* Sequence,
	const int32 FrameIndex,
	const FString& ExpectedFingerprint,
	FString& OutError)
{
	const IAnimationDataModel* Model = Sequence->GetDataModel();
	const int32 NumberOfKeys = Model->GetNumberOfKeys();
	if (NumberOfKeys < 2)
	{
		OutError = TEXT("The sequence has fewer than two sampled keys.");
		return false;
	}
	if (FrameIndex <= 0 || FrameIndex >= NumberOfKeys)
	{
		OutError = FString::Printf(
			TEXT("frame_index must be between 1 and %d for this sequence."),
			NumberOfKeys - 1);
		return false;
	}
	if (ExpectedFingerprint.IsEmpty())
	{
		OutError = TEXT("expected_fingerprint is required.");
		return false;
	}

	const FString ActualFingerprint = BuildFingerprint(Sequence);
	if (!ActualFingerprint.Equals(ExpectedFingerprint, ESearchCase::CaseSensitive))
	{
		OutError = TEXT("The animation sequence changed after inspection; its fingerprint is stale.");
		return false;
	}
	return true;
}

bool ReplaceAllTransformCurveKeysWithFrame(
	UAnimSequence* Sequence,
	const int32 FrameIndex,
	FString& OutError)
{
	const IAnimationDataModel* Model = Sequence->GetDataModel();
	const TArray<FTransformCurve>& Curves = Model->GetTransformCurves();
	if (Curves.IsEmpty())
	{
		OutError = TEXT("The sequence has no transform curves to promote.");
		return false;
	}

	struct FReplacementCurve
	{
		FName Name;
		FTransform Transform;
	};
	TArray<FReplacementCurve> Replacements;
	Replacements.Reserve(Curves.Num());
	const float SourceTime = Model->GetFrameRate().AsSeconds(FrameIndex);
	for (const FTransformCurve& Curve : Curves)
	{
		Replacements.Add({Curve.GetName(), Curve.Evaluate(SourceTime, 1.0f)});
	}

	IAnimationDataController& Controller = Sequence->GetController();
	IAnimationDataController::FScopedBracket ScopedBracket(
		Controller,
		LOCTEXT("PromoteStaticPoseBracket", "Promoting Static Animation Pose"));
	const TArray<float> TimeKeys = {0.0f, Sequence->GetPlayLength()};
	for (const FReplacementCurve& Replacement : Replacements)
	{
		const TArray<FTransform> TransformValues = {
			Replacement.Transform,
			Replacement.Transform};
		const FAnimationCurveIdentifier CurveIdentifier(
			Replacement.Name,
			ERawCurveTrackTypes::RCT_Transform);
		if (!Controller.SetTransformCurveKeys(
			CurveIdentifier,
			TransformValues,
			TimeKeys))
		{
			OutError = FString::Printf(
				TEXT("Failed to replace keys for transform curve '%s'."),
				*Replacement.Name.ToString());
			return false;
		}
	}
	return true;
}
}

FString InspectAnimationSequenceJson(const FString& AnimationPath)
{
	FString Error;
	UAnimSequence* Sequence = ResolveAnimationSequence(AnimationPath, Error);
	if (Sequence == nullptr)
	{
		return SerializeJson(MakeErrorJson(AnimationPath, Error));
	}
	return SerializeJson(MakeSequenceJson(Sequence));
}

FString RunPromoteFrameToStart(
	const FString& AnimationPath,
	const int32 FrameIndex,
	const FString& ExpectedFingerprint,
	const bool bApply)
{
	FString Error;
	UAnimSequence* Sequence = ResolveAnimationSequence(AnimationPath, Error);
	if (Sequence == nullptr)
	{
		FJsonObjectPtr Result = MakeErrorJson(AnimationPath, Error);
		Result->SetBoolField(TEXT("valid"), false);
		Result->SetBoolField(TEXT("applied"), false);
		return SerializeJson(Result);
	}

	FJsonObjectPtr Result = MakeSequenceJson(Sequence);
	Result->SetNumberField(TEXT("frame_index"), FrameIndex);
	Result->SetStringField(TEXT("expected_fingerprint"), ExpectedFingerprint);
	if (!ValidatePromote(Sequence, FrameIndex, ExpectedFingerprint, Error))
	{
		Result->SetBoolField(TEXT("valid"), false);
		Result->SetBoolField(TEXT("applied"), false);
		Result->SetStringField(TEXT("error"), Error);
		return SerializeJson(Result);
	}

	Result->SetBoolField(TEXT("valid"), true);
	Result->SetBoolField(TEXT("applied"), false);
	Result->SetNumberField(TEXT("frames_removed_from_start"), FrameIndex);
	if (!bApply)
	{
		return SerializeJson(Result);
	}

	const int32 NumberOfKeys = Sequence->GetDataModel()->GetNumberOfKeys();
	const int32 RemainingKeys = NumberOfKeys - FrameIndex;
	const FScopedTransaction Transaction(
		LOCTEXT("PromoteAnimationFrame", "Promote Animation Frame to Start"));
	Sequence->Modify();

	// Editor-authored skeletal adjustments are transform curves rather than
	// ordinary bone tracks. Unreal represents a static one-frame sequence with
	// two samples, so replace both curve samples directly instead of resizing.
	if (RemainingKeys == 1)
	{
		if (!ReplaceAllTransformCurveKeysWithFrame(Sequence, FrameIndex, Error))
		{
			Result->SetBoolField(TEXT("valid"), false);
			Result->SetBoolField(TEXT("applied"), false);
			Result->SetStringField(TEXT("error"), Error);
			return SerializeJson(Result);
		}
	}
	else
	{
		UE::Anim::AnimationData::RemoveKeys(Sequence, 0, FrameIndex);
	}

	Result = MakeSequenceJson(Sequence);
	Result->SetBoolField(TEXT("valid"), true);
	Result->SetBoolField(TEXT("applied"), true);
	Result->SetNumberField(TEXT("promoted_source_frame"), FrameIndex);
	Result->SetNumberField(TEXT("frames_removed_from_start"), FrameIndex);
	return SerializeJson(Result);
}
}

#undef LOCTEXT_NAMESPACE
