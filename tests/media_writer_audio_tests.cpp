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
#include <cstring>

namespace okuflow {
struct VideoRecorderTestAccess {
    static std::uint64_t PcmFrames(const VideoRecorder& recorder) {
        return recorder.audioTimeline_.Frames();
    }
    static void ClearExpectedInputRejection(VideoRecorder& recorder) {
        recorder.lastError_.clear();
    }
    static bool TrySinkAtExistingPath(VideoRecorder& recorder,
                                       const std::filesystem::path& path) {
        try {
            return recorder.InitializeSink(path.wstring(), 320, 180, 30, 1,
                VideoRecorder::Codec::H264, nullptr, nullptr, MFVideoFormat_ARGB32);
        } catch (...) {
            return false;
        }
    }
};
} // namespace okuflow

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
    const DWORD evidenceLength = GetEnvironmentVariableW(
        L"OKUFLOW_TEST_AAC_EVIDENCE_DIR", nullptr, 0);
    if (evidenceLength > 0) {
        std::wstring evidence(evidenceLength, L'\0');
        evidence.resize(GetEnvironmentVariableW(L"OKUFLOW_TEST_AAC_EVIDENCE_DIR",
            evidence.data(), evidenceLength));
        std::filesystem::create_directories(evidence);
        path = std::filesystem::path(evidence) / path.filename();
    }
    return path;
}

bool ValidateDecodedAudio(const std::filesystem::path& path,
                          std::int64_t expectedGap100ns,
                          std::int64_t* lateMarker100ns)
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
                FAILED(pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1)) ||
                FAILED(pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000)) ||
                FAILED(pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) ||
                FAILED(reader->SetCurrentMediaType(stream, nullptr, pcm.Get()))) return false;
            std::uint64_t decodedBytes = 0;
            LONGLONG lastTimestamp = -1;
            *lateMarker100ns = -1;
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
                ComPtr<IMFMediaBuffer> buffer;
                BYTE* data = nullptr;
                DWORD length = 0;
                if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) ||
                    FAILED(buffer->Lock(&data, nullptr, &length))) return false;
                // Compare 10 ms tone windows, independent of AAC packet size
                // and encoder priming. The 880 Hz marker starts after the gap;
                // payload length or a late PTS alone cannot prove its position.
                constexpr std::size_t window = 480;
                for (std::size_t at = 0; at + window <= length / 2;
                     at += window) {
                    double lowRe = 0, lowIm = 0, highRe = 0, highIm = 0;
                    for (std::size_t index = 0; index < window; ++index) {
                        std::int16_t value;
                        std::memcpy(&value, data + (at + index) * 2, 2);
                        const double phase = 2.0 * 3.14159265358979323846 *
                                             index / 48'000.0;
                        lowRe += value * std::cos(440.0 * phase);
                        lowIm += value * std::sin(440.0 * phase);
                        highRe += value * std::cos(880.0 * phase);
                        highIm += value * std::sin(880.0 * phase);
                    }
                    const double low = lowRe * lowRe + lowIm * lowIm;
                    const double high = highRe * highRe + highIm * highIm;
                    if (*lateMarker100ns < 0 && high > 1.0e10 && high > low * 4.0) {
                        *lateMarker100ns = timestamp +
                            static_cast<LONGLONG>(at) * 10'000'000 / 48'000;
                    }
                }
                buffer->Unlock();
            }
            // AAC's fixed packets may pad a boundary. Decode the entire track,
            // rather than accepting a stream header with missing late speech.
            const auto decodedFrames = decodedBytes / 2;
            std::cout << "AAC decoded PCM frames=" << decodedFrames
                      << " last timestamp=" << lastTimestamp
                      << " late-tone marker=" << *lateMarker100ns
                      << " expected=" << 10'000'000 + expectedGap100ns << '\n';
            // The decoder may expose a PTS gap or materialize it as silence.
            // Either representation must preserve the audible marker's time.
            return ended && decodedFrames >= 144'000 - 4096 &&
                decodedFrames <= 144'000 +
                    static_cast<std::uint64_t>(expectedGap100ns * 48'000 / 10'000'000) + 4096 &&
                std::abs(*lateMarker100ns - (10'000'000 + expectedGap100ns)) <= 1'500'000 &&
                lastTimestamp >= 28'000'000 + expectedGap100ns;
        }
    }
    return false;
}

