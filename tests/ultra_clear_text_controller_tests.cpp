// Selected-media controller contract. Qt offscreen only: no camera, desktop
// input, decoder, native OCR runtime or downloaded sample is required.

#include "okuflow/app/ultra_clear_text_controller.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include "ultra_clear_text_test_support.hpp"

#include <QAccessible>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QUuid>
#include <QtTest>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

using namespace okuflow;
using namespace okuflow::uct;
using namespace uct_test;

namespace {

struct AccessibilityActivation {
    bool previous{QAccessible::isActive()};
    AccessibilityActivation() { QAccessible::setActive(true); }
    ~AccessibilityActivation() { QAccessible::setActive(previous); }
};

struct ImmutableScript {
    std::mutex mutex;
    std::condition_variable changed;
    bool holdInitialization{};
    bool holdRecognition{};
    QVector<PageWord> words;
    std::atomic<int> initialized{};
    std::atomic<int> calls{};
    std::atomic<int> deviceCalls{};
    std::atomic<bool> wantsDevice{};

    void Release()
    {
        {
            std::lock_guard lock(mutex);
            holdInitialization = false;
            holdRecognition = false;
        }
        changed.notify_all();
    }
};

class ImmutableRecognizer final : public Recognizer {
public:
    explicit ImmutableRecognizer(std::shared_ptr<ImmutableScript> script) : script_(std::move(script)) {}

    BackendStatus Initialize() override
    {
        std::unique_lock lock(script_->mutex);
        ++script_->initialized;
        script_->changed.wait(lock, [&] { return !script_->holdInitialization; });
        BackendStatus status;
        status.state = BackendState::kReady;
        status.name = QStringLiteral("immutable fixture");
        return status;
    }

    bool WantsDeviceInput() const override { return script_->wantsDevice.load(); }

    RecognitionOutput Recognize(const QImage& image, const std::atomic<bool>& cancelled) override
    {
        ++script_->calls;
        return Produce(image.width(), image.height(), cancelled);
    }

    RecognitionOutput RecognizeDevice(const DeviceImage& image, const std::atomic<bool>& cancelled) override
    {
        ++script_->deviceCalls;
        return Produce(image.width, image.height, cancelled);
    }

private:
    RecognitionOutput Produce(int width, int height, const std::atomic<bool>& cancelled)
    {
        QVector<PageWord> words;
        {
            std::unique_lock lock(script_->mutex);
            words = script_->words;
            script_->changed.wait(lock, [&] { return !script_->holdRecognition; });
        }
        RecognitionOutput output;
        output.cancelled = cancelled.load();
        output.ok = !output.cancelled;
        if (!output.ok) return output;
        RecognizedLine line;
        for (const PageWord& word : words) {
            line.words.push_back({word.text,
                QRectF(word.sceneRect.x() * width, word.sceneRect.y() * height,
                       word.sceneRect.width() * width, word.sceneRect.height() * height)});
        }
        output.lines.push_back(std::move(line));
        return output;
    }

