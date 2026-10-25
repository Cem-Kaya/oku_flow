#ifdef _WIN32
#include "okuflow/common/video_audio_output.hpp"
#include "../src/common/video_audio_gain.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using namespace okuflow;
using Clock = std::chrono::steady_clock;
int failures{};
void Expect(bool condition, const char* description) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
VideoPlaybackAudio Pcm(std::uint64_t generation, std::uint32_t frames = 480,
    std::int64_t pts = 0, std::uint32_t rate = 48'000, std::uint32_t channels = 2) {
    VideoPlaybackAudio p;
    p.requestGeneration = generation; p.sampleRate = rate; p.channels = channels;
    p.pts100ns = pts; p.pcm.resize(static_cast<std::size_t>(frames) * channels * 2);
    return p;
}
VideoAudioOutput::Options AdmissionOnly() {
    VideoAudioOutput::Options options; options.nativePlayback = false; return options;
}
void SilentUntilExplicitPlayback() {
    VideoAudioOutput sink(AdmissionOnly());
    Expect(!sink.Push(Pcm(1)), "default output is silent");
    sink.Configure(1, false, false);
    Expect(!sink.Push(Pcm(1)), "paused output discards PCM");
    sink.Configure(1, true, true);
    Expect(!sink.Push(Pcm(1)), "muted output discards PCM");
    sink.Configure(1, true, false);
    Expect(sink.Push(Pcm(1, 480, 900'000'000)), "explicit unmuted playback admits owned PCM");
    const auto s = sink.Snapshot();
    Expect(s.queuedBytes == 1920 && s.queuedPackets == 1 && s.queuedDuration100ns == 100'000,
        "queued packet reports its real byte and media-time cost");
    Expect(!s.available && !s.clockValid && !s.renderedPts100ns && !s.sampledAtSteady100ns,
        "admission does not fabricate a rendered audio clock");
}
void PauseSeekMuteAndCloseRevoke() {
    VideoAudioOutput sink(AdmissionOnly());
    sink.Configure(8, true, false); Expect(sink.Push(Pcm(8)), "initial packet admitted");
    sink.Configure(8, true, false);
    Expect(sink.Snapshot().queuedPackets == 1, "idempotent polling preserves queue");
    sink.Configure(8, false, false);
    Expect(!sink.Snapshot().queuedBytes && !sink.Push(Pcm(8)), "pause immediately clears and rejects output");
    sink.Configure(8, true, false); Expect(sink.Push(Pcm(8)), "resume accepts new PCM");
    sink.Configure(9, true, false);
    Expect(!sink.Snapshot().queuedBytes && !sink.Push(Pcm(8)), "seek invalidates prior source generation");
    Expect(sink.Push(Pcm(9)), "new seek generation admitted");
    sink.Configure(9, true, true);
    Expect(!sink.Snapshot().queuedPackets && !sink.Push(Pcm(9)), "mute flushes decoded audio");
    sink.Configure(9, true, false); Expect(sink.Push(Pcm(9)), "unmute accepts fresh PCM");
    sink.Close();
    const auto closed = sink.Snapshot();
    Expect(!closed.playing && closed.muted && !closed.queuedPackets && !closed.clockValid,
        "Close silences and revokes clock synchronously");
    Expect(!sink.Push(Pcm(9)), "Close refuses later PCM");
    sink.Configure(10, true, false);
    Expect(sink.Push(Pcm(10)), "same sink can serve another document without another worker");
    sink.Reset(11);
    Expect(sink.Snapshot().requestGeneration == 11 && !sink.Snapshot().queuedBytes,
        "explicit reset establishes a new silent generation");
}
void ResourceCapsAndRecovery() {
    VideoAudioOutput sink(AdmissionOnly());
    sink.Configure(1, true, false);
    for (std::size_t i = 0; i < VideoAudioOutput::kPacketLimit; ++i)
        Expect(sink.Push(Pcm(1, 8, static_cast<std::int64_t>(i) * 10'000, 8'000, 1)),
            "bounded short packet admitted");
    Expect(sink.Snapshot().queuedPackets == VideoAudioOutput::kPacketLimit, "packet boundary admitted exactly");
    Expect(!sink.Push(Pcm(1, 8, 320'000, 8'000, 1)), "packet-count overflow rejected");
    auto s = sink.Snapshot();
    Expect(!s.queuedBytes && !s.available && !s.clockValid && !s.error.empty(),
        "overflow discards output and reports unavailable instead of silently losing sync");
    sink.Configure(1, true, false);
    Expect(!sink.Push(Pcm(1)), "unchanged polling does not retry a failed output");
    sink.Reset(2); sink.Configure(2, true, false);
    Expect(sink.Push(Pcm(2, 14'400, 0, 48'000, 1)) &&
        sink.Push(Pcm(2, 9'600, 3'000'000, 48'000, 1)), "exact 500 ms duration boundary admitted");
    Expect(sink.Snapshot().queuedDuration100ns == VideoAudioOutput::kDurationLimit100ns,
        "time cost includes all queued packets");
    Expect(!sink.Push(Pcm(2, 1, 5'000'000, 48'000, 1)), "one extra audio frame cannot exceed the time cap");
    sink.Reset(3); sink.Configure(3, true, false);
    Expect(sink.Push(Pcm(3, 131'072, 0, 384'000, 8)), "exact 2 MiB PCM byte boundary admitted");
    Expect(sink.Snapshot().queuedBytes == VideoAudioOutput::kByteLimit, "byte cap is inclusive");
    Expect(!sink.Push(Pcm(3, 1, 3'413'334, 384'000, 8)), "byte overflow rejected independently of time cap");
    sink.Configure(4, true, false);
    Expect(sink.Push(Pcm(4)) && sink.Snapshot().error.empty(), "new generation explicitly recovers bounded admission");
}
void MalformedPcmFailsClosed() {
    VideoAudioOutput sink(AdmissionOnly());
    std::uint64_t generation = 1;
    auto reject = [&](VideoPlaybackAudio p, const char* description) {
        sink.Reset(generation); sink.Configure(generation, true, false);
        p.requestGeneration = generation++;
        Expect(!sink.Push(std::move(p)), description);
        const auto s = sink.Snapshot();
        Expect(!s.available && !s.clockValid && !s.queuedBytes && !s.error.empty(),
            "malformed PCM cannot publish available audio");
    };
    auto p = Pcm(0); p.bitsPerSample = 32; reject(std::move(p), "only decoded PCM16 is accepted");
    p = Pcm(0); p.channels = 0; reject(std::move(p), "zero channels rejected");
    p = Pcm(0); p.sampleRate = 0; reject(std::move(p), "zero sample rate rejected");
    p = Pcm(0); p.pcm.pop_back(); reject(std::move(p), "partial interleaved frame rejected");
    p = Pcm(0, 24'001); reject(std::move(p), "oversized duration packet rejected");
    p = Pcm(0); p.pts100ns = -1; reject(std::move(p), "negative media time rejected");
    p = Pcm(0); p.pts100ns = std::numeric_limits<std::int64_t>::max();
    reject(std::move(p), "media-time overflow rejected");
}
void ConcurrentSupersessionRemainsBounded() {
    VideoAudioOutput sink(AdmissionOnly());
    std::atomic<bool> start{};
    std::thread producer([&] {
        while (!start.load()) std::this_thread::yield();
        for (std::uint64_t generation = 1; generation <= 1000; ++generation)
            sink.Push(Pcm(generation, 1));
    });
    start.store(true);
    for (std::uint64_t generation = 1; generation <= 1000; ++generation) {
        sink.Configure(generation, true, false);
        const auto s = sink.Snapshot();
        Expect(s.queuedBytes <= VideoAudioOutput::kByteLimit &&
            s.queuedPackets <= VideoAudioOutput::kPacketLimit &&
            s.queuedDuration100ns <= VideoAudioOutput::kDurationLimit100ns,
            "concurrent source changes preserve all three resource caps");
    }
    producer.join(); sink.Reset(1001);
    Expect(!sink.Push(Pcm(1000)) && !sink.Snapshot().queuedBytes,
        "retired producer cannot restore old audio after supersession");
}
void PlaybackGainPreservesSourceAndTiming() {
    const std::array<std::int16_t,8> source{-32768,-32767,-3,-1,0,1,3,32767};
    std::array<std::int16_t,8> output{};
    detail::CopyPlaybackPcm16(output.data(),source.data(),sizeof(source),50);
    Expect(output==std::array<std::int16_t,8>{-16384,-16383,-1,0,0,0,1,16383},
        "50 percent attenuates signed PCM16 including extrema without overflow");
    detail::CopyPlaybackPcm16(output.data(),source.data(),sizeof(source),150);
    Expect(output==source,"100 percent and above preserve exact PCM bytes");
    detail::CopyPlaybackPcm16(output.data(),source.data(),sizeof(source),-10);
    Expect(output==std::array<std::int16_t,8>{},"zero percent and below produce silence");
    Expect(source==std::array<std::int16_t,8>{-32768,-32767,-3,-1,0,1,3,32767},
        "gain never changes decoded source PCM");

    VideoAudioOutput sink(AdmissionOnly());
    Expect(sink.Snapshot().volumePercent==100,"playback starts at full volume");
    sink.Configure(41,true,false);
    Expect(sink.Push(Pcm(41,480,900'000)),"volume fixture admits timed PCM");
    const auto before=sink.Snapshot();
    sink.SetVolumePercent(25);
    const auto after=sink.Snapshot();
    Expect(after.volumePercent==25 && after.requestGeneration==before.requestGeneration &&
        after.queuedBytes==before.queuedBytes && after.queuedPackets==before.queuedPackets &&
        after.queuedDuration100ns==before.queuedDuration100ns && after.playing==before.playing &&
        after.muted==before.muted && after.clockValid==before.clockValid,
        "gain leaves queued timing, mute, playback and generation intact");
    sink.SetVolumePercent(-1);Expect(sink.Snapshot().volumePercent==0,"minimum volume is clamped");
    sink.SetVolumePercent(101);Expect(sink.Snapshot().volumePercent==100,"maximum volume is clamped");
    sink.SetVolumePercent(35);sink.Reset(42);sink.Close();
    Expect(sink.Snapshot().volumePercent==35,"session volume survives seek and document close");
}
void NativeEndpointOptIn() {
    wchar_t enabled[8]{};
    if (GetEnvironmentVariableW(L"OKUFLOW_TEST_VIDEO_AUDIO_OUTPUT", enabled, 8) != 1 || enabled[0] != L'1') {
        std::cout << "SKIP opt-in speaker playback: set OKUFLOW_TEST_VIDEO_AUDIO_OUTPUT=1\n";
        return;
    }
    VideoAudioOutput sink;
    sink.Configure(100, true, false);
    auto p = Pcm(100, 9600, 70'000'000);
    for (std::size_t frame = 0; frame < 9600; ++frame) {
        const auto sample = static_cast<std::int16_t>(1200 * std::sin(frame * 6.283185307179586 * 440 / 48000));
        for (std::size_t channel = 0; channel < 2; ++channel)
            std::memcpy(p.pcm.data() + frame * 4 + channel * 2, &sample, sizeof(sample));
    }
    Expect(sink.Push(std::move(p)), "opt-in speaker PCM admitted");
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    bool rendered{};
    while (Clock::now() < deadline) {
        const auto s = sink.Snapshot();
        if (s.available && s.clockValid && s.renderedPts100ns > 70'000'000 &&
            s.renderedPts100ns <= 72'000'000) { rendered = true; break; }
        if (!s.error.empty()) { std::cerr << s.error << '\n'; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Expect(rendered, "opt-in device clock advances through actually rendered PCM");
    sink.Configure(101, false, true);
    Expect(!sink.Snapshot().clockValid && !sink.Push(Pcm(100)), "native pause and seek fence old clock and PCM");
}
}
int main() {
    SilentUntilExplicitPlayback(); PauseSeekMuteAndCloseRevoke(); ResourceCapsAndRecovery();
    MalformedPcmFailsClosed(); ConcurrentSupersessionRemainsBounded();
    PlaybackGainPreservesSourceAndTiming(); NativeEndpointOptIn();
    std::cout << (failures ? "FAIL" : "PASS") << " video audio output contracts\n";
    return failures ? 1 : 0;
}
#endif
