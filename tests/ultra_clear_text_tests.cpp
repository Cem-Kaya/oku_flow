// Desktop-free Ultra Clear Text contract fixture. Runs under Qt offscreen:
// the overlay and panel are real production widgets but create no native
// windows. Recognition uses scripted fakes; the real Windows OCR backend has
// its own optional acceptance fixture.

#include "okuflow/app/ultra_clear_text_controller.hpp"
#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include "ultra_clear_text_test_support.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QHash>
#include <QLabel>
#include <QPlainTextEdit>
#include <QtTest>

#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>

using namespace okuflow;
using namespace okuflow::uct;
using namespace uct_test;

namespace {

constexpr int kViewWidth = 960;
constexpr int kViewHeight = 540;
constexpr int kSceneWidth = 1600;
constexpr int kSceneHeight = 900;

ViewTransform Transform(float zoom, float fx, float fy, ViewportFitMode mode = ViewportFitMode::kFit)
{
    return ComputeViewTransform(kSceneWidth, kSceneHeight, kViewWidth, kViewHeight, zoom, fx, fy, mode);
}

const QStringList kLines{QStringLiteral("Reading glasses help"),
                         QStringLiteral("people see printed pages"),
                         QStringLiteral("clearly every morning")};

quint64 Checksum(const QImage& image)
{
    return qHashBits(image.constBits(), static_cast<size_t>(image.sizeInBytes()));
}

// Recognizer whose output is scripted from ground truth and which can be
// held to emulate arbitrarily slow OCR.
struct ScriptShared {
    std::mutex mutex;
    std::condition_variable cv;
    bool hold{};
    int calls{};
    int inputMutations{};
    BackendState initState{BackendState::kReady};
    QHash<quint64, ViewTransform> transforms; // image checksum -> transform
    QVector<PageWord> words;                  // scene ground truth
    bool fail{};

    void SetWords(const QVector<PageWord>& value)
    {
        std::lock_guard lock(mutex);
        words = value;
    }

    void SetHold(bool value)
    {
        {
            std::lock_guard lock(mutex);
            hold = value;
        }
        cv.notify_all();
    }
};

class ScriptedRecognizer final : public Recognizer {
public:
    explicit ScriptedRecognizer(std::shared_ptr<ScriptShared> shared) : shared_(std::move(shared)) {}

    BackendStatus Initialize() override
    {
        BackendStatus status;
        status.name = QStringLiteral("scripted");
        std::lock_guard lock(shared_->mutex);
        status.state = shared_->initState;
        status.detail = status.state == BackendState::kReady
                            ? QStringLiteral("ready")
                            : QStringLiteral("Windows has no OCR-capable language installed.");
        return status;
    }

    RecognitionOutput Recognize(const QImage& image, const std::atomic<bool>& cancelled) override
    {
        const quint64 before = Checksum(image);
        std::unique_lock lock(shared_->mutex);
        ++shared_->calls;
        shared_->cv.wait(lock, [&] { return !shared_->hold; });
        RecognitionOutput output;
        if (Checksum(image) != before) {
            ++shared_->inputMutations; // input must stay owned and unchanged
        }
        if (shared_->fail) {
            output.error = QStringLiteral("scripted failure");
            return output;
        }
        output.cancelled = cancelled.load();
        output.ok = !output.cancelled;
        const auto it = shared_->transforms.constFind(before);
        if (it == shared_->transforms.constEnd()) {
            return output; // unknown image: recognize nothing
        }
        // One recognized line per printed row, like a real OCR engine.
        for (const PageWord& word : shared_->words) {
            const QRectF px = SceneRectToView(it.value(), image.width(), image.height(), word.sceneRect);
            if (!QRectF(image.rect()).contains(px)) {
                continue;
            }
            RecognizedLine* row = nullptr;
            for (RecognizedLine& line : output.lines) {
                if (std::abs(line.words.first().boundsPx.top() - px.top()) < px.height() * 0.5) {
                    row = &line;
                }
            }
            if (!row) {
                output.lines.push_back({});
                row = &output.lines.last();
            }
            row->words.push_back({word.text, px});
        }
        return output;
    }

private:
    std::shared_ptr<ScriptShared> shared_;
};

// Drives the production controller like the presenter does: OnPresented on
// every frame, and an asynchronous viewport copy only when requested.
struct Harness {
    std::shared_ptr<ScriptShared> shared = std::make_shared<ScriptShared>();
    std::int64_t now{1000};
    QWidget owner;
    QWidget* target{};
    UltraClearTextOverlay* overlay{};
    UltraClearTextPanel* panel{};
    std::unique_ptr<UltraClearTextController> controller;
    SyntheticPage page;
    std::uint64_t nextReadback{1};
    std::uint64_t frameSeq{1};
    UctFrameContext context;
    double maxPresentMs{};
    double maxProbeMs{};
    bool completeReadbacks{true};