    std::shared_ptr<ImmutableScript> script_;
};

SharedPixels OwnedPixels(const QImage& image)
{
    auto pixels = std::make_shared<PixelBuffer>();
    pixels->width = image.width();
    pixels->height = image.height();
    pixels->strideBytes = static_cast<int>(image.bytesPerLine());
    pixels->bytes.resize(static_cast<std::size_t>(image.sizeInBytes()));
    std::memcpy(pixels->bytes.data(), image.constBits(), pixels->bytes.size());
    return pixels;
}

// Independently owned fake GPU scene. Readback/ReadProbe stand in for worker
// copies and record any accidental UI-thread pixel access.
struct FakeDeviceState {
    std::thread::id uiThread{std::this_thread::get_id()};
    std::atomic<int> readbacks{};
    std::atomic<int> probes{};
    std::atomic<bool> readOnUiThread{};
    std::atomic<bool> probeFails{};
};

class FakeDeviceImage final : public DeviceImage {
public:
    FakeDeviceImage(std::shared_ptr<FakeDeviceState> state, QImage page,
                    std::uint64_t epoch, std::uint64_t frame, std::int64_t ptsMs)
        : state_(std::move(state)), page_(std::move(page))
    {
        width = page_.width();
        height = page_.height();
        pitch = static_cast<std::size_t>(page_.bytesPerLine());
        sourceEpoch = epoch;
        frameSequence = frame;
        timestampMs = ptsMs;
    }
    const void* Data() const override { return this; }
    bool WaitReady(void*, QString&) const override { return true; }
    QImage Readback(const std::atomic<bool>&, QString&) const override
    {
        ++state_->readbacks;
        NoteThread();
        return page_;
    }
    QImage ReadProbe(const std::atomic<bool>&, QString& error) const override
    {
        ++state_->probes;
        NoteThread();
        if (state_->probeFails.load()) {
            error = QStringLiteral("fixture probe failure");
            return {};
        }
        return page_.scaled(width / 2, height / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

private:
    void NoteThread() const
    {
        if (std::this_thread::get_id() == state_->uiThread) state_->readOnUiThread = true;
    }
    std::shared_ptr<FakeDeviceState> state_;
    QImage page_;
};

struct ImmutableHarness {
    std::shared_ptr<ImmutableScript> script = std::make_shared<ImmutableScript>();
    std::shared_ptr<FakeDeviceState> devices = std::make_shared<FakeDeviceState>();
    std::int64_t now{3600000};
    QWidget owner;
    QWidget target{&owner};
    UltraClearTextOverlay overlay{&target, &owner};
    UltraClearTextPanel panel{&owner};
    SyntheticPage page = MakePage({QStringLiteral("Reading helps")}, {}, 960, 540, 48);
    std::unique_ptr<UltraClearTextController> controller;

    explicit ImmutableHarness(UctTiming timing = {})
    {
        target.resize(960, 540);
        script->words = page.words;
        auto shared = script;
        controller = std::make_unique<UltraClearTextController>(
            &overlay, &panel, [shared] { return std::make_unique<ImmutableRecognizer>(shared); },
            nullptr, [this] { return now; }, timing);
    }

    ~ImmutableHarness()
    {
        script->Release();
        controller.reset();
    }

    UctImmutableScene Scene(std::uint64_t frame = 10) const
    {
        UctImmutableScene scene;
        scene.geometry.settings.sourceEpoch = 41;
        scene.geometry.sceneWidth = page.image.width();
        scene.geometry.sceneHeight = page.image.height();
        scene.sourceAuthority = 3;
        scene.selected = {OwnedPixels(page.image), frame, 1}; // genuinely hour-old source
        scene.transform.valid = true;
        scene.viewportWidth = 960;
        scene.viewportHeight = 540;
        return scene;
    }

    void Enable()
    {
        controller->SetEnabled(true);
    }

    UctAdvancingMediaScene PlayFrame(std::uint64_t frame, std::uint64_t authority = 3)
    {
        UctAdvancingMediaScene scene;
        scene.geometry.settings.sourceEpoch = 41;
        scene.geometry.sceneWidth = page.image.width();
        scene.geometry.sceneHeight = page.image.height();
        scene.sourceAuthority = authority;
        scene.current = {OwnedPixels(IndependentFramePixels(page.image, frame)), frame,
                         10000 + static_cast<std::int64_t>(frame) * 20};
        scene.transform.valid = true;
        scene.viewportWidth = 960;
        scene.viewportHeight = 540;
        {
            std::lock_guard lock(script->mutex);
            script->words = page.words;
        }
        now += 20; // wall/job time advances independently from the real media PTS
        controller->OnAdvancingMediaPresented(scene);
        QTest::qWait(3);
        return scene;
    }

    SharedDeviceImage Device(const QImage& image, std::uint64_t frame, std::int64_t ptsMs,
                             std::uint64_t epoch = 41) const
    {
        return std::make_shared<FakeDeviceImage>(devices, image, epoch, frame, ptsMs);
    }

    // Device-only selected still: no CPU pixels are retained at all.
    UctImmutableScene DeviceScene(std::uint64_t frame = 10) const
    {
        auto scene = Scene(frame);
        scene.selected = {{}, frame, 1, Device(page.image, frame, 1)};
        return scene;
    }

    UctAdvancingMediaScene PlayDeviceFrame(std::uint64_t frame, std::uint64_t authority = 3)
    {
        UctAdvancingMediaScene scene;
        scene.geometry.settings.sourceEpoch = 41;
        scene.geometry.sceneWidth = page.image.width();
        scene.geometry.sceneHeight = page.image.height();
        scene.sourceAuthority = authority;
        const std::int64_t pts = 10000 + static_cast<std::int64_t>(frame) * 20;
        scene.current = {{}, frame, pts,
                         Device(IndependentFramePixels(page.image, frame), frame, pts)};
        scene.transform.valid = true;
        scene.viewportWidth = 960;
        scene.viewportHeight = 540;
        {
            std::lock_guard lock(script->mutex);
            script->words = page.words;
        }
        now += 20;
        controller->OnAdvancingMediaPresented(scene);
        QTest::qWait(5);
        return scene;
    }

    // No-analysis display receipt for `frame`: real media PTS, no pixels.
    UctAdvancingMediaPresentation GapReceipt(std::uint64_t frame, std::uint64_t authority = 3) const
    {
        UctAdvancingMediaPresentation receipt;
        receipt.geometry.settings.sourceEpoch = 41;
        receipt.geometry.sceneWidth = page.image.width();
        receipt.geometry.sceneHeight = page.image.height();
        receipt.sourceAuthority = authority;
        receipt.frameSeq = frame;
        receipt.timestampMs = 10000 + static_cast<std::int64_t>(frame) * 20;
        receipt.transform.valid = true;
        receipt.viewportWidth = 960;
        receipt.viewportHeight = 540;
        return receipt;
    }

    bool PresentGap(std::uint64_t frame, std::uint64_t authority = 3, std::int64_t wallAdvanceMs = 20)
    {
        now += wallAdvanceMs;
        return controller->OnAdvancingMediaPresentedWithoutAnalysis(GapReceipt(frame, authority));
    }

    // Plays real device frames from `first` until glyphs are drawn, then a
    // few more so validation is current. Returns the last played frame.
    std::uint64_t EstablishDeviceGlyphs(std::uint64_t first = 1, std::uint64_t authority = 3)
    {
        std::uint64_t frame = first;
        for (; frame < first + 240 && overlay.WordCount() == 0; ++frame)
            PlayDeviceFrame(frame, authority);
        for (int i = 0; i < 8; ++i) PlayDeviceFrame(frame++, authority);
        QTest::qWait(10);
        return frame - 1;
    }

    QString Reading() const { return panel.readingPane()->toPlainText(); }
};

} // namespace

class UltraClearTextControllerTests final : public QObject {
    Q_OBJECT
private slots:
    void immutableOldFrameAndIdleDoNoPeriodicWork();
    void backendReadySubmitsWaitingMediaOnce();
    void coveredZoomReprojectsTheSameArtifact();
    void identicalPixelsNeverCorroborate();
    void changingAuthorityAndClearingFenceLateResults();
    void queuedCompletionCannotCrossSourceAuthority();
    void modelReplacementRevokesSelectedEvidence();
    void immutableDeadlineIsIndependentOfSourceAge();
    void invalidSelectedMediaDoesNotResumeHiddenLiveInput();
    void advancingMediaPacesOcrAndValidatesLatestFrames();
    void advancingMediaValidatesWhileOcrHeldAndRejectsChangedPixels();
    void advancingMediaAuthorityRevokesAndSuppressesLiveInput();
    void advancingMediaPauseReturnsToOneShotAnalysis();
    void advancingMediaDeadlineUsesWallClock();
    void mediaPresentationFailureRevokesCachedArtifact_data();
    void mediaPresentationFailureRevokesCachedArtifact();
    void immutablePresentationFailureBlocksBackendReadiness();
    void advancingPresentationFailureBlocksLateResult();
    void deviceImmutableRecognizesWithoutCpuPixels();
    void deviceImmutableDistinctCorroborationDrawsGlyphs();
    void deviceImmutableIdenticalPixelsNeverCorroborate();
    void deviceImmutableRejectsMismatchedImageAndFallsBackToPixels();
    void deviceAdvancingMediaPacesAndValidates();
    void deviceAdvancingSeekAndBackwardFrameRevoke();
    void deviceAdvancingSlowJobUsesWallDeadline();
    void deviceAdvancingProbeFailureRecovers();
    void deviceAdvancingReleasesConsumedAnalysisLease();
    void gapReceiptRetainsBrieflyThenMediaExpiryHidesAndCurrentInputRecovers();
    void gapReceiptWallExpiryHidesDespiteFreshMediaPts();
    void gapReceiptDiscontinuitiesRevokeImmediately();
    void gapReceiptLateOldRecognitionCannotReviveAfterExpiry();
    void gapReceiptSameFrameReprojectsRepeatedlyWithoutWork();
    void processingCorrespondenceGateRevokesTextAndUiaWithoutChangingPreference();
};

void UltraClearTextControllerTests::processingCorrespondenceGateRevokesTextAndUiaWithoutChangingPreference()
{
    AccessibilityActivation accessibility;
    ImmutableHarness h;
    h.Enable();
    auto scene = h.Scene();
    auto independent = h.page.image.copy();
    independent.setPixelColor(950, 530, Qt::red);
    scene.corroboration = UctImmutableFrame{OwnedPixels(independent), 11, 2};
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QVERIFY(!h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextAccessibility()->ExposedLineCount() > 0);
    h.controller->SetMediaOverlaySuppressed(true);
    QVERIFY(h.controller->IsEnabled()); // user preference remains on
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QCOMPARE(h.controller->ScreenTextAccessibility()->ExposedLineCount(), 0);
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    QVERIFY(h.controller->SetImmutableScene(scene)); // cannot bypass processing correspondence
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    h.controller->OnMediaPresentationSucceeded();
    QTest::qWait(50);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.controller->SetMediaOverlaySuppressed(false);
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QVERIFY(!h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextAccessibility()->ExposedLineCount() > 0);
    h.controller->SetMediaOverlaySuppressed(true);
    h.controller->ClearImmutableScene(); // leaving media must clear the gate even without an owned scene
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount() > 0);
}

void UltraClearTextControllerTests::immutableOldFrameAndIdleDoNoPeriodicWork()
{
    ImmutableHarness h;
    auto scene = h.Scene();
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(admitted.size(), 1);
    QVERIFY(h.Reading().startsWith(QChar(0x2026)));
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.overlay.WordCount(), 0); // one still is reading evidence, not temporal proof
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    QCOMPARE(h.controller->Model().Lines().first().provenance.timestampMs, std::int64_t(1));
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, scene.selected.frameSeq);

    const QString reading = h.Reading();
    h.now += 3600000;
    UctFrameContext live;
    live.geometry.settings.sourceEpoch = 900;
    live.cameraActive = true;
    live.frameSeq = 900;
    live.transform.valid = true;
    live.viewportWidth = 960;
    live.viewportHeight = 540;
    for (int i = 0; i < 20; ++i) {
        QVERIFY(!h.controller->OnPresented(live));
        h.controller->OnCpuViewport(live, h.page.image.constBits(), h.page.image.width(),
                                    h.page.image.height(), static_cast<int>(h.page.image.bytesPerLine()));
        h.controller->OnDeviceScene(live, {});
        h.controller->OnReadbackRequested(17, live);
        h.controller->OnPresentationSucceeded();
        h.controller->OnPresentationFailed();
        h.controller->OnSourceRetired();
    }
    QTest::qWait(250); // past the ordinary watchdog tick; no media watchdog renewal
    QCOMPARE(h.Reading(), reading);
    QCOMPARE(h.script->calls.load(), 1);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
    QVERIFY(!h.controller->HasPendingReadback());
    QVERIFY(h.controller->SetImmutableScene(scene)); // exact repeated selection is projection only
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
}

