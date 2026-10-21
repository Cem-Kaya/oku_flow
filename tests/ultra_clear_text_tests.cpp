// Desktop-free Ultra Clear Text contract fixture. Runs under Qt offscreen:
// the overlay and panel are real production widgets but create no native
// windows. Recognition uses scripted fakes; the real Windows OCR backend has
// its own optional acceptance fixture.

#include "okuflow/app/ultra_clear_text_controller.hpp"
#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include "ultra_clear_text_test_support.hpp"
#include "uct_capture_test_cases.hpp"
#include "uct_color_test_support.hpp"
#include "uct_quad_reference_test_support.hpp"

#include <QApplication>
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QWheelEvent>
#include <QToolButton>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QHash>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSet>
#include <QTextCursor>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

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
    QVector<quint64> inputChecksums;
    int inputMutations{};
    BackendState initState{BackendState::kReady};
    QHash<quint64, ViewTransform> transforms; // image checksum -> transform
    QVector<PageWord> words;                  // scene ground truth
    bool fail{};
    bool paddedBounds{};
    double inkSearchPadding{};

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
        shared_->inputChecksums.push_back(before);
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
            row->words.push_back({word.text, px, shared_->paddedBounds,
                shared_->inkSearchPadding>0 ? px.adjusted(-shared_->inkSearchPadding,0,shared_->inkSearchPadding,0) : QRectF{}});
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
    // Emulates the presenter's reusable readback vector.
    std::vector<std::uint8_t> readback;
    QVector<double> presentMs;
    QVector<double> readbackMs;

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
        context.geometry.settings.sourceEpoch = 7;
        context.geometry.sceneWidth = kSceneWidth;
        context.geometry.sceneHeight = kSceneHeight;
        context.requested = context.geometry.settings;
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

    // A newly processed scene with these settings, matching the request.
    void SetSceneSettings(const UctSceneSettings& settings)
    {
        context.geometry.settings = settings;
        context.requested = settings;
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
        controller->OnPresentationSucceeded();
        presentMs.push_back(timer.nsecsElapsed() / 1e6);
        maxPresentMs = std::max(maxPresentMs, presentMs.last());
        if (!wanted) {
            return false;
        }
        const std::uint64_t id = nextReadback++;
        controller->OnReadbackRequested(id, context);
        if (completeReadbacks) {
            const int w = static_cast<int>(context.viewportWidth);
            const int hgt = static_cast<int>(context.viewportHeight);
            const QImage view = IndependentFramePixels(
                RenderView(page.image, context.transform, w, hgt), context.frameSeq);
            {
                std::lock_guard lock(shared->mutex);
                shared->transforms.insert(Checksum(view), context.transform);
            }
            // The presenter's own mapped copy into its reusable vector.
            readback.resize(static_cast<std::size_t>(w) * hgt * 4);
            std::memcpy(readback.data(), view.constBits(), readback.size());
            QElapsedTimer probeTimer;
            probeTimer.start();
            controller->OnReadbackCompleted(id, readback, w, hgt, w * 4);
            readbackMs.push_back(probeTimer.nsecsElapsed() / 1e6);
            maxProbeMs = std::max(maxProbeMs, readbackMs.last());
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
    RecognitionResult result;
    result.stamp = stamp;
    if (result.stamp.frameSeq == 0)
        result.stamp.frameSeq = static_cast<std::uint64_t>(std::max<std::int64_t>(0, stamp.timestampMs));
    const QImage view = IndependentFramePixels(RenderView(page.image, t, kViewWidth, kViewHeight),
                                               result.stamp.frameSeq);
    result.sourcePixelDigest = SourcePixelsDigest(view);
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

// Dense printed page: many long rows of small print, as on a textbook page.
QStringList DenseLines(int rows)
{
    static const QStringList kWords{
        QStringLiteral("reading"), QStringLiteral("page"),    QStringLiteral("camera"),
        QStringLiteral("glasses"), QStringLiteral("people"),  QStringLiteral("printed"),
        QStringLiteral("morning"), QStringLiteral("letters"), QStringLiteral("window"),
        QStringLiteral("teacher"), QStringLiteral("board"),   QStringLiteral("notes"),
        QStringLiteral("market"),  QStringLiteral("river"),   QStringLiteral("garden"),
        QStringLiteral("evening"), QStringLiteral("travel"),  QStringLiteral("number")};
    QStringList lines;
    for (int r = 0; r < rows; ++r) {
        QStringList words;
        for (int w = 0; w < 30; ++w) {
            words.push_back(kWords[(r * 7 + w * 5 + (w * w) % 11) % kWords.size()]);
        }
        lines.push_back(words.join(QLatin1Char(' ')));
    }
    return lines;
}

// Generalized ResultFor for any viewport size.
RecognitionResult ResultForView(const SyntheticPage& page, const ViewTransform& t, const SourceStamp& stamp,
                                int width, int height)
{
    RecognitionResult result;
    result.stamp = stamp;
    if (result.stamp.frameSeq == 0)
        result.stamp.frameSeq = static_cast<std::uint64_t>(std::max<std::int64_t>(0, stamp.timestampMs));
    const QImage view = IndependentFramePixels(RenderView(page.image, t, width, height), result.stamp.frameSeq);
    result.sourcePixelDigest = SourcePixelsDigest(view);
    result.stamp.viewTransform = t;
    result.stamp.viewportWidth = static_cast<std::uint32_t>(width);
    result.stamp.viewportHeight = static_cast<std::uint32_t>(height);
    result.output.ok = true;
    for (const PageWord& word : page.words) {
        const QRectF px = SceneRectToView(t, width, height, word.sceneRect);
        if (!QRectF(0, 0, width, height).contains(px)) {
            continue;
        }
        if (result.output.lines.isEmpty() ||
            std::abs(result.output.lines.last().words.first().boundsPx.top() - px.top()) > px.height() * 0.5) {
            result.output.lines.push_back({});
            result.lineColors.push_back({});
        }
        result.output.lines.last().words.push_back({word.text, px});
    }
    result.reference = MakeProbeFrame(view.constBits(), view.width(), view.height(),
                                      static_cast<int>(view.bytesPerLine()), 4, result.stamp);
    return result;
}

double Percentile(QVector<double> values, double p)
{
    if (values.isEmpty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const int index = std::clamp(static_cast<int>(std::ceil(p * values.size())) - 1, 0,
                                 static_cast<int>(values.size()) - 1);
    return values[index];
}

struct DeviceJobState {
    std::atomic<int> readbacks{},probes{},gpuCalls{},cpuCalls{};
    std::atomic<int> liveImages{};
    std::atomic<int> probeDelayMs{};
    std::atomic<bool> probeFails{};
    std::atomic<int> preferences{};
    bool flipPreference{};
    std::atomic<bool> started{},release{},wrongOrder{};
};
class TestDeviceImage final : public DeviceImage {
public:
    explicit TestDeviceImage(std::shared_ptr<DeviceJobState> state):state_(std::move(state)) {
        ++state_->liveImages;
        width=128;height=64;pitch=width*4;sourceEpoch=1;frameSequence=1;
    }
    ~TestDeviceImage() override{--state_->liveImages;}
    const void* Data() const override{return this;}
    bool WaitReady(void*,QString&) const override{return true;}
    QImage Readback(const std::atomic<bool>&,QString&) const override {
        if(!state_->release.load())state_->wrongOrder=true;
        ++state_->readbacks;QImage page(width,height,QImage::Format_ARGB32);page.fill(Qt::white);return page;
    }
    QImage ReadProbe(const std::atomic<bool>&,QString& error) const override {
        ++state_->probes;
        std::this_thread::sleep_for(std::chrono::milliseconds(state_->probeDelayMs.load()));
        if (state_->probeFails.load()) { error="fixture copy failure"; return {}; }
        QImage page(width,height,QImage::Format_ARGB32);page.fill(Qt::white);return page;
    }
private:std::shared_ptr<DeviceJobState> state_;
};
class DeviceRecognizer final : public Recognizer {
public:
    explicit DeviceRecognizer(std::shared_ptr<DeviceJobState> state):state_(std::move(state)){}
    BackendStatus Initialize() override{return {BackendState::kReady,"device fake","",""};}
    bool WantsDeviceInput() const override {
        const int query=++state_->preferences;
        return !state_->flipPreference || query==1;
    }
    RecognitionOutput Recognize(const QImage&,const std::atomic<bool>&) override{++state_->cpuCalls;return {};}
    RecognitionOutput RecognizeDevice(const DeviceImage&,const std::atomic<bool>&) override {
        ++state_->gpuCalls;state_->started=true;
        while(!state_->release.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
        RecognitionOutput output;output.ok=true;output.lines={{{{"TEST",QRectF(8,8,40,16)}}}};return output;
    }
private:std::shared_ptr<DeviceJobState> state_;
};
struct ReleaseDeviceJob {std::shared_ptr<DeviceJobState> state;~ReleaseDeviceJob(){state->release=true;}};
class SceneDeviceImage final : public DeviceImage {
public:
    explicit SceneDeviceImage(QImage page):page_(std::move(page)){width=page_.width();height=page_.height();pitch=page_.bytesPerLine();sourceEpoch=7;}
    const void* Data() const override{return this;}
    bool WaitReady(void*,QString&) const override{return true;}
    QImage Readback(const std::atomic<bool>&,QString&) const override{return page_;}
    QImage ReadProbe(const std::atomic<bool>&,QString&) const override{return page_.scaled(width/2,height/2,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);}
    bool HasHoldProvenance() const override { return hold.has_value(); }
    HoldProvenance ReadHoldProvenance(const std::atomic<bool>&, QString&) const override
    { return hold.value_or(HoldProvenance{HoldDisplay::Unknown, 0, 0.0f}); }
    std::optional<HoldProvenance> hold;
private:QImage page_;
};

// Hardware-free scheduler protocol oracle. Its fingerprint is explicitly fake:
// this verifies selection of the owner's canonical analysis path, not the CUDA
// tree-hash implementation or recognition quality.
struct ResidentBridgeState {
    std::atomic<bool> preferred{}, fail{}, holdAnalysis{}, analysisStarted{}, releaseAnalysis{};
    std::atomic<int> readbacks{}, validationReads{}, probeReads{}, preparations{}, cpuCalls{}, deviceCalls{};
    std::mutex mutex;
    std::condition_variable wake;
    void Release() { releaseAnalysis = true; wake.notify_all(); }
};
class CanonicalResidentProbe final : public ResidentProbe {
public:
    int Width() const noexcept override { return 128; }
    int Height() const noexcept override { return 64; }
    ReferencePatch Reference(const SourceStamp&, const QRectF& rect) const override {
        ReferencePatch patch; patch.sceneRect = rect; patch.valid = !rect.isEmpty();
        patch.resident = std::make_shared<ResidentPatch>(); return patch;
    }
    PatchMatch Match(const SourceStamp&, const ReferencePatch& patch, const MatchTuning&,
                     std::optional<QPointF>) const override {
        return {patch.valid && bool(patch.resident), true, 1, {}};
    }
    std::optional<double> Similarity(const SourceStamp&, const ResidentProbe&, const SourceStamp&) const override { return 1; }
};
class CanonicalResidentImage final : public DeviceImage {
public:
    explicit CanonicalResidentImage(std::shared_ptr<ResidentBridgeState> state)
        : state_(std::move(state)), page_(128, 64, QImage::Format_ARGB32),
          probe_(std::make_shared<CanonicalResidentProbe>()) {
        page_.fill(QColor(237, 219, 181));
        width = 128; height = 64; pitch = width * 4; sourceEpoch = 7; frameSequence = 55;
        kind = AnalysisKind::TemporallySmoothed;
        historyEpoch = 1; seedFrameSequence = 54; blendFrameCount = 2; temporalAlpha = .25f;
        canonical_ = QCryptographicHash::hash(QByteArray("resident-protocol-oracle:") +
            QByteArray(reinterpret_cast<const char*>(page_.constBits()), page_.sizeInBytes()), QCryptographicHash::Sha256);
    }
    const void* Data() const override { return this; }
    bool WaitReady(void*, QString&) const override { return true; }
    bool HasResidentAnalysis() const override { return true; }
    const QByteArray& CanonicalDigest() const { return canonical_; }
    const QImage& Pixels() const { return page_; }
    QImage Readback(const std::atomic<bool>& cancelled, QString&) const override {
        ++state_->readbacks; return cancelled.load() ? QImage{} : page_;
    }
    QImage ReadValidation(const std::atomic<bool>&, QString& error) const override {
        ++state_->validationReads; error = "unexpected CPU validation fallback"; return {};
    }
    QImage ReadProbe(const std::atomic<bool>&, QString& error) const override {
        ++state_->probeReads; error = "unexpected CPU probe fallback"; return {};
    }
    bool PrepareResidentAnalysis(RecognitionResult& result, int, bool probeOnly,
                                  const std::atomic<bool>& cancelled, QString& error) const override {
        ++state_->preparations; state_->analysisStarted = true;
        if (state_->holdAnalysis.load()) {
            std::unique_lock lock(state_->mutex);
            state_->wake.wait(lock, [&] { return state_->releaseAnalysis.load(); });
        }
        if (cancelled.load() || state_->fail.load()) { error = "resident fixture refusal"; return false; }
        result.reference = {}; result.reference.stamp = result.stamp; result.reference.resident = probe_;
        if (!probeOnly) {
            result.sourcePixelDigestKind = RecognitionResult::DigestKind::GpuTreeSha256V1;
            result.sourcePixelDigest = canonical_;
            const LineColors colors{QColor(11, 29, 47), QColor(231, 197, 103)};
            result.lineColors.fill(colors, result.output.lines.size());
            for (auto& line : result.output.lines) for (auto& word : line.words) word.colors = colors;
            result.currentInputMatches.fill(true, result.output.lines.size());
        }
        return true;
    }
private:
    std::shared_ptr<ResidentBridgeState> state_;
    QImage page_;
    std::shared_ptr<const CanonicalResidentProbe> probe_;
    QByteArray canonical_;
};
class ResidentBridgeRecognizer final : public Recognizer {
public:
    explicit ResidentBridgeRecognizer(std::shared_ptr<ResidentBridgeState> state) : state_(std::move(state)) {}
    BackendStatus Initialize() override { return {BackendState::kReady, "bridge fixture", "", ""}; }
    bool WantsDeviceInput() const override { return state_->preferred.load(); }
    RecognitionOutput Recognize(const QImage&, const std::atomic<bool>& cancelled) override {
        ++state_->cpuCalls; return Output("Bridge", cancelled);
    }
    RecognitionOutput RecognizeDevice(const DeviceImage&, const std::atomic<bool>& cancelled) override {
        ++state_->deviceCalls; return Output("Preferred", cancelled);
    }
private:
    RecognitionOutput Output(const char* text, const std::atomic<bool>& cancelled) const {
        RecognitionOutput output; output.ok = !cancelled.load(); output.cancelled = cancelled.load();
        output.backendPreparing = !state_->preferred.load();
        output.lines = {{{{QString::fromLatin1(text), QRectF(8, 8, 80, 24)}}}}; return output;
    }
    std::shared_ptr<ResidentBridgeState> state_;
};

SourceStamp ResidentBridgeStamp()
{
    SourceStamp stamp; stamp.sourceEpoch = 7; stamp.geometryGeneration = 3; stamp.frameSeq = 55;
    stamp.timestampMs = 55'000; stamp.viewportWidth = 128; stamp.viewportHeight = 64;
    stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
    stamp.temporallySmoothed = true; stamp.historyEpoch = 1; stamp.seedFrameSeq = 54;
    stamp.blendFrameCount = 2; stamp.temporalAlpha = .25f;
    return stamp;
}

} // namespace

class UltraClearTextTests final : public QObject {
    Q_OBJECT

private slots:
    void quadReferenceNormalizesRotationWithoutAcceptingChangedInk()
    {
        using namespace uct_quad_reference_test;
        const auto source=Render(.48), target=Render(.57), changed=Render(.57,true);
        for(const int decimation : {1,2,4}) {
            const auto original=Probe(source,1,decimation), neighbor=Probe(target,2,decimation);
            const auto patch=BuildQuadReferencePatch(original,source.scene);
            QVERIFY(patch.valid); QVERIFY(patch.sceneQuad.valid);
            const MatchTuning strict{.94,.95,2};
            const auto moved=Reproject(patch,neighbor,target.scene);
            const auto match=MatchPatch(moved,neighbor,strict);
            QVERIFY2(match.matched,qPrintable(QString("decimation=%1 score=%2").arg(decimation).arg(match.score,0,'g',14)));
            QVERIFY(!MatchPatch(moved,Probe(changed,3,decimation),strict).matched);
            auto invalid=source.scene; std::swap(invalid.points[1],invalid.points[2]);
            QVERIFY(!BuildQuadReferencePatch(original,invalid).valid);
            invalid=source.scene; invalid.points[0].setX(std::numeric_limits<double>::quiet_NaN());
            QVERIFY(!BuildQuadReferencePatch(original,invalid).valid);
            QVERIFY(!BuildQuadReferencePatch(original,TextQuad::FromRect({-.1,-.1,.2,.2})).valid);
            auto malformed=moved; malformed.sceneQuad=invalid;
            QVERIFY(!MatchPatch(malformed,neighbor,strict).matched);
        }
    }
    void immutableWordProofUsesDetectedRotationAndKeepsSourceIdentity()
    {
        using namespace uct_quad_reference_test;
        const auto source=Render(.48), target=Render(.57);
        struct Backend final : Recognizer {
            RecognizedWord word;
            BackendStatus Initialize() override { return {BackendState::kReady,"quad-proof","",""}; }
            RecognitionOutput Recognize(const QImage&,const std::atomic<bool>&) override {
                RecognitionOutput output; output.ok=true; output.lines={{{word}}}; return output;
            }
        };
        for(const int variant : {0,1,2,3}) {
            auto backend=std::make_unique<Backend>();
            backend->word.text=variant==3 ? QStringLiteral("0") : QStringLiteral("Reading");
            backend->word.boundsPx=target.pixels.Bounds(); backend->word.quadPx=target.pixels;
            auto word=backend->word; word.boundsPx=source.pixels.Bounds(); word.quadPx=source.pixels;
            RecognitionResult selected; selected.stamp=uct_quad_reference_test::Stamp(1);
            selected.reference=Probe(source,1,2); selected.output.ok=true; selected.output.lines={{{word}}};
            CorroborationRequest proof; proof.selectedJobId=91; proof.selectedReference=selected.reference;
            proof.selectedLines=PrepareLines(selected,7);
            RecognitionResult measured; std::atomic<bool> received{false};
            RecognitionScheduler scheduler(std::move(backend),[&](RecognitionResult result) {
                measured=std::move(result); received.store(true);
            },{});
            auto stamp=uct_quad_reference_test::Stamp(variant==1 ? 1 : 2);
            if(variant==2) ++stamp.sourceEpoch;
            QVERIFY(scheduler.Submit(stamp,target.image,proof));
            QTRY_VERIFY(received.load()); QVERIFY(measured.output.ok);
            if(variant==1 || variant==2) QVERIFY(measured.corroborationWordMatches.isEmpty());
            else {
                QCOMPARE(measured.corroborationWordMatches.size(),1);
                QCOMPARE(measured.corroborationWordMatches[0],QVector<bool>({variant==0}));
            }
        }
    }
    void frozenExportMatchesGlyphsAndSurvivesLiveChanges()
    { uct_capture_test::FrozenExportMatchesGlyphsAndSurvivesLiveChanges(); }
    void exportProtectsOmissionsAndKeepsInkAboveText()
    { uct_capture_test::ExportProtectsOmissionsAndKeepsInkAboveText(); }
    void initTestCase() { RegisterFixtureFonts(); }
    void independentModelSelectorsKeepActivationSessionOnly() {
        UltraClearTextPanel panel;
        const auto defaults=panel.ModelSettings();
        QCOMPARE(defaults.gpuDetector,ModelTier::Medium);QCOMPARE(defaults.gpuRecognizer,ModelTier::Medium);
        QCOMPARE(DefaultTensorRtBatchCapacity(ModelTier::Small),8);
        QCOMPARE(DefaultTensorRtBatchCapacity(ModelTier::Medium),8);
        QCOMPARE(DefaultTensorRtBatchCapacity(ModelTier::Tiny),1);
        QCOMPARE(defaults.cpuDetector,ModelTier::Tiny);QCOMPARE(defaults.cpuRecognizer,ModelTier::Small);
        QSignalSpy changes(&panel,&UltraClearTextPanel::modelSettingsChanged);
        panel.cpuDetectorCombo()->setCurrentIndex(1);
        QCOMPARE(changes.size(),1);QCOMPARE(panel.ModelSettings().cpuDetector,ModelTier::Small);
        QCOMPARE(panel.ModelSettings().gpuDetector,ModelTier::Medium);QVERIFY(!panel.IsEnabledByUser());
        panel.SetModelSettings(defaults);QCOMPARE(changes.size(),1);QVERIFY(panel.ModelSettings()==defaults);
        auto* advanced=panel.findChild<QToolButton*>("uctAdvancedSettings");QVERIFY(advanced);
        QVERIFY(advanced->isCheckable());QVERIFY(!advanced->isChecked());
        QVERIFY(!panel.gpuDetectorCombo()->accessibleName().isEmpty());QCOMPARE(panel.gpuRecognizerCombo()->count(),3);
    }
    void modelSwitchRetiresHeldWorkerWithoutBlockingOrDisablingUct() {
        Harness h;
        h.Enable();QVERIFY(h.RunUntilShown());
        h.shared->SetHold(true);
        const auto release=qScopeGuard([&]{h.shared->SetHold(false);});
        int callsBefore;
        {std::lock_guard lock(h.shared->mutex);callsBefore=h.shared->calls;}
        h.SetPage(MakePage({"Changed page needs recognition"}));
        QTRY_VERIFY(([&]{h.Run(1);std::lock_guard lock(h.shared->mutex);return h.shared->calls>callsBefore;})());
        h.controller->SetShowOriginal(true);
        auto replacement=std::make_shared<ScriptShared>();
        replacement->initState=BackendState::kUnavailable;
        QElapsedTimer timer;timer.start();
        h.controller->ReplaceRecognizerFactory([replacement]{return std::make_unique<ScriptedRecognizer>(replacement);});
        QVERIFY2(timer.elapsed()<1000,"Model switch joined a held inference worker on the UI thread");
        QVERIFY(h.panel->IsEnabledByUser());QVERIFY(h.panel->IsShowingOriginal());
        QTRY_COMPARE(h.controller->BackendStatusSnapshot().state,BackendState::kUnavailable);
        h.shared->SetHold(false);
        h.Run(20);QCoreApplication::processEvents();
        QCOMPARE(h.overlay->WordCount(),0);
        QVERIFY(h.panel->readingPane()->toPlainText().isEmpty());
        QCOMPARE(h.controller->BackendStatusSnapshot().state,BackendState::kUnavailable);
    }
    void modelSelectorsIgnoreWheelAndKeepDeliberateKeyboardSelection() {
        UltraClearTextPanel panel;
        auto* advanced=panel.findChild<QToolButton*>("uctAdvancedSettings");QVERIFY(advanced);
        advanced->setChecked(true);panel.show();
        const QVector<QComboBox*> selectors{panel.gpuDetectorCombo(),panel.gpuRecognizerCombo(),
                                           panel.cpuDetectorCombo(),panel.cpuRecognizerCombo()};
        for(auto* combo:selectors) combo->setCurrentIndex(1);
        const auto initial=panel.ModelSettings();
        QSignalSpy changes(&panel,&UltraClearTextPanel::modelSettingsChanged);
        for(auto* combo:selectors) {
            for(const bool focused:{false,true}) {
                if(focused) combo->setFocus();else advanced->setFocus();
                for(const int direction:{-120,120}) {
                    const QPointF point(combo->rect().center());
                    QWheelEvent wheel(point,combo->mapToGlobal(point.toPoint()),QPoint(),QPoint(0,direction),
                                      Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
                    wheel.setAccepted(true);QApplication::sendEvent(combo,&wheel);
                    QVERIFY(!wheel.isAccepted());QCOMPARE(combo->currentIndex(),1);
                }
                const QPointF point(combo->rect().center());
                QWheelEvent trackpad(point,combo->mapToGlobal(point.toPoint()),QPoint(0,24),QPoint(),
                                     Qt::NoButton,Qt::NoModifier,Qt::ScrollUpdate,false);
                trackpad.setAccepted(true);QApplication::sendEvent(combo,&trackpad);
                QVERIFY(!trackpad.isAccepted());QCOMPARE(combo->currentIndex(),1);
            }
        }
        QCOMPARE(changes.size(),0);QVERIFY(panel.ModelSettings()==initial);
        for(auto* combo:selectors) {
            combo->setFocus();QTest::keyClick(combo,Qt::Key_Down);
            QCOMPARE(combo->currentIndex(),2);
        }
        QCOMPARE(changes.size(),4);QVERIFY(!panel.IsEnabledByUser());
        const auto changed=panel.ModelSettings();
        QCOMPARE(changed.cpuDetector,ModelTier::Medium);QCOMPARE(changed.cpuRecognizer,ModelTier::Medium);
        panel.SetModelSettings(initial);QCOMPARE(changes.size(),4);QVERIFY(panel.ModelSettings()==initial);
    }

    // --- Geometry -----------------------------------------------------------
    void geometryRoundTripsThroughFitFillZoomPanAndDpi();
    void processedSceneProvenanceSurvivesDeferredFrames();
    void sourceRetirementRevokesWithoutAnotherPresentation();
    void mixedHeightReadingOrderIsStableAndOnlyFreshTextIsRead();
    void unchangedWordsRefreshMeasuredColors();
    void glyphsShareLineBaselineDespiteDifferentInkHeights();
    void fractionalProtectionCutsKeepProvedWords_data();
    void fractionalProtectionCutsKeepProvedWords();
    void paddedRecognitionFitsVisibleInk_data();
    void paddedRecognitionFitsVisibleInk();
    void overlayKeepsSourceBackgroundInsteadOfBlackWhite_data();
    void overlayKeepsSourceBackgroundInsteadOfBlackWhite();
    void deferredPresentationKeepsCommittedProjectionAndExpires();
    void continuousPanDoesNotHideOverlayWhileOcrHeld();
    void overlayStaysHiddenWhileOwnerBlockedOrApplicationInactive();
    void overlayStaysHiddenAcrossNestedModalDialogs();

    // --- Validation primitives ----------------------------------------------
    void patchMatchesUnchangedTextAcrossPanAndZoom();
    void patchTracksSmallMotionAndRejectsLargeMotion();
    void patchRejectsChangedContent();
    void patchKeepsDenseZeroWhenSparseDistractorWins_data();
    void patchKeepsDenseZeroWhenSparseDistractorWins();
    void partialRecognitionDoesNotDuplicateAfterPanBack();
    void residentBatchesRespectBudgetAndPreserveCompletedChecks();
    void budgetRefreshKeepsExactlyRevalidatedText();
    void missedProbeBudgetKeepsOnlyUnexpiredProof();
    void refreshRetainsLineOnceAndStabilizesColors();
    void refreshKeepsProvenGeometryThroughOmissionBoxJitter();
    void refreshKeepsIndependentlyValidatedOmissionsDuringRegionalScroll();
    void probeReferencesPrioritizeUnverifiedAndOldestLines();
    void budgetRefreshCannotBorrowOtherProof_data();
    void budgetRefreshCannotBorrowOtherProof();
    void schedulerKeepsLocalSurfaceColors_data();
    void schedulerKeepsLocalSurfaceColors();
    void immutableRefinementRefreshesOnlyProvenMatchingColors_data();
    void immutableRefinementRefreshesOnlyProvenMatchingColors();
    void schedulerPreservesDenseTextPolarity_data();
    void schedulerPreservesDenseTextPolarity();
    void cpuBridgeKeepsCanonicalResidentAnalysisAcrossPromotion();
    void residentAnalysisRefusalNeverFallsBackToCpu_data();
    void residentAnalysisRefusalNeverFallsBackToCpu();
    void residentProbePreparationRemainsSinglePass();
    void highRiskTokensFailClosed();

    // --- Model --------------------------------------------------------------
    void modelRequiresTemporalAgreement();
    void livePendingEvidenceRejectsDuplicatePixels();
    void liveCaptureAcceptsIdenticalPixelsOnlyFromNewAcquisitions();
    void heldFramesReadOnceAndCrossfadesRevoke();
    void liveDuplicateRefreshPreservesEstablishedWords();
    void pendingEvidenceCannotCrossResetOrAdmissionMode();
    void modelRejectsStaleSceneAndLateResults();
    void modelRejectsResultWhoseContentMoved();
    void modelAdmissionBudgetNeverDisplaysUncheckedText();
    void modelHidesTextOnMotionAndExpiresFreshness();
    void modelReprojectsWithCurrentTransform();
    void modelOmitsHighRiskWordsFromReadingText();
    void readingTextPreservesOmissionPositions_data();
    void readingTextPreservesOmissionPositions();
    void rejectedTinyAndDenseSpansKeepOrderedOmissions();
    void untrackableTextStaysReadableWithoutReplacement();
    void rejectedRefreshCannotRestoreOldAcceptedText();
    void immutableEvidenceRequiresDistinctPixelsAndSafeRegistration();
    void immutableCompletedRegistrationIgnoresUiBudget_data();
    void immutableCompletedRegistrationIgnoresUiBudget();
    void immutableRefinementRetainsExactProvenCoverage_data();
    void immutableRefinementRetainsExactProvenCoverage();
    void immutableRefinementAddsOwnProofWithoutDuplicateReading();
    void immutableRefinementCannotCarryProofAcrossIdentity_data();
    void immutableRefinementCannotCarryProofAcrossIdentity();
    void immutableRefinementContradictionRevokesAffectedProof();
    void immutableWordConflictKeepsSafeSibling_data();
    void immutableWordConflictKeepsSafeSibling();
    void immutableConflictBlocksFollowExactSelectedIdentity_data();
    void immutableConflictBlocksFollowExactSelectedIdentity();
    void immutableRefinementUnionIsBounded();
    void immutableRefinementLiteralGroupingKeepsPriorProof_data();
    void immutableRefinementLiteralGroupingKeepsPriorProof();
    void immutableRefinementKeepsMeaningfulWhitespaceDistinct();
    void immutableRefinementBoundaryWordUsesIncomingProof_data();
    void immutableRefinementBoundaryWordUsesIncomingProof();
    void immutableRefinementAdmitsAdjacentWordWithSmallOverlap();
    void protectedAdjacentInkSurvivesCompositing_data();
    void protectedAdjacentInkSurvivesCompositing();

    // --- Scheduler ----------------------------------------------------------
    void schedulerCoalescesLatestOnly();
    void schedulerCancelRevokesWithoutReleasingInput();
    void schedulerDestructionIsBoundedWhileHeld();
    void schedulerPixelEvidenceIgnoresAllocationPadding();
    void deviceRecognitionPrecedesReadbackAndDoesNotBlockUi();
    void cancelledDeviceJobRetainsInputAndRejectsLateOutput();
    void smoothedProbeFallbackDownloadsOnlyTheCurrentProbe();
    void deviceProbesContinueAt16xWhileInferenceIsHeld();
    void staticDeviceSceneDoesNotContinuouslyRunOcr();
    void heldAcquisitionRetriesRejectedOcr_data();
    void heldAcquisitionRetriesRejectedOcr();
    void slowDeviceProbesStillSubmitOcrAndInputHealthRecovers();

    // --- Controller (production widgets, scripted OCR) ----------------------
    void slowOcrNeverBlocksPresentationClock();
    void cachedGlyphsRepaintWhileRecognitionIsHeld();
    void neuralPublicationRevokesHeldRecognition();
    void controllerCoalescesChangingSceneWhileOcrHeld();
    void acceptedTextReprojectsOnPresentationClockWithDpi();
    void sourceSwitchAndGeometryChangeInvalidateImmediately();
    void contentMotionHidesTextAndStaleProbeExpires();
    void compositeFramesPauseAndShowOriginalPreservesState();
    void smoothedResultsNeedCurrentFrameProbeProof();
    void unavailableBackendIsReportedHonestly();
    void explicitEnableRetriesBackendInitialization_data();
    void explicitEnableRetriesBackendInitialization();
    void originalPixelsAndNonTextGraphicsArePreserved();
    void presentationWatchdogHidesTextWithoutPresents();
    void watchdogCannotRestoreTextWithoutSuccessfulPresentation();
    void failedPresentationHidesGlyphsAndReadingPane();
    void probeWorkIsBoundedForLargeViewports();
    void denseMovingPageValidationCostAt4K();
    void panelExposesAccessibleControls();
    void panelStatusUpdatesCachedAccessibleTextImmediately();
    void glyphAndPlateVisibilityUseTheSameEligibility_data();
    void glyphAndPlateVisibilityUseTheSameEligibility();
    void readingPanePreservesSelectionAndScroll_data();
    void readingPanePreservesSelectionAndScroll();
    void readingPaneRemapsChangedText_data();
    void readingPaneRemapsChangedText();
};

void UltraClearTextTests::deviceRecognitionPrecedesReadbackAndDoesNotBlockUi()
{
    auto shared=std::make_shared<DeviceJobState>();ReleaseDeviceJob release{shared};
    shared->flipPreference=true;
    std::atomic<int> completions{};RecognitionResult completed;
    RecognitionScheduler scheduler(std::make_unique<DeviceRecognizer>(shared),[&](RecognitionResult value){completed=std::move(value);++completions;},{});
    QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
    auto image=std::make_shared<TestDeviceImage>(shared);
    SourceStamp stamp;stamp.sourceEpoch=1;stamp.frameSeq=1;stamp.viewportWidth=128;stamp.viewportHeight=64;stamp.viewTransform.valid=true;
    QVERIFY(scheduler.SubmitDevice(stamp,image)!=0);
    QTRY_VERIFY(shared->started.load());QCOMPARE(shared->readbacks.load(),0);
    int ticks=0;QTimer timer;connect(&timer,&QTimer::timeout,[&]{++ticks;});timer.start(1);
    QTRY_VERIFY(ticks>=10);QCOMPARE(completions.load(),0);
    shared->release=true;QTRY_COMPARE(completions.load(),1);
    QCOMPARE(shared->readbacks.load(),1);QCOMPARE(shared->cpuCalls.load(),0);QVERIFY(!shared->wrongOrder.load());
    QVERIFY(completed.deviceInput);QVERIFY(completed.output.ok);QVERIFY(completed.reference.IsValid());
    QCOMPARE(shared->preferences.load(),1);
    QCOMPARE(completed.stamp.frameSeq,stamp.frameSeq);
    image->width=9000;QCOMPARE(scheduler.SubmitDevice(stamp,image),std::uint64_t{0});
}

void UltraClearTextTests::cancelledDeviceJobRetainsInputAndRejectsLateOutput()
{
    auto shared=std::make_shared<DeviceJobState>();ReleaseDeviceJob release{shared};std::atomic<int> completions{};
    RecognitionScheduler scheduler(std::make_unique<DeviceRecognizer>(shared),[&](RecognitionResult){++completions;},{});
    QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
    auto image=std::make_shared<TestDeviceImage>(shared);std::weak_ptr<const DeviceImage> owned=image;
    SourceStamp stamp;stamp.sourceEpoch=1;stamp.frameSeq=1;stamp.viewportWidth=128;stamp.viewportHeight=64;stamp.viewTransform.valid=true;
    QVERIFY(scheduler.SubmitDevice(stamp,image)!=0);QTRY_VERIFY(shared->started.load());
    scheduler.Cancel();image.reset();QVERIFY(!owned.expired());
    shared->release=true;QTRY_VERIFY(!scheduler.HasWork());QTRY_VERIFY(owned.expired());
    QCOMPARE(completions.load(),0);QCOMPARE(shared->readbacks.load(),0);
}

void UltraClearTextTests::cpuBridgeKeepsCanonicalResidentAnalysisAcrossPromotion()
{
    auto shared = std::make_shared<ResidentBridgeState>();
    auto image = std::make_shared<CanonicalResidentImage>(shared);
    std::mutex mutex; QVector<RecognitionResult> completed;
    RecognitionScheduler scheduler(std::make_unique<ResidentBridgeRecognizer>(shared), [&](RecognitionResult result) {
        std::lock_guard lock(mutex); completed.push_back(std::move(result));
    }, {});
    QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
    const auto count = [&] { std::lock_guard lock(mutex); return completed.size(); };
    const auto stamp = ResidentBridgeStamp();
    QVERIFY(scheduler.SubmitDevice(stamp, image));
    QTRY_COMPARE(count(), 1);
    shared->preferred = true;
    QVERIFY(scheduler.SubmitDevice(stamp, image));
    QTRY_COMPARE(count(), 2);
    std::lock_guard lock(mutex);
    const auto& bridge = completed[0]; const auto& preferred = completed[1];
    QVERIFY(bridge.output.ok); QVERIFY(preferred.output.ok);
    QCOMPARE(bridge.output.lines[0].words[0].text, QStringLiteral("Bridge"));
    QCOMPARE(preferred.output.lines[0].words[0].text, QStringLiteral("Preferred"));
    QVERIFY(bridge.output.backendPreparing); QVERIFY(!preferred.output.backendPreparing);
    QVERIFY(bridge.jobId != preferred.jobId); QCOMPARE(bridge.authority, preferred.authority);
    QCOMPARE(bridge.sourcePixelDigestKind, RecognitionResult::DigestKind::GpuTreeSha256V1);
    QCOMPARE(preferred.sourcePixelDigestKind, bridge.sourcePixelDigestKind);
    QCOMPARE(bridge.sourcePixelDigest, image->CanonicalDigest());
    QCOMPARE(preferred.sourcePixelDigest, bridge.sourcePixelDigest);
    QVERIFY(bridge.sourcePixelDigest != SourcePixelsDigest(image->Pixels()));
    for (const auto& result : completed) {
        QVERIFY(result.deviceInput); QVERIFY(result.reference.resident); QVERIFY(result.reference.IsValid());
        QVERIFY(result.reference.luma.isNull()); QVERIFY(result.reference.integral.isEmpty());
        QCOMPARE(result.stamp.frameSeq, stamp.frameSeq); QCOMPARE(result.stamp.timestampMs, stamp.timestampMs);
        QCOMPARE(result.prepared.size(), 1); QVERIFY(result.prepared[0].reference.resident);
        QVERIFY(result.prepared[0].reference.values.isEmpty());
        QCOMPARE(result.lineColors[0].foreground, QColor(11, 29, 47));
        QVERIFY(result.output.lines[0].words[0].colors);
        QCOMPARE(result.output.lines[0].words[0].colors->background, QColor(231, 197, 103));
        QCOMPARE(result.currentInputMatches, QVector<bool>({true}));
    }
    QCOMPARE(shared->cpuCalls.load(), 1); QCOMPARE(shared->deviceCalls.load(), 1);
    QCOMPARE(shared->readbacks.load(), 1); QCOMPARE(shared->preparations.load(), 2);
    QCOMPARE(shared->validationReads.load(), 0); QCOMPARE(shared->probeReads.load(), 0);
}

void UltraClearTextTests::residentAnalysisRefusalNeverFallsBackToCpu_data()
{
    QTest::addColumn<bool>("preferred"); QTest::addColumn<bool>("cancel");
    QTest::newRow("cpu-bridge-refusal") << false << false;
    QTest::newRow("preferred-device-refusal") << true << false;
    QTest::newRow("cpu-bridge-cancelled") << false << true;
    QTest::newRow("preferred-device-cancelled") << true << true;
}

void UltraClearTextTests::residentAnalysisRefusalNeverFallsBackToCpu()
{
    QFETCH(bool, preferred); QFETCH(bool, cancel);
    auto shared = std::make_shared<ResidentBridgeState>();
    shared->preferred = preferred; shared->fail = !cancel; shared->holdAnalysis = cancel;
    const auto release = qScopeGuard([&] { shared->Release(); });
    auto image = std::make_shared<CanonicalResidentImage>(shared);
    std::weak_ptr<const DeviceImage> owned = image;
    RecognitionResult completed; std::atomic<int> completions{};
    RecognitionScheduler scheduler(std::make_unique<ResidentBridgeRecognizer>(shared), [&](RecognitionResult result) {
        completed = std::move(result); ++completions;
    }, {});
    QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
    QVERIFY(scheduler.SubmitDevice(ResidentBridgeStamp(), image));
    if (cancel) {
        QTRY_VERIFY(shared->analysisStarted.load());
        scheduler.Cancel(); image.reset(); QVERIFY(!owned.expired());
        shared->Release(); QTRY_VERIFY(!scheduler.HasWork()); QTRY_VERIFY(owned.expired());
        QCOMPARE(completions.load(), 0);
    } else {
        QTRY_COMPARE(completions.load(), 1);
        QVERIFY(!completed.output.ok); QVERIFY(completed.output.inputFault);
        QVERIFY(!completed.reference.IsValid()); QVERIFY(completed.prepared.isEmpty());
        QVERIFY(completed.sourcePixelDigest.isEmpty()); QVERIFY(completed.lineColors.isEmpty());
        QCOMPARE(completed.output.error, QStringLiteral("resident fixture refusal"));
    }
    QCOMPARE(shared->preparations.load(), 1);
    QCOMPARE(shared->readbacks.load(), preferred ? 0 : 1);
    QCOMPARE(shared->validationReads.load(), 0); QCOMPARE(shared->probeReads.load(), 0);
}

void UltraClearTextTests::residentProbePreparationRemainsSinglePass()
{
    auto shared = std::make_shared<ResidentBridgeState>();
    auto image = std::make_shared<CanonicalResidentImage>(shared);
    RecognitionResult completed; std::atomic<bool> done{};
    RecognitionScheduler scheduler(nullptr, [&](RecognitionResult result) { completed = std::move(result); done = true; }, {}, 1, 7, true);
    QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
    QVERIFY(scheduler.SubmitDevice(ResidentBridgeStamp(), image)); QTRY_VERIFY(done.load());
    QVERIFY(completed.output.ok); QVERIFY(completed.reference.resident);
    QVERIFY(completed.reference.IsValid()); QVERIFY(completed.sourcePixelDigest.isEmpty());
    QCOMPARE(shared->preparations.load(), 1); QCOMPARE(shared->readbacks.load(), 0);
    QCOMPARE(shared->validationReads.load(), 0); QCOMPARE(shared->probeReads.load(), 0);
    QCOMPARE(shared->cpuCalls.load(), 0); QCOMPARE(shared->deviceCalls.load(), 0);
}

void UltraClearTextTests::smoothedProbeFallbackDownloadsOnlyTheCurrentProbe()
{
    // A CPU-only DeviceImage has no resident validator. Its ReadProbe already
    // returns current-frame pixels; it must not then download a full image.
    auto shared=std::make_shared<DeviceJobState>();
    RecognitionResult completed; std::atomic<bool> done{};
    RecognitionScheduler reader(nullptr,[&](RecognitionResult r){completed=std::move(r);done=true;},{},1,4,true);
    QTRY_COMPARE(reader.Status().state,BackendState::kReady);
    auto image=std::make_shared<TestDeviceImage>(shared);
    image->kind=AnalysisKind::TemporallySmoothed;
    SourceStamp stamp;stamp.sourceEpoch=1;stamp.frameSeq=1;
    stamp.temporallySmoothed=true;stamp.viewportWidth=128;stamp.viewportHeight=64;
    stamp.viewTransform={0,0,1,1,0,0,1,1,true};
    QVERIFY(reader.SubmitDevice(stamp,image));
    QTRY_VERIFY(done.load());
    QVERIFY(completed.output.ok);QVERIFY(completed.reference.IsValid());
    QCOMPARE(shared->probes.load(),1);
    QCOMPARE(shared->readbacks.load(),0);
    QCOMPARE(shared->cpuCalls.load(),0);QCOMPARE(shared->gpuCalls.load(),0);
}

void UltraClearTextTests::deviceProbesContinueAt16xWhileInferenceIsHeld()
{
    auto shared=std::make_shared<DeviceJobState>();ReleaseDeviceJob release{shared};
    QWidget owner;UltraClearTextOverlay overlay(&owner,&owner);UltraClearTextPanel panel(&owner);
    std::int64_t now=1000;
    UltraClearTextController controller(&overlay,&panel,[shared]{return std::make_unique<DeviceRecognizer>(shared);},nullptr,[&]{return now;});
    controller.SetEnabled(true);QTRY_COMPARE(controller.BackendStatusSnapshot().state,BackendState::kReady);
    UctFrameContext context;context.geometry.settings.sourceEpoch=1;context.requested=context.geometry.settings;
    context.geometry.sceneWidth=128;context.geometry.sceneHeight=64;context.cameraActive=true;
    context.viewportWidth=960;context.viewportHeight=540;
    context.transform=ComputeViewTransform(128,64,960,540,16.f,.5f,.5f,ViewportFitMode::kFit);
    int captured=0;
    for(int frame=1;frame<=40;++frame){
        now+=90;context.frameSeq=frame;controller.OnPresented(context);controller.OnPresentationSucceeded();
        if(shared->liveImages.load()<2){auto image=std::make_shared<TestDeviceImage>(shared);image->frameSequence=frame;image->timestampMs=now;
            controller.OnDeviceScene(context,image);++captured;}
        QTest::qWait(4);
    }
    QTRY_VERIFY(controller.ProbeStatistics().completed>0);
    QTRY_COMPARE(shared->gpuCalls.load(),1);QVERIFY(shared->probes.load()>=10);QVERIFY(captured>=10);QCOMPARE(shared->readbacks.load(),0);
    QCOMPARE(controller.SchedulerStatistics().coalesced,std::uint64_t{0});
    controller.SetEnabled(false);shared->release=true;
    const auto acceptedBefore = controller.AcceptedResults();
    QTRY_COMPARE(shared->liveImages.load(),0);
    QTest::qWait(20);
    QCOMPARE(controller.AcceptedResults(),acceptedBefore);
    QCOMPARE(overlay.WordCount(),0);
}

void UltraClearTextTests::slowDeviceProbesStillSubmitOcrAndInputHealthRecovers()
{
    auto shared=std::make_shared<DeviceJobState>(); shared->release=true;
    shared->probeDelayMs=120; shared->probeFails=true;
    QWidget owner;UltraClearTextOverlay overlay(&owner,&owner);UltraClearTextPanel panel(&owner);
    std::int64_t now=1000;
    UltraClearTextController controller(&overlay,&panel,[shared]{return std::make_unique<DeviceRecognizer>(shared);},nullptr,[&]{return now;});
    controller.SetEnabled(true);QTRY_COMPARE(controller.BackendStatusSnapshot().state,BackendState::kReady);
    UctFrameContext context;context.geometry.settings.sourceEpoch=1;context.requested=context.geometry.settings;
    context.geometry.sceneWidth=128;context.geometry.sceneHeight=64;context.cameraActive=true;
    context.viewportWidth=960;context.viewportHeight=540;
    context.transform=ComputeViewTransform(128,64,960,540,16.f,.5f,.5f,ViewportFitMode::kFit);
    auto tick=[&] {
        now+=10; ++context.frameSeq;
        const bool wanted=controller.OnPresented(context);controller.OnPresentationSucceeded();
        if(wanted) { auto image=std::make_shared<TestDeviceImage>(shared);
            image->frameSequence=context.frameSeq;image->timestampMs=now;
            controller.OnDeviceScene(context,std::move(image)); }
        QTest::qWait(10);
    };
    for(int i=0;i<150 && !controller.StatusSource().contains("stopped working");++i) tick();
    QVERIFY2(controller.StatusSource().contains("stopped working"),qPrintable(controller.StatusSource()));
    QCOMPARE(controller.BackendStatusSnapshot().state,BackendState::kReady);
    shared->probeFails=false;
    for(int i=0;i<150 && shared->gpuCalls.load()==0;++i) tick();
    QVERIFY(shared->gpuCalls.load()>0);
    QVERIFY(!controller.StatusSource().contains("stopped working"));
    controller.OnAnalysisHealth(true);
    QVERIFY(controller.StatusSource().contains("stopped working"));
    controller.OnCpuViewport(context,nullptr,128,64,128*4);
    QVERIFY(controller.StatusSource().contains("stopped working"));
    QImage cpu(128,64,QImage::Format_ARGB32);cpu.fill(Qt::white);
    controller.OnCpuViewport(context,cpu.constBits(),cpu.width(),cpu.height(),cpu.bytesPerLine());
    QVERIFY(!controller.StatusSource().contains("stopped working"));
    QCOMPARE(controller.BackendStatusSnapshot().state,BackendState::kReady);
    controller.SetEnabled(false);
    QTRY_COMPARE(shared->liveImages.load(),0);
}

void UltraClearTextTests::staticDeviceSceneDoesNotContinuouslyRunOcr()
{
    const auto page=MakePage(kLines);auto shared=std::make_shared<ScriptShared>();shared->SetWords(page.words);
    ViewTransform identity;identity.valid=true;shared->transforms.insert(Checksum(page.image),identity);
    QWidget owner;UltraClearTextOverlay overlay(&owner,&owner);UltraClearTextPanel panel(&owner);
    std::int64_t now=1000;UctTiming timing;timing.refreshIntervalMs=10000;
    UltraClearTextController controller(&overlay,&panel,[shared]{return std::make_unique<ScriptedRecognizer>(shared);},nullptr,[&]{return now;},timing);
    controller.SetEnabled(true);QTRY_COMPARE(controller.BackendStatusSnapshot().state,BackendState::kReady);
    UctFrameContext context;context.geometry.settings.sourceEpoch=7;context.requested=context.geometry.settings;
    context.geometry.sceneWidth=page.image.width();context.geometry.sceneHeight=page.image.height();context.cameraActive=true;
    context.viewportWidth=960;context.viewportHeight=540;context.transform=ComputeViewTransform(page.image.width(),page.image.height(),960,540,16.f,.2f,.2f,ViewportFitMode::kFit);
    int captures=0;
    for(int frame=1;frame<=300;++frame){
        now+=20;context.frameSeq=frame;const bool wanted=controller.OnPresented(context);controller.OnPresentationSucceeded();
        if(wanted){const auto pixels=IndependentFramePixels(page.image,frame);
            {std::lock_guard lock(shared->mutex);shared->transforms.insert(Checksum(pixels),identity);}
            auto image=std::make_shared<SceneDeviceImage>(pixels);image->timestampMs=now;image->frameSequence=frame;
            controller.OnDeviceScene(context,image);++captures;}
        QTest::qWait(5);
    }
    QVERIFY(controller.AcceptedResults()>0);QVERIFY(captures>=20);
    std::lock_guard lock(shared->mutex);QVERIFY2(shared->calls<10,"A stable accepted device scene should not infer on every capture.");
    QVERIFY(shared->calls>=2);
}

void UltraClearTextTests::heldAcquisitionRetriesRejectedOcr_data()
{
    QTest::addColumn<int>("rejection");
    QTest::newRow("crossfade-before-completion") << int(AdmitVerdict::kContentMoved);
    QTest::newRow("backend-failure") << int(AdmitVerdict::kFailed);
    QTest::newRow("too-old") << int(AdmitVerdict::kTooOld);
}

void UltraClearTextTests::heldAcquisitionRetriesRejectedOcr()
{
    QFETCH(int, rejection);
    const auto livePage = MakePage({"Reading glasses help"});
    const auto heldPage = MakePage({"Reading glasses help", "Additional held text"});
    auto shared = std::make_shared<ScriptShared>();
    shared->SetWords(livePage.words);
    ViewTransform identity; identity.valid = true;
    shared->transforms.insert(Checksum(livePage.image), identity);
    shared->transforms.insert(Checksum(heldPage.image), identity);
    QWidget owner;
    UltraClearTextOverlay overlay(&owner, &owner);
    UltraClearTextPanel panel(&owner);
    std::int64_t now = 1000;
    UctTiming timing; timing.refreshIntervalMs = 100;
    UltraClearTextController controller(&overlay, &panel,
        [shared] { return std::make_unique<ScriptedRecognizer>(shared); },
        nullptr, [&] { return now; }, timing);
    const auto release = qScopeGuard([&] { shared->SetHold(false); });
    controller.SetEnabled(true);
    QTRY_COMPARE(controller.BackendStatusSnapshot().state, BackendState::kReady);
    UctFrameContext context;
    context.geometry.settings.sourceEpoch = 7;
    context.geometry.settings.stabilization = true;
    context.requested = context.geometry.settings;
    context.geometry.sceneWidth = livePage.image.width();
    context.geometry.sceneHeight = livePage.image.height();
    context.cameraActive = true;
    context.heldFrames = true;
    context.compositeFrames = true;
    context.viewportWidth = kViewWidth; context.viewportHeight = kViewHeight;
    context.transform = Transform(1.0f, .5f, .5f);
    std::uint64_t sequence = 0;
    const auto calls = [&] { std::lock_guard lock(shared->mutex); return shared->calls; };
    const auto tick = [&](const SyntheticPage& page, HoldDisplay display, std::uint64_t acquisition) {
        now += 100; context.frameSeq = ++sequence;
        const bool wanted = controller.OnPresented(context);
        controller.OnPresentationSucceeded();
        if (wanted) {
            auto image = std::make_shared<SceneDeviceImage>(page.image);
            image->frameSequence = context.frameSeq; image->timestampMs = now;
            image->hold = HoldProvenance{display,
                display == HoldDisplay::Live ? sequence : acquisition,
                display == HoldDisplay::Crossfade ? .5f : 0.0f};
            controller.OnDeviceScene(context, std::move(image));
        }
        QTest::qWait(10);
    };
    QTRY_VERIFY(([&] {
        tick(livePage, HoldDisplay::Live, 0);
        return controller.AcceptedResults() > 0 && panel.readingPane()->toPlainText().contains("Reading");
    })());
    // Drain the last warm-up probe/result before blocking the next job. The
    // failure must belong to H, not an older live job queued during warm-up.
    now += 100; // reopen normal input admission after the final warm-up copy
    QTRY_VERIFY(([&] {
        QCoreApplication::processEvents();
        const bool inputIdle = controller.OnPresented(context);
        controller.OnPresentationSucceeded();
        const auto scheduler = controller.SchedulerStatistics();
        const auto probes = controller.ProbeStatistics();
        return inputIdle && scheduler.submitted == scheduler.completed &&
            probes.submitted == probes.completed;
    })());
    // Keep established lines in the model, then hold a new acquisition with
    // additional text. A rejected job must not mark that acquisition as read.
    shared->SetWords(heldPage.words);
    shared->SetHold(true);
    const int beforeHeld = calls();
    const auto acquisition = ++sequence;
    QSignalSpy admitted(&controller, &UltraClearTextController::recognitionAdmitted);
    QTRY_VERIFY(([&] {
        tick(heldPage, HoldDisplay::Held, acquisition);
        return calls() > beforeHeld;
    })());
    { std::lock_guard lock(shared->mutex);
      QCOMPARE(shared->inputChecksums.constLast(), Checksum(heldPage.image)); }
    admitted.clear();
    if (rejection == int(AdmitVerdict::kContentMoved)) {
        const auto probesBefore = controller.ProbeStatistics().completed;
        QTRY_VERIFY(([&] {
            tick(heldPage, HoldDisplay::Crossfade, acquisition);
            return controller.ProbeStatistics().completed > probesBefore && overlay.WordCount() == 0;
        })());
    } else if (rejection == int(AdmitVerdict::kFailed)) {
        std::lock_guard lock(shared->mutex); shared->fail = true;
    } else {
        now += 5000; // real age rejection, independently of OCR/probe ordering
        controller.OnPresented(context); controller.OnPresentationSucceeded();
    }
    shared->SetHold(false);
    QTRY_VERIFY(!admitted.isEmpty());
    QCOMPARE(admitted.constLast().front().toInt(), rejection);
    { std::lock_guard lock(shared->mutex); shared->fail = false; }
    const int afterRejected = calls();
    QTRY_VERIFY(([&] {
        tick(heldPage, HoldDisplay::Held, acquisition);
        return calls() > afterRejected;
    })());
    // The fresh retry has entered the recognizer. Waiting for that worker must
    // not advance its injected wall clock by 100ms per polling iteration and
    // expire this job too on a loaded runner. The deliberate 5-second jump
    // above still proves that the original job was rejected as too old.
    QTRY_VERIFY(panel.readingPane()->toPlainText().contains("Additional"));
    const int afterRetry = calls();
    for (int frame = 0; frame < 20; ++frame) tick(heldPage, HoldDisplay::Held, acquisition);
    QCOMPARE(calls(), afterRetry); // an admitted held acquisition is still read once
    QVERIFY(!panel.readingPane()->toPlainText().contains("Additional Additional"));
}

void UltraClearTextTests::paddedRecognitionFitsVisibleInk_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("darkInk");
    QTest::addColumn<bool>("neighbour");
    QTest::addColumn<bool>("shifted");
    QTest::newRow("title-light") << QStringLiteral("Control") << false << false << false;
    QTest::newRow("descenders-dark") << QStringLiteral("gypsy") << true << false << false;
    QTest::newRow("accent-light") << QStringLiteral("caf\u00e9") << false << false << false;
    QTest::newRow("adjacent-line-descender") << QStringLiteral("Control") << false << true << false;
    QTest::newRow("ctc-left-undershoot") << QStringLiteral("Control") << false << false << true;
}

void UltraClearTextTests::paddedRecognitionFitsVisibleInk() {
    QFETCH(QString, text);
    QFETCH(bool, darkInk);
    QFETCH(bool, neighbour);
    QFETCH(bool, shifted);
    QImage image(640, 180, QImage::Format_ARGB32);
    image.fill(darkInk ? QColor(230, 230, 230) : QColor(80, 80, 80));
    QPainter painter(&image);
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(64);
    painter.setFont(font);
    painter.setPen(darkInk ? Qt::black : Qt::white);
    painter.drawText(QPointF(70, 110), text);
    painter.end();
    QRect actual;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const int value = qRed(image.pixel(x, y));
            if (darkInk ? value < 115 : value > 167)
                actual = actual.isNull() ? QRect(x, y, 1, 1) : actual.united(QRect(x, y, 1, 1));
        }
    QVERIFY(!actual.isEmpty());
    QRect padded = actual.adjusted(-10, -16, 10, 16);
    if (neighbour) {
        // A disconnected descender from the line above lies inside the padded
        // detection region; it must not inflate this word's cap height.
        QPainter extra(&image);
        extra.fillRect(QRect(actual.left() + 15, actual.top() - 13, 18, 6), Qt::white);
    }
    if (shifted)
        padded.setLeft(actual.left() + 12);
    auto shared = std::make_shared<ScriptShared>();
    shared->paddedBounds = true;
    shared->inkSearchPadding = shifted ? 24 : 0;
    shared->words = {{text, QRectF(padded.x() / 640.0, padded.y() / 180.0, padded.width() / 640.0,
                                   padded.height() / 180.0)}};
    const auto t = ComputeViewTransform(640, 180, 640, 180, 1, .5, .5, ViewportFitMode::kFit);
    shared->transforms.insert(Checksum(image), t);
    RecognitionResult received;
    std::atomic<bool> done{false};
    RecognitionScheduler scheduler(std::make_unique<ScriptedRecognizer>(shared),
                                   [&](RecognitionResult value) {
                                       received = std::move(value);
                                       done.store(true);
                                   },
                                   {});
    QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
    auto stamp = Stamp(1, 1, 100, t);
    stamp.viewportWidth = 640;
    stamp.viewportHeight = 180;
    QVERIFY(scheduler.Submit(stamp, image));
    QTRY_VERIFY(done.load());
    QCOMPARE(received.output.lines.size(), 1);
    const auto fitted = received.output.lines.first().words.first().boundsPx;
    QVERIFY(std::abs(fitted.top() - actual.top()) <= 2);
    QVERIFY(std::abs(fitted.height() - actual.height()) <= 3);
    QVERIFY(std::abs(fitted.width() - actual.width()) <= 3);
    QVERIFY(fitted.height() < padded.height() * .8);
}

void UltraClearTextTests::overlayStaysHiddenWhileOwnerBlockedOrApplicationInactive()
{
    QWidget owner;
    owner.resize(640, 480);
    QWidget target(&owner);
    target.setGeometry(20, 30, 580, 400);
    UltraClearTextOverlay overlay(&target, &owner);
    owner.show();
    const auto words = QVector<UltraClearTextOverlay::SceneWord>{
        {QStringLiteral("Reading"), QRectF(.2, .3, .4, .1),
         {QColor(Qt::white), QColor(Qt::black)}, 1}};
    overlay.SetViewTransform(ComputeViewTransform(640, 480, target.width(), target.height(),
        1.0f, .5f, .5f, ViewportFitMode::kFill));
    overlay.SetWords(words);
    QTRY_VERIFY(overlay.isVisible());

    QEvent blocked(QEvent::WindowBlocked);
    QApplication::sendEvent(&owner, &blocked);
    QVERIFY(!overlay.isVisible());
    overlay.SetWords(words); // completion while a native dialog owns the window
    target.resize(560, 380);
    owner.hide();
    owner.show();
    QApplication::processEvents();
    QVERIFY(!overlay.isVisible());
    QCOMPARE(overlay.WordCount(), 1);
    QVERIFY(!overlay.PaintedGeometryCurrent());

    QEvent unblocked(QEvent::WindowUnblocked);
    QApplication::sendEvent(&owner, &unblocked);
    QTRY_VERIFY(overlay.isVisible());
    QCOMPARE(overlay.size(), target.size());
    QCOMPARE(overlay.mapToGlobal(QPoint()), target.mapToGlobal(QPoint()));

    owner.setEnabled(false);
    QVERIFY(!overlay.isVisible());
    overlay.SetWords(words);
    QVERIFY(!overlay.isVisible());
    owner.setEnabled(true);
    QTRY_VERIFY(overlay.isVisible());

    const auto restoreActivation = qScopeGuard([] {
        QEvent activated(QEvent::ApplicationActivate);
        QApplication::sendEvent(qApp, &activated);
    });
    QEvent deactivated(QEvent::ApplicationDeactivate);
    QApplication::sendEvent(qApp, &deactivated);
    QVERIFY(!overlay.isVisible());
    overlay.SetWords(words);
    owner.move(owner.pos() + QPoint(10, 15));
    QApplication::processEvents();
    QVERIFY(!overlay.isVisible());
    QCOMPARE(overlay.WordCount(), 1);
    QEvent activated(QEvent::ApplicationActivate);
    QApplication::sendEvent(qApp, &activated);
    QTRY_VERIFY(overlay.isVisible());
    QTRY_VERIFY(overlay.PaintedGeometryCurrent());
}

void UltraClearTextTests::overlayStaysHiddenAcrossNestedModalDialogs()
{
    QWidget owner;
    owner.resize(640, 480);
    UltraClearTextOverlay overlay(&owner, &owner);
    owner.show();
    const auto words = QVector<UltraClearTextOverlay::SceneWord>{
        {QStringLiteral("Reading"), QRectF(.2, .3, .4, .1),
         {QColor(Qt::white), QColor(Qt::black)}, 1}};
    overlay.SetWords(words);
    QTRY_VERIFY(overlay.isVisible());
    QDialog outer(&owner);
    outer.setWindowModality(Qt::WindowModal);
    outer.show();
    QTRY_VERIFY(!overlay.isVisible());
    overlay.SetWords(words);
    QVERIFY(!overlay.isVisible());

    QDialog inner(&outer);
    inner.setWindowModality(Qt::WindowModal);
    inner.show();
    overlay.SetWords(words);
    QVERIFY(!overlay.isVisible());
    inner.reject();
    QApplication::processEvents();
    overlay.SetWords(words);
    QVERIFY(!overlay.isVisible());
    outer.reject();
    QTRY_VERIFY(overlay.isVisible());
    QCOMPARE(overlay.WordCount(), 1);
}

void UltraClearTextTests::overlayKeepsSourceBackgroundInsteadOfBlackWhite_data() {
    QTest::addColumn<QColor>("ink");
    QTest::addColumn<QColor>("paper");
    QTest::addColumn<bool>("lightInk");
    QTest::newRow("white-on-gray") << QColor(190, 190, 190) << QColor(80, 80, 80) << true;
    QTest::newRow("white-on-midgray") << QColor(210, 210, 210) << QColor(140, 140, 140) << true;
    QTest::newRow("dark-on-paper") << QColor(120, 120, 120) << QColor(190, 190, 190) << false;
    QTest::newRow("neutral-ink-still-snaps-white")
        << QColor(200, 200, 200) << QColor(30, 30, 30) << true;
}

void UltraClearTextTests::overlayKeepsSourceBackgroundInsteadOfBlackWhite() {
    QFETCH(QColor, ink);
    QFETCH(QColor, paper);
    QFETCH(bool, lightInk);
    QWidget target;
    target.resize(640, 180);
    UltraClearTextOverlay overlay(&target, &target);
    overlay.resize(target.size());
    overlay.SetViewTransform(
        ComputeViewTransform(640, 180, 640, 180, 1, .5, .5, ViewportFitMode::kFit));
    overlay.SetWords({{QStringLiteral("Control"), QRectF(.1, .25, .6, .4), {ink, paper}, 1}});
    QImage pixels(640, 180, QImage::Format_ARGB32_Premultiplied);
    pixels.fill(Qt::transparent);
    overlay.render(&pixels);
    const QColor plate = pixels.pixelColor(60, 50);
    QVERIFY(plate.alpha() > 250);
    QVERIFY(plate.red() > 0);
    QVERIFY(plate.red() < 255);
    // Keep the exact source paper whenever pure ink can provide enough contrast.
    if (paper.red() != 140)
        QCOMPARE(plate, paper);
    else
        QVERIFY(plate.red() < paper.red());
    bool foundInk = false;
    for (int y = 40; y < 125; ++y)
        for (int x = 65; x < 450; ++x) {
            const auto color = pixels.pixelColor(x, y);
            if (color.alpha() > 250 && (lightInk ? color.red() > 245 : color.red() < 10))
                foundInk = true;
        }
    QVERIFY(foundInk);
}

void UltraClearTextTests::deferredPresentationKeepsCommittedProjectionAndExpires() {
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    const auto committed = h.overlay->CurrentViewTransform();
    const auto rects = h.overlay->ProjectedWordRects();
    const auto reading = h.panel->readingPane()->toPlainText();
    h.completeReadbacks = false;
    for (int i = 0; i < 5; ++i) {
        h.now += 10;
        h.context.transform = Transform(3, .4f + i * .01f, .3f, ViewportFitMode::kFill);
        h.controller->OnPresented(h.context);
        h.controller->OnPresentationDeferred();
        QCOMPARE(h.overlay->CurrentViewTransform().sourceX, committed.sourceX);
        QCOMPARE(h.overlay->ProjectedWordRects(), rects);
        QCOMPARE(h.panel->readingPane()->toPlainText(), reading);
    }
    h.controller->OnPresented(h.context);
    h.controller->OnPresentationSucceeded();
    QCOMPARE(h.overlay->CurrentViewTransform().sourceX, h.context.transform.sourceX);
    h.now += 700;
    h.controller->OnPresented(h.context);
    h.controller->OnPresentationDeferred();
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.panel->readingPane()->toPlainText().isEmpty());
    h.controller->OnPresentationFailed();
    h.controller->OnPresentationDeferred();
    QCOMPARE(h.overlay->WordCount(), 0);
    h.controller->OnPresented(h.context);
    h.controller->OnPresentationSucceeded();
    h.context.viewportWidth += 80;
    h.controller->OnPresented(h.context);
    h.controller->OnPresentationDeferred();
    QCOMPARE(h.overlay->WordCount(), 0); // sticky busy state cannot authorize resized projection
}

void UltraClearTextTests::continuousPanDoesNotHideOverlayWhileOcrHeld() {
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    const auto started = h.controller->SchedulerStatistics().started;
    h.shared->SetHold(true);
    h.now += 1600;
    for (int i = 0; i < 200 && h.controller->SchedulerStatistics().started == started; ++i) {
        h.Present();
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QVERIFY(h.controller->SchedulerStatistics().started > started);
    QTRY_VERIFY(h.overlay->WordCount() > 0);
    h.frameSeq = h.controller->Model().Lines().first().validatedFrameSeq;
    h.context.frameSeq = h.frameSeq;
    h.completeReadbacks = false;
    struct VisibilityEvents : QObject {
        int hides{}, shows{};
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::Hide)
                ++hides;
            if (event->type() == QEvent::Show)
                ++shows;
            return false;
        }
    } events;
    h.overlay->installEventFilter(&events);
    const int words = h.overlay->WordCount();
    const auto reading = h.panel->readingPane()->toPlainText();
    for (int i = 0; i < 300; ++i) {
        ++h.now;
        h.context.transform = Transform(4, .35f + (i % 90) * .002f, .3f, ViewportFitMode::kFill);
        h.controller->OnPresented(h.context);
        if (i % 2)
            h.controller->OnPresentationDeferred();
        else
            h.controller->OnPresentationSucceeded();
        QCoreApplication::processEvents();
        QCOMPARE(h.overlay->WordCount(), words);
        QCOMPARE(h.panel->readingPane()->toPlainText(), reading);
    }
    QCOMPARE(events.hides, 0);
    QCOMPARE(events.shows, 0);
}

