#ifdef _WIN32
#include "okuflow/common/video_audio_output.hpp"
#include "video_audio_gain.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mmreg.h>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace okuflow {
namespace {
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
std::atomic_flag nativeOwner = ATOMIC_FLAG_INIT;
constexpr std::int64_t kNativeDeadline100ns = 20'000'000;
constexpr std::size_t kClockSpanLimit = 128;
std::int64_t Now100ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count() / 100;
}
struct Packet {
    VideoPlaybackAudio audio;
    std::uint64_t epoch{};
    std::int64_t duration100ns{};
};
struct Superseded {};
std::int64_t FramesTime(std::uint64_t frames, std::uint32_t rate) {
    return static_cast<std::int64_t>((frames / rate) * 10'000'000 +
        (frames % rate) * 10'000'000 / rate);
}
std::optional<std::int64_t> PacketDuration(const VideoPlaybackAudio& p) {
    if (p.bitsPerSample != 16 || p.sampleRate < 8'000 || p.sampleRate > 384'000 ||
        !p.channels || p.channels > 8 || p.pts100ns < 0 || p.pcm.empty() ||
        p.pcm.size() > VideoAudioOutput::kByteLimit || p.pcm.size() % (p.channels * 2))
        return std::nullopt;
    const auto frames = p.pcm.size() / (p.channels * 2);
    // Round admission up: repeated short packets cannot evade the time cap.
    const auto duration = static_cast<std::int64_t>(
        (frames * 10'000'000ull + p.sampleRate - 1) / p.sampleRate);
    if (!duration || duration > VideoAudioOutput::kDurationLimit100ns ||
        p.pts100ns > std::numeric_limits<std::int64_t>::max() - duration)
        return std::nullopt;
    return duration;
}
std::string NativeError(const char* operation, HRESULT hr) {
    char code[32]{};
    std::snprintf(code, sizeof(code), " (0x%08lx)", static_cast<unsigned long>(hr));
    return std::string(operation) + code;
}
void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(NativeError(operation, hr));
}
}

struct VideoAudioOutput::State {
    mutable std::mutex mutex;
    std::condition_variable changed;
    VideoAudioOutputSnapshot snapshot;
    std::deque<Packet> pending;
    std::atomic<bool> stop{}, active{}, failed{};
    std::atomic<int> volumePercent{100};
    std::atomic<std::uint64_t> epoch{1};
    std::atomic<std::int64_t> nativeCallStarted{};
    bool nativePlayback{}, ownsWorker{};

    bool Current(std::uint64_t e) const {
        return !stop.load() && active.load() && !failed.load() && epoch.load() == e;
    }
    bool Stalled() const {
        const auto started = nativeCallStarted.load();
        return started && Now100ns() - started > kNativeDeadline100ns;
    }
};

namespace {
// All calls and COM releases occur without holding State::mutex. A hung call
// owns this state and the process-wide lease until it really returns.
struct NativeCall {
    VideoAudioOutput::State& state;
    explicit NativeCall(VideoAudioOutput::State& s) : state(s) {
        state.nativeCallStarted.store(Now100ns());
    }
    ~NativeCall() { state.nativeCallStarted.store(0); }
};
template<class F> HRESULT Call(VideoAudioOutput::State& s, std::uint64_t e, F f) {
    if (!s.Current(e)) throw Superseded{};
    NativeCall scope(s);
    return f();
}
void DiscardPending(VideoAudioOutput::State& s) {
    for (const auto& p : s.pending) {
        s.snapshot.queuedBytes -= p.audio.pcm.size();
        s.snapshot.queuedDuration100ns -= p.duration100ns;
        --s.snapshot.queuedPackets;
    }
    s.snapshot.discardedPackets += s.pending.size();
    s.pending.clear();
}
void Invalidate(VideoAudioOutput::State& s) {
    s.epoch.fetch_add(1);
    DiscardPending(s);
    s.snapshot.available = s.snapshot.clockValid = false;
    s.snapshot.sampledAtSteady100ns = 0;
    s.snapshot.flushPending = s.ownsWorker;
}
void Fail(VideoAudioOutput::State& s, std::uint64_t epoch, std::string error) {
    std::lock_guard lock(s.mutex);
    if (s.epoch.load() != epoch || s.stop.load()) return;
    s.failed.store(true);
    DiscardPending(s);
    s.snapshot.available = s.snapshot.clockValid = false;
    s.snapshot.error = std::move(error);
    s.snapshot.flushPending = s.ownsWorker;
}
void ReleasePacket(VideoAudioOutput::State& s, std::optional<Packet>& packet, bool discarded) {
    if (!packet) return;
    {
        std::lock_guard lock(s.mutex);
        s.snapshot.queuedBytes -= packet->audio.pcm.size();
        s.snapshot.queuedDuration100ns -= packet->duration100ns;
        --s.snapshot.queuedPackets;
        s.snapshot.inFlightBytes = 0;
        if (discarded) ++s.snapshot.discardedPackets;
    }
    packet.reset();
}
struct ClockSpan {
    std::uint64_t firstFrame{}, frames{};
    std::int64_t pts100ns{};
};
bool ExtendsSpan(const ClockSpan& span, std::uint64_t frames, std::int64_t pts,
                 std::uint32_t rate) {
    const auto duration = FramesTime(span.frames + frames, rate);
    if (span.pts100ns > std::numeric_limits<std::int64_t>::max() - duration) return false;
    const auto expected = static_cast<long double>(span.pts100ns) + FramesTime(span.frames, rate);
    return static_cast<long double>(pts) >= expected - 2 &&
        static_cast<long double>(pts) <= expected + 2;
}
struct Device {
    VideoAudioOutput::State& state;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> endpoint;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    std::deque<ClockSpan> spans;
    std::uint32_t rate{}, channels{}, bufferFrames{};
    std::uint64_t frequency{}, submittedFrames{};
    std::int64_t lastSubmittedEnd100ns{};
    bool started{};

