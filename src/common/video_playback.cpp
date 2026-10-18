#ifdef _WIN32
#include "okuflow/common/video_playback.hpp"
#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace okuflow {
using Microsoft::WRL::ComPtr;
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::int64_t kMaximumClock100ns = INT64_MAX / 100;
constexpr unsigned kEndSeekLimit = 8;
std::int64_t AdvanceClock(std::int64_t base, std::int64_t elapsed) {
    base = std::clamp<std::int64_t>(base, 0, kMaximumClock100ns);
    return base + std::min(kMaximumClock100ns - base, std::max<std::int64_t>(0, elapsed));
}
std::atomic_flag nativeOwner = ATOMIC_FLAG_INIT;
struct PixelBudget { std::atomic<std::size_t> bytes{}, peak{}; };
void ReservePixels(const std::shared_ptr<PixelBudget>& pool, std::size_t bytes) {
    auto used = pool->bytes.load();
    do { if (bytes > VideoPlayback::kDecodedByteLimit || used > VideoPlayback::kDecodedByteLimit - bytes)
        throw std::runtime_error("Decoded staging and retained scenes exceed the 96 MiB pool.");
    } while (!pool->bytes.compare_exchange_weak(used, used + bytes));
    const auto total = used + bytes; auto peak = pool->peak.load();
    while (peak < total && !pool->peak.compare_exchange_weak(peak, total)) {}
}
enum class CommandKind { None, Open, Seek, End, Close };
struct Command {
    CommandKind kind{CommandKind::None}; std::uint64_t generation{};
    std::wstring path; std::shared_ptr<RecordingArchive> archive;
    std::int64_t target{}; Clock::time_point queued{};
};
std::string Error(const char* text, HRESULT hr) {
    char code[32]{}; std::snprintf(code, sizeof(code), " (0x%08lx)", static_cast<unsigned long>(hr));
    return std::string(text) + code;
}
void Check(HRESULT hr, const char* message) { if (FAILED(hr)) throw std::runtime_error(Error(message, hr)); }
struct DecodeFailure : std::runtime_error { using std::runtime_error::runtime_error; };
struct Reader {
    ComPtr<IMFSourceReader> source;
    ComPtr<IMFByteStream> bytes;
    VideoFileIdentity file;
    std::shared_ptr<RecordingArchive> archive;
    RecordingArchiveSnapshot archiveAdmission;
    std::uint32_t width{}, height{}, stride{}, rotation{}, parN{1}, parD{1};
    std::uint32_t videoStreamIndex{};
    std::uint32_t audioRate{}, audioChannels{};
    std::int64_t duration{};
    std::int64_t audioFloor100ns{};
    std::int64_t audioEnd100ns{};
    bool audioEos{};
    bool hasAudio{}, audioAvailable{}, eos{}, incomplete{};
    // Actual endpoint is discovered by reaching EOS, never duration - 1.
    std::int64_t lastDecodedPts{-1};
    std::shared_ptr<const VideoPlaybackFrame> pendingFrame;
    void Reset() {
        source.Reset(); bytes.Reset(); archive.reset(); lastDecodedPts = -1; pendingFrame.reset();
        width = height = stride = rotation = audioRate = audioChannels = 0;
        duration = 0; audioFloor100ns = audioEnd100ns = 0; audioEos = false;
        hasAudio = audioAvailable = eos = incomplete = false;
        parN = parD = 1; file = {}; archiveAdmission = {};
    }
};
bool HasDecodeCredit(const Reader& r, const std::shared_ptr<PixelBudget>& pool) {
    const auto squareWidth = (std::uint64_t(r.width) * r.parN + r.parD / 2) / r.parD;
    const auto needed = std::uint64_t(r.stride) * r.height + squareWidth * r.height * 4;
    return needed <= VideoPlayback::kDecodedByteLimit &&
        pool->bytes.load() <= VideoPlayback::kDecodedByteLimit - needed;
}
}
struct VideoPlayback::State {
    mutable std::mutex mutex; std::condition_variable changed;
    std::atomic<bool> stop{}; std::atomic<std::uint64_t> generation{};
    Command pending;
    VideoPlaybackSnapshot snapshot;
    std::deque<VideoPlaybackAudio> audio;
    std::size_t audioBytes{};
    std::uint64_t audioDuration100ns{};
    bool playing{}, muted{true}, restartAtCursor{};
    Clock::time_point operationStart{}, clockStart{};
    std::int64_t clockPts{};
    std::int64_t restartCursor100ns{};
    std::int64_t audioClockPts{}, audioClockSample{};
    bool audioClockValid{};
    std::shared_ptr<PixelBudget> pixels{std::make_shared<PixelBudget>()};
    bool Current(std::uint64_t g) const { return !stop.load() && generation.load() == g; }
};
namespace {
void ClearAudio(VideoPlayback::State& s) { s.audio.clear(); s.audioBytes = 0; s.audioDuration100ns = 0; s.audioClockValid = false; }
std::int64_t PlaybackNowLocked(const VideoPlayback::State& s, Clock::time_point now) {
    const auto nowTicks = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() / 100;
    if (!s.muted && s.audioClockValid && nowTicks >= s.audioClockSample && nowTicks - s.audioClockSample <= 5'000'000)
        return AdvanceClock(s.audioClockPts, nowTicks - s.audioClockSample);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - s.clockStart).count() / 100;
    return AdvanceClock(s.clockPts, elapsed);
}
void ConfigureVideo(Reader& r) {
    ComPtr<IMFMediaType> type;
    Check(r.source->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type), "Read decoded video format");
    GUID subtype{}; Check(type->GetGUID(MF_MT_SUBTYPE, &subtype), "Read decoded video subtype");
    if (subtype != MFVideoFormat_RGB32 && subtype != MFVideoFormat_ARGB32)
        throw std::runtime_error("The decoder did not provide bounded BGRA video.");
    Check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &r.width, &r.height), "Read video dimensions");
    UINT32 stride{};
    if (FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
        LONG calculated{}; Check(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
            r.width, &calculated), "Read video stride"); stride = static_cast<UINT32>(calculated);
    }
    const auto signedStride = static_cast<LONG>(stride);
    if (!VideoPlaybackLayoutFits(r.width, r.height, signedStride))
        throw std::runtime_error("Decoded video exceeds the 96 MiB staging budget or has an invalid layout.");
    r.stride = static_cast<std::uint32_t>(signedStride < 0 ? -std::int64_t(signedStride) : signedStride);
    type->GetUINT32(MF_MT_VIDEO_ROTATION, &r.rotation);
    if (r.rotation != 0 && r.rotation != 90 && r.rotation != 180 && r.rotation != 270)
        throw std::runtime_error("Unsupported video orientation metadata.");
    if (FAILED(MFGetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &r.parN, &r.parD)) || !r.parN || !r.parD)
        r.parN = r.parD = 1;
}
void ConfigureReader(Reader& r, const Command& cmd) {
    if (cmd.archive) {
        r.archive = cmd.archive;
        Check(cmd.archive->OpenReadStream(&r.bytes, &r.archiveAdmission), "Active recording is not yet safely readable");
        r.file = r.archiveAdmission.file; r.duration = r.archiveAdmission.duration100ns;
        r.incomplete = r.archiveAdmission.finalized && !r.archiveAdmission.complete;
    } else Check(OpenStableVideoReadStream(cmd.path, &r.bytes, &r.file), "Open a stable read-only video handle");
    ComPtr<IMFAttributes> attrs; Check(MFCreateAttributes(&attrs, 4), "Create playback attributes");
    Check(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "Enable native BGRA conversion");
    Check(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE), "Select bounded CPU decode output");
    Check(MFCreateSourceReaderFromByteStream(r.bytes.Get(), attrs.Get(), &r.source), "Open native video decoder");
    Check(r.source->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), "Select playback tracks");
    Check(r.source->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE), "Select video track");
    ComPtr<IMFMediaType> nativeVideo;
    Check(r.source->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeVideo), "Probe coded video format");
    UINT32 codedWidth{}, codedHeight{};
    Check(MFGetAttributeSize(nativeVideo.Get(), MF_MT_FRAME_SIZE, &codedWidth, &codedHeight), "Probe coded video dimensions");
    if (!VideoPlaybackLayoutFits(codedWidth, codedHeight, std::int64_t(codedWidth) * 4))
        throw std::runtime_error("Coded video dimensions exceed playback admission limits.");
    ComPtr<IMFMediaType> output; Check(MFCreateMediaType(&output), "Create decoded video type");
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    Check(r.source->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output.Get()), "Request BGRA video");
    ConfigureVideo(r);
    if (nativeVideo) {
        UINT32 rotation{}; UINT32 numerator{}, denominator{};
        if (SUCCEEDED(nativeVideo->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation)) &&
            (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270)) r.rotation = rotation;
        if (SUCCEEDED(MFGetAttributeRatio(nativeVideo.Get(), MF_MT_PIXEL_ASPECT_RATIO, &numerator, &denominator)) &&
            numerator && denominator) { r.parN = numerator; r.parD = denominator; }
    }
    PROPVARIANT duration; PropVariantInit(&duration);
    if (SUCCEEDED(r.source->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration))) {
        LONGLONG value{};
        if (SUCCEEDED(PropVariantToInt64(duration, &value)) && value > 0 && !cmd.archive) r.duration = value;
    }
    PropVariantClear(&duration);
    if (!VideoPlaybackTimestampFits(r.duration)) throw std::runtime_error("Video duration exceeds the supported host-clock range.");
    ComPtr<IMFMediaType> audioNative;
    r.hasAudio = SUCCEEDED(r.source->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audioNative));
    if (r.hasAudio) {
        ComPtr<IMFMediaType> pcm; Check(MFCreateMediaType(&pcm), "Create playback PCM type");
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(r.source->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcm.Get()))) {
            ComPtr<IMFMediaType> actual;
            UINT32 bits{};
            if (SUCCEEDED(r.source->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)) &&
                SUCCEEDED(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r.audioRate)) &&
                SUCCEEDED(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &r.audioChannels)) &&
                SUCCEEDED(actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) &&
                bits == 16 && r.audioRate >= 8000 && r.audioRate <= 192000 &&
                r.audioChannels > 0 && r.audioChannels <= 8) r.audioAvailable = true;
        }
    }
}
std::shared_ptr<const VideoPlaybackFrame> CopyFrame(Reader& r, IMFSample* sample,
    LONGLONG timestamp, std::uint64_t gen, const std::shared_ptr<PixelBudget>& budget) {
    const auto sourceBytes = std::uint64_t(r.stride) * r.height;
    const auto minimum = std::uint64_t(r.stride) * (r.height - 1) + std::uint64_t(r.width) * 4;
    DWORD count{}, total{};
    Check(sample->GetBufferCount(&count), "Count decoded video buffers");
    Check(sample->GetTotalLength(&total), "Measure decoded video staging");
    if (count != 1 || total < minimum || total > sourceBytes)
        throw std::runtime_error("Decoded RGB32 must use one buffer within its reserved staging size.");
    ComPtr<IMFMediaBuffer> buffer;
    Check(sample->GetBufferByIndex(0, &buffer), "Access decoded pixels without a contiguous copy");
    DWORD length{}, maximum{};
    Check(buffer->GetCurrentLength(&length), "Validate decoded sample length");
    Check(buffer->GetMaxLength(&maximum), "Validate decoded sample capacity");
    if (length != total || maximum > sourceBytes)
        throw std::runtime_error("Decoded video buffer capacity exceeds reserved staging.");
    const auto squareWidth64 = (std::uint64_t(r.width) * r.parN + r.parD / 2) / r.parD;
    if (squareWidth64 > UINT32_MAX || !VideoPlaybackLayoutFits(static_cast<UINT32>(squareWidth64), r.height,
            static_cast<std::int64_t>(squareWidth64 * 4)))
        throw std::runtime_error("Square-pixel video dimensions exceed the decoded pool.");
    const auto squareWidth = static_cast<UINT32>(squareWidth64);
    const auto bytes = std::size_t(squareWidth) * r.height * 4;
    ReservePixels(budget, bytes);
    std::shared_ptr<VideoPlaybackFrame> frame(new VideoPlaybackFrame,
        [budget, bytes](VideoPlaybackFrame* f) { delete f; budget->bytes.fetch_sub(bytes); });
    frame->bgra.resize(bytes); frame->width = squareWidth; frame->height = r.height;
    frame->strideBytes = squareWidth * 4; frame->rotationDegrees = r.rotation;
    frame->pixelAspectNumerator = frame->pixelAspectDenominator = 1;
    const auto copyRow = [&](std::uint32_t y, const BYTE* source) {
        auto* destination = frame->bgra.data() + std::size_t(y) * frame->strideBytes;
        if (squareWidth == r.width) std::memcpy(destination, source, frame->strideBytes);
        else {
            for (std::uint32_t x = 0; x < squareWidth; ++x) {
                const auto sampleX = std::min<std::uint32_t>(r.width - 1,
                    static_cast<std::uint32_t>(std::uint64_t(x) * r.width / squareWidth));
                std::memcpy(destination + std::size_t(x) * 4, source + std::size_t(sampleX) * 4, 4);
            }
        }
        // MF RGB32's fourth byte is padding, not valid transparency.
        for (std::uint32_t x = 0; x < squareWidth; ++x) destination[std::size_t(x) * 4 + 3] = 255;
    };
    if (!VideoPlaybackTimestampFits(timestamp))
        throw std::runtime_error("Decoded video timestamp is outside the supported monotonic clock range.");
    frame->pts100ns = timestamp; sample->GetSampleDuration(&frame->duration100ns);
    if (!VideoPlaybackTimestampFits(timestamp, frame->duration100ns))
        throw std::runtime_error("Decoded video duration is invalid.");
    frame->requestGeneration = gen; frame->archiveGeneration = r.archiveAdmission.generation;
    frame->sampleId = static_cast<std::uint64_t>(timestamp);
    frame->videoTrackId = r.archive ? r.archiveAdmission.videoTrackId : r.videoStreamIndex;
    ComPtr<IMF2DBuffer2> twoD;
    if (SUCCEEDED(buffer.As(&twoD))) {
        BYTE* first{}; BYTE* beginning{}; DWORD available{}; LONG pitch{};
        Check(twoD->Lock2DSize(MF2DBuffer_LockFlags_Read, &first, &pitch, &beginning, &available), "Lock decoded video rows");
        const auto firstAddress = reinterpret_cast<std::uintptr_t>(first);
        const auto beginAddress = reinterpret_cast<std::uintptr_t>(beginning);
        const auto lastDelta = std::int64_t(r.height - 1) * pitch;
        const auto low = lastDelta < 0 ? firstAddress - static_cast<std::uint64_t>(-lastDelta) : firstAddress;
        const auto high = lastDelta < 0 ? firstAddress : firstAddress + static_cast<std::uint64_t>(lastDelta);
        if (!VideoPlaybackLayoutFits(r.width, r.height, pitch) || available > std::uint64_t(r.stride) * r.height ||
            low < beginAddress || high < low ||
            high - beginAddress > available || r.width * 4 > available - (high - beginAddress)) {
            twoD->Unlock2D(); throw std::runtime_error("Invalid decoded row pitch or buffer range.");
        }
        for (std::uint32_t y = 0; y < r.height; ++y)
            copyRow(y, first + std::int64_t(y) * pitch);
        twoD->Unlock2D();
    } else {
        BYTE* data{}; DWORD current{}; Check(buffer->Lock(&data, &maximum, &current), "Lock decoded pixels");
        // RGB32's default-stride sign, rather than an assumed bitmap origin,
        // decides row order for buffers without IMF2DBuffer.
        ComPtr<IMFMediaType> type; LONG pitch = static_cast<LONG>(r.stride);
        if (SUCCEEDED(r.source->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type))) {
            UINT32 raw{}; if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw))) pitch = static_cast<LONG>(raw);
        }
        for (std::uint32_t y = 0; y < r.height; ++y) {
            const auto row = pitch < 0 ? r.height - 1 - y : y;
            copyRow(y, data + std::size_t(row) * r.stride);
        }
        buffer->Unlock();
    }
    if (r.archive && !r.archive->ValidateRange(r.archiveAdmission.generation, 0,
            r.archiveAdmission.verifiedMediaBytes)) throw std::runtime_error("Recording bytes changed during decode; seek again.");
    return frame;
}
bool ReadVideo(Reader& r, VideoPlayback::State& state, std::uint64_t gen,
               std::shared_ptr<const VideoPlaybackFrame>& frame,
               std::shared_ptr<const VideoPlaybackFrame>* optionalPrevious = nullptr) {
    const auto staging = std::size_t(r.stride) * r.height;
    ReservePixels(state.pixels, staging);
    struct StagingLease {
        std::shared_ptr<PixelBudget> pool; std::size_t bytes;
        ~StagingLease() { pool->bytes.fetch_sub(bytes); }
    } lease{state.pixels, staging};
    DWORD stream{}, flags{}; LONGLONG time{}; ComPtr<IMFSample> sample;
    const auto readHr = r.source->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        &stream, &flags, &time, &sample);
    if (FAILED(readHr)) throw DecodeFailure(Error("Decode video sample", readHr));
    if (!state.Current(gen)) return false;
    r.videoStreamIndex = stream;
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) ConfigureVideo(r);
    if (std::uint64_t(r.stride) * r.height > staging)
        throw std::runtime_error("Video format changed beyond the admitted decoded staging size.");
    if (flags & MF_SOURCE_READERF_ERROR) throw DecodeFailure("Native video source reported a decoder error.");
    if (sample) {
        const auto squareWidth = (std::uint64_t(r.width) * r.parN + r.parD / 2) / r.parD;
        const auto destination = squareWidth * r.height * 4;
        if (optionalPrevious && (destination > VideoPlayback::kDecodedByteLimit ||
            state.pixels->bytes.load() > VideoPlayback::kDecodedByteLimit - destination)) optionalPrevious->reset();
        frame = CopyFrame(r, sample.Get(), time, gen, state.pixels); r.lastDecodedPts = frame->pts100ns;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) r.eos = true;
    return sample != nullptr;
}
void SeekReader(Reader& r, std::int64_t target) {
    if (!VideoPlaybackTimestampFits(target)) throw std::runtime_error("Seek target exceeds the supported host-clock range.");
    PROPVARIANT position; PropVariantInit(&position); position.vt = VT_I8; position.hVal.QuadPart = std::max<std::int64_t>(0, target);
    Check(r.source->SetCurrentPosition(GUID_NULL, position), "Seek native video source"); r.eos = false; r.pendingFrame.reset(); r.lastDecodedPts = -1;
    r.audioFloor100ns = std::max<std::int64_t>(0, target);
    r.audioEnd100ns = r.audioFloor100ns; r.audioEos = false;
}
void MarkNativeOperation(VideoPlayback::State& s, std::uint64_t generation, bool active) {
    std::lock_guard lock(s.mutex);
    if (s.Current(generation)) s.operationStart = active ? Clock::now() : Clock::time_point{};
}
void QueueAudio(Reader& r, VideoPlayback::State& s, std::uint64_t gen,
                std::int64_t until, Clock::time_point deadline) {
    if (!r.audioAvailable || r.audioEos) return;
    // Keep audio independent from microphone settings. Up to eight short
    // decoder samples per pass, 500 ms and 2 MiB total remain queued.
    for (unsigned i = 0; i < 8 && Clock::now() < deadline && s.Current(gen); ++i) {
        { std::lock_guard lock(s.mutex);
          if (s.muted || !s.playing || s.audio.size() >= 32 || s.audioBytes >= VideoPlayback::kAudioByteLimit ||
              s.audioDuration100ns >= 5'000'000 ||
              (!s.audio.empty() && s.audio.back().pts100ns > until + 5'000'000)) return;
        }
        DWORD stream{}, flags{}; LONGLONG time{}; ComPtr<IMFSample> sample;
        const auto hr = r.source->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &stream, &flags, &time, &sample);
        if (!s.Current(gen)) return;
        if (Clock::now() > deadline) { r.audioAvailable = false; return; }
        if (FAILED(hr)) { r.audioAvailable = false; return; }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ComPtr<IMFMediaType> actual; UINT32 bits{}, rate{}, channels{};
            if (FAILED(r.source->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)) ||
                FAILED(actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) || bits != 16 ||
                FAILED(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) || rate < 8000 || rate > 192000 ||
                FAILED(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) || !channels || channels > 8) {
                r.audioAvailable = false; return;
            }
            r.audioRate = rate; r.audioChannels = channels;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) r.audioEos = true;
        if (!sample) return;
        DWORD count{}; if (FAILED(sample->GetBufferCount(&count)) || count != 1) { r.audioAvailable = false; return; }
        ComPtr<IMFMediaBuffer> buffer; if (FAILED(sample->GetBufferByIndex(0, &buffer))) return;
        DWORD length{}, maximum{};
        if (FAILED(buffer->GetCurrentLength(&length)) || FAILED(buffer->GetMaxLength(&maximum)) ||
            maximum > 256 * 1024 || !length || length > maximum ||
            length % (r.audioChannels * 2)) { r.audioAvailable = false; return; }
        const auto duration = std::uint64_t(length) * 10'000'000 / (r.audioRate * r.audioChannels * 2);
        if (duration > 5'000'000 || time > kMaximumClock100ns) { r.audioAvailable = false; return; }
        if (time + static_cast<std::int64_t>(duration) <= r.audioFloor100ns) continue;
        const auto frameBytes = r.audioChannels * 2;
        const auto frames = length / frameBytes;
        std::uint64_t skipFrames{};
        if (time < r.audioFloor100ns) {
            const auto delta = r.audioFloor100ns - time; // bounded by this packet's <=500 ms duration
            skipFrames = (std::uint64_t(delta) * r.audioRate + 9'999'999) / 10'000'000;
            while (skipFrames < frames && time + static_cast<std::int64_t>(skipFrames * 10'000'000 / r.audioRate) < r.audioFloor100ns) ++skipFrames;
        }
        if (skipFrames >= frames) continue;
        const auto skipBytes = static_cast<DWORD>(skipFrames * frameBytes);
        const auto keptBytes = length - skipBytes;
        const auto keptDuration = std::uint64_t(keptBytes) * 10'000'000 / (r.audioRate * frameBytes);
        VideoPlaybackAudio packet; packet.pcm.resize(keptBytes);
        packet.pts100ns = time + static_cast<std::int64_t>(skipFrames * 10'000'000 / r.audioRate);
        if (!VideoPlaybackTimestampFits(packet.pts100ns, static_cast<std::int64_t>(keptDuration))) { r.audioAvailable = false; return; }
        packet.requestGeneration = gen; packet.sampleRate = r.audioRate; packet.channels = r.audioChannels;
        r.audioEnd100ns = std::max(r.audioEnd100ns, packet.pts100ns + static_cast<std::int64_t>(keptDuration));
        BYTE* data{}; if (FAILED(buffer->Lock(&data, nullptr, nullptr))) return;
        std::memcpy(packet.pcm.data(), data + skipBytes, keptBytes); buffer->Unlock();
        std::lock_guard lock(s.mutex);
        if (!s.Current(gen) || s.muted || !s.playing) return;
        if (s.audioBytes + keptBytes > VideoPlayback::kAudioByteLimit || s.audio.size() >= 32 ||
            s.audioDuration100ns + keptDuration > 5'000'000) return;
        s.audioBytes += keptBytes; s.audioDuration100ns += keptDuration; s.audio.push_back(std::move(packet));
        if (time >= until || r.audioEos) return;
    }
}
void Publish(VideoPlayback::State& s, Reader& r, const Command& cmd,
             std::shared_ptr<const VideoPlaybackFrame> frame, VideoPlaybackState status,
             std::shared_ptr<const VideoPlaybackFrame> neighbor = {}) {
    std::lock_guard lock(s.mutex);
    if (!s.Current(cmd.generation)) return;
    s.snapshot.requestGeneration = cmd.generation; s.snapshot.state = status;
    if (frame) s.snapshot.frame = std::move(frame);
    s.snapshot.neighborFrame = std::move(neighbor);
    s.snapshot.file = r.file; s.snapshot.path = cmd.archive ? r.archiveAdmission.path : cmd.path;
    s.snapshot.videoTrackId = r.archive ? r.archiveAdmission.videoTrackId : r.videoStreamIndex;
    s.snapshot.duration100ns = r.duration;
    s.snapshot.seekableEnd100ns = r.archive ? r.archiveAdmission.seekableEnd100ns
        : (r.eos ? r.lastDecodedPts : -1);
    s.snapshot.hasAudio = r.hasAudio; s.snapshot.audioAvailable = r.audioAvailable;
    s.snapshot.activeRecording = r.archive && !r.archiveAdmission.finalized;
    s.snapshot.incomplete = r.incomplete; s.snapshot.error.clear();
    s.operationStart = {};
    if (s.snapshot.frame) { s.clockPts = s.snapshot.frame->pts100ns; s.clockStart = Clock::now(); }
}
void Worker(std::shared_ptr<VideoPlayback::State> s) {
    bool permit{}; bool com{}; bool mf{}; Reader reader; Command document;
    try {
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Check(hr, "Initialize playback COM worker"); com = true;
        Check(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Initialize playback Media Foundation"); mf = true;
        while (!s->stop.load()) {
            Command command; bool play{};
            { std::unique_lock lock(s->mutex);
              s->changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return s->stop.load() || s->pending.kind != CommandKind::None; });
              if (s->stop.load()) break;
              command = std::move(s->pending); s->pending = {}; play = s->playing;
            }
            if (command.kind != CommandKind::None) {
                try {
                    if (!s->Current(command.generation)) continue;
                    if (command.kind == CommandKind::Close) {
                        reader.Reset(); document = {};
                        if (permit) { nativeOwner.clear(); permit = false; }
                        continue;
                    }
                    if (Clock::now() - command.queued > std::chrono::seconds(2)) throw std::runtime_error("Playback request expired before native admission.");
                    if (!permit) {
                        if (nativeOwner.test_and_set()) throw std::runtime_error("The previous native playback pipeline is still retiring.");
                        permit = true;
                    }
                    if (command.kind == CommandKind::Open) {
                        // Release the old pipeline completely before creating
                        // the successor. A stuck Release retains the permit.
                        reader.Reset();
                        if (!s->Current(command.generation)) continue;
                        ConfigureReader(reader, command); document = command;
                    } else {
                        if (!reader.source) throw std::runtime_error("No readable video is selected.");
                        document.generation = command.generation;
                        if (document.archive) {
                            const auto now = document.archive->Snapshot();
                            if (now.generation != reader.archiveAdmission.generation ||
                                now.verifiedMediaBytes != reader.archiveAdmission.verifiedMediaBytes) {
                                reader.Reset();
                                if (!s->Current(command.generation)) continue;
                                ConfigureReader(reader, document);
                            }
                        }
                    }
                    if (!s->Current(command.generation)) continue;
                    const bool end = command.kind == CommandKind::End;
                    // An active index proves an actual video PTS. Saved-file
                    // duration can extend beyond video (for example AAC tail),
                    // so its near-tail anchor remains only a search position.
                    std::int64_t endLookback = std::min<std::int64_t>(reader.duration, 1'000'000);
                    auto target = end ? (reader.archive && reader.archiveAdmission.seekableEnd100ns >= 0
                        ? reader.archiveAdmission.seekableEnd100ns : reader.duration - endLookback) : command.target;
                    if (end) endLookback = std::max<std::int64_t>(endLookback, reader.duration - target);
                    // Every command repositions audio and video together; no
                    // hidden audio cursor can continue through a scrub.
                    if (reader.hasAudio) reader.source->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
                    unsigned seeks{};
                    if (command.kind != CommandKind::Open) { SeekReader(reader, target); ++seeks; }
                    const auto deadline = command.queued + std::chrono::seconds(2);
                    if (Clock::now() >= deadline) throw std::runtime_error("Native open/seek exhausted its request-age deadline.");
                    std::shared_ptr<const VideoPlaybackFrame> chosen, neighbor;
                    unsigned samples{}, decoded{};
                    while (samples < VideoPlayback::kSeekSampleLimit && Clock::now() < deadline && s->Current(command.generation)) {
                        ++samples;
                        // Keep the last decoded frame through an EOS read.
                        // A real replacement sample may release this optional
                        // lease before its owned pixel copy is allocated.
                        neighbor.reset();
                        std::shared_ptr<const VideoPlaybackFrame> next;
                        try { ReadVideo(reader, *s, command.generation, next, &chosen); }
                        catch (const DecodeFailure&) {
                            if (!end || !chosen) throw;
                            reader.eos = true; reader.incomplete = true; break;
                        }
                        if (next) { ++decoded; neighbor = std::move(chosen); chosen = std::move(next); }
                        if (end && reader.eos && !chosen && target > 0 && seeks < kEndSeekLimit) {
                            // Only observed video EOS without a frame permits
                            // an earlier anchor. All attempts share the same
                            // read/deadline budget; the last seek reaches zero
                            // to cover a sparse video followed by long audio.
                            endLookback = std::min(reader.duration, std::max<std::int64_t>(1'000'000, endLookback * 2));
                            auto earlier = seeks + 1 == kEndSeekLimit ? 0 : reader.duration - endLookback;
                            if (earlier >= target) earlier = target / 2;
                            target = earlier; SeekReader(reader, target); ++seeks;
                            continue;
                        }
                        if (reader.eos || (!end && chosen && chosen->pts100ns >= target)) break;
                    }
                    if (!s->Current(command.generation)) continue;
                    if (Clock::now() > deadline || !chosen || (end && !reader.eos) || (!end && chosen->pts100ns < target))
                        throw std::runtime_error("This seek exceeds the 120-read / two-second decode budget: reads=" +
                            std::to_string(samples) + ", seeks=" + std::to_string(seeks) + ", decoded=" + std::to_string(decoded) +
                            ", eos=" + std::to_string(reader.eos) + ", chosen=" + std::to_string(bool(chosen)) +
                            ", lastPts=" + std::to_string(reader.lastDecodedPts) + ", anchor=" + std::to_string(target) +
                            ", duration=" + std::to_string(reader.duration) + ", elapsedMs=" +
                            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - command.queued).count()) + ".");
                    if (!neighbor && !reader.eos && samples < VideoPlayback::kSeekSampleLimit && Clock::now() < deadline && HasDecodeCredit(reader, s->pixels)) {
                        // Optional corroboration shares this decoder and does
                        // not change the exact selected frame. Retain it as
                        // the next playback frame so Resume skips nothing.
                        try {
                            std::shared_ptr<const VideoPlaybackFrame> next;
                            ReadVideo(reader, *s, command.generation, next);
                            if (next && next->pts100ns != chosen->pts100ns) {
                                neighbor = next; reader.pendingFrame = next;
                            }
                        } catch (const std::exception&) { /* Neighbor is optional. */ }
                    }
                    if (neighbor && neighbor->pts100ns == chosen->pts100ns) neighbor.reset();
                    if (Clock::now() > deadline) throw std::runtime_error("Native playback returned after its request-age deadline.");
                    // End is exact only after EOS. Failed seeks leave the
                    // externally held image intact.
                    { std::lock_guard lock(s->mutex); s->playing = false; ClearAudio(*s); }
                    document.generation = command.generation;
                    Publish(*s, reader, document, chosen, end || (reader.eos && !reader.pendingFrame) ? VideoPlaybackState::Ended : VideoPlaybackState::Paused, neighbor);
                } catch (const std::exception& error) {
                    std::lock_guard lock(s->mutex);
                    if (s->Current(command.generation)) { s->snapshot.state = VideoPlaybackState::Unavailable;
                        s->snapshot.requestGeneration = command.generation; s->snapshot.error = error.what(); s->playing = false; ClearAudio(*s); s->operationStart = {}; }
                }
                continue;
            }
            if (!play || !reader.source || !s->Current(document.generation)) continue;
            if (reader.eos && !reader.pendingFrame) {
                if (document.archive) {
                    const auto current = document.archive->Snapshot();
                    if (!current.finalized && current.generation == reader.archiveAdmission.generation &&
                        current.verifiedMediaBytes == reader.archiveAdmission.verifiedMediaBytes) {
                        std::lock_guard lock(s->mutex);
                        if (s->Current(document.generation)) {
                            if (s->snapshot.state != VideoPlaybackState::Waiting) ClearAudio(*s);
                            s->snapshot.state = VideoPlaybackState::Waiting;
                            s->snapshot.error = "Waiting for the next complete recording fragment.";
                        }
                        continue;
                    }
                    if (current.verifiedMediaBytes > reader.archiveAdmission.verifiedMediaBytes ||
                        current.generation != reader.archiveAdmission.generation) {
                        try {
                            reader.Reset();
                            ConfigureReader(reader, document);
                            std::lock_guard lock(s->mutex);
                            if (s->Current(document.generation)) {
                                s->restartAtCursor = true;
                                if (s->snapshot.frame) s->restartCursor100ns = s->snapshot.frame->pts100ns;
                                // Waiting is a stopped media clock. New bytes
                                // resume from held PTS, not from wall time
                                // accumulated while the archive had no tail.
                                s->clockStart = Clock::now();
                                s->clockPts = s->restartCursor100ns;
                                ClearAudio(*s);
                                s->snapshot.error.clear();
                            }
                            continue;
                        } catch (const std::exception& error) {
                            std::lock_guard lock(s->mutex);
                            if (s->Current(document.generation)) { s->playing = false;
                                s->snapshot.state = VideoPlaybackState::Unavailable; s->snapshot.error = error.what(); }
                            continue;
                        }
                    }
                }
                if (!document.archive && reader.audioAvailable) {
                    bool enabled{}; std::int64_t mediaNow{};
                    { std::lock_guard lock(s->mutex);
                      enabled = s->Current(document.generation) && s->playing && !s->muted;
                      mediaNow = PlaybackNowLocked(*s, Clock::now());
                    }
                    if (enabled && (!reader.audioEos || mediaNow < reader.audioEnd100ns)) {
                        MarkNativeOperation(*s, document.generation, true);
                        QueueAudio(reader, *s, document.generation, mediaNow + 2'000'000,
                            Clock::now() + std::chrono::seconds(2));
                        MarkNativeOperation(*s, document.generation, false);
                        std::unique_lock lock(s->mutex);
                        s->changed.wait_for(lock, std::chrono::milliseconds(20), [&] {
                            return !s->Current(document.generation) || !s->playing || s->muted;
                        });
                        continue;
                    }
                }
                std::lock_guard lock(s->mutex);
                if (s->Current(document.generation)) { s->playing = false; s->snapshot.state = VideoPlaybackState::Ended;
                    if (!document.archive && s->snapshot.frame) s->snapshot.seekableEnd100ns = s->snapshot.frame->pts100ns; }
                continue;
            }
            try {
                MarkNativeOperation(*s, document.generation, true);
                const auto deadline = Clock::now() + std::chrono::seconds(2);
                bool restart{}, audioEnabled{}; std::int64_t cursor{};
                { std::lock_guard lock(s->mutex);
                  restart = s->restartAtCursor; s->restartAtCursor = false;
                  audioEnabled = !s->muted && reader.audioAvailable;
                  cursor = s->restartCursor100ns;
                  s->snapshot.neighborFrame.reset();
                }
                if (restart) {
                    if (reader.hasAudio) Check(reader.source->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, audioEnabled), "Select playback audio");
                    SeekReader(reader, cursor);
                }
                std::shared_ptr<const VideoPlaybackFrame> next;
                // Read one next frame, then wait on the shared CV. New seeks
                // and Pause immediately supersede its publication authority.
                if (reader.pendingFrame) { next = std::move(reader.pendingFrame); }
                else {
                    unsigned samples{};
                    do {
                        next.reset(); ReadVideo(reader, *s, document.generation, next);
                    } while (restart && next && next->pts100ns <= cursor && !reader.eos &&
                        ++samples < VideoPlayback::kSeekSampleLimit && Clock::now() < deadline && s->Current(document.generation));
                    if (restart && next && next->pts100ns <= cursor && !reader.eos)
                        throw std::runtime_error("Resume exceeds the bounded seek budget.");
                }
                if (Clock::now() > deadline) throw std::runtime_error("Playback decode exceeded its cooperative deadline.");
                MarkNativeOperation(*s, document.generation, false);
                if (!next) continue;
                bool superseded{};
                for (;;) {
                    std::int64_t mediaNow{}, waitTicks{};
                    { std::lock_guard lock(s->mutex);
                      if (!s->playing || !s->Current(document.generation) || s->restartAtCursor) {
                          if (s->Current(document.generation)) reader.pendingFrame = next;
                          superseded = true; break;
                      }
                      mediaNow = PlaybackNowLocked(*s, Clock::now());
                      waitTicks = std::max<std::int64_t>(0, next->pts100ns - mediaNow);
                    }
                    // Audio progresses while the same owned video frame is
                    // waiting for a distant VFR timestamp. No second reader
                    // or audio thread is introduced; the queue remains capped.
                    MarkNativeOperation(*s, document.generation, true);
                    QueueAudio(reader, *s, document.generation, mediaNow + 2'000'000,
                        Clock::now() + std::chrono::seconds(2));
                    MarkNativeOperation(*s, document.generation, false);
                    { std::lock_guard lock(s->mutex);
                      if (s->Current(document.generation)) s->snapshot.audioAvailable = reader.audioAvailable;
                    }
                    if (!waitTicks) break;
                    std::unique_lock lock(s->mutex);
                    s->changed.wait_for(lock, std::min(std::chrono::nanoseconds(waitTicks * 100),
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(20))),
                        [&] { return s->stop.load() || !s->playing || !s->Current(document.generation) || s->restartAtCursor; });
                }
                if (superseded) continue;
                // Preserve one clock origin across ordinary playback frames.
                { std::lock_guard lock(s->mutex);
                  if (s->Current(document.generation) && s->playing) {
                      s->snapshot.neighborFrame = s->snapshot.frame;
                      s->snapshot.frame = next; s->snapshot.state = VideoPlaybackState::Playing;
                      s->snapshot.audioAvailable = reader.audioAvailable;
                      if (reader.eos) s->snapshot.seekableEnd100ns = next->pts100ns;
                  }
                }
            } catch (const DecodeFailure& error) {
                std::lock_guard lock(s->mutex);
                if (s->Current(document.generation)) {
                    s->operationStart = {};
                    s->playing = false; ClearAudio(*s);
                    s->snapshot.state = s->snapshot.frame ? VideoPlaybackState::Ended : VideoPlaybackState::Unavailable;
                    s->snapshot.incomplete = true; s->snapshot.error = error.what();
                    if (s->snapshot.frame) s->snapshot.seekableEnd100ns = s->snapshot.frame->pts100ns;
                    reader.incomplete = true; reader.eos = true;
                }
            } catch (const std::exception& error) {
                std::lock_guard lock(s->mutex);
                if (s->Current(document.generation)) { s->playing = false; ClearAudio(*s);
                    s->operationStart = {};
                    s->snapshot.state = VideoPlaybackState::Unavailable; s->snapshot.error = error.what(); }
            }
        }
    } catch (const std::exception& error) {
        std::lock_guard lock(s->mutex); s->snapshot.state = VideoPlaybackState::Unavailable; s->snapshot.error = error.what();
    }
    reader.Reset(); // retirement is part of the single owner budget
    if (mf) MFShutdown();
    if (com) CoUninitialize();
    if (permit) nativeOwner.clear();
}
std::uint64_t Submit(const std::shared_ptr<VideoPlayback::State>& s, Command command) {
    std::lock_guard lock(s->mutex);
    command.generation = s->generation.fetch_add(1) + 1; command.queued = Clock::now();
    s->snapshot.requestGeneration = command.generation;
    s->snapshot.state = command.kind == CommandKind::Open ? VideoPlaybackState::Opening : VideoPlaybackState::Seeking;
    s->snapshot.error.clear(); s->playing = false; ClearAudio(*s);
    if (command.kind == CommandKind::Open) { s->muted = true; s->snapshot.muted = true; }
    s->snapshot.neighborFrame.reset();
    s->operationStart = command.queued; s->pending = std::move(command); s->changed.notify_one();
    return s->generation.load();
}
}
bool VideoPlaybackLayoutFits(std::uint32_t width, std::uint32_t height, std::int64_t stride) {
    if (!width || !height || width > 16384 || height > 16384 || stride == INT64_MIN) return false;
    const auto pitch = static_cast<std::uint64_t>(stride < 0 ? -stride : stride);
    return pitch >= std::uint64_t(width) * 4 && pitch <= VideoPlayback::kDecodedByteLimit / height &&
        std::uint64_t(width) * height * 4 <= VideoPlayback::kDecodedByteLimit;
}
bool VideoPlaybackTimestampFits(std::int64_t pts, std::int64_t duration) {
    return pts >= 0 && pts <= kMaximumClock100ns && duration >= 0 && duration <= kMaximumClock100ns - pts;
}
VideoPlayback::VideoPlayback() : state_(std::make_shared<State>()) { std::thread(Worker, state_).detach(); }
VideoPlayback::~VideoPlayback() { state_->stop.store(true); state_->generation.fetch_add(1); state_->changed.notify_one(); }
std::uint64_t VideoPlayback::OpenFile(std::wstring path) { Command c; c.kind = CommandKind::Open; c.path = std::move(path); return Submit(state_, std::move(c)); }
std::uint64_t VideoPlayback::OpenArchive(std::shared_ptr<RecordingArchive> archive) { Command c; c.kind = CommandKind::Open; c.archive = std::move(archive); return Submit(state_, std::move(c)); }
std::uint64_t VideoPlayback::Seek100ns(std::int64_t target) { Command c; c.kind = CommandKind::Seek; c.target = std::max<std::int64_t>(0, target); return Submit(state_, std::move(c)); }
std::uint64_t VideoPlayback::JumpToEnd() { Command c; c.kind = CommandKind::End; return Submit(state_, std::move(c)); }
void VideoPlayback::SetPlaying(bool play) {
    std::lock_guard lock(state_->mutex);
    if (play && (!state_->snapshot.frame ||
        (state_->snapshot.state != VideoPlaybackState::Paused &&
         state_->snapshot.state != VideoPlaybackState::Playing &&
         state_->snapshot.state != VideoPlaybackState::Waiting &&
         !(state_->snapshot.state == VideoPlaybackState::Ended && state_->snapshot.activeRecording)) ||
        (state_->snapshot.state == VideoPlaybackState::Ended && !state_->snapshot.activeRecording) ||
        state_->pending.kind != CommandKind::None)) return;
    state_->playing = play; ClearAudio(*state_);
    if (play) { state_->restartAtCursor = true; state_->restartCursor100ns = state_->snapshot.frame->pts100ns;
        state_->clockStart = Clock::now(); state_->clockPts = state_->snapshot.frame->pts100ns; state_->snapshot.state = VideoPlaybackState::Playing; }
    else if (state_->snapshot.frame && (state_->snapshot.state == VideoPlaybackState::Playing || state_->snapshot.state == VideoPlaybackState::Waiting))
        state_->snapshot.state = VideoPlaybackState::Paused;
    state_->changed.notify_one();
}
void VideoPlayback::SetMuted(bool muted) {
    std::lock_guard lock(state_->mutex);
    if (state_->muted == muted) return;
    if (state_->playing) {
        const auto now = Clock::now(); state_->restartCursor100ns = PlaybackNowLocked(*state_, now);
        state_->clockPts = state_->restartCursor100ns; state_->clockStart = now; state_->restartAtCursor = true;
    }
    state_->muted = muted; state_->snapshot.muted = muted; ClearAudio(*state_); state_->changed.notify_one();
}
void VideoPlayback::SetAudioClock100ns(std::int64_t pts, std::int64_t sampled, std::uint64_t generation) {
    std::lock_guard lock(state_->mutex);
    if (!state_->Current(generation)) return;
    if (pts < 0) { state_->audioClockValid = false; state_->changed.notify_one(); return; }
    if (!VideoPlaybackTimestampFits(pts) || sampled < 0 || sampled > kMaximumClock100ns || state_->muted || !state_->playing) return;
    state_->audioClockPts = pts; state_->audioClockSample = sampled; state_->audioClockValid = true;
    state_->changed.notify_one();
}
void VideoPlayback::Close() {
    Command c; c.kind = CommandKind::Close; Submit(state_, std::move(c));
    std::lock_guard lock(state_->mutex); state_->snapshot = {}; state_->snapshot.requestGeneration = state_->generation.load();
}
VideoPlaybackSnapshot VideoPlayback::Snapshot() const {
    std::lock_guard lock(state_->mutex); auto result = state_->snapshot;
    result.decodedBytes = state_->pixels->bytes.load(); result.peakDecodedBytes = state_->pixels->peak.load();
    if (state_->operationStart != Clock::time_point{} && Clock::now() - state_->operationStart > std::chrono::seconds(2)) {
        result.state = VideoPlaybackState::Unavailable;
        result.error = "Native playback operation exceeded its cooperative deadline; replacement waits for the same worker.";
    }
    return result;
}
std::vector<VideoPlaybackAudio> VideoPlayback::TakeAudio() {
    std::lock_guard lock(state_->mutex); std::vector<VideoPlaybackAudio> result;
    if (!state_->muted && state_->playing) while (!state_->audio.empty()) {
        auto packet = std::move(state_->audio.front()); state_->audio.pop_front();
        if (packet.requestGeneration == state_->generation.load()) result.push_back(std::move(packet));
    }
    state_->audio.clear(); state_->audioBytes = 0; state_->audioDuration100ns = 0;
    return result;
}
} // namespace okuflow
#endif