void UltraClearTextTests::glyphsShareLineBaselineDespiteDifferentInkHeights()
{
    QWidget target;
    target.resize(960, 540);
    UltraClearTextOverlay overlay(&target, &target);
    overlay.resize(target.size());
    overlay.SetViewTransform(ComputeViewTransform(960, 540, 960, 540, 1.0f, 0.5f, 0.5f,
                                                ViewportFitMode::kFit));
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(100);
    font.setWeight(QFont::Medium);
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF metrics(font);
    QVector<UltraClearTextOverlay::SceneWord> words;
    QVector<QRectF> expected;
    const QStringList texts{QStringLiteral("Changing"), QStringLiteral("the"),
                            QStringLiteral("page"), QStringLiteral("of"), QStringLiteral("CAPITAL")};
    qreal left = 30.0;
    for (int line = 0; line < 2; ++line) {
        const qreal baseline = 150.0 + line * 130.0;
        left = 30.0;
        for (const QString& text : texts) {
            const QRectF ink = metrics.tightBoundingRect(text);
            const QRectF bounds(left, baseline + ink.top() * 0.45,
                                ink.width() * 0.45, ink.height() * 0.45);
            expected.push_back(bounds);
            words.push_back({text, QRectF(bounds.x() / 960.0, bounds.y() / 540.0,
                                         bounds.width() / 960.0, bounds.height() / 540.0),
                             {}, static_cast<std::uint64_t>(line + 1)});
            left += bounds.width() + 20.0;
        }
    }
    overlay.SetWords(words);
    const auto glyphs = overlay.ProjectedGlyphs();
    QCOMPARE(glyphs.size(), words.size());
    for (int i = 0; i < glyphs.size(); ++i) {
        QVERIFY(std::abs(glyphs[i].baselineY - (150.0 + (i / texts.size()) * 130.0)) < 0.1);
        QVERIFY(std::abs(glyphs[i].inkRect.top() - expected[i].top()) < 0.1);
        QVERIFY(std::abs(glyphs[i].inkRect.height() - expected[i].height()) < 0.1);
    }
    // Pan/zoom uses the existing cached line layout, with no new OCR words.
    const auto zoom = ComputeViewTransform(960, 540, 960, 540, 2.0f, 0.4f, 0.4f,
                                           ViewportFitMode::kFill);
    overlay.SetViewTransform(zoom);
    const auto moved = overlay.ProjectedGlyphs();
    for (int i = 0; i < moved.size(); ++i) {
        const QPointF base(0.5, (150.0 + (i / texts.size()) * 130.0) / 540.0);
        QVERIFY(std::abs(moved[i].baselineY - SceneToViewPixel(zoom, 960, 540, base).y()) < 0.1);
    }
}

