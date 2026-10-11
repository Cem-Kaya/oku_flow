#ifdef _WIN32

#include "okuflow/common/media_writer.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

std::filesystem::path TemporaryMp4Path()
{
    wchar_t directory[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(directory), directory);
    wchar_t file[MAX_PATH]{};
    GetTempFileNameW(directory, L"oza", 0, file);
    std::filesystem::path path(file);
    std::filesystem::remove(path);
    path.replace_extension(L".mp4");
    return path;
}

bool ValidateDecodedAudio(const std::filesystem::path& path,
                          std::int64_t expectedGap100ns = 0)
{
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(
            path.c_str(), nullptr, reader.GetAddressOf()))) {
        return false;
    }
    for (DWORD stream = 0; stream < 8; ++stream) {
        ComPtr<IMFMediaType> type;
        const HRESULT result = reader->GetNativeMediaType(
            stream, 0, type.GetAddressOf());
        if (result == MF_E_INVALIDSTREAMNUMBER) {
            break;
        }
        GUID major = GUID_NULL;
        if (SUCCEEDED(result) && type &&
            SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
            major == MFMediaType_Audio) {
            reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
            if (FAILED(reader->SetStreamSelection(stream, TRUE))) return false;
            ComPtr<IMFMediaType> pcm;
            if (FAILED(MFCreateMediaType(pcm.GetAddressOf())) ||
                FAILED(pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
                FAILED(pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)) ||
                FAILED(reader->SetCurrentMediaType(stream, nullptr, pcm.Get()))) return false;
            std::uint64_t decodedBytes = 0;
            LONGLONG lastTimestamp = -1;
            bool ended = false;
            for (unsigned read = 0; read < 1024 && !ended; ++read) {
                DWORD flags = 0;
                LONGLONG timestamp = 0;
                ComPtr<IMFSample> sample;
                if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags,
                                               &timestamp, sample.GetAddressOf()))) return false;
                ended = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
                if (!sample) continue;
                if (timestamp < lastTimestamp) return false;
                lastTimestamp = timestamp;
                DWORD bytes = 0;
                if (FAILED(sample->GetTotalLength(&bytes))) return false;
                decodedBytes += bytes;
            }
            // AAC's fixed packets may pad a boundary. Decode the entire track,
            // rather than accepting a stream header with missing late speech.
            const auto decodedFrames = decodedBytes / 2;
            std::cout << "AAC decoded PCM frames=" << decodedFrames
                      << " last timestamp=" << lastTimestamp << '\n';
            return ended && decodedFrames >= 144'000 - 4096 &&
                decodedFrames <= 144'000 + 4096 &&
                lastTimestamp >= 28'000'000 + expectedGap100ns;
        }
    }
    return false;
}

} // namespace