void UltraClearTextControllerTests::backendReadySubmitsWaitingMediaOnce()
{
    ImmutableHarness h;
    h.script->holdInitialization = true;
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(h.Scene()));
    QTRY_COMPARE(h.script->initialized.load(), 1);
    QTest::qWait(250);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
    h.script->Release();
    QTRY_VERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QTest::qWait(250);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::coveredZoomReprojectsTheSameArtifact()
{
    ImmutableHarness h;
    auto scene = h.Scene();
    auto neighbour = h.page.image.copy();
    neighbour.setPixelColor(950, 530, Qt::red); // distinct source pixels outside the text patch
    scene.corroboration = UctImmutableFrame{OwnedPixels(neighbour), 11, 2};
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QCOMPARE(h.script->calls.load(), 2);
    const auto originalWords = h.overlay.Words();
    const auto originalRects = h.overlay.ProjectedWordRects();
    const auto generation = h.controller->GeometryGeneration();
    const auto stamp = h.controller->Model().Lines().first().provenance;
    // Fit intentionally preserves the complete scene and ignores zoom/pan.
    // Fill is the production cropped-view contract for a magnified scene.
    const auto zoom = ComputeViewTransform(960, 540, 960, 540, 2.0f, 0.3f, 0.3f,
                                           ViewportFitMode::kFill);
    QVERIFY(zoom.valid);
    QCOMPARE(zoom.sourceWidth, 0.5f);
    QCOMPARE(zoom.sourceHeight, 0.5f);
    h.now += 120000;
    h.controller->UpdateImmutableView(zoom, 960, 540, 2);
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.overlay.CurrentViewTransform().sourceX, zoom.sourceX);
    QCOMPARE(h.overlay.CurrentViewTransform().sourceY, zoom.sourceY);
    QCOMPARE(h.overlay.CurrentViewTransform().sourceWidth, zoom.sourceWidth);
    QCOMPARE(h.overlay.CurrentViewTransform().sourceHeight, zoom.sourceHeight);
    QCOMPARE(h.overlay.Words().first().sceneRect, originalWords.first().sceneRect);
    QCOMPARE(h.overlay.ProjectedWordRects().first(),
             SceneRectToView(zoom, h.overlay.width(), h.overlay.height(), originalWords.first().sceneRect));
    QVERIFY(h.overlay.ProjectedWordRects().first() != originalRects.first());
    QVERIFY(std::abs(h.overlay.ProjectedWordRects().first().width() -
                     2.0 * originalRects.first().width()) < 0.001);
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, stamp.frameSeq);
    QCOMPARE(h.controller->Model().Lines().first().provenance.timestampMs, stamp.timestampMs);
    h.controller->SetShowOriginal(true);
    QCOMPARE(h.overlay.WordCount(), 0);
    h.controller->SetShowOriginal(false);
    QVERIFY(h.overlay.WordCount() > 0);
    QTest::qWait(250);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::identicalPixelsNeverCorroborate()
{
    ImmutableHarness h;
    auto scene = h.Scene();
    scene.corroboration = UctImmutableFrame{OwnedPixels(h.page.image), 11, 2};
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(admitted.size(), 2);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().startsWith(QChar(0x2026)));
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    h.now += 120000;
    QTest::qWait(250);
    QCOMPARE(h.script->calls.load(), 2); // bounded failed corroboration, no retry loop
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::changingAuthorityAndClearingFenceLateResults()
{
    ImmutableHarness h;
    h.script->holdRecognition = true;
    auto first = h.Scene();
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(first));
    QTRY_COMPARE(h.script->calls.load(), 1);
    auto next = h.Scene(20);
    ++next.sourceAuthority;
    const auto generation = h.controller->GeometryGeneration();
    QVERIFY(h.controller->SetImmutableScene(next));
    QVERIFY(h.controller->GeometryGeneration() > generation);
    QVERIFY(h.Reading().isEmpty());
    h.script->Release();
    QTRY_VERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, std::uint64_t(20));
    QVERIFY(h.controller->SchedulerStatistics().revoked >= 1);
    QCOMPARE(h.controller->SchedulerStatistics().started, std::uint64_t(2));

    {
        std::lock_guard lock(h.script->mutex);
        h.script->holdRecognition = true;
    }
    auto pending = h.Scene(30);
    QVERIFY(h.controller->SetImmutableScene(pending));
    QTRY_COMPARE(h.script->calls.load(), 3);
    h.controller->ClearImmutableScene();
    QVERIFY(!h.controller->HasImmutableScene());
    QVERIFY(h.Reading().isEmpty());
    h.script->Release();
    QTRY_VERIFY(h.controller->SchedulerStatistics().revoked >= 2);
    QVERIFY(h.controller->Model().Lines().isEmpty());
    UctFrameContext live;
    live.geometry.settings.sourceEpoch = 91;
    live.geometry.sceneWidth = 960;
    live.geometry.sceneHeight = 540;
    live.requested = live.geometry.settings;
    live.cameraActive = true;
    live.transform.valid = true;
    live.viewportWidth = 960;
    live.viewportHeight = 540;
    QVERIFY(h.controller->OnPresented(live)); // live can resume only after explicit clear
}

void UltraClearTextControllerTests::modelReplacementRevokesSelectedEvidence()
{
    ImmutableHarness h;
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(h.Scene()));
    QTRY_VERIFY(!h.Reading().isEmpty());
    const auto generation = h.controller->GeometryGeneration();
    auto shared = h.script;
    h.controller->ReplaceRecognizerFactory([shared] { return std::make_unique<ImmutableRecognizer>(shared); });
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->GeometryGeneration() > generation);
    QTRY_COMPARE(h.script->initialized.load(), 2);
    QTRY_VERIFY(!h.Reading().isEmpty());
    QCOMPARE(h.script->calls.load(), 2);
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::queuedCompletionCannotCrossSourceAuthority()
{
    ImmutableHarness h;
    h.Enable();
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
    QVERIFY(h.controller->SetImmutableScene(h.Scene()));
    // Let the native worker finish while the UI callback stays queued. This
    // exercises the controller fence as well as the worker's cancellation.
    QElapsedTimer deadline;
    deadline.start();
    while (h.controller->SchedulerStatistics().completed < 1 && deadline.elapsed() < 3000)
        QTest::qSleep(1);
    QCOMPARE(h.controller->SchedulerStatistics().completed, std::uint64_t(1));
    auto replacement = h.Scene(20);
    ++replacement.sourceAuthority;
    QVERIFY(h.controller->SetImmutableScene(replacement));
    QVERIFY(h.Reading().isEmpty());
    QTRY_VERIFY(!h.Reading().isEmpty());
    QVERIFY(h.controller->RejectedResults() >= 1);
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, std::uint64_t(20));
}