void UltraClearTextTests::fractionalProtectionCutsKeepProvedWords_data()
{
    QTest::addColumn<float>("zoom");
    QTest::addColumn<float>("pan");
    QTest::addColumn<bool>("oriented");
    for (const bool oriented : {false, true}) {
        for (int i = 0; i < 4; ++i)
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(oriented).arg(i)))
                << (i < 2 ? 1.f : 3.f) << (.38f + i * .00031f) << oriented;
    }
}

void UltraClearTextTests::fractionalProtectionCutsKeepProvedWords()
{
    QFETCH(float, zoom);
    QFETCH(float, pan);
    QFETCH(bool, oriented);
    const auto scene = [](const QRectF& r) {
        return QRectF(r.x() / 960, r.y() / 540, r.width() / 960, r.height() / 540);
    };
    const QRectF proved(241.25, 166.75, 140, 40);
    // These are the exact continuous remainders of an ordinary uncertainty
    // box after the newly proved footprint was subtracted. The incoming
    // detector word extends two pixels past the old uncertainty, towards an
    // already proved neighbour, just as in the recorded book example.
    const QVector<QRectF> protection{scene({237.25, 160.25, 142, 6.5}),
                                    scene({237.25, 206.75, 142, 6}),
                                    scene({237.25, 166.75, 4, 40})};
    QWidget target;
    target.resize(960, 540);
    UltraClearTextOverlay overlay(&target, &target);
    target.show();
    const auto transform = ComputeViewTransform(960, 540, 960, 540, zoom, pan, .35f,
                                               ViewportFitMode::kFill);
    overlay.SetViewTransform(transform);
    QVector<UltraClearTextOverlay::SceneWord> words{
        {QStringLiteral("to"), scene(proved), {Qt::black, Qt::white}, 1, protection, 0},
        {QStringLiteral("an"), scene({380.45, 166.75, 110, 40}),
         {Qt::black, Qt::white}, 2, protection, 0}};
    if (oriented) for (auto& word : words) word.sceneQuad = TextQuad::FromRect(word.sceneRect);
    overlay.SetWords(words);
    QTRY_VERIFY(overlay.PaintedGeometryCurrent());
    for (const auto& word : words) {
        UltraClearTextOverlay::PaintedWordGeometry geometry;
        QVERIFY(overlay.PaintedWord(word.lineId, word.wordIndex, word.text, geometry));
        QVERIFY2(geometry.drawn, "Rounding a carved protection edge suppressed proved ink.");
    }
    QImage base(target.size(), QImage::Format_ARGB32);
    base.fill(Qt::white);
    const QColor punctuation(191, 17, 39);
    {
        QPainter painter(&base);
        painter.fillRect(SceneRectToView(transform, 960, 540, scene({238.25, 162.25, 2, 2})),
                         punctuation);
    }
    QImage composite = base;
    {
        QPainter painter(&composite);
        overlay.CaptureSnapshot(transform, composite.size()).Paint(painter, composite.size());
    }
    int protectedPixels = 0;
    for (int y = 0; y < base.height(); ++y) for (int x = 0; x < base.width(); ++x)
        if (base.pixelColor(x, y) == punctuation) {
            ++protectedPixels;
            QCOMPARE(composite.pixelColor(x, y), punctuation);
        }
    QVERIFY(protectedPixels > 0);
}

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
                                 QStringLiteral("5S"), QStringLiteral("x_2"), QStringLiteral("a_b"), QString()}) {
        QVERIFY2(IsHighRiskToken(risky), qPrintable(risky));
    }
    for (const QString& safe : {QStringLiteral("Hello"), QStringLiteral("3.14"), QStringLiteral("-12"),
                                QStringLiteral("1,000"), QStringLiteral("page"), QStringLiteral("(see)"),
                                QStringLiteral("42"), QStringLiteral("Grüße"),
                                QStringLiteral("scripts/integrate_castle_runtime.py"),
                                QStringLiteral("C:\\My_Files\\paper.txt")}) {
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

void UltraClearTextTests::livePendingEvidenceRejectsDuplicatePixels()
{
    const auto page=MakePage({QStringLiteral("Read page")});
    const auto t=Transform(1,.5f,.5f);
    auto first=ResultFor(page,t,Stamp(3,4,100,t));first.jobId=1;first.authority=7;
    OverlayModel model;
    QCOMPARE(model.Admit(first,first.stamp,nullptr,110),AdmitVerdict::kPending);
    auto duplicate=first;
    for(std::uint64_t job=2;job<=5;++job) {
        duplicate.jobId=job;duplicate.stamp.frameSeq=100+job;
        duplicate.stamp.timestampMs=100+static_cast<std::int64_t>(job)*10;
        duplicate.reference.stamp=duplicate.stamp;
        QCOMPARE(duplicate.sourcePixelDigest,first.sourcePixelDigest); // literally the same source bytes
        QCOMPARE(model.Admit(duplicate,duplicate.stamp,nullptr,duplicate.stamp.timestampMs+1),
                 AdmitVerdict::kPending);
        QCOMPARE(model.AcceptedWordCount(),0);
        QVERIFY(model.SceneWords(duplicate.stamp.timestampMs+1,duplicate.stamp.frameSeq).isEmpty());
    }
    auto sameFrame=ResultFor(page,t,Stamp(3,4,200,t));sameFrame.authority=7;
    sameFrame.stamp.frameSeq=duplicate.stamp.frameSeq;sameFrame.reference.stamp=sameFrame.stamp;
    QVERIFY(sameFrame.sourcePixelDigest!=duplicate.sourcePixelDigest);
    QCOMPARE(model.Admit(sameFrame,sameFrame.stamp,nullptr,210),AdmitVerdict::kPending);
    auto independent=ResultFor(page,t,Stamp(3,4,300,t));independent.authority=7;
    QVERIFY(independent.sourcePixelDigest!=sameFrame.sourcePixelDigest);
    QCOMPARE(model.Admit(independent,independent.stamp,nullptr,310),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(310,independent.stamp.frameSeq),QStringLiteral("Read page"));

    OverlayModel missing;
    missing.Admit(first,first.stamp,nullptr,110);
    auto unknown=independent;unknown.sourcePixelDigest.clear();
    QCOMPARE(missing.Admit(unknown,unknown.stamp,nullptr,310),AdmitVerdict::kPending);
    auto fresh=ResultFor(page,t,Stamp(3,4,400,t));fresh.authority=7;
    QCOMPARE(missing.Admit(fresh,fresh.stamp,nullptr,410),AdmitVerdict::kPending);
    auto otherAuthority=ResultFor(page,t,Stamp(3,4,500,t));otherAuthority.authority=8;
    QCOMPARE(missing.Admit(otherAuthority,otherAuthority.stamp,nullptr,510),AdmitVerdict::kPending);
    QCOMPARE(missing.AcceptedWordCount(),0);
}

void UltraClearTextTests::liveCaptureAcceptsIdenticalPixelsOnlyFromNewAcquisitions()
{
    const auto page=MakePage({QStringLiteral("Read page")});
    const auto transform=Transform(1,.5f,.5f);
    auto first=ResultFor(page,transform,Stamp(3,4,100,transform));first.authority=7;
    first.stamp.liveCameraCapture=true;first.reference.stamp=first.stamp;
    first.sourcePixelDigest.clear(); // camera jobs no longer scan pixels for a hash
    OverlayModel model;
    QCOMPARE(model.AdmitLiveCamera(first,first.stamp,nullptr,110),AdmitVerdict::kPending);
    auto replay=first;
    for(int tick=1;tick<=4;++tick) {
        replay.jobId=20+tick;replay.stamp.timestampMs=100+tick*10;replay.reference.stamp=replay.stamp;
        QCOMPARE(model.AdmitLiveCamera(replay,replay.stamp,nullptr,150),AdmitVerdict::kPending);
        QCOMPARE(model.AcceptedWordCount(),0);
    }
    auto next=replay;next.stamp.frameSeq++;next.reference.stamp=next.stamp;
    QCOMPARE(next.sourcePixelDigest,first.sourcePixelDigest);
    QCOMPARE(model.AdmitLiveCamera(next,next.stamp,nullptr,160),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(160,next.stamp.frameSeq),QStringLiteral("Read page"));
    QCOMPARE(model.SceneWords(160,next.stamp.frameSeq).size(),2);

    // Entering a saved/unknown input policy cannot borrow camera evidence.
    QCOMPARE(model.AdmitAdvancingMedia(next,next.stamp,nullptr,160),AdmitVerdict::kPending);
    QCOMPARE(model.AcceptedWordCount(),0);
    auto saved=next;saved.stamp.frameSeq++;saved.reference.stamp=saved.stamp;
    QCOMPARE(model.AdmitAdvancingMedia(saved,saved.stamp,nullptr,170),AdmitVerdict::kPending);
    QCOMPARE(model.AcceptedWordCount(),0);

    // Missing identity, older acquisitions, source/settings changes and a
    // different scheduler authority remain insufficient camera evidence.
    for(int fault=0;fault<5;++fault) {
        OverlayModel guarded;
        guarded.AdmitLiveCamera(first,first.stamp,nullptr,110);
        auto bad=next;
        if(fault==0)bad.stamp.liveCameraCapture=false;
        if(fault==1)bad.stamp.frameSeq=first.stamp.frameSeq-1;
        if(fault==2)++bad.stamp.sourceEpoch;
        if(fault==3)++bad.stamp.geometryGeneration;
        if(fault==4)++bad.authority;
        bad.reference.stamp=bad.stamp;
        QCOMPARE(guarded.AdmitLiveCamera(bad,bad.stamp,nullptr,160),AdmitVerdict::kPending);
        QCOMPARE(guarded.AcceptedWordCount(),0);
    }
}

void UltraClearTextTests::heldFramesReadOnceAndCrossfadesRevoke()
{
    const auto page=MakePage({QStringLiteral("Read page")});
    const auto t=Transform(1,.5f,.5f);
    const auto live=[&](std::int64_t ms){
        auto r=ResultFor(page,t,Stamp(3,4,ms,t));r.authority=7;
        r.stamp.liveCameraCapture=true;r.reference.stamp=r.stamp;r.sourcePixelDigest.clear();
        return r;};
    const auto held=[&](std::int64_t ms,std::uint64_t acquisition){
        auto r=live(ms);r.stamp.holdDisplay=HoldDisplay::Held;
        r.stamp.holdAcquisition=acquisition;r.reference.stamp=r.stamp;return r;};

    // Enabling UCT during a hold reads the exact held still once: its words
    // reach the reading pane behind an omission mark, but no glyph or
    // screen-reader word is claimed from a single observation.
    {
        OverlayModel model;
        auto first=held(110,100);
        QCOMPARE(model.AdmitLiveCamera(first,first.stamp,nullptr,120),AdmitVerdict::kPending);
        QVERIFY(model.SceneWords(120,110).isEmpty());
        QVERIFY(model.ScreenLines(120,110).isEmpty());
        QCOMPARE(model.ReadingText(120,110),QStringLiteral("\u2026 Read page"));
        // Repeating the same held acquisition under newer presentation
        // sequences is still that one observation, never corroboration.
        auto repeat=held(150,100);
        QCOMPARE(model.AdmitLiveCamera(repeat,repeat.stamp,nullptr,160),AdmitVerdict::kPending);
        QVERIFY(model.SceneWords(160,150).isEmpty());
        // The next genuinely new acquisition confirms the held pending text.
        auto next=live(200);
        QCOMPARE(model.AdmitLiveCamera(next,next.stamp,nullptr,210),AdmitVerdict::kAccepted);
        QCOMPARE(model.SceneWords(210,200).size(),2);
        QCOMPARE(model.ReadingText(210,200),QStringLiteral("Read page"));
    }
    // A held copy of an acquisition already read live cannot confirm it.
    {
        OverlayModel model;
        auto first=live(100);
        QCOMPARE(model.AdmitLiveCamera(first,first.stamp,nullptr,110),AdmitVerdict::kPending);
        auto same=held(120,100);
        QCOMPARE(model.AdmitLiveCamera(same,same.stamp,nullptr,130),AdmitVerdict::kPending);
        QVERIFY(model.SceneWords(130,120).isEmpty());
        auto again=held(180,100);
        model.AdmitLiveCamera(again,again.stamp,nullptr,190);
        QVERIFY(model.SceneWords(190,180).isEmpty());
        auto next=live(200);
        QCOMPARE(model.AdmitLiveCamera(next,next.stamp,nullptr,210),AdmitVerdict::kAccepted);
        QCOMPARE(model.SceneWords(210,200).size(),2);
    }
    // Crossfade, unknown and inconsistent held stamps are never evidence,
    // whatever their presentation sequence claims.
    for (int kind=0;kind<4;++kind) {
        OverlayModel model;
        auto bad=live(110);
        if(kind==0)bad.stamp.holdDisplay=HoldDisplay::Crossfade;
        if(kind==1)bad.stamp.holdDisplay=HoldDisplay::Unknown;
        if(kind==2){bad.stamp.holdDisplay=HoldDisplay::Held;bad.stamp.holdAcquisition=110;}
        if(kind==3){bad.stamp.holdDisplay=HoldDisplay::Held;bad.stamp.holdAcquisition=120;}
        bad.reference.stamp=bad.stamp;
        QCOMPARE(model.AdmitLiveCamera(bad,bad.stamp,nullptr,120),AdmitVerdict::kFailed);
        QCOMPARE(model.AcceptedWordCount(),0);
        QVERIFY(model.ReadingText(120,110).isEmpty());
    }
    // Composite identity revokes display even with no queued line checks or
    // an exhausted worker budget, and blocks a late older OCR completion.
    for (int checkMode=0;checkMode<3;++checkMode) {
        OverlayModel model;
        auto first=live(100);model.AdmitLiveCamera(first,first.stamp,nullptr,110);
        auto second=live(200);
        QCOMPARE(model.AdmitLiveCamera(second,second.stamp,nullptr,210),AdmitVerdict::kAccepted);
        QCOMPARE(model.SceneWords(210,200).size(),2);
        auto blend=ProbeOf(RenderView(page.image,t,kViewWidth,kViewHeight),t,3,4,250,250);
        blend.stamp.holdDisplay=HoldDisplay::Crossfade;
        QVector<LineCheck> checks;
        if (checkMode==1) {
            checks=CheckLines(model.LineReferences(blend.stamp),blend,{},0.0);
            for(auto& check:checks)check.checked=false;
        }
        const ValidationSummary revoked=checkMode==2 ? model.Validate(blend,250)
            : model.ApplyChecks(checks,blend.stamp,false);
        QCOMPARE(revoked.valid,0);
        QVERIFY(model.SceneWords(260,250).isEmpty());
        QVERIFY(model.ReadingText(260,250).isEmpty());
        QCOMPARE(model.Lines().first().validatedFrameSeq,std::uint64_t{200});
        QCOMPARE(model.AdmitLiveCamera(second,blend.stamp,&blend,260),AdmitVerdict::kContentMoved);
        QVERIFY(model.SceneWords(260,250).isEmpty());
        const auto current=ProbeOf(RenderView(page.image,t,kViewWidth,kViewHeight),t,3,4,300,300);
        const ValidationSummary restored=model.Validate(current,300);
        QVERIFY(restored.valid>0);
        QCOMPARE(model.SceneWords(310,300).size(),2);
        QCOMPARE(model.Lines().first().validatedFrameSeq,std::uint64_t{300});
    }
}
void UltraClearTextTests::smoothedResultsNeedCurrentFrameProbeProof()
{
    const auto page=MakePage({QStringLiteral("Read page")});
    const auto t=Transform(1,.5f,.5f);
    const auto smoothed=[&](std::int64_t ms){
        auto result=ResultFor(page,t,Stamp(3,4,ms,t));
        result.authority=7;
        result.stamp.liveCameraCapture=true;
        result.stamp.temporallySmoothed=true;
        result.stamp.historyEpoch=5;
        result.stamp.seedFrameSeq=result.stamp.frameSeq;
        result.stamp.blendFrameCount=4;
        result.stamp.temporalAlpha=0.25f;
        result.currentInputMatches.fill(true,result.output.lines.size());
        result.reference.stamp=result.stamp;
        result.sourcePixelDigest.clear();
        return result;
    };
    OverlayModel model;
    auto first=smoothed(100);
    QCOMPARE(model.AdmitLiveCamera(first,first.stamp,nullptr,110),AdmitVerdict::kPending);
    auto second=smoothed(200);
    model.AdmitLiveCamera(second,second.stamp,nullptr,210);
    // Two agreeing smoothed reads confirm the words but draw/read nothing:
    // smoothed pixels are never their own proof.
    QCOMPARE(model.AcceptedWordCount(),2);
    QVERIFY(model.SceneWords(210,second.stamp.frameSeq).isEmpty());
    QVERIFY(model.ReadingText(210,second.stamp.frameSeq).isEmpty() ||
            !model.ReadingText(210,second.stamp.frameSeq).contains(QStringLiteral("Read")));
    // An older single-frame probe cannot validate the newer blend.
    const ProbeFrame older=ProbeOf(RenderView(page.image,t,kViewWidth,kViewHeight),t,3,4,
                                    second.stamp.frameSeq-1,250);
    model.Validate(older,250);
    QVERIFY(model.SceneWords(260,second.stamp.frameSeq).isEmpty());
    // A current-frame probe at the blend's latest frame validates it.
    const ProbeFrame current=ProbeOf(RenderView(page.image,t,kViewWidth,kViewHeight),t,3,4,
                                      second.stamp.frameSeq,300);
    const ValidationSummary ok=model.Validate(current,300);
    QVERIFY(ok.valid>0);
    QVERIFY(!model.SceneWords(310,second.stamp.frameSeq).isEmpty());
    QCOMPARE(model.Lines().first().validatedFrameSeq,second.stamp.frameSeq);
    // Text that leaves the current frame is removed by the next probe, even
    // though a fading smoothed blend would still show it.
    const SyntheticPage moved=MakePage({QStringLiteral("Read page")},QPointF(150.0,90.0));
    const ProbeFrame gone=ProbeOf(RenderView(moved.image,t,kViewWidth,kViewHeight),t,3,4,
                                   second.stamp.frameSeq+1,400);
    const ValidationSummary bad=model.Validate(gone,400);
    QVERIFY(bad.invalid>0);
    QVERIFY(model.SceneWords(410,second.stamp.frameSeq+1).isEmpty());

    // A perfectly trackable raw patch may contain DIFFERENT text from the
    // fading OCR input. Current-to-current tracking must not validate it.
    for (const bool missingProof : {false,true}) {
        OverlayModel changedInput;
        auto staleFirst=first,staleSecond=second;
        if (missingProof) { staleFirst.currentInputMatches.clear(); staleSecond.currentInputMatches.clear(); }
        else { staleFirst.currentInputMatches.fill(false,staleFirst.output.lines.size());
               staleSecond.currentInputMatches.fill(false,staleSecond.output.lines.size()); }
        changedInput.AdmitLiveCamera(staleFirst,staleFirst.stamp,nullptr,110);
        changedInput.AdmitLiveCamera(staleSecond,staleSecond.stamp,nullptr,210);
        changedInput.Validate(current,300);
        QVERIFY(changedInput.SceneWords(310,second.stamp.frameSeq).isEmpty());
        QVERIFY(!changedInput.ReadingText(310,second.stamp.frameSeq).contains(QStringLiteral("Read")));
        OverlayModel immutableInput;
        immutableInput.AdmitImmutable(staleSecond,staleSecond.stamp,210);
        QVERIFY(!immutableInput.ReadingText(210,second.stamp.frameSeq).contains(QStringLiteral("Read")));
    }
    // An intermittent OCR/current mismatch adds no new evidence, but must not
    // erase an old line already validated by a probe at/after that OCR frame.
    OverlayModel retained;
    retained.AdmitLiveCamera(first,first.stamp,nullptr,110);
    retained.AdmitLiveCamera(second,second.stamp,nullptr,210);
    const auto latest=ProbeOf(RenderView(page.image,t,kViewWidth,kViewHeight),t,3,4,320,330);
    retained.Validate(latest,330);
    QVERIFY(!retained.SceneWords(335,320).isEmpty());
    const auto old=retained.Lines().first();
    auto failed=smoothed(320);failed.currentInputMatches.fill(false,failed.output.lines.size());
    retained.AdmitLiveCamera(failed,failed.stamp,&latest,340);
    QCOMPARE(retained.ReadingText(340,320),QStringLiteral("Read page"));
    QCOMPARE(retained.Lines().first().id,old.id);
    QCOMPARE(retained.Lines().first().validatedAtMs,old.validatedAtMs);
    QCOMPARE(retained.Lines().first().validatedFrameSeq,old.validatedFrameSeq);
    retained.Validate(ProbeOf(RenderView(moved.image,t,kViewWidth,kViewHeight),t,3,4,321,350),350);
    QVERIFY(retained.SceneWords(360,321).isEmpty());
}

void UltraClearTextTests::liveDuplicateRefreshPreservesEstablishedWords()
{
    const auto page=MakePage({QStringLiteral("Read page")});
    const auto t=Transform(1,.5f,.5f);
    OverlayModel model;
    auto first=ResultFor(page,t,Stamp(3,4,100,t));first.output.lines[0].words.removeLast();
    auto second=ResultFor(page,t,Stamp(3,4,200,t));second.output.lines[0].words.removeLast();
    model.Admit(first,first.stamp,nullptr,110);
    QCOMPARE(model.Admit(second,second.stamp,nullptr,210),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(210,second.stamp.frameSeq),QStringLiteral("Read"));
    auto withNewWord=ResultFor(page,t,Stamp(3,4,300,t));
    QCOMPARE(model.Admit(withNewWord,withNewWord.stamp,nullptr,310),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(310,withNewWord.stamp.frameSeq),QStringLiteral("Read \u2026"));
    QCOMPARE(model.AcceptedWordCount(),1);
    auto duplicate=withNewWord;duplicate.jobId=19;duplicate.stamp.timestampMs=400;duplicate.stamp.frameSeq=400;
    duplicate.reference.stamp=duplicate.stamp;
    QCOMPARE(model.Admit(duplicate,duplicate.stamp,nullptr,410),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(410,duplicate.stamp.frameSeq),QStringLiteral("Read \u2026"));
    QCOMPARE(model.SceneWords(410,duplicate.stamp.frameSeq).size(),1);
    QCOMPARE(model.SceneWords(410,duplicate.stamp.frameSeq).first().text,QStringLiteral("Read"));
    auto independent=ResultFor(page,t,Stamp(3,4,500,t));
    QCOMPARE(model.Admit(independent,independent.stamp,nullptr,510),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(510,independent.stamp.frameSeq),QStringLiteral("Read page"));
}

void UltraClearTextTests::pendingEvidenceCannotCrossResetOrAdmissionMode()
{
    const auto page=MakePage({QStringLiteral("Read page")});
    const auto t=Transform(1,.5f,.5f);
    const auto first=ResultFor(page,t,Stamp(3,4,100,t));
    const auto second=ResultFor(page,t,Stamp(3,4,200,t));
    OverlayModel model;
    QCOMPARE(model.Admit(first,first.stamp,nullptr,110),AdmitVerdict::kPending);
    model.Reset();
    QCOMPARE(model.Admit(second,second.stamp,nullptr,210),AdmitVerdict::kPending);
    const auto third=ResultFor(page,t,Stamp(3,4,300,t));
    QCOMPARE(model.AdmitImmutable(third,third.stamp,310),AdmitVerdict::kPending);
    QVERIFY(model.SceneWords(310,third.stamp.frameSeq).isEmpty());
    const auto fourth=ResultFor(page,t,Stamp(3,4,400,t));
    QCOMPARE(model.Admit(fourth,fourth.stamp,nullptr,410),AdmitVerdict::kPending);
    QCOMPARE(model.AcceptedWordCount(),0);
    const auto fifth=ResultFor(page,t,Stamp(3,4,500,t));
    QCOMPARE(model.Admit(fifth,fifth.stamp,nullptr,510),AdmitVerdict::kAccepted);
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
    ModelTuning tuning;
    // This tests matching, not the controller's bounded UI admission work.
    tuning.admitMatchBudgetMs = 1000.0;
    OverlayModel model(tuning);
    const SourceStamp current = Stamp(3, 4, 0, t);
    const ProbeFrame newer = ProbeOf(RenderView(moved.image, t, kViewWidth, kViewHeight), t, 3, 4, 9, 500);
    QCOMPARE(model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), current, &newer, 520),
             AdmitVerdict::kContentMoved);
    QCOMPARE(model.AcceptedWordCount(), 0);
}

