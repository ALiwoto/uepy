#include "Assets/UEPyAudioAssetBridge.h"

#include "Audio.h"
#include "Compression/CompressedBuffer.h"
#include "HAL/FileManager.h"
#include "Memory/MemoryView.h"
#include "Memory/SharedBuffer.h"
#include "Misc/ByteSwap.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "uewav.h"

namespace
{
	constexpr uint8 CompressedBufferMagic[] = {0xB7, 0x75, 0x63, 0x62};
	constexpr int64 CompressedBufferHeaderSize = 64;
	constexpr int64 TotalCompressedSizeOffset = 24;

	struct FAudioExtractionEntry
	{
		FString SourceFile;
		FString DestinationFile;
	};

	bool HasBytesAt(
		const FSharedBuffer& Buffer,
		const uint64 Offset,
		const ANSICHAR* Expected,
		const uint64 ExpectedSize)
	{
		return Offset <= Buffer.GetSize()
			&& ExpectedSize <= Buffer.GetSize() - Offset
			&& FMemory::Memcmp(
				static_cast<const uint8*>(Buffer.GetData()) + Offset,
				Expected,
				ExpectedSize) == 0;
	}

	bool IsWavePayload(const FSharedBuffer& Buffer)
	{
		const bool bRiffContainer = HasBytesAt(Buffer, 0, "RIFF", 4)
			|| HasBytesAt(Buffer, 0, "RF64", 4);
		return bRiffContainer && HasBytesAt(Buffer, 8, "WAVE", 4);
	}

	bool NormalizeWavePayload(FSharedBuffer& WavePayload, FString& OutError)
	{
		if (WavePayload.GetSize() > static_cast<uint64>(MAX_int32))
		{
			OutError = TEXT("WAVE payload is too large for Unreal's WAVE parser.");
			return false;
		}

		FUniqueBuffer MutablePayload = WavePayload.MoveToUnique();
		FWaveModInfo WaveInfo;
		FString ParseError;
		if (!WaveInfo.ReadWaveInfo(
			static_cast<const uint8*>(MutablePayload.GetData()),
			static_cast<int32>(MutablePayload.GetSize()),
			&ParseError))
		{
			OutError = ParseError.IsEmpty()
				? TEXT("Extracted payload has an invalid WAVE structure.")
				: MoveTemp(ParseError);
			return false;
		}

		if (*WaveInfo.pFormatTag == FWaveModInfo::WAVE_INFO_FORMAT_OODLE_WAVE)
		{
#if WITH_OOWAV
			if (*WaveInfo.pBitsPerSample != 16 || *WaveInfo.pChannels == 0)
			{
				OutError = TEXT("UEWavComp payload is not supported 16-bit PCM audio.");
				return false;
			}

			*WaveInfo.pFormatTag = FWaveModInfo::WAVE_INFO_FORMAT_PCM;
			const int64 SampleCount = static_cast<int64>(WaveInfo.GetNumSamples());
			const int64 ChannelCount = static_cast<int64>(*WaveInfo.pChannels);
			if (SampleCount <= 0)
			{
				OutError = TEXT("UEWavComp payload contains no decodable samples.");
				return false;
			}
			FUniqueBuffer Scratch = FUniqueBuffer::Alloc(
				static_cast<uint64>(SampleCount) * sizeof(int16));
			uewav_decode16(
				reinterpret_cast<int16*>(const_cast<uint8*>(WaveInfo.SampleDataStart)),
				static_cast<int16*>(Scratch.GetData()),
				SampleCount,
				ChannelCount);
#else
			OutError = TEXT("This platform does not provide the UEWavComp decoder.");
			return false;
#endif
		}

		WavePayload = MutablePayload.MoveToShared();
		return true;
	}