    explicit Harness(BackendState initState = BackendState::kReady)
    {
        shared->initState = initState;
        owner.resize(kViewWidth / 2 + 20, kViewHeight / 2 + 20);
        target = new QWidget(&owner);
        // Logical overlay is half the viewport's physical pixels (200% DPI).
        target->setGeometry(10, 10, kViewWidth / 2, kViewHeight / 2);
        overlay = new UltraClearTextOverlay(target, &owner);
        panel = new UltraClearTextPanel(&owner);
        owner.show();
        auto sharedCopy = shared;
        controller = std::make_unique<UltraClearTextController>(
            overlay, panel, [sharedCopy] { return std::make_unique<ScriptedRecognizer>(sharedCopy); },
            nullptr, [this] { return now; });
        SetPage(MakePage(kLines));
        context.geometry = {7, kSceneWidth, kSceneHeight, 0, false, false};
        context.transform = Transform(1.0f, 0.5f, 0.5f);
        context.viewportWidth = kViewWidth;
        context.viewportHeight = kViewHeight;
        context.cameraActive = true;
    }

    ~Harness()
    {
        shared->SetHold(false);
        controller.reset();
    }

    void SetPage(const SyntheticPage& value)
    {
        page = value;
        shared->SetWords(page.words);
    }

    void Enable()
    {
        panel->enableCheckbox()->setChecked(true);
        QTRY_VERIFY(controller->BackendStatusSnapshot().state != BackendState::kNotStarted);
    }

    // One presentation tick; returns whether a copy was requested.
    bool Present(bool newFrame = true, std::int64_t stepMs = 10)
    {
        now += stepMs;
        if (newFrame) {
            ++frameSeq;
        }
        context.frameSeq = frameSeq;
        QElapsedTimer timer;
        timer.start();
        const bool wanted = controller->OnPresented(context);
        maxPresentMs = std::max(maxPresentMs, timer.nsecsElapsed() / 1e6);
        if (!wanted) {
            return false;
        }
        const std::uint64_t id = nextReadback++;
        controller->OnReadbackRequested(id, context);
        if (completeReadbacks) {
            const QImage view = RenderView(page.image, context.transform, kViewWidth, kViewHeight);
            {
                std::lock_guard lock(shared->mutex);
                shared->transforms.insert(Checksum(view), context.transform);
            }
            QElapsedTimer probeTimer;
            probeTimer.start();
            controller->OnReadbackCompleted(id, view.constBits(), view.width(), view.height(),
                                            static_cast<int>(view.bytesPerLine()));
            maxProbeMs = std::max(maxProbeMs, probeTimer.nsecsElapsed() / 1e6);
        }
        return true;
    }

    void Run(int frames, std::int64_t stepMs = 10)
    {
        for (int i = 0; i < frames; ++i) {
            Present(true, stepMs);
            QCoreApplication::processEvents();
        }
    }

    bool RunUntilShown(int maxFrames = 400)
    {
        for (int i = 0; i < maxFrames; ++i) {
            Present();
            QCoreApplication::processEvents();
            if (overlay->WordCount() > 0) {
                return true;
            }
            QThread::msleep(1);
        }
        return false;
    }
};

class HeldRecognizer final : public Recognizer {
public:
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        bool release{};
        int calls{};
        int mutations{};
    };
    explicit HeldRecognizer(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}
    BackendStatus Initialize() override
    {
        BackendStatus s;
        s.state = BackendState::kReady;
        return s;
    }
    RecognitionOutput Recognize(const QImage& image, const std::atomic<bool>& cancelled) override
    {
        const quint64 before = Checksum(image);
        std::unique_lock lock(shared_->mutex);
        ++shared_->calls;
        shared_->cv.wait(lock, [&] { return shared_->release; });
        if (Checksum(image) != before) {
            ++shared_->mutations;
        }
        RecognitionOutput out;
        out.ok = !cancelled.load();
        out.cancelled = cancelled.load();
        return out;
    }

private:
    std::shared_ptr<Shared> shared_;
};

QImage SolidImage(int seed)
{
    QImage image(64, 32, QImage::Format_ARGB32);
    image.fill(QColor(seed % 255, (seed * 7) % 255, (seed * 13) % 255));
    return image;
}

SourceStamp Stamp(std::uint64_t epoch, std::uint64_t generation, std::int64_t timestamp,
                  const ViewTransform& t)
{
    SourceStamp stamp;
    stamp.sourceEpoch = epoch;
    stamp.geometryGeneration = generation;
    stamp.timestampMs = timestamp;
    stamp.viewTransform = t;
    stamp.viewportWidth = kViewWidth;
    stamp.viewportHeight = kViewHeight;
    return stamp;
}

// Builds a recognition result as the scheduler would, from a rendered view.
RecognitionResult ResultFor(const SyntheticPage& page, const ViewTransform& t, const SourceStamp& stamp,
                            const QStringList& replaceText = {})
{
    const QImage view = RenderView(page.image, t, kViewWidth, kViewHeight);
    RecognitionResult result;
    result.stamp = stamp;
    result.stamp.viewTransform = t;
    result.output.ok = true;
    int index = 0;
    for (const PageWord& word : page.words) {
        const QString text = index < replaceText.size() ? replaceText[index] : word.text;
        const QRectF px = SceneRectToView(t, kViewWidth, kViewHeight, word.sceneRect);
        // One recognized line per printed row.
        if (result.output.lines.isEmpty() ||
            std::abs(result.output.lines.last().words.first().boundsPx.top() - px.top()) > px.height() * 0.5) {
            result.output.lines.push_back({});
            result.lineColors.push_back({});
        }
        result.output.lines.last().words.push_back({text, px});
        ++index;
    }
    result.reference = MakeProbeFrame(view.constBits(), view.width(), view.height(),
                                      static_cast<int>(view.bytesPerLine()), 2, result.stamp);
    return result;
}

} // namespace

class UltraClearTextTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { RegisterFixtureFonts(); }

    // --- Geometry -----------------------------------------------------------
    void geometryRoundTripsThroughFitFillZoomPanAndDpi();

    // --- Validation primitives ----------------------------------------------
    void patchMatchesUnchangedTextAcrossPanAndZoom();
    void patchTracksSmallMotionAndRejectsLargeMotion();
    void patchRejectsChangedContent();
    void highRiskTokensFailClosed();

    // --- Model --------------------------------------------------------------
    void modelRequiresTemporalAgreement();
    void modelRejectsStaleSceneAndLateResults();
    void modelRejectsResultWhoseContentMoved();
    void modelHidesTextOnMotionAndExpiresFreshness();
    void modelReprojectsWithCurrentTransform();
    void modelOmitsHighRiskWordsFromReadingText();

    // --- Scheduler ----------------------------------------------------------
    void schedulerCoalescesLatestOnly();
    void schedulerCancelRevokesWithoutReleasingInput();
    void schedulerDestructionIsBoundedWhileHeld();

    // --- Controller (production widgets, scripted OCR) ----------------------
    void slowOcrNeverBlocksPresentationClock();
    void controllerCoalescesChangingSceneWhileOcrHeld();
    void acceptedTextReprojectsOnPresentationClockWithDpi();
    void sourceSwitchAndGeometryChangeInvalidateImmediately();
    void contentMotionHidesTextAndStaleProbeExpires();
    void compositeFramesPauseAndShowOriginalPreservesState();
    void unavailableBackendIsReportedHonestly();
    void originalPixelsAndNonTextGraphicsArePreserved();
    void presentationWatchdogHidesTextWithoutPresents();
    void probeWorkIsBoundedForLargeViewports();
    void panelExposesAccessibleControls();
};

void UltraClearTextTests::geometryRoundTripsThroughFitFillZoomPanAndDpi()
{
    const ViewTransform transforms[] = {
        Transform(1.0f, 0.5f, 0.5f, ViewportFitMode::kFit),
        Transform(1.0f, 0.5f, 0.5f, ViewportFitMode::kFill),
        Transform(2.5f, 0.2f, 0.8f, ViewportFitMode::kFill),
        ComputeViewTransform(900, 1600, kViewWidth, kViewHeight, 1.7f, 0.4f, 0.6f, ViewportFitMode::kFit),
    };
    for (const ViewTransform& t : transforms) {
        QVERIFY(t.valid);
        for (const QPointF scene : {QPointF(0.3, 0.4), QPointF(0.5, 0.5), QPointF(0.61, 0.27)}) {
            const QPointF view = SceneToViewPixel(t, kViewWidth, kViewHeight, scene);
            QPointF back;
            QVERIFY(ViewPixelToScene(t, kViewWidth, kViewHeight, view, back));
            QVERIFY(std::abs(back.x() - scene.x()) < 1e-6);
            QVERIFY(std::abs(back.y() - scene.y()) < 1e-6);
            // DPI is applied exactly once: halving the surface halves pixels.
            const QPointF logical = SceneToViewPixel(t, kViewWidth / 2.0, kViewHeight / 2.0, scene);
            QVERIFY(std::abs(logical.x() * 2.0 - view.x()) < 1e-6);
            QVERIFY(std::abs(logical.y() * 2.0 - view.y()) < 1e-6);
        }
    }
    QPointF ignored;
    QVERIFY(!ViewPixelToScene(ViewTransform{}, kViewWidth, kViewHeight, QPointF(1, 1), ignored));
}

void UltraClearTextTests::patchMatchesUnchangedTextAcrossPanAndZoom()
{
    const SyntheticPage page = MakePage(kLines);
    const ViewTransform t0 = Transform(1.0f, 0.5f, 0.5f);
    const ViewTransform t1 = Transform(1.6f, 0.35f, 0.3f, ViewportFitMode::kFill);
    const ProbeFrame reference = ProbeOf(RenderView(page.image, t0, kViewWidth, kViewHeight), t0, 1, 1, 1, 0);
    QVERIFY(reference.IsValid());
    const QRectF line = page.words[0].sceneRect.united(page.words[1].sceneRect);
    const ReferencePatch patch = BuildReferencePatch(reference, line);
    QVERIFY(patch.valid);
    const PatchMatch same = MatchPatch(patch, reference);
    QVERIFY(same.matched);
    QVERIFY(same.score > 0.98);
    // A different view of the same scene still registers at zero offset.
    const ProbeFrame zoomed = ProbeOf(RenderView(page.image, t1, kViewWidth, kViewHeight), t1, 1, 1, 2, 10);
    const PatchMatch moved = MatchPatch(patch, zoomed);
    QVERIFY2(moved.matched, qPrintable(QString::number(moved.score)));
    QVERIFY(std::abs(moved.sceneOffset.x()) < 0.003);
    QVERIFY(std::abs(moved.sceneOffset.y()) < 0.003);
}

