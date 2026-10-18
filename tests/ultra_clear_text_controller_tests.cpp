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
#include <memory>
#include <mutex>

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

    RecognitionOutput Recognize(const QImage& image, const std::atomic<bool>& cancelled) override
    {
        std::unique_lock lock(script_->mutex);
        const auto words = script_->words;
        ++script_->calls;
        script_->changed.wait(lock, [&] { return !script_->holdRecognition; });
        RecognitionOutput output;
        output.cancelled = cancelled.load();
        output.ok = !output.cancelled;
        if (!output.ok) return output;
        RecognizedLine line;
        for (const PageWord& word : words) {
            line.words.push_back({word.text,
                QRectF(word.sceneRect.x() * image.width(), word.sceneRect.y() * image.height(),
                       word.sceneRect.width() * image.width(), word.sceneRect.height() * image.height())});
        }
        output.lines.push_back(std::move(line));
        return output;
    }

private:
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

struct ImmutableHarness {
    std::shared_ptr<ImmutableScript> script = std::make_shared<ImmutableScript>();
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
};

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