void UltraClearTextControllerTests::immutableDeadlineIsIndependentOfSourceAge()
{
    UctTiming timing;
    timing.immutableJobTimeoutMs = 100;
    ImmutableHarness h(timing);
    h.script->holdRecognition = true;
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(h.Scene()));
    QTRY_COMPARE(h.script->calls.load(), 1);
    h.now += 101; // current analysis deadline, not the hour-old source clock
    h.script->Release();
    QTRY_COMPARE(h.controller->RejectedResults(), std::uint64_t(1));
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->Model().Lines().isEmpty());
    QTest::qWait(250);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
}

void UltraClearTextControllerTests::invalidSelectedMediaDoesNotResumeHiddenLiveInput()
{
    ImmutableHarness h;
    auto scene = h.Scene();
    scene.selected.pixels.reset();
    QVERIFY(!h.controller->SetImmutableScene(scene));
    h.Enable();
    QVERIFY(h.controller->HasImmutableScene());
    UctFrameContext live;
    live.cameraActive = true;
    QVERIFY(!h.controller->OnPresented(live));
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
    QVERIFY(h.Reading().isEmpty());
}

void UltraClearTextControllerTests::advancingMediaPacesOcrAndValidatesLatestFrames()
{
    ImmutableHarness h;
    h.Enable();
    UctAdvancingMediaScene last;
    for(std::uint64_t frame=1;frame<=100;++frame) last=h.PlayFrame(frame);
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QVERIFY(h.controller->HasAdvancingMediaScene());
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    const auto jobs=h.controller->SchedulerStatistics().submitted;
    QVERIFY2(jobs>=2 && jobs<=7,"Two seconds of 50fps media must not submit OCR per frame");
    const auto probes=h.controller->ProbeStatistics();
    QVERIFY(probes.submitted>=10 && probes.submitted<=26);
    QVERIFY(probes.completed>=10);
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    const auto& line=h.controller->Model().Lines().first();
    QVERIFY(line.validatedFrameSeq>=90);
    QVERIFY(line.provenance.timestampMs>=10000 && line.provenance.timestampMs<=12000);
    QVERIFY(line.validatedAtMs>=11800 && line.validatedAtMs<=12000);
    QCOMPARE(h.controller->Model().SceneWords(last.current.timestampMs,last.current.frameSeq).size(),
             h.overlay.WordCount());
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->PixelAllocations(),std::uint64_t(0)); // shared scene leases, no viewport copy
    const auto generation=h.controller->GeometryGeneration();
    const auto submitted=h.controller->ProbeStatistics().submitted;
    last.transform=ComputeViewTransform(960,540,960,540,2,.3f,.3f,ViewportFitMode::kFill);
    QVERIFY(h.controller->OnAdvancingMediaPresented(last)); // retained frame/view receipt only
    QCOMPARE(h.controller->GeometryGeneration(),generation);
    QCOMPARE(h.controller->ProbeStatistics().submitted,submitted);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,jobs);
}

void UltraClearTextControllerTests::advancingMediaValidatesWhileOcrHeldAndRejectsChangedPixels()
{
    ImmutableHarness h;
    h.Enable();
    for(std::uint64_t frame=1;frame<=60;++frame) h.PlayFrame(frame);
    QTRY_VERIFY(h.overlay.WordCount()>0);
    const auto validated=h.controller->Model().Lines().first().validatedFrameSeq;
    const int calls=h.script->calls.load();
    {
        std::lock_guard lock(h.script->mutex);
        h.script->holdRecognition=true;
    }
    std::uint64_t frame=61;
    for(;frame<=180 && h.script->calls.load()==calls;++frame) h.PlayFrame(frame);
    QTRY_VERIFY(h.script->calls.load()>calls);
    const auto jobs=h.controller->SchedulerStatistics().submitted;
    for(int i=0;i<25;++i) h.PlayFrame(frame++);
    QVERIFY(h.overlay.WordCount()>0);
    QVERIFY(h.controller->Model().Lines().first().validatedFrameSeq>validated);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.controller->SchedulerStatistics().submitted,jobs); // held inference gets no replacement queue

    h.page=MakePage({QStringLiteral("Completely changed content")},QPointF(190,90),960,540,48);
    for(int i=0;i<12;++i) h.PlayFrame(frame++);
    QTRY_COMPARE(h.overlay.WordCount(),0);
    QVERIFY(!h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.script->Release();
    QTest::qWait(20);
    QVERIFY(!h.Reading().contains(QStringLiteral("Reading helps")));
}

void UltraClearTextControllerTests::advancingMediaAuthorityRevokesAndSuppressesLiveInput()
{
    ImmutableHarness h;
    h.script->holdRecognition=true;
    h.Enable();
    std::uint64_t frame=1;
    for(;frame<40 && h.script->calls.load()==0;++frame) h.PlayFrame(frame);
    QTRY_COMPARE(h.script->calls.load(),1);
    const auto oldGeneration=h.controller->GeometryGeneration();
    h.PlayFrame(frame++,9);
    QVERIFY(h.controller->GeometryGeneration()>oldGeneration);
    const auto generation=h.controller->GeometryGeneration();
    QVERIFY(h.Reading().isEmpty());
    h.script->Release();
    for(int i=0;i<70;++i) h.PlayFrame(frame++,9);
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QVERIFY(h.controller->SchedulerStatistics().revoked>=1);
    QCOMPARE(h.controller->Model().Lines().first().provenance.geometryGeneration,generation);
    UctFrameContext camera;
    camera.cameraActive=true;camera.frameSeq=999;camera.geometry.settings.sourceEpoch=999;
    const auto probes=h.controller->ProbeStatistics().submitted;
    const auto jobs=h.controller->SchedulerStatistics().submitted;
    QVERIFY(!h.controller->OnPresented(camera));
    h.controller->OnCpuViewport(camera,h.page.image.constBits(),960,540,h.page.image.bytesPerLine());
    h.controller->OnDeviceScene(camera,{});
    h.controller->OnSourceRetired();h.controller->OnPresentationFailed();
    QCOMPARE(h.controller->GeometryGeneration(),generation);
    QCOMPARE(h.controller->ProbeStatistics().submitted,probes);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,jobs);
    QVERIFY(h.overlay.WordCount()>0);
    h.controller->ClearImmutableScene();
    QVERIFY(!h.controller->HasImmutableScene());
    QVERIFY(h.Reading().isEmpty());
}