void UltraClearTextTests::modelAdmissionBudgetNeverDisplaysUncheckedText()
{
    const SyntheticPage page = MakePage(kLines);
    const ViewTransform t = Transform(1.0f, 0.5f, 0.5f);
    ModelTuning tuning;tuning.admitMatchBudgetMs = 0.0;
    OverlayModel model(tuning);const auto current=Stamp(3,4,0,t);
    const auto newer=ProbeOf(RenderView(page.image,t,kViewWidth,kViewHeight),t,3,4,9,500);
    model.Admit(ResultFor(page,t,Stamp(3,4,100,t)),current,nullptr,110);
    model.Admit(ResultFor(page,t,Stamp(3,4,200,t)),current,&newer,520);
    QVERIFY(model.HasAcceptedText());
    QVERIFY(model.SceneWords(520,9).isEmpty());QVERIFY(model.ReadingText(520,9).isEmpty());
    model.ApplyChecks(CheckLines(model.LineReferences(current),newer,tuning.match,0.0),newer.stamp);
    QVERIFY(!model.SceneWords(530,9).isEmpty());QVERIFY(!model.ReadingText(530,9).isEmpty());
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
    const QString text = model.ReadingText(210, model.Lines().first().validatedFrameSeq);
    QVERIFY2(text.contains(QStringLiteral("3.14")), qPrintable(text));
    QVERIFY2(!text.contains(QStringLiteral("l0O")), qPrintable(text));
    QCOMPARE(text, QStringLiteral("Total is 3.14 and \u2026 ok"));
}

void UltraClearTextTests::readingTextPreservesOmissionPositions_data()
{
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("expected");
    QTest::newRow("beginning") << QStringLiteral("1 tablet daily")
                               << QStringLiteral("\u2026 tablet daily");
    QTest::newRow("middle") << QStringLiteral("Take 1 tablet")
                            << QStringLiteral("Take \u2026 tablet");
    QTest::newRow("end") << QStringLiteral("Take tablet 1")
                         << QStringLiteral("Take tablet \u2026");
    QTest::newRow("consecutive") << QStringLiteral("Take 1 O tablet")
                                 << QStringLiteral("Take \u2026 tablet");
    QTest::newRow("separate-gaps") << QStringLiteral("1 Take O tablet 1")
                                   << QStringLiteral("\u2026 Take \u2026 tablet \u2026");
}

void UltraClearTextTests::readingTextPreservesOmissionPositions()
{
    QFETCH(QString, source);
    QFETCH(QString, expected);
    const SyntheticPage page = MakePage({source});
    const ViewTransform transform = Transform(1.0f, 0.5f, 0.5f);
    const SourceStamp current = Stamp(1, 1, 0, transform);
    OverlayModel model;
    QCOMPARE(model.Admit(ResultFor(page, transform, Stamp(1, 1, 100, transform)),
                         current, nullptr, 110), AdmitVerdict::kPending);
    QCOMPARE(model.Admit(ResultFor(page, transform, Stamp(1, 1, 200, transform)),
                         current, nullptr, 210), AdmitVerdict::kAccepted);
    QVERIFY(!model.Lines().isEmpty());
    const auto frame = model.Lines().first().validatedFrameSeq;
    QCOMPARE(model.ReadingText(210, frame), expected);
    // The omission marker belongs only to the accessible text, never to a
    // replacement glyph drawn over the deliberately excluded source token.
    for (const SceneWord& word : model.SceneWords(210, frame)) {
        QVERIFY(!IsHighRiskToken(word.text));
        QVERIFY(word.text != QStringLiteral("\u2026"));
    }
}

void UltraClearTextTests::rejectedTinyAndDenseSpansKeepOrderedOmissions()
{
    const auto page=MakePage({QStringLiteral("Read x\u00b2 page")});
    const auto t=Transform(1,.5f,.5f);
    const auto make=[&](std::int64_t time) {
        auto result=ResultFor(page,t,Stamp(1,1,time,t));
        const auto original=result.output.lines[0].words;
        QVector<RecognizedWord> spans{original[0]};
        const QRectF tiny(original[0].boundsPx.right()+1,original[0].boundsPx.top(),2,3);
        spans.push_back({QStringLiteral("."),tiny});
        spans.push_back(original[1]); // full-height unsupported notation
        for(int i=0;i<180;++i)
            spans.push_back({QStringLiteral("not"),original[1].boundsPx,false,{},
                             TokenDisposition::kLowConfidence,.60,original[1].boundsPx});
        spans.push_back(original[2]);
        result.output.lines[0].words=spans;
        result.prepared=PrepareLines(result,7);
        return result;
    };
    auto first=make(100),second=make(200);
    QCOMPARE(second.prepared[0].words.size(),184);
    QCOMPARE(second.prepared[0].words[1].disposition,TokenDisposition::kTooSmall);
    QCOMPARE(second.prepared[0].words[2].disposition,TokenDisposition::kHighRisk);
    OverlayModel model;
    QCOMPARE(model.Admit(first,first.stamp,nullptr,110),AdmitVerdict::kPending);
    QCOMPARE(model.Admit(second,second.stamp,nullptr,210),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(210,0),QStringLiteral("Read \u2026 page"));
    QCOMPARE(model.Lines()[0].omittedWords,182);
    QCOMPARE(model.Lines()[0].protectedSceneRects.size(),182);
    QCOMPARE(model.SceneWords(210,0).size(),2);
}

void UltraClearTextTests::untrackableTextStaysReadableWithoutReplacement()
{
    const auto page=MakePage({QStringLiteral("Read page")});
    const auto t=Transform(1,.5f,.5f);
    const auto make=[&](std::int64_t time) {
        auto result=ResultFor(page,t,Stamp(1,1,time,t));
        QImage flat(kViewWidth,kViewHeight,QImage::Format_ARGB32);flat.fill(Qt::white);
        flat=IndependentFramePixels(flat,result.stamp.frameSeq);
        result.sourcePixelDigest=SourcePixelsDigest(flat);
        result.reference=MakeProbeFrame(flat.constBits(),flat.width(),flat.height(),flat.bytesPerLine(),2,result.stamp);
        result.prepared=PrepareLines(result,7);
        return result;
    };
    auto first=make(100),second=make(200);
    QVERIFY(!second.prepared[0].reference.valid);
    OverlayModel model;
    QCOMPARE(model.Admit(first,first.stamp,nullptr,110),AdmitVerdict::kPending);
    QCOMPARE(model.Admit(second,second.stamp,nullptr,210),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(210,0),QStringLiteral("\u2026 Read page"));
    QVERIFY(model.SceneWords(210,0).isEmpty());
    QVERIFY(model.LineReferences(second.stamp).isEmpty());
    QCOMPARE(model.ReadingText(5000,second.stamp.frameSeq),QStringLiteral("\u2026 Read page"));
    QVERIFY(model.ReadingText(551,1).isEmpty());
    // An unrelated probe cannot renew text without a registration reference.
    model.Validate(second.reference,6000);
    QVERIFY(model.ReadingText(6000,1).isEmpty());
}