bool ValidateDecodedVideoMarker(const std::filesystem::path& path,
                                std::int64_t audioMarker100ns,
                                std::int64_t gap100ns)
{
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> rgb;
    if (FAILED(MFCreateAttributes(&attributes, 1)) ||
        FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
        FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader)) ||
        FAILED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)) ||
        FAILED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)) ||
        FAILED(MFCreateMediaType(&rgb)) ||
        FAILED(rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
        FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                          nullptr, rgb.Get()))) return false;
    LONGLONG marker = -1, previous = -1;
    unsigned frames = 0;
    bool ended = false;
    for (unsigned read = 0; read < 1024 && !ended; ++read) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                     nullptr, &flags, &timestamp, &sample))) return false;
        ended = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        if (!sample) continue;
        if (timestamp <= previous) return false;
        previous = timestamp;
        ++frames;
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) ||
            FAILED(buffer->Lock(&data, nullptr, &length))) return false;
        constexpr std::size_t center = (90u * 320u + 160u) * 4u;
        const bool green = length >= center + 4 &&
            data[center + 1] > 150 && data[center + 2] < 80;
        buffer->Unlock();
        if (green && marker < 0) marker = timestamp;
    }
    std::cout << "Video decoded frames=" << frames << " green marker=" << marker
              << " audio/video marker delta=" << audioMarker100ns - marker << '\n';
    return ended && frames == 90 &&
        std::abs(marker - (10'000'000 + gap100ns)) <= 400'000 &&
        std::abs(audioMarker100ns - marker) <= 1'500'000;
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
        if (GetEnvironmentVariableW(L"OKUFLOW_TEST_AAC_EVIDENCE_DIR", nullptr, 0) == 0) {
            std::filesystem::remove(output, cleanupError);
        } else {
            std::cout << "Retained unavailable-codec fixture: " << output << '\n';
        }
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
    passed = passed && !recorder.CanAcceptAudioFrame(3200, 100'010'000, 333'333) &&
        okuflow::VideoRecorderTestAccess::PcmFrames(recorder) == 0;
    // The deliberately rejected oversized input sets LastError. Keep that
    // expected validation diagnostic separate from subsequent codec failures.
    if (passed) okuflow::VideoRecorderTestAccess::ClearExpectedInputRejection(recorder);

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
            video[pixel + 0] = 0;
            video[pixel + 1] = frame < 30 ? 0 : 230;
            video[pixel + 2] = frame < 30 ? 230 : 0;
            video[pixel + 3] = 255;
        }
        for (std::size_t sample = 0; sample < audio.size(); ++sample) {
            const double phase =
                2.0 * pi * (frame < 30 ? 440.0 : 880.0) *
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

    const auto expectedPcmFrames = 144'000ULL +
        static_cast<std::uint64_t>(gap100ns * 48'000 / 10'000'000);
    const auto submittedPcmFrames = okuflow::VideoRecorderTestAccess::PcmFrames(recorder);
    std::cout << "Submitted PCM frames=" << submittedPcmFrames
              << " expected=" << expectedPcmFrames << '\n';
    passed = passed && submittedPcmFrames == expectedPcmFrames;
    const okuflow::VideoRecorder::FinalizeResult finalized =
        recorder.Stop();
    std::int64_t lateAudioMarker100ns = -1;
    passed = passed && finalized.HasPlayableVideo() &&
             finalized.videoSamplesWritten == kFrameCount &&
             std::filesystem::exists(output) &&
             std::filesystem::file_size(output) > 1024u;
    const bool decodedAudio = ValidateDecodedAudio(output, gap100ns, &lateAudioMarker100ns);
    // Gap verification uses H.264's native decoder. Ordinary AV1 encoding
    // coverage does not require an independently installed AV1 decoder.
    const bool decodedVideo = gap100ns == 0 ||
        ValidateDecodedVideoMarker(output, lateAudioMarker100ns, gap100ns);
    passed = passed && decodedAudio && decodedVideo;
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
    if (passed && GetEnvironmentVariableW(
            L"OKUFLOW_TEST_AAC_EVIDENCE_DIR", nullptr, 0) == 0) {
        std::filesystem::remove(output, removeError);
    } else {
        std::cout << "Retained marked " << codecName << "/AAC fixture: " << output << '\n';
    }
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
    // Bypass Start's advisory precheck to model another process publishing a
    // file immediately before native sink creation. The sink itself must be
    // exclusive, including for a pre-existing zero-byte user file.
    for (const std::string bytes : {std::string(), std::string("user-owned bytes")}) {
        const auto collision = TemporaryMp4Path();
        { std::ofstream existing(collision, std::ios::binary); existing << bytes; }
        okuflow::VideoRecorder recorder;
        const bool rejected = !okuflow::VideoRecorderTestAccess::TrySinkAtExistingPath(
            recorder, collision);
        recorder.Stop();
        std::ifstream existing(collision, std::ios::binary);
        const std::string retained((std::istreambuf_iterator<char>(existing)),
                                    std::istreambuf_iterator<char>());
        existing.close();
        if (!rejected || retained != bytes) {
            std::cerr << "Exclusive recording output collision failed\n";
            passed = false;
        }
        std::error_code cleanup;
        std::filesystem::remove(collision, cleanup);
    }
    // Opt-in verification of an unconfirmed codec/timeline finding. Actual
    // decoded 440->880 Hz speech surrogate and red->green video markers must
    // remain aligned at 4 s after a 3 s capture gap. No fake codec is used.
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