void UltraClearTextControllerTests::advancingMediaPauseReturnsToOneShotAnalysis()
{
    ImmutableHarness h;
    h.Enable();
    UctAdvancingMediaScene last;
    for(std::uint64_t frame=1;frame<=60;++frame) last=h.PlayFrame(frame);
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QTest::qWait(20);
    const auto before=h.controller->SchedulerStatistics().submitted;
    UctImmutableScene paused;
    paused.geometry=last.geometry;paused.sourceAuthority=last.sourceAuthority;
    paused.selected=last.current;paused.transform=last.transform;
    paused.viewportWidth=last.viewportWidth;paused.viewportHeight=last.viewportHeight;
    paused.corroboration=UctImmutableFrame{
        OwnedPixels(IndependentFramePixels(h.page.image,last.current.frameSeq+1)),
        last.current.frameSeq+1,last.current.timestampMs+20};
    QVERIFY(h.controller->SetImmutableScene(paused));
    QVERIFY(!h.controller->HasAdvancingMediaScene());
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,before+2);
    const auto probes=h.controller->ProbeStatistics().submitted;
    h.now+=120000;
    QTest::qWait(250);
    QVERIFY(h.overlay.WordCount()>0);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,before+2);
    QCOMPARE(h.controller->ProbeStatistics().submitted,probes);
    QVERIFY(h.controller->SetImmutableScene(paused));
    QCOMPARE(h.controller->SchedulerStatistics().submitted,before+2);
}

void UltraClearTextControllerTests::advancingMediaDeadlineUsesWallClock()
{
    UctTiming timing;timing.immutableJobTimeoutMs=100;
    ImmutableHarness h(timing);
    h.script->holdRecognition=true;
    h.Enable();
    for(std::uint64_t frame=1;frame<40 && h.script->calls.load()==0;++frame) h.PlayFrame(frame);
    QTRY_COMPARE(h.script->calls.load(),1);
    h.now+=101; // media PTS stays fixed; only the current wall job deadline expires
    h.script->Release();
    QTRY_COMPARE(h.controller->RejectedResults(),std::uint64_t(1));
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->Model().Lines().isEmpty());
    QCOMPARE(h.controller->SchedulerStatistics().submitted,std::uint64_t(1));
}

void UltraClearTextControllerTests::mediaPresentationFailureRevokesCachedArtifact_data()
{
    QTest::addColumn<bool>("advancing");
    QTest::newRow("immutable") << false;
    QTest::newRow("advancing") << true;
}

void UltraClearTextControllerTests::mediaPresentationFailureRevokesCachedArtifact()
{
    QFETCH(bool, advancing);
    AccessibilityActivation accessibility;
    ImmutableHarness h;
    h.Enable();
    auto scene = h.Scene();
    if (advancing) {
        for (std::uint64_t frame = 1; frame <= 60; ++frame) h.PlayFrame(frame);
    } else {
        auto neighbour = h.page.image.copy();
        neighbour.setPixelColor(950, 530, Qt::red);
        scene.corroboration = UctImmutableFrame{OwnedPixels(neighbour), 11, 2};
        QVERIFY(h.controller->SetImmutableScene(scene));
    }
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QTest::qWait(20); // drain queued admission before inspecting the retained artifact
    const auto originalWords = h.overlay.Words();
    const auto originalReading = h.Reading();
    QVERIFY(!originalReading.isEmpty());
    const auto originalSnapshot = h.controller->ScreenTextSnapshot();
    QVERIFY(!originalSnapshot.lines.isEmpty());
    auto* screenText = h.controller->ScreenTextAccessibility();
    QVERIFY(screenText);
    QVERIFY(screenText->IsActive());
    QVERIFY(screenText->ExposedLineCount() > 0);
    const auto generation = h.controller->GeometryGeneration();
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    const auto probes = h.controller->ProbeStatistics().submitted;

    h.controller->OnMediaPresentationFailed();
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(screenText->ExposedLineCount(), 0);
    QVERIFY(!h.controller->Model().Lines().isEmpty()); // visible revocation retains valid evidence
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    const auto zoom = ComputeViewTransform(960, 540, 960, 540, 2, .3f, .3f,
                                           ViewportFitMode::kFill);
    h.controller->UpdateImmutableView(zoom, 960, 540);
    h.controller->OnPresentationSucceeded(); // a hidden camera receipt has no media authority
    QTest::qWait(250);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(screenText->ExposedLineCount(), 0);

    h.controller->OnMediaPresentationSucceeded(); // exact retained-frame/native-view receipt
    QCOMPARE(h.overlay.WordCount(), originalWords.size());
    QCOMPARE(h.overlay.Words().first().sceneRect, originalWords.first().sceneRect);
    QCOMPARE(h.Reading(), originalReading);
    const auto restoredSnapshot = h.controller->ScreenTextSnapshot();
    QVERIFY(!restoredSnapshot.lines.isEmpty());
    QCOMPARE(restoredSnapshot.lines.first().text, originalSnapshot.lines.first().text);
    QVERIFY(screenText->ExposedLineCount() > 0);
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.controller->ProbeStatistics().submitted, probes);

    if (!advancing) {
        h.controller->OnMediaPresentationFailed();
        QVERIFY(h.controller->SetImmutableScene(scene)); // exact scene API is itself a real receipt
        QCOMPARE(h.overlay.WordCount(), originalWords.size());
        QCOMPARE(h.Reading(), originalReading);
        QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    }
    h.controller->ClearImmutableScene();
    h.controller->OnMediaPresentationSucceeded(); // a late media receipt cannot restore live text
    QVERIFY(h.Reading().isEmpty());
    QCOMPARE(h.overlay.WordCount(), 0);
}

void UltraClearTextControllerTests::immutablePresentationFailureBlocksBackendReadiness()
{
    ImmutableHarness h;
    h.script->holdInitialization = true;
    auto scene = h.Scene();
    auto neighbour = h.page.image.copy();
    neighbour.setPixelColor(950, 530, Qt::red);
    scene.corroboration = UctImmutableFrame{OwnedPixels(neighbour), 11, 2};
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(h.script->initialized.load(), 1);
    h.controller->OnMediaPresentationFailed();
    h.script->Release(); // readiness and both OCR completions arrive after the presenter fault
    QTRY_COMPARE(h.controller->AcceptedResults(), std::uint64_t(1));
    QCOMPARE(h.script->calls.load(), 2);
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    h.controller->UpdateImmutableView(scene.transform, scene.viewportWidth, scene.viewportHeight);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.controller->OnMediaPresentationSucceeded();
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::advancingPresentationFailureBlocksLateResult()
{
    ImmutableHarness h;
    h.Enable();
    UctAdvancingMediaScene last;
    for (std::uint64_t frame = 1; frame <= 60; ++frame) last = h.PlayFrame(frame);
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    const int calls = h.script->calls.load();
    {
        std::lock_guard lock(h.script->mutex);
        h.script->holdRecognition = true;
    }
    for (std::uint64_t frame = 61; frame <= 180 && h.script->calls.load() == calls; ++frame)
        last = h.PlayFrame(frame);
    QTRY_VERIFY(h.script->calls.load() > calls);
    QVERIFY(h.overlay.WordCount() > 0);
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.controller->OnMediaPresentationFailed();
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.script->Release();
    QTRY_COMPARE(admitted.size(), 1); // an otherwise current late result must not reopen presentation
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    h.controller->UpdateImmutableView(last.transform, last.viewportWidth, last.viewportHeight);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.controller->OnMediaPresentationSucceeded();
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
}

void UltraClearTextControllerTests::deviceImmutableRecognizesWithoutCpuPixels()
{
    ImmutableHarness h;
    auto scene = h.DeviceScene();
    QVERIFY(!scene.selected.pixels);
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(admitted.size(), 1);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.overlay.WordCount(), 0); // one still is reading evidence only
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    const auto& provenance = h.controller->Model().Lines().first().provenance;
    QCOMPARE(provenance.frameSeq, scene.selected.frameSeq);
    QCOMPARE(provenance.timestampMs, std::int64_t(1)); // real source PTS, never refreshed
    QCOMPARE(provenance.viewportWidth, std::uint32_t(960));
    QCOMPARE(provenance.viewportHeight, std::uint32_t(540));
    // The native CPU recognizer fell back through a worker readback.
    QCOMPARE(h.script->calls.load(), 1);
    QCOMPARE(h.script->deviceCalls.load(), 0);
    QVERIFY(h.devices->readbacks.load() >= 1);
    QVERIFY(!h.devices->readOnUiThread.load());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
    QCOMPARE(h.controller->PixelAllocations(), std::uint64_t(0));
    // Exact device lease repeated: projection only, no second analysis.
    QVERIFY(h.controller->SetImmutableScene(scene));
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    // Same frame identity with a new device lease is a new selection.
    auto reselected = scene;
    reselected.selected.device = h.Device(h.page.image, 10, 1);
    const auto generation = h.controller->GeometryGeneration();
    QVERIFY(h.controller->SetImmutableScene(reselected));
    QVERIFY(h.controller->GeometryGeneration() > generation);
    QTRY_COMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
}

