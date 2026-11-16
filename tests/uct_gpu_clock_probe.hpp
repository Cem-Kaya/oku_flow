#pragma once
// Opt-in, unregistered characterization of the GPU's power management for
// paced OCR reads (docs/uct_okuread_port.md, round 13). Model-free: no engine,
// no recognizer. It measures the SM clock with an in-kernel meter
// (gpu::SampleClock) while it idles, loads the GPU in chosen duty cycles, or
// emulates the application's per-frame video work, and records each phase's
// wall-clock window so scripts/uct/gpu_clock_probe.py can attach nvidia-smi
// power-state, clock and power samples. It changes no GPU or system setting.
#include "uct_resident_diagnostic.hpp"
#include "okuflow/cuda/uct_kernels.hpp"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace okuflow::uct::uct_clock_probe_test {
#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT
using ProbeClock = std::chrono::steady_clock;

// Sleeps with a high-resolution waitable timer, so millisecond periods are
// kept without changing the process or system timer resolution.
inline void PreciseSleep(double ms)
{
    if (ms <= 0) return;
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0); // relative, 100 ns units
    if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
        return;
    }
    WaitForSingleObject(timer, INFINITE);
}

inline void SleepUntil(ProbeClock::time_point due)
{
    const auto left = std::chrono::duration<double, std::milli>(due - ProbeClock::now()).count();
    if (left > 0) PreciseSleep(left);
}

// Emulation of the application's per-frame GPU work while a live camera or
// video is shown: one NV12 1920x1080 upload from pinned memory and `passes`
// full-frame BGRA device copies (colour conversion, zoom, grading, sharpening
// and the presenter's copy are each about one read and one write of a frame).
// Its own stream and thread; started and stopped by the diagnostic, which
// joins it (test code only).
class FrameLoad {
public:
    FrameLoad(double fps, int passes) : fps_(fps), passes_(passes) {}
    ~FrameLoad() { Stop(); Release(); }
    FrameLoad(const FrameLoad&) = delete;
    FrameLoad& operator=(const FrameLoad&) = delete;
    bool Start()
    {
        if (cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking) != cudaSuccess ||
            cudaMallocHost(&host_, kUploadBytes) != cudaSuccess || cudaMalloc(&upload_, kUploadBytes) != cudaSuccess ||
            cudaMalloc(&a_, kFrameBytes) != cudaSuccess || cudaMalloc(&b_, kFrameBytes) != cudaSuccess ||
            cudaEventCreate(&begin_) != cudaSuccess || cudaEventCreate(&end_) != cudaSuccess)
            return false;
        std::memset(host_, 0x40, kUploadBytes);
        if (cudaMemset(a_, 0x20, kFrameBytes) != cudaSuccess || cudaMemset(b_, 0, kFrameBytes) != cudaSuccess) return false;
        running_ = true;
        thread_ = std::thread([this] { Run(); });
        return true;
    }
    void Stop()
    {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (stream_) cudaStreamSynchronize(stream_);
    }
    int Frames() const { return frames_; }
    int Failures() const { return failures_; }
    double MedianFrameGpuMs()
    {
        if (gpuMs_.empty()) return 0;
        std::sort(gpuMs_.begin(), gpuMs_.end());
        return gpuMs_[gpuMs_.size() / 2];
    }
    QJsonObject Json()
    {
        return {{"fps", fps_}, {"passes", passes_}, {"frames", frames_}, {"failures", failures_},
                {"medianFrameGpuMs", MedianFrameGpuMs()},
                {"emulates", "NV12 1920x1080 upload plus full-frame BGRA device copies per frame, own stream"}};
    }