void UltraClearTextTests::rejectedRefreshCannotRestoreOldAcceptedText()
{
    const auto page=MakePage({QStringLiteral("Read page")});
    const auto t=Transform(1,.5f,.5f);
    OverlayModel model;
    model.Admit(ResultFor(page,t,Stamp(1,1,100,t)),Stamp(1,1,100,t),nullptr,110);
    model.Admit(ResultFor(page,t,Stamp(1,1,200,t)),Stamp(1,1,200,t),nullptr,210);
    QVERIFY(!model.SceneWords(210,0).isEmpty());
    auto rejected=ResultFor(page,t,Stamp(1,1,300,t));
    for(auto& word:rejected.output.lines[0].words) {
        word.disposition=TokenDisposition::kLowConfidence;word.confidence=.60;
    }
    QCOMPARE(model.Admit(rejected,rejected.stamp,nullptr,310),AdmitVerdict::kEmpty);
    QVERIFY(model.SceneWords(310,0).isEmpty());
    QCOMPARE(model.ReadingText(310,0),QStringLiteral("\u2026"));
    QCOMPARE(model.AcceptedWordCount(),0);
    auto empty=ResultFor(page,t,Stamp(1,1,400,t));empty.output.lines.clear();
    QCOMPARE(model.Admit(empty,empty.stamp,nullptr,410),AdmitVerdict::kEmpty);
    QVERIFY(model.ReadingText(410,0).isEmpty());
}

void UltraClearTextTests::immutableEvidenceRequiresDistinctPixelsAndSafeRegistration()
{
    const auto page=MakePage({QStringLiteral("Read page")});
    const auto t=Transform(1,.5f,.5f);
    auto stamp=Stamp(1,1,100,t);stamp.frameSeq=50;
    auto selected=ResultFor(page,t,stamp);
    const auto digest=[](const QImage& pixels) {
        return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(pixels.constBits()),
                                                     pixels.sizeInBytes()),QCryptographicHash::Sha256);
    };
    auto pixels=IndependentFramePixels(RenderView(page.image,t,kViewWidth,kViewHeight),selected.stamp.frameSeq);
    selected.sourcePixelDigest=digest(pixels);
    stamp.frameSeq=51;stamp.timestampMs=200;
    auto neighbor=ResultFor(page,t,stamp);
    auto differentPixels=pixels.copy();differentPixels.setPixelColor(0,0,Qt::black);
    neighbor.sourcePixelDigest=digest(differentPixels);
    neighbor.reference=MakeProbeFrame(differentPixels.constBits(),kViewWidth,kViewHeight,
                                      differentPixels.bytesPerLine(),2,stamp);
    ModelTuning tuning;tuning.admitMatchBudgetMs=1000;
    OverlayModel model(tuning);
    constexpr std::int64_t hour=60*60*1000;
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour),AdmitVerdict::kPending);
    QCOMPARE(model.ReadingText(hour,50),QStringLiteral("\u2026 Read page"));
    QVERIFY(model.SceneWords(hour,50).isEmpty());
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour+1),AdmitVerdict::kPending);
    QVERIFY(model.SceneWords(hour+1,50).isEmpty());
    auto duplicate=neighbor;duplicate.sourcePixelDigestKind=RecognitionResult::DigestKind::GpuTreeSha256V1;
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&duplicate),AdmitVerdict::kPending);
    duplicate=neighbor;duplicate.sourcePixelDigest=selected.sourcePixelDigest;
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&duplicate),AdmitVerdict::kPending);
    duplicate=neighbor;duplicate.stamp.frameSeq=50;
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&duplicate),AdmitVerdict::kPending);
    duplicate=neighbor;duplicate.authority=selected.authority+1;
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&duplicate),AdmitVerdict::kPending);
    auto unsafe=neighbor;
    for(auto& word:unsafe.output.lines[0].words)word.confidence=.60;
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&unsafe),AdmitVerdict::kPending);
    auto moved=MakePage({QStringLiteral("Different content")},QPointF(200,100));
    auto mismatched=neighbor;
    auto changedPixels=RenderView(moved.image,t,kViewWidth,kViewHeight);
    mismatched.sourcePixelDigest=digest(changedPixels);
    mismatched.reference=MakeProbeFrame(changedPixels.constBits(),kViewWidth,kViewHeight,
                                        changedPixels.bytesPerLine(),2,stamp);
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&mismatched),AdmitVerdict::kPending);
    // Resident checks must belong to this selected job, line count and policy.
    struct ResidentMarker final:ResidentProbe {
        int Width() const noexcept override{return 100;}
        int Height() const noexcept override{return 100;}
        ReferencePatch Reference(const SourceStamp&,const QRectF&) const override{return {};}
        PatchMatch Match(const SourceStamp&,const ReferencePatch&,const MatchTuning&,std::optional<QPointF>) const override{return {};}
        std::optional<double> Similarity(const SourceStamp&,const ResidentProbe&,const SourceStamp&) const override{return {};}
    };
    auto residentNeighbor=neighbor;
    residentNeighbor.prepared=PrepareLines(neighbor,tuning.minimumWordHeightPx);
    selected.prepared=PrepareLines(selected,tuning.minimumWordHeightPx);
    selected.jobId=99;
    residentNeighbor.reference.resident=std::make_shared<ResidentMarker>();
    residentNeighbor.corroborationMatches.fill(true,selected.prepared.size());
    residentNeighbor.corroboratedJobId=100;
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&residentNeighbor),AdmitVerdict::kPending);
    residentNeighbor.corroboratedJobId=selected.jobId;
    residentNeighbor.corroborationMatches.push_back(true);
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&residentNeighbor),AdmitVerdict::kPending);
    residentNeighbor.corroborationMatches.removeLast();
    residentNeighbor.corroborationTuning.minimumScore=.50;
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&residentNeighbor),AdmitVerdict::kPending);
    residentNeighbor.corroborationTuning=tuning.match;
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,hour,&residentNeighbor),AdmitVerdict::kAccepted);
    auto smoothedNeighbor=neighbor;
    smoothedNeighbor.stamp.temporallySmoothed=true;
    smoothedNeighbor.currentInputMatches.fill(false,neighbor.output.lines.size());
    OverlayModel rejectedNeighbor;
    QCOMPARE(rejectedNeighbor.AdmitImmutable(selected,selected.stamp,hour,&smoothedNeighbor),AdmitVerdict::kPending);
    QVERIFY(rejectedNeighbor.SceneWords(hour,selected.stamp.frameSeq).isEmpty());
    smoothedNeighbor.currentInputMatches.fill(true,neighbor.output.lines.size());
    QCOMPARE(rejectedNeighbor.AdmitImmutable(selected,selected.stamp,hour,&smoothedNeighbor),AdmitVerdict::kAccepted);
    OverlayModel cpuModel(tuning);
    QCOMPARE(cpuModel.AdmitImmutable(selected,selected.stamp,hour,&neighbor),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(hour,50),QStringLiteral("Read page"));
    QCOMPARE(model.SceneWords(hour,50).size(),2);
    for(const auto& word:model.SceneWords(hour,50))QVERIFY(word.protectedSceneRects.isEmpty());
    auto wrongFrame=selected.stamp;++wrongFrame.frameSeq;
    QCOMPARE(model.AdmitImmutable(selected,wrongFrame,hour,&neighbor),AdmitVerdict::kStaleScene);
    // Exact immutable evidence cannot corroborate the live policy.
    QCOMPARE(model.Admit(selected,selected.stamp,nullptr,110),AdmitVerdict::kPending);
    QVERIFY(model.SceneWords(110,50).isEmpty());
    QCOMPARE(model.Admit(selected,selected.stamp,nullptr,hour),AdmitVerdict::kTooOld);
}

void UltraClearTextTests::immutableCompletedRegistrationIgnoresUiBudget_data()
{
    QTest::addColumn<bool>("resident");
    QTest::newRow("cpu") << false;
    QTest::newRow("resident") << true;
}

void UltraClearTextTests::immutableCompletedRegistrationIgnoresUiBudget()
{
    QFETCH(bool, resident);
    const auto page=MakePage({"Millwright","Orchard Beacon","Motion previews"});
    const auto transform=Transform(1,.5f,.5f);
    auto stamp=Stamp(1,1,100,transform);stamp.frameSeq=10;
    auto selected=ResultFor(page,transform,stamp);selected.jobId=99;
    selected.prepared=PrepareLines(selected,7);
    selected.sourcePixelDigest=QByteArray(32,'a');
    stamp.frameSeq=11;stamp.timestampMs=134;
    auto neighbor=ResultFor(page,transform,stamp);
    neighbor.prepared=PrepareLines(neighbor,7);
    neighbor.sourcePixelDigest=QByteArray(32,'b');
    struct NoUiMatching final:ResidentProbe {
        int Width()const noexcept override{return 960;} int Height()const noexcept override{return 540;}
        ReferencePatch Reference(const SourceStamp&,const QRectF&)const override{QTest::qFail("UI reference sampling",__FILE__,__LINE__);return {};}
        PatchMatch Match(const SourceStamp&,const ReferencePatch&,const MatchTuning&,std::optional<QPointF>)const override{QTest::qFail("UI pixel matching",__FILE__,__LINE__);return {};}
        std::optional<double> Similarity(const SourceStamp&,const ResidentProbe&,const SourceStamp&)const override{return {};}
    };
    if(resident)neighbor.reference.resident=std::make_shared<NoUiMatching>();
    neighbor.corroboratedJobId=selected.jobId;
    neighbor.corroborationMatches.fill(true,selected.prepared.size());
    ModelTuning tuning;tuning.admitMatchBudgetMs=0;
    neighbor.corroborationTuning=tuning.match;
    OverlayModel model(tuning);
    QCOMPARE(model.AdmitImmutable(selected,selected.stamp,500,&neighbor),AdmitVerdict::kAccepted);
    QCOMPARE(model.SceneWords(500,10).size(),page.words.size());
    for(int fault=0;fault<3;++fault) {
        auto bad=neighbor;
        if(fault==0)++bad.corroboratedJobId;
        if(fault==1)bad.corroborationMatches.removeLast();
        if(fault==2)bad.corroborationTuning.minimumScore=.5;
        OverlayModel rejected(tuning);
        QCOMPARE(rejected.AdmitImmutable(selected,selected.stamp,500,&bad),AdmitVerdict::kPending);
        QVERIFY(rejected.SceneWords(500,10).isEmpty());
    }
}

