#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/common/uct_model_pins.hpp"
#include "../src/common/uct_recognizer_width.hpp"
#include "../src/common/uct_runtime_precision.hpp"
#include "okuflow/cuda/uct_analysis_pool.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "ultra_clear_text_test_support.hpp"
#include "uct_color_test_support.hpp"
#include "uct_quad_reference_test_support.hpp"
#include "uct_startup_test_cases.hpp"
#include "uct_batch_diagnostic.hpp"
#include "uct_character_diagnostic.hpp"
#include "uct_resident_diagnostic.hpp"
#include "uct_paused_video_diagnostic.hpp"
#include "uct_active_regional_diagnostic.hpp"
#include <QFontDatabase>
#include <QLineF>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QTemporaryDir>
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
#include "uct_geometry_test_cases.hpp"
#include "uct_surface_test_cases.hpp"

class PaddleTests final : public QObject
{
    Q_OBJECT
  private slots:
    void localBf16ManifestRequiresReviewedRolesAndCacheOnlySchema()
    {
        using namespace runtime_precision;
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto detectorPath = root.filePath("det/medium/inference.onnx");
        const auto graphPath = root.filePath(QString("models/bf16-linear/%1/%2.onnx").arg(kRecognizerSource,kRecognizerGraph));
        const QJsonObject detector{{"precision","fp32"},{"sourceSha256",kDetectorSource},
                                   {"sha256",kDetectorSource},{"path",detectorPath}};
        const QJsonObject recognizer{{"precision","bf16-linear"},{"sourceSha256",kRecognizerSource},
                                     {"sha256",kRecognizerGraph},{"path",graphPath}};
        const QJsonObject valid{{"schema",1},{"prepare",false},{"artifactRoot",root.path()},
                                {"detector",detector},{"recognizer",recognizer}};
        const auto encode = [](const QJsonObject& json) { return QJsonDocument(json).toJson(); };
        const auto parsed = ParseManifest(encode(valid),detectorPath);
        QCOMPARE(parsed.recognizer.path,graphPath);
        QCOMPARE(parsed.detector.precision,QString("fp32"));
        // No receiptRoot is required or consumed by production.
        const auto refuses = [&](const QJsonObject& json) {
            try { ParseManifest(encode(json),detectorPath); return false; }
            catch (const ArtifactError&) { return true; }
        };
        for (const QJsonValue schema : {QJsonValue(),QJsonValue("1"),QJsonValue(1.1),QJsonValue(2)}) {
            auto json=valid; json["schema"]=schema; QVERIFY(refuses(json));
        }
        for (const QJsonValue prepare : {QJsonValue(),QJsonValue("false"),QJsonValue(true)}) {
            auto json=valid; json["prepare"]=prepare; QVERIFY(refuses(json));
        }
        auto json=valid; json["artifactRoot"]="relative"; QVERIFY(refuses(json));
        json=valid; json["artifactRoot"]="//server/share"; QVERIFY(refuses(json));
        json=valid; json["detector"]=recognizer; json["recognizer"]=detector; QVERIFY(refuses(json));
        json=valid; json.remove("detector"); QVERIFY(refuses(json));
        for (const QString role : {QString("detector"),QString("recognizer")}) {
            for (const QString field : {QString("sourceSha256"),QString("sha256")}) {
                for (const QString hash : {QString(64,'A'),QString(63,'a'),QString(64,'0'),QString("../wrong")}) {
                    auto graph=valid[role].toObject(); graph[field]=hash;
                    json=valid; json[role]=graph; QVERIFY(refuses(json));
                }
            }
            auto graph=valid[role].toObject(); graph["precision"]="fp16-linear";
            json=valid; json[role]=graph; QVERIFY(refuses(json));
            graph=valid[role].toObject(); graph["path"]=root.filePath("../escaped.onnx");
            json=valid; json[role]=graph; QVERIFY(refuses(json));
        }
        QVERIFY_EXCEPTION_THROWN(ParseManifest("[]",detectorPath),ArtifactError);
        QVERIFY_EXCEPTION_THROWN(ParseManifest("{",detectorPath),ArtifactError);
        QVERIFY_EXCEPTION_THROWN(ParseManifest(QByteArray(kManifestLimit+1,' '),detectorPath),ArtifactError);
        const auto manifest=root.filePath("cached.json");
        { QFile file(manifest); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(encode(valid)); }
        QCOMPARE(LoadManifest({manifest},detectorPath).recognizer.sha256,QByteArray(kRecognizerGraph));
        QVERIFY_EXCEPTION_THROWN(LoadManifest({"relative.json"},detectorPath),ArtifactError);
        QVERIFY_EXCEPTION_THROWN(LoadManifest({root.filePath("missing.json")},detectorPath),ArtifactError);
        { QFile file(manifest); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QByteArray(kManifestLimit+1,' ')); }
        QVERIFY_EXCEPTION_THROWN(LoadManifest({manifest},detectorPath),ArtifactError);
    }
    void localBf16KeyAndFallbackKeepStrictBaselineIdentity()
    {
        using namespace runtime_precision;
        Device device{QByteArray::fromHex(kDeviceUuid),13040,8};
        Graph graph{"bf16-linear",{},kRecognizerSource,kRecognizerGraph};
        const auto key=PrecisionPlanKey(graph,device,false,8);
        // Independent reviewed artifact filename, not a second implementation.
        QCOMPARE(key,QByteArray("e596b8ac0110f01e88fc94bcbdeda4511b83af90efb761aa4e194de3ea496073"));
        auto other=graph; other.precision="fp32"; QVERIFY(PrecisionPlanKey(other,device,false,8)!=key);
        other=graph; other.sourceSha256[0]='0'; QVERIFY(PrecisionPlanKey(other,device,false,8)!=key);
        other=graph; other.sha256[0]='0'; QVERIFY(PrecisionPlanKey(other,device,false,8)!=key);
        auto gpu=device; gpu.uuid[0]^=1; QVERIFY(PrecisionPlanKey(graph,gpu,false,8)!=key);
        gpu=device; ++gpu.driver; QVERIFY(PrecisionPlanKey(graph,gpu,false,8)!=key);
        gpu=device; gpu.tensorRtVersion="11.4.0.107"; QVERIFY(PrecisionPlanKey(graph,gpu,false,8)!=key);
        QVERIFY(PrecisionPlanKey(graph,device,true,8)!=key);
        QVERIFY(PrecisionPlanKey(graph,device,false,1)!=key);
        QVERIFY(PrecisionPlanKey(graph,device,false,8,"bucketed")!=key);
        QVERIFY(Unsupported(ModelTier::Medium,ModelTier::Medium,8,false,false,device).isEmpty());
        for (const auto tier : {ModelTier::Tiny,ModelTier::Small}) {
            QVERIFY(!Unsupported(tier,ModelTier::Medium,8,false,false,device).isEmpty());
            QVERIFY(!Unsupported(ModelTier::Medium,tier,8,false,false,device).isEmpty());
        }
        for (const int batch : {1,16}) QVERIFY(!Unsupported(ModelTier::Medium,ModelTier::Medium,batch,false,false,device).isEmpty());
        QVERIFY(!Unsupported(ModelTier::Medium,ModelTier::Medium,8,true,false,device).isEmpty());
        QVERIFY(!Unsupported(ModelTier::Medium,ModelTier::Medium,8,false,true,device).isEmpty());
        for (int change=0; change<4; ++change) {
            gpu=device;
            if(change==0)gpu.major=7;
            if(change==1)gpu.uuid[0]^=1;
            if(change==2)++gpu.driver;
            if(change==3)gpu.tensorRtVersion="11.4.1.0";
            QVERIFY(!Unsupported(ModelTier::Medium,ModelTier::Medium,8,false,false,gpu).isEmpty());
        }
        const auto fallback=ActualStatus(false,"artifact missing");
        QVERIFY(fallback.contains("Recognizer=FP32")); QVERIFY(fallback.contains("artifact missing"));
        QVERIFY(!fallback.contains("graph=BF16"));
        QVERIFY(ActualStatus(true).contains("graph=BF16-linear"));
        QVERIFY(ActualStatus(true).contains("mixed BF16/FP16/FP32"));
        QVERIFY(ActualStatus(true).contains("detector=FP32 medium"));
        // This scope changes only the fixture process and restores its environment.
        struct Restore { QByteArray old=qgetenv(kEnvironment); bool set=qEnvironmentVariableIsSet(kEnvironment);
            ~Restore(){ if(set)qputenv(kEnvironment,old); else qunsetenv(kEnvironment); } } restore;
        qunsetenv(kEnvironment); QVERIFY(!ReadApplicationRequest().Requested());
        qputenv(kEnvironment,""); QVERIFY(!ReadApplicationRequest().Requested());
        qputenv(kEnvironment,"relative.json");
        QVERIFY(ReadApplicationRequest().Requested());
        QVERIFY_EXCEPTION_THROWN(LoadManifest(ReadApplicationRequest(),""),ArtifactError);
        // Public factory remains callable with its unchanged FP32 API. No initialize/inference.
        auto baseline=CreateTensorRtRecognizer();
    }
    void localBf16ArtifactsAreReadOnlyBoundedAndHashChecked()
    {
        using namespace runtime_precision;
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto path=root.filePath("fixture.bin");
        const QByteArray original("bounded synthetic artifact");
        const auto digest=QCryptographicHash::hash(original,QCryptographicHash::Sha256).toHex();
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(original); }
        QCOMPARE(ReadVerified(path,digest,1024),original);
        QVERIFY_EXCEPTION_THROWN(ReadVerified(path,digest,original.size()),ArtifactError);
        QVERIFY_EXCEPTION_THROWN(ReadVerified(root.filePath("absent"),digest,1024),ArtifactError);
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QByteArray(original.size(),'x')); }
        QVERIFY_EXCEPTION_THROWN(ReadVerified(path,digest,1024),ArtifactError);
        CanonicalWithin(root.path(),path);
        QTemporaryDir outside; QVERIFY(outside.isValid());
        const auto external=outside.filePath("external.bin");
        { QFile file(external); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(original); }
        QVERIFY_EXCEPTION_THROWN(CanonicalWithin(root.path(),external),ArtifactError);
        const Selection selection{root.path(),{},Graph{"bf16-linear",root.filePath("missing.onnx"),kRecognizerSource,kRecognizerGraph}};
        const auto before=QDir(root.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot);
        const Device device{QByteArray::fromHex(kDeviceUuid),13040,8};
        QVERIFY_EXCEPTION_THROWN(ReadPlan(selection,device),ArtifactError);
        QCOMPARE(QDir(root.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot),before);
        QVERIFY(!QFile::exists(PlanPath(selection,device))); // never creates plan/receipt/cache directories
    }
    void orientedInkAndMapping() { uct_geometry_test::CpuGeometry(); }
    void postFitDuplicateKeepsAdjacentIdenticalTokens() { uct_geometry_test::DuplicateInk(); }
    void orientedPerspectiveAndConflictActualPaint() { uct_geometry_test::PaintGeometry(); }
    void sourceSurfacePerspective(){uct_surface_test::Cpu();uct_surface_test::LabelsAndSizes();uct_surface_test::Paint();}
    void gpuOrientedInkMatchesCpu() {
#if OKUFLOW_ENABLE_CUDA
        uct_geometry_test::GpuGeometry();
        uct_surface_test::Gpu();
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
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
    void recognizerWidthPolicyKeepsBoundsAndOrderedGroups()
    {
        namespace width = recognizer_width;
        const std::array<std::pair<int,int>,12> cases{{{-1,1024},{0,1024},{1,128},{128,128},
            {129,256},{256,256},{257,512},{512,512},{513,1024},{1024,1024},{1025,1024},{2147483647,1024}}};
        for (const auto [requested,canvas] : cases) {
            QCOMPARE(width::Canvas(requested),canvas); QVERIFY(width::IsCanvas(canvas));
            if (requested>0) QVERIFY(canvas>=std::min(requested,1024));
        }
        QVERIFY(!width::IsCanvas(127)); QVERIFY(!width::IsCanvas(384));
        struct Crop { int validWidth{}, order{}; };
        std::array<Crop,17> crops{};
        const std::array<int,17> requested{64,80,100,127,128,70,90,20,129,180,200,257,300,400,500,200,513};
        for (int i=0; i<int(crops.size()); ++i) crops[i]={requested[i],i};
        const auto before=crops;
        QCOMPARE(width::OrderedGroup(crops.data(),8),128);
        QCOMPARE(width::OrderedGroup(crops.data()+8,8),512);
        QCOMPARE(width::OrderedGroup(crops.data()+16,1),1024);
        QCOMPARE(width::OrderedGroup(crops.data(),16),512);
        QCOMPARE(width::OrderedGroup(crops.data(),17),1024);
        QCOMPARE(width::OrderedGroup(static_cast<const Crop*>(nullptr),1),1024);
        QCOMPARE(width::OrderedGroup(crops.data(),0),1024);
        for (int i=0; i<int(crops.size()); ++i) {
            QCOMPARE(crops[i].order,before[i].order); QCOMPARE(crops[i].validWidth,before[i].validWidth);
        }
        crops[2].validWidth=0; QCOMPARE(width::OrderedGroup(crops.data(),8),1024);
        crops[2].validWidth=-1; QCOMPARE(width::OrderedGroup(crops.data(),8),1024);
    }
    void recognizerWidthRectificationRetainsUsefulAspectSamples()
    {
        QImage image(1024,64,QImage::Format_ARGB32);
        for (int y=0; y<image.height(); ++y)
            for (int x=0; x<image.width(); ++x)
                image.setPixelColor(x,y,QColor(x%256,(x+y*7)%256,(x*3+y)%256));
        for (const int valid : {32,128,129,257,513,1024,2048}) {
            const PaddleQuad quad{{QPointF(0,8),QPointF(valid*.5,8),QPointF(valid*.5,32),QPointF(0,32)},1};
            const int aspectWidth=int(std::ceil(48*QLineF(quad.points[0],quad.points[1]).length()/
                                                QLineF(quad.points[0],quad.points[3]).length()));
            QCOMPARE(aspectWidth,valid);
            const int canvas=recognizer_width::Canvas(aspectWidth);
            const auto fixed=PaddleRectify(image,quad,1024,valid);
            const auto narrow=PaddleRectify(image,quad,canvas,valid);
            QCOMPARE(narrow.size(),std::size_t(canvas)*48*3);
            // Only channel/row stride and zero padding change. Real source
            // samples, rectification aspect and crop coordinates stay exact.
            for (int channel=0; channel<3; ++channel)
                for (const int y : {0,7,23,47}) {
                    for (int x=0; x<std::min(valid,1024); ++x)
                        QCOMPARE(narrow[channel*canvas*48+y*canvas+x],fixed[channel*1024*48+y*1024+x]);
                    for (int x=std::min(valid,canvas); x<canvas; ++x)
                        QCOMPARE(narrow[channel*canvas*48+y*canvas+x],0.f);
                }
        }
    }
    void recognizerWidthProfileRejectsRenamedFixedPlans()
    {
        std::array<std::int64_t,4> shape{1,3,48,128};
        QVERIFY(recognizer_width::MatchesProfile(shape.data(),4,1,128));
        shape[3]=1024; QVERIFY(!recognizer_width::MatchesProfile(shape.data(),4,1,128));
        shape={8,3,48,512}; QVERIFY(recognizer_width::MatchesProfile(shape.data(),4,8,512));
        QVERIFY(!recognizer_width::MatchesProfile(shape.data(),3,8,512));
        QVERIFY(!recognizer_width::MatchesProfile(shape.data(),4,16,512));
        shape[1]=1; QVERIFY(!recognizer_width::MatchesProfile(shape.data(),4,8,512));
        shape={8,3,32,512}; QVERIFY(!recognizer_width::MatchesProfile(shape.data(),4,8,512));
    }
    void gpuRecognizerWidthMetadataDoesNotReorderOrReadPastCount()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{}; if(cudaGetDeviceCount(&devices)!=cudaSuccess || !devices) QSKIP("No CUDA device.");
        FixtureStream stream;
        std::array<gpu::Quad,17> crops{};
        const std::array<int,17> widths{64,80,100,127,128,70,90,20,129,180,200,257,300,400,500,200,513};
        for (int i=0; i<int(crops.size()); ++i) { crops[i].xy[0]=float(i); crops[i].validWidth=widths[i]; }
        GpuBuffer<gpu::Quad> deviceCrops(crops.size()); GpuBuffer<int> count(1), canvases(144);
        QCOMPARE(cudaMemcpyAsync(deviceCrops.data,crops.data(),sizeof(crops),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
        const auto evaluate=[&](int n,int group) {
            std::vector<int> result((1152+group-1)/group,-1);
            if (cudaMemcpyAsync(count.data,&n,sizeof(n),cudaMemcpyHostToDevice,stream.value)!=cudaSuccess) return std::vector<int>{};
            gpu::RecognitionGroupWidths(deviceCrops.data,count.data,group,canvases.data,stream.value);
            if (cudaGetLastError()!=cudaSuccess || cudaMemcpyAsync(result.data(),canvases.data,result.size()*sizeof(int),cudaMemcpyDeviceToHost,stream.value)!=cudaSuccess ||
                cudaStreamSynchronize(stream.value)!=cudaSuccess) return std::vector<int>{};
            return result;
        };
        auto values=evaluate(17,8); QCOMPARE(values.size(),std::size_t(144));
        QCOMPARE(values[0],128); QCOMPARE(values[1],512); QCOMPARE(values[2],1024);
        for (std::size_t i=3; i<values.size(); ++i) QCOMPARE(values[i],0);
        values=evaluate(17,16); QCOMPARE(values.size(),std::size_t(72));
        QCOMPARE(values[0],512); QCOMPARE(values[1],1024);
        for (std::size_t i=2; i<values.size(); ++i) QCOMPARE(values[i],0);
        for (const int invalid : {-1,0,1153}) {
            values=evaluate(invalid,8); QCOMPARE(values.size(),std::size_t(144));
            for (const int value : values) QCOMPARE(value,0);
        }
        std::array<gpu::Quad,17> after{};
        QCOMPARE(cudaMemcpyAsync(after.data(),deviceCrops.data,sizeof(after),cudaMemcpyDeviceToHost,stream.value),cudaSuccess);
        QCOMPARE(cudaStreamSynchronize(stream.value),cudaSuccess);
        for (int i=0; i<int(crops.size()); ++i) {
            QCOMPARE(after[i].xy[0],crops[i].xy[0]); QCOMPARE(after[i].validWidth,crops[i].validWidth);
        }
#else
        QSKIP("CUDA adapter not compiled.");
#endif
    }
    void gpuRecognizerWidthRectificationMatchesCpuAcrossCanvasTransitions()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};
        if (cudaGetDeviceCount(&devices) != cudaSuccess || !devices)
            QSKIP("No CUDA device.");
        QImage image(640, 96, QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, QColor((x * 3 + y * 5) % 256,
                                                (x + y * 7) % 256, (x * 5 + y) % 256));
        constexpr std::size_t guard = 64, fixedSample = 1024 * 48 * 3;
        constexpr float sentinel = 6.25f; // outside normalized source range
        std::array<gpu::Quad, 2> quads{};
        std::array<PaddleQuad, 2> cpuQuads{};
        std::vector<float> reset(guard + 2 * fixedSample + guard, sentinel);
        std::vector<float> actual(reset.size()), fixed(2 * fixedSample);
        GpuBuffer<unsigned char> pixels(image.sizeInBytes());
        GpuBuffer<gpu::Quad> boxes(quads.size());
        GpuBuffer<float> reusable(reset.size()), fixedCrops(fixed.size());
        // On assertion failure, drain before any device allocation or async
        // host source/destination above is destroyed.
        FixtureStream stream;
        QCOMPARE(cudaMemcpyAsync(pixels.data, image.constBits(), image.sizeInBytes(),
                                 cudaMemcpyHostToDevice, stream.value), cudaSuccess);
        // Nonmonotonic transitions exercise row, channel and batch strides on
        // the same high-water allocation. Two crops have distinct useful widths.
        const std::array<std::array<int, 2>, 8> usefulWidths{{
            {32, 127}, {257, 511}, {129, 255}, {513, 1024},
            {128, 31}, {256, 130}, {512, 258}, {1024, 514}}};
        const std::array<int, 8> canvases{128, 512, 256, 1024, 128, 256, 512, 1024};
        for (std::size_t transition = 0; transition < usefulWidths.size(); ++transition)
        {
            for (std::size_t sample = 0; sample < quads.size(); ++sample)
            {
                const int valid = usefulWidths[transition][sample];
                // Half-pixel/fractional positions require real bilinear
                // sampling; crop 1 also straddles the left image boundary.
                const float left = sample ? -3.25f : 8.25f;
                const float top = sample ? 60.5f : 8.5f;
                quads[sample] = {{left, top, left + valid * .5f, top,
                                  left + valid * .5f, top + 24, left, top + 24}, 1, valid};
                for (int corner = 0; corner < 4; ++corner)
                    cpuQuads[sample].points[corner] =
                        {quads[sample].xy[corner * 2], quads[sample].xy[corner * 2 + 1]};
                cpuQuads[sample].score = 1;
                QCOMPARE(int(std::ceil(48 * QLineF(cpuQuads[sample].points[0],
                                                   cpuQuads[sample].points[1]).length() /
                                           QLineF(cpuQuads[sample].points[0],
                                                  cpuQuads[sample].points[3]).length())), valid);
            }
            const int canvas = recognizer_width::OrderedGroup(quads.data(), int(quads.size()));
            QCOMPARE(canvas, canvases[transition]);
            const std::size_t samplePixels = std::size_t(canvas) * 48 * 3;
            QCOMPARE(cudaMemcpyAsync(boxes.data, quads.data(), sizeof(quads),
                                     cudaMemcpyHostToDevice, stream.value), cudaSuccess);
            QCOMPARE(cudaMemcpyAsync(reusable.data, reset.data(), reset.size() * sizeof(float),
                                     cudaMemcpyHostToDevice, stream.value), cudaSuccess);
            for (std::size_t sample = 0; sample < quads.size(); ++sample)
            {
                gpu::Rectify(pixels.data, image.bytesPerLine(), image.width(), image.height(),
                             boxes.data, int(sample), fixedCrops.data + sample * fixedSample,
                             1024, stream.value);
                gpu::Rectify(pixels.data, image.bytesPerLine(), image.width(), image.height(),
                             boxes.data, int(sample), reusable.data + guard + sample * samplePixels,
                             canvas, stream.value);
            }
            QCOMPARE(cudaGetLastError(), cudaSuccess);
            QCOMPARE(cudaMemcpyAsync(actual.data(), reusable.data, actual.size() * sizeof(float),
                                     cudaMemcpyDeviceToHost, stream.value), cudaSuccess);
            QCOMPARE(cudaMemcpyAsync(fixed.data(), fixedCrops.data, fixed.size() * sizeof(float),
                                     cudaMemcpyDeviceToHost, stream.value), cudaSuccess);
            QCOMPARE(cudaStreamSynchronize(stream.value), cudaSuccess);
            for (std::size_t i = 0; i < guard; ++i)
                QCOMPARE(actual[i], sentinel);
            for (std::size_t i = guard + quads.size() * samplePixels; i < actual.size(); ++i)
                QCOMPARE(actual[i], sentinel);
            for (std::size_t sample = 0; sample < quads.size(); ++sample)
            {
                const int valid = usefulWidths[transition][sample];
                const auto reference = PaddleRectify(image, cpuQuads[sample], canvas, valid);
                QCOMPARE(reference.size(), samplePixels);
                for (int channel = 0; channel < 3; ++channel)
                    for (int y = 0; y < 48; ++y)
                        for (int x = 0; x < canvas; ++x)
                        {
                            const std::size_t index = std::size_t(channel) * canvas * 48 + y * canvas + x;
                            const float value = actual[guard + sample * samplePixels + index];
                            // The CPU reference evaluates the quad in double;
                            // CUDA uses float. This bound is <0.032 RGB levels.
                            QVERIFY2(std::abs(value - reference[index]) < .00025f,
                                     "CUDA rectification differs from CPU reference.");
                            if (x < valid)
                                QCOMPARE(value, fixed[sample * fixedSample +
                                    std::size_t(channel) * 1024 * 48 + y * 1024 + x]);
                            else
                                QCOMPARE(value, 0.f);
                        }
            }
        }
#else
        QSKIP("CUDA adapter not compiled.");
#endif
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
    void pausedDetailNormalization()
    {
        QImage image(200,120,QImage::Format_ARGB32);
        image.fill(QColor(255,0,0));
        const DetailFocus focus{{44,32,100,60},kMaximumDetailMagnification};
        QCOMPARE(PaddleDetailMagnification(focus),8.0);
        const auto region=PaddleDetailRegion(image.size(),focus);
        QCOMPARE(region,QRect(34,26,120,72));
        QCOMPARE(PaddleRecognitionRegions(image.size(),focus),QVector<QRect>{region});
        const auto input=PaddleDetectionSize(region.size(),PaddleDetailMagnification(focus));
        QCOMPARE(input,QSize(960,576));
        const auto values=PaddleNormalize(image.copy(region),input,true);
        const auto plane=std::size_t(input.width())*input.height();
        QCOMPARE(values.size(),plane*3);
        // Check actual enlarged CPU tensor contents, including every border.
        for(std::size_t i=0;i<plane;++i) {
            QVERIFY(std::abs(values[i]-(0-.485f)/.229f)<.00001f);
            QVERIFY(std::abs(values[plane+i]-(0-.456f)/.224f)<.00001f);
            QVERIFY(std::abs(values[2*plane+i]-(1-.406f)/.225f)<.00001f);
        }
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
    void mergeKeepsIndependentColumnsAndTypeSizes()
    {
        const QSize size(1920, 1080);
        const QRect full(QPoint(), size);
        const auto word = [](QString text, QRectF bounds) {
            return RecognizedWord{text, bounds, true, bounds, TokenDisposition::kCandidate, .99, bounds};
        };
        const RecognizedLine code{{word("scripts/integrate_castle_runtime.py", {102,249,275,18}),
                                   word("+53", {509,249,27,14}), word("-53", {543,249,23,14})}};
        const RecognizedLine title{{word("The", {643,248,89,40}), word("tribe,", {748,248,128,46}),
                                    word("the", {894,248,79,40}), word("machines", {997,248,221,40})}};
        for (bool reverse : {false, true}) {
            QVector<RecognizedLine> output;
            PaddleMergeRecognition(output, reverse ? title : code, full, size);
            PaddleMergeRecognition(output, reverse ? code : title, full, size);
            int count = 0;
            for (const auto& line : output) {
                QStringList text;
                for (const auto& token : line.words) { text.push_back(token.text); ++count; }
                if (text.contains(QStringLiteral("The")))
                    QCOMPARE(text.join(' '), QStringLiteral("The tribe, the machines"));
            }
            QCOMPARE(count, 7);
        }
        QVector<RecognizedLine> columns;
        PaddleMergeRecognition(columns, {{word("Left", {100,400,100,20})}}, full, size);
        PaddleMergeRecognition(columns, {{word("Right", {320,400,100,20})}}, full, size);
        QCOMPARE(columns.size(), 2);
        QVector<RecognizedLine> fragments;
        PaddleMergeRecognition(fragments, {{word("m", {1000.7155,242.9774,17.3408,51.8340})}}, full, size);
        PaddleMergeRecognition(fragments, {{word("machines", {1005.9677,242.5213,202.7807,51.5256})}}, full, size);
        QCOMPARE(fragments.size(), 1);
        QCOMPARE(fragments.first().words.size(), 1);
        QCOMPARE(fragments.first().words.first().text, QStringLiteral("machines"));
        QVector<RecognizedLine> adjacent;
        PaddleMergeRecognition(adjacent, {{word("m", {100,600,20,20})}}, full, size);
        PaddleMergeRecognition(adjacent, {{word("machines", {110,600,130,20})}}, full, size);
        int adjacentWords = 0;
        for (const auto& row : adjacent) adjacentWords += row.words.size();
        QCOMPARE(adjacentWords, 2); // weaker overlap is not fragment evidence
    }
    void mergedHeadlineAndFilePathPaintTogether()
    {
        uct_test::RegisterFixtureFonts();
        const QSize size(1920, 1080);
        QImage image(size, QImage::Format_ARGB32);
        image.fill(Qt::black);
        QPainter sourcePainter(&image);
        sourcePainter.setPen(Qt::white);
        QFont body(QStringLiteral("Segoe UI")); body.setPixelSize(22);
        QFont titleFont = body; titleFont.setPixelSize(52);
        const auto draw = [&](RecognizedLine& line, const QString& text, QPointF origin, const QFont& font) {
            sourcePainter.setFont(font);
            sourcePainter.drawText(origin, text);
            const QRectF bounds = QFontMetricsF(font).tightBoundingRect(text).translated(origin);
            line.words.push_back({text, bounds, false, {}, TokenDisposition::kCandidate, .99, bounds});
        };
        RecognizedLine code, title;
        draw(code, QStringLiteral("scripts/integrate_castle_runtime.py"), {100,270}, body);
        qreal x = 650;
        for (const auto& text : QStringLiteral("The tribe, the machines").split(' ')) {
            draw(title, text, {x,290}, titleFont);
            x += QFontMetricsF(titleFont).horizontalAdvance(text + ' ');
        }
        sourcePainter.end();
        QVector<RecognizedLine> merged;
        PaddleMergeRecognition(merged, code, QRect(QPoint(), size), size);
        PaddleMergeRecognition(merged, title, QRect(QPoint(), size), size);
        const auto result = [&](std::uint64_t frame, std::int64_t timestamp) {
            RecognitionResult r;
            r.stamp.sourceEpoch = 1; r.stamp.geometryGeneration = 1;
            r.stamp.frameSeq = frame; r.stamp.timestampMs = timestamp;
            r.stamp.viewportWidth = size.width(); r.stamp.viewportHeight = size.height();
            r.stamp.viewTransform = {0,0,1,1,0,0,1,1,true};
            r.output.ok = true; r.output.lines = merged;
            for (const auto& line : merged) { Q_UNUSED(line); r.lineColors.push_back({Qt::white, Qt::black}); }
            const auto pixels = uct_test::IndependentFramePixels(image, frame);
            r.sourcePixelDigest = uct_test::SourcePixelsDigest(pixels);
            r.reference = MakeProbeFrame(pixels.constBits(), pixels.width(), pixels.height(),
                pixels.bytesPerLine(), 2, r.stamp);
            return r;
        };
        const auto first = result(1,100), second = result(2,200);
        OverlayModel model;
        model.Admit(first, first.stamp, nullptr, 110);
        QCOMPARE(model.Admit(second, second.stamp, nullptr, 210), AdmitVerdict::kAccepted);
        const auto words = model.SceneWords(210, second.stamp.frameSeq);
        QCOMPARE(words.size(), 5);
        QWidget viewport; viewport.resize(size); viewport.show();
        okuflow::UltraClearTextOverlay overlay(&viewport, &viewport);
        overlay.SetViewTransform(second.stamp.viewTransform);
        QVector<okuflow::UltraClearTextOverlay::SceneWord> glyphs;
        for (const auto& word : words)
            glyphs.push_back({word.text,word.sceneRect,word.colors,word.lineId,word.protectedSceneRects,
                             word.wordIndex,word.sceneQuad,word.layoutGroupId,word.carvedSceneRects});
        overlay.SetWords(std::move(glyphs));
        QTRY_VERIFY(overlay.PaintedGeometryCurrent());
        QImage layer(size, QImage::Format_ARGB32_Premultiplied); layer.fill(Qt::transparent);
        overlay.render(&layer);
        for (const auto& word : words) {
            okuflow::UltraClearTextOverlay::PaintedWordGeometry geometry;
            QVERIFY(overlay.PaintedWord(word.lineId,word.wordIndex,word.text,geometry));
            QVERIFY2(geometry.drawn, qPrintable(word.text));
            const QRect region = geometry.box.adjusted(-2,-2,2,2).toAlignedRect().intersected(layer.rect());
            int whiteInk = 0;
            for (int y = region.top(); y <= region.bottom(); ++y)
                for (int px = region.left(); px <= region.right(); ++px) {
                    const auto color = layer.pixel(px,y);
                    whiteInk += qAlpha(color) > 0 && qRed(color) > 200;
                }
            QVERIFY2(whiteInk > 5, qPrintable(word.text));
        }
    }
    void regionalOwnershipUsesBackendDetectorInterior_data()
    {
        QTest::addColumn<bool>("vertical");
        QTest::newRow("detector-tail-dropped-despite-contained-ink") << false;
        QTest::newRow("detector-top-dropped-without-new-probe") << true;
    }
    void regionalOwnershipUsesBackendDetectorInterior()
    {
        QFETCH(bool,vertical);
        uct_test::RegisterFixtureFonts();
        const QSize size(960,540);
        const QRect region = vertical ? QRect(0,60,400,200) : QRect(0,0,400,200);
        QImage image(size,QImage::Format_ARGB32); image.fill(Qt::white);
        QPainter painter(&image); painter.setPen(Qt::black);
        QFont font(QStringLiteral("Segoe UI")); font.setPixelSize(24); painter.setFont(font);
        RecognizedLine fitted,detected;
        const auto word = [&](QString text,QRectF ink,QRectF detector) {
            painter.drawText(ink.bottomLeft(),text);
            RecognizedWord fit{text,ink}; fit.protectedBoundsPx=detector; fit.inkFitted=true;
            fitted.words.push_back(fit);
            RecognizedWord raw{text,detector.translated(-region.topLeft())};
            raw.protectedBoundsPx=raw.boundsPx;
            detected.words.push_back(raw);
        };
        if (vertical) word(QStringLiteral("here"),{125,72,55,22},{120,64,70,35});
        else {
            word(QStringLiteral("end"),{265,75,35,25},{260,70,45,35});
            word(QStringLiteral("here"),{355,75,34,25},{350,70,47,35});
        }
        painter.end();
        const auto result = [&](std::uint64_t frame,std::int64_t time) {
            RecognitionResult r;
            r.stamp.sourceEpoch=1; r.stamp.geometryGeneration=1;
            r.stamp.frameSeq=frame; r.stamp.timestampMs=time;
            r.stamp.viewportWidth=size.width(); r.stamp.viewportHeight=size.height();
            r.stamp.viewTransform={0,0,1,1,0,0,1,1,true};
            r.output.ok=true; r.output.lines={fitted};
            const auto pixels=uct_test::IndependentFramePixels(image,frame);
            r.sourcePixelDigest=uct_test::SourcePixelsDigest(pixels);
            r.reference=MakeProbeFrame(pixels.constBits(),pixels.width(),pixels.height(),
                pixels.bytesPerLine(),2,r.stamp);
            return r;
        };
        const auto first=result(1,100),second=result(2,200);
        OverlayModel model;
        model.Admit(first,first.stamp,nullptr,110);
        QCOMPARE(model.Admit(second,second.stamp,nullptr,210),AdmitVerdict::kAccepted);
        const auto old=model.Lines().first();
        const auto reading=model.ReadingText(310,3);
        const auto screen=model.ScreenLines(310,3);
        QCOMPARE(reading,vertical ? QStringLiteral("here") : QStringLiteral("end here"));
        QVector<RecognizedLine> merged;
        PaddleMergeRecognition(merged,detected,region,size); // real production edge rejection
        QCOMPARE(merged.size(),vertical ? 0 : 1);
        if (!vertical) {
            QCOMPARE(merged.first().words.size(),1);
            QCOMPARE(merged.first().words.first().text,QStringLiteral("end"));
        }
        auto roi=result(3,300); roi.output.lines=merged; roi.output.detailRegionPx=region;
        model.Admit(roi,roi.stamp,nullptr,310); // no newer validating probe is available
        QCOMPARE(model.ReadingText(310,3),reading);
        QCOMPARE(model.Lines().size(),1);
        QCOMPARE(model.Lines().first().id,old.id);
        QCOMPARE(model.Lines().first().readingTokens,old.readingTokens);
        QCOMPARE(model.Lines().first().protectedSceneRects,old.protectedSceneRects);
        QCOMPARE(model.Lines().first().carvedSceneRects,old.carvedSceneRects);
        QCOMPARE(model.Lines().first().validatedFrameSeq,old.validatedFrameSeq);
        QCOMPARE(model.Lines().first().validatedAtMs,old.validatedAtMs);
        QCOMPARE(model.SceneWords(310,3).size(),fitted.words.size());
        QCOMPARE(model.ScreenLines(310,3).first().lineId,screen.first().lineId);
        QCOMPARE(model.PendingWordCount(),0);
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
        // Retain the exact asynchronous TensorRT closure through diagnostic
        // process exit, including early QtTest assertion returns.
        auto retirement=gpu?std::make_unique<uct_batch_test::RetirementReceipts>():nullptr;
        auto backend=gpu?CreateTensorRtRecognizer({},{},ModelTier::Medium,ModelTier::Medium,retirement->Observer())
                        :CreatePaddleCpuRecognizer();
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
        backend.reset();
        if(retirement)QVERIFY2(retirement->Wait().Passed(),"Exact diagnostic TensorRT State closure failed.");
    }
    void optionalPausedVideoProof()
    {
        uct_paused_video_test::Run();
    }
    void optionalActiveRegionalProof()
    {
        uct_active_regional_test::Run();
    }
    void supplementalFailureKeepsPreferredBackend()
    {
        uct_startup_test::Calls gpu, cpu, windows;
        auto backend = uct_startup_test::Make(gpu, cpu, windows);
        QCOMPARE(backend->Initialize().state, BackendState::kReady);
        std::atomic<bool> cancelled{};
        QVERIFY(backend->Recognize({}, cancelled).ok);
        gpu.fail = true;
        backend->SetDetailFocus(DetailFocus{QRect(0, 0, 160, 96), 1.0, 480, true});
        QVERIFY(!backend->Recognize({}, cancelled).ok);
        QCOMPARE(backend->Status().name, QStringLiteral("GPU"));
        QCOMPARE(gpu.initialized, 1);
        QCOMPARE(gpu.recognized, 2);
        QCOMPARE(cpu.initialized, 0); QCOMPARE(cpu.recognized, 0);
        QCOMPARE(windows.initialized, 0); QCOMPARE(windows.recognized, 0);
        backend->SetDetailFocus(std::nullopt);
        gpu.fail = false;
        QVERIFY(backend->Recognize({}, cancelled).ok);
        QCOMPARE(backend->Status().name, QStringLiteral("GPU"));
        QCOMPARE(gpu.initialized, 1); QCOMPARE(gpu.recognized, 3);
        QCOMPARE(cpu.initialized, 0); QCOMPARE(windows.initialized, 0);
    }
    void supplementalPreparingDoesNotStartCpuBridge()
    {
        uct_startup_test::Calls gpu, cpu, windows;
        gpu.preparing = true;
        auto backend = uct_startup_test::Make(gpu, cpu, windows);
        QCOMPARE(backend->Initialize().state, BackendState::kReady);
        std::atomic<bool> cancelled{};
        backend->SetDetailFocus(DetailFocus{QRect(0, 0, 160, 96), 1.0, 480, true});
        const auto output = backend->Recognize({}, cancelled);
        QVERIFY(!output.ok && output.backendPreparing);
        QCOMPARE(gpu.recognized, 0);
        QCOMPARE(cpu.initialized, 0); QCOMPARE(cpu.recognized, 0);
        QCOMPARE(windows.initialized, 0); QCOMPARE(windows.recognized, 0);
        backend->SetDetailFocus(std::nullopt);
        gpu.preparing = false;
        QVERIFY(backend->Recognize({}, cancelled).ok);
        QCOMPARE(backend->Status().name, QStringLiteral("GPU"));
        QCOMPARE(gpu.initialized, 1); QCOMPARE(gpu.recognized, 1);
        QCOMPARE(cpu.initialized, 0); QCOMPARE(windows.initialized, 0);
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
    void gpuLocalColorDescriptorsMatchCpu_data()
    {
        QTest::addColumn<int>("sample");
        const auto fixtures = uct_test::LocalColorFixtures();
        for (int i = 0; i < fixtures.size(); ++i)
            QTest::newRow(qPrintable(fixtures[i].name)) << i;
    }
    void gpuLocalColorDescriptorsMatchCpu()
    {
#if OKUFLOW_ENABLE_CUDA
        int devices{};
        if (cudaGetDeviceCount(&devices) != cudaSuccess || !devices) QSKIP("No CUDA device.");
        QFETCH(int, sample);
        const auto f = uct_test::LocalColorFixtures()[sample];
        if (f.expectInkCorners) QVERIFY2(uct_test::SolidInkCorners(f), qPrintable(f.name));
        struct Backend final : Recognizer {
            RecognitionOutput output;
            BackendStatus Initialize() override { return {BackendState::kReady, "colors", "", ""}; }
            RecognitionOutput Recognize(const QImage&, const std::atomic<bool>&) override { return output; }
        };
        auto backend = std::make_unique<Backend>(); backend->output.ok = true;
        RecognizedLine line; line.words.push_back({QStringLiteral("Button"), QRectF(f.bounds)});
        backend->output.lines.push_back(line);
        RecognitionResult cpu;
        std::atomic<bool> received{false};
        RecognitionScheduler scheduler(std::move(backend), [&](RecognitionResult result) {
            cpu = std::move(result); received.store(true);
        }, {});
        SourceStamp stamp; stamp.sourceEpoch = 1; stamp.geometryGeneration = 1; stamp.frameSeq = 1;
        stamp.viewportWidth = f.pixels.width(); stamp.viewportHeight = f.pixels.height();
        stamp.viewTransform = {0,0,1,1,0,0,1,1,true};
        QVERIFY(scheduler.Submit(stamp, f.pixels)); QTRY_VERIFY(received.load());
        QVERIFY(cpu.output.ok); QCOMPARE(cpu.lineColors.size(), 1);
        QVERIFY(cpu.output.lines[0].words[0].colors);
        FixtureStream stream;
        GpuBuffer<unsigned char> pixels(f.pixels.sizeInBytes());
        GpuBuffer<gpu::Rect> rectangle(1); GpuBuffer<gpu::Colors> descriptor(1);
        // Production resident::Rect clips on the host before dispatch too.
        const QRect clipped = f.bounds.intersected(f.pixels.rect());
        const gpu::Rect rect{clipped.x(), clipped.y(), clipped.width(), clipped.height()};
        QCOMPARE(cudaMemcpyAsync(pixels.data, f.pixels.constBits(), f.pixels.sizeInBytes(),
                                 cudaMemcpyHostToDevice, stream.value), cudaSuccess);
        QCOMPARE(cudaMemcpyAsync(rectangle.data, &rect, sizeof(rect), cudaMemcpyHostToDevice, stream.value), cudaSuccess);
        gpu::LineColors(pixels.data, f.pixels.bytesPerLine(), f.pixels.width(), f.pixels.height(),
                        rectangle.data, 1, descriptor.data, stream.value);
        QCOMPARE(cudaGetLastError(), cudaSuccess);
        gpu::Colors raw{};
        // Only the 24-byte descriptor crosses back; resident image pixels do not.
        QCOMPARE(cudaMemcpyAsync(&raw, descriptor.data, sizeof(raw), cudaMemcpyDeviceToHost, stream.value), cudaSuccess);
        QCOMPARE(cudaStreamSynchronize(stream.value), cudaSuccess);
        const QColor ink(raw.foreground[0], raw.foreground[1], raw.foreground[2]);
        const QColor paper(raw.background[0], raw.background[1], raw.background[2]);
        QCOMPARE(ink, cpu.lineColors[0].foreground); QCOMPARE(paper, cpu.lineColors[0].background);
        QCOMPARE(ink, cpu.output.lines[0].words[0].colors->foreground);
        QCOMPARE(paper, cpu.output.lines[0].words[0].colors->background);
        if (f.expectSourceColors) {
            QVERIFY2(uct_test::ColorDistance(ink, f.ink) <= f.tolerance, qPrintable(f.name));
            QVERIFY2(uct_test::ColorDistance(paper, f.paper) <= f.tolerance, qPrintable(f.name));
        }
#else
        QSKIP("CUDA adapter not compiled.");
#endif
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
        for(int phase=0;phase<5;++phase){
            QImage image(640,360,QImage::Format_ARGB32);image.fill(phase==2?Qt::white:QColor(246,244,236));
            if(phase!=2){QPainter painter(&image);painter.drawImage(phase==1?QPoint(3,2):QPoint(),page.image);}
            if(phase==4){
                QVERIFY(rect.right()*640 < 380); // do not paint over the reference's own ink
                QPainter painter(&image);painter.fillRect(400,0,240,360,Qt::black);
            }
            auto gpuProbe=prepare(image,phase+2);QVERIFY(gpuProbe.IsValid());
            auto cpuProbe=MakeProbeFrame(image.constBits(),640,360,image.bytesPerLine(),decimation,stamp);
            if(phase==3){ // Reproject both references through a cropped/panned view.
                gpuProbe.stamp.viewTransform={.1f,.05f,.8f,.9f,0,0,1,1,true};cpuProbe.stamp=gpuProbe.stamp;
            }
            const QPointF seed=phase==1?QPointF(3./640,2./360):QPointF();
            for(const int radius:{6,1000,-3}) {
                MatchTuning tuning;tuning.searchRadiusPx=radius;
                const auto c=MatchPatch(cpuPatch,cpuProbe,tuning,seed);
                if(phase==4) QVERIFY(c.matched); // independent right region cannot revoke left ink
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
            if(phase==4){QVERIFY(a.has_value());QVERIFY(*a<.92);} // globally moving, locally unchanged
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
    void gpuQuadReferencesNormalizeRotationAndMatchCpuWithoutImageEgress()
    {
#if OKUFLOW_ENABLE_CUDA
        using namespace uct_quad_reference_test;
        int devices{}; if(cudaGetDeviceCount(&devices)!=cudaSuccess || !devices) QSKIP("No CUDA device.");
        const auto source=Render(.48), target=Render(.57), changed=Render(.57,true);
        FixtureStream stream; GpuBuffer<unsigned char> pixels(source.image.sizeInBytes()); AnalysisPool pool;
        std::atomic<bool> cancel{false};
        for(const bool perspective : {false,true}) for(const int decimation : {1,2,4}) {
            const auto a=perspective ? Render(.48,false,true) : source;
            const auto b=perspective ? Render(.57,false,true) : target;
            const auto c=perspective ? Render(.57,true,true) : changed;
            const auto prepare=[&](const Fixture& fixture,int frame) {
                cudaMemcpyAsync(pixels.data,fixture.image.constBits(),fixture.image.sizeInBytes(),cudaMemcpyHostToDevice,stream.value);
                auto owned=pool.Capture(pixels.data,fixture.image.bytesPerLine(),640,360,stream.value,1,frame,1);
                RecognitionResult result; result.stamp=uct_quad_reference_test::Stamp(frame); QString error;
                if(owned) owned->PrepareResidentAnalysis(result,decimation,true,cancel,error);
                return result.reference;
            };
            const auto gpuSource=prepare(a,1), gpuTarget=prepare(b,2), gpuChanged=prepare(c,3);
            QVERIFY(gpuSource.resident); QVERIFY(gpuTarget.resident); QVERIFY(gpuChanged.resident);
            QVERIFY(gpuSource.luma.isNull()); QVERIFY(gpuTarget.luma.isNull());
            const auto cpuSource=Probe(a,1,decimation), cpuTarget=Probe(b,2,decimation), cpuChanged=Probe(c,3,decimation);
            const auto cpuPatch=BuildQuadReferencePatch(cpuSource,a.scene);
            const auto gpuPatch=BuildQuadReferencePatch(gpuSource,a.scene);
            QVERIFY(cpuPatch.valid); QVERIFY(gpuPatch.valid); QVERIFY(gpuPatch.resident); QVERIFY(gpuPatch.values.isEmpty());
            QCOMPARE(gpuPatch.columns,cpuPatch.columns); QCOMPARE(gpuPatch.rows,cpuPatch.rows);
            const MatchTuning strict{.94,.95,2};
            for(const bool different : {false,true}) {
                const auto& cpuProbe=different ? cpuChanged : cpuTarget;
                const auto& gpuProbe=different ? gpuChanged : gpuTarget;
                const auto expected=MatchPatch(Reproject(cpuPatch,cpuProbe,b.scene),cpuProbe,strict);
                if(!perspective) QCOMPARE(expected.matched,!different);
                for(const auto& original : {cpuPatch,gpuPatch}) {
                    const auto patch=Reproject(original,gpuProbe,b.scene);
                    const auto matches=gpuProbe.resident->Matches(gpuProbe.stamp,{patch},strict,{});
                    QCOMPARE(matches.size(),1); QCOMPARE(matches[0].matched,expected.matched);
                    QCOMPARE(matches[0].visible,expected.visible);
                    QVERIFY2(std::abs(matches[0].score-expected.score)<1e-5,
                        qPrintable(QString("perspective=%1 decimation=%2 changed=%3 GPU=%4 CPU=%5")
                            .arg(perspective).arg(decimation).arg(different).arg(matches[0].score,0,'g',14).arg(expected.score,0,'g',14)));
                }
            }
        }
        for(const double angle : {-.3,-.012,.012,.032,.48}) for(const bool perspective : {false,true}) {
            const auto a=Render(angle,false,perspective);
            for(const int control : {0,1,2,3,4,5}) {
                const auto b=Render(angle+.04,control==2,perspective);
                VerifyWordProof(a,b,control,true,[&](RecognitionScheduler& scheduler,const SourceStamp& stamp,
                    const Fixture& frame,CorroborationRequest request) {
                    if(cudaMemcpyAsync(pixels.data,frame.image.constBits(),frame.image.sizeInBytes(),
                        cudaMemcpyHostToDevice,stream.value)!=cudaSuccess)return std::uint64_t{};
                    auto owned=pool.Capture(pixels.data,frame.image.bytesPerLine(),640,360,stream.value,
                        stamp.sourceEpoch,stamp.frameSeq,stamp.geometryGeneration);
                    return scheduler.SubmitDevice(stamp,std::move(owned),std::move(request));
                });
            }
        }
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

        {
            // Production GPU detail uses a strided subview of the owned scene.
            // Exercise the new 8x geometry and actual kernel against CPU data.
            QImage source(200,120,QImage::Format_ARGB32);
            for(int y=0;y<source.height();++y) for(int x=0;x<source.width();++x)
                source.setPixelColor(x,y,QColor(x,y,(x+y)%256));
            const DetailFocus focus{{44,32,100,60},kMaximumDetailMagnification};
            const auto region=PaddleDetailRegion(source.size(),focus);
            QCOMPARE(region,QRect(34,26,120,72));
            const auto input=PaddleDetectionSize(region.size(),PaddleDetailMagnification(focus));
            QCOMPARE(input,QSize(960,576));
            GpuBuffer<unsigned char> owned(source.sizeInBytes());
            QCOMPARE(cudaMemcpyAsync(owned.data,source.constBits(),source.sizeInBytes(),cudaMemcpyHostToDevice,stream.value),cudaSuccess);
            const auto count=std::size_t(input.width())*input.height()*3;
            GpuBuffer<float> tensor(count);
            std::vector<float> actual(count);
            const auto* crop=owned.data+std::size_t(region.y())*source.bytesPerLine()+region.x()*4;
            gpu::Normalize(crop,source.bytesPerLine(),region.width(),region.height(),tensor.data,
                           input.width(),input.height(),stream.value);
            QCOMPARE(cudaMemcpyAsync(actual.data(),tensor.data,count*sizeof(float),cudaMemcpyDeviceToHost,stream.value),cudaSuccess);
            QCOMPARE(cudaStreamSynchronize(stream.value),cudaSuccess);
            const auto expected=PaddleNormalize(source.copy(region),input,true);
            QCOMPARE(actual.size(),expected.size());
            for(std::size_t i=0;i<count;++i) QVERIFY(std::abs(actual[i]-expected[i])<.00001f);
        }

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