void UltraClearTextTests::patchTracksSmallMotionAndRejectsLargeMotion()
{
    const ViewTransform t = Transform(1.0f, 0.5f, 0.5f);
    const SyntheticPage page = MakePage(kLines);
    const ProbeFrame reference = ProbeOf(RenderView(page.image, t, kViewWidth, kViewHeight), t, 1, 1, 1, 0);
    const QRectF line = page.words[0].sceneRect.united(page.words[2].sceneRect);
    const ReferencePatch patch = BuildReferencePatch(reference, line);
    QVERIFY(patch.valid);

    // Six scene pixels right (~3.6 viewport px, ~1.8 probe px).
    const SyntheticPage small = MakePage(kLines, QPointF(6.0, 0.0));
    const ProbeFrame smallProbe = ProbeOf(RenderView(small.image, t, kViewWidth, kViewHeight), t, 1, 1, 2, 10);
    const PatchMatch tracked = MatchPatch(patch, smallProbe);
    QVERIFY2(tracked.matched, qPrintable(QString::number(tracked.score)));
    QVERIFY(std::abs(tracked.sceneOffset.x() - 6.0 / kSceneWidth) < 1.5 / kSceneWidth * 2.0);

    // Eighty scene pixels: beyond the validated search window.
    const SyntheticPage large = MakePage(kLines, QPointF(80.0, 30.0));
    const ProbeFrame largeProbe = ProbeOf(RenderView(large.image, t, kViewWidth, kViewHeight), t, 1, 1, 3, 20);
    QVERIFY(!MatchPatch(patch, largeProbe).matched);
}

void UltraClearTextTests::patchRejectsChangedContent()
{
    const ViewTransform t = Transform(1.0f, 0.5f, 0.5f);
    const SyntheticPage page = MakePage(kLines);
    const SyntheticPage other = MakePage({QStringLiteral("Totally different words"),
                                          QStringLiteral("appear on this board now"),
                                          QStringLiteral("after the teacher wrote")});
    const ProbeFrame reference = ProbeOf(RenderView(page.image, t, kViewWidth, kViewHeight), t, 1, 1, 1, 0);
    const ProbeFrame changed = ProbeOf(RenderView(other.image, t, kViewWidth, kViewHeight), t, 1, 1, 2, 10);
    const QRectF line = page.words[0].sceneRect.united(page.words[2].sceneRect);
    const ReferencePatch patch = BuildReferencePatch(reference, line);
    QVERIFY(patch.valid);
    QVERIFY(!MatchPatch(patch, changed).matched);
    // A blank region is untrackable and never becomes a reference.
    QVERIFY(!BuildReferencePatch(reference, QRectF(0.05, 0.9, 0.1, 0.04)).valid);
}

void UltraClearTextTests::highRiskTokensFailClosed()
{
    for (const QString& risky : {QStringLiteral("l0O"), QStringLiteral("COVID-19"), QStringLiteral("B2"),
                                 QStringLiteral("x²"), QStringLiteral("∑"), QStringLiteral("1"),
                                 QStringLiteral("I"), QStringLiteral("a^2"), QStringLiteral("√2"),
                                 QStringLiteral("5S"), QString()}) {
        QVERIFY2(IsHighRiskToken(risky), qPrintable(risky));
    }
    for (const QString& safe : {QStringLiteral("Hello"), QStringLiteral("3.14"), QStringLiteral("-12"),
                                QStringLiteral("1,000"), QStringLiteral("page"), QStringLiteral("(see)"),
                                QStringLiteral("42"), QStringLiteral("Grüße")}) {
        QVERIFY2(!IsHighRiskToken(safe), qPrintable(safe));
    }
}

void UltraClearTextTests::modelRequiresTemporalAgreement()
{
    const SyntheticPage page = MakePage(kLines);
    const ViewTransform t = Transform(1.0f, 0.5f, 0.5f);
    OverlayModel model;
    const SourceStamp current = Stamp(3, 4, 0, t);
    QCOMPARE(model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), current, nullptr, 120), AdmitVerdict::kPending);
    QCOMPARE(model.AcceptedWordCount(), 0);
    QVERIFY(model.SceneWords(120, 1).isEmpty());
    QCOMPARE(model.Admit(ResultFor(page, t, Stamp(3, 4, 200, t)), current, nullptr, 220), AdmitVerdict::kAccepted);
    QCOMPARE(model.AcceptedWordCount(), page.words.size());
    // Disagreeing text for one word is not confirmed.
    OverlayModel disagree;
    QStringList altered{QStringLiteral("Reeding")};
    disagree.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), current, nullptr, 120);
    disagree.Admit(ResultFor(page, t, Stamp(3, 4, 200, t), altered), current, nullptr, 220);
    QCOMPARE(disagree.AcceptedWordCount(), page.words.size() - 1);
}