// Records ~3 s of synthetic video+audio with the given codec and validates
// playability, sample accounting, the audio stream, and the fMP4 fragment
// cadence that crash survivability depends on. Returns false on failure.
// Sets *skipped (and returns true) when the codec's encoder is unavailable
// on this machine — AV1 encoding exists only as a hardware MFT, so its leg
// must skip cleanly where H.264's software fallback always runs.
bool RunCodecPass(okuflow::VideoRecorder::Codec codec, bool* skipped,
                  std::int64_t gap100ns = 0)
{
    *skipped = false;
    const char* codecName = okuflow::VideoRecorder::CodecName(codec);
    const std::filesystem::path output = TemporaryMp4Path();
    okuflow::VideoRecorder recorder;
    const okuflow::VideoRecorder::AudioFormat audioFormat{};
    bool passed = recorder.Start(
        output.wstring(), 320, 180, 30, 1, codec, &audioFormat);
    if (!passed) {
        std::cerr << codecName << " encoder unavailable on this machine: "
                  << recorder.LastError() << '\n';
        *skipped = true;
        std::error_code cleanupError;
        std::filesystem::remove(output, cleanupError);
        return true;
    }

    const std::uint8_t oneByte = 0;
    const std::size_t oversized = static_cast<std::size_t>(
        std::numeric_limits<DWORD>::max()) + 1;
    passed = !recorder.CanAcceptAudioFrame(3, 0, 333'333) &&
        !recorder.CanAcceptAudioFrame(3200, 0, 1) &&
        !recorder.CanAcceptAudioFrame(3200, -1, 333'333) &&
        !recorder.CanAcceptAudioFrame(3200,
            std::numeric_limits<std::int64_t>::max(), 333'333) &&
        !recorder.AddAudioFrame(&oneByte, oversized, 0, 333'333) &&
        recorder.IsRecording(); // invalid input must never copy/truncate DWORD

    std::vector<std::uint8_t> video(320u * 180u * 4u, 0);
    std::vector<std::int16_t> audio(1600u, 0);
    constexpr std::int64_t frameDuration100ns = 333'333;
    constexpr double pi = 3.14159265358979323846;
    // 90 frames = 3 s of timeline: with the explicit ~2 s keyframe/fragment
    // bound this must produce at least two completed fMP4 fragments, which
    // is what makes a crashed session partially recoverable.
    constexpr int kFrameCount = 90;
    for (int frame = 0; passed && frame < kFrameCount; ++frame) {
        for (std::size_t pixel = 0; pixel < video.size(); pixel += 4) {
            video[pixel + 0] =
                static_cast<std::uint8_t>((pixel / 4 + frame * 7) % 255);
            video[pixel + 1] =
                static_cast<std::uint8_t>((frame * 17) % 255);
            video[pixel + 2] = 96;
            video[pixel + 3] = 255;
        }
        for (std::size_t sample = 0; sample < audio.size(); ++sample) {
            const double phase =
                2.0 * pi * 440.0 *
                (frame * audio.size() + sample) / 48'000.0;
            audio[sample] =
                static_cast<std::int16_t>(std::sin(phase) * 6000.0);
        }

        okuflow::RecordingFrameIdentity identity;
        identity.captureTimestamp100ns =
            static_cast<std::int64_t>(frame) * frameDuration100ns +
            (frame >= 30 ? gap100ns : 0);
        identity.captureClock100ns = identity.captureTimestamp100ns;
        identity.sequenceNumber = static_cast<std::uint64_t>(frame);
        identity.frameRateNumerator = 30;
        identity.frameRateDenominator = 1;
        // Exercise the production admission query without dropping generated
        // fixture frames. A bounded retry exposes unsupported statistics or
        // an encoder whose startup lookahead cannot fit the live budget.
        const auto admissionDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        bool admitted = recorder.CanAcceptVideoFrame();
        while (!admitted && std::chrono::steady_clock::now() < admissionDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            admitted = recorder.CanAcceptVideoFrame();
        }
        if (!admitted) {
            std::cerr << codecName << " video admission did not recover; frame="
                      << frame << " statistics HRESULT="
                      << recorder.LastEncoderStatisticsError() << '\n';
        }
        passed = admitted && recorder.AddFrame(
            video.data(), 320u * 4u, identity);
        const auto audioDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        bool audioAdmitted = recorder.CanAcceptAudioFrame(
            audio.size() * sizeof(std::int16_t), identity.captureTimestamp100ns,
            frameDuration100ns);
        while (!audioAdmitted && std::chrono::steady_clock::now() < audioDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            audioAdmitted = recorder.CanAcceptAudioFrame(
                audio.size() * sizeof(std::int16_t), identity.captureTimestamp100ns,
                frameDuration100ns);
        }
        if (!audioAdmitted) std::cerr << codecName << " AAC admission did not recover\n";
        passed = passed && audioAdmitted && recorder.AddAudioFrame(
            reinterpret_cast<const std::uint8_t*>(audio.data()),
            audio.size() * sizeof(std::int16_t),
            identity.captureTimestamp100ns,
            frameDuration100ns);
    }

    const okuflow::VideoRecorder::FinalizeResult finalized =
        recorder.Stop();
    passed = passed && finalized.HasPlayableVideo() &&
             finalized.videoSamplesWritten == kFrameCount &&
             std::filesystem::exists(output) &&
             std::filesystem::file_size(output) > 1024u &&
             ValidateDecodedAudio(output, gap100ns);
    if (!passed) {
        std::cerr << "Synthetic " << codecName
                  << "/AAC MP4 validation failed: "
                  << recorder.LastError() << '\n';
    }
    if (passed) {
        // Crash-survivability contract: fragments must land at the pinned
        // keyframe cadence, not accumulate until Finalize. Count moof boxes.
        std::ifstream stream(output, std::ios::binary);
        const std::string bytes(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        std::size_t moofCount = 0;
        for (std::size_t at = bytes.find("moof");
             at != std::string::npos;
             at = bytes.find("moof", at + 4)) {
            ++moofCount;
        }
        if (moofCount < 2) {
            passed = false;
            std::cerr << codecName
                      << ": expected at least two fMP4 fragments for a 3 s "
                         "recording; found "
                      << moofCount << '\n';
        } else {
            std::cout << codecName << ": " << moofCount
                      << " fragments across 3 s\n";
        }
    }
    std::error_code removeError;
    std::filesystem::remove(output, removeError);
    return passed;
}

int main()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        std::cerr << "COM initialization failed\n";
        return 1;
    }
    const HRESULT mediaFoundation =
        MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mediaFoundation)) {
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        std::cerr << "Media Foundation initialization failed\n";
        return 1;
    }

    // H.264 has a guaranteed software MFT on every Windows machine: its leg
    // is mandatory. AV1 — the codec production tries FIRST — exists only as
    // a hardware encoder MFT; run it for real where the hardware exists
    // (the owner's RTX 40-series) and skip cleanly elsewhere. The runtime
    // wedge class from 2026-07-30 makes actual sample submission part of
    // codec validation, not just BeginWriting.
    bool h264Skipped = false;
    bool passed = RunCodecPass(
        okuflow::VideoRecorder::Codec::H264, &h264Skipped);
    if (h264Skipped) {
        std::cerr << "H.264 must always be available; treating the skip "
                     "as a failure\n";
        passed = false;
    }
    bool av1Skipped = false;
    const bool av1Passed = RunCodecPass(
        okuflow::VideoRecorder::Codec::Av1, &av1Skipped);
    std::cout << "AV1 leg: "
              << (av1Skipped
                      ? "SKIPPED (no hardware AV1 encoder)"
                      : (av1Passed ? "PASSED" : "FAILED"))
              << '\n';
    passed = passed && av1Passed;
    // Explicit diagnostic for the pre-existing MP4/AAC gap-collapse defect.
    // Kept out of the ordinary codec gate until its separate timeline repair.
    if (GetEnvironmentVariableW(L"OKUFLOW_TEST_AAC_GAP_DIAGNOSTIC", nullptr, 0) > 0) {
        bool gapSkipped = false;
        const bool gapPassed = RunCodecPass(okuflow::VideoRecorder::Codec::H264,
                                           &gapSkipped, 30'000'000);
        passed = passed && gapPassed && !gapSkipped;
    }
    std::error_code removeError;

    const std::filesystem::path emptyOutput = TemporaryMp4Path();
    okuflow::VideoRecorder emptyRecorder;
    bool emptyPassed = emptyRecorder.Start(
        emptyOutput.wstring(), 320, 180, 30, 1,
        okuflow::VideoRecorder::Codec::H264);
    const okuflow::VideoRecorder::FinalizeResult emptyFinalized =
        emptyRecorder.Stop();
    emptyPassed =
        emptyPassed && !emptyFinalized.HasPlayableVideo() &&
        emptyFinalized.videoSamplesWritten == 0;
    if (!emptyPassed) {
        std::cerr << "Empty recording sample accounting failed\n";
    }
    passed = passed && emptyPassed;
    std::filesystem::remove(emptyOutput, removeError);
    MFShutdown();
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return passed ? 0 : 1;
}

#endif // _WIN32