	bool TryExtractWavePayload(
		const FString& PackageFilename,
		FSharedBuffer& OutWavePayload,
		FString& OutError)
	{
		OutWavePayload.Reset();
		OutError.Reset();

		TArray64<uint8> PackageData;
		if (!FFileHelper::LoadFileToArray(PackageData, *PackageFilename))
		{
			OutError = FString::Printf(TEXT("Could not read package: %s"), *PackageFilename);
			return false;
		}

		bool bCompressedBufferDecompressionFailed = false;
		const int64 LastHeaderOffset = PackageData.Num() - CompressedBufferHeaderSize;
		for (int64 Offset = 0; Offset <= LastHeaderOffset; ++Offset)
		{
			if (FMemory::Memcmp(
				PackageData.GetData() + Offset,
				CompressedBufferMagic,
				UE_ARRAY_COUNT(CompressedBufferMagic)) != 0)
			{
				continue;
			}

			uint64 NetworkCompressedSize = 0;
			FMemory::Memcpy(
				&NetworkCompressedSize,
				PackageData.GetData() + Offset + TotalCompressedSizeOffset,
				sizeof(NetworkCompressedSize));
			const uint64 CompressedSize = NETWORK_ORDER64(NetworkCompressedSize);
			const uint64 RemainingSize = static_cast<uint64>(PackageData.Num() - Offset);
			if (CompressedSize < static_cast<uint64>(CompressedBufferHeaderSize)
				|| CompressedSize > RemainingSize)
			{
				continue;
			}

			FSharedBuffer CompressedData = FSharedBuffer::Clone(
				PackageData.GetData() + Offset,
				CompressedSize);
			FCompressedBuffer CompressedBuffer = FCompressedBuffer::FromCompressed(
				MoveTemp(CompressedData));
			if (!CompressedBuffer)
			{
				continue;
			}

			FSharedBuffer RawPayload = CompressedBuffer.Decompress();
			if (!RawPayload)
			{
				bCompressedBufferDecompressionFailed = true;
				continue;
			}
			if (IsWavePayload(RawPayload))
			{
				if (!NormalizeWavePayload(RawPayload, OutError))
				{
					OutError = FString::Printf(
						TEXT("Could not normalize embedded WAVE payload in %s: %s"),
						*PackageFilename,
						*OutError);
					return false;
				}
				OutWavePayload = MoveTemp(RawPayload);
				return true;
			}

			Offset += static_cast<int64>(CompressedSize) - 1;
		}

		if (bCompressedBufferDecompressionFailed)
		{
			OutError = FString::Printf(
				TEXT("Could not decompress one or more embedded buffers: %s"),
				*PackageFilename);
		}
		return false;
	}

	bool WriteWaveAtomically(
		const FString& DestinationFilename,
		const FSharedBuffer& WavePayload,
		FString& OutError)
	{
		IFileManager& FileManager = IFileManager::Get();
		const FString DestinationDirectory = FPaths::GetPath(DestinationFilename);
		if (!FileManager.MakeDirectory(*DestinationDirectory, true))
		{
			OutError = FString::Printf(
				TEXT("Could not create output directory: %s"),
				*DestinationDirectory);
			return false;
		}

		const FString TemporaryFilename = DestinationFilename + TEXT(".uepy.tmp");
		FileManager.Delete(*TemporaryFilename, false, true, true);
		TUniquePtr<FArchive> Writer(FileManager.CreateFileWriter(*TemporaryFilename));
		if (!Writer)
		{
			OutError = FString::Printf(
				TEXT("Could not create temporary output file: %s"),
				*TemporaryFilename);
			return false;
		}

		Writer->Serialize(
			const_cast<void*>(WavePayload.GetData()),
			static_cast<int64>(WavePayload.GetSize()));
		const bool bWriteSucceeded = !Writer->IsError();
		Writer.Reset();
		if (!bWriteSucceeded)
		{
			FileManager.Delete(*TemporaryFilename, false, true, true);
			OutError = FString::Printf(
				TEXT("Could not write WAVE data: %s"),
				*TemporaryFilename);
			return false;
		}

		if (!FileManager.Move(
			*DestinationFilename,
			*TemporaryFilename,
			true,
			true,
			false,
			true))
		{
			FileManager.Delete(*TemporaryFilename, false, true, true);
			OutError = FString::Printf(
				TEXT("Could not move temporary WAVE to destination: %s"),
				*DestinationFilename);
			return false;
		}
		return true;
	}

	FString NormalizeFullPath(const FString& Path)
	{
		FString FullPath = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(FullPath);
		return FullPath;
	}
}