void UltraClearTextTests::modelRejectsStaleSceneAndLateResults()
{
    const SyntheticPage page = MakePage(kLines);
    const ViewTransform t = Transform(1.0f, 0.5f, 0.5f);
    OverlayModel model;
    QCOMPARE(model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), Stamp(4, 4, 0, t), nullptr, 120),
             AdmitVerdict::kStaleScene);
    QCOMPARE(model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), Stamp(3, 5, 0, t), nullptr, 120),
             AdmitVerdict::kStaleScene);
    QCOMPARE(model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), Stamp(3, 4, 0, t), nullptr, 100 + 4001),
             AdmitVerdict::kTooOld);
    RecognitionResult failed = ResultFor(page, t, Stamp(3, 4, 100, t));
    failed.output.ok = false;
    QCOMPARE(model.Admit(failed, Stamp(3, 4, 0, t), nullptr, 120), AdmitVerdict::kFailed);
    QCOMPARE(model.AcceptedWordCount(), 0);
}

void UltraClearTextTests::modelRejectsResultWhoseContentMoved()
{
    const SyntheticPage page = MakePage(kLines);
    const SyntheticPage moved = MakePage(kLines, QPointF(120.0, 70.0));
    const ViewTransform t = Transform(1.0f, 0.5f, 0.5f);
    OverlayModel model;
    const SourceStamp current = Stamp(3, 4, 0, t);
    const ProbeFrame newer = ProbeOf(RenderView(moved.image, t, kViewWidth, kViewHeight), t, 3, 4, 9, 500);
    QCOMPARE(model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), current, &newer, 520),
             AdmitVerdict::kContentMoved);
    QCOMPARE(model.AcceptedWordCount(), 0);
}

void UltraClearTextTests::modelHidesTextOnMotionAndExpiresFreshness()
{
    const SyntheticPage page = MakePage(kLines);
    const ViewTransform t = Transform(1.0f, 0.5f, 0.5f);
    OverlayModel model;
    const SourceStamp current = Stamp(3, 4, 0, t);
    model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), current, nullptr, 110);
    model.Admit(ResultFor(page, t, Stamp(3, 4, 200, t)), current, nullptr, 210);
    QVERIFY(model.HasAcceptedText());
    const std::uint64_t validatedSeq = model.Lines().first().validatedFrameSeq;
    // Fresh enough, or no newer camera frame: shown.
    QVERIFY(!model.SceneWords(400, validatedSeq + 1).isEmpty());
    QVERIFY(!model.SceneWords(5000, validatedSeq).isEmpty());
    // Stale validation with newer frames: hidden.
    QVERIFY(model.SceneWords(200 + 351, validatedSeq + 1).isEmpty());
    // A probe of the same content revalidates.
    const ProbeFrame same = ProbeOf(RenderView(page.image, t, kViewWidth, kViewHeight), t, 3, 4, 20, 700);
    const ValidationSummary ok = model.Validate(same, 700);
    QVERIFY(ok.valid > 0);
    QVERIFY(!model.SceneWords(720, 21).isEmpty());
    // Content moved far: removed, nothing floats.
    const SyntheticPage moved = MakePage(kLines, QPointF(150.0, 90.0));
    const ProbeFrame motion = ProbeOf(RenderView(moved.image, t, kViewWidth, kViewHeight), t, 3, 4, 21, 800);
    const ValidationSummary bad = model.Validate(motion, 800);
    QVERIFY(bad.invalid > 0);
    QVERIFY(model.SceneWords(800, 21).isEmpty());
}

void UltraClearTextTests::modelReprojectsWithCurrentTransform()
{
    const SyntheticPage page = MakePage(kLines);
    const ViewTransform t = Transform(1.0f, 0.5f, 0.5f);
    OverlayModel model;
    const SourceStamp current = Stamp(3, 4, 0, t);
    model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), current, nullptr, 110);
    model.Admit(ResultFor(page, t, Stamp(3, 4, 200, t)), current, nullptr, 210);
    const std::uint64_t seq = model.Lines().first().validatedFrameSeq;
    const ViewTransform zoomed = Transform(2.0f, 0.3f, 0.2f, ViewportFitMode::kFill);
    const QVector<DisplayWord> a = model.DisplayWords(t, kViewWidth, kViewHeight, 220, seq);
    const QVector<DisplayWord> b = model.DisplayWords(zoomed, kViewWidth, kViewHeight, 220, seq);
    QVERIFY(!a.isEmpty());
    QVERIFY(!b.isEmpty());
    const QRectF expected = SceneRectToView(zoomed, kViewWidth, kViewHeight, page.words[0].sceneRect);
    QCOMPARE(b.first().text, page.words[0].text);
    QVERIFY(std::abs(b.first().viewRect.left() - expected.left()) < 0.5);
    QVERIFY(std::abs(b.first().viewRect.height() - expected.height()) < 0.5);
    QVERIFY(std::abs(b.first().viewRect.height() - a.first().viewRect.height() * 2.0) < 1.0);
}

void UltraClearTextTests::modelOmitsHighRiskWordsFromReadingText()
{
    const SyntheticPage page = MakePage({QStringLiteral("Total is 3.14 and l0O ok")});
    const ViewTransform t = Transform(1.0f, 0.5f, 0.5f);
    OverlayModel model;
    const SourceStamp current = Stamp(1, 1, 0, t);
    model.Admit(ResultFor(page, t, Stamp(1, 1, 100, t)), current, nullptr, 110);
    model.Admit(ResultFor(page, t, Stamp(1, 1, 200, t)), current, nullptr, 210);
    const QString text = model.ReadingText();
    QVERIFY2(text.contains(QStringLiteral("3.14")), qPrintable(text));
    QVERIFY2(!text.contains(QStringLiteral("l0O")), qPrintable(text));
    QVERIFY2(text.endsWith(QStringLiteral("…")), qPrintable(text));
}