    explicit Device(VideoAudioOutput::State& s) : state(s) {}
    ~Device() { Clear(); }
    void Clear() noexcept {
        NativeCall scope(state);
        if (client && started) client->Stop();
        clock.Reset(); render.Reset(); client.Reset(); endpoint.Reset(); enumerator.Reset();
        spans.clear(); rate = channels = bufferFrames = 0;
        frequency = submittedFrames = 0; lastSubmittedEnd100ns = 0; started = false;
    }
    void Initialize(const VideoPlaybackAudio& p, std::uint64_t epoch) {
        Clear();
        Check(Call(state, epoch, [&] { return CoCreateInstance(__uuidof(MMDeviceEnumerator),
            nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)); }), "Create playback audio endpoint enumerator");
        Check(Call(state, epoch, [&] { return enumerator->GetDefaultAudioEndpoint(
            eRender, eMultimedia, &endpoint); }), "Open default playback audio endpoint");
        Check(Call(state, epoch, [&] { return endpoint->Activate(__uuidof(IAudioClient),
            CLSCTX_ALL, nullptr, &client); }), "Activate playback audio endpoint");
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(p.channels);
        format.nSamplesPerSec = p.sampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(p.channels * 2);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        Check(Call(state, epoch, [&] { return client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            1'000'000, 0, &format, nullptr); }), "Initialize shared playback audio");
        Check(Call(state, epoch, [&] { return client->GetBufferSize(&bufferFrames); }),
            "Read playback audio buffer size");
        if (!bufferFrames || bufferFrames > p.sampleRate / 4)
            throw std::runtime_error("Playback endpoint exceeds the 250 ms device-buffer limit.");
        Check(Call(state, epoch, [&] { return client->GetService(IID_PPV_ARGS(&render)); }),
            "Open playback audio render service");
        Check(Call(state, epoch, [&] { return client->GetService(IID_PPV_ARGS(&clock)); }),
            "Open playback audio clock");
        Check(Call(state, epoch, [&] { return clock->GetFrequency(&frequency); }),
            "Read playback audio clock frequency");
        if (!frequency) throw std::runtime_error("Playback endpoint has no usable device clock.");
        rate = p.sampleRate; channels = p.channels;
    }
    UINT32 Padding(std::uint64_t epoch) {
        UINT32 padding{};
        Check(Call(state, epoch, [&] { return client->GetCurrentPadding(&padding); }),
            "Read playback audio padding");
        if (padding > bufferFrames) throw std::runtime_error("Invalid playback audio buffer padding.");
        return padding;
    }
    void StopAndReset(std::uint64_t epoch) {
        if (started) Check(Call(state, epoch, [&] { return client->Stop(); }), "Stop playback audio");
        started = false;
        Check(Call(state, epoch, [&] { return client->Reset(); }), "Flush playback audio");
        spans.clear(); submittedFrames = 0; lastSubmittedEnd100ns = 0;
    }
    bool CanAppend(std::uint64_t frames, std::int64_t pts) const {
        if (spans.size() < kClockSpanLimit) return true;
        return ExtendsSpan(spans.back(), frames, pts, rate);
    }
    void AppendSpan(std::uint64_t frames, std::int64_t pts) {
        if (!spans.empty()) {
            auto& last = spans.back();
            if (ExtendsSpan(last, frames, pts, rate)) {
                last.frames += frames;
                submittedFrames += frames;
                lastSubmittedEnd100ns = pts + FramesTime(frames, rate);
                return;
            }
        }
        spans.push_back({submittedFrames, frames, pts});
        submittedFrames += frames;
        lastSubmittedEnd100ns = pts + FramesTime(frames, rate);
    }
    void PublishClock(std::uint64_t epoch, UINT32 padding) {
        if (!started || spans.empty()) return;
        UINT64 position{}, qpc{};
        const auto sampleBegin = Now100ns();
        const auto hr = Call(state, epoch, [&] { return clock->GetPosition(&position, &qpc); });
        const auto sampleEnd = Now100ns();
        Check(hr, "Read rendered playback audio position");
        // Position units are defined by GetFrequency, not necessarily frames
        // or bytes. Clamp to submitted PCM, including an endpoint underrun.
        const auto framePosition = static_cast<long double>(position) * rate / frequency;
        const auto consumed = framePosition >= static_cast<long double>(submittedFrames)
            ? submittedFrames : static_cast<std::uint64_t>(framePosition);
        while (spans.size() > 1 && consumed >= spans.front().firstFrame + spans.front().frames)
            spans.pop_front();
        const auto& span = spans.front();
        const auto offset = consumed > span.firstFrame
            ? std::min(consumed - span.firstFrame, span.frames) : 0;
        std::lock_guard lock(state.mutex);
        if (!state.Current(epoch)) return;
        state.snapshot.available = true;
        // S_FALSE denotes a delayed/less accurate clock observation.
        state.snapshot.clockValid = hr == S_OK && position != 0 && sampleEnd - sampleBegin <= 500'000;
        state.snapshot.renderedPts100ns = span.pts100ns + FramesTime(offset, rate);
        state.snapshot.sampledAtSteady100ns = sampleBegin + (sampleEnd - sampleBegin) / 2;
        state.snapshot.deviceBufferedDuration100ns = FramesTime(padding, rate);
    }
};