void UltraClearTextControllerTests::deviceImmutableDistinctCorroborationDrawsGlyphs()
{
    ImmutableHarness h;
    h.script->wantsDevice = true;
    auto scene = h.DeviceScene();
    auto neighbour = h.page.image.copy();
    neighbour.setPixelColor(950, 530, Qt::red); // genuinely distinct pixels
    scene.corroboration = UctImmutableFrame{{}, 11, 2, h.Device(neighbour, 11, 2)};
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QCOMPARE(h.script->deviceCalls.load(), 2);
    QCOMPARE(h.script->calls.load(), 0); // GPU-preferred path, no CPU recognition
    QVERIFY(!h.devices->readOnUiThread.load());
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::deviceImmutableIdenticalPixelsNeverCorroborate()
{
    ImmutableHarness h;
    auto scene = h.DeviceScene();
    // Different lease and frame identity, byte-identical scene pixels.
    scene.corroboration = UctImmutableFrame{{}, 11, 2, h.Device(h.page.image, 11, 2)};
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(admitted.size(), 2);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QTest::qWait(250);
    QCOMPARE(h.script->calls.load(), 2); // bounded attempt, no retry loop
}

void UltraClearTextControllerTests::deviceImmutableRejectsMismatchedImageAndFallsBackToPixels()
{
    { // dimensions do not cover the declared scene
        ImmutableHarness h;
        auto scene = h.DeviceScene();
        const QImage small(64, 64, QImage::Format_ARGB32);
        scene.selected.device = h.Device(small, 10, 1);
        QVERIFY(!h.controller->SetImmutableScene(scene));
        h.Enable();
        QVERIFY(h.controller->HasImmutableScene()); // live input stays suspended
        QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
        QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
        QVERIFY(h.Reading().isEmpty());
    }
    { // device frame identity disagrees with the frame's authoritative stamp
        ImmutableHarness h;
        auto scene = h.DeviceScene();
        scene.selected.device = h.Device(h.page.image, 99, 1);
        QVERIFY(!h.controller->SetImmutableScene(scene));
        h.Enable();
        QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
        QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
    }
    { // device source epoch disagrees with the scene geometry
        ImmutableHarness h;
        auto scene = h.DeviceScene();
        scene.selected.device = h.Device(h.page.image, 10, 1, 7);
        QVERIFY(!h.controller->SetImmutableScene(scene));
        h.Enable();
        QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
        QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
    }
    { // a broken device lease must not lose UCT when CPU pixels are owned too
        ImmutableHarness h;
        auto scene = h.Scene();
        const QImage small(64, 64, QImage::Format_ARGB32);
        scene.selected.device = h.Device(small, 10, 1);
        h.Enable();
        QVERIFY(h.controller->SetImmutableScene(scene));
        QTRY_VERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
        QCOMPARE(h.script->calls.load(), 1);
    }
}

void UltraClearTextControllerTests::deviceAdvancingMediaPacesAndValidates()
{
    ImmutableHarness h;
    h.Enable();
    UctAdvancingMediaScene last;
    for (std::uint64_t frame = 1; frame <= 120 && h.overlay.WordCount() == 0; ++frame)
        last = h.PlayDeviceFrame(frame);
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.controller->HasAdvancingMediaScene());
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    const std::uint64_t lastFrame = last.current.frameSeq;
    for (std::uint64_t frame = lastFrame + 1; frame <= lastFrame + 40; ++frame)
        last = h.PlayDeviceFrame(frame);
    QVERIFY(h.overlay.WordCount() > 0);
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    QVERIFY2(jobs >= 2 && jobs <= 12, "Advancing device media must pace OCR, not submit per frame");
    QVERIFY(h.devices->probes.load() >= 5);
    QVERIFY(h.controller->ProbeStatistics().completed >= 5);
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    const auto& line = h.controller->Model().Lines().first();
    QVERIFY(line.validatedFrameSeq > line.provenance.frameSeq); // independent probes keep validating
    QVERIFY(line.provenance.timestampMs >= 10000 && line.provenance.timestampMs <= 14000);
    QVERIFY(line.validatedAtMs >= line.provenance.timestampMs);
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->PixelAllocations(), std::uint64_t(0)); // no UI viewport copy
    QVERIFY(!h.devices->readOnUiThread.load());
    QVERIFY(h.script->calls.load() + h.script->deviceCalls.load() >= 2);
}

void UltraClearTextControllerTests::deviceAdvancingSeekAndBackwardFrameRevoke()
{
    ImmutableHarness h;
    h.script->holdRecognition = true;
    h.Enable();
    std::uint64_t frame = 1;
    for (; frame < 60 && h.script->calls.load() == 0; ++frame) h.PlayDeviceFrame(frame);
    QTRY_COMPARE(h.script->calls.load(), 1); // recognition held in flight
    const auto beforeSeek = h.controller->GeometryGeneration();
    h.PlayDeviceFrame(frame++, 9); // seek: new source authority
    QVERIFY(h.controller->GeometryGeneration() > beforeSeek);
    QVERIFY(h.Reading().isEmpty());
    const auto seekGeneration = h.controller->GeometryGeneration();
    h.script->Release();
    for (int i = 0; i < 120 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++, 9);
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.controller->SchedulerStatistics().revoked >= 1);
    QCOMPARE(h.controller->Model().Lines().first().provenance.geometryGeneration, seekGeneration);
    // A backward frame under the same authority is a discontinuity too.
    const auto beforeBackward = h.controller->GeometryGeneration();
    h.PlayDeviceFrame(2, 9);
    QVERIFY(h.controller->GeometryGeneration() > beforeBackward);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
}