void UltraClearTextTests::schedulerCoalescesLatestOnly()
{
    auto shared = std::make_shared<HeldRecognizer::Shared>();
    std::mutex resultsMutex;
    QVector<std::uint64_t> completed;
    {
        RecognitionScheduler scheduler(
            std::make_unique<HeldRecognizer>(shared),
            [&](RecognitionResult r) {
                std::lock_guard lock(resultsMutex);
                completed.push_back(r.jobId);
            },
            {});
        QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
        const std::uint64_t first = scheduler.Submit({}, SolidImage(1));
        QTRY_VERIFY(scheduler.InFlight());
        scheduler.Submit({}, SolidImage(2));
        scheduler.Submit({}, SolidImage(3));
        const std::uint64_t last = scheduler.Submit({}, SolidImage(4));
        QCOMPARE(scheduler.Stats().coalesced, std::uint64_t{2});
        QCOMPARE(scheduler.Stats().started, std::uint64_t{1});
        {
            std::lock_guard lock(shared->mutex);
            shared->release = true;
        }
        shared->cv.notify_all();
        QTRY_VERIFY(!scheduler.HasWork());
        QTRY_COMPARE(scheduler.Stats().completed, std::uint64_t{2});
        std::lock_guard lock(resultsMutex);
        QCOMPARE(completed, (QVector<std::uint64_t>{first, last}));
    }
    QCOMPARE(shared->calls, 2);
    QCOMPARE(shared->mutations, 0);
}

void UltraClearTextTests::schedulerCancelRevokesWithoutReleasingInput()
{
    auto shared = std::make_shared<HeldRecognizer::Shared>();
    std::atomic<int> callbacks{0};
    RecognitionScheduler scheduler(std::make_unique<HeldRecognizer>(shared),
                                   [&](RecognitionResult) { ++callbacks; }, {});
    QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
    QImage input = SolidImage(9);
    scheduler.Submit({}, input);
    input.fill(Qt::red); // caller reuses its buffer immediately
    QTRY_VERIFY(scheduler.InFlight());
    const std::uint64_t authority = scheduler.Authority();
    scheduler.Cancel();
    QVERIFY(scheduler.Authority() > authority);
    {
        std::lock_guard lock(shared->mutex);
        shared->release = true;
    }
    shared->cv.notify_all();
    QTRY_COMPARE(scheduler.Stats().revoked, std::uint64_t{1});
    QCOMPARE(callbacks.load(), 0);
    QCOMPARE(shared->mutations, 0); // the worker's copy stayed intact
}

void UltraClearTextTests::schedulerDestructionIsBoundedWhileHeld()
{
    auto shared = std::make_shared<HeldRecognizer::Shared>();
    QElapsedTimer timer;
    {
        auto scheduler = std::make_unique<RecognitionScheduler>(
            std::make_unique<HeldRecognizer>(shared), [](RecognitionResult) {},
            RecognitionScheduler::StatusChanged{});
        QTRY_COMPARE(scheduler->Status().state, BackendState::kReady);
        scheduler->Submit({}, SolidImage(5));
        QTRY_VERIFY(scheduler->InFlight());
        timer.start();
        scheduler.reset();
    }
    QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
    // The detached worker still owns its state; releasing lets it retire.
    {
        std::lock_guard lock(shared->mutex);
        shared->release = true;
    }
    shared->cv.notify_all();
    QTRY_COMPARE(shared->calls, 1);
}

void UltraClearTextTests::slowOcrNeverBlocksPresentationClock()
{
    Harness h;
    h.shared->SetHold(true);
    h.Enable();
    int presentsWhileHeld = 0;
    for (int i = 0; i < 300; ++i) {
        // Continuous pan: a new transform on every presentation.
        h.context.transform = Transform(1.4f, 0.35f + 0.001f * (i % 100), 0.5f, ViewportFitMode::kFill);
        h.Present();
        QCoreApplication::processEvents();
        if (h.controller->SchedulerStatistics().started > 0) {
            ++presentsWhileHeld;
            // The overlay follows the presentation clock, not recognition.
            QCOMPARE(h.overlay->CurrentViewTransform().sourceX, h.context.transform.sourceX);
        }
    }
    QVERIFY2(presentsWhileHeld > 200, qPrintable(QString::number(presentsWhileHeld)));
    QVERIFY2(h.maxPresentMs < 25.0, qPrintable(QString::number(h.maxPresentMs)));
    const SchedulerStats held = h.controller->SchedulerStatistics();
    QCOMPARE(held.started, std::uint64_t{1}); // never more than one in flight
    QVERIFY2(held.submitted <= 2, qPrintable(QString::number(held.submitted)));
    h.shared->SetHold(false);
    QVERIFY(h.RunUntilShown());
    QVERIFY(h.overlay->WordCount() > 0);
}

