#pragma once
// Opt-in, unregistered corpus timing for same-binary switch comparisons.
//
// One process reads every selected image through the production GPU factory
// and RecognitionScheduler::SubmitDevice: one first read, then the requested
// warm reads, each on a fresh owned analysis capture completed before
// submission. scripts/uct/perf_measure.py runs these processes in reversed
// cohorts and compares arms. It never prepares an engine, changes a default
// or runs in ordinary CTest. See docs/uct_corpus_timing.md.
#include "uct_resident_diagnostic.hpp"
#include "uct_gpu_clock_probe.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#ifdef small
#undef small // rpcndr.h; later headers use it as a name
#endif

namespace okuflow::uct::uct_corpus_test {
#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT
// Measurement only (round 13): ID3D12Device::SetStablePowerState on the CUDA
// device's adapter for the life of this test process. It needs Windows
// Developer Mode, applies to the whole adapter while the device lives and is
// documented for profiling, so the application never calls it. Loaded at run
// time; the state ends when the device is released (here, or at exit).
class StablePowerState {
public:
    bool Enable(const char (&luid)[8])
    {
        dxgi_ = LoadLibraryW(L"dxgi.dll");
        d3d12_ = LoadLibraryW(L"d3d12.dll");
        if (!dxgi_ || !d3d12_) return Fail("dxgi.dll or d3d12.dll unavailable");
        using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
        const auto createFactory = reinterpret_cast<CreateFactory>(GetProcAddress(dxgi_, "CreateDXGIFactory1"));
        const auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12_, "D3D12CreateDevice"));
        if (!createFactory || !createDevice) return Fail("entry points unavailable");
        IDXGIFactory4* factory{};
        if (FAILED(createFactory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) return Fail("no DXGI factory");
        LUID id{};
        std::memcpy(&id, luid, sizeof(id));
        IDXGIAdapter1* adapter{};
        const HRESULT found = factory->EnumAdapterByLuid(id, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
        factory->Release();
        if (FAILED(found)) return Fail("no adapter with the CUDA device's LUID");
        const HRESULT created = createDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                             reinterpret_cast<void**>(&device_));
        adapter->Release();
        if (FAILED(created)) return Fail("D3D12CreateDevice failed");
        result_ = device_->SetStablePowerState(TRUE);
        detail_ = QStringLiteral("SetStablePowerState(TRUE) hr=0x%1, device removed reason 0x%2")
                      .arg(quint32(result_), 8, 16, QLatin1Char('0'))
                      .arg(quint32(device_->GetDeviceRemovedReason()), 8, 16, QLatin1Char('0'));
        return SUCCEEDED(result_);
    }
    ~StablePowerState()
    {
        if (device_) {
            if (SUCCEEDED(result_)) device_->SetStablePowerState(FALSE);
            device_->Release();
        }
        if (d3d12_) FreeLibrary(d3d12_);
        if (dxgi_) FreeLibrary(dxgi_);
    }
    QString Detail() const { return detail_; }

private:
    bool Fail(const char* why)
    {
        detail_ = QString::fromLatin1(why);
        return false;
    }
    HMODULE dxgi_{}, d3d12_{};
    ID3D12Device* device_{};
    HRESULT result_{E_FAIL};
    QString detail_;
};