private:
    static constexpr std::size_t kUploadBytes = 1920ull * 1080 * 3 / 2;
    static constexpr std::size_t kFrameBytes = 1920ull * 1080 * 4;
    void Run()
    {
        const auto period = std::chrono::duration_cast<ProbeClock::duration>(std::chrono::duration<double>(1.0 / fps_));
        auto due = ProbeClock::now();
        bool timing = false;
        while (running_) {
            // Previous frame's GPU time, read without waiting for it.
            if (timing && cudaEventQuery(end_) == cudaSuccess) {
                float ms{};
                if (cudaEventElapsedTime(&ms, begin_, end_) == cudaSuccess && gpuMs_.size() < 100000) gpuMs_.push_back(ms);
                timing = false;
            }
            const bool record = !timing;
            bool ok = !record || cudaEventRecord(begin_, stream_) == cudaSuccess;
            ok = ok && cudaMemcpyAsync(upload_, host_, kUploadBytes, cudaMemcpyHostToDevice, stream_) == cudaSuccess;
            for (int pass = 0; ok && pass < passes_; ++pass)
                ok = cudaMemcpyAsync(pass % 2 ? a_ : b_, pass % 2 ? b_ : a_, kFrameBytes,
                                     cudaMemcpyDeviceToDevice, stream_) == cudaSuccess;
            if (ok && record) {
                ok = cudaEventRecord(end_, stream_) == cudaSuccess;
                timing = ok;
            }
            ++frames_;
            failures_ += !ok;
            due += period;
            SleepUntil(due);
        }
    }
    void Release()
    {
        if (begin_) cudaEventDestroy(begin_);
        if (end_) cudaEventDestroy(end_);
        if (a_) cudaFree(a_);
        if (b_) cudaFree(b_);
        if (upload_) cudaFree(upload_);
        if (host_) cudaFreeHost(host_);
        if (stream_) cudaStreamDestroy(stream_);
        begin_ = end_ = nullptr; a_ = b_ = upload_ = host_ = nullptr; stream_ = nullptr;
    }
    double fps_;
    int passes_;
    std::atomic<bool> running_{};
    std::thread thread_;
    cudaStream_t stream_{};
    void *host_{}, *upload_{}, *a_{}, *b_{};
    cudaEvent_t begin_{}, end_{};
    int frames_{}, failures_{};
    std::vector<float> gpuMs_;
};

// Parses "fps:passes" (for example "30:6"); empty or "0" is no load.
inline std::optional<std::pair<double, int>> ParseFrameLoad(const QString& text, bool& valid)
{
    valid = true;
    if (text.isEmpty() || text == "0") return std::nullopt;
    const auto parts = text.split(QLatin1Char(':'));
    bool fpsOk = false, passesOk = false;
    const double fps = parts.size() == 2 ? parts[0].toDouble(&fpsOk) : 0;
    const int passes = parts.size() == 2 ? parts[1].toInt(&passesOk) : 0;
    valid = fpsOk && passesOk && fps >= 1 && fps <= 240 && passes >= 0 && passes <= 64;
    if (!valid) return std::nullopt;
    return std::pair{fps, passes};
}