void UltraClearTextControllerTests::deviceAdvancingSlowJobUsesWallDeadline()
{
    UctTiming timing;
    timing.immutableJobTimeoutMs = 100;
    ImmutableHarness h(timing);
    h.script->holdRecognition = true;
    h.Enable();
    for (std::uint64_t frame = 1; frame < 60 && h.script->calls.load() == 0; ++frame)
        h.PlayDeviceFrame(frame);
    QTRY_COMPARE(h.script->calls.load(), 1);
    h.now += 101; // media PTS unchanged; only the wall job deadline expires
    h.script->Release();
    QTRY_COMPARE(h.controller->RejectedResults(), std::uint64_t(1));
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->Model().Lines().isEmpty());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
}

void UltraClearTextControllerTests::deviceAdvancingProbeFailureRecovers()
{
    ImmutableHarness h;
    h.devices->probeFails = true;
    h.Enable();
    std::uint64_t frame = 1;
    for (; frame <= 80 && !h.controller->StatusSource().contains(QStringLiteral("stopped working")); ++frame)
        h.PlayDeviceFrame(frame);
    QVERIFY2(h.controller->StatusSource().contains(QStringLiteral("stopped working")),
             qPrintable(h.controller->StatusSource()));
    QCOMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
    QVERIFY(h.Reading().isEmpty());
    QCOMPARE(h.overlay.WordCount(), 0);
    h.devices->probeFails = false; // copies recover: UCT must not stay lost
    for (int i = 0; i < 160 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++);
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(!h.controller->StatusSource().contains(QStringLiteral("stopped working")));
}

void UltraClearTextControllerTests::deviceAdvancingReleasesConsumedAnalysisLease()
{
    ImmutableHarness h;
    h.Enable();
    std::map<std::uint64_t, std::weak_ptr<const DeviceImage>> leases;
    for (std::uint64_t frame = 1; frame <= 160 && h.overlay.WordCount() == 0; ++frame) {
        const auto scene = h.PlayDeviceFrame(frame);
        leases[frame] = scene.current.device; // the returned scene copy drops here
    }
    QVERIFY(h.overlay.WordCount() > 0);
    const auto provenFrame = h.controller->Model().Lines().first().provenance.frameSeq;
    const auto lease = leases.find(provenFrame);
    QVERIFY(lease != leases.end());
    // Validation and recognition consumed this exact device image. Once they
    // finish, the retained advancing scene must not keep its analysis-pool
    // lease alive merely as receipt metadata...
    QTRY_VERIFY_WITH_TIMEOUT(lease->second.expired(), 5000);
    // ...while the displayed scene remains presentable and its text drawn.
    QVERIFY(h.controller->HasAdvancingMediaScene());
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
}

void UltraClearTextControllerTests::gapReceiptRetainsBrieflyThenMediaExpiryHidesAndCurrentInputRecovers()
{
    ImmutableHarness h;
    h.Enable();
    const std::uint64_t last = h.EstablishDeviceGlyphs();
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    const auto line = h.controller->Model().Lines().first();
    const auto generation = h.controller->GeometryGeneration();
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    const auto probes = h.controller->ProbeStatistics().submitted;
    const int deviceProbes = h.devices->probes.load();
    const int deviceReadbacks = h.devices->readbacks.load();

    // Ordinary forward gap at 4x media speed (20 ms PTS per 5 ms wall):
    // retained while the previous real validation is media-fresh.
    std::uint64_t frame = last + 1;
    while (10000 + static_cast<std::int64_t>(frame) * 20 <= line.validatedAtMs + 200) {
        QVERIFY(h.PresentGap(frame++, 3, 5));
    }
    QVERIFY(frame > last + 1); // at least one gap frame was actually presented
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->AdvancingDisplayFrameSeq(), frame - 1);
    QCOMPARE(h.controller->AdvancingDisplayTimestampMs(),
             10000 + static_cast<std::int64_t>(frame - 1) * 20);
    // No synthetic work and no restamping of the old analysis frame.
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.controller->ProbeStatistics().submitted, probes);
    QCOMPARE(h.devices->probes.load(), deviceProbes);
    QCOMPARE(h.devices->readbacks.load(), deviceReadbacks);
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, line.provenance.frameSeq);
    QCOMPARE(h.controller->Model().Lines().first().provenance.timestampMs, line.provenance.timestampMs);
    QCOMPARE(h.controller->Model().Lines().first().validatedAtMs, line.validatedAtMs);
    QCOMPARE(h.controller->Model().Lines().first().validatedFrameSeq, line.validatedFrameSeq);
    QVERIFY(!h.devices->readOnUiThread.load());

    // Media expiry: displayed PTS passes the 350 ms validation bound while
    // the wall advance stays well inside it.
    while (10000 + static_cast<std::int64_t>(frame) * 20 <= line.validatedAtMs + 350) {
        QVERIFY(h.PresentGap(frame++, 3, 5));
    }
    QVERIFY(h.PresentGap(frame++, 3, 5)); // first receipt beyond the bound
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QVERIFY(h.controller->HasAdvancingMediaScene()); // expiry is not a discontinuity
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);

    // The next real device frames continue the same chain and recover text.
    for (int i = 0; i < 240 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++);
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QVERIFY(h.controller->Model().Lines().first().validatedFrameSeq > line.validatedFrameSeq);
}

void UltraClearTextControllerTests::gapReceiptWallExpiryHidesDespiteFreshMediaPts()
{
    ImmutableHarness h;
    h.Enable();
    const std::uint64_t last = h.EstablishDeviceGlyphs();
    QVERIFY(h.overlay.WordCount() > 0);
    const auto generation = h.controller->GeometryGeneration();
    const auto jobs = h.controller->SchedulerStatistics().submitted;

    // One forward frame, only +20 ms of media PTS, but the wall clock has
    // run far past the validation bound (slow motion / stalled presenter).
    QVERIFY(h.PresentGap(last + 1, 3, 5000));
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->GeometryGeneration(), generation); // expiry, not revocation
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);

    std::uint64_t frame = last + 2;
    for (int i = 0; i < 240 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++);
    QVERIFY(h.overlay.WordCount() > 0);
    QCOMPARE(h.controller->GeometryGeneration(), generation);
}

