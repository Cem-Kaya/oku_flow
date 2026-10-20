#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/common/uct_model_pins.hpp"
#include "okuflow/cuda/uct_analysis_pool.hpp"
#include "ultra_clear_text_test_support.hpp"
#include "uct_startup_test_cases.hpp"
#include "uct_batch_diagnostic.hpp"
#include "uct_character_diagnostic.hpp"
#include "uct_resident_diagnostic.hpp"
#include <QFontDatabase>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QtTest>
#include <limits>
#include <stdexcept>
#include <mutex>
#include <optional>
#if OKUFLOW_ENABLE_CUDA
#include "okuflow/cuda/uct_analysis_pool.hpp"
#include "okuflow/cuda/uct_kernels.hpp"
#endif

namespace okuflow::uct
{
// Exercise the production cap/commit path without an optional TensorRT SDK.
struct TensorRtCropDiagnosticTestAccess
{
    static bool TryCommit(TensorRtCropDiagnostic& diagnostic,
                          TensorRtCropDiagnostic::Plan&& plan) noexcept
    {
        static_assert(noexcept(diagnostic.TryCommit(std::move(plan))));
        return diagnostic.TryCommit(std::move(plan));
    }
    static const TensorRtCropDiagnostic::Plan& PlanAt(const TensorRtCropDiagnostic& diagnostic,
                                                     std::size_t index)
    {
        return diagnostic.plans_.at(index);
    }
};
} // namespace okuflow::uct

using namespace okuflow::uct;
namespace
{
struct BackendCalls
{
    int initialized{}, calls{};
    bool preparing{true}, fail{}, inputFault{};
};
class FakeBackend final : public Recognizer
{
  public:
    FakeBackend(BackendCalls &calls, int behavior, bool device = false)
        : calls_(calls), behavior_(behavior), device_(device)
    {
    }
    BackendStatus Initialize() override
    {
        ++calls_.initialized;
        if (behavior_ == 4)
            throw std::runtime_error("initialization failure");
        return {behavior_ == 1 ? BackendState::kUnavailable : BackendState::kReady, "fake", "", ""};
    }
    RecognitionOutput Recognize(const QImage &, const std::atomic<bool> &) override
    {
        ++calls_.calls;
        RecognitionOutput output;
        if (behavior_ == 5)
            throw std::runtime_error("runtime failure");
        output.ok = (behavior_ == 0 || behavior_ == 7) && !calls_.fail && !calls_.inputFault;
        output.cancelled = behavior_ == 3;
        output.inputFault = behavior_ == 6 || calls_.inputFault;
        output.error = "runtime failure";
        return output;
    }
    BackendStatus Status() const override
    {
        return {behavior_ == 1 || behavior_ == 4 ? BackendState::kUnavailable
                                                 : BackendState::kReady,
                "fake", "", ""};
    }
    bool Preparing() const override
    {
        return behavior_ == 7 && calls_.preparing;
    }
    bool WantsDeviceInput() const override
    {
        return device_;
    }
    RecognitionOutput RecognizeDevice(const DeviceImage &image,
                                      const std::atomic<bool> &cancelled) override
    {
        return device_ ? Recognize({}, cancelled) : Recognizer::RecognizeDevice(image, cancelled);
    }