void Run(const std::shared_ptr<VideoAudioOutput::State>& s) noexcept {
    // This guard is outside Device's lifetime: a blocked Stop/Release or
    // CoUninitialize retains the lease and prevents replacement workers.
    struct OwnerLease { ~OwnerLease() { nativeOwner.clear(); } } lease;
    bool comInitialized{};
    std::optional<Packet> packet;
    std::size_t offsetBytes{};
    try {
        Device device(*s);
        std::uint64_t appliedEpoch{};
        while (!s->stop.load()) {
            const auto epoch = s->epoch.load();
            if (appliedEpoch != epoch || !s->Current(epoch)) {
                device.Clear();
                ReleasePacket(*s, packet, true); offsetBytes = 0;
                appliedEpoch = epoch;
                std::lock_guard lock(s->mutex);
                if (s->epoch.load() == epoch) {
                    s->snapshot.flushPending = false;
                    s->snapshot.available = s->snapshot.clockValid = false;
                    s->snapshot.deviceBufferedDuration100ns = 0;
                }
            }
            if (!s->Current(epoch)) {
                std::unique_lock lock(s->mutex);
                s->changed.wait(lock, [&] { return s->stop.load() ||
                    s->epoch.load() != epoch || s->Current(epoch); });
                continue;
            }
            if (!packet) {
                std::lock_guard lock(s->mutex);
                if (s->Current(epoch) && !s->pending.empty() && s->pending.front().epoch == epoch) {
                    packet.emplace(std::move(s->pending.front())); s->pending.pop_front();
                    s->snapshot.inFlightBytes = packet->audio.pcm.size();
                    offsetBytes = 0;
                }
            }
            try {
                if (packet && !comInitialized) {
                    const auto hr = Call(*s, epoch, [] { return CoInitializeEx(nullptr, COINIT_MULTITHREADED); });
                    Check(hr, "Initialize playback audio COM worker"); comInitialized = true;
                }
                if (packet) {
                    const auto& p = packet->audio;
                    if (packet->epoch != epoch) throw Superseded{};
                    const bool formatChanged = device.rate != p.sampleRate || device.channels != p.channels;
                    const bool discontinuous = !offsetBytes && device.submittedFrames &&
                        (static_cast<long double>(p.pts100ns) - device.lastSubmittedEnd100ns > 10'000 ||
                         static_cast<long double>(p.pts100ns) - device.lastSubmittedEnd100ns < -10'000);
                    if (formatChanged || discontinuous) {
                        if (device.client && device.started) {
                            const auto padding = device.Padding(epoch);
                            device.PublishClock(epoch, padding);
                            if (padding) {
                                std::unique_lock lock(s->mutex);
                                s->changed.wait_for(lock, std::chrono::milliseconds(5));
                                continue;
                            }
                        }
                        device.Initialize(p, epoch);
                    }
                    auto padding = device.Padding(epoch);
                    if (!padding && device.started) {
                        device.PublishClock(epoch, 0);
                        // A device clock may advance through an underrun. A
                        // new stream origin avoids assigning that gap to PCM.
                        device.StopAndReset(epoch);
                    }
                    const auto frames = std::min<std::size_t>(device.bufferFrames - padding,
                        (p.pcm.size() - offsetBytes) / (p.channels * 2));
                    const auto pts = p.pts100ns + FramesTime(offsetBytes / (p.channels * 2), p.sampleRate);
                    if (frames && device.CanAppend(frames, pts)) {
                        BYTE* destination{};
                        Check(Call(*s, epoch, [&] { return device.render->GetBuffer(
                            static_cast<UINT32>(frames), &destination); }), "Acquire playback audio buffer");
                        const auto bytes = frames * p.channels * 2;
                        detail::CopyPlaybackPcm16(destination,p.pcm.data()+offsetBytes,bytes,s->volumePercent.load());
                        // An acquired native buffer must always be released,
                        // including cancellation while GetBuffer was blocked.
                        HRESULT release{};
                        {
                            NativeCall call(*s);
                            release = device.render->ReleaseBuffer(static_cast<UINT32>(frames),
                                s->Current(epoch) ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                        Check(release, "Submit playback audio buffer");
                        if (!s->Current(epoch)) throw Superseded{};
                        device.AppendSpan(frames, pts);
                        offsetBytes += bytes;
                        padding += static_cast<UINT32>(frames);
                        if (!device.started) {
                            Check(Call(*s, epoch, [&] { return device.client->Start(); }), "Start playback audio");
                            device.started = true;
                        }
                        if (offsetBytes == p.pcm.size()) ReleasePacket(*s, packet, false);
                    }
                    device.PublishClock(epoch, padding);
                } else if (device.client && device.started) {
                    const auto padding = device.Padding(epoch);
                    device.PublishClock(epoch, padding);
                    if (!padding) {
                        device.StopAndReset(epoch);
                        std::lock_guard lock(s->mutex);
                        if (s->Current(epoch)) s->snapshot.clockValid = false;
                    }
                }
            } catch (const Superseded&) {
                // The next iteration performs worker-owned stop/flush.
            } catch (const std::exception& e) {
                Fail(*s, epoch, e.what());
            }
            std::unique_lock lock(s->mutex);
            if (s->pending.empty() && !packet && !device.started) {
                s->changed.wait(lock, [&] { return s->stop.load() ||
                    s->epoch.load() != epoch || !s->Current(epoch) || !s->pending.empty(); });
            } else {
                s->changed.wait_for(lock, std::chrono::milliseconds(5));
            }
        }
    } catch (const std::exception& e) {
        Fail(*s, s->epoch.load(), e.what());
    } catch (...) {
        Fail(*s, s->epoch.load(), "Unexpected playback audio worker failure.");
    }
    ReleasePacket(*s, packet, true);
    if (comInitialized) { NativeCall call(*s); CoUninitialize(); }
    std::lock_guard lock(s->mutex);
    s->ownsWorker = false;
    s->snapshot.available = s->snapshot.clockValid = s->snapshot.flushPending = false;
    s->snapshot.deviceBufferedDuration100ns = 0;
}
}

VideoAudioOutput::VideoAudioOutput() : VideoAudioOutput(Options{}) {}
VideoAudioOutput::VideoAudioOutput(Options options) : state_(std::make_shared<State>()) {
    state_->nativePlayback = options.nativePlayback;
    if (!options.nativePlayback) return;
    if (nativeOwner.test_and_set()) {
        state_->failed.store(true);
        state_->snapshot.error = "Playback audio is already owned by an active or retiring worker.";
        return;
    }
    state_->ownsWorker = true;
    try { std::thread([s = state_] { Run(s); }).detach(); }
    catch (const std::exception& e) {
        state_->ownsWorker = false; state_->failed.store(true);
        state_->snapshot.error = std::string("Cannot start playback audio worker: ") + e.what();
        nativeOwner.clear();
    }
}
VideoAudioOutput::~VideoAudioOutput() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stop.store(true); state_->active.store(false);
        Invalidate(*state_);
    }
    state_->changed.notify_all();
}
void VideoAudioOutput::SetVolumePercent(int percent) {
    percent=std::clamp(percent,0,100);
    std::lock_guard lock(state_->mutex);
    state_->volumePercent.store(percent);
    state_->snapshot.volumePercent=percent;
}
void VideoAudioOutput::Configure(std::uint64_t generation, bool playing, bool muted) {
    {
        std::lock_guard lock(state_->mutex);
        auto& snapshot = state_->snapshot;
        const bool changedGeneration = generation != snapshot.requestGeneration;
        if (changedGeneration || (!playing && snapshot.playing) || (muted && !snapshot.muted))
            Invalidate(*state_);
        if (changedGeneration && (!state_->nativePlayback || state_->ownsWorker)) {
            state_->failed.store(false); snapshot.error.clear();
        }
        snapshot.requestGeneration = generation;
        snapshot.playing = playing; snapshot.muted = muted;
        state_->active.store(playing && !muted);
    }
    state_->changed.notify_all();
}
bool VideoAudioOutput::Push(VideoPlaybackAudio packet) {
    const auto duration = PacketDuration(packet);
    bool accepted{};
    {
        std::lock_guard lock(state_->mutex);
        auto& snapshot = state_->snapshot;
        if (!state_->active.load() || state_->stop.load() ||
            packet.requestGeneration != snapshot.requestGeneration) {
            ++snapshot.discardedPackets; return false;
        }
        if (state_->failed.load() || state_->Stalled()) {
            ++snapshot.discardedPackets; return false;
        }
        const bool exceeds = !duration || snapshot.queuedPackets >= kPacketLimit ||
            packet.pcm.size() > kByteLimit - snapshot.queuedBytes ||
            *duration > kDurationLimit100ns - snapshot.queuedDuration100ns;
        if (exceeds) {
            state_->failed.store(true);
            ++snapshot.discardedPackets; DiscardPending(*state_);
            snapshot.available = snapshot.clockValid = false;
            snapshot.flushPending = state_->ownsWorker;
            snapshot.error = duration ? "Playback audio queue limit reached; output was discarded."
                : "Playback audio has an unsupported PCM format or exceeds the packet limit.";
        } else {
            const auto bytes = packet.pcm.size();
            state_->pending.push_back({std::move(packet), state_->epoch.load(), *duration});
            snapshot.queuedBytes += bytes; ++snapshot.queuedPackets;
            snapshot.queuedDuration100ns += *duration;
            accepted = true;
        }
    }
    state_->changed.notify_all();
    return accepted;
}
void VideoAudioOutput::Reset(std::uint64_t generation) {
    {
        std::lock_guard lock(state_->mutex);
        Invalidate(*state_);
        state_->snapshot.requestGeneration = generation;
        state_->snapshot.playing = false; state_->snapshot.muted = true;
        state_->active.store(false);
        if (!state_->nativePlayback || state_->ownsWorker) {
            state_->failed.store(false); state_->snapshot.error.clear();
        }
    }
    state_->changed.notify_all();
}
void VideoAudioOutput::Close() {
    {
        std::lock_guard lock(state_->mutex);
        Invalidate(*state_);
        state_->snapshot.playing = false; state_->snapshot.muted = true;
        state_->active.store(false);
    }
    state_->changed.notify_all();
}
VideoAudioOutputSnapshot VideoAudioOutput::Snapshot() const {
    std::lock_guard lock(state_->mutex);
    auto snapshot = state_->snapshot;
    if (state_->Stalled()) {
        snapshot.available = snapshot.clockValid = false;
        snapshot.error = "Playback audio endpoint is not responding; its worker remains owned.";
    }
    return snapshot;
}
} // namespace okuflow
#endif