void UltraClearTextControllerTests::gapReceiptDiscontinuitiesRevokeImmediately()
{
    { // seek: new source authority, even with a forward frame
        ImmutableHarness h;
        h.Enable();
        const std::uint64_t last = h.EstablishDeviceGlyphs();
        QVERIFY(h.overlay.WordCount() > 0);
        const auto generation = h.controller->GeometryGeneration();
        QVERIFY(!h.PresentGap(last + 1, 9));
        QVERIFY(h.controller->GeometryGeneration() > generation);
        QCOMPARE(h.overlay.WordCount(), 0);
        QVERIFY(h.Reading().isEmpty());
        QVERIFY(h.controller->Model().Lines().isEmpty());
        QVERIFY(h.controller->HasAdvancingMediaScene()); // display-only chain suspends live
        // Real frames under the new authority establish a new chain.
        std::uint64_t frame = last + 2;
        for (int i = 0; i < 240 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++, 9);
        QVERIFY(h.overlay.WordCount() > 0);
    }
    { // backward frame under the same authority
        ImmutableHarness h;
        h.Enable();
        const std::uint64_t last = h.EstablishDeviceGlyphs();
        QVERIFY(h.overlay.WordCount() > 0);
        const auto generation = h.controller->GeometryGeneration();
        QVERIFY(!h.PresentGap(last - 2));
        QVERIFY(h.controller->GeometryGeneration() > generation);
        QCOMPARE(h.overlay.WordCount(), 0);
        QVERIFY(h.Reading().isEmpty());
        QVERIFY(h.controller->Model().Lines().isEmpty());
    }
    { // geometry change carried by the display receipt
        ImmutableHarness h;
        h.Enable();
        const std::uint64_t last = h.EstablishDeviceGlyphs();
        QVERIFY(h.overlay.WordCount() > 0);
        const auto generation = h.controller->GeometryGeneration();
        auto receipt = h.GapReceipt(last + 1);
        receipt.geometry.sceneWidth = 640;
        receipt.geometry.sceneHeight = 360;
        h.now += 20;
        QVERIFY(!h.controller->OnAdvancingMediaPresentedWithoutAnalysis(receipt));
        QVERIFY(h.controller->GeometryGeneration() > generation);
        QCOMPARE(h.overlay.WordCount(), 0);
        QVERIFY(h.Reading().isEmpty());
    }
    { // forward sequence with backward PTS, and same sequence with new PTS
        ImmutableHarness h;
        h.Enable();
        const std::uint64_t last = h.EstablishDeviceGlyphs();
        QVERIFY(h.overlay.WordCount() > 0);
        const auto generation = h.controller->GeometryGeneration();
        auto receipt = h.GapReceipt(last + 1);
        receipt.timestampMs -= 40; // PTS moved backward
        h.now += 20;
        QVERIFY(!h.controller->OnAdvancingMediaPresentedWithoutAnalysis(receipt));
        QVERIFY(h.controller->GeometryGeneration() > generation);
        QCOMPARE(h.overlay.WordCount(), 0);

        auto sameFrame = receipt; // the receipt above is the display now
        sameFrame.timestampMs += 20; // same sequence, inconsistent PTS
        const auto afterFirst = h.controller->GeometryGeneration();
        h.now += 20;
        QVERIFY(!h.controller->OnAdvancingMediaPresentedWithoutAnalysis(sameFrame));
        QVERIFY(h.controller->GeometryGeneration() > afterFirst);
    }
}

void UltraClearTextControllerTests::gapReceiptLateOldRecognitionCannotReviveAfterExpiry()
{
    ImmutableHarness h;
    h.Enable();
    auto frame = h.EstablishDeviceGlyphs() + 1;
    QVERIFY(h.overlay.WordCount() > 0);
    const int calls = h.script->calls.load() + h.script->deviceCalls.load();
    {
        std::lock_guard lock(h.script->mutex);
        h.script->holdRecognition = true;
    }
    for (; frame < 400 && h.script->calls.load() + h.script->deviceCalls.load() == calls; ++frame)
        h.PlayDeviceFrame(frame);
    QVERIFY(h.script->calls.load() + h.script->deviceCalls.load() > calls); // refresh held in flight
    QVERIFY(h.overlay.WordCount() > 0);

    // No-analysis gap runs both freshness bounds out; text hides on expiry.
    for (int i = 0; i < 30; ++i) QVERIFY(h.PresentGap(frame++, 3, 20));
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());

    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.script->Release(); // the held, now-old recognition completes late
    QTRY_COMPARE(admitted.size(), 1);
    QTest::qWait(20);
    QCOMPARE(h.overlay.WordCount(), 0); // it cannot revive expired text
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());

    for (int i = 0; i < 240 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++);
    QVERIFY(h.overlay.WordCount() > 0); // later current input recovers
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
}

void UltraClearTextControllerTests::gapReceiptSameFrameReprojectsRepeatedlyWithoutWork()
{
    ImmutableHarness h;
    h.Enable();
    const std::uint64_t last = h.EstablishDeviceGlyphs();
    QVERIFY(h.overlay.WordCount() > 0);
    QTest::qWait(20); // drain in-flight probes so counters below are stable
    const auto words = h.overlay.Words();
    const auto rects = h.overlay.ProjectedWordRects();
    const auto generation = h.controller->GeometryGeneration();
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    const auto probes = h.controller->ProbeStatistics().submitted;
    const int deviceProbes = h.devices->probes.load();
    const auto provenance = h.controller->Model().Lines().first().provenance;

    // The same displayed frame presented again under two different views:
    // pure reprojection, repeated, with no probe/OCR or restamping.
    auto receipt = h.GapReceipt(last);
    receipt.transform = ComputeViewTransform(960, 540, 960, 540, 2.0f, 0.3f, 0.3f,
                                             ViewportFitMode::kFill);
    QVERIFY(receipt.transform.valid);
    h.now += 20;
    QVERIFY(h.controller->OnAdvancingMediaPresentedWithoutAnalysis(receipt));
    QCOMPARE(h.overlay.WordCount(), words.size());
    QCOMPARE(h.overlay.Words().first().sceneRect, words.first().sceneRect);
    QCOMPARE(h.overlay.ProjectedWordRects().first(),
             SceneRectToView(receipt.transform, h.overlay.width(), h.overlay.height(),
                             words.first().sceneRect));
    QVERIFY(h.overlay.ProjectedWordRects().first() != rects.first());

    receipt.transform = ComputeViewTransform(960, 540, 960, 540, 3.0f, 0.6f, 0.4f,
                                             ViewportFitMode::kFill);
    h.now += 20;
    QVERIFY(h.controller->OnAdvancingMediaPresentedWithoutAnalysis(receipt));
    QCOMPARE(h.overlay.WordCount(), words.size());
    QCOMPARE(h.overlay.ProjectedWordRects().first(),
             SceneRectToView(receipt.transform, h.overlay.width(), h.overlay.height(),
                             words.first().sceneRect));
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.controller->ProbeStatistics().submitted, probes);
    QCOMPARE(h.devices->probes.load(), deviceProbes);
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, provenance.frameSeq);
    QCOMPARE(h.controller->Model().Lines().first().provenance.timestampMs, provenance.timestampMs);
    QCOMPARE(h.controller->AdvancingDisplayFrameSeq(), last);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Preserve an explicit file logger and replay its bytes through the
    // executable's stdout. The implicit QtTest logger left the first tracked
    // invocation with a failure code and no captured diagnostics.
    const QString root = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(
        QStringLiteral("OkuFlow/test-evidence/immutable-uct"));
    if (!QDir().mkpath(root)) {
        std::fprintf(stderr, "Cannot create immutable UCT evidence directory.\n");
        return 1;
    }
    const QString report = QDir(root).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".txt"));
    std::fprintf(stdout, "Immutable UCT QtTest report: %s\n", qPrintable(report));
    std::fflush(stdout);
    QStringList arguments = QCoreApplication::arguments();
    arguments << QStringLiteral("-o") << report + QStringLiteral(",txt");
    UltraClearTextControllerTests tests;
    const int result = QTest::qExec(&tests, arguments);
    QFile saved(report);
    if (saved.open(QIODevice::ReadOnly)) {
        const auto bytes = saved.readAll();
        std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
    } else {
        std::fprintf(stderr, "Cannot read immutable UCT QtTest report.\n");
    }
    std::fprintf(stdout, "Immutable UCT QtTest exit code: %d\n", result);
    std::fflush(stdout);
    return result;
}

#include "ultra_clear_text_controller_tests.moc"