void UltraClearTextTests::controllerCoalescesChangingSceneWhileOcrHeld()
{
    Harness h;
    h.shared->SetHold(true);
    h.Enable();
    const SyntheticPage a = MakePage(kLines);
    const SyntheticPage b = MakePage({QStringLiteral("Board text changes"), QStringLiteral("while recognition waits")});
    for (int i = 0; i < 400; ++i) {
        // Content alternates in pairs: each pair is briefly steady.
        h.SetPage((i / 16) % 2 ? b : a);
        h.Present();
        QCoreApplication::processEvents();
    }
    const SchedulerStats stats = h.controller->SchedulerStatistics();
    QCOMPARE(stats.started, std::uint64_t{1});
    QVERIFY2(stats.coalesced >= 1, qPrintable(QString::number(stats.coalesced)));
    QVERIFY(h.overlay->WordCount() == 0);
    h.shared->SetHold(false);
    QTRY_VERIFY(h.controller->SchedulerStatistics().started >= 2);
    QVERIFY(h.controller->SchedulerStatistics().started < stats.submitted);
}

void UltraClearTextTests::acceptedTextReprojectsOnPresentationClockWithDpi()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    QCOMPARE(h.panel->readingPane()->toPlainText().split(QLatin1Char('\n')).size(), kLines.size());
    // View-only change: pan/zoom. No recognition is needed to follow it.
    const std::uint64_t startedBefore = h.controller->SchedulerStatistics().started;
    h.context.transform = Transform(2.0f, 0.3f, 0.25f, ViewportFitMode::kFill);
    h.Present(false);
    h.Present(false, 90); // a probe validates the newly visible region
    QCoreApplication::processEvents();
    QVERIFY(h.overlay->WordCount() > 0);
    const QVector<QRectF> rects = h.overlay->ProjectedWordRects();
    QVERIFY(!rects.isEmpty());
    // Overlay logical size is half the viewport's pixels: same normalized
    // mapping, no stale viewport pixels and no double DPI scaling.
    // Every drawn plate sits where its scene word projects now.
    for (const QRectF& r : rects) {
        bool matches = false;
        for (const PageWord& word : h.page.words) {
            const QRectF expected =
                SceneRectToView(h.context.transform, h.overlay->width(), h.overlay->height(), word.sceneRect);
            matches = matches || (std::abs(r.left() - expected.left()) < 2.0 &&
                                  std::abs(r.top() - expected.top()) < 2.0 &&
                                  std::abs(r.height() - expected.height()) < 1.5);
        }
        QVERIFY2(matches, qPrintable(QStringLiteral("%1,%2 %3x%4").arg(r.left()).arg(r.top())
                                         .arg(r.width()).arg(r.height())));
    }
    QVERIFY(h.controller->SchedulerStatistics().started <= startedBefore + 1);
}

void UltraClearTextTests::sourceSwitchAndGeometryChangeInvalidateImmediately()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    // Hold the next (refresh) recognition, then switch source while it is
    // in flight.
    const std::uint64_t startedBefore = h.controller->SchedulerStatistics().started;
    h.shared->SetHold(true);
    for (int i = 0; i < 200 && h.controller->SchedulerStatistics().started == startedBefore; ++i) {
        h.Present(true, 50);
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QVERIFY(h.controller->SchedulerStatistics().started > startedBefore);
    const std::uint64_t generation = h.controller->GeometryGeneration();
    h.context.geometry.sourceEpoch = 8;
    h.Present(true, 1);
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.controller->Model().Lines().isEmpty());
    QVERIFY(h.controller->GeometryGeneration() > generation);
    QVERIFY(h.panel->readingPane()->toPlainText().isEmpty());
    const std::uint64_t rejected = h.controller->RejectedResults();
    const std::uint64_t revoked = h.controller->SchedulerStatistics().revoked;
    h.shared->SetHold(false);
    // The old in-flight result is revoked by authority, never admitted.
    QTRY_VERIFY(h.controller->SchedulerStatistics().revoked > revoked ||
                h.controller->RejectedResults() > rejected);
    QCOMPARE(h.overlay->WordCount(), 0);

    QVERIFY(h.RunUntilShown());
    h.context.geometry.rotationQuarterTurns = 1;
    h.Present(true, 1);
    QCOMPARE(h.overlay->WordCount(), 0);
}

void UltraClearTextTests::contentMotionHidesTextAndStaleProbeExpires()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    // The page moves under the camera beyond the validated search window.
    h.SetPage(MakePage(kLines, QPointF(140.0, 80.0)));
    for (int i = 0; i < 20 && h.overlay->WordCount() > 0; ++i) {
        h.Present(true, 20);
    }
    QCOMPARE(h.overlay->WordCount(), 0);

    // Re-acquire, then starve probes while frames keep arriving.
    QVERIFY(h.RunUntilShown(600));
    h.completeReadbacks = false;
    h.Present(true, 100); // requests a copy that never completes
    for (int i = 0; i < 40 && h.overlay->WordCount() > 0; ++i) {
        h.Present(true, 20);
    }
    QCOMPARE(h.overlay->WordCount(), 0);
}