inline void CorpusTiming()
{
    using namespace uct_resident_test;
    if (qEnvironmentVariable("OKUFLOW_UCT_CORPUS_TIMING") != "1")
        QSKIP("Explicit corpus timing not selected.");
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_EVIDENCE");
    QVERIFY2(QDir::isAbsolutePath(evidence) && !QFileInfo::exists(evidence),
             "OKUFLOW_UCT_CORPUS_EVIDENCE must name a new absolute file.");
    QStringList paths;
    for (const auto& path : qEnvironmentVariable("OKUFLOW_UCT_CORPUS_IMAGES").split(QLatin1Char(';'), Qt::SkipEmptyParts))
        paths.push_back(QFileInfo(path.trimmed()).absoluteFilePath());
    QVERIFY2(!paths.isEmpty() && paths.size() <= 64, "Select 1-64 images with OKUFLOW_UCT_CORPUS_IMAGES.");
    bool ok = true;
    const int passes = qEnvironmentVariableIsSet("OKUFLOW_UCT_CORPUS_PASSES")
        ? qEnvironmentVariable("OKUFLOW_UCT_CORPUS_PASSES").toInt(&ok) : 10;
    QVERIFY2(ok && passes >= 1 && passes <= 50, "OKUFLOW_UCT_CORPUS_PASSES must be 1-50.");
    const int gapMs = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_GAP_MS").toInt();
    QVERIFY2(gapMs >= 0 && gapMs <= 5000, "OKUFLOW_UCT_CORPUS_GAP_MS must be 0-5000.");
    const auto stampKind = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_STAMP");
    QVERIFY2(stampKind.isEmpty() || stampKind == "camera" || stampKind == "media",
             "OKUFLOW_UCT_CORPUS_STAMP must be camera or media.");
    const bool media = stampKind == "media";
    const auto tiers = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_PAIR", "medium:medium").split(QLatin1Char(':'));
    const auto detector = tiers.size() == 2 ? ParseModelTier(tiers[0]) : std::nullopt;
    const auto recognizerTier = tiers.size() == 2 ? ParseModelTier(tiers[1]) : std::nullopt;
    QVERIFY2(detector && recognizerTier, "OKUFLOW_UCT_CORPUS_PAIR must be detector:recognizer.");
    const auto cache = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_ENGINE_CACHE");
    QVERIFY2(cache.isEmpty() || QDir::isAbsolutePath(cache), "The engine cache override must be absolute.");
    const bool profile = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_PROFILE") == "1";
    // Round 13: the private keep-warm switch as the application reads it, an
    // optional emulation of the application's per-frame video GPU work, and an
    // idle tail after the last read that shows keep-warm stopping.
    const auto keepWarm = KeepWarmFromEnvironment();
    QVERIFY2(keepWarm.error.isEmpty(), qPrintable(keepWarm.error));
    bool frameLoadValid = true;
    const auto frameLoadShape = uct_clock_probe_test::ParseFrameLoad(
        qEnvironmentVariable("OKUFLOW_UCT_CORPUS_FRAME_LOAD"), frameLoadValid);
    QVERIFY2(frameLoadValid, "OKUFLOW_UCT_CORPUS_FRAME_LOAD must be fps:passes (1-240, 0-64).");
    const int tailMs = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_TAIL_MS").toInt();
    QVERIFY2(tailMs >= 0 && tailMs <= 20000, "OKUFLOW_UCT_CORPUS_TAIL_MS must be 0-20000.");
    const bool stablePower = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_STABLE_POWER") == "1";
    // Round 14: the content changes after every paced read (a playing video
    // whose text keeps changing), and the session scenario
    // refreshMs,changeMs,settleMs,durationMs (see the read loop below).
    const bool changeEach = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_CHANGE_EACH") == "1";
    struct Session { int refreshMs{}, changeMs{}, settleMs{}, durationMs{}; };
    std::optional<Session> session;
    if (const auto text = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_SESSION"); !text.isEmpty()) {
        const auto parts = text.split(QLatin1Char(','));
        bool valid = parts.size() == 4;
        int values[4]{};
        for (int i = 0; valid && i < 4; ++i) values[i] = parts[i].toInt(&valid);
        valid = valid && values[0] >= 0 && values[0] <= 10000 && values[1] >= 0 && values[1] <= 60000 &&
                values[2] >= 0 && values[2] <= 5000 && values[3] >= 1000 && values[3] <= 600000 &&
                (values[0] > 0 || values[1] > 0) && (!values[1] || values[2] < values[1]);
        QVERIFY2(valid, "OKUFLOW_UCT_CORPUS_SESSION must be refreshMs,changeMs,settleMs,durationMs "
                        "(0-10000, 0-60000, 0-5000 below changeMs, 1000-600000; refresh or change set).");
        session = Session{values[0], values[1], values[2], values[3]};
    }
    QVERIFY2(!(session && (gapMs || changeEach)), "A session sets its own pacing; drop the gap and change-each.");

    QVERIFY(cudaSetDevice(0) == cudaSuccess);
    cudaDeviceProp properties{};
    QVERIFY(cudaGetDeviceProperties(&properties, 0) == cudaSuccess);
    int driver{};
    QVERIFY(cudaDriverGetVersion(&driver) == cudaSuccess);
    std::unique_ptr<StablePowerState> stable;
    if (stablePower) {
        stable = std::make_unique<StablePowerState>();
        QVERIFY2(stable->Enable(properties.luid), qPrintable(stable->Detail()));
    }

    struct Scene {
        QString path, sha256;
        QImage image;
        void* device{};
    };
    DeviceStream stream; // destroyed last
    DeviceUploads uploads;
    QVERIFY2(stream.value, "Fixture CUDA stream unavailable.");
    uploads.stream = stream.value;
    std::vector<Scene> scenes;
    for (const auto& path : paths) {
        Scene scene;
        scene.path = path;
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        const auto bytes = file.readAll();
        scene.sha256 = Hex(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        scene.image = QImage::fromData(bytes).convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(!scene.image.isNull() && scene.image.width() <= 8192 && scene.image.height() <= 8192, qPrintable(path));
        scene.device = uploads.Allocate(std::size_t(scene.image.sizeInBytes()));
        QVERIFY2(scene.device, "Source upload allocation failed.");
        QVERIFY(cudaMemcpyAsync(scene.device, scene.image.constBits(), std::size_t(scene.image.sizeInBytes()),
                                cudaMemcpyHostToDevice, stream.value) == cudaSuccess);
        scenes.push_back(std::move(scene));
    }
    QVERIFY(cudaStreamSynchronize(stream.value) == cudaSuccess);

    uct_batch_test::Environment profiling("OKUFLOW_UCT_TRT_PROFILE", profile ? "1" : "0");
    ProfileCapture capture;
    uct_batch_test::RetirementReceipts retirement;
    const auto mailbox = std::make_shared<Mailbox>();
    auto pool = std::make_unique<AnalysisPool>();
    const auto constructed = Clock::now();
    auto scheduler = std::make_unique<RecognitionScheduler>(
        CreateProductionTensorRtRecognizer(*detector, *recognizerTier, retirement.Observer(), cache),
        [mailbox](RecognitionResult result) {
            const auto at = Clock::now();
            {
                std::lock_guard lock(mailbox->mutex);
                mailbox->completed = Completion{std::move(result), mailbox->backend, at};
            }
            mailbox->ready.notify_all();
        },
        [mailbox](BackendStatus status) {
            std::lock_guard lock(mailbox->mutex);
            mailbox->backend = std::move(status);
        },
        2, ModelTuning{}.minimumWordHeightPx, false, false);
    if (keepWarm.settings.Enabled()) scheduler->SetKeepWarm(keepWarm.settings);
    std::unique_ptr<uct_clock_probe_test::FrameLoad> frameLoad;
    if (frameLoadShape) {
        frameLoad = std::make_unique<uct_clock_probe_test::FrameLoad>(frameLoadShape->first, frameLoadShape->second);
        QVERIFY2(frameLoad->Start(), "Frame load emulation could not start.");
    }

    std::uint64_t frame = 1;
    struct Read {
        std::optional<Completion> completion;
        MeteredImage::Meters meters;
        double submitToCallbackMs{};
        qint64 submittedUnixMs{}; // wall clock, to align reads with external GPU-state samples
        QVector<QJsonObject> profiles;
        QString error;
    };
    const auto read = [&](int sceneIndex) {
        Read result;
        const auto& scene = scenes[std::size_t(sceneIndex)];
        const int w = scene.image.width(), h = scene.image.height();
        SharedDeviceImage owned;
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (!owned) {
            owned = pool->Capture(scene.device, std::size_t(w) * 4, w, h, stream.value,
                                  std::uint64_t(sceneIndex) + 1, frame, 1);
            if (owned) break;
            if (pool->HasFailure() || Clock::now() >= deadline) {
                result.error = "The analysis pool could not supply an owned capture.";
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (cudaStreamSynchronize(stream.value) != cudaSuccess) {
            result.error = "The analysis copy did not complete.";
            return result;
        }
        const auto metered = std::make_shared<MeteredImage>(std::move(owned));
        SourceStamp stamp;
        stamp.sourceEpoch = metered->sourceEpoch;
        stamp.geometryGeneration = 1;
        stamp.frameSeq = frame++;
        stamp.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        stamp.viewportWidth = std::uint32_t(w);
        stamp.viewportHeight = std::uint32_t(h);
        stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
        stamp.liveCameraCapture = !media;
        capture.Take();
        result.submittedUnixMs = QDateTime::currentMSecsSinceEpoch();
        const auto submitted = Clock::now();
        if (!scheduler->SubmitDevice(stamp, metered)) {
            result.error = "Submission refused: " + scheduler->Status().detail;
            return result;
        }
        result.completion = Await(*mailbox, submitted + std::chrono::seconds(60));
        if (!result.completion) {
            result.error = "A read exceeded 60 seconds.";
            return result;
        }
        result.submitToCallbackMs = Ms(result.completion->at - submitted);
        result.meters = metered->Read();
        result.profiles = capture.Take();
        return result;
    };

    // Startup: cached plan load plus the first inference. Never a timed read.
    std::optional<Read> first;
    int preparingPolls = 0;
    while (!first && Ms(Clock::now() - constructed) < 120000) {
        auto attempt = read(0);
        QVERIFY2(attempt.completion, qPrintable(attempt.error));
        if (attempt.completion->result.output.backendPreparing && !attempt.completion->result.output.ok) {
            ++preparingPolls;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else first = std::move(attempt);
    }
    QVERIFY2(first, "Cached GPU engines were not ready within 120 seconds; prepare them first.");
    const double startupMs = Ms(first->completion->at - constructed);
    QVERIFY2(first->completion->result.output.ok, qPrintable(first->completion->result.output.error));
    const auto backend = first->completion->backend;
    QVERIFY2(backend.usesGpu, qPrintable(backend.detail));

    QJsonArray sceneRecords;
    for (int index = 0; index < int(scenes.size()); ++index) {
        QJsonArray reads;
        QJsonObject firstOutput;
        QString firstSha;
        bool identical = true;
        int pass = 0;
        // One read of this scene and its record. `extra` adds the scenario's
        // fields. Returns false after a failed verification.
        const auto readAndRecord = [&](QJsonObject extra) {
            extra["armedAtSubmit"] = scheduler->KeepWarmArmed();
            auto attempt = read(index);
            if (!attempt.completion) {
                QTest::qFail(qPrintable(attempt.error), __FILE__, __LINE__);
                return false;
            }
            const auto& result = attempt.completion->result;
            if (!result.output.ok) {
                QTest::qFail(qPrintable(scenes[std::size_t(index)].path + ": " + result.output.error), __FILE__, __LINE__);
                return false;
            }
            if (!(result.deviceInput && attempt.meters.egress == 0 && attempt.meters.residentCalls == 1)) {
                QTest::qFail("A read left the resident GPU path.", __FILE__, __LINE__);
                return false;
            }
            const auto sha = Hex(QCryptographicHash::hash(ExactOutputBytes(result.output), QCryptographicHash::Sha256));
            if (pass == 0) {
                firstSha = sha;
                firstOutput = OutputJson(result.output);
                firstOutput.remove("exactOrderedOutputV1");
                // Placement evidence (round 12b): the worker's ink-fit result,
                // the final quad and the private straight rule's provenance.
                QJsonArray lines = firstOutput["lines"].toArray();
                for (int i = 0; i < lines.size() && i < result.output.lines.size(); ++i) {
                    QJsonArray words = lines[i].toArray();
                    const auto& source = result.output.lines[i].words;
                    for (int j = 0; j < words.size() && j < source.size(); ++j) {
                        QJsonObject word = words[j].toObject();
                        word["inkFitted"] = source[j].inkFitted;
                        if (source[j].movedLineBow > 0) word["movedLineBow"] = source[j].movedLineBow;
                        if (source[j].quadPx.valid) {
                            QJsonArray quad;
                            for (const auto& point : source[j].quadPx.points) { quad.push_back(point.x()); quad.push_back(point.y()); }
                            word["quad"] = quad;
                        }
                        words[j] = word;
                    }
                    lines[i] = words;
                }
                firstOutput["lines"] = lines;
            } else identical = identical && sha == firstSha;
            int words = 0, candidates = 0;
            for (const auto& line : result.output.lines)
                for (const auto& word : line.words) {
                    ++words;
                    candidates += word.disposition == TokenDisposition::kCandidate;
                }
            QJsonObject record{{"pass", pass}, {"workerMs", result.durationMs},
                {"submitToCallbackMs", attempt.submitToCallbackMs}, {"submittedUnixMs", attempt.submittedUnixMs},
                {"residentMs", attempt.meters.residentMs},
                {"inputWaitMs", attempt.meters.waitMs}, {"outputSha256", sha}, {"words", words}, {"candidates", candidates}};
            for (auto it = extra.begin(); it != extra.end(); ++it) record[it.key()] = it.value();
            if (!attempt.profiles.isEmpty()) {
                QJsonArray lines;
                for (const auto& line : attempt.profiles) lines.push_back(line);
                record["profile"] = lines;
            }
            reads.push_back(record);
            ++pass;
            return true;
        };
        QJsonObject sessionRecord;
        if (!session) {
            for (int repeat = 0; repeat <= passes; ++repeat) { // pass 0 is this scene's first read in this process
                if (gapMs) std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
                if (!readAndRecord({})) return;
                // As the application does: announce the next paced read only
                // once a read was submitted (round 14; the first timed read of
                // a process is never armed). No effect unless keep-warm is on;
                // change mode ignores it. With OKUFLOW_UCT_CORPUS_CHANGE_EACH
                // the content changes after every read (playing video), which
                // a probe would report as a view change.
                scheduler->ExpectNextRead(Clock::now() + std::chrono::milliseconds(gapMs));
                if (changeEach) scheduler->NoteViewChange();
            }
        } else {
            // Round 14 session: the controller's timeline on a live view. A
            // view change every changeMs (Cancel, then NoteViewChange, as
            // Invalidate does), its read settleMs later; a refresh read
            // refreshMs after the previous read; each read announces the next
            // refresh. Fixed duration, so power can be averaged over it.
            using SessionClock = uct_clock_probe_test::ProbeClock;
            const auto begin = SessionClock::now();
            const auto end = begin + std::chrono::milliseconds(session->durationMs);
            const auto never = SessionClock::time_point::max();
            auto nextChange = session->changeMs ? begin + std::chrono::milliseconds(session->changeMs) : never;
            auto nextRefresh = session->refreshMs ? begin + std::chrono::milliseconds(session->refreshMs) : never;
            sessionRecord["startUnixMs"] = QDateTime::currentMSecsSinceEpoch();
            int changes = 0;
            for (;;) {
                const auto due = std::min(nextChange, nextRefresh);
                if (due >= end) break;
                uct_clock_probe_test::SleepUntil(due);
                QJsonObject extra;
                if (due == nextChange) {
                    scheduler->Cancel();
                    scheduler->NoteViewChange();
                    const auto changed = SessionClock::now();
                    uct_clock_probe_test::SleepUntil(changed + std::chrono::milliseconds(session->settleMs));
                    extra["kind"] = "change";
                    extra["sinceChangeMs"] = std::chrono::duration<double, std::milli>(SessionClock::now() - changed).count();
                    nextChange += std::chrono::milliseconds(session->changeMs);
                    ++changes;
                } else {
                    extra["kind"] = "refresh";
                }
                const auto submitted = SessionClock::now();
                if (!readAndRecord(extra)) return;
                if (session->refreshMs) {
                    nextRefresh = submitted + std::chrono::milliseconds(session->refreshMs);
                    scheduler->ExpectNextRead(Clock::now() + std::chrono::milliseconds(session->refreshMs));
                }
            }
            uct_clock_probe_test::SleepUntil(end);
            sessionRecord["endUnixMs"] = QDateTime::currentMSecsSinceEpoch();
            sessionRecord["changes"] = changes;
        }
        const auto& scene = scenes[std::size_t(index)];
        QJsonObject sceneRecord{{"path", scene.path}, {"sha256", scene.sha256},
            {"width", scene.image.width()}, {"height", scene.image.height()}, {"reads", reads},
            {"repeatedOutputsIdentical", identical}, {"firstOutputSha256", firstSha}, {"firstOutput", firstOutput}};
        if (session) sceneRecord["session"] = sessionRecord;
        sceneRecords.push_back(sceneRecord);
    }

    const auto statsJson = [](const SchedulerStats& stats) {
        return QJsonObject{{"launches", double(stats.keepWarmLaunches)}, {"pending", double(stats.keepWarmPending)},
            {"notReady", double(stats.keepWarmNotReady)}, {"timeouts", double(stats.keepWarmTimeouts)},
            {"busyMs", stats.keepWarmBusyMs}, {"stops", double(stats.keepWarmStops)},
            {"changes", double(stats.keepWarmChanges)}, {"stopMaxUs", stats.keepWarmStopMaxUs}};
    };
    QJsonObject keepWarmRecord{{"mode", int(keepWarm.settings.mode)},
        {"periodMs", double(keepWarm.settings.period.count())}, {"busyUs", double(keepWarm.settings.busy.count())},
        {"leadMs", double(keepWarm.settings.lead.count())}, {"idleTimeoutMs", double(keepWarm.settings.idleTimeout.count())},
        {"changeWindowMs", double(keepWarm.settings.changeWindow.count())}, {"tailMs", double(keepWarm.settings.tail.count())},
        {"afterReads", statsJson(scheduler->Stats())}};
    QJsonObject tail;
    if (tailMs) {
        // No more reads: keep-warm must stop by its idle time-out and stay
        // stopped; the frame load (if any) keeps running as the app's would.
        tail["startUnixMs"] = QDateTime::currentMSecsSinceEpoch();
        tail["armedAtStart"] = scheduler->KeepWarmArmed();
        std::this_thread::sleep_for(std::chrono::milliseconds(tailMs));
        tail["endUnixMs"] = QDateTime::currentMSecsSinceEpoch();
        tail["armedAtEnd"] = scheduler->KeepWarmArmed();
        tail["stats"] = statsJson(scheduler->Stats());
    }
    if (frameLoad) frameLoad->Stop();
    scheduler->Cancel();
    scheduler.reset();
    const auto closed = retirement.Wait();
    QJsonObject switches;
    const auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& key : environment.keys())
        if (key.startsWith("OKUFLOW_UCT_") && !key.startsWith("OKUFLOW_UCT_CORPUS_")) switches[key] = environment.value(key);
    QFile executable(QCoreApplication::applicationFilePath());
    QVERIFY(executable.open(QIODevice::ReadOnly));
    QCryptographicHash executableHash(QCryptographicHash::Sha256);
    QVERIFY(executableHash.addData(&executable));
    const QJsonObject report{{"schema", 1}, {"scenes", sceneRecords}, {"passes", passes}, {"gapMs", gapMs},
        {"stamp", media ? "media" : "camera"}, {"pair", tiers.join(QLatin1Char(':'))}, {"profile", profile},
        {"startupFirstResultMs", startupMs}, {"startupPreparingPolls", preparingPolls},
        // The startup read is scene zero's true first read in this process.
        {"startupWorkerMs", first->completion->result.durationMs},
        {"backendDetail", backend.detail}, {"switches", switches},
        {"executableSha256", Hex(executableHash.result())},
        {"gpu", QString::fromLatin1(properties.name)}, {"cudaDriverApi", driver},
        {"retirement", closed.Json()}, {"profileLinesMalformed", capture.Malformed()},
        {"keepWarm", keepWarmRecord}, {"tail", tail}, {"changeEach", changeEach},
        {"session", session ? QJsonObject{{"refreshMs", session->refreshMs}, {"changeMs", session->changeMs},
                                          {"settleMs", session->settleMs}, {"durationMs", session->durationMs}}
                            : QJsonObject{}},
        {"stablePowerState", stable ? stable->Detail() : QString()},
        {"frameLoad", frameLoad ? frameLoad->Json() : QJsonObject{}},
        {"scope", "Scheduler worker time on file fixtures: no camera, decoder, presenter, paint or accessibility."}};
    QVERIFY(uct_batch_test::Save(evidence, report));
    QVERIFY(closed.Passed());
}
// GPU contract for the private keep-warm (round 13): with real engines, reads
// interleaved with running spins are byte-identical to reads of a scheduler
// that never warms, a spin outstanding at destruction does not hold up
// retirement, and every TensorRT State closure completes. Opt-in: needs the
// prepared plans, OKUFLOW_UCT_KEEP_WARM_GPU_TEST=1 and one or more images in
// OKUFLOW_UCT_CORPUS_IMAGES.
inline void KeepWarmGpuContract()
{
    using namespace uct_resident_test;
    if (qEnvironmentVariable("OKUFLOW_UCT_KEEP_WARM_GPU_TEST") != "1")
        QSKIP("Explicit keep-warm GPU contract not selected.");
    QStringList paths;
    for (const auto& path : qEnvironmentVariable("OKUFLOW_UCT_CORPUS_IMAGES").split(QLatin1Char(';'), Qt::SkipEmptyParts))
        paths.push_back(QFileInfo(path.trimmed()).absoluteFilePath());
    QVERIFY2(!paths.isEmpty() && paths.size() <= 8, "Select 1-8 images with OKUFLOW_UCT_CORPUS_IMAGES.");
    QVERIFY(cudaSetDevice(0) == cudaSuccess);
    DeviceStream stream;
    DeviceUploads uploads;
    QVERIFY(stream.value);
    uploads.stream = stream.value;
    struct Scene { QImage image; void* device{}; };
    std::vector<Scene> scenes;
    for (const auto& path : paths) {
        Scene scene;
        scene.image = QImage(path).convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(!scene.image.isNull(), qPrintable(path));
        scene.device = uploads.Allocate(std::size_t(scene.image.sizeInBytes()));
        QVERIFY(scene.device);
        QVERIFY(cudaMemcpyAsync(scene.device, scene.image.constBits(), std::size_t(scene.image.sizeInBytes()),
                                cudaMemcpyHostToDevice, stream.value) == cudaSuccess);
        scenes.push_back(std::move(scene));
    }
    QVERIFY(cudaStreamSynchronize(stream.value) == cudaSuccess);
    auto pool = std::make_unique<AnalysisPool>();
    std::uint64_t frame = 1;
    // One owned read through `scheduler`; returns the exact output digest.
    const auto read = [&](RecognitionScheduler& scheduler, Mailbox& mailbox, int index, QByteArray& sha) {
        const auto& scene = scenes[std::size_t(index)];
        const int w = scene.image.width(), h = scene.image.height();
        SharedDeviceImage owned;
        for (int attempt = 0; !owned && attempt < 2000; ++attempt) {
            owned = pool->Capture(scene.device, std::size_t(w) * 4, w, h, stream.value, std::uint64_t(index) + 1, frame, 1);
            if (!owned) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        QVERIFY(owned);
        QVERIFY(cudaStreamSynchronize(stream.value) == cudaSuccess);
        SourceStamp stamp;
        stamp.sourceEpoch = owned->sourceEpoch;
        stamp.geometryGeneration = 1;
        stamp.frameSeq = frame++;
        stamp.viewportWidth = std::uint32_t(w);
        stamp.viewportHeight = std::uint32_t(h);
        stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
        stamp.liveCameraCapture = true;
        for (int attempt = 0; attempt < 6000; ++attempt) {
            const auto submitted = Clock::now();
            QVERIFY(scheduler.SubmitDevice(stamp, owned));
            const auto completion = Await(mailbox, submitted + std::chrono::seconds(60));
            QVERIFY(completion);
            const auto& output = completion->result.output;
            if (output.backendPreparing && !output.ok) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            QVERIFY2(output.ok, qPrintable(output.error));
            sha = QCryptographicHash::hash(ExactOutputBytes(output), QCryptographicHash::Sha256);
            return;
        }
        QFAIL("Cached GPU engines were not ready; prepare them first.");
    };
    const auto make = [&](uct_batch_test::RetirementReceipts& retirement, const std::shared_ptr<Mailbox>& mailbox) {
        return std::make_unique<RecognitionScheduler>(
            CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, retirement.Observer(), QString()),
            [mailbox](RecognitionResult result) {
                const auto at = Clock::now();
                {
                    std::lock_guard lock(mailbox->mutex);
                    mailbox->completed = Completion{std::move(result), mailbox->backend, at};
                }
                mailbox->ready.notify_all();
            },
            [mailbox](BackendStatus status) {
                std::lock_guard lock(mailbox->mutex);
                mailbox->backend = std::move(status);
            },
            2, ModelTuning{}.minimumWordHeightPx, false, false);
    };
    // Reference digests from a scheduler that never warms.
    QVector<QByteArray> reference(int(scenes.size()));
    {
        uct_batch_test::RetirementReceipts retirement;
        const auto mailbox = std::make_shared<Mailbox>();
        auto plain = make(retirement, mailbox);
        for (int i = 0; i < int(scenes.size()); ++i) {
            read(*plain, *mailbox, i, reference[i]);
            if (QTest::currentTestFailed()) return;
        }
        plain.reset();
        QVERIFY(retirement.Wait().Passed());
    }
    // Continuous keep-warm: reads land while spins run or wait on the stream.
    uct_batch_test::RetirementReceipts retirement;
    const auto mailbox = std::make_shared<Mailbox>();
    auto warm = make(retirement, mailbox);
    KeepWarmSettings settings;
    settings.mode = KeepWarmMode::Continuous;
    warm->SetKeepWarm(settings);
    QByteArray sha;
    read(*warm, *mailbox, 0, sha); // promotes the prepared engines
    if (QTest::currentTestFailed()) return;
    QCOMPARE(sha, reference[0]);
    int compared = 0;
    for (int round = 0; round < 4; ++round)
        for (int i = 0; i < int(scenes.size()); ++i) {
            const auto launched = warm->Stats().keepWarmLaunches;
            warm->ExpectNextRead(std::chrono::steady_clock::now() + std::chrono::milliseconds(350));
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (warm->Stats().keepWarmLaunches < launched + 2 && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            QVERIFY2(warm->Stats().keepWarmLaunches >= launched + 2, "Keep-warm did not launch on the GPU backend.");
            std::this_thread::sleep_for(std::chrono::milliseconds(3 + 7 * round)); // land at varied spin phases
            read(*warm, *mailbox, i, sha);
            if (QTest::currentTestFailed()) return;
            QCOMPARE(sha, reference[i]);
            ++compared;
        }
    // Round 14: change mode. A view change warms; reads that land while its
    // spins run or wait are byte-identical too.
    {
        KeepWarmSettings change;
        change.mode = KeepWarmMode::Change;
        warm->SetKeepWarm(change);
        for (int i = 0; i < int(scenes.size()); ++i) {
            const auto launched = warm->Stats().keepWarmLaunches;
            warm->Cancel();
            warm->NoteViewChange();
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (warm->Stats().keepWarmLaunches < launched + 2 && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            QVERIFY2(warm->Stats().keepWarmLaunches >= launched + 2, "A view change did not warm the GPU backend.");
            std::this_thread::sleep_for(std::chrono::milliseconds(4 + 3 * i));
            read(*warm, *mailbox, i, sha);
            if (QTest::currentTestFailed()) return;
            QCOMPARE(sha, reference[i]);
            ++compared;
        }
        warm->SetKeepWarm(settings);
    }
    // Round 14 review: disarming ends spins that are RUNNING on the GPU, not
    // only future launches. Freeing a frame-sized (8 MiB) allocation waits for
    // all queued work of the context (r14-clock-1; small blocks may not), so
    // the time a cudaFree issued right after StopKeepWarm() takes bounds how
    // long the spins kept running; without the stop it waits for the running
    // and the queued spin.
    {
        QVector<double> stopped, unstopped;
        for (int i = 0; i < 16; ++i) {
            const bool stop = i % 2 == 0;
            const auto launched = warm->Stats().keepWarmLaunches;
            warm->ExpectNextRead(std::chrono::steady_clock::now() + std::chrono::milliseconds(350));
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (warm->Stats().keepWarmLaunches < launched + 2 && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            QVERIFY(warm->Stats().keepWarmLaunches >= launched + 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            void* buffer{};
            QVERIFY(cudaMalloc(&buffer, 8 << 20) == cudaSuccess);
            const auto begin = Clock::now();
            if (stop) warm->StopKeepWarm();
            QVERIFY(cudaFree(buffer) == cudaSuccess);
            (stop ? stopped : unstopped).push_back(Ms(Clock::now() - begin));
            warm->StopKeepWarm();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        std::sort(stopped.begin(), stopped.end());
        std::sort(unstopped.begin(), unstopped.end());
        qInfo().noquote() << "keep-warm GPU stop: cudaFree after StopKeepWarm median" << stopped[stopped.size() / 2]
                          << "ms, max" << stopped.back() << "ms; without the stop median" << unstopped[unstopped.size() / 2]
                          << "ms, max" << unstopped.back() << "ms; worker stop latency max"
                          << warm->Stats().keepWarmStopMaxUs << "us";
        // A stop raised while the worker is itself inside a CUDA call that the
        // free holds up is handled when that call returns: never later than
        // the spins' own bound (two 10 ms spins).
        QVERIFY2(stopped[stopped.size() / 2] < 2.0, "Disarming did not end the running spins promptly.");
        QVERIFY2(stopped.back() < 25.0, "Disarming took longer than the spins' own bound.");
    }
    // Destroy while spins are outstanding: prompt, and every State closes.
    warm->ExpectNextRead(std::chrono::steady_clock::now() + std::chrono::milliseconds(350));
    const auto launched = warm->Stats().keepWarmLaunches;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (warm->Stats().keepWarmLaunches < launched + 2 && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    QVERIFY(warm->Stats().keepWarmLaunches >= launched + 2);
    const auto stats = warm->Stats();
    const auto destroying = Clock::now();
    warm.reset();
    const double destroyMs = Ms(Clock::now() - destroying);
    const auto& closed = retirement.Wait();
    const double retiredMs = Ms(Clock::now() - destroying);
    qInfo().noquote() << "keep-warm GPU contract: reads compared" << compared << "launches" << stats.keepWarmLaunches
                      << "pending" << stats.keepWarmPending << "destroy ms" << destroyMs << "retired ms" << retiredMs;
    QVERIFY(closed.Passed());
    QVERIFY2(destroyMs < 1000, "Destroying a warming scheduler took too long.");
}
#else
inline void KeepWarmGpuContract()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_KEEP_WARM_GPU_TEST") != "1")
        QSKIP("Explicit keep-warm GPU contract not selected.");
    QFAIL("The keep-warm GPU contract needs the CUDA analysis pool and TensorRT adapter.");
}
inline void CorpusTiming()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_CORPUS_TIMING") != "1")
        QSKIP("Explicit corpus timing not selected.");
    QFAIL("Corpus timing was selected, but this build lacks the CUDA analysis pool or TensorRT adapter.");
}
#endif
} // namespace okuflow::uct::uct_corpus_test
