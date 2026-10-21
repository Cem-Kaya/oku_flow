#ifdef _WIN32
#include "okuflow/common/video_playback.hpp"
#include "video_playback_seek_budget.hpp"
#include "video_ffmpeg_reader.hpp"
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
struct VideoPlaybackFrameLease {
    std::weak_ptr<const void> owner;
    std::weak_ptr<const VideoPlaybackFrame> frame;
    VideoFileIdentity file;
    std::uint64_t requestGeneration{}, archiveGeneration{};
    std::uint32_t videoTrackId{};
};
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::int64_t kMaximumClock100ns = INT64_MAX / 100;
constexpr unsigned kEndSeekLimit = 8;
constexpr std::int64_t kPausedNeighborWindow100ns = 2'500'000; // existing 250-ms word proof window
std::int64_t AdvanceClock(std::int64_t base, std::int64_t elapsed) {
    base = std::clamp<std::int64_t>(base, 0, kMaximumClock100ns);
    return base + std::min(kMaximumClock100ns - base, std::max<std::int64_t>(0, elapsed));
}
std::atomic_flag nativeOwner = ATOMIC_FLAG_INIT;
struct PixelBudget {
    std::atomic<std::size_t> bytes{}, peak{}, limit{VideoPlayback::kMinimumDecodedByteLimit};
};
void ReservePixels(const std::shared_ptr<PixelBudget>& pool, std::size_t bytes) {
    auto used = pool->bytes.load();
    const auto limit=pool->limit.load();
    do { if (bytes > limit || used > limit - bytes)
        throw std::runtime_error("Decoded staging and retained scenes exceed the admitted frame pool.");
    } while (!pool->bytes.compare_exchange_weak(used, used + bytes));
    const auto total = used + bytes; auto peak = pool->peak.load();
    while (peak < total && !pool->peak.compare_exchange_weak(peak, total)) {}
}
enum class CommandKind { None, Open, Seek, End, Close };
struct Command {
    CommandKind kind{CommandKind::None}; std::uint64_t generation{};
    std::wstring path; std::shared_ptr<RecordingArchive> archive;
    std::int64_t target{}; Clock::time_point queued{};
    std::optional<LUID> gpuAdapter;
    bool forceCpu{};
    std::string backendReason;
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
    VideoReadStreamClone cloneRead;
    ComPtr<IMFMediaType> nativeMetadata;
    VideoFileIdentity file;
    std::shared_ptr<RecordingArchive> archive;
    RecordingArchiveSnapshot archiveAdmission;
    std::uint32_t width{}, height{}, stride{}, rotation{}, parN{1}, parD{1};
    std::uint32_t gpuComponentBits{};
    std::uint32_t videoStreamIndex{};
    std::uint32_t audioRate{}, audioChannels{};
    std::int64_t duration{};
    std::int64_t audioFloor100ns{};
    std::int64_t audioEnd100ns{};
    bool audioEos{};
    bool hasAudio{}, audioAvailable{}, eos{}, incomplete{};
    // Actual endpoint is discovered by reaching EOS, never duration - 1.
    std::int64_t lastDecodedPts{-1};
    std::int64_t decodeCost100ns{10'000};
    VideoPlaybackMemoryPlan memory;
    std::string backend{"media-foundation"}, backendReason;
    std::shared_ptr<VideoGpuDecoder> gpu;
    std::weak_ptr<const VideoPlaybackFrame> cachedSeekFrame;
    std::int64_t cachedSeekLowerBound{};
    bool cachedSeekEnded{},needsReposition{},resumeClockPending{};
    void Reset() {
        // The decoder drops a completed conversion sample before its reader;
        // an unknown-completion closure stays quarantined inside the decoder
        // (with its own reader reference), so releasing our refs below can
        // never free a sample or reader the GPU may still touch.
        if (gpu) gpu->DetachSourceReader();
        gpu.reset(); source.Reset(); nativeMetadata.Reset(); bytes.Reset(); cloneRead={}; archive.reset(); lastDecodedPts = -1; memory = {};
        cachedSeekFrame.reset();needsReposition=resumeClockPending=false;
        width = height = stride = rotation = audioRate = audioChannels = gpuComponentBits = 0;
        duration = 0; audioFloor100ns = audioEnd100ns = 0; audioEos = false;
        hasAudio = audioAvailable = eos = incomplete = false;
        parN = parD = 1; file = {}; archiveAdmission = {};
        backend="media-foundation";backendReason.clear();
    }
};
bool HasDecodeCredit(const Reader& r, const std::shared_ptr<PixelBudget>& pool) {
    if(r.gpu)return r.gpu->HasConversionCredit();
    const auto needed=r.memory.workingBytes;
    const auto limit=pool->limit.load();
    return needed && needed <= limit && pool->bytes.load() <= limit - needed;
}
}
struct VideoPlayback::State {
    mutable std::mutex mutex; std::condition_variable changed;
    std::atomic<bool> stop{}; std::atomic<std::uint64_t> generation{};
    Command pending;
    std::optional<LUID> gpuAdapter;
    bool cpuFallback{};
    std::string backendReason;
    VideoPlaybackSnapshot snapshot;
    std::deque<std::shared_ptr<const VideoPlaybackFrame>> ready;
    bool prepareNeighbor{};
    bool neighborMayReposition{}; // only the remainder of an explicit Open/Seek
    Clock::time_point neighborNotBefore{};
    std::deque<VideoPlaybackAudio> audio;
    std::size_t audioBytes{};
    std::uint64_t audioDuration100ns{};
    bool playing{}, muted{true}, restartAtCursor{};
    bool repositionOnResume{}; // paused display may be behind the decoder cursor
    Clock::time_point operationStart{}, clockStart{};
    std::int64_t clockPts{};
    std::int64_t restartCursor100ns{};
    std::int64_t audioClockPts{}, audioClockSample{};
    bool audioClockValid{};
    std::shared_ptr<PixelBudget> pixels{std::make_shared<PixelBudget>()};
    std::shared_ptr<const void> frameOwner{std::make_shared<const int>(0)};
    bool Current(std::uint64_t g) const { return !stop.load() && generation.load() == g; }
};
namespace {
void ClearAudio(VideoPlayback::State& s) { s.audio.clear(); s.audioBytes = 0; s.audioDuration100ns = 0; s.audioClockValid = false; }
void SealFrame(const std::shared_ptr<VideoPlaybackFrame>& frame, const VideoPlayback::State& s,
               const VideoFileIdentity& file) {
    auto lease = std::make_shared<VideoPlaybackFrameLease>();
    lease->owner = s.frameOwner; lease->frame = frame; lease->file = file;
    lease->requestGeneration = frame->requestGeneration;
    lease->archiveGeneration = frame->archiveGeneration; lease->videoTrackId = frame->videoTrackId;
    frame->decoderLease = std::move(lease);
}
bool IssuedFrame(const VideoPlayback::State& s, const std::shared_ptr<const VideoPlaybackFrame>& frame) {
    if (!frame || !frame->decoderLease || !s.Current(frame->requestGeneration)) return false;
    const auto& lease = *frame->decoderLease;
    const auto issued = lease.frame.lock();
    return lease.owner.lock() == s.frameOwner && issued == frame &&
        !issued.owner_before(frame) && !frame.owner_before(issued) &&
        lease.requestGeneration == frame->requestGeneration &&
        lease.archiveGeneration == frame->archiveGeneration && lease.videoTrackId == frame->videoTrackId &&
        lease.file.SameFile(s.snapshot.file) && frame->videoTrackId == s.snapshot.videoTrackId &&
        s.snapshot.frame && frame->archiveGeneration == s.snapshot.frame->archiveGeneration;
}
bool FollowingFrame(const VideoPlayback::State& s, const std::shared_ptr<const VideoPlaybackFrame>& frame,
                    const VideoPlaybackFrame& selected) {
    return IssuedFrame(s, frame) && frame->sampleId != selected.sampleId &&
        frame->pts100ns > selected.pts100ns &&
        frame->pts100ns - selected.pts100ns <= kPausedNeighborWindow100ns &&
        frame->width == selected.width && frame->height == selected.height &&
        frame->rotationDegrees == selected.rotationDegrees;
}
bool PauseAtFrameLocked(VideoPlayback::State& s, const std::shared_ptr<const VideoPlaybackFrame>& shown) {
    if (s.pending.kind != CommandKind::None || !IssuedFrame(s, shown) ||
        !VideoPlaybackTimestampFits(shown->pts100ns) ||
        shown->pts100ns > s.snapshot.frame->pts100ns ||
        (s.snapshot.state != VideoPlaybackState::Playing && s.snapshot.state != VideoPlaybackState::Waiting &&
         s.snapshot.state != VideoPlaybackState::Paused && s.snapshot.state != VideoPlaybackState::Ended)) return false;
    // Prefer the nearest already decoded future lease. It stays in the ready
    // queue: corroboration neither consumes playback nor creates image bytes.
    auto next = FollowingFrame(s, s.snapshot.neighborFrame, *shown) ? s.snapshot.neighborFrame
        : std::shared_ptr<const VideoPlaybackFrame>{};
    if (FollowingFrame(s, s.snapshot.frame, *shown) && (!next || s.snapshot.frame->pts100ns < next->pts100ns))
        next = s.snapshot.frame;
    for (const auto& ready : s.ready)
        if (FollowingFrame(s, ready, *shown) && (!next || ready->pts100ns < next->pts100ns)) next = ready;
    const bool rewound = shown != s.snapshot.frame;
    const bool atEnd = s.snapshot.state == VideoPlaybackState::Ended && !rewound;
    const bool pastWindow = rewound && s.snapshot.frame->pts100ns - shown->pts100ns > kPausedNeighborWindow100ns;
    s.playing = false; ClearAudio(s);
    s.restartAtCursor = false;
    s.repositionOnResume = s.repositionOnResume || rewound;
    s.snapshot.frame = shown;
    s.snapshot.gpuResident = bool(shown->gpu);
    s.snapshot.neighborFrame = std::move(next); // a predecessor never reaches paused OCR
    s.snapshot.state = atEnd ? VideoPlaybackState::Ended : VideoPlaybackState::Paused;
    s.clockPts = shown->pts100ns; s.clockStart = Clock::now();
    s.prepareNeighbor = !s.snapshot.neighborFrame && !atEnd && !pastWindow;
    s.neighborMayReposition = false; // Pause never authorizes a native seek
    s.neighborNotBefore = Clock::now() + std::chrono::milliseconds(50);
    return true;
}
void AdmitReaderMemory(Reader& r, VideoPlayback::State& s,std::uint64_t generation) {
    std::lock_guard lock(s.mutex);
    if(!s.Current(generation))return;
    // Old externally retained frames remain charged across source changes.
    // Keep enough headroom for the new working set without exceeding the hard cap.
    const auto used=s.pixels->bytes.load();
    const auto carried=used>VideoPlayback::kDecodedByteLimit-r.memory.workingBytes
        ? VideoPlayback::kDecodedByteLimit : used+r.memory.workingBytes;
    s.pixels->limit.store(std::max(r.memory.byteLimit,carried));
    s.snapshot.decodeWorkingBytes=r.memory.workingBytes;
    s.snapshot.lookaheadFrames=r.memory.lookaheadFrames;
}
bool ReadyEmpty(VideoPlayback::State& s) {
    std::lock_guard lock(s.mutex);return s.ready.empty();
}
bool WaitForDecodeCredit(Reader& r,VideoPlayback::State& s,std::uint64_t generation,
                         Clock::time_point deadline,std::shared_ptr<const VideoPlaybackFrame>& previous) {
    const auto credit=[&] {
        if(r.gpu)return r.gpu->HasConversionCredit();
        // A seek-local predecessor can be dropped AFTER a real replacement
        // sample arrives. Keep it across a no-sample EOS read, which needs
        // staging alone and must retain the actual last image.
        const auto reclaim=previous && previous.use_count()==1 && previous->bgra &&
            previous->bgra.use_count()==1?previous->bgra->size():0;
        const auto needed=std::max(r.memory.stagingBytes,r.memory.workingBytes-std::min(reclaim,r.memory.frameBytes));
        const auto limit=s.pixels->limit.load();
        return needed<=limit && s.pixels->bytes.load()<=limit-needed;
    };
    // A GPU predecessor lease can itself hold the conversion slot being
    // waited for. Drop that optional lease before waiting (only under actual
    // pressure), mirroring the CPU copy path's pre-allocation release;
    // waiting while holding it could never free credit.
    if(r.gpu && previous && previous->gpu && !credit())previous.reset();
    while(s.Current(generation) && !credit()) {
        std::unique_lock lock(s.mutex);
        s.snapshot.neighborFrame.reset(); // optional; external leases remain honestly charged
        s.snapshot.memoryPressure=true;
        if(Clock::now()>=deadline)return false;
        s.changed.wait_for(lock,std::chrono::milliseconds(5),[&]{return !s.Current(generation);});
    }
    return s.Current(generation);
}
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
    if(r.gpu) {
        if(r.nativeMetadata)for(const auto& key:{MF_MT_MINIMUM_DISPLAY_APERTURE,MF_MT_GEOMETRIC_APERTURE,
            MF_MT_PIXEL_ASPECT_RATIO,MF_MT_VIDEO_ROTATION,MF_MT_TRANSFER_FUNCTION,MF_MT_VIDEO_PRIMARIES,
            MF_MT_YUV_MATRIX,MF_MT_VIDEO_NOMINAL_RANGE,MF_MT_INTERLACE_MODE}) {
            PROPVARIANT value;PropVariantInit(&value);MF_ATTRIBUTE_TYPE kind{};
            UINT32 scalar{};
            const bool scalarUnknown=(key==MF_MT_YUV_MATRIX || key==MF_MT_VIDEO_NOMINAL_RANGE ||
                key==MF_MT_TRANSFER_FUNCTION || key==MF_MT_VIDEO_PRIMARIES || key==MF_MT_INTERLACE_MODE) &&
                SUCCEEDED(type->GetUINT32(key,&scalar)) && scalar==0;
            if((FAILED(type->GetItemType(key,&kind)) || scalarUnknown) &&
               SUCCEEDED(r.nativeMetadata->GetItem(key,&value)))type->SetItem(key,value);
            PropVariantClear(&value);
        }
        const auto format=r.gpu->SetFormat(type.Get());
        r.gpuComponentBits=format.subtype==MFVideoFormat_P010 || format.subtype==MFVideoFormat_Y410?10:
            format.subtype==MFVideoFormat_P016 || format.subtype==MFVideoFormat_Y416?16:8;
        r.width=format.squareWidth;r.height=format.height;r.stride=r.width*4;r.rotation=format.rotation;r.parN=r.parD=1;
        const auto plan=PlanVideoPlaybackMemory(r.width,r.height,r.stride,1,1,bool(r.archive));
        if(!plan)throw std::runtime_error("GPU playback working dimensions exceed admission.");
        r.memory=*plan;return;
    }
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
        throw std::runtime_error("Decoded video exceeds working-set admission or has an invalid layout.");
    r.stride = static_cast<std::uint32_t>(signedStride < 0 ? -std::int64_t(signedStride) : signedStride);
    type->GetUINT32(MF_MT_VIDEO_ROTATION, &r.rotation);
    if (r.rotation != 0 && r.rotation != 90 && r.rotation != 180 && r.rotation != 270)
        throw std::runtime_error("Unsupported video orientation metadata.");
    if (FAILED(MFGetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &r.parN, &r.parD)) || !r.parN || !r.parD)
        r.parN = r.parD = 1;
    const auto memory=PlanVideoPlaybackMemory(r.width,r.height,r.stride,r.parN,r.parD,bool(r.archive));
    if(!memory)throw std::runtime_error("The decoded working set exceeds the playback memory ceiling.");
    r.memory=*memory;
}
bool IsHighPrecisionFormat(const GUID& format) {
    return format == MFVideoFormat_P010 || format == MFVideoFormat_P016 ||
           format == MFVideoFormat_Y410 || format == MFVideoFormat_Y416;
}
// Coded precision proven by the container's standardized codec-config
// record inside the native type's sample description, when present and
// well-formed. Unknown stays unknown: negotiation then follows the
// installed decoder's own preference order instead of guessing.
std::optional<unsigned> CodedBitsPerComponent(IMFMediaType* native, const GUID& coded) {
    UINT32 blobSize{};
    std::vector<std::uint8_t> blob;
    if (SUCCEEDED(native->GetBlobSize(MF_MT_MPEG4_SAMPLE_DESCRIPTION, &blobSize)) &&
        blobSize && blobSize <= 64u * 1024) {
        blob.resize(blobSize); UINT32 actual{};
        if (FAILED(native->GetBlob(MF_MT_MPEG4_SAMPLE_DESCRIPTION, blob.data(), blobSize, &actual)))
            blob.clear();
        else blob.resize(actual);
    }
    const auto record = [&](const char* fourcc) -> const std::uint8_t* {
        for (std::size_t i = 0; i + 4 <= blob.size(); ++i)
            if (std::memcmp(blob.data() + i, fourcc, 4) == 0) return blob.data() + i + 4;
        return nullptr;
    };
    const auto remaining = [&](const std::uint8_t* at) -> std::size_t {
        return at ? std::size_t(blob.data() + blob.size() - at) : 0;
    };
    if (coded == MFVideoFormat_AV1) {
        // AV1CodecConfigurationRecord: marker|version, seq_profile/level,
        // then tier, high_bitdepth, twelve_bit flag bits.
        const auto* r = record("av1C");
        if (r && remaining(r) >= 4 && (r[0] & 0x80) && (r[0] & 0x7f) == 1)
            return (r[2] & 0x20) ? 12u : ((r[2] & 0x40) ? 10u : 8u);
    } else if (coded == MFVideoFormat_HEVC) {
        // HEVCDecoderConfigurationRecord byte 17 low bits: bitDepthLumaMinus8.
        const auto* r = record("hvcC");
        if (r && remaining(r) >= 19 && r[0] == 1) return 8u + (r[17] & 0x7u);
        const auto profile = MFGetAttributeUINT32(native, MF_MT_MPEG2_PROFILE, 0);
        if (profile == 1) return 8u;         // Main
        if (profile == 2) return 10u;        // Main 10
    } else if (coded == MFVideoFormat_VP90) {
        // VPCodecConfigurationRecord full box: version, flags, profile,
        // level, then bitDepth in the high nibble.
        const auto* r = record("vpcC");
        if (r && remaining(r) >= 7 && r[0] <= 1 && r[4] <= 3) {
            const unsigned bits = r[6] >> 4;
            if (bits == 8 || bits == 10 || bits == 12) return bits;
        }
    }
    return std::nullopt;
}
void ConfigureReader(Reader& r, const Command& cmd, VideoPlayback::State& state) {
    if (cmd.archive) {
        r.archive = cmd.archive;
        if(!r.bytes)Check(cmd.archive->OpenReadStream(&r.bytes, &r.archiveAdmission,&r.cloneRead), "Active recording is not yet safely readable");
        r.file = r.archiveAdmission.file; r.duration = r.archiveAdmission.duration100ns;
        r.incomplete = r.archiveAdmission.finalized && !r.archiveAdmission.complete;
    } else if(!r.bytes)Check(OpenStableVideoReadStream(cmd.path, &r.bytes, &r.file,&r.cloneRead), "Open a stable read-only video handle");
    ComPtr<IMFAttributes> attrs; Check(MFCreateAttributes(&attrs, 4), "Create playback attributes");
    if(detail::FfmpegPlaybackRequested()) {
        if(cmd.archive)r.backendReason="Recording archives use the Media Foundation backend.";
        else {
            const auto gen=state.generation.load();
            const auto result=detail::CreateFfmpegVideoReader(r.bytes.Get(),r.cloneRead,
                [&state,gen]{return state.Current(gen);},cmd.queued+std::chrono::seconds(2),&r.source,r.backendReason);
            if(!state.Current(gen))throw std::runtime_error("Embedded open was superseded.");
            if(SUCCEEDED(result))r.backend="ffmpeg-dav1d";
            else {
                // A failed probe never reopens the path. Native fallback gets
                // an independent cursor on the identical held file backing.
                ComPtr<IMFByteStream> fallback;
                Check(r.cloneRead(&fallback),"Clone held stream after embedded decoder refusal");
                r.bytes=std::move(fallback);
            }
        }
    }
    if(!r.source && cmd.gpuAdapter && !cmd.forceCpu) {
        r.gpu=VideoGpuDecoder::Create(*cmd.gpuAdapter);r.gpu->ConfigureReader(attrs.Get());
    } else {
        Check(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "Enable native BGRA conversion");
        Check(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE), "Select bounded CPU decode output");
    }
    if(!r.source)Check(MFCreateSourceReaderFromByteStream(r.bytes.Get(), attrs.Get(), &r.source), "Open native video decoder");
    if(r.gpu)r.gpu->AttachSourceReader(r.source.Get());
    Check(r.source->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE), "Select playback tracks");
    Check(r.source->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE), "Select video track");
    ComPtr<IMFMediaType> nativeVideo;
    Check(r.source->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeVideo), "Probe coded video format");
    r.nativeMetadata=nativeVideo;
    UINT32 codedWidth{}, codedHeight{};
    Check(MFGetAttributeSize(nativeVideo.Get(), MF_MT_FRAME_SIZE, &codedWidth, &codedHeight), "Probe coded video dimensions");
    if (!VideoPlaybackLayoutFits(codedWidth, codedHeight, std::int64_t(codedWidth) * 4))
        throw std::runtime_error("Coded video dimensions exceed playback admission limits.");
    ComPtr<IMFMediaType> output; Check(MFCreateMediaType(&output), "Create decoded video type");
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if(r.gpu) {
        // Negotiate formats, not a codec-name allowlist. An installed
        // decoder may accept a partial type it merely tolerates and then
        // stall inside the first native read (observed when forcing a
        // 10-bit output type for an 8-bit stream whose decoder lists that
        // type last), so attempt order follows evidence instead of a fixed
        // precision-first list: proven coded precision first, then the
        // decoder's own offered-type preference, then the previous
        // opportunistic order when neither is known. High-bit-depth and
        // packed output remain eligible in every case.
        bool selected{};
        GUID codedSubtype{};nativeVideo->GetGUID(MF_MT_SUBTYPE,&codedSubtype);
        const auto profile=MFGetAttributeUINT32(nativeVideo.Get(),MF_MT_MPEG2_PROFILE,0);
        // H.264 baseline/main/high are 8-bit.
        const bool knownEightBitAvc=codedSubtype==MFVideoFormat_H264 && profile<110;
        auto provenBits=CodedBitsPerComponent(nativeVideo.Get(),codedSubtype);
        if(!provenBits && knownEightBitAvc)provenBits=8u;
        const std::array<GUID,9> highPrecision{MFVideoFormat_P010,MFVideoFormat_P016,MFVideoFormat_Y410,
            MFVideoFormat_Y416,MFVideoFormat_NV12,MFVideoFormat_YUY2,MFVideoFormat_AYUV,MFVideoFormat_ARGB32,MFVideoFormat_RGB32};
        const std::array<GUID,9> eightBit{MFVideoFormat_NV12,MFVideoFormat_P010,MFVideoFormat_P016,MFVideoFormat_YUY2,
            MFVideoFormat_AYUV,MFVideoFormat_Y410,MFVideoFormat_Y416,MFVideoFormat_ARGB32,MFVideoFormat_RGB32};
        const auto offered=r.gpu->DecoderPreferredOutputs(nativeVideo.Get());
        std::vector<GUID> offeredSupported;
        for(const auto& format:offered)
            if(std::find(highPrecision.begin(),highPrecision.end(),format)!=highPrecision.end() &&
               std::find(offeredSupported.begin(),offeredSupported.end(),format)==offeredSupported.end())
                offeredSupported.push_back(format);
        std::vector<GUID> order;
        const auto append=[&](const GUID& format) {
            if(std::find(order.begin(),order.end(),format)==order.end())order.push_back(format);
        };
        if(provenBits && *provenBits>8) {
            for(const auto& format:offeredSupported)if(IsHighPrecisionFormat(format))append(format);
            for(const auto& format:highPrecision)if(IsHighPrecisionFormat(format))append(format);
            for(const auto& format:offeredSupported)append(format);
            for(const auto& format:highPrecision)append(format);
        } else if(provenBits) {
            for(const auto& format:offeredSupported)if(!IsHighPrecisionFormat(format))append(format);
            for(const auto& format:highPrecision)if(!IsHighPrecisionFormat(format))append(format);
            for(const auto& format:offeredSupported)append(format);
            for(const auto& format:highPrecision)append(format);
        } else if(!offeredSupported.empty()) {
            for(const auto& format:offeredSupported)append(format);
            for(const auto& format:highPrecision)if(!IsHighPrecisionFormat(format))append(format);
            for(const auto& format:highPrecision)append(format);
        } else {
            for(const auto& format:knownEightBitAvc?eightBit:highPrecision)append(format);
        }
        for(const auto& format:order) {
            output->SetGUID(MF_MT_SUBTYPE,format);
            if(SUCCEEDED(r.source->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,output.Get()))) {selected=true;break;}
        }
        if(!selected)throw VideoGpuIncompatible("No installed GPU-compatible decoder output type was accepted.");
    } else {
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        Check(r.source->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output.Get()), "Request BGRA video");
    }
    ConfigureVideo(r);
    if (nativeVideo && !r.gpu) {
        UINT32 rotation{}; UINT32 numerator{}, denominator{};
        if (SUCCEEDED(nativeVideo->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation)) &&
            (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270)) r.rotation = rotation;
        if (SUCCEEDED(MFGetAttributeRatio(nativeVideo.Get(), MF_MT_PIXEL_ASPECT_RATIO, &numerator, &denominator)) &&
            numerator && denominator) { r.parN = numerator; r.parD = denominator; }
    }
    const auto memory=PlanVideoPlaybackMemory(r.width,r.height,r.stride,r.parN,r.parD,bool(r.archive));
    if(!memory)throw std::runtime_error("The square-pixel working set exceeds the playback memory ceiling.");
    r.memory=*memory;
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
std::shared_ptr<std::vector<std::uint8_t>> AllocatePixels(std::size_t bytes,const std::shared_ptr<PixelBudget>& budget) {
    auto owned=std::make_unique<std::vector<std::uint8_t>>();
    ReservePixels(budget,bytes);
    std::shared_ptr<std::vector<std::uint8_t>> pixels{owned.release(),[budget,bytes](std::vector<std::uint8_t>* data){
        delete data;budget->bytes.fetch_sub(bytes);}};
    pixels->resize(bytes);
    return pixels;
}
std::shared_ptr<VideoPlaybackFrame> CopyFrame(Reader& r, IMFSample* sample,
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
    auto frame=std::make_shared<VideoPlaybackFrame>();
    const auto pixels=AllocatePixels(bytes,budget);
    frame->bgra=pixels; frame->width = squareWidth; frame->height = r.height;
    frame->strideBytes = squareWidth * 4; frame->rotationDegrees = r.rotation;
    frame->pixelAspectNumerator = frame->pixelAspectDenominator = 1;
    const auto copyRow = [&](std::uint32_t y, const BYTE* source) {
        auto* destination = pixels->data() + std::size_t(y) * frame->strideBytes;
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
               std::shared_ptr<const VideoPlaybackFrame>& frame, Clock::time_point deadline,
               std::shared_ptr<const VideoPlaybackFrame>* optionalPrevious = nullptr,
               std::int64_t minimumCopiedPts = -1) {
    if(!state.Current(gen))return false;
    // Never start a new native read on a reader whose previous MF sample is
    // still retained by an unsafe conversion; fall back through the typed
    // error instead of risking the decoder pool.
    if(r.gpu && r.gpu->HasUnsafePending())
        throw VideoGpuIncompatible("GPU playback conversion is retained with unknown completion; using CPU playback.");
    const auto started=Clock::now();
    const auto staging = r.gpu?0:r.memory.stagingBytes;
    ReservePixels(state.pixels, staging);
    struct StagingLease {
        std::shared_ptr<PixelBudget> pool; std::size_t bytes;
        ~StagingLease() { pool->bytes.fetch_sub(bytes); }
    } lease{state.pixels, staging};
    DWORD stream{}, flags{}; LONGLONG time{}; ComPtr<IMFSample> sample;
    detail::ArmFfmpegVideoReader(r.source.Get(),[&state,gen]{return state.Current(gen);},deadline,minimumCopiedPts);
    const auto readHr = r.source->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        &stream, &flags, &time, &sample);
    if (FAILED(readHr)) {
        if(r.archive && !r.archive->ValidateRange(r.archiveAdmission.generation,0,r.archiveAdmission.verifiedMediaBytes))
            throw std::runtime_error("Recording bytes changed during native decode; seek again.");
        if(r.gpu)throw VideoGpuIncompatible(Error("Decode GPU video sample",readHr));
        throw DecodeFailure(Error("Decode video sample",readHr)+detail::FfmpegVideoReaderError(r.source.Get()));
    }
    if (!state.Current(gen)) return false;
    r.videoStreamIndex = stream;
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {ConfigureVideo(r);AdmitReaderMemory(r,state,gen);}
    if (!r.gpu && std::uint64_t(r.stride) * r.height > staging)
        throw std::runtime_error("Video format changed beyond the admitted decoded staging size.");
    if (flags & MF_SOURCE_READERF_ERROR) {
        // Same classified GPU fault as a failed ReadSample HRESULT above: a
        // decoder stream error on the GPU reader latches CPU compatibility
        // for this document instead of ending it as generically incomplete.
        if (r.archive && !r.archive->ValidateRange(r.archiveAdmission.generation,0,r.archiveAdmission.verifiedMediaBytes))
            throw std::runtime_error("Recording bytes changed during native decode; seek again.");
        if (r.gpu) throw VideoGpuIncompatible("Native GPU video decoder reported a stream error; using CPU playback.");
        throw DecodeFailure("Native video source reported a decoder error.");
    }
    if(sample)r.lastDecodedPts=time;
    // Seek preroll still goes through the native codec (its references are
    // required), but discarded frames need no owned BGRA copy or alpha sweep.
    if (sample && (minimumCopiedPts<0 || time>=minimumCopiedPts)) {
        if(r.gpu) {
            if(!VideoPlaybackTimestampFits(time))throw std::runtime_error("GPU playback returned an invalid timestamp.");
            auto owned=std::make_shared<VideoPlaybackFrame>();
            // Mirror the CPU branch: a superseded optional predecessor must
            // not hold the conversion credit this real replacement needs.
            if(optionalPrevious && *optionalPrevious && (*optionalPrevious)->gpu &&
               !r.gpu->HasConversionCredit())optionalPrevious->reset();
            owned->gpu=r.gpu->Convert(sample.Get(),[&]{return state.Current(gen);},deadline);
            if(!owned->gpu || !state.Current(gen))return false;
            owned->width=r.width;owned->height=r.height;owned->strideBytes=r.width*4;
            owned->rotationDegrees=r.rotation;owned->pts100ns=time;sample->GetSampleDuration(&owned->duration100ns);
            if(!VideoPlaybackTimestampFits(time,owned->duration100ns))throw std::runtime_error("GPU playback returned an invalid duration.");
            owned->requestGeneration=gen;owned->archiveGeneration=r.archiveAdmission.generation;
            owned->sampleId=std::uint64_t(time);owned->videoTrackId=r.archive?r.archiveAdmission.videoTrackId:r.videoStreamIndex;
            if(r.archive && !r.archive->ValidateRange(r.archiveAdmission.generation,0,r.archiveAdmission.verifiedMediaBytes))
                throw std::runtime_error("Recording bytes changed during GPU decode; seek again.");
            SealFrame(owned, state, r.file);
            frame=std::move(owned);
        } else {
        const auto squareWidth = (std::uint64_t(r.width) * r.parN + r.parD / 2) / r.parD;
        const auto destination = squareWidth * r.height * 4;
        const auto limit=state.pixels->limit.load();
        if (optionalPrevious && (destination > limit ||
            state.pixels->bytes.load() > limit - destination)) optionalPrevious->reset();
        auto copied = CopyFrame(r, sample.Get(), time, gen, state.pixels);
        SealFrame(copied, state, r.file);
        frame = std::move(copied); r.lastDecodedPts = frame->pts100ns;
        }
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) r.eos = true;
    const auto cost=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started).count()/100;
    r.decodeCost100ns=std::max<std::int64_t>(cost,(r.decodeCost100ns*3+cost)/4);
    return sample != nullptr;
}
void SeekReader(Reader& r, std::int64_t target,VideoPlayback::State& state,std::uint64_t gen) {
    // No new native seek on a reader whose sample is retained by an unsafe
    // conversion; the typed error routes to a fresh CPU reader instead.
    if(r.gpu && r.gpu->HasUnsafePending())
        throw VideoGpuIncompatible("GPU playback conversion is retained with unknown completion; using CPU playback.");
    if (!VideoPlaybackTimestampFits(target)) throw std::runtime_error("Seek target exceeds the supported host-clock range.");
    detail::ArmFfmpegVideoReader(r.source.Get(),[&state,gen]{return state.Current(gen);},Clock::now()+std::chrono::seconds(2));
    PROPVARIANT position; PropVariantInit(&position); position.vt = VT_I8; position.hVal.QuadPart = std::max<std::int64_t>(0, target);
    Check(r.source->SetCurrentPosition(GUID_NULL, position), "Seek native video source"); r.eos = false; r.lastDecodedPts = -1;
    r.audioFloor100ns = std::max<std::int64_t>(0, target);
    r.audioEnd100ns = r.audioFloor100ns; r.audioEos = false;
    r.resumeClockPending = false;
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
        // The UI transfers queued PCM to a separate bounded output sink.
        // TakeAudio empties our queue, not the media already handed off.
        // Keep that decoded high-water so video-wait polling cannot feed
        // future audio faster than its consumed/host playback clock.
        if (r.audioEnd100ns >= until) return;
        { std::lock_guard lock(s.mutex);
          if (s.muted || !s.playing || s.audio.size() >= 32 || s.audioBytes >= VideoPlayback::kAudioByteLimit ||
              s.audioDuration100ns >= 5'000'000 ||
              (!s.audio.empty() && s.audio.back().pts100ns > until + 5'000'000)) return;
        }
        DWORD stream{}, flags{}; LONGLONG time{}; ComPtr<IMFSample> sample;
        detail::ArmFfmpegVideoReader(r.source.Get(),[&s,gen]{return s.Current(gen);},deadline);
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
             std::shared_ptr<const VideoPlaybackFrame> neighbor = {},unsigned reads = 0,unsigned copies = 0,bool reused=false) {
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
    s.snapshot.gpuResident=bool(s.snapshot.frame && s.snapshot.frame->gpu);
    s.snapshot.gpuDecodedBitsPerComponent=r.gpu?r.gpuComponentBits:0;
    s.snapshot.decoderBackend=r.backend;
    s.snapshot.backendReason=r.backendReason.empty()?s.backendReason:
        r.backendReason+(s.backendReason.empty()?"":" "+s.backendReason);
    s.snapshot.memoryPressure=false;
    s.snapshot.lastRequestReadCount=reads;
    s.snapshot.lastRequestCopiedFrames=copies;
    s.snapshot.lastRequestReusedFrame=reused;
    s.snapshot.lastRequestLatencyUs=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-cmd.queued).count();
    s.prepareNeighbor=status==VideoPlaybackState::Paused && !s.snapshot.neighborFrame && !r.eos;
    s.neighborMayReposition = true; // deferred work of this explicit Open/Seek
    s.neighborNotBefore=Clock::now()+std::chrono::milliseconds(50);
    s.operationStart = {};
    if (s.snapshot.frame) { s.clockPts = s.snapshot.frame->pts100ns; s.clockStart = Clock::now(); }
}
// Same-worker, same-generation CPU fallback for a genuine GPU fault found
// outside an open/seek command (playback, neighbor preparation, archive
// refresh). Requeues a seek at the externally held cursor; the command path
// rebuilds the reader on the same stable byte-stream handle and identity.
// The old GPU reader is only detached there: if its conversion completion is
// unknown, the decoder quarantine retains that exact reader/sample untouched
// while the fresh CPU reader proceeds; it is never seeked or read again.
void QueueCpuFallback(VideoPlayback::State& s, Command document, std::int64_t lastDecodedPts,
                      const std::string& reason) {
    std::lock_guard lock(s.mutex);
    if (!s.Current(document.generation) || s.cpuFallback || document.forceCpu) return;
    s.cpuFallback = true; s.backendReason = reason;
    document.kind = CommandKind::Seek; document.forceCpu = true; document.gpuAdapter.reset();
    document.backendReason = reason;
    document.target = s.snapshot.frame ? s.snapshot.frame->pts100ns
                                       : std::max<std::int64_t>(0, lastDecodedPts);
    document.queued = Clock::now();
    s.playing = false; ClearAudio(s); s.ready.clear();
    s.prepareNeighbor = false; s.restartAtCursor = false;
    s.snapshot.state = VideoPlaybackState::Seeking; s.snapshot.error.clear();
    s.operationStart = document.queued;
    s.pending = std::move(document); s.changed.notify_one();
}
void Worker(std::shared_ptr<VideoPlayback::State> s) {
    bool permit{}; bool com{}; bool mf{}; Reader reader; Command document;
    try {
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Check(hr, "Initialize playback COM worker"); com = true;
        Check(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Initialize playback Media Foundation"); mf = true;
        while (!s->stop.load()) {
            Command command; bool play{},prepareNeighbor{};
            { std::unique_lock lock(s->mutex);
              s->changed.wait(lock,[&] { return s->stop.load() || s->pending.kind != CommandKind::None || s->playing || s->prepareNeighbor; });
              if (s->stop.load()) break;
              command = std::move(s->pending); s->pending = {}; play = s->playing;
              prepareNeighbor=s->prepareNeighbor;s->prepareNeighbor=false;
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
                        ConfigureReader(reader, command,*s); AdmitReaderMemory(reader,*s,command.generation); document = command;
                    } else {
                        if((command.forceCpu && reader.gpu) || (!reader.source && command.forceCpu)) {
                            // Keep the stable stream handle and identity across
                            // fallback. Re-opening a path could select new bytes.
                            auto clone=reader.cloneRead;const auto identity=reader.file;
                            const auto admission=reader.archiveAdmission;const auto archive=reader.archive;
                            // Even an unsafe reader can retain native prefetch.
                            // Share the exact held file backing, never its cursor.
                            ComPtr<IMFByteStream> bytes;
                            if(!clone)throw std::runtime_error("No independent held-file cursor is available for CPU fallback.");
                            Check(clone(&bytes),"Clone the held video stream for CPU fallback");
                            reader.Reset();reader.bytes=bytes;reader.cloneRead=std::move(clone);reader.file=identity;
                            reader.archiveAdmission=admission;reader.archive=archive;
                            document.forceCpu=true;document.gpuAdapter.reset();
                            ConfigureReader(reader,document,*s);AdmitReaderMemory(reader,*s,command.generation);
                        }
                        if (!reader.source) throw std::runtime_error("No readable video is selected.");
                        document.generation = command.generation;
                        if (document.archive) {
                            const auto now = document.archive->Snapshot();
                            if (now.generation != reader.archiveAdmission.generation ||
                                now.verifiedMediaBytes != reader.archiveAdmission.verifiedMediaBytes) {
                                reader.Reset();
                                if (!s->Current(command.generation)) continue;
                                ConfigureReader(reader, document,*s); AdmitReaderMemory(reader,*s,command.generation);
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
                    // A prior exact seek proves that every requested position
                    // between its request and selected PTS resolves to this same
                    // frame. Weak retention adds no history/cache allocations.
                    // Share the exact immutable pixels, reticket provenance, and
                    // reposition the native tracks before subsequent playback.
                    if(command.kind==CommandKind::Seek)if(const auto cached=reader.cachedSeekFrame.lock();
                       cached && target>=reader.cachedSeekLowerBound && target<=cached->pts100ns) {
                        auto selected=std::make_shared<VideoPlaybackFrame>(*cached);
                        selected->requestGeneration=command.generation;
                        SealFrame(selected, *s, reader.file);
                        if(!s->Current(command.generation))continue;
                        reader.cachedSeekFrame=selected;reader.needsReposition=true;
                        reader.eos=reader.cachedSeekEnded;reader.lastDecodedPts=selected->pts100ns;
                        document.generation=command.generation;document.queued=command.queued;
                        Publish(*s,reader,document,selected,reader.cachedSeekEnded?VideoPlaybackState::Ended:VideoPlaybackState::Paused,{},0,0,true);
                        continue;
                    }
                    // A stale generation may have left an unsafe pending
                    // conversion on this reader; no native call may touch it.
                    if(reader.gpu && reader.gpu->HasUnsafePending())
                        throw VideoGpuIncompatible("GPU playback conversion is retained with unknown completion; using CPU playback.");
                    // Every command repositions audio and video together; no
                    // hidden audio cursor can continue through a scrub.
                    if (reader.hasAudio) reader.source->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
                    unsigned seeks{};
                    if (command.kind != CommandKind::Open) { SeekReader(reader, target,*s,command.generation); ++seeks; }
                    detail::VideoPlaybackSeekBudget budget(command.queued);
                    if (budget.Expired(Clock::now())) throw std::runtime_error("Native open/seek exhausted its progress deadline.");
                    std::shared_ptr<const VideoPlaybackFrame> chosen, neighbor;
                    unsigned samples{}, decoded{},copies{};
                    while (samples < VideoPlayback::kSeekSampleLimit && !budget.Expired(Clock::now()) && s->Current(command.generation)) {
                        const auto deadline = budget.Deadline();
                        // Keep the predecessor across a sample-less EOS read
                        // when credit permits; under pressure it stays optional.
                        if(!HasDecodeCredit(reader,s->pixels))neighbor.reset();
                        if(!WaitForDecodeCredit(reader,*s,command.generation,deadline,chosen))break;
                        ++samples;
                        // Keep the last decoded frame through an EOS read.
                        // A real replacement sample may release this optional
                        // lease before its owned pixel copy is allocated.
                        std::shared_ptr<const VideoPlaybackFrame> next;
                        bool readSample{};
                        try { if((readSample=ReadVideo(reader,*s,command.generation,next,deadline,&chosen,end?-1:target)))++decoded; }
                        catch (const DecodeFailure&) {
                            if (!end || !chosen) throw;
                            reader.eos = true; reader.incomplete = true; break;
                        }
                        if (readSample && budget.Observe(reader.lastDecodedPts,Clock::now()))
                            MarkNativeOperation(*s,command.generation,true);
                        if (next) {++copies;neighbor = std::move(chosen); chosen = std::move(next); }
                        if (end && reader.eos && !chosen && target > 0 && seeks < kEndSeekLimit) {
                            // Only observed video EOS without a frame permits
                            // an earlier anchor. All attempts share the same
                            // read/deadline budget; the last seek reaches zero
                            // to cover a sparse video followed by long audio.
                            endLookback = std::min(reader.duration, std::max<std::int64_t>(1'000'000, endLookback * 2));
                            auto earlier = seeks + 1 == kEndSeekLimit ? 0 : reader.duration - endLookback;
                            if (earlier >= target) earlier = target / 2;
                            target = earlier; SeekReader(reader, target,*s,command.generation); ++seeks;
                            continue;
                        }
                        if (reader.eos || (!end && chosen && chosen->pts100ns >= target)) break;
                    }
                    if (!s->Current(command.generation)) continue;
                    if (budget.Expired(Clock::now()) || !chosen || (end && !reader.eos) || (!end && chosen->pts100ns < target))
                        throw std::runtime_error("This seek exceeds the " + std::to_string(VideoPlayback::kSeekSampleLimit) + "-read / two-second progress / sixty-second request budget: reads=" +
                            std::to_string(samples) + ", seeks=" + std::to_string(seeks) + ", decoded=" + std::to_string(decoded) +
                            ", eos=" + std::to_string(reader.eos) + ", chosen=" + std::to_string(bool(chosen)) +
                            ", lastPts=" + std::to_string(reader.lastDecodedPts) + ", anchor=" + std::to_string(target) +
                            ", duration=" + std::to_string(reader.duration) + ", elapsedMs=" +
                            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - command.queued).count()) + ".");
                    if (neighbor && neighbor->pts100ns == chosen->pts100ns) neighbor.reset();
                    if (budget.Expired(Clock::now())) throw std::runtime_error("Native playback returned after its progress deadline.");
                    // End is exact only after EOS. Failed seeks leave the
                    // externally held image intact.
                    { std::lock_guard lock(s->mutex); s->playing = false; ClearAudio(*s); }
                    document.generation = command.generation;
                    document.queued=command.queued;
                    reader.cachedSeekFrame=chosen;
                    // End scans intentionally pass earlier samples; its anchor
                    // is not a proof that all positions resolve to the last one.
                    reader.cachedSeekLowerBound=end?chosen->pts100ns:target;
                    reader.cachedSeekEnded=end || reader.eos;reader.needsReposition=false;
                    Publish(*s,reader,document,chosen,end || reader.eos?VideoPlaybackState::Ended:VideoPlaybackState::Paused,neighbor,samples,copies);
                } catch (const VideoGpuIncompatible& error) {
                    // Only a genuine GPU device/format/conversion fault
                    // latches this document to CPU. Ordinary failures below
                    // (revoked archive bytes, the seek read/deadline budget,
                    // texture-credit backpressure, stale generations) must
                    // not discard a healthy GPU path.
                    std::lock_guard lock(s->mutex);
                    if(s->Current(command.generation) && command.gpuAdapter && !command.forceCpu) {
                        // A failed first hardware attempt never publishes a
                        // pretend GPU frame. The same worker, generation and
                        // stable handle perform the bounded CPU retry.
                        command.forceCpu=true;command.backendReason=error.what();
                        s->cpuFallback=true;s->backendReason=error.what();
                        if(command.kind==CommandKind::Open) {document=command;command.kind=CommandKind::Seek;}
                        // The retry is a new CPU pipeline, not more time for
                        // the failed GPU attempt: it gets its own request-age
                        // budget (mirroring QueueCpuFallback). A GPU fault
                        // surfacing at the deadline -- conversion-drain
                        // expiry by construction -- would otherwise strand
                        // the healthy CPU path as expired/Unavailable.
                        command.queued=Clock::now();s->operationStart=command.queued;
                        s->pending=std::move(command);s->changed.notify_one();continue;
                    }
                    if (s->Current(command.generation)) { s->snapshot.state = VideoPlaybackState::Unavailable;
                        s->snapshot.requestGeneration = command.generation; s->snapshot.error = error.what(); s->playing = false; ClearAudio(*s); s->operationStart = {}; }
                } catch (const std::exception& error) {
                    std::lock_guard lock(s->mutex);
                    if (s->Current(command.generation)) { s->snapshot.state = VideoPlaybackState::Unavailable;
                        s->snapshot.requestGeneration = command.generation; s->snapshot.error = error.what(); s->playing = false; ClearAudio(*s); s->operationStart = {}; }
                }
                continue;
            }
            // A seek publishes its exact target before this optional native
            // read. New commands always take priority at the next boundary.
            if(!play && prepareNeighbor && reader.source && s->Current(document.generation)) {
                try {
                    std::shared_ptr<const VideoPlaybackFrame> selected;
                    bool mayReposition{};
                    {std::unique_lock lock(s->mutex);
                     if(!s->Current(document.generation) || !s->snapshot.frame || s->playing ||
                        s->pending.kind!=CommandKind::None)continue;
                     if(Clock::now()<s->neighborNotBefore) {
                         s->prepareNeighbor=true;
                         s->changed.wait_until(lock,s->neighborNotBefore,[&]{return s->stop.load() || s->pending.kind!=CommandKind::None || s->playing;});
                         continue;
                     }
                     selected=s->snapshot.frame;
                     mayReposition=s->neighborMayReposition;
                     // An in-flight playback read may have filled the ready
                     // queue after Pause. Reuse its nearest true future frame.
                     std::shared_ptr<const VideoPlaybackFrame> readyNeighbor;
                     for (const auto& ready : s->ready)
                         if (FollowingFrame(*s, ready, *selected) &&
                             (!readyNeighbor || ready->pts100ns < readyNeighbor->pts100ns)) readyNeighbor = ready;
                     if (readyNeighbor) { s->snapshot.neighborFrame = std::move(readyNeighbor); continue; }
                     // A queued sparse/VFR future outside the proof window
                     // cannot corroborate this selected frame. Reading past
                     // it adds no proof and must not discard playback samples
                     // when the bounded ready queue is already full.
                     if (s->ready.size()>=reader.memory.lookaheadFrames ||
                         std::any_of(s->ready.begin(),s->ready.end(),[&](const auto& ready) {
                             return ready && ready->pts100ns>selected->pts100ns;
                         }))continue;}
                    const auto selectedPts = selected->pts100ns;
                    const auto preempted=[&] {std::lock_guard lock(s->mutex);
                        return !s->Current(document.generation) || s->playing || s->pending.kind!=CommandKind::None ||
                            s->snapshot.frame != selected || s->ready.size()>=reader.memory.lookaheadFrames;};
                    if(preempted())continue;
                    // Pause itself never seeks the decoder. Deferred cursor
                    // realignment belongs only to an explicit cached Seek.
                    if (reader.eos || (reader.needsReposition && !mayReposition)) continue;
                    const auto deadline=Clock::now()+std::chrono::seconds(2);
                    bool touched{};
                    if (reader.needsReposition) {
                        touched=true; SeekReader(reader,selectedPts,*s,document.generation); reader.needsReposition=false;
                    }
                    std::shared_ptr<const VideoPlaybackFrame> next;
                    for(unsigned reads=0;reads<VideoPlayback::kSeekSampleLimit && !reader.eos &&
                        Clock::now()<deadline && s->Current(document.generation) && HasDecodeCredit(reader,s->pixels);++reads) {
                        if(preempted())break;
                        touched=true;
                        ReadVideo(reader,*s,document.generation,next,deadline,nullptr,selectedPts+1);
                        if(next)break;
                    }
                    std::lock_guard lock(s->mutex);
                    if(s->playing || s->pending.kind!=CommandKind::None || s->snapshot.frame != selected) {
                        if(touched)reader.needsReposition=true;continue;
                    }
                    if(next && s->Current(document.generation) &&
                       s->ready.size()<reader.memory.lookaheadFrames) {
                        // Keep playback order even if a sparse next frame lies
                        // outside the proof window; it is never shown by Pause.
                        s->ready.push_back(next);
                        if (FollowingFrame(*s, next, *selected)) s->snapshot.neighborFrame=std::move(next);
                    }
                } catch(const VideoGpuIncompatible& error) {
                    // The completed seek's frame stays published; rebuild on
                    // CPU at that held cursor rather than leaving an unsafe
                    // or incompatible GPU reader for the next play/seek.
                    reader.needsReposition=true;
                    QueueCpuFallback(*s,document,reader.lastDecodedPts,error.what());
                } catch(const std::exception&) {reader.needsReposition=true; /* Completed seek remains valid. */ }
                continue;
            }
            if (!play || !reader.source || !s->Current(document.generation)) {
                if(play && !reader.source) {std::lock_guard lock(s->mutex);s->playing=false;}
                continue;
            }
            {std::lock_guard lock(s->mutex);
             if(!s->Current(document.generation) || !s->playing)continue;
             // A real Resume may restore an earlier issued displayed frame
             // after the decoder reached EOS. Consume that intent before EOS
             // can publish Ended at the earlier held PTS or shorten the tail.
             if(s->repositionOnResume) {reader.needsReposition=true;s->repositionOnResume=false;}}
            if (reader.eos && ReadyEmpty(*s) && !reader.needsReposition) {
                if (document.archive) {
                    const auto current = document.archive->Snapshot();
                    if (!current.finalized && current.generation == reader.archiveAdmission.generation &&
                        current.verifiedMediaBytes == reader.archiveAdmission.verifiedMediaBytes) {
                        std::unique_lock lock(s->mutex);
                        if (s->Current(document.generation) && s->playing) {
                            if (s->snapshot.state != VideoPlaybackState::Waiting) ClearAudio(*s);
                            s->snapshot.state = VideoPlaybackState::Waiting;
                            s->snapshot.error = "Waiting for the next complete recording fragment.";
                        }
                        s->changed.wait_for(lock,std::chrono::milliseconds(20),[&]{return !s->Current(document.generation) || !s->playing;});
                        continue;
                    }
                    if (current.verifiedMediaBytes > reader.archiveAdmission.verifiedMediaBytes ||
                        current.generation != reader.archiveAdmission.generation) {
                        try {
                            reader.Reset();
                            ConfigureReader(reader, document,*s); AdmitReaderMemory(reader,*s,document.generation);
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
                        } catch (const VideoGpuIncompatible& error) {
                            QueueCpuFallback(*s,document,reader.lastDecodedPts,error.what());
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
                if (s->Current(document.generation) && s->playing) { s->playing = false; s->snapshot.state = VideoPlaybackState::Ended;
                    if (!document.archive && s->snapshot.frame) s->snapshot.seekableEnd100ns = s->snapshot.frame->pts100ns; }
                continue;
            }
            try {
                MarkNativeOperation(*s, document.generation, true);
                detail::VideoPlaybackSeekBudget budget(Clock::now());
                bool restart{}, audioEnabled{}; std::int64_t cursor{},copyFloor{};
                { std::lock_guard lock(s->mutex);
                  // A playback iteration captured before Pause must not clear
                  // its newly selected future proof or consume restart intent.
                  if (!s->Current(document.generation) || !s->playing) {
                      if (s->Current(document.generation)) s->operationStart = {};
                      continue;
                  }
                  restart = s->restartAtCursor; s->restartAtCursor = false;
                  audioEnabled = !s->muted && reader.audioAvailable;
                  cursor = s->restartCursor100ns;
                  copyFloor=s->snapshot.frame?s->snapshot.frame->pts100ns+1:0;
                  s->snapshot.neighborFrame.reset();
                }
                if (restart) {
                    if (reader.hasAudio) Check(reader.source->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, audioEnabled), "Select playback audio");
                    if((audioEnabled && reader.hasAudio) || reader.needsReposition) {
                        {std::lock_guard lock(s->mutex);s->ready.clear();}
                        SeekReader(reader,cursor,*s,document.generation);
                        reader.needsReposition=false;
                        reader.resumeClockPending=true;
                    } else {
                        // Muted/silent Resume reuses prepared video. Audio Resume
                        // repositions both tracks to preserve exact PCM continuity.
                        std::lock_guard lock(s->mutex);
                        while(!s->ready.empty() && s->ready.front()->pts100ns<=cursor)s->ready.pop_front();
                    }
                    reader.audioFloor100ns=cursor;
                }
                unsigned samples{};
                for(;;) {
                    const auto deadline=budget.Deadline();
                    std::size_t readyCount{};std::int64_t headroom{};
                    {std::lock_guard lock(s->mutex);
                     if(!s->Current(document.generation) || !s->playing || s->restartAtCursor)break;
                     readyCount=s->ready.size();
                     if(readyCount)headroom=s->ready.front()->pts100ns-PlaybackNowLocked(*s,Clock::now());}
                    if(reader.eos || readyCount>=reader.memory.lookaheadFrames)break;
                    // An optional second read never delays an already-due frame.
                    // Use observed native/copy cost, with a 1 ms scheduling margin.
                    if(readyCount && headroom<=reader.decodeCost100ns+10'000)break;
                    if(!HasDecodeCredit(reader,s->pixels)) {
                        std::lock_guard lock(s->mutex);
                        s->snapshot.neighborFrame.reset();s->snapshot.memoryPressure=true;
                        break; // backpressure, not a failed recording or decoder
                    }
                    std::shared_ptr<const VideoPlaybackFrame> decoded;
                    bool readSample{};
                    if(samples>=VideoPlayback::kSeekSampleLimit || budget.Expired(Clock::now()))
                        throw std::runtime_error("Resume exceeds the bounded seek budget.");
                    ++samples;
                    try {readSample=ReadVideo(reader,*s,document.generation,decoded,deadline,nullptr,copyFloor);}
                    catch(const DecodeFailure&) {
                        if(!readyCount)throw;
                        reader.eos=true;reader.incomplete=true;break;
                    }
                    if(!s->Current(document.generation))break;
                    if(budget.Expired(Clock::now()))
                        throw std::runtime_error("Playback decode returned after its progress deadline.");
                    if(readSample && budget.Observe(reader.lastDecodedPts,Clock::now()))
                        MarkNativeOperation(*s,document.generation,true);
                    if(readSample && !decoded && !reader.eos) {
                        if(samples>=VideoPlayback::kSeekSampleLimit || budget.Expired(Clock::now()))
                            throw std::runtime_error("Resume exceeds the bounded seek budget.");
                        continue;
                    }
                    const bool decodedFrame=bool(decoded);
                    if(decodedFrame) {
                        std::lock_guard lock(s->mutex);
                        if(s->Current(document.generation)) {
                            if(reader.resumeClockPending) {
                                // Preroll is decode work, not played media.
                                // Without this rebase a healthy software seek
                                // starts video/audio many seconds past its hold.
                                s->clockPts=reader.audioFloor100ns;s->clockStart=Clock::now();
                                s->audioClockValid=false;reader.resumeClockPending=false;
                            }
                            s->ready.push_back(std::move(decoded));s->snapshot.memoryPressure=false;
                        }
                    }
                    if(!decodedFrame && !reader.eos) {
                        // Empty preroll reads remain in this same bounded
                        // request. A new outer iteration must not restart its
                        // no-progress/absolute budget without decoded media.
                        if(reader.resumeClockPending)continue;
                        break;
                    }
                }
                if (budget.Expired(Clock::now()) && ReadyEmpty(*s)) throw std::runtime_error("Playback decode exceeded its cooperative progress deadline.");
                MarkNativeOperation(*s, document.generation, false);
                std::shared_ptr<const VideoPlaybackFrame> next;
                {std::unique_lock lock(s->mutex);
                 if(!s->Current(document.generation) || !s->playing || s->restartAtCursor)continue;
                 if(s->ready.empty()) {
                     s->changed.wait_for(lock,std::chrono::milliseconds(5),[&]{return !s->Current(document.generation) || !s->playing || s->restartAtCursor;});
                     continue;
                 }
                 next=s->ready.front();}
                bool superseded{};
                for (;;) {
                    std::int64_t mediaNow{}, waitTicks{};
                    { std::lock_guard lock(s->mutex);
                      if (!s->playing || !s->Current(document.generation) || s->restartAtCursor) {
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
                  if (s->Current(document.generation) && s->playing && !s->restartAtCursor &&
                      !s->ready.empty() && s->ready.front()==next) {
                      s->ready.pop_front();
                      s->snapshot.neighborFrame = s->snapshot.frame;
                      s->snapshot.frame = next; s->snapshot.state = VideoPlaybackState::Playing;
                      s->snapshot.audioAvailable = reader.audioAvailable;
                      if (reader.eos) s->snapshot.seekableEnd100ns = next->pts100ns;
                  }
                }
            } catch (const VideoGpuIncompatible& error) {
                // Coherent in-worker fallback: rebuild this same document on
                // CPU at the held cursor instead of stranding playback in
                // Unavailable with a live (possibly unsafe) GPU reader.
                { std::lock_guard lock(s->mutex); s->operationStart = {}; }
                QueueCpuFallback(*s,document,reader.lastDecodedPts,error.what());
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
    if(command.kind==CommandKind::Open) {s->cpuFallback=false;s->backendReason.clear();}
    if(command.forceCpu) {s->cpuFallback=true;s->backendReason=command.backendReason;}
    command.gpuAdapter=s->gpuAdapter;command.forceCpu=command.forceCpu || s->cpuFallback;
    s->snapshot.requestGeneration = command.generation;
    s->snapshot.state = command.kind == CommandKind::Open ? VideoPlaybackState::Opening : VideoPlaybackState::Seeking;
    s->snapshot.error.clear(); s->playing = false; ClearAudio(*s);
    if (command.kind == CommandKind::Open) { s->muted = true; s->snapshot.muted = true; }
    s->snapshot.neighborFrame.reset();
    s->ready.clear();s->prepareNeighbor=false;s->restartAtCursor=false;s->repositionOnResume=false;
    s->snapshot.memoryPressure=false;
    s->snapshot.lastRequestReadCount=0;s->snapshot.lastRequestCopiedFrames=0;
    s->snapshot.lastRequestLatencyUs=0;s->snapshot.lastRequestReusedFrame=false;
    s->operationStart = command.queued; s->pending = std::move(command); s->changed.notify_one();
    return s->generation.load();
}
}
std::optional<VideoPlaybackMemoryPlan> PlanVideoPlaybackMemory(std::uint32_t width,std::uint32_t height,
    std::int64_t stride,std::uint32_t parN,std::uint32_t parD,bool archive) {
    if (!width || !height || width > 16384 || height > 16384 || stride == INT64_MIN || !parN || !parD)return {};
    const auto pitch = static_cast<std::uint64_t>(stride < 0 ? -stride : stride);
    const auto squareWidth=(std::uint64_t(width)*parN+parD/2)/parD;
    if(pitch<std::uint64_t(width)*4 || pitch>VideoPlayback::kDecodedByteLimit/height ||
       !squareWidth || squareWidth>16384)return {};
    const auto staging=pitch*height,frame=squareWidth*height*4;
    // Admit a complete working set, not merely one individually valid image.
    // Four retained/presentation/seek roles plus at least one future frame,
    // and native staging. Optional second lookahead degrades at the ceiling.
    if(frame>VideoPlayback::kDecodedByteLimit/5 || staging>VideoPlayback::kDecodedByteLimit-5*frame)return {};
    auto lookahead=archive?1u:2u;
    const auto desired=staging+(4+lookahead)*frame;
    const auto limit=std::min<std::uint64_t>(VideoPlayback::kDecodedByteLimit,
        std::max<std::uint64_t>(VideoPlayback::kMinimumDecodedByteLimit,desired));
    lookahead=std::min<std::uint32_t>(lookahead,static_cast<std::uint32_t>((limit-staging)/frame-4));
    const auto working=staging+frame;
    return VideoPlaybackMemoryPlan{std::size_t(staging),std::size_t(frame),std::size_t(working),
        std::size_t(limit-working),std::size_t(limit),lookahead};
}
bool VideoPlaybackLayoutFits(std::uint32_t width,std::uint32_t height,std::int64_t stride) {
    return PlanVideoPlaybackMemory(width,height,stride).has_value();
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
void VideoPlayback::SetGpuAdapter(std::optional<LUID> adapter) {std::lock_guard lock(state_->mutex);state_->gpuAdapter=adapter;}
std::uint64_t VideoPlayback::UseCpuFallback(std::string reason) {
    Command c;c.kind=CommandKind::Seek;c.forceCpu=true;c.backendReason=std::move(reason);
    {std::lock_guard lock(state_->mutex);if(state_->snapshot.frame)c.target=state_->snapshot.frame->pts100ns;}
    return Submit(state_,std::move(c));
}
void VideoPlayback::SetPlaying(bool play) {
    std::lock_guard lock(state_->mutex);
    if (play && (!state_->snapshot.frame ||
        (state_->snapshot.state != VideoPlaybackState::Paused &&
         state_->snapshot.state != VideoPlaybackState::Playing &&
         state_->snapshot.state != VideoPlaybackState::Waiting &&
         !(state_->snapshot.state == VideoPlaybackState::Ended && state_->snapshot.activeRecording)) ||
        (state_->snapshot.state == VideoPlaybackState::Ended && !state_->snapshot.activeRecording) ||
        state_->pending.kind != CommandKind::None)) return;
    if (!play && state_->playing && PauseAtFrameLocked(*state_, state_->snapshot.frame)) {
        state_->changed.notify_one(); return;
    }
    state_->playing = play; ClearAudio(*state_);
    if (play) { state_->restartAtCursor = true; state_->restartCursor100ns = state_->snapshot.frame->pts100ns;
        state_->clockStart = Clock::now(); state_->clockPts = state_->snapshot.frame->pts100ns; state_->snapshot.state = VideoPlaybackState::Playing; }
    else if (state_->snapshot.frame && (state_->snapshot.state == VideoPlaybackState::Playing || state_->snapshot.state == VideoPlaybackState::Waiting))
        state_->snapshot.state = VideoPlaybackState::Paused;
    state_->changed.notify_one();
}
bool VideoPlayback::PauseAtFrame(std::shared_ptr<const VideoPlaybackFrame> shown) {
    std::lock_guard lock(state_->mutex);
    if (!PauseAtFrameLocked(*state_, shown)) return false;
    state_->changed.notify_one();
    return true;
}
void VideoPlayback::SetMuted(bool muted) {
    std::lock_guard lock(state_->mutex);
    if (state_->muted == muted) return;
    if (state_->playing && state_->snapshot.audioAvailable) {
        const auto now = Clock::now(); state_->restartCursor100ns = PlaybackNowLocked(*state_, now);
        state_->clockPts = state_->restartCursor100ns; state_->clockStart = now; state_->restartAtCursor = true;
        state_->ready.clear();
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
    result.decodedByteLimit=state_->pixels->limit.load();result.readyFrames=static_cast<std::uint32_t>(state_->ready.size());
    result.gpuTextureBytes=VideoGpuDecoder::RetainedBytes();
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