EUEPyEmbeddedAudioExtractionResult UUEPyAudioAssetBridge::ExtractEmbeddedWaveAudio(
	const FString& SourceFilesystemPath,
	const FString& DestinationDirectory,
	const bool bForce,
	int32& OutScannedPackageCount,
	int32& OutExtractedWaveCount,
	int32& OutSkippedPackageCount,
	int32& OutFailedPackageCount,
	TArray<FString>& OutWrittenFiles,
	TArray<FString>& OutErrors)
{
	OutScannedPackageCount = 0;
	OutExtractedWaveCount = 0;
	OutSkippedPackageCount = 0;
	OutFailedPackageCount = 0;
	OutWrittenFiles.Reset();
	OutErrors.Reset();

	IFileManager& FileManager = IFileManager::Get();
	const FString SourcePath = NormalizeFullPath(SourceFilesystemPath);
	FString DestinationPath = NormalizeFullPath(DestinationDirectory);
	FPaths::NormalizeDirectoryName(DestinationPath);
	if (SourcePath.IsEmpty()
		|| (!FileManager.FileExists(*SourcePath) && !FileManager.DirectoryExists(*SourcePath)))
	{
		OutErrors.Add(FString::Printf(TEXT("Source path does not exist: %s"), *SourcePath));
		return EUEPyEmbeddedAudioExtractionResult::InvalidSourcePath;
	}
	if (DestinationPath.IsEmpty() || FileManager.FileExists(*DestinationPath))
	{
		OutErrors.Add(FString::Printf(
			TEXT("Destination must be a directory path: %s"),
			*DestinationPath));
		return EUEPyEmbeddedAudioExtractionResult::InvalidDestinationPath;
	}

	TArray<FString> SourceFiles;
	FString SourceRoot;
	if (FileManager.FileExists(*SourcePath))
	{
		if (!FPaths::GetExtension(SourcePath).Equals(TEXT("uasset"), ESearchCase::IgnoreCase))
		{
			OutErrors.Add(TEXT("Source file must have a .uasset extension."));
			return EUEPyEmbeddedAudioExtractionResult::InvalidSourcePath;
		}
		SourceRoot = FPaths::GetPath(SourcePath);
		SourceFiles.Add(SourcePath);
	}
	else
	{
		SourceRoot = SourcePath;
		FileManager.FindFilesRecursive(
			SourceFiles,
			*SourceRoot,
			TEXT("*.uasset"),
			true,
			false);
		SourceFiles.Sort();
	}

	TArray<FAudioExtractionEntry> Entries;
	Entries.Reserve(SourceFiles.Num());
	for (const FString& SourceFile : SourceFiles)
	{
		FString RelativeFilename = SourceFile;
		FString RelativeRoot = SourceRoot;
		FPaths::NormalizeDirectoryName(RelativeRoot);
		RelativeRoot += TEXT("/");
		if (!FPaths::MakePathRelativeTo(RelativeFilename, *RelativeRoot))
		{
			RelativeFilename = FPaths::GetCleanFilename(SourceFile);
		}
		RelativeFilename = FPaths::ChangeExtension(RelativeFilename, TEXT("wav"));
		Entries.Add({SourceFile, DestinationPath / RelativeFilename});
	}

	if (!bForce)
	{
		for (const FAudioExtractionEntry& Entry : Entries)
		{
			if (FileManager.FileExists(*Entry.DestinationFile))
			{
				OutErrors.Add(FString::Printf(
					TEXT("Destination exists; pass --force to replace it: %s"),
					*Entry.DestinationFile));
			}
		}
		if (!OutErrors.IsEmpty())
		{
			return EUEPyEmbeddedAudioExtractionResult::DestinationExists;
		}
	}

	for (const FAudioExtractionEntry& Entry : Entries)
	{
		++OutScannedPackageCount;
		FSharedBuffer WavePayload;
		FString ExtractionError;
		if (!TryExtractWavePayload(Entry.SourceFile, WavePayload, ExtractionError))
		{
			if (ExtractionError.IsEmpty())
			{
				++OutSkippedPackageCount;
			}
			else
			{
				++OutFailedPackageCount;
				OutErrors.Add(MoveTemp(ExtractionError));
			}
			continue;
		}

		FString WriteError;
		if (!WriteWaveAtomically(Entry.DestinationFile, WavePayload, WriteError))
		{
			++OutFailedPackageCount;
			OutErrors.Add(MoveTemp(WriteError));
			continue;
		}

		++OutExtractedWaveCount;
		OutWrittenFiles.Add(Entry.DestinationFile);
	}

	if (OutFailedPackageCount > 0)
	{
		return EUEPyEmbeddedAudioExtractionResult::ExtractionFailed;
	}
	if (OutExtractedWaveCount == 0)
	{
		return EUEPyEmbeddedAudioExtractionResult::NoWavePayload;
	}
	return EUEPyEmbeddedAudioExtractionResult::Success;
}