namespace {
// Completed worker receipts, with real distinct synthetic source pixels. The
// core fixture exercises admission/retention, never an extra OCR acquisition.
std::pair<RecognitionResult, RecognitionResult> ImmutablePair(const SyntheticPage& page)
{
    const auto transform = Transform(1, .5f, .5f);
    auto stamp = Stamp(1, 1, 55'000, transform); stamp.frameSeq = 55;
    auto selected = ResultFor(page, transform, stamp); selected.jobId = 101; selected.authority = 7;
    selected.prepared = PrepareLines(selected, 7);
    stamp.frameSeq = 56; stamp.timestampMs = 55'033;
    auto neighbor = ResultFor(page, transform, stamp); neighbor.authority = selected.authority;
    neighbor.prepared = PrepareLines(neighbor, 7);
    neighbor.corroboratedJobId = selected.jobId;
    neighbor.corroborationTuning = ModelTuning{}.match;
    neighbor.corroborationMatches.fill(false, selected.prepared.size());
    for (const auto& line : selected.prepared)
        neighbor.corroborationWordMatches.push_back(QVector<bool>(line.words.size(), true));
    return {selected, neighbor};
}
}

void UltraClearTextTests::immutableRefinementRetainsExactProvenCoverage_data()
{
    QTest::addColumn<bool>("selectedOmission");
    QTest::newRow("selected-omits-three") << true;
    QTest::newRow("neighbor-proves-one") << false;
}

void UltraClearTextTests::immutableRefinementRetainsExactProvenCoverage()
{
    QFETCH(bool, selectedOmission);
    auto [selected, neighbor] = ImmutablePair(MakePage({"Reading glasses help people"}));
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto prior = model.Lines().first();
    const auto reading = model.ReadingText(60'000, 55);
    auto refinement = selected;
    if (selectedOmission) {
        refinement.output.lines[0].words.resize(1);
        refinement.prepared = PrepareLines(refinement, 7);
        // OCR-box jitter is not authority to move an already proved glyph.
        refinement.prepared[0].words[0].sceneRect.translate(.001, .001);
        neighbor.corroborationWordMatches[0].resize(1);
    } else {
        neighbor.corroborationWordMatches[0].fill(false);
        neighbor.corroborationWordMatches[0][0] = true;
    }
    QCOMPARE(model.AdmitImmutable(refinement, refinement.stamp, 70'000, &neighbor), AdmitVerdict::kAccepted);
    QCOMPARE(model.SceneWords(70'000, 55).size(), 4);
    QCOMPARE(model.ReadingText(70'000, 55), reading);
    QCOMPARE(model.ScreenLines(70'000, 55).size(), 1);
    QCOMPARE(model.ScreenLines(70'000, 55).first().text, reading);
    QCOMPARE(model.Lines().size(), 1);
    const auto& kept = model.Lines().first();
    QCOMPARE(kept.id, prior.id); QCOMPARE(kept.sceneRect, prior.sceneRect);
    QCOMPARE(kept.reference.sceneRect, prior.reference.sceneRect);
    QCOMPARE(kept.reference.values, prior.reference.values);
    QCOMPARE(kept.reference.resident, prior.reference.resident);
    QCOMPARE(kept.provenance.frameSeq, prior.provenance.frameSeq);
    QCOMPARE(kept.provenance.timestampMs, prior.provenance.timestampMs);
    QCOMPARE(kept.validatedAtMs, prior.validatedAtMs);
    QCOMPARE(kept.validatedFrameSeq, prior.validatedFrameSeq);
    for (int i = 0; i < prior.words.size(); ++i) {
        QCOMPARE(kept.words[i].sceneRect, prior.words[i].sceneRect);
        QCOMPARE(model.SceneWords(70'000, 55)[i].wordIndex, i);
    }
}

void UltraClearTextTests::immutableRefinementAddsOwnProofWithoutDuplicateReading()
{
    auto [selected, neighbor] = ImmutablePair(MakePage({"Reading glasses help people", "New disjoint words"}));
    neighbor.corroborationWordMatches[0].fill(false);
    neighbor.corroborationWordMatches[0][0] = true;
    neighbor.corroborationWordMatches[1].fill(false);
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto prior = model.Lines().first();
    QCOMPARE(model.SceneWords(60'000, 55).size(), 1);
    // Repeating the selected observation cannot promote its reading-only words.
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 61'000), AdmitVerdict::kAccepted);
    QCOMPARE(model.SceneWords(61'000, 55).size(), 1);
    for (auto& line : neighbor.corroborationWordMatches) line.fill(true);
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 62'000, &neighbor), AdmitVerdict::kAccepted);
    const auto words = model.SceneWords(62'000, 55);
    QCOMPARE(words.size(), 7);
    QCOMPARE(model.AcceptedWordCount(), 7);
    QCOMPARE(model.ReadingText(62'000, 55), QStringLiteral("Reading glasses help people\nNew disjoint words"));
    const auto screen = model.ScreenLines(62'000, 55);
    QCOMPARE(screen.size(), 2);
    QCOMPARE(screen[0].text, QStringLiteral("Reading glasses help people"));
    QCOMPARE(screen[1].text, QStringLiteral("New disjoint words"));
    QCOMPARE(model.Lines().first().id, prior.id);
    QCOMPARE(model.Lines().first().words[0].sceneRect, prior.words[0].sceneRect);
    QCOMPARE(model.Lines().first().reference.values, prior.reference.values);
    QCOMPARE(model.Lines().first().validatedAtMs, prior.validatedAtMs);
    QSet<std::pair<std::uint64_t, int>> identities;
    for (const auto& word : words) {
        QVERIFY(!identities.contains({word.lineId, word.wordIndex}));
        identities.insert({word.lineId, word.wordIndex});
        for (const auto& protectedRect : word.protectedSceneRects)
            QVERIFY(!protectedRect.intersects(word.sceneRect));
    }
    for (int repeat = 0; repeat < 8; ++repeat) {
        QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 63'000 + repeat, &neighbor), AdmitVerdict::kAccepted);
        QCOMPARE(model.SceneWords(64'000, 55).size(), 7);
        QCOMPARE(model.ReadingText(64'000, 55), QStringLiteral("Reading glasses help people\nNew disjoint words"));
        QCOMPARE(model.Lines().first().words.size(), 7); // four originals plus three appended proofs
    }
}

void UltraClearTextTests::immutableRefinementCannotCarryProofAcrossIdentity_data()
{
    QTest::addColumn<int>("changed");
    QTest::newRow("source") << 0; QTest::newRow("geometry") << 1;
    QTest::newRow("frame") << 2; QTest::newRow("pts") << 3;
    QTest::newRow("authority") << 4; QTest::newRow("pixel-digest") << 5;
    QTest::newRow("digest-kind") << 6; QTest::newRow("missing-digest") << 7;
}

void UltraClearTextTests::immutableRefinementCannotCarryProofAcrossIdentity()
{
    QFETCH(int, changed);
    auto [selected, neighbor] = ImmutablePair(MakePage({"Reading glasses help people"}));
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto oldId = model.Lines().first().id;
    if (changed == 0) { ++selected.stamp.sourceEpoch; ++neighbor.stamp.sourceEpoch; }
    if (changed == 1) { ++selected.stamp.geometryGeneration; ++neighbor.stamp.geometryGeneration; }
    if (changed == 2) { selected.stamp.frameSeq += 2; neighbor.stamp.frameSeq += 2; }
    if (changed == 3) ++selected.stamp.timestampMs;
    if (changed == 4) { ++selected.authority; ++neighbor.authority; }
    if (changed == 5) selected.sourcePixelDigest[0] ^= 1;
    if (changed == 6) selected.sourcePixelDigestKind = neighbor.sourcePixelDigestKind = RecognitionResult::DigestKind::GpuTreeSha256V1;
    if (changed == 7) selected.sourcePixelDigest.clear();
    selected.reference.stamp = selected.stamp; neighbor.reference.stamp = neighbor.stamp;
    selected.output.lines[0].words.resize(1); selected.prepared = PrepareLines(selected, 7);
    neighbor.corroborationWordMatches[0].resize(1);
    const auto verdict = model.AdmitImmutable(selected, selected.stamp, 70'000, &neighbor);
    QCOMPARE(verdict, changed == 7 ? AdmitVerdict::kPending : AdmitVerdict::kAccepted);
    QCOMPARE(model.SceneWords(70'000, selected.stamp.frameSeq).size(), changed == 7 ? 0 : 1);
    QVERIFY(model.Lines().isEmpty() || model.Lines().first().id != oldId);
}

void UltraClearTextTests::immutableRefinementContradictionRevokesAffectedProof()
{
    auto [selected, neighbor] = ImmutablePair(MakePage({"Reading glasses", "Safe distant text"}));
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto first = model.Lines()[0], safe = model.Lines()[1];
    selected.output.lines[0].words[0].text = QStringLiteral("Writing");
    neighbor.output.lines[0].words[0].text = QStringLiteral("Writing");
    selected.prepared = PrepareLines(selected, 7); neighbor.prepared = PrepareLines(neighbor, 7);
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 70'000, &neighbor), AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(70'000, 55), QStringLiteral("\u2026 glasses\nSafe distant text"));
    QCOMPARE(model.SceneWords(70'000, 55).size(), 4);
    QCOMPARE(model.Lines().first().id, first.id);
    QCOMPARE(model.SceneWords(70'000, 55).first().wordIndex, 1);
    QCOMPARE(model.Lines()[1].id, safe.id);
    // No unrelated proof to keep: the explicit conflict is observable and empty.
    auto pair = ImmutablePair(MakePage({"Reading glasses"}));
    OverlayModel conflictOnly;
    QCOMPARE(conflictOnly.AdmitImmutable(pair.first, pair.first.stamp, 60'000, &pair.second), AdmitVerdict::kAccepted);
    pair.first.output.lines[0].words[0].text = QStringLiteral("Writing");
    pair.second.output.lines[0].words[0].text = QStringLiteral("Writing");
    pair.first.output.lines[0].words[1].text = QStringLiteral("goggles");
    pair.second.output.lines[0].words[1].text = QStringLiteral("goggles");
    pair.first.prepared = PrepareLines(pair.first, 7); pair.second.prepared = PrepareLines(pair.second, 7);
    QCOMPARE(conflictOnly.AdmitImmutable(pair.first, pair.first.stamp, 70'000, &pair.second), AdmitVerdict::kContentMoved);
    QVERIFY(conflictOnly.SceneWords(70'000, 55).isEmpty());
    QVERIFY(conflictOnly.ReadingText(70'000, 55).isEmpty());
    QVERIFY(conflictOnly.ScreenLines(70'000, 55).isEmpty());
}

void UltraClearTextTests::immutableWordConflictKeepsSafeSibling_data()
{
    QTest::addColumn<QString>("oldText"); QTest::addColumn<QString>("newText");
    QTest::newRow("punctuation") << QStringLiteral("Hello,") << QStringLiteral("Hello");
    QTest::newRow("case") << QStringLiteral("Read") << QStringLiteral("read");
    QTest::newRow("rn-m") << QStringLiteral("modern") << QStringLiteral("modem");
}

void UltraClearTextTests::immutableWordConflictKeepsSafeSibling()
{
    QFETCH(QString, oldText); QFETCH(QString, newText);
    auto [selected, neighbor] = ImmutablePair(MakePage({oldText + QStringLiteral(" Safe")}));
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto prior = model.Lines().first();
    const auto oldRect = prior.words[0].sceneRect;
    selected.output.lines[0].words[0].text = newText;
    neighbor.output.lines[0].words[0].text = newText;
    selected.prepared = PrepareLines(selected, 7); neighbor.prepared = PrepareLines(neighbor, 7);
    selected.prepared[0].words[0].sceneRect.translate(.001, 0);
    const auto newRect = selected.prepared[0].words[0].sceneRect;
    for (int repeat = 0; repeat < 4; ++repeat) {
        QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 70'000 + repeat, &neighbor), AdmitVerdict::kAccepted);
        const auto words = model.SceneWords(70'000 + repeat, 55);
        QCOMPARE(words.size(), 1); QCOMPARE(words[0].text, QStringLiteral("Safe"));
        QCOMPARE(model.AcceptedWordCount(), 1);
        QCOMPARE(words[0].protectedSceneRects.size(), 2); // repeated pairs cannot accumulate block history
        QCOMPARE(words[0].lineId, prior.id); QCOMPARE(words[0].wordIndex, 1);
        QCOMPARE(words[0].sceneRect, prior.words[1].sceneRect);
        QCOMPARE(model.ReadingText(70'000 + repeat, 55), QStringLiteral("\u2026 Safe"));
        const auto screen = model.ScreenLines(70'000 + repeat, 55);
        QCOMPARE(screen.size(), 1); QCOMPARE(screen[0].lineId, prior.id);
        QCOMPARE(screen[0].text, QStringLiteral("\u2026 Safe"));
        QCOMPARE(screen[0].tokens.last().wordIndex, 1);
        const auto& kept = model.Lines().first();
        QCOMPARE(kept.sceneRect, prior.sceneRect); QCOMPARE(kept.reference.values, prior.reference.values);
        QCOMPARE(kept.provenance.frameSeq, prior.provenance.frameSeq);
        QCOMPARE(kept.provenance.timestampMs, prior.provenance.timestampMs);
        QCOMPARE(kept.validatedAtMs, prior.validatedAtMs); QCOMPARE(kept.validatedFrameSeq, prior.validatedFrameSeq);
        for (const auto& rect : {oldRect, newRect})
            QVERIFY(std::any_of(words[0].protectedSceneRects.begin(), words[0].protectedSceneRects.end(),
                [&](const auto& blocked) { return blocked.contains(rect); }));
        // A failed foreign result cannot erase the still-selected input's
        // conflict history and thereby permit the next same-source revival.
        auto failed = selected; ++failed.stamp.sourceEpoch; failed.output.ok = false;
        QCOMPARE(model.AdmitImmutable(failed, failed.stamp, 70'000 + repeat), AdmitVerdict::kFailed);
    }
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 80'000), AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(80'000, 55), QStringLiteral("\u2026 Safe"));
}

void UltraClearTextTests::immutableConflictBlocksFollowExactSelectedIdentity_data()
{
    QTest::addColumn<int>("changed");
    QTest::newRow("source") << 0; QTest::newRow("geometry") << 1;
    QTest::newRow("frame") << 2; QTest::newRow("pts") << 3;
    QTest::newRow("authority") << 4; QTest::newRow("pixel-digest") << 5;
    QTest::newRow("digest-kind") << 6; QTest::newRow("explicit-reset") << 7;
    QTest::newRow("temporal-history") << 8;
}

void UltraClearTextTests::immutableConflictBlocksFollowExactSelectedIdentity()
{
    QFETCH(int, changed);
    auto [selected, neighbor] = ImmutablePair(MakePage({"Reading glasses"}));
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto oldId = model.Lines().first().id;
    selected.output.lines[0].words[0].text = neighbor.output.lines[0].words[0].text = QStringLiteral("Writing");
    selected.output.lines[0].words[1].text = neighbor.output.lines[0].words[1].text = QStringLiteral("goggles");
    selected.prepared = PrepareLines(selected, 7); neighbor.prepared = PrepareLines(neighbor, 7);
    for (int repeat = 0; repeat < 3; ++repeat) {
        QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 70'000 + repeat, &neighbor), AdmitVerdict::kContentMoved);
        QVERIFY(model.SceneWords(70'000 + repeat, 55).isEmpty());
        QVERIFY(model.ReadingText(70'000 + repeat, 55).isEmpty());
        QVERIFY(model.ScreenLines(70'000 + repeat, 55).isEmpty());
    }
    if (changed == 0) { ++selected.stamp.sourceEpoch; ++neighbor.stamp.sourceEpoch; }
    if (changed == 1) { ++selected.stamp.geometryGeneration; ++neighbor.stamp.geometryGeneration; }
    if (changed == 2) { selected.stamp.frameSeq += 2; neighbor.stamp.frameSeq += 2; }
    if (changed == 3) ++selected.stamp.timestampMs;
    if (changed == 4) { ++selected.authority; ++neighbor.authority; }
    if (changed == 5) selected.sourcePixelDigest[0] ^= 1;
    if (changed == 6) selected.sourcePixelDigestKind = neighbor.sourcePixelDigestKind = RecognitionResult::DigestKind::GpuTreeSha256V1;
    if (changed == 7) model.Reset();
    if (changed == 8) ++selected.stamp.historyEpoch;
    selected.reference.stamp = selected.stamp; neighbor.reference.stamp = neighbor.stamp;
    selected.prepared = PrepareLines(selected, 7); neighbor.prepared = PrepareLines(neighbor, 7);
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 80'000, &neighbor), AdmitVerdict::kAccepted);
    QCOMPARE(model.SceneWords(80'000, selected.stamp.frameSeq).size(), 2);
    QCOMPARE(model.ReadingText(80'000, selected.stamp.frameSeq), QStringLiteral("Writing goggles"));
    QVERIFY(model.Lines().first().id != oldId);
    for (const auto& word : model.SceneWords(80'000, selected.stamp.frameSeq)) QVERIFY(word.protectedSceneRects.isEmpty());
}

void UltraClearTextTests::immutableRefinementUnionIsBounded()
{
    auto [selected, neighbor] = ImmutablePair(MakePage({"Reading"}));
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto oldId = model.Lines().first().id;
    // The receipt is intentionally dense; no model/runtime or large image is
    // involved. Immutable refinement must cap its retained metadata too.
    selected.output.lines.clear(); selected.prepared.clear();
    neighbor.output.lines.clear(); neighbor.prepared.clear();
    neighbor.corroborationMatches.clear(); neighbor.corroborationWordMatches.clear();
    for (int lineIndex = 0; lineIndex < 1024; ++lineIndex) {
        RecognizedLine line;
        PreparedLine prepared;
        const QRectF rect(.01, .2 + lineIndex * .0007, .8, .0005);
        prepared.sceneRect = rect; prepared.reference.valid = true; prepared.reference.sceneRect = rect;
        for (int word = 0; word < 8; ++word) {
            const QRectF bounds(.01 + word * .1, rect.top(), .08, rect.height());
            line.words.push_back({QStringLiteral("Text"), bounds});
            prepared.words.push_back({QStringLiteral("Text"), bounds, TokenDisposition::kCandidate, 1, bounds});
        }
        selected.output.lines.push_back(line); neighbor.output.lines.push_back(line);
        selected.prepared.push_back(prepared); neighbor.prepared.push_back(prepared);
        neighbor.corroborationMatches.push_back(true);
        neighbor.corroborationWordMatches.push_back(QVector<bool>(8, true));
    }
    for (int repeat = 0; repeat < 3; ++repeat) {
        QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 61'000 + repeat, &neighbor), AdmitVerdict::kAccepted);
        QVERIFY(model.Lines().size() <= 1024);
        qsizetype words = 0, tokens = 0, protectedRects = 0;
        for (const auto& line : model.Lines()) {
            words += line.words.size(); tokens += line.readingTokens.size();
            protectedRects += line.protectedSceneRects.size();
        }
        QVERIFY(words <= 8192); QVERIFY(tokens <= 32768); QVERIFY(protectedRects <= 32768);
        QVERIFY(std::any_of(model.Lines().begin(), model.Lines().end(), [&](const auto& line) { return line.id == oldId; }));
    }
}

void UltraClearTextTests::immutableRefinementLiteralGroupingKeepsPriorProof_data()
{
    QTest::addColumn<bool>("split");
    QTest::newRow("unsafe-grouped-token-cannot-erase-proved-words") << false;
    QTest::newRow("safe-split-tokens-follow-rejected-grouped-token") << true;
}

void UltraClearTextTests::immutableRefinementLiteralGroupingKeepsPriorProof()
{
    QFETCH(bool, split);
    auto original = ImmutablePair(MakePage({"Reading glasses"}));
    auto grouped = original;
    for (auto* result : {&grouped.first, &grouped.second}) {
        auto merged = result->output.lines[0].words[0];
        merged.text = QStringLiteral("Reading glasses");
        merged.boundsPx = merged.boundsPx.united(result->output.lines[0].words[1].boundsPx);
        result->output.lines[0].words = {merged};
        result->prepared = PrepareLines(*result, 7);
    }
    grouped.second.corroborationWordMatches[0] = {true};
    auto initial = split ? grouped : original;
    auto refinement = split ? original : grouped;
    OverlayModel model;
    if (split) {
        // An OCR "word" containing a space is outside the reviewed token
        // alphabet. It cannot establish prior glyph proof, even when a fake
        // worker supplies a positive match. Ordinary individual words can
        // subsequently gain their own independent selected/neighbor proof.
        QVERIFY(IsHighRiskToken(initial.first.output.lines[0].words[0].text));
        QCOMPARE(model.AdmitImmutable(initial.first, initial.first.stamp, 60'000, &initial.second), AdmitVerdict::kPending);
        QVERIFY(model.SceneWords(60'000, 55).isEmpty());
        QVERIFY(model.ScreenLines(60'000, 55).isEmpty());
        QCOMPARE(model.ReadingText(60'000, 55), QStringLiteral("\u2026"));
        QCOMPARE(model.AdmitImmutable(refinement.first, refinement.first.stamp, 70'000, &refinement.second), AdmitVerdict::kAccepted);
        QCOMPARE(model.SceneWords(70'000, 55).size(), 2);
        QCOMPARE(model.ReadingText(70'000, 55), QStringLiteral("Reading glasses"));
        QCOMPARE(model.ScreenLines(70'000, 55).first().text, QStringLiteral("Reading glasses"));
        return;
    }
    QCOMPARE(model.AdmitImmutable(initial.first, initial.first.stamp, 60'000, &initial.second), AdmitVerdict::kAccepted);
    const auto prior = model.Lines().first();
    const auto words = model.SceneWords(60'000, 55);
    QCOMPARE(model.AdmitImmutable(refinement.first, refinement.first.stamp, 70'000, &refinement.second), AdmitVerdict::kAccepted);
    QCOMPARE(model.Lines().size(), 1);
    QCOMPARE(model.SceneWords(70'000, 55).size(), words.size());
    QCOMPARE(model.ReadingText(70'000, 55), QStringLiteral("Reading glasses"));
    QCOMPARE(model.ScreenLines(70'000, 55).first().text, QStringLiteral("Reading glasses"));
    QCOMPARE(model.Lines().first().id, prior.id);
    QCOMPARE(model.Lines().first().reference.values, prior.reference.values);
    for (int i = 0; i < words.size(); ++i) {
        const auto kept = model.SceneWords(70'000, 55)[i];
        QCOMPARE(kept.sceneRect, words[i].sceneRect); QCOMPARE(kept.wordIndex, words[i].wordIndex);
    }
}

void UltraClearTextTests::immutableRefinementKeepsMeaningfulWhitespaceDistinct()
{
    auto [selected, neighbor] = ImmutablePair(MakePage({"nowhere"}));
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    for (auto* result : {&selected, &neighbor}) {
        const auto prior = result->output.lines[0].words[0];
        auto left = prior, right = prior;
        left.text = QStringLiteral("now"); right.text = QStringLiteral("here");
        left.boundsPx.setWidth(prior.boundsPx.width() * .45);
        right.boundsPx.setLeft(left.boundsPx.right());
        result->output.lines[0].words = {left, right};
        result->prepared = PrepareLines(*result, 7);
    }
    neighbor.corroborationWordMatches[0] = {true, true};
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 70'000, &neighbor), AdmitVerdict::kContentMoved);
    QVERIFY(model.SceneWords(70'000, 55).isEmpty());
    QVERIFY(model.ReadingText(70'000, 55).isEmpty());
    QVERIFY(model.ScreenLines(70'000, 55).isEmpty());
}

void UltraClearTextTests::immutableRefinementBoundaryWordUsesIncomingProof_data()
{
    QTest::addColumn<bool>("proved");
    QTest::newRow("own-incoming-neighbor-proof") << true;
    QTest::newRow("reading-only-cannot-promote") << false;
}

void UltraClearTextTests::immutableRefinementBoundaryWordUsesIncomingProof()
{
    QFETCH(bool, proved);
    auto [selected, neighbor] = ImmutablePair(MakePage({"Reading glasses"}));
    neighbor.corroborationWordMatches[0] = {true, false};
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto prior = model.Lines().first();
    auto refinement = selected;
    refinement.prepared[0].words[1].sceneRect.translate(.001, 0);
    refinement.prepared[0].sceneRect = refinement.prepared[0].sceneRect.united(refinement.prepared[0].words[1].sceneRect);
    neighbor.corroborationWordMatches[0] = {false, proved};
    QCOMPARE(model.AdmitImmutable(refinement, refinement.stamp, 70'000, &neighbor), AdmitVerdict::kAccepted);
    const auto words = model.SceneWords(70'000, 55);
    QCOMPARE(words.size(), proved ? 2 : 1);
    QCOMPARE(model.ReadingText(70'000, 55).count(QStringLiteral("Reading")), 1);
    QCOMPARE(model.ReadingText(70'000, 55).count(QStringLiteral("glasses")), 1);
    QCOMPARE(words.first().lineId, prior.id); QCOMPARE(words.first().wordIndex, 0);
    QCOMPARE(words.first().sceneRect, prior.words[0].sceneRect);
    QCOMPARE(model.Lines().first().reference.values, prior.reference.values);
    QCOMPARE(model.Lines().first().validatedAtMs, prior.validatedAtMs);
    if (proved) {
        QCOMPARE(words[1].text, QStringLiteral("glasses"));
        QCOMPARE(words[1].sceneRect, refinement.prepared[0].words[1].sceneRect);
        QVERIFY(words[1].lineId != prior.id);
        QCOMPARE(model.Lines()[1].reference.values, refinement.prepared[0].reference.values);
        int spoken = 0;
        for (const auto& line : model.ScreenLines(70'000, 55))
            for (const auto& token : line.tokens) if (token.wordIndex >= 0) ++spoken;
        QCOMPARE(spoken, 2);
        for (const auto& rect : words[1].protectedSceneRects) QVERIFY(!rect.intersects(words[1].sceneRect));
    }
}

void UltraClearTextTests::immutableRefinementAdmitsAdjacentWordWithSmallOverlap()
{
    auto [selected, neighbor] = ImmutablePair(MakePage({"Reading glasses"}));
    neighbor.corroborationWordMatches[0] = {true, false};
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto prior = model.SceneWords(60'000, 55).first();
    auto refinement = selected;
    auto& next = refinement.prepared[0].words[1].sceneRect;
    next.moveLeft(refinement.prepared[0].words[0].sceneRect.right() - .0005);
    neighbor.corroborationWordMatches[0] = {false, true};
    QCOMPARE(model.AdmitImmutable(refinement, refinement.stamp, 70'000, &neighbor), AdmitVerdict::kAccepted);
    const auto words = model.SceneWords(70'000, 55);
    QCOMPARE(words.size(), 2);
    QCOMPARE(words[0].lineId, prior.lineId); QCOMPARE(words[0].wordIndex, prior.wordIndex);
    QCOMPARE(words[0].sceneRect, prior.sceneRect); QCOMPARE(words[1].sceneRect, next);
    QCOMPARE(model.ReadingText(70'000, 55).count(QStringLiteral("glasses")), 1);
    const auto screen = model.ScreenLines(70'000, 55);
    int spoken = 0;
    for (const auto& line : screen)
        for (const auto& token : line.tokens) if (token.wordIndex >= 0) ++spoken;
    QCOMPARE(spoken, 2);
}

void UltraClearTextTests::protectedAdjacentInkSurvivesCompositing_data()
{
    QTest::addColumn<QString>("notation");QTest::addColumn<QRectF>("protectedPx");QTest::addColumn<int>("scale");
    for(const int scale:{1,2}) {
        const auto add=[&](const char* name,const QString& text,const QRectF& r) {
            QTest::newRow(qPrintable(QString::fromLatin1(name)+QString::number(scale)))<<text<<r<<scale;
        };
        add("minus-",QStringLiteral("-"),QRectF(242,90,4,2));
        add("exponent-",QStringLiteral("\u00b2"),QRectF(242,57,4,4));
        add("decimal-",QStringLiteral("."),QRectF(242,118,2,2));
        add("punctuation-",QStringLiteral(","),QRectF(242,80,2,4));
    }
}

void UltraClearTextTests::protectedAdjacentInkSurvivesCompositing()
{
    QFETCH(QString,notation);QFETCH(QRectF,protectedPx);QFETCH(int,scale);
    const QRectF acceptedPx(80,60,160,60);
    QImage source(960,540,QImage::Format_ARGB32);source.fill(QColor(246,244,236));
    QPainter ink(&source);ink.setFont(PageFont(56));ink.setPen(Qt::black);
    ink.drawText(acceptedPx,Qt::AlignCenter,QStringLiteral("READ"));
    ink.fillRect(protectedPx,QColor(191,17,39));ink.end();
    const auto transform=ComputeViewTransform(960,540,960,540,1,.5f,.5f,ViewportFitMode::kFit);
    const auto result=[&](std::int64_t time) {
        RecognitionResult r;r.output.ok=true;r.stamp=Stamp(1,1,time,transform);r.stamp.frameSeq=time;
        const auto observed=IndependentFramePixels(source,r.stamp.frameSeq);
        r.sourcePixelDigest=SourcePixelsDigest(observed);
        r.output.lines.push_back({{{QStringLiteral("READ"),acceptedPx},{notation,protectedPx}}});
        r.reference=MakeProbeFrame(observed.constBits(),960,540,observed.bytesPerLine(),1,r.stamp);
        return r;
    };
    OverlayModel model;
    const auto first=result(100),second=result(200);
    model.Admit(first,first.stamp,nullptr,110);
    QCOMPARE(model.Admit(second,second.stamp,nullptr,210),AdmitVerdict::kAccepted);
    QCOMPARE(model.ReadingText(210,0),QStringLiteral("READ \u2026"));
    const auto sceneWords=model.SceneWords(210,0);QCOMPARE(sceneWords.size(),1);
    QWidget target;target.resize(960*scale,540*scale);
    UltraClearTextOverlay overlay(&target,&target);overlay.resize(target.size());
    overlay.SetViewTransform(transform);
    const auto& word=sceneWords[0];
    overlay.SetWords({{word.text,word.sceneRect,word.colors,word.lineId,{}}});
    QImage unprotected(target.size(),QImage::Format_ARGB32_Premultiplied);unprotected.fill(Qt::transparent);
    overlay.render(&unprotected);
    const QRect probe=QRectF(protectedPx.x()*scale,protectedPx.y()*scale,
                             protectedPx.width()*scale,protectedPx.height()*scale).toAlignedRect();
    QVERIFY2(qAlpha(unprotected.pixel(probe.center()))>200,"Fixture did not reproduce plate coverage of adjacent ink");
    overlay.SetWords({{word.text,word.sceneRect,word.colors,word.lineId,word.protectedSceneRects}});
    QImage layer(target.size(),QImage::Format_ARGB32_Premultiplied);layer.fill(Qt::transparent);overlay.render(&layer);
    QImage original=RenderView(source,transform,target.width(),target.height());
    QImage composite=original.copy();QPainter painter(&composite);painter.drawImage(0,0,layer);painter.end();
    for(int y=probe.top();y<=probe.bottom();++y)for(int x=probe.left();x<=probe.right();++x) {
        QCOMPARE(qAlpha(layer.pixel(x,y)),0);
        QCOMPARE(composite.pixel(x,y),original.pixel(x,y));
    }
    int replacementPixels=0;
    for(int y=60*scale;y<120*scale;++y)for(int x=80*scale;x<240*scale;++x)
        replacementPixels+=qAlpha(layer.pixel(x,y))>0;
    QVERIFY(replacementPixels>100);
}

void UltraClearTextTests::schedulerPixelEvidenceIgnoresAllocationPadding()
{
    auto shared=std::make_shared<HeldRecognizer::Shared>();shared->release=true;
    std::mutex mutex;QVector<RecognitionResult> results;
    RecognitionScheduler scheduler(std::make_unique<HeldRecognizer>(shared),
        [&](RecognitionResult result) {std::lock_guard lock(mutex);results.push_back(std::move(result));},
        [](BackendStatus){},1);
    QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
    QImage source(16,16,QImage::Format_ARGB32);source.fill(QColor(10,20,30));
    const auto pixels=[&](std::uint8_t padding) {
        auto buffer=std::make_shared<PixelBuffer>();buffer->width=16;buffer->height=16;buffer->strideBytes=80;
        buffer->bytes.resize(80*16,padding);
        for(int y=0;y<16;++y)std::memcpy(buffer->bytes.data()+y*80,source.constScanLine(y),16*4);
        return SharedPixels(buffer);
    };
    ViewTransform transform;transform.valid=true;
    SourceStamp stamp;stamp.sourceEpoch=1;stamp.geometryGeneration=1;stamp.frameSeq=1;
    stamp.viewportWidth=16;stamp.viewportHeight=16;stamp.viewTransform=transform;
    QVERIFY(scheduler.Submit(stamp,pixels(13))!=0);
    QTRY_VERIFY(([&]{std::lock_guard lock(mutex);return results.size()==1;})());
    ++stamp.frameSeq;
    QVERIFY(scheduler.Submit(stamp,pixels(91))!=0);
    QTRY_VERIFY(([&]{std::lock_guard lock(mutex);return results.size()==2;})());
    // The same scheduler skips the full-image digest for trusted live capture.
    ++stamp.frameSeq;stamp.liveCameraCapture=true;
    QVERIFY(scheduler.Submit(stamp,pixels(52))!=0);
    QTRY_VERIFY(([&]{std::lock_guard lock(mutex);return results.size()==3;})());
    const auto expected=QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(source.constBits()),
                                                              source.sizeInBytes()),QCryptographicHash::Sha256);
    std::lock_guard lock(mutex);
    QCOMPARE(results[0].sourcePixelDigest.size(),32);
    QCOMPARE(results[0].sourcePixelDigest,expected);
    QCOMPARE(results[1].sourcePixelDigest,expected);
    QVERIFY(results[0].stamp.frameSeq!=results[1].stamp.frameSeq);
    QVERIFY(results[2].stamp.liveCameraCapture);
    QVERIFY(results[2].sourcePixelDigest.isEmpty());
    QVERIFY(results[2].reference.IsValid()); // residual validation still receives its pixels
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

void UltraClearTextTests::patchKeepsDenseZeroWhenSparseDistractorWins_data()
{
    QTest::addColumn<bool>("stationary");
    QTest::newRow("preserve-dense-zero") << true;
    QTest::newRow("refine-distant-coarse-neighbor") << false;
}

void UltraClearTextTests::patchKeepsDenseZeroWhenSparseDistractorWins()
{
    QFETCH(bool, stationary);
    QImage image(40, 32, QImage::Format_ARGB32);
    image.fill(QColor(128, 128, 128));
    ReferencePatch patch;
    patch.valid = true;
    patch.columns = 24; patch.rows = 1;
    patch.sceneRect = QRectF(8.0 / 40, 16.0 / 32, 24.0 / 40, 1.0 / 32);
    for (int i = 0; i < 24; ++i) {
        const int a = i % 2 == 0 ? 1 : -1;
        const int b = i % 4 < 2 ? 1 : -1;
        patch.values.push_back(static_cast<float>(a));
        const int zero = stationary ? 128 + 40 * a + 24 * b : 128;
        image.setPixelColor(8 + i, 16, QColor(zero, zero, zero));
        const int distractor = 128 + (i % 3 == 0 ? 40 : -40) * a;
        image.setPixelColor(8 + i, 10, QColor(distractor, distractor, distractor));
        if (!stationary) {
            const int moved = 128 + 40 * a;
            image.setPixelColor(8 + i, 11, QColor(moved, moved, moved));
        }
    }
    const auto transform = ComputeViewTransform(40, 32, 40, 32, 1.0f, 0.5f, 0.5f,
                                                ViewportFitMode::kFit);
    auto stamp = Stamp(1, 1, 100, transform);
    stamp.viewportWidth = 40; stamp.viewportHeight = 32;
    const auto probe = MakeProbeFrame(image.constBits(), 40, 32, image.bytesPerLine(), 1, stamp);
    const auto match = MatchPatch(patch, probe);
    // Zero NCC is exactly 40/sqrt(40*40+24*24), about .8575; the
    // distant row's stride-3 NCC is 1 but its dense score is -1/3.
    QVERIFY(match.visible);
    QVERIFY(match.matched);
    QVERIFY(match.score >= 0.82);
    if (!stationary) QVERIFY(std::abs(match.sceneOffset.y() * 32 + 5) < 0.5);
}

void UltraClearTextTests::partialRecognitionDoesNotDuplicateAfterPanBack()
{
    const SyntheticPage page = MakeLayoutPage({
        {QStringLiteral("Alpha"), {100, 350}, 64},
        {QStringLiteral("Beta"), {500, 350}, 64},
        {QStringLiteral("Gamma"), {1200, 350}, 64}});
    const auto full = Transform(1.0f, 0.5f, 0.5f);
    const auto crop = Transform(2.0f, 0.75f, 0.5f, ViewportFitMode::kFill);
    const auto current = Stamp(3, 4, 0, full);
    ModelTuning tuning; tuning.admitMatchBudgetMs = 1000;
    OverlayModel model(tuning);
    model.Admit(ResultForView(page, full, Stamp(3,4,100,full), kViewWidth,kViewHeight), current,nullptr,110);
    model.Admit(ResultForView(page, full, Stamp(3,4,200,full), kViewWidth,kViewHeight), current,nullptr,210);
    QCOMPARE(model.AcceptedWordCount(), 3);
    const auto cropProbe = ProbeOf(RenderView(page.image,crop,kViewWidth,kViewHeight),crop,3,4,9,300);
    model.Validate(cropProbe,300);
    // Old full line is now invisible, but retained outside the OCR coverage.
    const auto partial = ResultForView(page,crop,Stamp(3,4,300,crop),kViewWidth,kViewHeight);
    QCOMPARE(partial.output.lines.size(),1);
    QCOMPARE(partial.output.lines.first().words.size(),1);
    QCOMPARE(model.Admit(partial,current,nullptr,310),AdmitVerdict::kAccepted);
    const auto returned = ProbeOf(RenderView(page.image,full,kViewWidth,kViewHeight),full,3,4,10,400);
    model.Validate(returned,400);
    int gamma = 0;
    for (const auto& word : model.SceneWords(410,10)) {
        if (word.text == QStringLiteral("Gamma")) ++gamma;
    }
    QCOMPARE(gamma,1);
}

void UltraClearTextTests::residentBatchesRespectBudgetAndPreserveCompletedChecks()
{
    struct BatchOracle final:ResidentProbe {
        mutable int calls{};bool delay{};std::atomic<bool>* cancel{};
        int Width() const noexcept override{return 100;}
        int Height() const noexcept override{return 100;}
        ReferencePatch Reference(const SourceStamp&,const QRectF&) const override{return {};}
        PatchMatch Match(const SourceStamp&,const ReferencePatch&,const MatchTuning&,std::optional<QPointF>) const override{return {};}
        std::optional<double> Similarity(const SourceStamp&,const ResidentProbe&,const SourceStamp&) const override{return {};}
        QVector<PatchMatch> Matches(const SourceStamp&,const QVector<ReferencePatch>& p,const MatchTuning&,
                                   const QVector<QPointF>&,const std::atomic<bool>*) const override {
            ++calls;if(delay)std::this_thread::sleep_for(std::chrono::milliseconds(2));
            QVector<PatchMatch> out(p.size());for(auto& v:out){v.matched=true;v.visible=true;v.score=1;}
            if(cancel)cancel->store(true);return out;
        }
    };
    auto resident=std::make_shared<BatchOracle>();ProbeFrame probe;probe.resident=resident;probe.stamp.viewTransform.valid=true;
    QVector<LineReference> lines;for(int i=0;i<1100;++i)lines.push_back({std::uint64_t(i+1),{}, {}});
    auto checks=CheckLines(lines,probe,{},0);QCOMPARE(checks.size(),1100);QCOMPARE(resident->calls,18);
    for(int i=0;i<checks.size();++i){QVERIFY(checks[i].checked);QVERIFY(checks[i].match.matched);QCOMPARE(checks[i].lineId,std::uint64_t(i+1));}
    resident->calls=0;resident->delay=true;checks=CheckLines(lines,probe,{},1);
    QCOMPARE(resident->calls,1);for(int i=0;i<checks.size();++i)QCOMPARE(checks[i].checked,i<64);
    resident->calls=0;resident->delay=false;std::atomic<bool> cancelled{false};resident->cancel=&cancelled;
    checks=CheckLines(lines,probe,{},0,&cancelled);QCOMPARE(resident->calls,1);
    for(int i=0;i<checks.size();++i)QCOMPARE(checks[i].checked,i<64);
}

void UltraClearTextTests::budgetRefreshKeepsExactlyRevalidatedText()
{
    const SyntheticPage page = MakePage(kLines);
    const auto t = Transform(1.0f,0.5f,0.5f);
    const auto current = Stamp(3,4,0,t);
    ModelTuning tuning; tuning.admitMatchBudgetMs = 0;
    OverlayModel model(tuning);
    model.Admit(ResultFor(page,t,Stamp(3,4,100,t)),current,nullptr,110);
    model.Admit(ResultFor(page,t,Stamp(3,4,200,t)),current,nullptr,210);
    const auto newer = ProbeOf(RenderView(page.image,t,kViewWidth,kViewHeight),t,3,4,9,500);
    model.Validate(newer,500);
    const int before = model.SceneWords(510,9).size();
    QVERIFY(before > 0);
    // The existing identical references already passed the exact newest
    // probe; a later receipt of older OCR must not discard that validation.
    model.Admit(ResultFor(page,t,Stamp(3,4,300,t)),current,&newer,520);
    QCOMPARE(model.SceneWords(520,9).size(),before);
    // Companion safety cases: altered reference bytes, changed word text,
    // changed geometry, or old validation preceding newer must NOT inherit
    // validation. Existing modelAdmissionBudgetNeverDisplaysUncheckedText
    // remains mandatory and must continue to pass.
}

void UltraClearTextTests::missedProbeBudgetKeepsOnlyUnexpiredProof() {
    const auto page = MakePage({QStringLiteral("Reading glasses help")});
    const auto t = Transform(1, .5, .5);
    const auto current = Stamp(3, 4, 0, t);
    OverlayModel model;
    model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), current, nullptr, 110);
    model.Admit(ResultFor(page, t, Stamp(3, 4, 200, t)), current, nullptr, 210);
    const auto newer = ProbeOf(RenderView(page.image, t, kViewWidth, kViewHeight), t, 3, 4, 9, 500);
    model.Validate(newer, 500);
    const auto id = model.Lines().first().id;
    auto missed = newer.stamp;
    missed.timestampMs = 600;
    missed.frameSeq = 10;
    const auto summary = model.ApplyChecks({LineCheck{id, false, {}}}, missed);
    QCOMPARE(summary.unchecked, 1);
    QCOMPARE(summary.valid, 0);
    QVERIFY(!model.SceneWords(610, 10).isEmpty());
    QCOMPARE(model.Lines().first().validatedAtMs, 500);
    QVERIFY(model.SceneWords(851, 10).isEmpty());
    const auto changed = MakePage({QStringLiteral("Completely different text")});
    auto changedProbe =
        ProbeOf(RenderView(changed.image, t, kViewWidth, kViewHeight), t, 3, 4, 11, 860);
    model.Validate(changedProbe, 860);
    QVERIFY(model.SceneWords(860, 11).isEmpty());
}

void UltraClearTextTests::refreshRetainsLineOnceAndStabilizesColors() {
    const auto page = MakePage({QStringLiteral("Reading glasses help")});
    const auto t = Transform(1, .5, .5);
    const auto current = Stamp(3, 4, 0, t);
    ModelTuning tuning;
    tuning.admitMatchBudgetMs = 0;
    OverlayModel model(tuning);
    auto first = ResultFor(page, t, Stamp(3, 4, 100, t));
    first.lineColors = {{QColor(220, 220, 220), QColor(80, 80, 80)}};
    model.Admit(first, current, nullptr, 110);
    auto second = ResultFor(page, t, Stamp(3, 4, 200, t));
    second.lineColors = first.lineColors;
    model.Admit(second, current, nullptr, 210);
    const auto newer = ProbeOf(RenderView(page.image, t, kViewWidth, kViewHeight), t, 3, 4, 9, 500);
    model.Validate(newer, 500);
    const auto old = model.Lines().first();
    auto refresh = ResultFor(page, t, Stamp(3, 4, 300, t));
    refresh.lineColors = {{QColor(223, 223, 223), QColor(83, 83, 83)}};
    refresh.output.lines.push_back(refresh.output.lines.first());
    refresh.lineColors.push_back(refresh.lineColors.first());
    model.Admit(refresh, current, &newer, 520);
    QCOMPARE(model.Lines().size(), 1);
    QCOMPARE(model.Lines().first().id, old.id);
    QCOMPARE(model.Lines().first().colors.background, old.colors.background);
    QCOMPARE(model.Lines().first().colors.foreground, old.colors.foreground);
    QCOMPARE(model.Lines().first().validatedAtMs, old.validatedAtMs);
    refresh.lineColors = {{QColor(245, 245, 245), QColor(120, 120, 120)},
                          {QColor(245, 245, 245), QColor(120, 120, 120)}};
    model.Admit(refresh, current, &newer, 530);
    QVERIFY(model.Lines().first().colors.background.red() > 110);
    const auto stabilized = model.Lines().first().colors;
    auto fresh = ResultFor(page, t, Stamp(3, 4, 510, t));
    fresh.lineColors = {{QColor(247, 247, 247), QColor(122, 122, 122)}};
    model.Admit(fresh, current, &newer, 540);
    QCOMPARE(model.Lines().first().colors.background, stabilized.background);
    // Repeated unknown drafts cannot renew proof forever.
    model.Admit(refresh, current, &newer, 870);
    QVERIFY(model.SceneWords(870, 10).isEmpty());
}

void UltraClearTextTests::refreshKeepsProvenGeometryThroughOmissionBoxJitter()
{
    const auto page = MakePage({QStringLiteral("Reading B2 glasses")});
    const auto t = Transform(1, .5, .5);
    const auto current = Stamp(3, 4, 0, t);
    ModelTuning tuning;
    tuning.admitMatchBudgetMs = 0;
    OverlayModel model(tuning);
    model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), current, nullptr, 110);
    model.Admit(ResultFor(page, t, Stamp(3, 4, 200, t)), current, nullptr, 210);
    const auto probe = ProbeOf(RenderView(page.image, t, kViewWidth, kViewHeight), t, 3, 4, 9, 500);
    model.Validate(probe, 500);
    QVERIFY(!model.Lines().isEmpty());
    const auto old = model.Lines().first();
    QVERIFY(!old.protectedSceneRects.isEmpty());
    auto refresh = ResultFor(page, t, Stamp(3, 4, 300, t));
    for (auto& line : refresh.output.lines)
        for (auto& word : line.words) word.boundsPx.translate(.5, .5);
    model.Admit(refresh, current, &probe, 520);
    QCOMPARE(model.Lines().size(), 1);
    const auto stable = model.Lines().first();
    QCOMPARE(stable.id, old.id);
    QCOMPARE(stable.sceneRect, old.sceneRect);
    QCOMPARE(stable.sceneOffset, old.sceneOffset);
    QCOMPARE(stable.validatedAtMs, old.validatedAtMs);
    QCOMPARE(stable.validatedFrameSeq, old.validatedFrameSeq);
    QCOMPARE(stable.words.size(), old.words.size());
    for (int i = 0; i < old.words.size(); ++i)
        QCOMPARE(stable.words[i].sceneRect, old.words[i].sceneRect);
    // A retained word never buys a smaller exclusion around uncertain ink.
    for (int i = 0; i < old.protectedSceneRects.size(); ++i)
        QVERIFY(stable.protectedSceneRects[i].contains(old.protectedSceneRects[i]));
    model.Admit(refresh, current, &probe, 870);
    QVERIFY(model.SceneWords(870, 10).isEmpty());
}

void UltraClearTextTests::refreshKeepsIndependentlyValidatedOmissionsDuringRegionalScroll()
{
    const auto before = MakeLayoutPage({{"Reading", {80, 80}, 48},
                                        {"Before", {1000, 80}, 48}});
    auto after = MakeLayoutPage({{"Reading", {80, 80}, 48},
                                {"Afterward", {1000, 400}, 48}});
    // Large independent change on the right, with exactly unchanged left ink.
    { QPainter painter(&after.image); painter.fillRect(800, 600, 800, 300, Qt::black); }
    const auto t = Transform(1, .5, .5);
    const auto current = Stamp(3, 4, 0, t);
    const auto at = [&](int frame, int ms) { auto s = Stamp(3, 4, ms, t); s.frameSeq = frame; return s; };
    auto initial = ResultFor(before, t, at(1, 100));
    const auto splitLines = [](RecognitionResult& result) {
        QVector<RecognizedWord> words;
        for (const auto& line : result.output.lines) words += line.words;
        result.output.lines.clear();
        for (const auto& word : words) {
            RecognizedLine line; line.words.push_back(word);
            result.output.lines.push_back(line);
        }
    };
    splitLines(initial);
    ModelTuning tuning; tuning.admitMatchBudgetMs = 0;
    OverlayModel model(tuning);
    model.Admit(initial, current, nullptr, 110);
    auto second = ResultFor(before, t, at(2, 200));
    splitLines(second);
    model.Admit(second, current, nullptr, 210);
    QCOMPARE(model.SceneWords(210, 0).size(), 2);
    const auto probe = ProbeOf(RenderView(after.image, t, kViewWidth, kViewHeight), t, 3, 4, 9, 500);
    const auto checks = model.Validate(probe, 500);
    QCOMPARE(checks.valid, 1);
    QCOMPARE(checks.invalid, 1); // scrolling right ink loses its old position
    const auto old = model.Lines().first();
    QCOMPARE(old.words.first().text, QStringLiteral("Reading"));
    // A real detector miss: only the independently moving right word is returned.
    auto refresh = ResultFor(after, t, at(3, 300));
    splitLines(refresh);
    refresh.output.lines.removeFirst();
    model.Admit(refresh, current, &probe, 520);
    const auto shown = model.SceneWords(520, 9);
    QCOMPARE(shown.size(), 1);
    QCOMPARE(shown.first().text, QStringLiteral("Reading"));
    QCOMPARE(shown.first().lineId, old.id);
    const auto retained = std::find_if(model.Lines().begin(), model.Lines().end(),
        [&](const auto& line) { return line.id == old.id; });
    QVERIFY(retained != model.Lines().end());
    QCOMPARE(retained->sceneRect, old.sceneRect);
    QCOMPARE(retained->validatedAtMs, old.validatedAtMs);
    QCOMPARE(retained->validatedFrameSeq, old.validatedFrameSeq);
    // Omission cannot renew proof, and a checked pixel contradiction revokes it.
    QVERIFY(model.SceneWords(851, 10).isEmpty());
    auto changed = after.image.copy();
    { QPainter painter(&changed); painter.fillRect(0, 0, 800, 900, Qt::white); }
    model.Validate(ProbeOf(RenderView(changed, t, kViewWidth, kViewHeight), t, 3, 4, 10, 550), 550);
    QVERIFY(model.SceneWords(550, 10).isEmpty());

    // An explicit rejected observation at the old ink is not a detector miss.
    OverlayModel conflict(tuning);
    conflict.Admit(initial, current, nullptr, 110);
    conflict.Admit(second, current, nullptr, 210);
    conflict.Validate(probe, 500);
    auto rejected = ResultFor(after, t, at(3, 300));
    splitLines(rejected);
    rejected.output.lines.first().words.first().confidence = .6;
    conflict.Admit(rejected, current, &probe, 520);
    QVERIFY(conflict.SceneWords(520, 9).isEmpty());

    // A check older than the OCR acquisition cannot excuse an omission.
    OverlayModel stale(tuning);
    stale.Admit(initial, current, nullptr, 110);
    stale.Admit(second, current, nullptr, 210);
    stale.Validate(probe, 500);
    refresh.stamp = at(10, 600);
    stale.Admit(refresh, current, &probe, 610);
    QVERIFY(stale.SceneWords(610, 10).isEmpty());
}

void UltraClearTextTests::probeReferencesPrioritizeUnverifiedAndOldestLines() {
    const auto page = MakePage(kLines);
    const auto t = Transform(1, .5, .5);
    const auto current = Stamp(3, 4, 0, t);
    OverlayModel model;
    model.Admit(ResultFor(page, t, Stamp(3, 4, 100, t)), current, nullptr, 110);
    model.Admit(ResultFor(page, t, Stamp(3, 4, 200, t)), current, nullptr, 210);
    QCOMPARE(model.Lines().size(), 3);
    const auto a = model.Lines()[0].id, b = model.Lines()[1].id, c = model.Lines()[2].id;
    auto probe = Stamp(3, 4, 500, t);
    probe.frameSeq = 9;
    model.ApplyChecks({LineCheck{a, true, PatchMatch{true, true, 1, {}}},
                       LineCheck{b, true, PatchMatch{false, false, 0, {}}}},
                      probe);
    const auto refs = model.LineReferences(current);
    QCOMPARE(refs.size(), 3);
    QCOMPARE(refs[0].lineId, b);
    QCOMPARE(refs[1].lineId, c);
    QCOMPARE(refs[2].lineId, a);
}

void UltraClearTextTests::budgetRefreshCannotBorrowOtherProof_data()
{
    QTest::addColumn<QString>("change");
    for (const char* name : {"reference", "text", "word-geometry", "line-geometry",
                             "older-time", "older-frame", "scene"}) {
        QTest::newRow(name) << QString::fromLatin1(name);
    }
}
void UltraClearTextTests::budgetRefreshCannotBorrowOtherProof()
{
    QFETCH(QString, change);
    const SyntheticPage page = MakePage({QStringLiteral("Reading glasses help")});
    const auto t = Transform(1.0f,0.5f,0.5f);
    auto current = Stamp(3,4,0,t);
    ModelTuning tuning; tuning.admitMatchBudgetMs = 0;
    OverlayModel model(tuning);
    model.Admit(ResultFor(page,t,Stamp(3,4,100,t)),current,nullptr,110);
    model.Admit(ResultFor(page,t,Stamp(3,4,200,t)),current,nullptr,210);
    auto newer = ProbeOf(RenderView(page.image,t,kViewWidth,kViewHeight),t,3,4,9,500);
    model.Validate(newer,500);
    QVERIFY(!model.SceneWords(510,9).isEmpty());
    auto result = ResultFor(page,t,Stamp(3,4,300,t));
    result.prepared = PrepareLines(result,tuning.minimumWordHeightPx);
    QCOMPARE(result.prepared.size(),1);
    QVERIFY(result.prepared.first().reference.valid);
    if (change == QStringLiteral("reference")) {
        result.prepared[0].reference.values[0] += 0.25f;
    } else if (change == QStringLiteral("text")) {
        result.prepared[0].words[0].text = QStringLiteral("Changed");
        result.output.lines[0].words[0].text = QStringLiteral("Changed");
    } else if (change == QStringLiteral("word-geometry")) {
        result.prepared[0].words[0].sceneRect.translate(0.001,0.0);
    } else if (change == QStringLiteral("line-geometry")) {
        result.prepared[0].sceneRect.translate(0.001,0.0);
    } else if (change == QStringLiteral("older-time")) {
        newer.stamp.timestampMs = 501;
    } else if (change == QStringLiteral("older-frame")) {
        newer.stamp.frameSeq = 10;
    } else if (change == QStringLiteral("scene")) {
        ++current.geometryGeneration;
        result.stamp.geometryGeneration = current.geometryGeneration;
        result.reference.stamp.geometryGeneration = current.geometryGeneration;
        newer.stamp.geometryGeneration = current.geometryGeneration;
    }
    const AcceptedLine previouslyValidated=model.Lines().first();
    model.Admit(result,current,&newer,520);
    const bool keepOld=change!=QStringLiteral("text") && change!=QStringLiteral("scene");
    if(keepOld){
        // The NEW reference/geometry is never given the OLD proof. Keep the
        // complete previously verified object through this near-identical draft.
        QVERIFY(!model.SceneWords(520,newer.stamp.frameSeq).isEmpty());
        QCOMPARE(model.Lines().first().id,previouslyValidated.id);
        QCOMPARE(model.Lines().first().reference.values,previouslyValidated.reference.values);
        QCOMPARE(model.Lines().first().sceneRect,previouslyValidated.sceneRect);
        QCOMPARE(model.Lines().first().words.first().sceneRect,previouslyValidated.words.first().sceneRect);
        QCOMPARE(model.Lines().first().validatedAtMs,previouslyValidated.validatedAtMs);
    }else{
        QVERIFY(model.SceneWords(520,newer.stamp.frameSeq).isEmpty());
    }
    // Same-scene rows retain either the verified old object or a hidden new
    // candidate for later worker checks; new references never borrow proof.
    if (change != QStringLiteral("scene")) {
        QVERIFY(model.HasAcceptedText());
        QVERIFY(!model.LineReferences(current).isEmpty());
    }
}

void UltraClearTextTests::schedulerKeepsLocalSurfaceColors_data()
{
    QTest::addColumn<int>("sample");
    const auto fixtures = LocalColorFixtures();
    for (int i = 0; i < fixtures.size(); ++i)
        QTest::newRow(qPrintable(fixtures[i].name)) << i;
}

void UltraClearTextTests::schedulerKeepsLocalSurfaceColors()
{
    QFETCH(int, sample);
    const auto f = LocalColorFixtures()[sample];
    if (f.expectInkCorners) QVERIFY2(SolidInkCorners(f), qPrintable(f.name));
    struct Backend final : Recognizer {
        RecognitionOutput output;
        BackendStatus Initialize() override { return {BackendState::kReady, "colors", "", ""}; }
        RecognitionOutput Recognize(const QImage&, const std::atomic<bool>&) override { return output; }
    };
    auto backend = std::make_unique<Backend>();
    backend->output.ok = true;
    RecognizedLine line; line.words.push_back({QStringLiteral("Button"), QRectF(f.bounds)});
    backend->output.lines.push_back(line);
    RecognitionResult measured;
    std::atomic<bool> received{false};
    RecognitionScheduler scheduler(std::move(backend), [&](RecognitionResult result) {
        measured = std::move(result); received.store(true);
    }, {});
    auto stamp = Stamp(1, 1, 100, {0,0,1,1,0,0,1,1,true});
    stamp.viewportWidth = f.pixels.width(); stamp.viewportHeight = f.pixels.height();
    QVERIFY(scheduler.Submit(stamp, f.pixels));
    QTRY_VERIFY(received.load());
    QVERIFY(measured.output.ok);
    QCOMPARE(measured.lineColors.size(), 1);
    QVERIFY(measured.output.lines[0].words[0].colors);
    const auto check = [&](const LineColors& colors) {
        return ColorDistance(colors.foreground, f.ink) <= f.tolerance &&
               ColorDistance(colors.background, f.paper) <= f.tolerance;
    };
    if (f.expectSourceColors) {
        QVERIFY2(check(measured.lineColors[0]), qPrintable(f.name + " line ink=" +
            measured.lineColors[0].foreground.name() + " paper=" + measured.lineColors[0].background.name()));
        QVERIFY2(check(*measured.output.lines[0].words[0].colors), qPrintable(f.name));
    }
    // Boundary rows exercise raw measurement, independently of admission or
    // fallback-polarity semantics. A tiny/outside word may have no prepared word.
    QVERIFY(measured.lineColors[0].foreground.isValid());
    QVERIFY(measured.lineColors[0].background.isValid());
    QCOMPARE(measured.output.lines[0].words[0].colors->foreground, measured.lineColors[0].foreground);
    QCOMPARE(measured.output.lines[0].words[0].colors->background, measured.lineColors[0].background);
    // The overlay's separate 7:1 adjustment and 64-level background
    // stabilization are deliberately not source equality.
    if (!measured.prepared.isEmpty() && !measured.prepared[0].words.isEmpty()) {
        QVERIFY(measured.prepared[0].words[0].colors);
        QCOMPARE(measured.prepared[0].words[0].colors->foreground, measured.lineColors[0].foreground);
        QCOMPARE(measured.prepared[0].words[0].colors->background, measured.lineColors[0].background);
    }
}

void UltraClearTextTests::immutableRefinementRefreshesOnlyProvenMatchingColors_data()
{
    QTest::addColumn<bool>("proven"); QTest::addColumn<bool>("matching");
    QTest::newRow("matching-own-proof-colors-only") << true << true;
    QTest::newRow("matching-reading-only-cannot-refresh") << false << true;
    QTest::newRow("different-reading-only-cannot-refresh") << false << false;
}

void UltraClearTextTests::immutableRefinementRefreshesOnlyProvenMatchingColors()
{
    QFETCH(bool, proven); QFETCH(bool, matching);
    auto [selected, neighbor] = ImmutablePair(MakePage({"Reading glasses"}));
    const LineColors original{QColor(230,240,232), QColor(32,64,48)};
    const LineColors corrected{QColor(12,18,24), QColor(255,192,96)};
    for (auto& word : selected.prepared[0].words) word.colors = original;
    OverlayModel model;
    QCOMPARE(model.AdmitImmutable(selected, selected.stamp, 60'000, &neighbor), AdmitVerdict::kAccepted);
    const auto before = model.Lines()[0];
    const auto beforeWords = model.SceneWords(60'000, 55);
    auto refined = selected;
    refined.prepared[0].words[0].colors = corrected;
    refined.prepared[0].words[0].sceneRect.translate(.001, .001);
    if (!matching) refined.prepared[0].words[0].text = QStringLiteral("Writing");
    neighbor.corroborationWordMatches[0][0] = proven;
    QCOMPARE(model.AdmitImmutable(refined, refined.stamp, 90'000, &neighbor), AdmitVerdict::kAccepted);
    const auto& after = model.Lines()[0];
    QCOMPARE(after.id, before.id); QCOMPARE(after.words.size(), before.words.size());
    QCOMPARE(after.sceneRect, before.sceneRect); QCOMPARE(after.sceneOffset, before.sceneOffset);
    QCOMPARE(after.reference.sceneRect, before.reference.sceneRect);
    QCOMPARE(after.reference.values, before.reference.values); QCOMPARE(after.reference.resident, before.reference.resident);
    QCOMPARE(after.provenance.frameSeq, before.provenance.frameSeq);
    QCOMPARE(after.provenance.timestampMs, before.provenance.timestampMs);
    QCOMPARE(after.validatedAtMs, before.validatedAtMs); QCOMPARE(after.validatedFrameSeq, before.validatedFrameSeq);
    const auto words = model.SceneWords(90'000, 55);
    QCOMPARE(words.size(), beforeWords.size());
    QCOMPARE(model.ReadingText(90'000, 55), QStringLiteral("Reading glasses"));
    for (int i = 0; i < words.size(); ++i) {
        QCOMPARE(words[i].lineId, beforeWords[i].lineId); QCOMPARE(words[i].wordIndex, beforeWords[i].wordIndex);
        QCOMPARE(after.words[i].sceneRect, before.words[i].sceneRect);
        QCOMPARE(after.words[i].text, before.words[i].text);
    }
    const auto expected = proven && matching ? corrected : *before.words[0].colors;
    QCOMPARE(after.words[0].colors->foreground, expected.foreground);
    QCOMPARE(after.words[0].colors->background, expected.background);
    QCOMPARE(after.words[1].colors->foreground, before.words[1].colors->foreground);
    QCOMPARE(after.words[1].colors->background, before.words[1].colors->background);
}

void UltraClearTextTests::schedulerPreservesDenseTextPolarity_data()
{
    QTest::addColumn<bool>("darkInk");
    QTest::newRow("dense-dark-on-light") << true;
    QTest::newRow("dense-light-on-dark") << false;
}

void UltraClearTextTests::schedulerPreservesDenseTextPolarity()
{
    QFETCH(bool, darkInk);
    const QColor dark(18, 25, 36), light(232, 238, 226);
    const QColor ink = darkInk ? dark : light;
    const QColor paper = darkInk ? light : dark;
    QImage image(160, 80, QImage::Format_ARGB32);
    image.fill(paper);
    const QRect box(40, 20, 80, 40);
    // Controlled dense strokes occupy 75% of the tight word rectangle.
    // Recognition is scripted: this checks measured colors, not OCR accuracy.
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            if ((x - box.left()) % 8 < 6) image.setPixelColor(x, y, ink);
        }
    }
    const auto t = ComputeViewTransform(160, 80, 160, 80, 1.0f, 0.5f, 0.5f, ViewportFitMode::kFit);
    auto shared = std::make_shared<ScriptShared>();
    shared->words = {{QStringLiteral("MMMM"), QRectF(0.25, 0.25, 0.5, 0.5)}};
    shared->transforms.insert(Checksum(image), t);
    LineColors colors;
    std::atomic<bool> received{false};
    RecognitionScheduler scheduler(std::make_unique<ScriptedRecognizer>(shared),
        [&](RecognitionResult result) {
            if (result.lineColors.size() == 1) colors = result.lineColors.first();
            received.store(true);
        }, {});
    QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
    auto stamp = Stamp(1, 1, 100, t);
    stamp.viewportWidth = 160; stamp.viewportHeight = 80;
    QVERIFY(scheduler.Submit(stamp, image) != 0);
    QTRY_VERIFY(received.load());
    QCOMPARE(colors.foreground, ink);
    QCOMPARE(colors.background, paper);
}

void UltraClearTextTests::slowOcrNeverBlocksPresentationClock()
{
    Harness h;
    h.shared->SetHold(true);
    h.Enable();
    // First establish the blocked recognition. Worker startup is asynchronous
    // and can be delayed by other CPU tests; it is not part of the 300-frame
    // presentation-independence assertion.
    QTRY_VERIFY(([&] {
        h.Present();
        return h.controller->SchedulerStatistics().started == 1;
    })());
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
    // Establish the first held OCR call using completed probe feedback.
    // Alternating camera frames faster than the asynchronous probe worker
    // can inspect them is not evidence of stable content or an OCR admission.
    for (int i = 0; i < 200 && h.controller->SchedulerStatistics().started == 0; ++i) {
        h.Present(); QCoreApplication::processEvents(); QThread::msleep(1);
    }
    QCOMPARE(h.controller->SchedulerStatistics().started, std::uint64_t{1});
    for (const SyntheticPage& page : {MakePage({QStringLiteral("Board text changes")}), MakePage(kLines)}) {
        h.SetPage(page);
        const auto before = h.controller->SchedulerStatistics().submitted;
        for (int i = 0; i < 200 && h.controller->SchedulerStatistics().submitted == before; ++i) {
            h.Present(); QCoreApplication::processEvents(); QThread::msleep(1);
        }
        QVERIFY(h.controller->SchedulerStatistics().submitted > before);
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
    {
        UctSceneSettings settings = h.context.geometry.settings;
        settings.sourceEpoch = 8;
        h.SetSceneSettings(settings);
    }
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
    {
        UctSceneSettings settings = h.context.geometry.settings;
        settings.rotationQuarterTurns = 1;
        h.SetSceneSettings(settings);
    }
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

    // A displayed Temporal Smooth composite alone no longer pauses UCT:
    // CUDA analysis uses its own paired neutral capture for it.
    h.context.compositeFrames = true;
    h.Present(false, 1);
    QVERIFY(h.overlay->WordCount() > 0);
    // An unsupported composite from a stabilized scene still pauses and
    // names Extra Stable; supported held frames arrive with their record.
    h.context.geometry.settings.stabilization = true;
    h.context.requested.stabilization = true;
    h.context.unsupportedCompositeFrames = true;
    QVERIFY(!h.Present());
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("Extra Stable")));
    QVERIFY(!h.controller->StatusSource().contains(QStringLiteral("Temporal Smooth")));
    for (int i = 0; i < 20; ++i) {
        QVERIFY(!h.Present());
    }
    h.controller->SetMediaOverlaySuppressed(true,true);
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("Extra Stable")));
    auto* status=QAccessible::queryAccessibleInterface(h.panel->statusLabel());QVERIFY(status);
    QCOMPARE(status->text(QAccessible::Name),h.panel->statusLabel()->text());
    h.controller->SetMediaOverlaySuppressed(true,false);
    QVERIFY(!h.controller->StatusSource().contains(QStringLiteral("Extra Stable")));
    h.controller->SetMediaOverlaySuppressed(false);
    h.context.geometry.settings.stabilization=false;
    h.context.requested.stabilization=false;
    h.Present();
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("debug composite")));
}

void UltraClearTextTests::explicitEnableRetriesBackendInitialization_data()
{
    QTest::addColumn<int>("initialState");
    QTest::newRow("unavailable-initialization") << static_cast<int>(BackendState::kUnavailable);
    QTest::newRow("failed-initialization") << static_cast<int>(BackendState::kFailed);
}

void UltraClearTextTests::explicitEnableRetriesBackendInitialization()
{
    QFETCH(int, initialState);
    const auto state = static_cast<BackendState>(initialState);
    Harness h(state);
    h.Enable();
    QCOMPARE(h.controller->BackendStatusSnapshot().state, state);
    {
        std::lock_guard lock(h.shared->mutex);
        h.shared->initState = BackendState::kReady;
    }
    h.controller->SetEnabled(false);
    h.controller->SetEnabled(true);
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
    QVERIFY(h.RunUntilShown());
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
    const quint64 before = Checksum(view);
    std::optional<quint64> previousObservation;
    int observations = 0;
    for (int i = 0; i < 200 && h.overlay->WordCount() == 0; ++i) {
        h.now += 100;
        h.context.frameSeq = ++h.frameSeq;
        if (h.controller->OnPresented(h.context)) {
            // This manual CPU path bypasses Harness::Present. Supply actual
            // distinct observations here too; new sequence IDs over the same
            // view are deliberately insufficient for temporal admission.
            const QImage observed = IndependentFramePixels(view, h.context.frameSeq);
            const quint64 observationBefore = Checksum(observed);
            if (previousObservation) QVERIFY(observationBefore != *previousObservation);
            previousObservation = observationBefore;
            ++observations;
            {
                std::lock_guard lock(h.shared->mutex);
                h.shared->transforms.insert(observationBefore, h.context.transform);
            }
            h.controller->OnCpuViewport(h.context, observed.constBits(), observed.width(), observed.height(),
                                        static_cast<int>(observed.bytesPerLine()));
            QCOMPARE(Checksum(observed), observationBefore);
        }
        h.controller->OnPresentationSucceeded();
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    QVERIFY(h.overlay->WordCount() > 0);
    QVERIFY(observations >= 2);
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
    QTRY_VERIFY(h.overlay->WordCount() > 0); // fresh validation is now asynchronous
}

void UltraClearTextTests::watchdogCannotRestoreTextWithoutSuccessfulPresentation()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    QVERIFY(!h.panel->readingPane()->toPlainText().isEmpty());
    // Keep precisely the last validated image. This exercises the intentional
    // unchanged-frame freshness exemption rather than ordinary age expiry.
    h.frameSeq = h.controller->Model().Lines().first().validatedFrameSeq;
    h.completeReadbacks = false;
    h.Present(false, 1);
    QVERIFY(h.overlay->WordCount() > 0);
    h.now += 701;
    QTRY_COMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.controller->Model().HasAcceptedText());

    h.panel->showOriginalCheckbox()->setChecked(true);
    h.panel->showOriginalCheckbox()->setChecked(false);
    // Neither a control toggle nor event delivery is a presentation receipt.
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.panel->readingPane()->toPlainText().isEmpty());
    QCoreApplication::processEvents();
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.panel->readingPane()->toPlainText().isEmpty());

    // Only the explicit successful presentation receipt restores the cached
    // unchanged scene; no new readback or recognition is needed.
    h.Present(false, 1);
    QVERIFY(h.overlay->WordCount() > 0);
    QVERIFY(!h.panel->readingPane()->toPlainText().isEmpty());
}

void UltraClearTextTests::failedPresentationHidesGlyphsAndReadingPane()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    QVERIFY(!h.panel->readingPane()->toPlainText().isEmpty());
    h.completeReadbacks = false;
    h.frameSeq = h.controller->Model().Lines().first().validatedFrameSeq;
    h.context.frameSeq = h.frameSeq;
    // Production calls this preflight before the presenter can reject work.
    h.controller->OnPresented(h.context);
    h.controller->OnPresentationFailed();
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.panel->readingPane()->toPlainText().isEmpty());
    h.panel->showOriginalCheckbox()->setChecked(true);
    h.panel->showOriginalCheckbox()->setChecked(false);
    QCoreApplication::processEvents();
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.panel->readingPane()->toPlainText().isEmpty());

    // Retrying a submission does not establish that new pixels reached the
    // presenter. Only the subsequent explicit success receipt restores text.
    h.controller->OnPresented(h.context);
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.panel->readingPane()->toPlainText().isEmpty());
    h.Present(false, 1);
    QVERIFY(h.overlay->WordCount() > 0);
    QVERIFY(!h.panel->readingPane()->toPlainText().isEmpty());
}

void UltraClearTextTests::neuralPublicationRevokesHeldRecognition()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    const auto started = h.controller->SchedulerStatistics().started;
    h.shared->SetHold(true);
    h.now += 1600;
    for (int i = 0; i < 200 && h.controller->SchedulerStatistics().started == started; ++i) {
        h.Present(); QCoreApplication::processEvents(); QThread::msleep(1);
    }
    QVERIFY(h.controller->SchedulerStatistics().started > started);
    h.completeReadbacks = false;
    // Same source epoch and camera frame: only the presented enhancement
    // changes. Neither accepted glyphs nor held base-image OCR may carry over.
    const auto revoked = h.controller->SchedulerStatistics().revoked;
    h.context.geometry.neuralEnhancement = true;
    h.Present(false, 1);
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(!h.controller->Model().HasAcceptedText());
    h.shared->SetHold(false);
    QTRY_VERIFY(h.controller->SchedulerStatistics().revoked > revoked);
    h.Present(false, 1);
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(!h.controller->Model().HasAcceptedText());
}

void UltraClearTextTests::cachedGlyphsRepaintWhileRecognitionIsHeld()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    const auto started = h.controller->SchedulerStatistics().started;
    h.shared->SetHold(true);
    h.now += 1600;
    for (int i = 0; i < 200 && h.controller->SchedulerStatistics().started == started; ++i) {
        h.Present(); QCoreApplication::processEvents(); QThread::msleep(1);
    }
    QVERIFY(h.controller->SchedulerStatistics().started > started);
    QTRY_VERIFY(h.overlay->WordCount() > 0);
    const auto completed = h.controller->SchedulerStatistics().completed;
    // Re-present one known camera scene at a high rate while its next OCR
    // result is held. Existing glyphs must actually draw, not only update a
    // transform in an empty overlay.
    h.frameSeq = h.controller->Model().Lines().first().validatedFrameSeq;
    h.completeReadbacks = false;
    h.owner.resize(1940, 1100);
    h.target->resize(1920, 1080);
    QImage painted(1920, 1080, QImage::Format_ARGB32_Premultiplied);
    QVector<double> paints;
    h.presentMs.clear();
    for (int frame = 0; frame < 300; ++frame) {
        h.context.transform = Transform(1.3f, 0.45f + 0.0001f * frame, 0.5f, ViewportFitMode::kFill);
        h.Present(false, 8);
        QVERIFY(h.overlay->WordCount() > 0);
        QCOMPARE(h.overlay->CurrentViewTransform().sourceX, h.context.transform.sourceX);
        QElapsedTimer timer; timer.start();
        painted.fill(Qt::transparent);
        h.overlay->render(&painted);
        paints.push_back(timer.nsecsElapsed() / 1e6);
    }
    QCOMPARE(h.controller->SchedulerStatistics().completed, completed);
    qInfo("cached glyphs with held OCR: OnPresented p95 %.3f / p99 %.3f ms; 1920x1080 paint p95 %.3f / p99 %.3f ms",
        Percentile(h.presentMs, 0.95), Percentile(h.presentMs, 0.99), Percentile(paints, 0.95), Percentile(paints, 0.99));
    QVERIFY(Percentile(h.presentMs, 0.99) < 25.0);
    h.shared->SetHold(false);
}

void UltraClearTextTests::processedSceneProvenanceSurvivesDeferredFrames()
{
    UctSceneProvenance scenes;
    UctSceneSettings requested;
    requested.sourceEpoch = 7;
    scenes.OnCudaSceneProcessed(requested, 1600, 900, 42, false);
    const auto present = [&](bool cuda) { return scenes.Context(cuda, requested, false,
        Transform(1.0f, 0.5f, 0.5f, ViewportFitMode::kFit), 960, 540, true, 0); };
    QCOMPARE(present(true).frameSeq, std::uint64_t{42});
    // A newer capture/request can arrive while the GPU rejects or defers it.
    // No successful scene publication occurred, so the old pixels keep 42/0.
    requested.rotationQuarterTurns = 1;
    requested.keystone = true;
    requested.warpRevision = 9;
    auto held = present(true);
    QCOMPARE(held.frameSeq, std::uint64_t{42});
    QCOMPARE(held.geometry.settings.rotationQuarterTurns, 0);
    QVERIFY(!(held.geometry.settings == held.requested));
    scenes.OnCudaSceneProcessed(requested, 900, 1600, 43, false);
    QCOMPARE(present(true).frameSeq, std::uint64_t{43});
    QVERIFY(present(true).geometry.settings == present(true).requested);
    scenes.OnCpuSceneProcessed(7, 1, 900, 1600, 44, false);
    requested.stabilization = true;
    requested.colorScheme = 0x100000001ull;
    const auto cpu = present(false);
    QCOMPARE(cpu.frameSeq, std::uint64_t{44});
    QVERIFY(!cpu.geometry.settings.keystone && !cpu.geometry.settings.stabilization);
    QCOMPARE(cpu.geometry.settings.colorScheme, std::uint64_t{0});
    QVERIFY(cpu.geometry.settings == cpu.requested); // CPU has no pending CUDA warp/color.
    scenes.Reset();
    QVERIFY(!present(true).cameraActive);
    QVERIFY(!present(false).cameraActive);
}

void UltraClearTextTests::sourceRetirementRevokesWithoutAnotherPresentation()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    const auto oldGeneration = h.controller->GeometryGeneration();
    h.controller->OnSourceRetired();
    QCOMPARE(h.overlay->WordCount(), 0);
    QVERIFY(h.controller->Model().Lines().isEmpty());
    QVERIFY(h.panel->readingPane()->toPlainText().isEmpty());
    QVERIFY(h.controller->GeometryGeneration() > oldGeneration);
    QCoreApplication::processEvents();
    QCOMPARE(h.overlay->WordCount(), 0);
}

void UltraClearTextTests::mixedHeightReadingOrderIsStableAndOnlyFreshTextIsRead()
{
    const auto top = [](int size, double centre) {
        const QFontMetricsF metrics(PageFont(size));
        return centre - (metrics.ascent() + metrics.descent()) * 0.5;
    };
    const SyntheticPage page = MakeLayoutPage({
        {QStringLiteral("Alpha"), {800, top(20, 100)}, 20},
        {QStringLiteral("Beta"), {500, top(100, 108)}, 100},
        {QStringLiteral("Gamma"), {200, top(20, 116)}, 20}});
    const auto transform = Transform(1.0f, 0.5f, 0.5f, ViewportFitMode::kFit);
    auto stamp = Stamp(7, 1, 100, transform);
    stamp.frameSeq = 9;
    const auto base = ResultFor(page, transform, stamp);
    std::array<int, 3> order{0, 1, 2};
    do {
        auto result = base;
        result.output.lines.clear(); result.lineColors.clear();
        for (const int index : order) {
            const auto& word = page.words[index];
            result.output.lines.push_back({{{word.text, SceneRectToView(transform, kViewWidth, kViewHeight, word.sceneRect)}}});
            result.lineColors.push_back({});
        }
        OverlayModel model;
        result.jobId = 1;
        model.Admit(result, stamp, nullptr, 100);
        auto secondStamp=stamp;secondStamp.timestampMs=200;secondStamp.frameSeq=10;
        auto second=ResultFor(page,transform,secondStamp);
        second.output.lines=result.output.lines;second.lineColors=result.lineColors;second.jobId=2;
        QCOMPARE(model.Admit(second, stamp, nullptr, 200), AdmitVerdict::kAccepted);
        QCOMPARE(model.ReadingText(200, 10), QStringLiteral("Beta\nAlpha\nGamma"));
        QVERIFY(model.ReadingText(1000, 11).isEmpty());
    } while (std::next_permutation(order.begin(), order.end()));
}

void UltraClearTextTests::unchangedWordsRefreshMeasuredColors()
{
    Harness h;
    h.Enable();
    QVERIFY(h.RunUntilShown());
    h.SetPage(MakePage(kLines, {}, 1600, 900, 64, QColor(100, 0, 0), QColor(255, 255, 255)));
    h.now += 1600; // recognition refresh, with identical word geometry
    const auto red = [&] {
        return !h.overlay->Words().isEmpty() && h.overlay->Words().first().colors.foreground.red() >
            h.overlay->Words().first().colors.foreground.blue() + 20;
    };
    for (int i = 0; i < 200 && !red(); ++i) {
        h.Present(); QCoreApplication::processEvents(); QThread::msleep(1);
    }
    QVERIFY(red());
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
        const bool wanted = h.controller->OnPresented(h.context);
        h.controller->OnPresentationSucceeded();
        if (!wanted) {
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

void UltraClearTextTests::denseMovingPageValidationCostAt4K()
{
    // Worst case for residual validation: a dense 4K page whose lines are all
    // accepted, then moving a little every probe so no line takes the static
    // fast path. Reports the per-probe core cost (probe build + validation).
    constexpr int kW = 3840;
    constexpr int kH = 2160;
    const QStringList lines = DenseLines(52);
    const SyntheticPage page = MakePage(lines, {}, kW, kH, 24);
    const ViewTransform t = ComputeViewTransform(kW, kH, kW, kH, 1.0f, 0.5f, 0.5f, ViewportFitMode::kFit);
    OverlayModel model;
    const SourceStamp current = Stamp(1, 1, 0, t);
    model.Admit(ResultForView(page, t, Stamp(1, 1, 100, t), kW, kH), current, nullptr, 110);
    model.Admit(ResultForView(page, t, Stamp(1, 1, 200, t), kW, kH), current, nullptr, 210);
    const int acceptedLines = model.Lines().size();
    QVERIFY2(acceptedLines >= 48, qPrintable(QString::number(acceptedLines)));
    QVector<double> probeMs;
    QVector<double> validateMs;
    int searched = 0;
    for (int i = 0; i < 12; ++i) {
        // Alternating small shifts: every line needs the full translation search.
        const QPointF shift((i % 2) ? 12.0 : -12.0, (i % 2) ? 7.0 : -7.0);
        const SyntheticPage moved = MakePage(lines, shift, kW, kH, 24);
        const QImage view = RenderView(moved.image, t, kW, kH);
        SourceStamp stamp = Stamp(1, 1, 300 + i * 80, t);
        stamp.viewportWidth = kW;
        stamp.viewportHeight = kH;
        stamp.frameSeq = 10 + i;
        QElapsedTimer timer;
        timer.start();
        const ProbeFrame probe = MakeProbeFrame(view.constBits(), kW, kH, static_cast<int>(view.bytesPerLine()),
                                                4, stamp);
        probeMs.push_back(timer.nsecsElapsed() / 1e6);
        timer.restart();
        const ValidationSummary summary = model.Validate(probe, stamp.timestampMs);
        validateMs.push_back(timer.nsecsElapsed() / 1e6);
        searched += summary.valid + summary.invalid;
        // Re-admit so later iterations still have every line to validate.
        if (model.Lines().size() < acceptedLines) {
            model.Reset();
            model.Admit(ResultForView(page, t, Stamp(1, 1, stamp.timestampMs, t), kW, kH), current, nullptr,
                        stamp.timestampMs);
            model.Admit(ResultForView(page, t, Stamp(1, 1, stamp.timestampMs + 1, t), kW, kH), current, nullptr,
                        stamp.timestampMs + 1);
        }
    }
    qInfo("dense 4K moving page, %d lines: probe build p50 %.2f / max %.2f ms; validation p50 %.2f / "
          "p95 %.2f / max %.2f ms (%d line checks)",
          acceptedLines, Percentile(probeMs, 0.5), Percentile(probeMs, 1.0), Percentile(validateMs, 0.5),
          Percentile(validateMs, 0.95), Percentile(validateMs, 1.0), searched);
}

void UltraClearTextTests::glyphAndPlateVisibilityUseTheSameEligibility_data()
{
    QTest::addColumn<QString>("token");
    QTest::addColumn<double>("originalHeight");
    QTest::newRow("plate-without-small-dash") << QStringLiteral("-") << 8.0;
    QTest::newRow("glyph-without-small-original") << QStringLiteral("H") << 5.0;
}

void UltraClearTextTests::glyphAndPlateVisibilityUseTheSameEligibility()
{
    QFETCH(QString, token);
    QFETCH(double, originalHeight);
    QWidget target;
    target.resize(960, 540);
    UltraClearTextOverlay overlay(&target, &target);
    overlay.resize(target.size());
    overlay.SetViewTransform(ComputeViewTransform(960, 540, 960, 540, 1.0f, 0.5f, 0.5f,
                                                ViewportFitMode::kFit));
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(100);
    font.setWeight(QFont::Medium);
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF metrics(font);
    const auto normalized = [](const QRectF& r) {
        return QRectF(r.x() / 960.0, r.y() / 540.0, r.width() / 960.0, r.height() / 540.0);
    };
    QVector<UltraClearTextOverlay::SceneWord> words;
    qreal left = 30.0;
    // Two ordinary words set the shared median font size; the isolated token
    // must share their line, otherwise its own ink is fitted to its OCR box.
    for (const QString& text : {QStringLiteral("HELLO"), QStringLiteral("WORLD")}) {
        const QRectF ink = metrics.tightBoundingRect(text);
        words.push_back({text, normalized(QRectF(left, 100.0 + ink.top() * 0.1,
                                                ink.width() * 0.1, ink.height() * 0.1)), {}, 1});
        left += 100.0;
    }
    words.push_back({token, normalized(QRectF(400.0, 90.0, 18.0, originalHeight)), {}, 1});
    overlay.SetWords(words);
    const auto glyphs = overlay.ProjectedGlyphs();
    QCOMPARE(glyphs.size(), 3);
    if (originalHeight >= 6.0) {
        QVERIFY(glyphs.last().inkRect.height() < 6.0);
    } else {
        QVERIFY(glyphs.last().inkRect.height() >= 6.0);
    }
    QImage painted(960, 540, QImage::Format_ARGB32_Premultiplied);
    painted.fill(Qt::transparent);
    overlay.render(&painted);
    // This rectangle contains the token, its plate padding and fitted ink,
    // and is far from both ordinary words. Leave original camera pixels
    // completely intact whenever either half of replacement is ineligible.
    for (int y = 50; y < 140; ++y) {
        for (int x = 380; x < 440; ++x) {
            QVERIFY2(qAlpha(painted.pixel(x, y)) == 0,
                     qPrintable(QStringLiteral("Unexpected replacement at %1,%2").arg(x).arg(y)));
        }
    }
}

void UltraClearTextTests::readingPanePreservesSelectionAndScroll_data()
{
    QTest::addColumn<int>("anchor");
    QTest::addColumn<int>("position");
    QTest::newRow("forward-selection") << 6 << 10;
    QTest::newRow("reverse-selection") << 10 << 6;
    QTest::newRow("caret") << 8 << 8;
}

void UltraClearTextTests::readingPanePreservesSelectionAndScroll()
{
    QFETCH(int, anchor);
    QFETCH(int, position);
    UltraClearTextPanel panel;
    panel.resize(420, 320);
    panel.show();
    QStringList lines;
    for (int i = 0; i < 80; ++i) {
        lines.push_back(QStringLiteral("Line %1: accepted printed words").arg(i));
    }
    const QString original = lines.join(QLatin1Char('\n'));
    panel.SetReadingText(original);
    QCoreApplication::processEvents();
    QPlainTextEdit* pane = panel.readingPane();
    QTextCursor cursor = pane->textCursor();
    cursor.setPosition(anchor);
    cursor.setPosition(position, QTextCursor::KeepAnchor);
    pane->setTextCursor(cursor);
    QScrollBar* scroll = pane->verticalScrollBar();
    QVERIFY(scroll->maximum() > 20);
    scroll->setValue(20);
    const int previousScroll = scroll->value();
    const QString selection = pane->textCursor().selectedText();

    // Recognition refresh adds another accepted line; the reader's existing
    // selection and viewport refer to an unchanged prefix of the document.
    panel.SetReadingText(original + QStringLiteral("\nNewly accepted line"));
    QCOMPARE(pane->textCursor().anchor(), anchor);
    QCOMPARE(pane->textCursor().position(), position);
    QCOMPARE(pane->textCursor().selectedText(), selection);
    QCOMPARE(scroll->value(), previousScroll);
}

void UltraClearTextTests::readingPaneRemapsChangedText_data()
{
    QTest::addColumn<QString>("previous");
    QTest::addColumn<QString>("next");
    QTest::addColumn<int>("anchor");
    QTest::addColumn<int>("position");
    QTest::addColumn<int>("expectedAnchor");
    QTest::addColumn<int>("expectedPosition");
    QTest::newRow("prepend-reverse-selection")
        << QStringLiteral("Reading printed words") << QStringLiteral("New first line\nReading printed words")
        << 10 << 6 << 25 << 21;
    QTest::newRow("remove-prefix")
        << QStringLiteral("New first line\nReading printed words") << QStringLiteral("Reading printed words")
        << 25 << 21 << 10 << 6;
    QTest::newRow("shorter-replaced-middle")
        << QStringLiteral("abcdefghi") << QStringLiteral("abz") << 7 << 5 << 3 << 3;
    QTest::newRow("new-surrogate-in-replaced-middle")
        << QStringLiteral("abcde") << QStringLiteral("a\U0001F642") << 2 << 2 << 1 << 1;
}

void UltraClearTextTests::readingPaneRemapsChangedText()
{
    QFETCH(QString, previous);
    QFETCH(QString, next);
    QFETCH(int, anchor);
    QFETCH(int, position);
    QFETCH(int, expectedAnchor);
    QFETCH(int, expectedPosition);
    UltraClearTextPanel panel;
    panel.SetReadingText(previous);
    QTextCursor cursor = panel.readingPane()->textCursor();
    cursor.setPosition(anchor);
    cursor.setPosition(position, QTextCursor::KeepAnchor);
    panel.readingPane()->setTextCursor(cursor);
    panel.SetReadingText(next);
    QCOMPARE(panel.readingPane()->textCursor().anchor(), expectedAnchor);
    QCOMPARE(panel.readingPane()->textCursor().position(), expectedPosition);
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

void UltraClearTextTests::panelStatusUpdatesCachedAccessibleTextImmediately()
{
    UltraClearTextPanel panel;
    panel.SetStatus(QStringLiteral("Reading text…"), true);
    // A screen reader retains the interface while the runtime changes state.
    // Do not drain Qt events: the advertised text must match the new state
    // immediately, independently of announcement/diagnostic timers.
    auto* accessible = QAccessible::queryAccessibleInterface(panel.statusLabel());
    QVERIFY(accessible);
    for (const auto& source : {
             QStringLiteral("Paused while Extra Stable is on. The original camera image is shown."),
             QStringLiteral("Reading text…"),
             QStringLiteral("No steady printed text recognized yet."),
             QStringLiteral("Ultra Clear Text is off.")}) {
        panel.SetStatus(source);
        QCOMPARE(accessible->text(QAccessible::Name), source);
        QCOMPARE(panel.statusLabel()->text(), source);
    }
    QCoreApplication::processEvents();
    QCOMPARE(accessible->text(QAccessible::Name), panel.statusLabel()->text());
    panel.enableCheckbox()->setChecked(true);
    panel.SetBackendStatus({BackendState::kReady,"PaddleOCR / TensorRT CUDA","","",true});
    auto* backend=QAccessible::queryAccessibleInterface(panel.backendLabel());
    QVERIFY(backend);QVERIFY(!panel.backendLabel()->isHidden());
    QCOMPARE(backend->text(QAccessible::Name),QString("Using GPU backend: PaddleOCR / TensorRT CUDA"));
    panel.SetBackendStatus({BackendState::kReady,"Windows.Media.Ocr","","",false});
    QCOMPARE(backend->text(QAccessible::Name),QString("Using CPU backend: Windows.Media.Ocr"));
    QCOMPARE(panel.backendLabel()->text(),backend->text(QAccessible::Name));
    panel.SetBackendStatus({BackendState::kFailed,"Windows.Media.Ocr","",""});
    QVERIFY(panel.backendLabel()->isHidden());QVERIFY(backend->text(QAccessible::Name).isEmpty());
    // Recognized text uses a stable control name and a changing text pattern.
    auto* pane = QAccessible::queryAccessibleInterface(panel.readingPane());
    QVERIFY(pane && pane->textInterface());
    panel.SetReadingText(QStringLiteral("first result"));
    QCOMPARE(pane->textInterface()->text(0, pane->textInterface()->characterCount()), QStringLiteral("first result"));
    panel.SetReadingText(QStringLiteral("new result"));
    QCOMPARE(pane->textInterface()->text(0, pane->textInterface()->characterCount()), QStringLiteral("new result"));
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