void UltraClearTextTests::compositeFramesPauseAndShowOriginalPreservesState()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    h.panel->showOriginalCheckbox()->setChecked(true);
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.controller->Model().HasAcceptedText());
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("Original")));
    h.panel->showOriginalCheckbox()->setChecked(false);
    h.Present(false, 1);
    QVERIFY(h.overlay->WordCount() > 0);

    h.context.compositeFrames = true;
    QVERIFY(!h.Present());
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("Temporal Smooth")));
    for (int i = 0; i < 20; ++i) {
        QVERIFY(!h.Present());
    }
}

void UltraClearTextTests::unavailableBackendIsReportedHonestly()
{
    Harness h(BackendState::kUnavailable);
    h.Enable();
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kUnavailable);
    for (int i = 0; i < 30; ++i) {
        h.Present();
    }
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t{0});
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("no OCR language")));
    QVERIFY(h.panel->statusLabel()->text().contains(QStringLiteral("unavailable")));
}

void UltraClearTextTests::originalPixelsAndNonTextGraphicsArePreserved()
{
    Harness h;
    h.Enable();
    // CPU presentation path: the controller must not modify the buffer.
    QImage view = RenderView(h.page.image, h.context.transform, kViewWidth, kViewHeight);
    {
        std::lock_guard lock(h.shared->mutex);
        h.shared->transforms.insert(Checksum(view), h.context.transform);
    }
    const quint64 before = Checksum(view);
    for (int i = 0; i < 200 && h.overlay->WordCount() == 0; ++i) {
        h.now += 100;
        h.context.frameSeq = ++h.frameSeq;
        if (h.controller->OnPresented(h.context)) {
            h.controller->OnCpuViewport(h.context, view.constBits(), view.width(), view.height(),
                                        static_cast<int>(view.bytesPerLine()));
        }
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QVERIFY(h.overlay->WordCount() > 0);
    QCOMPARE(Checksum(view), before);
    // No glyph plate may cover the curve.
    const QRectF chart = SceneRectToView(h.context.transform, h.overlay->width(), h.overlay->height(),
                                         h.page.chartRegion);
    for (const QRectF& r : h.overlay->ProjectedWordRects()) {
        QVERIFY(!r.intersects(chart));
    }
    QCOMPARE(h.shared->inputMutations, 0);
}

void UltraClearTextTests::presentationWatchdogHidesTextWithoutPresents()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    qInfo("max OnPresented %.3f ms, max probe handling %.3f ms (960x540)", h.maxPresentMs, h.maxProbeMs);
    // Presentations stop (camera stopped or a placeholder is shown).
    h.now += 700;
    QTRY_COMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.controller->Model().HasAcceptedText());
    // The next presentation of the unchanged scene restores validated text.
    h.Present(false, 1);
    QVERIFY(h.overlay->WordCount() > 0);
}

void UltraClearTextTests::probeWorkIsBoundedForLargeViewports()
{
    Harness h;
    h.Enable();
    const SyntheticPage page = MakePage(kLines, {}, 3840, 2160, 140);
    h.context.geometry.sceneWidth = 3840;
    h.context.geometry.sceneHeight = 2160;
    h.context.transform =
        ComputeViewTransform(3840, 2160, 3840, 2160, 1.0f, 0.5f, 0.5f, ViewportFitMode::kFit);
    h.context.viewportWidth = 3840;
    h.context.viewportHeight = 2160;
    const QImage view = RenderView(page.image, h.context.transform, 3840, 2160);
    double worst = 0.0;
    for (int i = 0; i < 12; ++i) {
        h.now += 100;
        h.context.frameSeq = ++h.frameSeq;
        if (!h.controller->OnPresented(h.context)) {
            continue;
        }
        QElapsedTimer timer;
        timer.start();
        h.controller->OnCpuViewport(h.context, view.constBits(), view.width(), view.height(),
                                    static_cast<int>(view.bytesPerLine()));
        worst = std::max(worst, timer.nsecsElapsed() / 1e6);
        QCoreApplication::processEvents();
    }
    qInfo("worst 4K probe handling incl. one owned submit copy: %.2f ms", worst);
    QVERIFY2(worst < 80.0, qPrintable(QString::number(worst)));
}

void UltraClearTextTests::panelExposesAccessibleControls()
{
    UltraClearTextPanel panel;
    QVERIFY(!panel.enableCheckbox()->isChecked()); // conservative default
    QVERIFY(!panel.enableCheckbox()->accessibleName().isEmpty());
    QVERIFY(!panel.showOriginalCheckbox()->accessibleName().isEmpty());
    QVERIFY(!panel.showOriginalCheckbox()->isEnabled());
    QVERIFY(panel.readingPane()->isReadOnly());
    QVERIFY(panel.readingPane()->textInteractionFlags() & Qt::TextSelectableByKeyboard);
    QCOMPARE(panel.readingPane()->accessibleName(), QStringLiteral("Recognized text"));
    panel.enableCheckbox()->setChecked(true);
    QVERIFY(panel.showOriginalCheckbox()->isEnabled());
    panel.SetReadingText(QStringLiteral("one\ntwo"));
    QCOMPARE(panel.readingPane()->toPlainText(), QStringLiteral("one\ntwo"));
}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    UltraClearTextTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ultra_clear_text_tests.moc"