  private:
    BackendCalls &calls_;
    int behavior_;
    bool device_;
};
class CopyFaultImage final : public DeviceImage
{
  public:
    mutable int copies{};
    const void *Data() const override
    {
        return this;
    }
    bool WaitReady(void *, QString &) const override
    {
        return true;
    }
    QImage Readback(const std::atomic<bool> &, QString &error) const override
    {
        if (++copies == 1)
        {
            error = "one failed input copy";
            return {};
        }
        QImage page(32, 32, QImage::Format_ARGB32);
        page.fill(Qt::white);
        return page;
    }
};
#if OKUFLOW_ENABLE_CUDA
template <class T> struct GpuBuffer
{
    T *data{};
    explicit GpuBuffer(std::size_t count)
    {
        if (cudaMalloc(reinterpret_cast<void **>(&data), count * sizeof(T)) != cudaSuccess)
            throw std::runtime_error("fixture allocation failed");
    }
    ~GpuBuffer()
    {
        cudaFree(data);
    }
};
struct FixtureStream
{
    cudaStream_t value{};
    FixtureStream()
    {
        if (cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking) != cudaSuccess)
            throw std::runtime_error("fixture stream unavailable");
    }
    ~FixtureStream()
    {
        cudaStreamSynchronize(value);
        cudaStreamDestroy(value);
    }
};
#endif
} // namespace
class PaddleTests final : public QObject
{
    Q_OBJECT
  private slots:
    void frozenCropDiagnosticRefusesFifthSourceWithoutChangingLeases()
    {
        class ImmutableInput final : public DeviceImage
        {
          public:
            explicit ImmutableInput(unsigned char marker)
                : pixels_([marker] {
                    std::array<unsigned char, 16 * 8 * 4> pixels{};
                    pixels.fill(marker);
                    return pixels;
                }())
            {
                width = 16; height = 8; pitch = 16 * 4;
                sourceEpoch = 9; frameSequence = marker; processingGeneration = 7;
            }
            const void* Data() const override { return pixels_.data(); }
            bool WaitReady(void*, QString&) const override { return true; }
            QImage Readback(const std::atomic<bool>&, QString&) const override { return {}; }
          private:
            const std::array<unsigned char, 16 * 8 * 4> pixels_;
        };
        TensorRtCropDiagnostic diagnostic;
        std::array<SharedDeviceImage, 5> inputs;
        std::array<std::weak_ptr<const DeviceImage>, 5> weak;
        std::array<const DeviceImage*, 5> identities{};
        std::array<const void*, 5> pixels{};
        // Allocate all five before releasing any external owners. Distinct
        // addresses cannot be an allocator's reuse of an earlier dead input.
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            inputs[i] = std::make_shared<const ImmutableInput>(static_cast<unsigned char>(i + 1));
            weak[i] = inputs[i]; identities[i] = inputs[i].get(); pixels[i] = inputs[i]->Data();
            for (std::size_t previous = 0; previous < i; ++previous) {
                QVERIFY(identities[i] != identities[previous]);
                QVERIFY(pixels[i] != pixels[previous]);
            }
        }
        const auto makePlan = [&](std::size_t i) {
            TensorRtCropDiagnostic::Plan plan;
            plan.source = std::move(inputs[i]);
            plan.detectorSha256 = "fixture-detector";
            TensorRtCropDiagnostic::Crop crop;
            crop.xy = {float(i), 1, float(i + 8), 1, float(i + 8), 7, float(i), 7};
            crop.score = 0.75f; crop.validWidth = 400 + int(i); crop.internalEdges = int(i);
            plan.regions.push_back({QRect(int(i), 1, 8, 6), {crop}});
            return plan;
        };
        for (std::size_t i = 0; i < 4; ++i) {
            auto plan = makePlan(i);
            QVERIFY(TensorRtCropDiagnosticTestAccess::TryCommit(diagnostic, std::move(plan)));
            QCOMPARE(diagnostic.CapturedSources(), int(i + 1));
            QVERIFY(!inputs[i]);
            QCOMPARE(weak[i].use_count(), 1L);
        }
        auto refused = makePlan(4);
        QVERIFY(!TensorRtCropDiagnosticTestAccess::TryCommit(diagnostic, std::move(refused)));
        QCOMPARE(diagnostic.CapturedSources(), 4);
        QCOMPARE(diagnostic.ReplayCalls(), std::uint64_t(0));
        QCOMPARE(refused.source.get(), identities[4]);
        QCOMPARE(weak[4].use_count(), 1L); // still caller-owned, never leased
        refused = {};
        QVERIFY(weak[4].expired());
        for (std::size_t i = 0; i < 4; ++i) {
            const auto& retained = TensorRtCropDiagnosticTestAccess::PlanAt(diagnostic, i);
            QCOMPARE(weak[i].use_count(), 1L); // only the original diagnostic lease
            QCOMPARE(retained.source.get(), identities[i]);
            QCOMPARE(retained.source->Data(), pixels[i]);
            QCOMPARE(retained.source->ValidationData(), pixels[i]);
            QCOMPARE(retained.source->frameSequence, std::uint64_t(i + 1));
            QCOMPARE(retained.source->sourceEpoch, std::uint64_t(9));
            QCOMPARE(retained.source->processingGeneration, std::uint64_t(7));
            const auto* immutablePixels = static_cast<const unsigned char*>(retained.source->Data());
            for (std::size_t byte = 0; byte < 16 * 8 * 4; ++byte)
                QCOMPARE(immutablePixels[byte], static_cast<unsigned char>(i + 1));
            QCOMPARE(retained.detectorSha256, QByteArray("fixture-detector"));
            QCOMPARE(retained.regions.size(), std::size_t(1));
            QCOMPARE(retained.regions[0].bounds, QRect(int(i), 1, 8, 6));
            QCOMPARE(retained.regions[0].crops.size(), std::size_t(1));
            const auto& crop = retained.regions[0].crops[0];
            const std::array<float, 8> expected{float(i), 1, float(i + 8), 1, float(i + 8), 7, float(i), 7};
            for (std::size_t coordinate = 0; coordinate < expected.size(); ++coordinate)
                QCOMPARE(crop.xy[coordinate], expected[coordinate]);
            QCOMPARE(crop.score, 0.75f);
            QCOMPARE(crop.validWidth, 400 + int(i));
            QCOMPARE(crop.internalEdges, int(i));
        }
    }
    void modelPinsMatchPreparationAndFailClosed()
    {
        QFile file(QFINDTESTDATA("../scripts/uct/pins.json"));QVERIFY(file.open(QIODevice::ReadOnly));
        const auto models=QJsonDocument::fromJson(file.readAll()).object()["models"].toArray();
        QCOMPARE(models.size(),qsizetype(6));QCOMPARE(kPaddleModelPins.size(),size_t(6));
        for(const bool detection:{false,true})for(const auto tier:{ModelTier::Tiny,ModelTier::Small,ModelTier::Medium}) {
            const auto* pin=FindPaddleModelPin(detection,tier);QVERIFY(pin);
            const auto value=[](std::string_view text){return QString::fromUtf8(text.data(),qsizetype(text.size()));};
            bool found=false;
            for(const auto& item:models) {
                const auto m=item.toObject();
                if(m["kind"].toString()!=(detection?"det":"rec") || m["tier"].toString()!=ModelTierToken(tier))continue;
                QVERIFY(!found);found=true;
                QCOMPARE(m["directory"].toString(),value(pin->directory));
                QCOMPARE(m["repository"].toString(),value(pin->repository));
                QCOMPARE(m["revision"].toString(),value(pin->revision));
                QCOMPARE(m["files"].toObject()["inference.onnx"].toString(),value(pin->modelSha256));
                QCOMPARE(m["files"].toObject()["inference.yml"].toString(),value(pin->configSha256));
            }
            QVERIFY(found);
            QTemporaryDir absent;const auto assets=LoadPaddleAssets(absent.path(),detection?tier:ModelTier::Tiny,detection?ModelTier::Small:tier);
            QVERIFY(!assets.IsValid());QVERIFY(!assets.error.isEmpty());
        }
        QVERIFY(!FindPaddleModelPin(true,static_cast<ModelTier>(999)));
        QVERIFY(!ParseModelTier("../../custom"));
    }
    void automaticFallback_data()
    {
        QTest::addColumn<int>("behavior");
        QTest::newRow("missing-gpu") << 1;
        QTest::newRow("gpu-runtime-failure") << 2;
        QTest::newRow("cancelled") << 3;
        QTest::newRow("initialization-exception") << 4;
        QTest::newRow("runtime-exception") << 5;
    }
    void automaticFallback()
    {
        QFETCH(int, behavior);
        BackendCalls gpu, cpu;
        std::vector<std::unique_ptr<Recognizer>> candidates;
        candidates.push_back(std::make_unique<FakeBackend>(gpu, behavior));
        candidates.push_back(std::make_unique<FakeBackend>(cpu, 0));
        auto backend = CreateAutomaticRecognizer(std::move(candidates));
        QCOMPARE(backend->Initialize().state, BackendState::kReady);
        std::atomic<bool> cancel{false};
        QImage page(16, 16, QImage::Format_ARGB32);
        auto output = backend->Recognize(page, cancel);
        QCOMPARE(output.ok, behavior != 3);
        QCOMPARE(output.cancelled, behavior == 3);
        QCOMPARE(cpu.calls, behavior == 3 ? 0 : 1);
        const int retiredCalls = gpu.calls;
        output = backend->Recognize(page, cancel);
        if (behavior != 3)
        {
            QVERIFY(output.ok);
            QCOMPARE(gpu.calls, retiredCalls);
            QCOMPARE(cpu.initialized, 1);
        }
        cancel = true;
        const auto before = cpu.calls;
        QVERIFY(backend->Recognize(page, cancel).cancelled);
        QCOMPARE(cpu.calls, before);
    }
    void allBackendsUnavailable()
    {
        BackendCalls first, second;
        std::vector<std::unique_ptr<Recognizer>> candidates;
        candidates.push_back(std::make_unique<FakeBackend>(first, 1));
        candidates.push_back(std::make_unique<FakeBackend>(second, 4));
        auto backend = CreateAutomaticRecognizer(std::move(candidates));
        QCOMPARE(backend->Initialize().state, BackendState::kUnavailable);
        std::atomic<bool> cancelled{false};
        QVERIFY(!backend->Recognize({}, cancelled).ok);
        QCOMPARE(first.calls, 0);
        QCOMPARE(second.calls, 0);
    }
    void copyFaultKeepsFallbackUsableAndCopiesOnce()
    {
        BackendCalls gpu, cpu, windows;
        std::vector<std::unique_ptr<Recognizer>> candidates;
        candidates.push_back(std::make_unique<FakeBackend>(gpu, 2, true));
        candidates.push_back(std::make_unique<FakeBackend>(cpu, 2));
        candidates.push_back(std::make_unique<FakeBackend>(windows, 0));
        auto backend = CreateAutomaticRecognizer(std::move(candidates));
        QCOMPARE(backend->Initialize().state, BackendState::kReady);
        CopyFaultImage image;
        std::atomic<bool> cancelled{false};
        const auto failed = backend->RecognizeDevice(image, cancelled);
        QVERIFY(failed.inputFault);
        QCOMPARE(image.copies, 1);
        QCOMPARE(cpu.calls, 0);
        QCOMPARE(windows.initialized, 0);
        QCOMPARE(backend->Status().state, BackendState::kReady);
        QVERIFY(backend->RecognizeDevice(image, cancelled).ok);
        QCOMPARE(image.copies, 2);
        QCOMPARE(gpu.calls, 1);
        QCOMPARE(cpu.calls, 1);
        QCOMPARE(windows.calls, 1);
    }
    void cpuServesWhileGpuPreparesThenGpuPromotes()
    {
        BackendCalls gpu, cpu;
        std::vector<std::unique_ptr<Recognizer>> candidates;
        candidates.push_back(std::make_unique<FakeBackend>(gpu, 7, true));
        candidates.push_back(std::make_unique<FakeBackend>(cpu, 0));
        auto backend = CreateAutomaticRecognizer(std::move(candidates));
        QCOMPARE(backend->Initialize().state, BackendState::kReady);
        std::atomic<bool> cancelled{false};
        QVERIFY(backend->Recognize({}, cancelled).ok);
        QCOMPARE(cpu.calls, 1);
        QCOMPARE(gpu.calls, 0);
        gpu.preparing = false;
        QVERIFY(backend->WantsDeviceInput());
        QVERIFY(backend->Recognize({}, cancelled).ok);
        QCOMPARE(gpu.calls, 1);
        QCOMPARE(cpu.calls, 1);
        QVERIFY(backend->Recognize({}, cancelled).ok);
        QCOMPARE(gpu.calls, 2);
        QCOMPARE(gpu.initialized, 1);
    }
    void promotedGpuFailureDoesNotResurrectRetiredCpu()
    {
        BackendCalls gpu, cpu, windows;
        std::vector<std::unique_ptr<Recognizer>> candidates;
        candidates.push_back(std::make_unique<FakeBackend>(gpu, 7, true));
        candidates.push_back(std::make_unique<FakeBackend>(cpu, 2));
        candidates.push_back(std::make_unique<FakeBackend>(windows, 0));
        auto backend = CreateAutomaticRecognizer(std::move(candidates));
        QCOMPARE(backend->Initialize().state, BackendState::kReady);
        std::atomic<bool> cancelled{false};
        QVERIFY(backend->Recognize({}, cancelled).ok);
        QCOMPARE(cpu.calls, 1);
        QCOMPARE(windows.calls, 1);
        gpu.preparing = false;
        QVERIFY(backend->Recognize({}, cancelled).ok);
        QCOMPARE(gpu.calls, 1);
        gpu.inputFault = true;
        QVERIFY(backend->Recognize({}, cancelled).inputFault);
        gpu.inputFault = false;
        QVERIFY(backend->Recognize({}, cancelled).ok);
        gpu.fail = true;
        QVERIFY(backend->Recognize({}, cancelled).ok);
        QCOMPARE(cpu.calls, 1);
        QCOMPARE(cpu.initialized, 1);
        QCOMPARE(windows.calls, 2);
        QCOMPARE(windows.initialized, 1);
        QCOMPARE(gpu.initialized, 1);
    }
    void windowsBridgeDefersCpuAndPromotes()
    {
        uct_startup_test::BridgeDefersCpuAndPromotes();
    }
    void missingWindowsBridgeWaitsWithoutStartingCpu()
    {
        uct_startup_test::MissingBridgeWaitsWithoutStartingCpu();
    }
    void failedGpuPreparationUsesCpuThenRetainsWindowsBridge()
    {
        uct_startup_test::FailedPreparationUsesCpuThenRetainsBridge();
    }
    void windowsBridgeCancellationAndInputFaultDoNotRetire()
    {
        uct_startup_test::BridgeCancellationAndInputFaultDoNotRetire();
    }
    void analysisAllocationBackoffAndUnknownCompletionQuarantine()
    {
        AnalysisCaptureHealth health;
        QVERIFY(health.CanAttempt(1000));
        health.Failure(1000, false);
        QVERIFY(!health.Failed());
        QVERIFY(!health.CanAttempt(1249));
        QVERIFY(health.CanAttempt(1250));
        health.Failure(1250, false);
        QVERIFY(!health.Failed());
        health.Failure(1500, false);
        QVERIFY(health.Failed());
        health.Success();
        QVERIFY(!health.Failed());
        QVERIFY(health.CanAttempt(1251));
        health.Failure(1300, true);
        QVERIFY(!health.CanAttempt(100000));
        QVERIFY(health.Failed());
    }
    // The Extra Stable record names live or held pixels only when it is
    // internally consistent; composites and inconsistent records name none.
    void holdRecordDecodingFailsClosed()
    {
        using H = HoldDisplay;
        const auto decode = [](HoldProvenanceRecord record) { return DecodeHoldProvenance(record); };
        const auto live = decode({42, 42, 0, 1, 1.0f, 0});
        QCOMPARE(int(live.display), int(H::Live));
        QCOMPARE(live.acquisition, std::uint64_t{42});
        const auto held = decode({40, 40, 1, 1, 0.0f, 0});
        QCOMPARE(int(held.display), int(H::Held));
        QCOMPARE(held.acquisition, std::uint64_t{40});
        QCOMPARE(int(decode({40, 40, 1, 0, 0.0f, 0}).display), int(H::Unknown)); // no valid surface
        QCOMPARE(int(decode({41, 40, 1, 1, 0.0f, 0}).display), int(H::Unknown)); // shown != held
        QCOMPARE(int(decode({0, 40, 2, 1, 0.5f, 0}).display), int(H::Crossfade));
        QCOMPARE(int(decode({0, 40, 2, 1, 1.5f, 0}).display), int(H::Unknown));
        QCOMPARE(int(decode({0, 0, 3, 0, 0.0f, 0}).display), int(H::Unknown));
        SourceStamp stamp;
        stamp.frameSeq = 42;
        QCOMPARE(EvidenceAcquisition(stamp).value_or(0), std::uint64_t{42}); // no hold policy
        auto resolved = stamp;
        ApplyHoldProvenance(resolved, held);
        QCOMPARE(int(resolved.holdDisplay), int(H::Held));
        QCOMPARE(EvidenceAcquisition(resolved).value_or(0), std::uint64_t{40});
        resolved = stamp;
        ApplyHoldProvenance(resolved, live);
        QCOMPARE(EvidenceAcquisition(resolved).value_or(0), std::uint64_t{42});
        const HoldProvenance inconsistent[] = {{H::Live, 41, 1.0f}, {H::Held, 42, 0.0f}, {H::Held, 43, 0.0f},
                                               {H::NotHeld, 42, 0.0f}, {H::Unknown, 40, 0.0f}};
        for (const auto& record : inconsistent)
        {
            resolved = stamp;
            ApplyHoldProvenance(resolved, record);
            QCOMPARE(int(resolved.holdDisplay), int(H::Unknown));
            QVERIFY(!EvidenceAcquisition(resolved));
        }
        resolved = stamp;
        ApplyHoldProvenance(resolved, {H::Crossfade, 40, 0.5f});
        QCOMPARE(int(resolved.holdDisplay), int(H::Crossfade));
        QCOMPARE(resolved.holdAcquisition, std::uint64_t{0});
        QVERIFY(!EvidenceAcquisition(resolved));
        // Hand-built stamps that bypass ApplyHoldProvenance fail closed too:
        // live must name exactly this frame, held a strictly earlier one.
        resolved = stamp;
        resolved.holdDisplay = H::Live;
        resolved.holdAcquisition = 41;
        QVERIFY(!EvidenceAcquisition(resolved));
        resolved.holdDisplay = H::Held;
        resolved.holdAcquisition = 42;
        QVERIFY(!EvidenceAcquisition(resolved));
        resolved.holdAcquisition = 43;
        QVERIFY(!EvidenceAcquisition(resolved));
        resolved.holdAcquisition = 40;
        QCOMPARE(EvidenceAcquisition(resolved).value_or(0), std::uint64_t{40});
    }
    void missingOrTamperedModelsFailClosed()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(!LoadPaddleAssets(directory.path()).IsValid());
        QDir().mkpath(directory.filePath("det"));
        QFile bad(directory.filePath("det/inference.onnx"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not a model");
        bad.close();
        auto backend = CreatePaddleCpuRecognizer(directory.path());
        QCOMPARE(backend->Initialize().state, BackendState::kUnavailable);
    }
    void normalizationAndBounds()
    {
        QImage image(64, 32, QImage::Format_ARGB32);
        image.fill(QColor(255, 0, 0));
        const auto data = PaddleNormalize(image, image.size(), true);
        QCOMPARE(data.size(), 64u * 32u * 3u);
        QVERIFY(std::abs(data[0] - (0 - .485f) / .229f) < .0001);
        QVERIFY(std::abs(data[64 * 32 * 2] - (1 - .406f) / .225f) < .0001);
        QCOMPARE(PaddleDetectionSize({4000, 2256}), QSize(960, 544));
        QVERIFY(PaddleDetectionSize({0, 0}).isEmpty());
    }
    void detectionRejectsNoiseAndMapsSource()
    {
        std::vector<float> map(128 * 64, .01f);
        for (int y = 20; y < 32; ++y)
            for (int x = 16; x < 100; ++x)
                map[y * 128 + x] = .95f;
        map[0] = .9f;
        std::atomic<bool> cancelled{false};
        const auto quads = PaddleDetect(map, 128, 64, {256, 128}, cancelled);
        QCOMPARE(quads.size(), 1);
        QVERIFY(quads[0].points[0].x() < 32);
        QVERIFY(quads[0].points[1].x() > 198);
        cancelled = true;
        QVERIFY(PaddleDetect(map, 128, 64, {256, 128}, cancelled).isEmpty());
    }
    void detailRegionsPreserveSmallTextAndMerge()
    {
        const QSize source(1920,1080);
        const auto regions=PaddleDetectionRegions(source);
        QCOMPARE(regions.size(),3);
        QCOMPARE(regions.first(),QRect(QPoint{},source));
        // The 12-pixel UI lettering lost at 0.5x now reaches the detector
        // at >0.85x, with full coverage and an overlap across the seam.
        for(const QRect& region:regions.sliced(1)) {
            QVERIFY(QRect(QPoint{},source).contains(region));
            QVERIFY(double(PaddleDetectionSize(region.size()).height())/region.height()>.85);
        }
        QVERIFY(regions[1].intersected(regions[2]).width()>=160);
        QCOMPARE(PaddleDetectionRegions({8192,8192}).size(),17);
        QCOMPARE(PaddleDetectionRegions({8193,100}),QVector<QRect>{QRect(0,0,8193,100)});
        QVector<RecognizedLine> output;
        const auto line=[](QString text,QRectF rect,double confidence=0.95) {
            return RecognizedLine{{RecognizedWord{text,rect,true,rect,TokenDisposition::kCandidate,confidence,rect}}};
        };
        PaddleMergeRecognition(output,line("Machines",{920,200,100,18}),regions[0],source);
        const QRect right=regions[2];
        const QRectF local(920-right.x(),200,100,18);
        PaddleMergeRecognition(output,line("Machines",local),right,source);
        QCOMPARE(output.size(),1);
        QCOMPARE(output[0].words.size(),1);
        QCOMPARE(output[0].words[0].boundsPx,QRectF(920,200,100,18));
        QCOMPARE(output[0].words[0].inkSearchPx,output[0].words[0].boundsPx);
        QCOMPARE(output[0].words[0].protectedBoundsPx,output[0].words[0].boundsPx);
        // An internal-edge fragment must not erase the global complete word.
        PaddleMergeRecognition(output,line("ines",{0,200,80,18}),right,source);
        QCOMPARE(output[0].words[0].text,QString("Machines"));
        // A weaker crop cannot erase the reliable word, even partially.
        PaddleMergeRecognition(output,line("Noise",local,.4),right,source);
        QCOMPARE(output[0].words[0].text,QString("Machines"));
        PaddleMergeRecognition(output,line("Small",{300,500,50,12}),right,source);
        QCOMPARE(output.size(),2);
        QCOMPARE(output[1].words[0].boundsPx,QRectF(right.x()+300,500,50,12));
        const PaddleQuad paragraph{{QPointF(20,30),QPointF(1220,30),QPointF(1220,48),QPointF(20,48)},.99f};
        const auto windows=PaddleRecognitionWindows(paragraph);
        QVERIFY(windows.size()>2 && windows.size()<=9);
        QCOMPARE(windows.first().points,paragraph.points);
        QCOMPARE(windows[1].internalEdges,2);
        QCOMPARE(windows.last().internalEdges,1);
        QCOMPARE(windows.last().points[1],paragraph.points[1]);
        for(int i=1;i<windows.size();++i) {
            const auto& q=windows[i];
            QVERIFY(q.points[1].x()-q.points[0].x()<=20*18+.001);
            if(i>1)QVERIFY(windows[i-1].points[1].x()-q.points[0].x()>=8*18-1);
        }
        for(const double aspect:{104.0,132.0,160.0,200.0}) {
            const double h=12,w=aspect*h;
            const auto capped=PaddleRecognitionWindows({{QPointF(0,0),QPointF(w,0),QPointF(w,h),QPointF(0,h)},1});
            QCOMPARE(capped.size(),9);
            QCOMPARE(capped[1].points[0],QPointF(0,0));
            QVERIFY(std::abs(capped.last().points[1].x()-w)<.001);
            for(int i=2;i<capped.size();++i)
                QVERIFY2(capped[i-1].points[1].x()-capped[i].points[0].x()>=8*h-.001,"capped windows must retain context and cover seams");
        }
        // Replacements stay in their logical line, including at capacity.
        QVector<RecognizedLine> merged;
        RecognizedLine row;
        for(int i=0;i<5;++i) row.words+=line(QString(QChar(static_cast<char16_t>('A'+i))),{100.0+30*i,100,20,12}).words;
        PaddleMergeRecognition(merged,row,regions[0],source);
        PaddleMergeRecognition(merged,line("X",{160,100,20,12},1),regions[0],source);
        QCOMPARE(merged.size(),1);
        QStringList order;for(const auto& word:merged[0].words)order<<word.text;
        QCOMPARE(order.join(' '),QString("A B X D E"));
        for(int i=1;i<1024;++i)merged.push_back(line("Other",{100,100.0+20*i,20,12}));
        PaddleMergeRecognition(merged,line("Y",{160,100,20,12},1),regions[0],source);
        QCOMPARE(merged.size(),1024);
        QCOMPARE(merged[0].words[2].text,QString("Y"));
        const QStringList dict{"","A","B","C"," "};
        const std::array<PaddleToken,5> tokens{{{1,0,5,.99f},{4,6,6,.99f},{2,45,50,.99f},{4,51,51,.99f},{3,94,99,.99f}}};
        for(int edges=0;edges<4;++edges) {
            PaddleQuad window{{QPointF(0,0),QPointF(200,0),QPointF(200,10),QPointF(0,10)},1,edges};
            const auto decoded=PaddleDecodeTokens(tokens,100,window,dict);
            QStringList text;for(const auto& word:decoded.words)text<<word.text;
            const QString expected=edges==0?"A B C":edges==1?"B C":edges==2?"A B":"B";
            QCOMPARE(text.join(' '),expected);
        }
    }
    void optionalTensorRtBatchComparison()
    {
        uct_batch_test::CompareTensorRtBatches();
    }
    void residentDiagnosticSelectionAndComparison()
    {
        uct_resident_test::SelectionAndComparisonSelfTest();
    }
    void optionalResidentBaseline()
    {
        uct_resident_test::ResidentBaselineDiagnostic();
    }
    void optionalTensorRtRetirement()
    {
        uct_batch_test::TensorRtRetirementDiagnostic();
    }
    void optionalCharacterChangeCpu()
    {
        SourceStamp stamp; stamp.sourceEpoch=1; stamp.geometryGeneration=1;
        stamp.viewportWidth=960; stamp.viewportHeight=128;
        stamp.viewTransform={0,0,1,1,0,0,1,1,true};
        uct_character_test::Run([&](const QImage& image,int decimation) {
            ++stamp.frameSeq;
            return MakeProbeFrame(image.constBits(),image.width(),image.height(),image.bytesPerLine(),decimation,stamp);
        },false);
    }
    void optionalCharacterChangeGpu()
    {
        if(qEnvironmentVariable("OKUFLOW_UCT_CHARACTER_DIAGNOSTIC")!="1")
            QSKIP("Explicit character-change diagnostic not selected.");
#if OKUFLOW_ENABLE_CUDA
        int devices{};QVERIFY(cudaGetDeviceCount(&devices)==cudaSuccess&&devices>0);
        FixtureStream stream;GpuBuffer<unsigned char> pixels(960*128*4);AnalysisPool pool;
        std::atomic<bool> cancelled{};SourceStamp stamp;stamp.sourceEpoch=1;stamp.geometryGeneration=1;
        stamp.viewportWidth=960;stamp.viewportHeight=128;stamp.viewTransform={0,0,1,1,0,0,1,1,true};
        uct_character_test::Run([&](const QImage& image,int decimation) {
            if(cudaMemcpyAsync(pixels.data,image.constBits(),image.sizeInBytes(),cudaMemcpyHostToDevice,stream.value)!=cudaSuccess)
                throw std::runtime_error("character fixture upload failed");
            ++stamp.frameSeq;
            auto input=pool.Capture(pixels.data,image.bytesPerLine(),960,128,stream.value,1,stamp.frameSeq,1);
            RecognitionResult result;result.stamp=stamp;QString error;
            if(!input||!input->PrepareResidentAnalysis(result,decimation,true,cancelled,error))
                throw std::runtime_error("character fixture resident analysis failed: "+error.toStdString());
            return result.reference;
        },true);
#else
        QFAIL("Explicit resident diagnostic requires CUDA.");
#endif
    }
    void optionalAutomaticStartup()
    {
        uct_startup_test::AutomaticStartupDiagnostic();
    }
    void optionalDenseImage()
    {
        const auto path=qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE");
        if(path.isEmpty())QSKIP("Explicit local image diagnostic not selected.");
        const QImage image(path);QVERIFY2(!image.isNull(),qPrintable(path));
        const bool gpu=qEnvironmentVariableIsSet("OKUFLOW_TEST_PADDLE_GPU");
        auto backend=gpu?CreateTensorRtRecognizer():CreatePaddleCpuRecognizer();
        const auto initialized=backend->Initialize();
        QVERIFY2(initialized.state==BackendState::kReady,qPrintable(initialized.detail));
        QTRY_VERIFY_WITH_TIMEOUT(!backend->Preparing(),180000);
        std::atomic<bool> cancelled{};QElapsedTimer timer;timer.start();
        const auto output=backend->Recognize(image,cancelled);
        QVERIFY2(output.ok,qPrintable(output.error));
        int words=0;QString text;
        for(const auto& line:output.lines) {
            for(const auto& word:line.words) {
                if(word.disposition!=TokenDisposition::kCandidate)continue;
                ++words;text+=word.text+' ';
            }
            text+='\n';
        }
        qInfo().noquote()<<"Dense image backend="<<backend->Status().name<<"words="<<words
            <<"detailRegions="<<PaddleDetectionRegions(image.size()).size()<<"ms="<<timer.elapsed()<<'\n'<<text;
        QVERIFY(words>0);
    }
    void ctcBlankRepeatSpaceAndConfidence()
    {
        const QStringList dictionary{"", "A", "B", " "};
        const std::array<int, 8> ids{1, 1, 0, 1, 3, 2, 2, 0};
        std::vector<float> values(8 * 4, .001f);
        for (int t = 0; t < 8; ++t)
            values[t * 4 + ids[t]] = .99f;
        PaddleQuad quad{{QPointF(0, 0), QPointF(160, 0), QPointF(160, 48), QPointF(0, 48)}, 1};
        const auto line = PaddleDecode(values, 8, 4, quad, dictionary);
        QCOMPARE(line.words.size(), 2);
        QCOMPARE(line.words[0].text, QString("AA"));
        QCOMPARE(line.words[1].text, QString("B"));
        const double boundary=(line.words[0].boundsPx.right()+line.words[1].boundsPx.left())*.5;
        QVERIFY(line.words[0].paddedBounds && line.words[1].paddedBounds);
        QVERIFY(line.words[0].inkSearchPx.right()<=boundary);
        QVERIFY(line.words[1].inkSearchPx.left()>=boundary);
        QVERIFY(QRectF(0,0,160,48).contains(line.words[0].inkSearchPx));
        QVERIFY(QRectF(0,0,160,48).contains(line.words[1].inkSearchPx));
        QVERIFY(PaddleDecode(values, 8, 3, quad, dictionary).words.isEmpty());
        values[1] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(PaddleDecode(values, 8, 4, quad, dictionary).words.isEmpty());
        const std::array<PaddleToken, 3> weak{{{1,0,0,.99f},{3,1,1,.99f},{2,2,2,.60f}}};
        const auto rejected = PaddleDecodeTokens(weak, 3, quad, dictionary);
        QCOMPARE(rejected.words.size(), 2);
        QCOMPARE(rejected.words[1].text, QStringLiteral("B"));
        QCOMPARE(rejected.words[1].disposition, TokenDisposition::kLowConfidence);
        QVERIFY(std::abs(rejected.words[1].confidence - .60) < .0001);
        QCOMPARE(rejected.words[1].protectedBoundsPx, rejected.words[1].boundsPx);
        QVERIFY(rejected.words[0].inkSearchPx.right() <= rejected.words[1].inkSearchPx.left());
    }
    void decoderPreservesUncertainNegationThroughReading()
    {
        const QString phrase = QStringLiteral("Do not proceed");
        QStringList dictionary{QString()};
        for (const QChar ch : phrase)
            if (!dictionary.contains(QString(ch))) dictionary.push_back(QString(ch));
        const int steps = phrase.size() * 2;
        const int classes = dictionary.size();
        std::vector<float> values(static_cast<size_t>(steps) * classes, .001f);
        for (int i = 0; i < phrase.size(); ++i) {
            const float confidence = i >= 3 && i <= 5 ? .60f : .99f;
            values[static_cast<size_t>(i * 2) * classes + dictionary.indexOf(QString(phrase[i]))] = confidence;
            values[static_cast<size_t>(i * 2 + 1) * classes] = .99f;
        }
        const PaddleQuad quad{{QPointF(20,20),QPointF(620,20),QPointF(620,68),QPointF(20,68)},1};
        const RecognizedLine decoded = PaddleDecode(values, steps, classes, quad, dictionary);
        QCOMPARE(decoded.words.size(), 3);
        QCOMPARE(decoded.words[1].text, QStringLiteral("not"));
        QCOMPARE(decoded.words[1].disposition, TokenDisposition::kLowConfidence);
        QImage pixels(640,128,QImage::Format_ARGB32);
        pixels.fill(Qt::white);
        QPainter painter(&pixels);
        QFont font; font.setPixelSize(32); painter.setFont(font); painter.setPen(Qt::black);
        for (const auto& word : decoded.words) painter.drawText(word.boundsPx, Qt::AlignCenter, word.text);
        painter.end();
        const auto transform = okuflow::ComputeViewTransform(640,128,640,128,1,.5f,.5f,
                                                            okuflow::ViewportFitMode::kFit);
        auto result = [&](std::uint64_t frame, std::int64_t time) {
            RecognitionResult out;
            out.output.ok = true; out.output.lines.push_back(decoded);
            out.stamp.sourceEpoch=1; out.stamp.geometryGeneration=1;
            out.stamp.frameSeq=frame; out.stamp.timestampMs=time;
            out.stamp.viewportWidth=640; out.stamp.viewportHeight=128; out.stamp.viewTransform=transform;
            const auto observed=uct_test::IndependentFramePixels(pixels,frame);
            out.sourcePixelDigest=uct_test::SourcePixelsDigest(observed);
            out.reference=MakeProbeFrame(observed.constBits(),640,128,observed.bytesPerLine(),1,out.stamp);
            out.prepared=PrepareLines(out,7);
            return out;
        };
        const auto first=result(10,100), second=result(11,200);
        QCOMPARE(second.prepared[0].words.size(),3);
        QCOMPARE(second.prepared[0].words[1].disposition,TokenDisposition::kLowConfidence);
        QVERIFY(!second.prepared[0].words[1].protectedSceneRect.isEmpty());
        QVERIFY(second.prepared[0].reference.valid);
        OverlayModel model;
        QCOMPARE(model.Admit(first,first.stamp,nullptr,110),AdmitVerdict::kPending);
        QCOMPARE(model.Admit(second,second.stamp,nullptr,210),AdmitVerdict::kAccepted);
        QCOMPARE(model.ReadingText(210,11),QStringLiteral("Do \u2026 proceed"));
        const auto words=model.SceneWords(210,11);
        QCOMPARE(words.size(),2);
        QCOMPARE(words[0].text,QStringLiteral("Do"));
        QCOMPARE(words[1].text,QStringLiteral("proceed"));
        for(const auto& word:words) {
            QCOMPARE(word.protectedSceneRects.size(),1);
            QCOMPARE(word.protectedSceneRects[0],second.prepared[0].words[1].protectedSceneRect);
        }
    }
    void gpuResidentSchedulerKeepsPixelsAndDescriptorsOnDevice_data()
    {
        QTest::addColumn<bool>("lightInk");
        QTest::addColumn<bool>("mixedColors");
        QTest::newRow("light-on-dark") << true << false;
        QTest::newRow("dark-on-light") << false << false;
        QTest::newRow("selected-yellow-button-in-dark-row") << true << true;
    }
    void gpuResidentSchedulerKeepsPixelsAndDescriptorsOnDevice()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)QSKIP("No CUDA device.");
        QFETCH(bool,lightInk);QFETCH(bool,mixedColors);
        auto page=uct_test::MakePage({"Printed not words",QString::fromUtf8("jig d\xc3\xa9j\xc3\xa0 words"),"Ready not later"},{},640,360,40,
            lightInk?QColor(226,230,234):QColor(18,18,22),lightInk?QColor(66,70,74):QColor(246,244,236));
        if(mixedColors) {
            const auto r=page.words.first().sceneRect;
            const auto selected=QRectF(r.x()*640-10,r.y()*360-10,r.width()*640+20,r.height()*360+20).toAlignedRect().intersected(page.image.rect());
            const QColor paper(240,190,96);
            for(int y=selected.top();y<=selected.bottom();++y) for(int x=selected.left();x<=selected.right();++x) {
                const double ink=std::clamp((qGray(page.image.pixel(x,y))-69.0)/160.0,0.0,1.0);
                page.image.setPixelColor(x,y,QColor(int(paper.red()*(1-ink)),int(paper.green()*(1-ink)),int(paper.blue()*(1-ink))));
            }
        }
        RecognitionOutput output;output.ok=true;
        for(int i=0;i<page.words.size();++i) {
            if(i%3==0)output.lines.push_back({});
            const auto& word=page.words[i];const auto r=word.sceneRect;
            RecognizedWord w{word.text,{r.x()*640-5,r.y()*360-3,r.width()*640+10,r.height()*360+6}};
            w.paddedBounds=true;
            if(word.text=="not")w.disposition=TokenDisposition::kLowConfidence;
            output.lines.back().words.push_back(w);
        }
        struct Backend final:Recognizer {
            RecognitionOutput output;bool gpu{};
            BackendStatus Initialize() override{return {BackendState::kReady,"fixture","",""};}
            bool WantsDeviceInput() const override{return gpu;}
            RecognitionOutput Recognize(const QImage&,const std::atomic<bool>&) override{return output;}
            RecognitionOutput RecognizeDevice(const DeviceImage&,const std::atomic<bool>&) override{return output;}
        };
        struct Guard final:DeviceImage {
            SharedDeviceImage owned;mutable std::atomic<int> downloads{};bool fail{};
            explicit Guard(SharedDeviceImage value):owned(std::move(value)) {
                width=owned->width;height=owned->height;device=owned->device;pitch=owned->pitch;
                sourceEpoch=owned->sourceEpoch;frameSequence=owned->frameSequence;
            }
            const void* Data() const override{return owned->Data();}
            bool WaitReady(void* stream,QString& e) const override{return owned->WaitReady(stream,e);}
            bool HasResidentAnalysis() const override{return true;}
            bool PrepareResidentAnalysis(RecognitionResult& r,int d,bool probe,const std::atomic<bool>& c,QString& e) const override {
                if(fail){e="forced resident failure";return false;}
                return owned->PrepareResidentAnalysis(r,d,probe,c,e);
            }
            QImage Readback(const std::atomic<bool>&,QString&) const override{++downloads;return {};}
            QImage ReadProbe(const std::atomic<bool>&,QString&) const override{++downloads;return {};}
        };
        FixtureStream stream;GpuBuffer<unsigned char> pixels(page.image.sizeInBytes());AnalysisPool pool;
        QCOMPARE(cudaMemcpyAsync(pixels.data,page.image.constBits(),page.image.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        auto guard=std::make_shared<Guard>(pool.Capture(pixels.data,page.image.bytesPerLine(),640,360,stream.value,7,1,1));
        SourceStamp stamp;stamp.sourceEpoch=7;stamp.geometryGeneration=1;stamp.frameSeq=1;stamp.timestampMs=100;
        stamp.viewportWidth=640;stamp.viewportHeight=360;stamp.viewTransform={0,0,1,1,0,0,1,1,true};stamp.liveCameraCapture=true;
        std::mutex mutex;std::optional<RecognitionResult> cpu,gpu,probe;
        auto cb=std::make_unique<Backend>();cb->output=output;
        auto gb=std::make_unique<Backend>();gb->output=output;gb->gpu=true;
        RecognitionScheduler cpuScheduler(std::move(cb),[&](auto r){std::lock_guard lock(mutex);cpu=std::move(r);},{});
        RecognitionScheduler gpuScheduler(std::move(gb),[&](auto r){std::lock_guard lock(mutex);gpu=std::move(r);},{});
        RecognitionScheduler probeScheduler(nullptr,[&](auto r){std::lock_guard lock(mutex);probe=std::move(r);},{},1,7,true);
        QVERIFY(cpuScheduler.Submit(stamp,page.image));QVERIFY(gpuScheduler.SubmitDevice(stamp,guard));QVERIFY(probeScheduler.SubmitDevice(stamp,guard));
        const auto done=[&]{std::lock_guard lock(mutex);return cpu&&gpu&&probe;};QTRY_VERIFY_WITH_TIMEOUT(done(),10000);
        {
        std::lock_guard lock(mutex);
        QVERIFY2(gpu->output.ok,qPrintable(gpu->output.error));QVERIFY(probe->output.ok);QVERIFY(cpu->output.ok);
        QCOMPARE(guard->downloads.load(),0);QVERIFY(gpu->sourcePixelDigest.isEmpty());QVERIFY(probe->sourcePixelDigest.isEmpty());
        for(const auto* r:{&*gpu,&*probe}){QVERIFY(r->reference.IsValid());QVERIFY(r->reference.resident);QVERIFY(r->reference.luma.isNull());QVERIFY(r->reference.integral.isEmpty());}
        QCOMPARE(gpu->lineColors.size(),cpu->lineColors.size());
        QCOMPARE(gpu->lineColors.size(),3);QCOMPARE(gpu->prepared.size(),cpu->prepared.size());
        for(int row=0;row<output.lines.size();++row){
            QCOMPARE(gpu->lineColors[row].foreground,cpu->lineColors[row].foreground);
            QCOMPARE(gpu->lineColors[row].background,cpu->lineColors[row].background);
            QVERIFY(gpu->prepared[row].reference.resident);QVERIFY(gpu->prepared[row].reference.values.isEmpty());
            for(int i=0;i<output.lines[row].words.size();++i){
                const auto& gc=gpu->output.lines[row].words[i].colors;
                const auto& cc=cpu->output.lines[row].words[i].colors;
                QVERIFY(gc);QVERIFY(cc);
                QCOMPARE(gc->foreground,cc->foreground);QCOMPARE(gc->background,cc->background);
                QVERIFY(gpu->prepared[row].words[i].colors);
                QCOMPARE(gpu->prepared[row].words[i].colors->background,gc->background);
                QCOMPARE(gpu->output.lines[row].words[i].boundsPx,cpu->output.lines[row].words[i].boundsPx);
                QCOMPARE(gpu->output.lines[row].words[i].protectedBoundsPx,output.lines[row].words[i].boundsPx);
            }
        }
        if(mixedColors) {
            const auto selected=*gpu->output.lines[0].words[0].colors;
            const auto neighbor=*gpu->output.lines[0].words[1].colors;
            QVERIFY(selected.background.red()>200 && selected.background.green()>140 && selected.background.blue()<140);
            QVERIFY(qGray(selected.foreground.rgb())<100);
            QVERIFY(qGray(neighbor.background.rgb())<120 && qGray(neighbor.foreground.rgb())>180);
        }
        const auto patch=gpu->prepared[0].reference;
        const auto checked=CheckLines({{42,patch,{}}},probe->reference,{},0);
        QCOMPARE(checked.size(),1);QVERIFY(checked[0].checked);QVERIFY(checked[0].match.matched);QVERIFY(checked[0].match.score>.99);
        const auto similarity=SceneSimilarity(gpu->reference,probe->reference);QVERIFY(similarity);QVERIFY(*similarity>.99);
        // UI-facing matching cannot launch/wait for a GPU kernel.
        QVERIFY(!MatchPatch(patch,probe->reference).matched);
        auto malformed=*gpu;malformed.prepared.clear();OverlayModel model;
        QCOMPARE(model.AdmitLiveCamera(malformed,stamp,nullptr,110),AdmitVerdict::kFailed);
        QCOMPARE(model.Validate(probe->reference,110).checked,0);
        const auto cpuPatch=cpu->prepared[0].reference;
        const auto batch=CheckLines({{41,patch,{}},{42,{},{}},{43,cpuPatch,{}}},probe->reference,{},0);
        QCOMPARE(batch.size(),3);QVERIFY(batch[0].match.matched);QVERIFY(!batch[1].match.matched);QVERIFY(batch[2].match.matched);
        for(int i=0;i<3;++i){QVERIFY(batch[i].checked);QCOMPARE(batch[i].lineId,std::uint64_t(41+i));}
        std::atomic<bool> cancelled{true};
        const auto dropped=CheckLines({{41,patch,{}}},probe->reference,{},0,&cancelled);
        QCOMPARE(dropped.size(),1);QVERIFY(!dropped[0].checked);
        }
        CorroborationRequest proof;proof.selectedJobId=4242;proof.tuning.minimumScore=.995;proof.tuning.searchRadiusPx=10;
        {std::lock_guard lock(mutex);proof.references={gpu->prepared[0].reference,{},cpu->prepared[0].reference};gpu.reset();}
        QVERIFY(gpuScheduler.SubmitDevice(stamp,guard,proof));QTRY_VERIFY_WITH_TIMEOUT(done(),10000);
        {std::lock_guard lock(mutex);QVERIFY(gpu->output.ok);QCOMPARE(gpu->corroboratedJobId,proof.selectedJobId);
            QCOMPARE(gpu->corroborationTuning.minimumScore,proof.tuning.minimumScore);QCOMPARE(gpu->corroborationTuning.searchRadiusPx,10);
            QCOMPARE(gpu->corroborationMatches,QVector<bool>({true,false,true}));}
        // Resident failures are input faults, never a hidden download fallback.
        guard->fail=true;
        {std::lock_guard lock(mutex);gpu.reset();probe.reset();}
        QVERIFY(gpuScheduler.SubmitDevice(stamp,guard));QVERIFY(probeScheduler.SubmitDevice(stamp,guard));
        QTRY_VERIFY_WITH_TIMEOUT(done(),10000);
        {std::lock_guard lock(mutex);QVERIFY(!gpu->output.ok);QVERIFY(gpu->output.inputFault);QVERIFY(!probe->output.ok);QVERIFY(probe->output.inputFault);}
        QCOMPARE(guard->downloads.load(),0);
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    void gpuResidentValidationMatchesCpuUnderMotionOcclusionAndCrop()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)QSKIP("No CUDA device.");
        const auto page=uct_test::MakePage({"Printed words"},{},640,360,40);
        FixtureStream stream;GpuBuffer<unsigned char> pixels(page.image.sizeInBytes());AnalysisPool pool;
        std::atomic<bool> cancel{false};
        SourceStamp stamp;stamp.sourceEpoch=1;stamp.geometryGeneration=1;stamp.frameSeq=1;stamp.viewportWidth=640;stamp.viewportHeight=360;
        stamp.viewTransform={0,0,1,1,0,0,1,1,true};stamp.liveCameraCapture=true;
        for(const int decimation:{1,2,4}) {
        const auto prepare=[&](const QImage& input,int seq){
            cudaMemcpyAsync(pixels.data,input.constBits(),input.sizeInBytes(),cudaMemcpyHostToDevice,stream.value);
            auto owned=pool.Capture(pixels.data,input.bytesPerLine(),640,360,stream.value,1,seq,1);
            RecognitionResult r;r.stamp=stamp;r.stamp.frameSeq=seq;QString error;
            if(owned)owned->PrepareResidentAnalysis(r,decimation,true,cancel,error);return r.reference;
        };
        const auto base=prepare(page.image,1);QVERIFY(base.IsValid());
        QRectF rect=page.words.front().sceneRect.united(page.words.back().sceneRect);
        const auto gpuPatch=BuildReferencePatch(base,rect);QVERIFY(gpuPatch.valid);QVERIFY(gpuPatch.resident);
        const auto cpuBase=MakeProbeFrame(page.image.constBits(),640,360,page.image.bytesPerLine(),decimation,stamp);
        const auto cpuPatch=BuildReferencePatch(cpuBase,rect);QVERIFY(cpuPatch.valid);
        for(int phase=0;phase<4;++phase){
            QImage image(640,360,QImage::Format_ARGB32);image.fill(phase==2?Qt::white:QColor(246,244,236));
            if(phase!=2){QPainter painter(&image);painter.drawImage(phase==1?QPoint(3,2):QPoint(),page.image);}
            auto gpuProbe=prepare(image,phase+2);QVERIFY(gpuProbe.IsValid());
            auto cpuProbe=MakeProbeFrame(image.constBits(),640,360,image.bytesPerLine(),decimation,stamp);
            if(phase==3){ // Reproject both references through a cropped/panned view.
                gpuProbe.stamp.viewTransform={.1f,.05f,.8f,.9f,0,0,1,1,true};cpuProbe.stamp=gpuProbe.stamp;
            }
            const QPointF seed=phase==1?QPointF(3./640,2./360):QPointF();
            for(const int radius:{6,1000,-3}) {
                MatchTuning tuning;tuning.searchRadiusPx=radius;
                const auto c=MatchPatch(cpuPatch,cpuProbe,tuning,seed);
                // Native GPU descriptor and compact CPU descriptor upload must agree.
                for(const auto& patch:{gpuPatch,cpuPatch}) {
                    const auto g=CheckLines({{7,patch,seed}},gpuProbe,tuning,0).front().match;
                    QCOMPARE(g.matched,c.matched);QCOMPARE(g.visible,c.visible);
                    QVERIFY2(std::abs(g.score-c.score)<1e-5,qPrintable(QString("decimation %1 phase %2 radius %3 GPU %4 CPU %5").arg(decimation).arg(phase).arg(radius).arg(g.score,0,'g',14).arg(c.score,0,'g',14)));
                    QVERIFY(std::abs(g.sceneOffset.x()-c.sceneOffset.x())<1e-5);QVERIFY(std::abs(g.sceneOffset.y()-c.sceneOffset.y())<1e-5);
                }
            }
            const auto a=SceneSimilarity(base,gpuProbe),b=SceneSimilarity(cpuBase,cpuProbe);
            QCOMPARE(a.has_value(),b.has_value());if(a)QVERIFY2(std::abs(*a-*b)<1e-5,qPrintable(QString("similarity phase %1 GPU %2 CPU %3").arg(phase).arg(*a,0,'g',15).arg(*b,0,'g',15)));
        }
        }
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    void gpuResidentBatchMappingMatchesDistinctCpuLines()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)QSKIP("No CUDA device.");
        const auto page=uct_test::MakePage({"Printed words","Second choice","Final sentence"},{},640,360,30);
        FixtureStream stream;GpuBuffer<unsigned char> pixels(page.image.sizeInBytes());AnalysisPool pool;
        std::atomic<bool> cancel{false};SourceStamp stamp;stamp.sourceEpoch=1;stamp.frameSeq=1;
        stamp.viewportWidth=640;stamp.viewportHeight=360;stamp.viewTransform={0,0,1,1,0,0,1,1,true};stamp.liveCameraCapture=true;
        const auto prepare=[&](const QImage& input,int seq){
            cudaMemcpyAsync(pixels.data,input.constBits(),input.sizeInBytes(),cudaMemcpyHostToDevice,stream.value);
            auto owned=pool.Capture(pixels.data,input.bytesPerLine(),640,360,stream.value,1,seq,1);
            RecognitionResult r;r.stamp=stamp;r.stamp.frameSeq=seq;QString error;
            if(owned)owned->PrepareResidentAnalysis(r,2,true,cancel,error);return r.reference;
        };
        const auto base=prepare(page.image,1);QVERIFY(base.IsValid());
        const auto cpuBase=MakeProbeFrame(page.image.constBits(),640,360,page.image.bytesPerLine(),2,stamp);
        QVector<QRectF> rects;for(int i=0;i<3;++i)rects.push_back(page.words[i*2].sceneRect.united(page.words[i*2+1].sceneRect));
        const auto gpuRefs=base.resident->References(stamp,rects);
        QVector<ReferencePatch> cpuRefs;for(const auto& rect:rects)cpuRefs.push_back(BuildReferencePatch(cpuBase,rect));
        for(int i=0;i<3;++i){QVERIFY(gpuRefs[i].valid);QVERIFY(cpuRefs[i].valid);}
        auto changed=page.image.copy();
        {QPainter painter(&changed);painter.fillRect(QRect(0,120,640,46),QColor(246,244,236));
            painter.fillRect(QRect(0,170,640,65),QColor(246,244,236));
            painter.drawImage(QPoint(3,172),page.image,QRect(0,170,640,65));}
        const auto gpuProbe=prepare(changed,2);QVERIFY(gpuProbe.IsValid());
        const auto cpuProbe=MakeProbeFrame(changed.constBits(),640,360,changed.bytesPerLine(),2,stamp);
        const QPointF seed(3./640,2./360);
        const auto results=CheckLines({{10,gpuRefs[0],{}},{11,{},{}},{12,gpuRefs[1],{}},{13,cpuRefs[2],seed}},gpuProbe,{},0);
        const QVector<PatchMatch> expected{MatchPatch(cpuRefs[0],cpuProbe),{},MatchPatch(cpuRefs[1],cpuProbe),MatchPatch(cpuRefs[2],cpuProbe,{},seed)};
        QCOMPARE(results.size(),4);QVERIFY(expected[0].matched);QVERIFY(!expected[2].matched);QVERIFY(expected[3].matched);
        for(int i=0;i<4;++i){QVERIFY(results[i].checked);QCOMPARE(results[i].lineId,std::uint64_t(10+i));
            QCOMPARE(results[i].match.matched,expected[i].matched);QCOMPARE(results[i].match.visible,expected[i].visible);
            QVERIFY(std::abs(results[i].match.score-expected[i].score)<1e-5);
            QVERIFY(std::abs(results[i].match.sceneOffset.x()-expected[i].sceneOffset.x())<1e-5);
            QVERIFY(std::abs(results[i].match.sceneOffset.y()-expected[i].sceneOffset.y())<1e-5);}
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    void gpuResidentShiftedWordReferenceKeepsSamplesOnDevice()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)QSKIP("No CUDA device.");
        const auto page=uct_test::MakeLayoutPage({{"Millwright",{90,70},32}},640,360);
        const auto moved=uct_test::MakeLayoutPage({{"Millwright",{90,150},32}},640,360);
        FixtureStream stream;GpuBuffer<unsigned char> pixels(page.image.sizeInBytes());AnalysisPool pool;
        std::atomic<bool> cancel{false};SourceStamp stamp;stamp.sourceEpoch=1;stamp.geometryGeneration=1;
        stamp.viewportWidth=640;stamp.viewportHeight=360;stamp.viewTransform={0,0,1,1,0,0,1,1,true};
        const auto prepare=[&](const QImage& image,int sequence){
            cudaMemcpyAsync(pixels.data,image.constBits(),image.sizeInBytes(),cudaMemcpyHostToDevice,stream.value);
            auto owned=pool.Capture(pixels.data,image.bytesPerLine(),640,360,stream.value,1,sequence,sequence);
            RecognitionResult r;r.stamp=stamp;r.stamp.frameSeq=sequence;QString error;
            if(owned)owned->PrepareResidentAnalysis(r,2,true,cancel,error);return r.reference;
        };
        const auto source=prepare(page.image,1),neighbor=prepare(moved.image,2);
        QVERIFY(source.resident);QVERIFY(neighbor.resident);QVERIFY(neighbor.luma.isNull());
        auto refs=source.resident->References(source.stamp,{page.words[0].sceneRect});
        QCOMPARE(refs.size(),1);QVERIFY(refs[0].valid);QVERIFY(refs[0].values.isEmpty());
        const MatchTuning tuning{.94,.95,2};
        const auto before=neighbor.resident->Matches(neighbor.stamp,refs,tuning,{});
        QCOMPARE(before.size(),1);QVERIFY(!before[0].matched);
        refs[0].sceneRect.translate(moved.words[0].sceneRect.center()-page.words[0].sceneRect.center());
        const auto matched=neighbor.resident->Matches(neighbor.stamp,refs,tuning,{});
        QCOMPARE(matched.size(),1);QVERIFY(matched[0].matched);
        QVERIFY(refs[0].values.isEmpty());
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    void gpuResidentFingerprintCoversEveryByteAndReleasesCaptureSlots()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)QSKIP("No CUDA device.");
        const auto tree=[](const QImage& page) {
            QByteArray packed;for(int y=0;y<page.height();++y)packed.append(reinterpret_cast<const char*>(page.constScanLine(y)),page.width()*4);
            QVector<QByteArray> nodes;
            for(qsizetype i=0;i<packed.size();i+=4096)nodes.push_back(QCryptographicHash::hash(QByteArray(1,char(0))+packed.mid(i,4096),QCryptographicHash::Sha256));
            while(nodes.size()>1){QVector<QByteArray> next;for(qsizetype i=0;i<nodes.size();i+=16){QByteArray bytes(1,char(1));for(qsizetype k=i;k<std::min(i+16,nodes.size());++k)bytes+=nodes[k];next.push_back(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256));}nodes=std::move(next);}return nodes.front();
        };
        FixtureStream stream;AnalysisPool pool;std::atomic<bool> cancel{false};
        QImage page(641,361,QImage::Format_ARGB32);page.fill(QColor(73,91,133));
        const size_t pitch=page.width()*4+32;GpuBuffer<unsigned char> pixels(pitch*page.height());
        QVector<RecognitionResult> retained;
        for(int frame=1;frame<=8;++frame){
            if(frame==3)page.setPixelColor(640,360,QColor(74,91,133)); // outside bounded probe sampling
            QCOMPARE(cudaMemsetAsync(pixels.data,frame,pitch*page.height(),stream.value),cudaSuccess);
            QCOMPARE(cudaMemcpy2DAsync(pixels.data,pitch,page.constBits(),page.bytesPerLine(),page.width()*4,page.height(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
            auto owned=pool.Capture(pixels.data,pitch,page.width(),page.height(),stream.value,1,frame,1);QVERIFY(owned);
            RecognitionResult r;r.stamp.sourceEpoch=1;r.stamp.frameSeq=frame;r.stamp.geometryGeneration=1;r.stamp.viewportWidth=page.width();r.stamp.viewportHeight=page.height();r.stamp.viewTransform={0,0,1,1,0,0,1,1,true};
            QString error;QVERIFY2(owned->PrepareResidentAnalysis(r,2,false,cancel,error),qPrintable(error));
            QCOMPARE(r.sourcePixelDigest,tree(page));QCOMPARE(r.sourcePixelDigestKind,RecognitionResult::DigestKind::GpuTreeSha256V1);
            if(frame==2)QCOMPARE(r.sourcePixelDigest,retained.back().sourcePixelDigest);
            if(frame==3)QVERIFY(r.sourcePixelDigest!=retained.back().sourcePixelDigest);
            QVERIFY(r.reference.resident);QVERIFY(r.reference.luma.isNull());retained.push_back(std::move(r));owned.reset();
        }
        // Eight retained probes must not pin either of the two full-size slots.
        QCOMPARE(retained.size(),8);cancel=true;QString error;RecognitionResult rejected;
        auto owned=pool.Capture(pixels.data,pitch,page.width(),page.height(),stream.value,1,9,1);QVERIFY(owned);
        QVERIFY(!owned->PrepareResidentAnalysis(rejected,2,true,cancel,error));QVERIFY(!rejected.reference.IsValid());
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    void gpuPairedCaptureKeepsSmoothedOcrAndCurrentValidationApart()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)QSKIP("No CUDA device.");
        const auto tree=[](const QImage& page) {
            QByteArray packed;for(int y=0;y<page.height();++y)packed.append(reinterpret_cast<const char*>(page.constScanLine(y)),page.width()*4);
            QVector<QByteArray> nodes;
            for(qsizetype i=0;i<packed.size();i+=4096)nodes.push_back(QCryptographicHash::hash(QByteArray(1,char(0))+packed.mid(i,4096),QCryptographicHash::Sha256));
            while(nodes.size()>1){QVector<QByteArray> next;for(qsizetype i=0;i<nodes.size();i+=16){QByteArray bytes(1,char(1));for(qsizetype k=i;k<std::min(i+16,nodes.size());++k)bytes+=nodes[k];next.push_back(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256));}nodes=std::move(next);}return nodes.front();
        };
        FixtureStream stream;AnalysisPool pool;std::atomic<bool> cancel{false};
        QImage smoothed(128,64,QImage::Format_ARGB32),current(128,64,QImage::Format_ARGB32);
        for(int y=0;y<64;++y)for(int x=0;x<128;++x){
            smoothed.setPixelColor(x,y,QColor((x*2)%256,(y*4)%256,128));
            current.setPixelColor(x,y,QColor(255-(x*2)%256,(y*3)%256,32));
        }
        GpuBuffer<unsigned char> smoothedPx(smoothed.sizeInBytes()),currentPx(current.sizeInBytes());
        QCOMPARE(cudaMemcpyAsync(smoothedPx.data,smoothed.constBits(),smoothed.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(currentPx.data,current.constBits(),current.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        auto pair=pool.CapturePair(smoothedPx.data,smoothed.bytesPerLine(),currentPx.data,current.bytesPerLine(),
                                   128,64,stream.value,7,42,3,9,40,3,0.25f);
        QVERIFY(pair);
        QVERIFY(pair->kind==AnalysisKind::TemporallySmoothed);
        QCOMPARE(pair->historyEpoch,std::uint64_t{9});
        QCOMPARE(pair->seedFrameSequence,std::uint64_t{40});
        QCOMPARE(pair->blendFrameCount,std::uint64_t{3});
        QCOMPARE(pair->frameSequence,std::uint64_t{42});
        QVERIFY(pair->Data()!=pair->ValidationData());
        QString error;
        const QImage ocr=pair->Readback(cancel,error);QVERIFY2(!ocr.isNull(),qPrintable(error));
        const QImage validation=pair->ReadValidation(cancel,error);QVERIFY2(!validation.isNull(),qPrintable(error));
        QCOMPARE(ocr.convertToFormat(QImage::Format_ARGB32),smoothed);
        QCOMPARE(validation.convertToFormat(QImage::Format_ARGB32),current);
        // Resident fingerprint and probe come from the current frame only.
        RecognitionResult r;r.stamp.sourceEpoch=7;r.stamp.frameSeq=42;r.stamp.geometryGeneration=1;
        r.stamp.viewportWidth=128;r.stamp.viewportHeight=64;r.stamp.viewTransform={0,0,1,1,0,0,1,1,true};
        QVERIFY2(pair->PrepareResidentAnalysis(r,1,false,cancel,error),qPrintable(error));
        QCOMPARE(r.sourcePixelDigest,tree(current));
        QVERIFY(r.sourcePixelDigest!=tree(smoothed));
        QVERIFY(r.reference.resident);
        // Atomicity: with both pair slots leased, no half-pair is published.
        auto pair2=pool.CapturePair(smoothedPx.data,smoothed.bytesPerLine(),currentPx.data,current.bytesPerLine(),
                                    128,64,stream.value,7,43,3,9,40,4,0.25f);
        QVERIFY(pair2);
        QVERIFY(!pool.CapturePair(smoothedPx.data,smoothed.bytesPerLine(),currentPx.data,current.bytesPerLine(),
                                  128,64,stream.value,7,44,3,9,40,5,0.25f));
        QVERIFY(!pool.HasFailure());
        pair.reset();
        auto pair3=pool.CapturePair(smoothedPx.data,smoothed.bytesPerLine(),currentPx.data,current.bytesPerLine(),
                                    128,64,stream.value,7,44,3,9,40,5,0.25f);
        QVERIFY(pair3);
        // A raw reference can be trackable while the smoothed OCR pixels
        // contain different, stale text. Require direct OCR-to-current proof
        // before later raw motion tracking is allowed to admit those words.
        {
            AnalysisPool registration;
            for(int y=0;y<64;++y)for(int x=0;x<128;++x){
                const unsigned cell=unsigned(x/3)+unsigned(y/3)*43;
                smoothed.setPixelColor(x,y,((cell*2654435761u)>>(cell%19))&1 ? Qt::white : Qt::black);
                current.setPixelColor(x,y,((cell*2246822519u+3266489917u)>>(cell%17))&1 ? Qt::white : Qt::black);
            }
            const auto prepare=[&](bool same,RecognitionResult& result) {
                const auto& raw=same?smoothed:current;
                if(cudaMemcpyAsync(smoothedPx.data,smoothed.constBits(),smoothed.sizeInBytes(),cudaMemcpyHostToDevice,stream.value)!=cudaSuccess ||
                   cudaMemcpyAsync(currentPx.data,raw.constBits(),raw.sizeInBytes(),cudaMemcpyHostToDevice,stream.value)!=cudaSuccess)return false;
                auto input=registration.CapturePair(smoothedPx.data,smoothed.bytesPerLine(),currentPx.data,raw.bytesPerLine(),
                    128,64,stream.value,7,46,3,9,40,7,.25f);
                if(!input)return false;
                result.stamp=r.stamp;
                RecognizedLine line;line.words.push_back({QStringLiteral("recognized"),QRectF(8,8,112,48)});
                result.output.lines.push_back(line);
                return input->PrepareResidentAnalysis(result,1,false,cancel,error);
            };
            RecognitionResult stale;QVERIFY2(prepare(false,stale),qPrintable(error));
            QCOMPARE(stale.currentInputMatches.size(),1);QVERIFY(!stale.currentInputMatches.front());
            QVERIFY(BuildReferencePatch(stale.reference,QRectF(8.0/128,8.0/64,112.0/128,48.0/64)).valid);
            RecognitionResult exact;QVERIFY2(prepare(true,exact),qPrintable(error));
            QCOMPARE(exact.currentInputMatches.size(),1);QVERIFY(exact.currentInputMatches.front());
        }
        {
            QVERIFY(QFontDatabase::addApplicationFont("C:/Windows/Fonts/arial.ttf")>=0);
            QFont font("Arial");font.setPixelSize(28);font.setBold(true);
            const QFontMetrics fm(font);
            const auto textPage=[&](const QString& last,int dy,RecognizedLine* line) {
                QImage image(640,256,QImage::Format_ARGB32);image.fill(Qt::white);
                QPainter painter(&image);painter.setFont(font);painter.setPen(Qt::black);
                int x=50;const int baseline=100+dy;
                for(const auto& word:QStringList{"Read","clear","words",last}) {
                    painter.drawText(x,baseline,word);
                    if(line)line->words.push_back({word,QRectF(fm.tightBoundingRect(word).translated(x,baseline))});
                    x+=fm.horizontalAdvance(word+QStringLiteral(" "));
                }
                return image;
            };
            RecognizedLine line;const auto stable=textPage("today",0,&line);
            GpuBuffer<unsigned char> a(stable.sizeInBytes()),b(stable.sizeInBytes());AnalysisPool words;
            for(int scenario=0;scenario<5;++scenario) {
                QImage raw=scenario==0?stable:textPage(scenario==1?"never":"today",
                    scenario<2?0:(scenario==2?12:scenario==3?24:48),nullptr);
                if(scenario==0)for(int y=0;y<raw.height();++y)for(int x=0;x<raw.width();++x) {
                    const int noise=(x*17+y*23)%13-6;
                    const int v=std::clamp(qRed(raw.pixel(x,y))+noise,0,255);raw.setPixel(x,y,qRgb(v,v,v));
                }
                QCOMPARE(cudaMemcpyAsync(a.data,stable.constBits(),stable.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
                QCOMPARE(cudaMemcpyAsync(b.data,raw.constBits(),raw.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
                auto input=words.CapturePair(a.data,stable.bytesPerLine(),b.data,raw.bytesPerLine(),
                    stable.width(),stable.height(),stream.value,7,47+scenario,3,9,40,8,.25f);QVERIFY(input);
                RecognitionResult proof;proof.stamp=r.stamp;proof.stamp.viewportWidth=stable.width();proof.stamp.viewportHeight=stable.height();
                proof.output.lines.push_back(line);
                QVERIFY2(input->PrepareResidentAnalysis(proof,2,false,cancel,error),qPrintable(error));
                QCOMPARE(proof.currentInputMatches.size(),1);
                QCOMPARE(proof.currentInputMatches.front(),scenario==0);
                const auto recognized=MakeProbeFrame(stable.constBits(),stable.width(),stable.height(),stable.bytesPerLine(),2,proof.stamp);
                const auto currentProbe=MakeProbeFrame(raw.constBits(),raw.width(),raw.height(),raw.bytesPerLine(),2,proof.stamp);
                bool cpu=true;
                for(const auto& word:line.words) {
                    const auto rect=ViewRectToScene(proof.stamp.viewTransform,stable.width(),stable.height(),word.boundsPx);
                    const auto match=MatchPatch(BuildReferencePatch(recognized,rect),currentProbe,kInputRegistrationTuning);
                    cpu &= match.matched && match.sceneOffset.isNull();
                }
                QCOMPARE(proof.currentInputMatches.front(),cpu);
            }
        }
        // The 128 MiB bound counts both halves: this extent fits singly
        // (69 MiB) but not as a pair, and is refused before any copy.
        AnalysisPool capped;
        QVERIFY(!capped.CapturePair(smoothedPx.data,4096*4,currentPx.data,4096*4,
                                    4096,4224,stream.value,7,45,3,9,40,6,0.25f));
        QVERIFY(!capped.HasFailure());
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    // The 32-byte record is copied with the pixels on the capture stream and
    // read only by a worker after that copy. Later device-state changes cannot
    // rename a capture; crossfades never reach the recognizer.
    void gpuHoldRecordTravelsWithCaptureAndResolvesOnWorker()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)QSKIP("No CUDA device.");
        FixtureStream stream;AnalysisPool pool;std::atomic<bool> cancel{false};QString error;
        QImage page(128,64,QImage::Format_ARGB32);
        for(int y=0;y<64;++y)for(int x=0;x<128;++x)page.setPixelColor(x,y,QColor((x*2)%256,(y*4)%256,96));
        GpuBuffer<unsigned char> pixels(page.sizeInBytes());GpuBuffer<HoldProvenanceRecord> record(1);
        const auto upload=[&](HoldProvenanceRecord value){
            return cudaMemcpyAsync(record.data,&value,sizeof(value),cudaMemcpyHostToDevice,stream.value)==cudaSuccess;};
        const auto capture=[&](std::uint64_t frame,bool withRecord){
            return pool.Capture(pixels.data,page.bytesPerLine(),page.width(),page.height(),stream.value,7,frame,3,
                                withRecord?record.data:nullptr);};
        QCOMPARE(cudaMemcpyAsync(pixels.data,page.constBits(),page.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        QVERIFY(upload({40,40,1,1,0.0f,0}));
        auto held=capture(42,true);QVERIFY(held);QVERIFY(held->HasHoldProvenance());
        QVERIFY(upload({43,43,0,1,1.0f,0})); // the next frame's state, stream-ordered after the copy
        const auto read=held->ReadHoldProvenance(cancel,error);
        QCOMPARE(int(read.display),int(HoldDisplay::Held));QCOMPARE(read.acquisition,std::uint64_t{40});
        QCOMPARE(held->Readback(cancel,error).convertToFormat(QImage::Format_ARGB32),page);
        SourceStamp stamp;stamp.sourceEpoch=7;stamp.frameSeq=42;stamp.geometryGeneration=1;
        stamp.viewportWidth=page.width();stamp.viewportHeight=page.height();
        stamp.viewTransform={0,0,1,1,0,0,1,1,true};stamp.liveCameraCapture=true;stamp.holdDisplay=HoldDisplay::Unknown;
        {
            RecognitionResult probe;std::atomic<bool> done{};
            RecognitionScheduler reader(nullptr,[&](RecognitionResult r){probe=std::move(r);done=true;},{},2,4,true);
            QTRY_COMPARE(reader.Status().state,BackendState::kReady);
            QVERIFY(reader.SubmitDevice(stamp,held));
            QTRY_VERIFY(done.load());
            QVERIFY(probe.output.ok);QVERIFY(probe.reference.resident);QVERIFY(probe.reference.luma.isNull());
            QCOMPARE(int(probe.stamp.holdDisplay),int(HoldDisplay::Held));
            QCOMPARE(int(probe.reference.stamp.holdDisplay),int(HoldDisplay::Held));
            QCOMPARE(EvidenceAcquisition(probe.stamp).value_or(0),std::uint64_t{40});
        }
        held.reset();
        BackendCalls calls;std::atomic<int> completions{};RecognitionResult ocr;
        RecognitionScheduler scheduler(std::make_unique<FakeBackend>(calls,0,true),
            [&](RecognitionResult r){ocr=std::move(r);++completions;},{});
        QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(upload({0,40,2,1,0.5f,0}));
        auto blend=capture(44,true);QVERIFY(blend);stamp.frameSeq=44;
        QVERIFY(scheduler.SubmitDevice(stamp,blend));QTRY_COMPARE(completions.load(),1);
        QVERIFY(ocr.output.compositeInput);QVERIFY(!ocr.output.ok);QVERIFY(!ocr.output.inputFault);
        QCOMPARE(calls.calls,0);QCOMPARE(int(ocr.stamp.holdDisplay),int(HoldDisplay::Crossfade));
        blend.reset();
        QVERIFY(upload({45,45,0,1,1.0f,0}));
        auto live=capture(45,true);QVERIFY(live);stamp.frameSeq=45;
        QVERIFY(scheduler.SubmitDevice(stamp,live));QTRY_COMPARE(completions.load(),2);
        QVERIFY(ocr.output.ok);QVERIFY(!ocr.output.compositeInput);QCOMPARE(calls.calls,1);
        QCOMPARE(int(ocr.stamp.holdDisplay),int(HoldDisplay::Live));QCOMPARE(ocr.stamp.holdAcquisition,std::uint64_t{45});
        live.reset();
        // Without a record, a stamp cannot keep claiming held pixels.
        auto plain=capture(46,false);QVERIFY(plain);QVERIFY(!plain->HasHoldProvenance());
        QCOMPARE(int(plain->ReadHoldProvenance(cancel,error).display),int(HoldDisplay::Unknown));
        stamp.frameSeq=46;stamp.holdDisplay=HoldDisplay::Held;stamp.holdAcquisition=40;
        QVERIFY(scheduler.SubmitDevice(stamp,plain));QTRY_COMPARE(completions.load(),3);
        QVERIFY(ocr.output.compositeInput);QCOMPARE(calls.calls,1);
        QCOMPARE(int(ocr.stamp.holdDisplay),int(HoldDisplay::Unknown));
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    // Explicit pool fault seam: clean allocation pressure backs off and
    // becomes visible after three attempts; unknown copy completion
    // quarantines the pool until an explicit reset replaces it.
    void gpuAnalysisFaultSeamLatchesPressureAndQuarantinesUnknownCompletion()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)QSKIP("No CUDA device.");
        FixtureStream stream;GpuBuffer<unsigned char> pixels(64*32*4);
        QCOMPARE(cudaMemsetAsync(pixels.data,0x40,64*32*4,stream.value),cudaSuccess);
        const auto capture=[&](AnalysisPool& pool,std::uint64_t frame){
            return pool.Capture(pixels.data,64*4,64,32,stream.value,1,frame,1);};
        {
            AnalysisPool pool;
            pool.InjectFaultForTesting(AnalysisPool::InjectedFault::CleanAllocation);
            QVERIFY(!capture(pool,1));QVERIFY(!pool.HasFailure());
            QVERIFY(!capture(pool,2));QVERIFY(!pool.HasFailure()); // bounded pause, no new attempt
            QTest::qSleep(270);
            QVERIFY(capture(pool,3));QVERIFY(!pool.HasFailure());
        }
        {
            AnalysisPool pool;
            for(int attempt=0;attempt<3;++attempt){
                pool.InjectFaultForTesting(AnalysisPool::InjectedFault::CleanAllocation);
                QVERIFY(!capture(pool,10+attempt));
                QCOMPARE(pool.HasFailure(),attempt==2);
                QTest::qSleep(270);
            }
            QVERIFY(capture(pool,20));QVERIFY(!pool.HasFailure()); // success clears clean pressure
        }
        {
            AnalysisPool pool;
            pool.InjectFaultForTesting(AnalysisPool::InjectedFault::UnknownCompletion);
            QVERIFY(!capture(pool,30));QVERIFY(pool.HasFailure());
            QTest::qSleep(300);
            QVERIFY(!capture(pool,31));QVERIFY(pool.HasFailure()); // no time-based recovery
            AnalysisPool reset; // explicit UCT reset replaces the pool
            QVERIFY(capture(reset,32));QVERIFY(!reset.HasFailure());
        }
        {
            // A hold-bearing capture is atomic with its 32-byte record: a
            // fault publishes neither pixels nor provenance, and the retry
            // after clean pressure still carries the exact record.
            GpuBuffer<HoldProvenanceRecord> record(1);
            const HoldProvenanceRecord held{40,40,1,1,0.0f,0};
            QCOMPARE(cudaMemcpyAsync(record.data,&held,sizeof(held),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
            AnalysisPool pool;
            pool.InjectFaultForTesting(AnalysisPool::InjectedFault::UnknownCompletion);
            QVERIFY(!pool.Capture(pixels.data,64*4,64,32,stream.value,1,42,1,record.data));
            QVERIFY(pool.HasFailure());
            AnalysisPool clean;
            clean.InjectFaultForTesting(AnalysisPool::InjectedFault::CleanAllocation);
            QVERIFY(!clean.Capture(pixels.data,64*4,64,32,stream.value,1,42,1,record.data));
            QVERIFY(!clean.HasFailure());
            QTest::qSleep(270);
            auto image=clean.Capture(pixels.data,64*4,64,32,stream.value,1,42,1,record.data);
            QVERIFY(image);QVERIFY(image->HasHoldProvenance());
            std::atomic<bool> cancel{false};QString error;
            const auto read=image->ReadHoldProvenance(cancel,error);
            QCOMPARE(int(read.display),int(HoldDisplay::Held));QCOMPARE(read.acquisition,std::uint64_t{40});
        }
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    // Explicit one-time experiment; not part of ordinary CTest acceptance.
    void residentDensePageLatencyExperiment()
    {
#if OKUFLOW_ENABLE_CUDA
        if(!qEnvironmentVariableIsSet("OKUFLOW_UCT_RESIDENT_PERF"))QSKIP("Opt-in dense-page latency experiment.");
        int devices{};if(cudaGetDeviceCount(&devices)!=cudaSuccess||!devices)QSKIP("No CUDA device.");
        QStringList lines;for(int i=0;i<60;++i)lines.push_back("Printed line stable");
        const auto page=uct_test::MakePage(lines,{},1280,2160,20);
        RecognitionOutput output;output.ok=true;
        for(int i=0;i<page.words.size();++i){
            if(i%3==0)output.lines.push_back({});
            const auto& w=page.words[i];const auto r=w.sceneRect;
            output.lines.back().words.push_back({w.text,{r.x()*1280,r.y()*2160,r.width()*1280,r.height()*2160}});
        }
        struct Backend final:Recognizer {
            RecognitionOutput output;bool gpu{};
            BackendStatus Initialize() override{return {BackendState::kReady,"fixture","",""};}
            bool WantsDeviceInput() const override{return gpu;}
            RecognitionOutput Recognize(const QImage&,const std::atomic<bool>&) override{return output;}
            RecognitionOutput RecognizeDevice(const DeviceImage&,const std::atomic<bool>&) override{return output;}
        };
        FixtureStream stream;GpuBuffer<unsigned char> pixels(page.image.sizeInBytes());AnalysisPool pool;
        QCOMPARE(cudaMemcpyAsync(pixels.data,page.image.constBits(),page.image.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        auto owned=pool.Capture(pixels.data,page.image.bytesPerLine(),1280,2160,stream.value,1,1,1);QVERIFY(owned);
        SourceStamp stamp;stamp.sourceEpoch=1;stamp.frameSeq=1;stamp.viewportWidth=1280;stamp.viewportHeight=2160;
        stamp.viewTransform={0,0,1,1,0,0,1,1,true};stamp.liveCameraCapture=true;
        std::mutex mutex;std::optional<RecognitionResult> cpu,gpu;
        auto cb=std::make_unique<Backend>();cb->output=output;
        auto gb=std::make_unique<Backend>();gb->output=output;gb->gpu=true;
        RecognitionScheduler cs(std::move(cb),[&](auto r){std::lock_guard lock(mutex);cpu=std::move(r);},{});
        RecognitionScheduler gs(std::move(gb),[&](auto r){std::lock_guard lock(mutex);gpu=std::move(r);},{});
        for(int trial=0;trial<4;++trial){
            {std::lock_guard lock(mutex);cpu.reset();gpu.reset();}
            QVERIFY(cs.Submit(stamp,page.image));QVERIFY(gs.SubmitDevice(stamp,owned));
            const auto done=[&]{std::lock_guard lock(mutex);return cpu&&gpu;};QTRY_VERIFY_WITH_TIMEOUT(done(),15000);
            std::lock_guard lock(mutex);QVERIFY(cpu->output.ok);QVERIFY2(gpu->output.ok,qPrintable(gpu->output.error));
            QCOMPARE(cpu->prepared.size(),60);QCOMPARE(gpu->prepared.size(),60);
            QVector<LineReference> cr,gr;
            for(int i=0;i<60;++i){cr.push_back({static_cast<std::uint64_t>(i+1),cpu->prepared[i].reference,{}});gr.push_back({static_cast<std::uint64_t>(i+1),gpu->prepared[i].reference,{}});}
            QElapsedTimer timer;timer.start();const auto cc=CheckLines(cr,cpu->reference,{},0);const double cpuCheck=timer.nsecsElapsed()/1e6;
            timer.restart();const auto gc=CheckLines(gr,gpu->reference,{},0);const double gpuCheck=timer.nsecsElapsed()/1e6;
            QCOMPARE(cc.size(),60);QCOMPARE(gc.size(),60);
            for(int i=0;i<60;++i){QVERIFY(cc[i].match.matched);QVERIFY(gc[i].match.matched);}
            qInfo().nospace()<<"dense60 trial="<<trial<<" (0 warmup), CPU prepareMs="<<cpu->durationMs<<", GPU prepareMs="<<gpu->durationMs<<", CPU checkMs="<<cpuCheck<<", GPU checkMs="<<gpuCheck;
        }
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    void gpuAlgorithmsAndOwnedInputs()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};
        if (cudaGetDeviceCount(&devices) != cudaSuccess || devices < 1)
            QSKIP("No CUDA device.");
        FixtureStream stream;
        QImage image(128, 64, QImage::Format_ARGB32);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 128; ++x)
                image.setPixelColor(x, y, QColor(x * 2, y * 4, (x + y) % 256));
        GpuBuffer<unsigned char> pixels(image.sizeInBytes());
        QCOMPARE(cudaMemcpyAsync(pixels.data, image.constBits(), image.sizeInBytes(),
                                 cudaMemcpyHostToDevice, stream.value),
                 cudaSuccess);
        GpuBuffer<float> normalized(64 * 32 * 3);
        std::vector<float> values(64 * 32 * 3);
        gpu::Normalize(pixels.data, image.bytesPerLine(), 128, 64, normalized.data, 64, 32,
                       stream.value);
        QCOMPARE(cudaMemcpyAsync(values.data(), normalized.data, values.size() * sizeof(float),
                                 cudaMemcpyDeviceToHost, stream.value),
                 cudaSuccess);
        QCOMPARE(cudaStreamSynchronize(stream.value), cudaSuccess);
        const auto reference = PaddleNormalize(image, {64, 32}, true);
        for (std::size_t i = 0; i < values.size(); ++i)
            QVERIFY(std::abs(values[i] - reference[i]) < .00001f);

        std::vector<float> map(128 * 64, .01f);
        for (int y = 20; y < 32; ++y)
            for (int x = 16; x < 100; ++x)
                map[y * 128 + x] = .95f;
        GpuBuffer<float> deviceMap(map.size());
        GpuBuffer<int> labels(map.size()), count(1);
        GpuBuffer<gpu::Component> components(map.size());
        GpuBuffer<gpu::Quad> boxes(128);
        QCOMPARE(cudaMemcpyAsync(deviceMap.data, map.data(), map.size() * sizeof(float),
                                 cudaMemcpyHostToDevice, stream.value),
                 cudaSuccess);
        gpu::Detect(deviceMap.data, 128, 64, 128, 64, labels.data, components.data, boxes.data,
                    count.data, stream.value);
        int found{};
        gpu::Quad box{};
        QCOMPARE(
            cudaMemcpyAsync(&found, count.data, sizeof(int), cudaMemcpyDeviceToHost, stream.value),
            cudaSuccess);
        QCOMPARE(
            cudaMemcpyAsync(&box, boxes.data, sizeof(box), cudaMemcpyDeviceToHost, stream.value),
            cudaSuccess);
        QCOMPARE(cudaStreamSynchronize(stream.value), cudaSuccess);
        QCOMPARE(found, 1);
        {
            for(const double aspect:{19.5,20.25,104.0,132.0,160.0,200.0})
            for(const double angle:{0.0,0.3}) {
                const double w=aspect*12,h=12;
                const auto at=[&](double x,double y) {
                    return QPointF(100+x*std::cos(angle)-y*std::sin(angle),
                                   100+x*std::sin(angle)+y*std::cos(angle));
                };
                PaddleQuad source{{at(0,0),at(w,0),at(w,h),at(0,h)},.99f};
                gpu::Quad raw{};raw.score=.99f;raw.validWidth=int(std::ceil(48*aspect));
                for(int k=0;k<4;++k) {
                    raw.xy[k*2]=float(source.points[k].x());raw.xy[k*2+1]=float(source.points[k].y());
                    source.points[k]={raw.xy[k*2],raw.xy[k*2+1]};
                }
                const int one=1;
                QCOMPARE(cudaMemcpyAsync(boxes.data,&raw,sizeof(raw),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
                QCOMPARE(cudaMemcpyAsync(count.data,&one,sizeof(one),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
                GpuBuffer<gpu::Quad> windows(128*9);
                gpu::RecognitionWindows(boxes.data,count.data,windows.data,stream.value);
                QCOMPARE(cudaMemcpyAsync(&found,count.data,sizeof(int),cudaMemcpyDeviceToHost,stream.value),cudaSuccess);
                QCOMPARE(cudaStreamSynchronize(stream.value),cudaSuccess);
                const auto expected=PaddleRecognitionWindows(source);
                QCOMPARE(found,expected.size());
                QVector<gpu::Quad> actual(found);
                QCOMPARE(cudaMemcpyAsync(actual.data(),windows.data,found*sizeof(gpu::Quad),cudaMemcpyDeviceToHost,stream.value),cudaSuccess);
                QCOMPARE(cudaStreamSynchronize(stream.value),cudaSuccess);
                for(int i=0;i<found;++i) {
                    QCOMPARE(actual[i].internalEdges,expected[i].internalEdges);
                    for(int k=0;k<4;++k) {
                        QVERIFY(std::abs(actual[i].xy[k*2]-expected[i].points[k].x())<.005);
                        QVERIFY(std::abs(actual[i].xy[k*2+1]-expected[i].points[k].y())<.005);
                    }
                }
            }

        }
        std::atomic<bool> cancel{false};
        const auto cpuBoxes = PaddleDetect(map, 128, 64, {128, 64}, cancel);
        for (int k = 0; k < 4; ++k)
        {
            QVERIFY(std::abs(box.xy[k * 2] - cpuBoxes[0].points[k].x()) < .1);
            QVERIFY(std::abs(box.xy[k * 2 + 1] - cpuBoxes[0].points[k].y()) < .1);
        }
        box = {{8, 8, 104, 8, 104, 32, 8, 32}, 1, 192};
        QCOMPARE(
            cudaMemcpyAsync(boxes.data, &box, sizeof(box), cudaMemcpyHostToDevice, stream.value),
            cudaSuccess);
        GpuBuffer<float> crop(1024 * 48 * 3);
        values.resize(1024 * 48 * 3);
        gpu::Rectify(pixels.data, image.bytesPerLine(), 128, 64, boxes.data, 0, crop.data, 1024,
                     stream.value);
        QCOMPARE(cudaMemcpyAsync(values.data(), crop.data, values.size() * sizeof(float),
                                 cudaMemcpyDeviceToHost, stream.value),
                 cudaSuccess);
        QCOMPARE(cudaStreamSynchronize(stream.value), cudaSuccess);
        PaddleQuad cpuBox{{QPointF(8, 8), QPointF(104, 8), QPointF(104, 32), QPointF(8, 32)}, 1};
        const auto cpuCrop = PaddleRectify(image, cpuBox, 1024, 192);
        for (std::size_t i = 0; i < values.size(); ++i)
            QVERIFY(std::abs(values[i] - cpuCrop[i]) < .0001f);

        std::vector<float> probabilities(8 * 4, .001f);
        const int expected[]{1, 1, 0, 1, 3, 2, 2, 0};
        for (int t = 0; t < 8; ++t)
            probabilities[t * 4 + expected[t]] = .99f;
        box.validWidth = 1024;
        QCOMPARE(
            cudaMemcpyAsync(boxes.data, &box, sizeof(box), cudaMemcpyHostToDevice, stream.value),
            cudaSuccess);
        GpuBuffer<float> logits(32), scores(8);
        GpuBuffer<int> ids(8);
        GpuBuffer<gpu::Token> tokens(8);
        gpu::Token decoded[8]{};
        for (int invalid = 0; invalid < 2; ++invalid)
        {
            if (invalid)
                probabilities[1] = std::numeric_limits<float>::quiet_NaN();
            QCOMPARE(cudaMemcpyAsync(logits.data, probabilities.data(), 32 * sizeof(float),
                                     cudaMemcpyHostToDevice, stream.value),
                     cudaSuccess);
            gpu::Decode(logits.data, 8, 4, boxes.data, 0, 1024, ids.data, scores.data, tokens.data,
                        count.data, stream.value);
            QCOMPARE(cudaMemcpyAsync(&found, count.data, sizeof(int), cudaMemcpyDeviceToHost,
                                     stream.value),
                     cudaSuccess);
            QCOMPARE(cudaMemcpyAsync(decoded, tokens.data, sizeof(decoded), cudaMemcpyDeviceToHost,
                                     stream.value),
                     cudaSuccess);
            QCOMPARE(cudaStreamSynchronize(stream.value), cudaSuccess);
            QCOMPARE(found, invalid ? -1 : 4);
            if (!invalid)
            {
                QCOMPARE(decoded[0].id, 1);
                QCOMPARE(decoded[0].first, 0);
                QCOMPARE(decoded[0].last, 1);
                QCOMPARE(decoded[3].id, 2);
                QCOMPARE(decoded[3].last, 6);
            }
        }
        AnalysisPool pool;
        auto first =
            pool.Capture(pixels.data, image.bytesPerLine(), 128, 64, stream.value, 7, 8, 9);
        QVERIFY(first);
        QCOMPARE(cudaMemsetAsync(pixels.data, 0, image.sizeInBytes(), stream.value), cudaSuccess);
        auto second =
            pool.Capture(pixels.data, image.bytesPerLine(), 128, 64, stream.value, 7, 9, 9);
        QVERIFY(second);
        QVERIFY(!pool.Capture(pixels.data, image.bytesPerLine(), 128, 64, stream.value, 7, 10, 9));
        QString error;
        QCOMPARE(first->Readback(cancel, error), image);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(first->sourceEpoch, std::uint64_t{7});
        QCOMPARE(first->processingGeneration, std::uint64_t{9});
        const auto black = second->Readback(cancel, error);
        QVERIFY(!black.isNull());
        QCOMPARE(black.pixel(1, 1), QRgb{0});
        const auto probe = first->ReadProbe(cancel, error);
        QCOMPARE(probe, image);
        // Model a held OCR lease with the first slot. Repeated validation must
        // keep using the second slot without an OCR-pending lease starving it.
        second.reset();
        for (std::uint64_t frame = 10; frame < 40; ++frame)
        {
            auto fresh =
                pool.Capture(pixels.data, image.bytesPerLine(), 128, 64, stream.value, 7, frame, 9);
            QVERIFY(fresh);
            QVERIFY(!fresh->ReadProbe(cancel, error).isNull());
            QCOMPARE(fresh->frameSequence, frame);
        }
        QImage large(1024, 512, QImage::Format_ARGB32);
        large.fill(QColor(20, 40, 60));
        GpuBuffer<unsigned char> largePixels(large.sizeInBytes());
        AnalysisPool largePool(AnalysisAllocation::Legacy);
        QCOMPARE(cudaMemcpyAsync(largePixels.data, large.constBits(), large.sizeInBytes(),
                                 cudaMemcpyHostToDevice, stream.value),
                 cudaSuccess);
        auto largeFrame = largePool.Capture(largePixels.data, large.bytesPerLine(), 1024, 512,
                                            stream.value, 7, 40, 9);
        QVERIFY(largeFrame);
        const auto reduced = largeFrame->ReadProbe(cancel, error);
        QCOMPARE(reduced.size(), QSize(512, 256));
        QCOMPARE(reduced.pixelColor(100, 100), QColor(20, 40, 60));
#else
        QSKIP("CUDA-only algorithm acceptance.");
#endif
    }
    void gpuDenseRegionSelectionIsDeterministic()
    {
#if OKUFLOW_ENABLE_CUDA
        constexpr int w = 256, h = 128;
        std::vector<float> map(w * h);
        for (int y = 4; y < 124; y += 8)
            for (int x = 4; x < 252; x += 8)
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 4; ++dx)
                        map[(y + dy) * w + x + dx] = .9f;
        std::atomic<bool> cancel{false};
        const auto cpu = PaddleDetect(map, w, h, {w, h}, cancel);
        QCOMPARE(cpu.size(), std::size_t{128});
        FixtureStream stream;
        GpuBuffer<float> pixels(w * h);
        GpuBuffer<int> labels(w * h), count(1);
        GpuBuffer<gpu::Component> components(w * h);
        GpuBuffer<gpu::Quad> boxes(128);
        QCOMPARE(cudaMemcpyAsync(pixels.data, map.data(), map.size() * sizeof(float),
                                 cudaMemcpyHostToDevice, stream.value),
                 cudaSuccess);
        for (int iteration = 0; iteration < 3; ++iteration)
        {
            gpu::Detect(pixels.data, w, h, w, h, labels.data, components.data, boxes.data,
                        count.data, stream.value);
            std::array<gpu::Quad, 128> result{};
            int total{};
            QCOMPARE(cudaMemcpyAsync(result.data(), boxes.data, sizeof(result),
                                     cudaMemcpyDeviceToHost, stream.value),
                     cudaSuccess);
            QCOMPARE(cudaMemcpyAsync(&total, count.data, sizeof(total), cudaMemcpyDeviceToHost,
                                     stream.value),
                     cudaSuccess);
            QCOMPARE(cudaStreamSynchronize(stream.value), cudaSuccess);
            QCOMPARE(total, 465);
            for (std::size_t i = 0; i < result.size(); ++i)
                for (int k = 0; k < 4; ++k)
                {
                    QVERIFY(std::abs(result[i].xy[k * 2] - cpu[i].points[k].x()) < .1);
                    QVERIFY(std::abs(result[i].xy[k * 2 + 1] - cpu[i].points[k].y()) < .1);
                }
        }
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    void gpuOrientedRegionsMatchCpuCornerOrder()
    {
#if OKUFLOW_ENABLE_CUDA
        constexpr int w = 256, h = 256;
        FixtureStream stream;
        GpuBuffer<float> pixels(w * h);
        GpuBuffer<int> labels(w * h), count(1);
        GpuBuffer<gpu::Component> components(w * h);
        GpuBuffer<gpu::Quad> boxes(128);
        std::atomic<bool> cancel{false};
        for (double angle : {0.0, 1.0, 1.5707963267948966, -1.0})
        {
            std::vector<float> map(w * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const double u = (x - w / 2) * std::cos(angle) + (y - h / 2) * std::sin(angle);
                    const double v = -(x - w / 2) * std::sin(angle) + (y - h / 2) * std::cos(angle);
                    if (std::abs(u) < 100 && std::abs(v) < 20)
                        map[y * w + x] = .9f;
                }
            const auto cpu = PaddleDetect(map, w, h, {w, h}, cancel);
            QCOMPARE(cpu.size(), std::size_t{1});
            QCOMPARE(cudaMemcpyAsync(pixels.data, map.data(), map.size() * sizeof(float),
                                     cudaMemcpyHostToDevice, stream.value),
                     cudaSuccess);
            gpu::Detect(pixels.data, w, h, w, h, labels.data, components.data, boxes.data,
                        count.data, stream.value);
            gpu::Quad result{};
            int total{};
            QCOMPARE(cudaMemcpyAsync(&result, boxes.data, sizeof(result), cudaMemcpyDeviceToHost,
                                     stream.value),
                     cudaSuccess);
            QCOMPARE(cudaMemcpyAsync(&total, count.data, sizeof(total), cudaMemcpyDeviceToHost,
                                     stream.value),
                     cudaSuccess);
            QCOMPARE(cudaStreamSynchronize(stream.value), cudaSuccess);
            QCOMPARE(total, 1);
            for (int k = 0; k < 4; ++k)
            {
                QVERIFY2(std::abs(result.xy[k * 2] - cpu[0].points[k].x()) < .1,
                         qPrintable(QString("x corner %1 angle %2").arg(k).arg(angle)));
                QVERIFY2(std::abs(result.xy[k * 2 + 1] - cpu[0].points[k].y()) < .1,
                         qPrintable(QString("y corner %1 angle %2").arg(k).arg(angle)));
            }
        }
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    void optionalRealCpu_data()
    {
        QTest::addColumn<int>("detector");QTest::addColumn<int>("recognizer");
        QTest::newRow("default-tiny-small")<<int(ModelTier::Tiny)<<int(ModelTier::Small);
        QTest::newRow("tiny-tiny")<<int(ModelTier::Tiny)<<int(ModelTier::Tiny);
        QTest::newRow("small-small")<<int(ModelTier::Small)<<int(ModelTier::Small);
        QTest::newRow("medium-medium")<<int(ModelTier::Medium)<<int(ModelTier::Medium);
    }
    void optionalRealCpu()
    {
        if (!qEnvironmentVariableIsSet("OKUFLOW_TEST_PADDLE_RUNTIME"))
            QSKIP("Explicit model-runtime acceptance not selected.");
        QFETCH(int,detector);QFETCH(int,recognizer);
        auto backend = CreatePaddleCpuRecognizer({},static_cast<ModelTier>(detector),static_cast<ModelTier>(recognizer));
        const auto status = backend->Initialize();
        QVERIFY2(status.state == BackendState::kReady, qPrintable(status.detail));
        QTRY_VERIFY_WITH_TIMEOUT(!backend->Preparing(), 180000);
        QVERIFY2(backend->Status().state == BackendState::kReady,
                 qPrintable(backend->Status().detail));
        QVERIFY(QFontDatabase::addApplicationFont("C:/Windows/Fonts/arial.ttf") >= 0);
        QImage page(1280, 720, QImage::Format_ARGB32);
        page.fill(Qt::white);
        QPainter painter(&page);
        QFont font("Arial", 42);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(100, 220, "MAGNIFIED PRINTED TEXT");
        painter.drawText(100, 330, "READS CLEARLY TODAY");
        painter.end();
        std::atomic<bool> cancelled{false};
        QElapsedTimer timer;
        timer.start();
        const auto output = backend->Recognize(page, cancelled);
        QVERIFY2(output.ok, qPrintable(output.error));
        QStringList words;
        for (const auto &line : output.lines)
            for (const auto &word : line.words)
                if (word.disposition == TokenDisposition::kCandidate) words.push_back(word.text);
        qInfo() << "Paddle CPU ms" << timer.elapsed() << words;
        for (const auto &expected :
             QString("MAGNIFIED PRINTED TEXT READS CLEARLY TODAY").split(' '))
            QVERIFY2(words.contains(expected), qPrintable(expected));
    }
    void optionalRealGpu()
    {
        if (!qEnvironmentVariableIsSet("OKUFLOW_TEST_PADDLE_GPU"))
            QSKIP("Explicit TensorRT model acceptance not selected.");
        auto recognizer = CreateTensorRtRecognizer();
        QVERIFY2(recognizer != nullptr, "TensorRT adapter was not compiled.");
        const auto status = recognizer->Initialize();
        QVERIFY2(status.state == BackendState::kReady, qPrintable(status.detail));
        QTRY_VERIFY_WITH_TIMEOUT(!recognizer->Preparing(), 180000);
        QVERIFY2(recognizer->Status().state == BackendState::kReady,
                 qPrintable(recognizer->Status().detail));
        QVERIFY(QFontDatabase::addApplicationFont("C:/Windows/Fonts/arial.ttf") >= 0);
        QImage page(1280, 720, QImage::Format_ARGB32);
        page.fill(Qt::white);
        QPainter painter(&page);
        QFont font("Arial", 42);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(100, 220, "MAGNIFIED PRINTED TEXT");
        painter.drawText(100, 330, "READS CLEARLY TODAY");
        painter.end();
        std::atomic<bool> cancelled{false};
#if OKUFLOW_ENABLE_CUDA
        FixtureStream stream;
        GpuBuffer<unsigned char> devicePixels(page.sizeInBytes());
        AnalysisPool pool;
        QCOMPARE(cudaMemcpyAsync(devicePixels.data, page.constBits(), page.sizeInBytes(),
                                 cudaMemcpyHostToDevice, stream.value),
                 cudaSuccess);
        auto owned = pool.Capture(devicePixels.data, page.bytesPerLine(), page.width(),
                                  page.height(), stream.value, 1, 1, 1);
        QVERIFY(owned);
        // An unowned input is refused before enqueue and cannot retire engines.
        CopyFaultImage unowned;
        QVERIFY(recognizer->RecognizeDevice(unowned, cancelled).inputFault);
        QVERIFY(recognizer->Recognize({}, cancelled).inputFault);
        QVERIFY(!recognizer->Preparing());
        QCOMPARE(recognizer->Status().state, BackendState::kReady);
#endif
        for (int iteration = 0; iteration < 3; ++iteration)
        {
#if OKUFLOW_ENABLE_CUDA
            if(iteration==2) {
                owned.reset();
                owned=pool.CapturePair(devicePixels.data,page.bytesPerLine(),devicePixels.data,page.bytesPerLine(),
                    page.width(),page.height(),stream.value,1,1,1,1,1,1,.25f);
                QVERIFY(owned);
            }
#endif
            QElapsedTimer timer;
            timer.start();
#if OKUFLOW_ENABLE_CUDA
            const auto output = iteration == 0 ? recognizer->Recognize(page, cancelled)
                                               : recognizer->RecognizeDevice(*owned, cancelled);
#else
            const auto output = recognizer->Recognize(page, cancelled);
#endif
            QVERIFY2(output.ok, qPrintable(output.error));
            QStringList words;
            for (const auto &line : output.lines)
                for (const auto &word : line.words)
                    if (word.disposition == TokenDisposition::kCandidate) words.push_back(word.text);
            qInfo() << "Paddle TensorRT iteration" << iteration << "ms" << timer.elapsed() << words;
#if OKUFLOW_ENABLE_CUDA
            if(iteration>=1) {
                RecognitionResult r;r.output=output;r.stamp.sourceEpoch=1;r.stamp.frameSeq=1;
                r.stamp.geometryGeneration=1;r.stamp.liveCameraCapture=true;
                r.stamp.temporallySmoothed=iteration==2;
                r.stamp.viewportWidth=page.width();r.stamp.viewportHeight=page.height();
                r.stamp.viewTransform={0,0,1,1,0,0,1,1,true};QString error;
                QVERIFY2(owned->PrepareResidentAnalysis(r,2,false,cancelled,error),qPrintable(error));
                if(iteration==2) {
                    QCOMPARE(r.currentInputMatches.size(),r.output.lines.size());
                    for(bool matched:r.currentInputMatches)QVERIFY(matched);
                }
                r.prepared=PrepareLines(r,7);QVERIFY(!r.prepared.isEmpty());
                QVERIFY(r.reference.resident);QVERIFY(r.reference.luma.isNull());QVERIFY(r.reference.integral.isEmpty());
                QVERIFY(r.sourcePixelDigest.isEmpty());
                QVector<LineReference> refs;int id=1;
                for(const auto& line:r.prepared){QVERIFY(line.reference.valid);QVERIFY(line.reference.resident);QVERIFY(line.reference.values.isEmpty());refs.push_back({std::uint64_t(id++),line.reference,{}});}
                const auto checks=CheckLines(refs,r.reference,{},0);
                for(const auto& check:checks){QVERIFY(check.checked);QVERIFY(check.match.matched);QVERIFY(check.match.score>.99);}
                qInfo()<<"TensorRT final colors, ink geometry and residual validation stayed device-resident";
            }
#endif
            for (const auto &expected :
                 QString("MAGNIFIED PRINTED TEXT READS CLEARLY TODAY").split(' '))
                QVERIFY2(words.contains(expected), qPrintable(expected));
        }
    }
};
QTEST_MAIN(PaddleTests)
#include "uct_paddle_tests.moc"
