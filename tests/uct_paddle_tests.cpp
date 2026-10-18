#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/cuda/uct_analysis_pool.hpp"
#include <QFontDatabase>
#include <QPainter>
#include <QtTest>
#include <limits>
#include <stdexcept>
#if OKUFLOW_ENABLE_CUDA
#include "okuflow/cuda/uct_analysis_pool.hpp"
#include "okuflow/cuda/uct_kernels.hpp"
#endif

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
        QVERIFY(PaddleDecode(values, 8, 3, quad, dictionary).words.isEmpty());
        values[1] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(PaddleDecode(values, 8, 4, quad, dictionary).words.isEmpty());
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
    void optionalRealCpu()
    {
        if (!qEnvironmentVariableIsSet("OKUFLOW_TEST_PADDLE_RUNTIME"))
            QSKIP("Explicit model-runtime acceptance not selected.");
        auto recognizer = CreatePaddleCpuRecognizer();
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
        QElapsedTimer timer;
        timer.start();
        const auto output = recognizer->Recognize(page, cancelled);
        QVERIFY2(output.ok, qPrintable(output.error));
        QStringList words;
        for (const auto &line : output.lines)
            for (const auto &word : line.words)
                words.push_back(word.text);
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
        for (int iteration = 0; iteration < 2; ++iteration)
        {
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
                    words.push_back(word.text);
            qInfo() << "Paddle TensorRT iteration" << iteration << "ms" << timer.elapsed() << words;
            for (const auto &expected :
                 QString("MAGNIFIED PRINTED TEXT READS CLEARLY TODAY").split(' '))
                QVERIFY2(words.contains(expected), qPrintable(expected));
        }
    }
};
QTEST_MAIN(PaddleTests)
#include "uct_paddle_tests.moc"
