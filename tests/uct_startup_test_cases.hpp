#pragma once

#include "okuflow/common/uct_paddle.hpp"
#include <QtTest>
#include <chrono>
#include <mutex>

namespace uct_startup_test {

struct Calls {
    int initialized{}, recognized{};
    bool preparing{}, unavailable{}, fail{}, inputFault{};
};

class Backend final : public okuflow::uct::Recognizer {
public:
    Backend(Calls& calls, QString name, bool gpu = false)
        : calls_(calls), name_(std::move(name)), gpu_(gpu) {}
    okuflow::uct::BackendStatus Initialize() override {
        ++calls_.initialized;
        return Status();
    }
    okuflow::uct::BackendStatus Status() const override {
        return {calls_.unavailable ? okuflow::uct::BackendState::kUnavailable
                                   : okuflow::uct::BackendState::kReady,
                name_, {}, {}, gpu_};
    }
    bool Preparing() const override { return calls_.preparing; }
    bool WantsDeviceInput() const override { return gpu_; }
    okuflow::uct::RecognitionOutput Recognize(const QImage&,
                                               const std::atomic<bool>&) override {
        ++calls_.recognized;
        okuflow::uct::RecognitionOutput output;
        output.ok = !calls_.fail && !calls_.inputFault;
        output.inputFault = calls_.inputFault;
        output.error = calls_.fail ? QStringLiteral("scripted runtime failure") : QString{};
        return output;
    }
private:
    Calls& calls_;
    QString name_;
    bool gpu_;
};

inline std::unique_ptr<okuflow::uct::Recognizer> Make(Calls& gpu, Calls& cpu, Calls& windows)
{
    std::vector<std::unique_ptr<okuflow::uct::Recognizer>> backends;
    backends.push_back(std::make_unique<Backend>(gpu, "GPU", true));
    backends.push_back(std::make_unique<Backend>(cpu, "Paddle CPU"));
    backends.push_back(std::make_unique<Backend>(windows, "Windows CPU"));
    return okuflow::uct::CreateAutomaticRecognizer(std::move(backends), 2);
}

inline void BridgeDefersCpuAndPromotes()
{
    using namespace okuflow::uct;
    Calls gpu, cpu, windows;
    gpu.preparing = true;
    auto backend = Make(gpu, cpu, windows);
    QCOMPARE(backend->Initialize().state, BackendState::kReady);
    QCOMPARE(cpu.initialized, 0);
    std::atomic<bool> cancelled{};
    const auto temporary = backend->Recognize({}, cancelled);
    QVERIFY(temporary.ok);
    QVERIFY(temporary.backendPreparing);
    QCOMPARE(backend->Status().name, QString("Windows CPU"));
    QVERIFY(!backend->Status().usesGpu);
    QVERIFY(!backend->WantsDeviceInput());
    QCOMPARE(windows.recognized, 1);
    QCOMPARE(cpu.initialized, 0);
    QCOMPARE(cpu.recognized, 0);
    gpu.preparing = false;
    QVERIFY(backend->WantsDeviceInput());
    const auto preferred = backend->Recognize({}, cancelled);
    QVERIFY(preferred.ok);
    QVERIFY(!preferred.backendPreparing);
    QCOMPARE(backend->Status().name, QString("GPU"));
    QVERIFY(backend->Status().usesGpu);
    QCOMPARE(gpu.recognized, 1);
    QCOMPARE(cpu.initialized, 0);
    QCOMPARE(windows.recognized, 1);
    gpu.fail = true;
    QVERIFY(backend->Recognize({}, cancelled).ok);
    QCOMPARE(backend->Status().name, QString("Paddle CPU"));
    QCOMPARE(cpu.initialized, 1);
    QCOMPARE(cpu.recognized, 1);
    QCOMPARE(windows.recognized, 1);
}

inline void MissingBridgeWaitsWithoutStartingCpu()
{
    using namespace okuflow::uct;
    Calls gpu, cpu, windows;
    gpu.preparing = true;
    windows.unavailable = true;
    auto backend = Make(gpu, cpu, windows);
    QCOMPARE(backend->Initialize().state, BackendState::kReady);
    std::atomic<bool> cancelled{};
    for (int i = 0; i < 2; ++i) {
        const auto output = backend->Recognize({}, cancelled);
        QVERIFY(output.backendPreparing);
        QVERIFY(!output.ok);
    }
    QCOMPARE(cpu.initialized, 0);
    QCOMPARE(windows.initialized, 1);
    gpu.preparing = false;
    QVERIFY(backend->Recognize({}, cancelled).ok);
    QCOMPARE(gpu.recognized, 1);
    QCOMPARE(cpu.initialized, 0);
    gpu.fail = true;
    cpu.fail = true;
    QVERIFY(!backend->Recognize({}, cancelled).ok);
    QCOMPARE(cpu.initialized, 1);
    QCOMPARE(windows.initialized, 1);
    QCOMPARE(windows.recognized, 0);
}

inline void FailedPreparationUsesCpuThenRetainsBridge()
{
    using namespace okuflow::uct;
    Calls gpu, cpu, windows;
    gpu.preparing = true;
    auto backend = Make(gpu, cpu, windows);
    QCOMPARE(backend->Initialize().state, BackendState::kReady);
    std::atomic<bool> cancelled{};
    QVERIFY(backend->Recognize({}, cancelled).ok);
    gpu.preparing = false;
    gpu.unavailable = true;
    QVERIFY(backend->Recognize({}, cancelled).ok);
    QCOMPARE(cpu.initialized, 1);
    QCOMPARE(cpu.recognized, 1);
    QCOMPARE(gpu.recognized, 0);
    cpu.fail = true;
    QVERIFY(backend->Recognize({}, cancelled).ok);
    QCOMPARE(backend->Status().name, QString("Windows CPU"));
    QCOMPARE(windows.initialized, 1);
    QCOMPARE(windows.recognized, 2);
    QCOMPARE(gpu.initialized, 1);
    QCOMPARE(cpu.initialized, 1);
}

inline void BridgeCancellationAndInputFaultDoNotRetire()
{
    using namespace okuflow::uct;
    Calls gpu, cpu, windows;
    gpu.preparing = true;
    auto backend = Make(gpu, cpu, windows);
    QCOMPARE(backend->Initialize().state, BackendState::kReady);
    std::atomic<bool> cancelled{true};
    QVERIFY(backend->Recognize({}, cancelled).cancelled);
    QCOMPARE(windows.initialized, 0);
    QCOMPARE(cpu.initialized, 0);
    cancelled = false;
    windows.inputFault = true;
    QVERIFY(backend->Recognize({}, cancelled).inputFault);
    windows.inputFault = false;
    QVERIFY(backend->Recognize({}, cancelled).ok);
    QCOMPARE(windows.initialized, 1);
    QCOMPARE(windows.recognized, 2);
    QCOMPARE(cpu.initialized, 0);
    cancelled = true;
    gpu.preparing = false;
    gpu.unavailable = true;
    QVERIFY(backend->Recognize({}, cancelled).cancelled);
    QCOMPARE(windows.recognized, 2);
    QCOMPARE(cpu.initialized, 0);
    cancelled = false;
    QVERIFY(backend->Recognize({}, cancelled).ok);
    QCOMPARE(gpu.recognized, 0);
    QCOMPARE(cpu.initialized, 1);
}

// Explicit private-image diagnostic, deliberately absent from CTest. Models
// and compatible engine plans must already be prepared; never clear the cache
// or use this entry point as a cold-compilation benchmark.
inline void AutomaticStartupDiagnostic()
{
    using namespace okuflow::uct;
    if (qEnvironmentVariable("OKUFLOW_UCT_STARTUP_DIAGNOSTIC") != "1")
        QSKIP("Explicit automatic startup diagnostic not selected.");
    const auto path = qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE");
    QVERIFY2(!path.isEmpty(), "OKUFLOW_UCT_TEST_IMAGE must select a private source image.");
    const QImage image(path);
    QVERIFY2(!image.isNull(), qPrintable(path));
    const bool requireGpu = qEnvironmentVariable("OKUFLOW_TEST_PADDLE_GPU") == "1";
    constexpr double deadlineMs = 30000;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [started] {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    };
    struct Completion {
        RecognitionResult result;
        BackendStatus backend;
        double elapsedMs{};
    };
    struct State {
        std::mutex mutex;
        BackendStatus backend;
        double readyMs{-1};
        std::vector<Completion> completed;
    };
    const auto state = std::make_shared<State>();
    RecognitionScheduler scheduler(
        CreateAutomaticRecognizer(),
        [state, elapsed](RecognitionResult result) {
            const auto completedAt = elapsed();
            std::lock_guard lock(state->mutex);
            state->completed.push_back({std::move(result), state->backend, completedAt});
        },
        [state, elapsed](BackendStatus backend) {
            std::lock_guard lock(state->mutex);
            if (state->readyMs < 0 && backend.state == BackendState::kReady)
                state->readyMs = elapsed();
            state->backend = std::move(backend);
        });
    SourceStamp stamp;
    stamp.sourceEpoch = 1;
    stamp.geometryGeneration = 1;
    stamp.frameSeq = 1; // every job observes the same immutable image
    stamp.viewportWidth = image.width();
    stamp.viewportHeight = image.height();
    stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
    QVERIFY(scheduler.Submit(stamp, image) != 0);
    bool pending = true, firstUsable = false, reachedGpu = false, terminalCpu = false;
    double nextSubmissionMs{};
    std::optional<Completion> lastUsable;
    while (elapsed() < deadlineMs) {
        std::vector<Completion> completed;
        double readyMs{};
        {
            std::lock_guard lock(state->mutex);
            completed.swap(state->completed);
            readyMs = state->readyMs;
        }
        for (auto& completion : completed) {
            pending = false;
            const auto& result = completion.result;
            int candidates = 0;
            for (const auto& line : result.output.lines)
                for (const auto& word : line.words)
                    candidates += word.disposition == TokenDisposition::kCandidate;
            qInfo().noquote()
                << "Automatic startup candidate completion job=" << result.jobId
                << "elapsedMs=" << completion.elapsedMs << "workerMs=" << result.durationMs
                << "readyStatusMs=" << readyMs << "backend=" << completion.backend.name
                << "usesGpu=" << completion.backend.usesGpu
                << "backendPreparing=" << result.output.backendPreparing
                << "ok=" << result.output.ok << "inputFault=" << result.output.inputFault
                << "candidateWords=" << candidates << "error=" << result.output.error;
            const bool usable = result.output.ok && !result.output.cancelled &&
                                result.reference.IsValid() && candidates > 0;
            if (usable) {
                if (!firstUsable) {
                    firstUsable = true;
                    qInfo().noquote() << "First usable complete candidate result elapsedMs="
                                     << completion.elapsedMs << "backend=" << completion.backend.name
                                     << "usesGpu=" << completion.backend.usesGpu;
                }
                reachedGpu = completion.backend.usesGpu && !result.output.backendPreparing;
                terminalCpu = !reachedGpu && !result.output.backendPreparing;
                lastUsable = std::move(completion);
            }
            nextSubmissionMs = elapsed() + 500;
        }
        if (reachedGpu || terminalCpu) break;
        if (!pending && elapsed() >= nextSubmissionMs) {
            QVERIFY2(scheduler.Submit(stamp, image) != 0,
                     qPrintable(scheduler.Status().detail));
            pending = true;
        }
        QTest::qWait(10);
    }
    scheduler.Cancel();
    QVERIFY2(firstUsable, "No usable complete candidate result within the 30-second startup horizon.");
    if (lastUsable) {
        const auto& completion = *lastUsable;
        qInfo().noquote() << "Automatic startup final elapsedMs=" << completion.elapsedMs
                         << "backend=" << completion.backend.name
                         << "usesGpu=" << completion.backend.usesGpu
                         << "backendPreparing=" << completion.result.output.backendPreparing
                         << "detail=" << completion.backend.detail;
        for (const auto& line : completion.result.output.lines) for (const auto& word : line.words) {
            if (!word.colors || (word.text.compare("All", Qt::CaseInsensitive) != 0 &&
                word.text.compare("assets", Qt::CaseInsensitive) != 0 &&
                word.text.compare("Robots", Qt::CaseInsensitive) != 0)) continue;
            qInfo().noquote() << "Automatic startup local word color text=" << word.text
                             << "foreground=" << word.colors->foreground.name()
                             << "background=" << word.colors->background.name();
        }
    }
    QVERIFY2(!requireGpu || reachedGpu,
             "Explicit GPU diagnostic did not reach a usable preferred GPU result within 30 seconds.");
    qInfo("Candidate completion only: glyph admission, native display and UI latency were not measured.");
}

} // namespace uct_startup_test