inline void ClockProbe()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_CLOCK_PROBE") != "1")
        QSKIP("Explicit GPU clock characterization not selected.");
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_CLOCK_PROBE_EVIDENCE");
    QVERIFY2(QDir::isAbsolutePath(evidence) && !QFileInfo::exists(evidence),
             "OKUFLOW_UCT_CLOCK_PROBE_EVIDENCE must name a new absolute file.");
    const auto plan = qEnvironmentVariable("OKUFLOW_UCT_CLOCK_PROBE_PLAN", "idle,stepdown,stepup,duty,frames")
                          .split(QLatin1Char(','), Qt::SkipEmptyParts);
    QVERIFY(cudaSetDevice(0) == cudaSuccess);
    cudaDeviceProp properties{};
    QVERIFY(cudaGetDeviceProperties(&properties, 0) == cudaSuccess);
    const int fullBlocks = properties.multiProcessorCount * 2;
    uct_resident_test::DeviceStream meterStream, loadStream; // destroyed after the buffers below
    QVERIFY(meterStream.value && loadStream.value);
    gpu::ClockSample* device{};
    gpu::ClockSample* host{};
    QVERIFY(cudaMalloc(&device, sizeof(gpu::ClockSample)) == cudaSuccess);
    QVERIFY(cudaMallocHost(&host, sizeof(gpu::ClockSample)) == cudaSuccess);
    const auto releaseBuffers = qScopeGuard([&] {
        cudaDeviceSynchronize();
        cudaFree(device);
        cudaFreeHost(host);
    });
    const auto origin = ProbeClock::now();
    const auto sinceMs = [&] { return std::chrono::duration<double, std::milli>(ProbeClock::now() - origin).count(); };
    // One 100 us meter on `stream`; returns MHz (0 on failure).
    const auto meter = [&](cudaStream_t stream) {
        gpu::SampleClock(device, 100000, stream);
        if (cudaMemcpyAsync(host, device, sizeof(gpu::ClockSample), cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
            cudaStreamSynchronize(stream) != cudaSuccess || host->nanoseconds == 0)
            return 0.0;
        return double(host->cycles) * 1000.0 / double(host->nanoseconds);
    };
    const auto hold = [&](double ms) {
        gpu::KeepWarm(nullptr, static_cast<unsigned long long>(ms * 1e6), fullBlocks, loadStream.value);
        cudaStreamSynchronize(loadStream.value);
    };
    QJsonArray phases;
    const auto phase = [&](const QString& name, QJsonObject data, qint64 start) {
        data["name"] = name;
        data["startUnixMs"] = start;
        data["endUnixMs"] = QDateTime::currentMSecsSinceEpoch();
        phases.push_back(data);
    };
    const auto now = [] { return QDateTime::currentMSecsSinceEpoch(); };
    // Duty pattern: every `periodMs`, `busyFraction` of the period in `blocks`
    // spinning warps; one meter every 250 ms after the burst.
    const auto duty = [&](double periodMs, double busyFraction, int blocks, double runMs, unsigned sleepNs = 0) {
        QJsonArray trajectory;
        const auto period = std::chrono::duration_cast<ProbeClock::duration>(
            std::chrono::duration<double, std::milli>(periodMs));
        const auto begin = ProbeClock::now();
        auto due = begin;
        double nextMeter = 0;
        int launches = 0;
        while (std::chrono::duration<double, std::milli>(ProbeClock::now() - begin).count() < runMs) {
            gpu::KeepWarm(nullptr, static_cast<unsigned long long>(periodMs * busyFraction * 1e6), blocks, loadStream.value,
                          sleepNs);
            ++launches;
            const double at = std::chrono::duration<double, std::milli>(ProbeClock::now() - begin).count();
            if (at >= nextMeter) {
                trajectory.push_back(QJsonArray{at, meter(loadStream.value)});
                nextMeter += 250;
            }
            due += period;
            SleepUntil(due);
        }
        cudaStreamSynchronize(loadStream.value);
        return QJsonObject{{"periodMs", periodMs}, {"busyFraction", busyFraction}, {"blocks", blocks},
                           {"sleepNs", double(sleepNs)}, {"launches", launches}, {"trajectory", trajectory}};
    };

    if (plan.contains("idle")) {
        const auto start = now();
        PreciseSleep(4000);
        phase("idle", {{"ms", 4000}}, start);
    }
    if (plan.contains("stepdown")) {
        // Clock after an idle gap that follows a busy second, both directions.
        const QList<int> gaps{0, 10, 25, 50, 75, 100, 150, 200, 300, 400, 500, 750, 1000, 1500, 2000, 3000, 5000};
        for (int repeat = 0; repeat < 2; ++repeat) {
            for (int i = 0; i < gaps.size(); ++i) {
                const int gap = repeat ? gaps[gaps.size() - 1 - i] : gaps[i];
                hold(500);
                const auto start = now();
                PreciseSleep(gap);
                const double first = meter(meterStream.value);
                double tenth = 0;
                for (int k = 0; k < 9; ++k) tenth = meter(meterStream.value);
                phase("stepdown", {{"gapMs", gap}, {"repeat", repeat}, {"firstMHz", first}, {"tenthMHz", tenth}}, start);
            }
        }
    }
    if (plan.contains("stepup")) {
        // From a settled low state, continuous load: a single warp (the meter
        // itself, back to back) or every SM; the clock every 5 ms.
        for (int repeat = 0; repeat < 2; ++repeat)
            for (const int idle : {1000, 5000})
                for (const bool full : {false, true}) {
                    hold(300);
                    PreciseSleep(idle);
                    const auto start = now();
                    if (full) gpu::KeepWarm(nullptr, 1500000000ull, fullBlocks, loadStream.value);
                    QJsonArray trajectory;
                    const auto begin = ProbeClock::now();
                    double kept = -1e9;
                    for (;;) {
                        const double at = std::chrono::duration<double, std::milli>(ProbeClock::now() - begin).count();
                        if (at > 1500) break;
                        const double mhz = meter(meterStream.value);
                        if (at - kept >= 5) {
                            trajectory.push_back(QJsonArray{at, mhz});
                            kept = at;
                        }
                    }
                    cudaStreamSynchronize(loadStream.value);
                    phase("stepup", {{"idleMs", idle}, {"load", full ? "all SMs" : "one warp"}, {"repeat", repeat},
                                     {"trajectory", trajectory}}, start);
                }
    }
    if (plan.contains("duty")) {
        const QList<double> fractions{0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1.0};
        for (const double periodMs : {16.0, 33.0, 100.0})
            for (const double fraction : fractions) {
                hold(500);
                const auto start = now();
                auto data = duty(periodMs, fraction, 1, 4000);
                data["from"] = "high";
                phase("duty", data, start);
            }
        for (const double periodMs : {16.0, 33.0})
            for (const double fraction : {0.02, 0.1}) {
                hold(500);
                const auto start = now();
                auto data = duty(periodMs, fraction, fullBlocks, 4000);
                data["from"] = "high";
                phase("duty", data, start);
            }
        for (const double periodMs : {16.0, 33.0, 100.0})
            for (const double fraction : {0.05, 0.25, 1.0}) {
                hold(300);
                PreciseSleep(3000);
                const auto start = now();
                auto data = duty(periodMs, fraction, 1, 3000);
                data["from"] = "idle 3000 ms";
                phase("duty", data, start);
            }
    }
    if (plan.contains("refine")) {
        // The holding threshold between 25 and 50 percent, the step-up
        // threshold from a low state, and a resident but napping warp.
        struct Case { double periodMs, fraction; unsigned sleepNs; bool fromIdle; };
        const QList<Case> cases{
            {16, 0.30, 0, false}, {16, 0.35, 0, false}, {16, 0.40, 0, false}, {16, 0.45, 0, false},
            {33, 0.40, 0, false}, {16, 1.0, 0, false}, {16, 1.0, 10000, false}, {100, 1.0, 10000, false},
            {16, 0.5, 10000, false}, {16, 0.5, 0, true}, {16, 0.75, 0, true}, {16, 1.0, 10000, true},
            {100, 1.0, 10000, true}, {16, 0.6, 10000, true}};
        for (const auto& item : cases) {
            if (item.fromIdle) { hold(300); PreciseSleep(3000); }
            else hold(500);
            const auto start = now();
            auto data = duty(item.periodMs, item.fraction, 1, 4000, item.sleepNs);
            data["from"] = item.fromIdle ? "idle 3000 ms" : "high";
            phase("duty", data, start);
        }
    }
    if (plan.contains("frames")) {
        struct Load { double fps; int passes; };
        for (const Load load : {Load{30, 6}, Load{60, 6}, Load{30, 16}})
            for (const bool fromHigh : {false, true}) {
                if (fromHigh) hold(500);
                else { hold(300); PreciseSleep(3000); }
                const auto start = now();
                FrameLoad frames(load.fps, load.passes);
                QVERIFY2(frames.Start(), "Frame load emulation could not start.");
                QJsonArray trajectory;
                const auto begin = ProbeClock::now();
                for (double at = 0; at < 5000; at = std::chrono::duration<double, std::milli>(ProbeClock::now() - begin).count()) {
                    trajectory.push_back(QJsonArray{at, meter(meterStream.value)});
                    PreciseSleep(250);
                }
                frames.Stop();
                auto data = frames.Json();
                data["from"] = fromHigh ? "high" : "idle 3000 ms";
                data["trajectory"] = trajectory;
                phase("frames", data, start);
            }
    }
    const QJsonObject report{{"schema", 1}, {"gpu", QString::fromLatin1(properties.name)},
        {"multiprocessors", properties.multiProcessorCount}, {"plan", plan.join(',')},
        {"meter", "one warp, 100 us, SM cycles over %globaltimer nanoseconds"},
        {"phases", phases}, {"elapsedMs", sinceMs()}};
    QVERIFY(uct_batch_test::Save(evidence, report));
}
#else
inline void ClockProbe()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_CLOCK_PROBE") != "1")
        QSKIP("Explicit GPU clock characterization not selected.");
    QFAIL("GPU clock characterization needs the CUDA and TensorRT test build.");
}
#endif
} // namespace okuflow::uct::uct_clock_probe_test
