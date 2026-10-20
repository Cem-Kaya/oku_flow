#include "okuflow/common/uct_paddle.hpp"

#if OKUFLOW_UCT_TRT
#include "okuflow/common/async_cleanup.hpp"
#include "okuflow/cuda/uct_analysis_pool.hpp"
#include "okuflow/cuda/uct_kernels.hpp"
#include "uct_tensorrt_profile.hpp"
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cuda_runtime.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace okuflow::uct
{
namespace
{
void Check(cudaError_t status)
{
    if (status != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(status));
}
template <class T> class DeviceBuffer
{
  public:
    ~DeviceBuffer()
    {
        if (data_)
            cudaFree(data_);
    }
    T *Reserve(std::size_t count)
    {
        if (count > capacity_)
        {
            auto *old = std::exchange(data_, nullptr);
            capacity_ = 0;
            if (old)
                Check(cudaFree(old));
            Check(cudaMalloc(reinterpret_cast<void **>(&data_), count * sizeof(T)));
            capacity_ = count;
        }
        return data_;
    }
    T *Get() const
    {
        return data_;
    }
    std::size_t Capacity() const { return capacity_; }

  private:
    T *data_{};
    std::size_t capacity_{};
};
template <class T> class HostBuffer
{
  public:
    ~HostBuffer()
    {
        if (data_)
            cudaFreeHost(data_);
    }
    T *Reserve(std::size_t count)
    {
        if (count > capacity_)
        {
            auto *old = std::exchange(data_, nullptr);
            capacity_ = 0;
            if (old)
                Check(cudaFreeHost(old));
            Check(cudaHostAlloc(reinterpret_cast<void **>(&data_), count * sizeof(T),
                                cudaHostAllocDefault));
            capacity_ = count;
        }
        return data_;
    }
    T *Get() const
    {
        return data_;
    }

  private:
    T *data_{};
    std::size_t capacity_{};
};
class Logger final : public nvinfer1::ILogger
{
  public:
    std::string Error()
    {
        std::lock_guard lock(mutex_);
        return error_;
    }
    void log(Severity severity, const char *message) noexcept override
    {
        if (severity <= Severity::kERROR)
        {
            try
            {
                std::lock_guard lock(mutex_);
                error_ = message ? message : "TensorRT error";
            }
            catch (...)
            {
            }
        }
    }

  private:
    std::mutex mutex_;
    std::string error_;
};
struct DllDirectory
{
    DLL_DIRECTORY_COOKIE value{};
    ~DllDirectory()
    {
        if (value)
            RemoveDllDirectory(value);
    }
};
struct Libraries
{
    HMODULE inference{}, parser{}, builderResource{};
    DllDirectory sdkDirectory, cudaDirectory;
    using Factory = void *(*)(void *, int);
    using ParserFactory = void *(*)(void *, void *, int);
    Factory builder{}, runtime{};
    ParserFactory createParser{};
    explicit Libraries(const QString &sdk)
    {
        const auto bin = QDir(sdk).filePath("bin").toStdWString();
        sdkDirectory.value = AddDllDirectory(bin.c_str());
        auto cuda = qEnvironmentVariable("OKUFLOW_UCT_CUDA_BIN");
        if (cuda.isEmpty())
        {
            const auto system = qEnvironmentVariable("CUDA_PATH_V12_9");
            if (!system.isEmpty())
                cuda = QDir(system).filePath("bin");
            else
                cuda = QDir(qEnvironmentVariable("LOCALAPPDATA"))
                           .filePath("OkuFlow/toolchains/cuda-12.9.2/toolkit/bin");
        }
        const auto cudaBin = cuda.toStdWString();
        cudaDirectory.value = AddDllDirectory(cudaBin.c_str());
        const auto inferPath = QDir(sdk).filePath("bin/nvinfer_11.dll").toStdWString();
        inference =
            LoadLibraryExW(inferPath.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!inference)
            throw std::runtime_error("TensorRT 11 runtime is unavailable.");
        using Version = int32_t (*)();
        const char *versionNames[]{"getInferLibMajorVersion", "getInferLibMinorVersion",
                                   "getInferLibPatchVersion", "getInferLibBuildVersion"};
        const int expected[]{11, 4, 0, 106};
        for (int i = 0; i < 4; ++i)
        {
            const auto version =
                reinterpret_cast<Version>(GetProcAddress(inference, versionNames[i]));
            if (!version || version() != expected[i])
                throw std::runtime_error("TensorRT must match the pinned 11.4.0.106 SDK.");
        }
        runtime =
            reinterpret_cast<Factory>(GetProcAddress(inference, "createInferRuntime_INTERNAL"));
        if (!runtime)
            throw std::runtime_error("TensorRT runtime factory API is unavailable.");
    }
    void PrepareBuilder(const QString &sdk)
    {
        if (builder && createParser)
            return;
        // Cached plans require only the inference runtime. The architecture
        // resource is hundreds of MiB and belongs to cache-miss compilation.
        const auto parserPath = QDir(sdk).filePath("bin/nvonnxparser_11.dll").toStdWString();
        parser =
            LoadLibraryExW(parserPath.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!parser)
            throw std::runtime_error("TensorRT 11 parser is unavailable.");
        int device{};
        cudaDeviceProp properties{};
        Check(cudaGetDevice(&device));
        Check(cudaGetDeviceProperties(&properties, device));
        auto resource = QDir(sdk).filePath(QString("bin/nvinfer_builder_resource_sm%1_11.dll")
                                               .arg(properties.major * 10 + properties.minor));
        if (!QFile::exists(resource))
            resource = QDir(sdk).filePath("bin/nvinfer_builder_resource_ptx_11.dll");
        const auto resourcePath = resource.toStdWString();
        // TensorRT's lazy loader uses a basename. Preload the exact SDK image
        // with explicit search flags instead of changing the process PATH.
        builderResource =
            LoadLibraryExW(resourcePath.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!builderResource)
            throw std::runtime_error("TensorRT builder resource/dependencies are unavailable.");
        builder =
            reinterpret_cast<Factory>(GetProcAddress(inference, "createInferBuilder_INTERNAL"));
        createParser =
            reinterpret_cast<ParserFactory>(GetProcAddress(parser, "createNvOnnxParser_INTERNAL"));
        if (!builder || !createParser)
            throw std::runtime_error("TensorRT builder/parser factory API is unavailable.");
    }
    ~Libraries()
    {
        // TensorRT may retain internal worker/tactic resources. Keep these
        // exact modules loaded for process lifetime; do not unload late work.
    }
};
class Progress final : public nvinfer1::IProgressMonitor
{
  public:
    explicit Progress(const std::atomic<bool> &cancelled) : cancelled_(cancelled)
    {
    }
    void phaseStart(const char *, const char *, int32_t) noexcept override
    {
    }
    void phaseFinish(const char *) noexcept override
    {
    }
    bool stepComplete(const char *, int32_t) noexcept override
    {
        return !cancelled_.load();
    }

  private:
    const std::atomic<bool> &cancelled_;
};
class Network
{
  public:
    Network(Libraries &libraries, Logger &logger, const QString &sdk, const QString &model, const QByteArray& expected, bool detection, int device,
            cudaStream_t stream, const std::atomic<bool> &cancelled, int maximumBatch = 1)
        : detection_(detection), maximumBatch_(maximumBatch),
          // A ceiling for checked on-demand allocation, not reserved VRAM.
          maximumOutputFloats_(!detection && maximumBatch == 16 ? 256ull * 1024 * 1024
                                                               : 64ull * 1024 * 1024)
    {
        Progress progress(cancelled);
        runtime_.reset(
            static_cast<nvinfer1::IRuntime *>(libraries.runtime(&logger, NV_TENSORRT_VERSION)));
        if (!runtime_)
            throw std::runtime_error("TensorRT runtime creation failed.");
        QFile modelFile(model);
        if (!modelFile.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read pinned OCR graph.");
        const auto bytes = modelFile.readAll();
        if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != expected)
            throw std::runtime_error("Pinned OCR graph changed before preparation.");
        cudaDeviceProp properties{};
        Check(cudaGetDeviceProperties(&properties, device));
        int driver{};
        Check(cudaDriverGetVersion(&driver));
        QByteArray identity =
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) +
            QByteArray(reinterpret_cast<const char *>(&properties.uuid), sizeof(properties.uuid)) +
            QByteArray::number(driver) + "-trt11.4.0.106-fp32-v1";
        if (!detection && (maximumBatch == 8 || maximumBatch == 16))
            identity += "-rec-min1x3x48x1024-opt" + QByteArray::number(maximumBatch) +
                        "x3x48x1024-max" + QByteArray::number(maximumBatch) + "x3x48x1024-v2";
        else if (maximumBatch != 1)
            throw std::runtime_error("Unsupported TensorRT OCR batch profile.");
        const auto key = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
        const auto cache =
            QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/ocr-engines");
        QDir().mkpath(cache);
        const auto path = QDir(cache).filePath(QString::fromLatin1(key) + ".plan");
        QFile cached(path);
        if (cached.open(QIODevice::ReadOnly) && cached.size() > 32 &&
            cached.size() < 256 * 1024 * 1024)
        {
            const auto digest = cached.read(32), plan = cached.readAll();
            if (QCryptographicHash::hash(plan, QCryptographicHash::Sha256) == digest)
                engine_.reset(runtime_->deserializeCudaEngine(plan.constData(), plan.size()));
        }
        if (!engine_)
        {
            libraries.PrepareBuilder(sdk);
            std::unique_ptr<nvinfer1::IBuilder> builder(
                static_cast<nvinfer1::IBuilder *>(libraries.builder(&logger, NV_TENSORRT_VERSION)));
            if (!builder)
                throw std::runtime_error("TensorRT builder creation failed: " + logger.Error());
            std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
            if (!network)
                throw std::runtime_error("TensorRT network creation failed.");
            std::unique_ptr<nvonnxparser::IParser> parser(static_cast<nvonnxparser::IParser *>(
                libraries.createParser(network.get(), &logger, NV_ONNX_PARSER_VERSION)));
            if (!network || !parser || !parser->parse(bytes.constData(), bytes.size()))
                throw std::runtime_error("PaddleOCR graph is unsupported by TensorRT: " +
                                         logger.Error());
            if (network->getNbInputs() != 1 || network->getNbOutputs() != 1)
                throw std::runtime_error("Unexpected TensorRT OCR graph I/O.");
            std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
            if (!config)
                throw std::runtime_error("TensorRT builder configuration failed.");
            config->setProgressMonitor(&progress);
            config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 256ull * 1024 * 1024);
            config->setMaxAuxStreams(0);
            config->setBuilderOptimizationLevel(2);
            config->setProfileStream(stream);
            auto *profile = builder->createOptimizationProfile();
            if (!profile)
                throw std::runtime_error("TensorRT shape profile creation failed.");
            const char *name = network->getInput(0)->getName();
            const nvinfer1::Dims4 minimum(1, 3, detection ? 32 : 48, detection ? 32 : 1024);
            const nvinfer1::Dims4 optimum(detection ? 1 : maximumBatch, 3,
                                         detection ? 544 : 48, detection ? 960 : 1024);
            const nvinfer1::Dims4 maximum(detection ? 1 : maximumBatch, 3,
                                         detection ? 960 : 48, detection ? 960 : 1024);
            if (!profile->setDimensions(name, nvinfer1::OptProfileSelector::kMIN, minimum) ||
                !profile->setDimensions(name, nvinfer1::OptProfileSelector::kOPT, optimum) ||
                !profile->setDimensions(name, nvinfer1::OptProfileSelector::kMAX, maximum) ||
                config->addOptimizationProfile(profile) < 0)
                throw std::runtime_error("Unsupported TensorRT OCR input profile.");
            std::unique_ptr<nvinfer1::IHostMemory> plan(
                builder->buildSerializedNetwork(*network, *config));
            if (!plan)
                throw std::runtime_error("TensorRT OCR compilation failed: " + logger.Error());
            engine_.reset(runtime_->deserializeCudaEngine(plan->data(), plan->size()));
            if (!engine_)
                throw std::runtime_error("TensorRT OCR engine creation failed.");
            const auto blob = QByteArray::fromRawData(static_cast<const char *>(plan->data()),
                                                      static_cast<qsizetype>(plan->size()));
            QSaveFile saved(path);
            if (saved.open(QIODevice::WriteOnly))
            {
                saved.write(QCryptographicHash::hash(blob, QCryptographicHash::Sha256));
                saved.write(blob);
                saved.commit();
            }
        }
        if (engine_->getNbIOTensors() != 2)
            throw std::runtime_error("Unexpected cached OCR engine I/O.");
        for (int i = 0; i < 2; ++i)
        {
            const auto *name = engine_->getIOTensorName(i);
            if (engine_->getTensorDataType(name) != nvinfer1::DataType::kFLOAT)
                throw std::runtime_error("OCR engine must use float32 tensors.");
            if (engine_->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
                input_ = name;
            else
                output_ = name;
        }
        context_.reset(engine_->createExecutionContext());
        if (!context_)
            throw std::runtime_error("TensorRT execution context creation failed.");
    }
    nvinfer1::Dims Run(float *input, int width, int height, cudaStream_t stream,
                       TensorRtProfile &profile, int batch = 1)
    {
        QElapsedTimer host;
        if (profile.Enabled()) host.start();
        int shapeSets = 0, addressSets = 0;
        if (batch < 1 || batch > maximumBatch_)
            throw std::runtime_error("TensorRT batch exceeds its prepared profile.");
        if (batch != batch_ || width != width_ || height != height_)
        {
            if (!context_->setInputShape(input_.c_str(), nvinfer1::Dims4(batch, 3, height, width)))
                throw std::runtime_error("TensorRT shape rejected.");
            ++shapeSets;
            shape_ = context_->getTensorShape(output_.c_str());
            std::size_t count = 1;
            if (shape_.nbDims < 1 || shape_.nbDims > 4)
                throw std::runtime_error("Invalid TensorRT output rank.");
            for (int i = 0; i < shape_.nbDims; ++i)
            {
                if (shape_.d[i] <= 0 ||
                    count > maximumOutputFloats_ / static_cast<std::size_t>(shape_.d[i]))
                    throw std::runtime_error("Invalid TensorRT output extent.");
                count *= shape_.d[i];
            }
            outputBuffer_.Reserve(count);
            batch_ = batch; width_ = width; height_ = height;
        }
        auto *output = outputBuffer_.Get();
        if (input != boundInput_)
        {
            if (!context_->setTensorAddress(input_.c_str(), input))
                throw std::runtime_error("TensorRT OCR input binding failed.");
            boundInput_ = input;
            ++addressSets;
        }
        if (output != boundOutput_)
        {
            if (!context_->setTensorAddress(output_.c_str(), output))
                throw std::runtime_error("TensorRT OCR output binding failed.");
            boundOutput_ = output;
            ++addressSets;
        }
        if (!context_->enqueueV3(stream))
            throw std::runtime_error("TensorRT OCR enqueue failed.");
        profile.NetworkCall(detection_, batch, profile.Enabled() ? host.nsecsElapsed() / 1e6 : 0,
                            shapeSets, addressSets);
        return shape_;
    }
    const float *Output() const
    {
        return outputBuffer_.Get();
    }
    std::size_t OutputCapacityBytes() const { return outputBuffer_.Capacity() * sizeof(float); }

  private:
    std::unique_ptr<nvinfer1::IRuntime> runtime_;
    std::unique_ptr<nvinfer1::ICudaEngine> engine_;
    std::unique_ptr<nvinfer1::IExecutionContext> context_;
    DeviceBuffer<float> outputBuffer_;
    std::string input_, output_;
    bool detection_{};
    int maximumBatch_{1};
    std::size_t maximumOutputFloats_{};
    int batch_{}, width_{}, height_{};
    nvinfer1::Dims shape_{};
    float *boundInput_{}, *boundOutput_{};
};
struct State
{
    int device{};
    int recognizerBatch{1};
    QString batchPreparationDetail;
    TensorRtProfile profile;
    cudaStream_t stream{};
    Logger logger;
    std::unique_ptr<Libraries> libraries;
    std::unique_ptr<Network> detector, recognizer;
    DeviceBuffer<float> detectorInput, recognizerInput, scores;
    DeviceBuffer<int> labels, count, ids, tokenCount;
    DeviceBuffer<gpu::Component> components;
    DeviceBuffer<gpu::Quad> detectedQuads, quads;
    DeviceBuffer<gpu::Token> tokens;
    HostBuffer<int> hostCount;
    HostBuffer<gpu::Quad> hostQuad;
    HostBuffer<gpu::Token> hostTokens;
    DeviceBuffer<unsigned char> uploaded;
    QImage uploadPixels;
    AnalysisPool uploadPool;
    SharedDeviceImage inFlight;
    ~State()
    {
        if (stream)
            cudaStreamDestroy(stream);
    }
};
// The legacy profile queues batch-one calls in the same bounded group. The
// wider profile executes that group as one neural tensor batch.
constexpr int kRecognitionSubmissionGroup = 8;
std::shared_ptr<AsyncCleanup> RetireState(std::shared_ptr<State> state)
{
    if (state)
        return AsyncCleanup::Start([state] {
            return cudaSetDevice(state->device) == cudaSuccess &&
                   (!state->stream || cudaStreamSynchronize(state->stream) == cudaSuccess);
        });
    return {};
}
void ObserveRetirement(const TensorRtRetirementObserver& observer,
                       std::shared_ptr<AsyncCleanup> receipt) noexcept
{
    // Cleanup owns the resource closure before observation. Losing a receipt or
    // a throwing diagnostic must not change completion/failure quarantine.
    if (observer && receipt)
    {
        try { observer(std::move(receipt)); }
        catch (...) {}
    }
}
struct Preparation
{
    int device{};
    std::atomic<bool> cancel{};
    std::atomic<int> phase{}; // 0 preparing, 1 ready, 2 failed
    std::mutex mutex;
    std::shared_ptr<State> state;
    QString error;
    TensorRtRetirementObserver retirementObserver;
    ~Preparation()
    {
        cancel = true;
        ObserveRetirement(retirementObserver, RetireState(std::move(state)));
    }
};
class TensorRtRecognizer final : public Recognizer
{
  public:
    TensorRtRecognizer(QString root, QString sdk,ModelTier detector,ModelTier recognizer,
                       TensorRtRetirementObserver retirementObserver)
        : root_(std::move(root)), sdk_(std::move(sdk)),detectorTier_(detector),recognizerTier_(recognizer),
          retirementObserver_(std::move(retirementObserver))
    {
        // Wider profiles remain opt-in until their numerical agreement and
        // process-exit validation are accepted; preserve the cached default.
        const auto batch = qEnvironmentVariable("OKUFLOW_UCT_TRT_BATCH");
        invalidBatch_ = !batch.isEmpty() && batch != "1" && batch != "8" && batch != "16";
        recognizerBatch_ = batch == "16" ? 16 : batch == "8" ? 8 : 1;
    }
    ~TensorRtRecognizer() override
    {
        Retire();
    }
    BackendStatus Initialize() override
    {
        status_.name = "PaddleOCR / TensorRT CUDA";
        status_.languageTag = "multilingual";
        status_.state = BackendState::kUnavailable;
        if (invalidBatch_)
        {
            status_.detail = "OKUFLOW_UCT_TRT_BATCH must be 1, 8 or 16.";
            return status_;
        }
        assets_ = LoadPaddleAssets(root_,detectorTier_,recognizerTier_);
        if (!assets_.IsValid())
        {
            status_.detail = assets_.error;
            return status_;
        }
        if (!QFile::exists(QDir(sdk_).filePath("bin/nvinfer_11.dll")))
        {
            status_.detail = "TensorRT 11.4 runtime is not installed.";
            return status_;
        }
        int count = 0;
        if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0)
        {
            status_.detail = "No CUDA device is available.";
            return status_;
        }
        cudaDeviceProp properties{};
        if (cudaGetDeviceProperties(&properties, 0) != cudaSuccess ||
            properties.major * 10 + properties.minor < 75)
        {
            status_.detail = "TensorRT requires Turing or newer; CPU OCR remains available.";
            return status_;
        }
        BeginPreparation(0);
        status_.state = BackendState::kReady;
        status_.usesGpu = true;
        status_.detail = "Preparing GPU text engines; CPU OCR is used until ready.";
        return status_;
    }
    BackendStatus Status() const override
    {
        auto status = status_;
        if (preparation_ && preparation_->phase.load(std::memory_order_acquire) == 2)
        {
            status.state = BackendState::kUnavailable;
            std::lock_guard lock(preparation_->mutex);
            status.detail = preparation_->error;
        }
        return status;
    }
    bool Preparing() const override
    {
        return preparation_ && preparation_->phase.load(std::memory_order_acquire) == 0;
    }
    bool WantsDeviceInput() const override
    {
        return true;
    }
    RecognitionOutput Recognize(const QImage &image, const std::atomic<bool> &cancelled) override
    {
        // System-memory camera input needs one upload, on this worker only.
        RecognitionOutput output;
        bool uploadQueued = false, handedOff = false;
        std::shared_ptr<State> retained;
        try
        {
            if (!Ensure(0))
            {
                output.backendPreparing = true;
                return output;
            }
            retained = state_;
            output.inputFault = true;
            state_->uploadPixels = image.convertToFormat(QImage::Format_ARGB32);
            const auto &pixels = state_->uploadPixels;
            if (pixels.isNull())
                throw std::runtime_error("OCR image is empty.");
            auto *ptr = state_->uploaded.Reserve(static_cast<std::size_t>(pixels.width()) *
                                                 pixels.height() * 4);
            Check(cudaMemcpy2DAsync(ptr, pixels.width() * 4, pixels.constBits(),
                                    pixels.bytesPerLine(), pixels.width() * 4, pixels.height(),
                                    cudaMemcpyHostToDevice, state_->stream));
            uploadQueued = true;
            auto frame = state_->uploadPool.Capture(ptr, pixels.width() * 4, pixels.width(),
                                                    pixels.height(), state_->stream, 0, 0, 0);
            if (!frame)
                throw std::runtime_error("OCR upload allocation unavailable.");
            handedOff = true;
            output = RecognizeDevice(*frame, cancelled);
            Check(cudaStreamSynchronize(retained->stream));
        }
        catch (const std::exception &error)
        {
            output.ok = false;
            output.error = QString::fromUtf8(error.what());
            if (!handedOff && retained &&
                (!uploadQueued || cudaStreamSynchronize(retained->stream) == cudaSuccess))
                output.inputFault = true; // resource/input fault; engines remain usable
            else
            {
                output.inputFault = false;
                Retire();
            }
        }
        return output;
    }
    RecognitionOutput RecognizeDevice(const DeviceImage &image,
                                      const std::atomic<bool> &cancelled) override
    {
        RecognitionOutput output;
        try
        {
            if (cancelled.load())
            {
                output.cancelled = true;
                return output;
            }
            if (!Ensure(image.device))
            {
                output.backendPreparing = true;
                return output;
            }
            QString error;
            output.inputFault = true;
            state_->inFlight = image.weak_from_this().lock();
            if (!state_->inFlight)
            {
                output.inputFault = true;
                throw std::runtime_error("GPU OCR requires an owned analysis image.");
            }
            if (!image.WaitReady(state_->stream, error))
            {
                output.inputFault = true;
                throw std::runtime_error(error.toStdString());
            }
            output.inputFault = false;
            auto &profile = state_->profile;
            profile.Reset();
            const int submissionGroup = state_->recognizerBatch == 16 ? 16 : kRecognitionSubmissionGroup;
            for (const QRect& region : PaddleDetectionRegions({image.width,image.height})) {
            if (cancelled.load()) {
                output.cancelled=true;output.lines.clear();state_->inFlight.reset();return output;
            }
            // A strided subview of the owned image, never a host pixel copy.
            const auto* cropPixels=static_cast<const unsigned char*>(image.Data())+
                std::size_t(region.y())*image.pitch+std::size_t(region.x())*4;
            const QSize size = PaddleDetectionSize(region.size());
            auto *input = state_->detectorInput.Reserve(static_cast<std::size_t>(size.width()) *
                                                        size.height() * 3);
            profile.Measure(TensorRtProfile::Normalize, state_->stream, [&] {
                gpu::Normalize(cropPixels, image.pitch, region.width(), region.height(), input,
                               size.width(), size.height(), state_->stream);
            });
            nvinfer1::Dims shape{};
            profile.Measure(TensorRtProfile::Detector, state_->stream, [&] {
                shape = state_->detector->Run(input, size.width(), size.height(), state_->stream,
                                              profile);
            });
            if (shape.nbDims != 4 || shape.d[0] != 1 || shape.d[1] != 1)
                throw std::runtime_error("Unexpected GPU detector output.");
            const int w = static_cast<int>(shape.d[3]), h = static_cast<int>(shape.d[2]);
            auto *hostCount = state_->hostCount.Reserve(submissionGroup);
            profile.Measure(TensorRtProfile::Regions, state_->stream, [&] {
                gpu::Detect(state_->detector->Output(), w, h, region.width(), region.height(),
                            state_->labels.Reserve(w * h), state_->components.Reserve(w * h),
                            state_->detectedQuads.Reserve(128), state_->count.Reserve(1), state_->stream);
                gpu::RecognitionWindows(state_->detectedQuads.Get(),state_->count.Get(),
                                         state_->quads.Reserve(128*9),state_->stream);
                Check(cudaGetLastError());
            });
            profile.Measure(TensorRtProfile::Metadata, state_->stream, [&] {
                Check(cudaMemcpyAsync(hostCount, state_->count.Get(), sizeof(int),
                                      cudaMemcpyDeviceToHost, state_->stream));
            });
            profile.Transfer(sizeof(int));
            Check(cudaStreamSynchronize(state_->stream)); // worker-only tiny control readback
            const int count = *hostCount;
            if (count < 0 || count > 128*9)
                throw std::runtime_error("GPU detector region count is invalid.");
            for (int first = 0; first < count; first += submissionGroup)
            {
                if (cancelled.load())
                {
                    output.cancelled = true;
                    output.lines.clear();
                    state_->inFlight.reset();
                    return output;
                }
                constexpr int width = 1024;
                constexpr std::size_t samplePixels = width * 48 * 3;
                const int groupSize = std::min(submissionGroup, count - first);
                auto *crop = state_->recognizerInput.Reserve(
                    samplePixels * state_->recognizerBatch);
                auto *hostQuads = state_->hostQuad.Reserve(submissionGroup);
                gpu::Token *hostTokens{};
                int steps{};
                for (int batchFirst = 0; batchFirst < groupSize; batchFirst += state_->recognizerBatch)
                {
                    const int batchSize = std::min(state_->recognizerBatch, groupSize - batchFirst);
                    profile.Measure(TensorRtProfile::Rectify, state_->stream, [&] {
                        for (int sample = 0; sample < batchSize; ++sample)
                            gpu::Rectify(cropPixels, image.pitch, region.width(), region.height(),
                                         state_->quads.Get(), first + batchFirst + sample,
                                         crop + samplePixels * sample, width, state_->stream);
                    });
                    nvinfer1::Dims recShape{};
                    profile.Measure(TensorRtProfile::Recognizer, state_->stream, [&] {
                        recShape = state_->recognizer->Run(crop, width, 48, state_->stream,
                                                          profile, batchSize);
                    });
                    if (recShape.nbDims != 3 || recShape.d[0] != batchSize ||
                        recShape.d[2] != assets_.dictionary.size() || recShape.d[1] <= 0)
                        throw std::runtime_error("GPU recognizer dictionary/output mismatch.");
                    if (steps && steps != recShape.d[1])
                        throw std::runtime_error("GPU recognizer step count changed within a group.");
                    steps = static_cast<int>(recShape.d[1]);
                    if (!hostTokens)
                        hostTokens = state_->hostTokens.Reserve(
                            static_cast<std::size_t>(steps) * submissionGroup);
                    const std::size_t sampleOutputs = static_cast<std::size_t>(steps) * recShape.d[2];
                    const std::size_t outputLimit = (state_->recognizerBatch == 16 ? 256ull : 64ull) * 1024 * 1024;
                    if (sampleOutputs > outputLimit / batchSize)
                        throw std::runtime_error("GPU recognizer batch output exceeds capacity.");
                    profile.Tensor(batchSize, state_->recognizerInput.Capacity() * sizeof(float),
                                   state_->recognizer->OutputCapacityBytes(), outputLimit * sizeof(float));
                    auto *ids = state_->ids.Reserve(static_cast<std::size_t>(steps) * state_->recognizerBatch);
                    auto *scores = state_->scores.Reserve(static_cast<std::size_t>(steps) * state_->recognizerBatch);
                    auto *tokens = state_->tokens.Reserve(static_cast<std::size_t>(steps) * state_->recognizerBatch);
                    auto *tokenCounts = state_->tokenCount.Reserve(state_->recognizerBatch);
                    profile.Measure(TensorRtProfile::Ctc, state_->stream, [&] {
                        for (int sample = 0; sample < batchSize; ++sample) {
                            const int index = first + batchFirst + sample;
                            gpu::Decode(state_->recognizer->Output() + sampleOutputs * sample,
                                        steps, assets_.dictionary.size(), state_->quads.Get(), index,
                                        width, ids + static_cast<std::size_t>(steps) * sample,
                                        scores + static_cast<std::size_t>(steps) * sample,
                                        tokens + static_cast<std::size_t>(steps) * sample,
                                        tokenCounts + sample, state_->stream);
                            Check(cudaGetLastError());
                        }
                    });
                    profile.Measure(TensorRtProfile::Metadata, state_->stream, [&] {
                        for (int sample = 0; sample < batchSize; ++sample) {
                            const int slot = batchFirst + sample, index = first + slot;
                            Check(cudaMemcpyAsync(hostCount + slot, tokenCounts + sample, sizeof(int),
                                                  cudaMemcpyDeviceToHost, state_->stream));
                            Check(cudaMemcpyAsync(hostQuads + slot, state_->quads.Get() + index,
                                                  sizeof(gpu::Quad), cudaMemcpyDeviceToHost, state_->stream));
                            Check(cudaMemcpyAsync(hostTokens + static_cast<std::size_t>(slot) * steps,
                                                  tokens + static_cast<std::size_t>(sample) * steps,
                                                  steps * sizeof(gpu::Token),
                                                  cudaMemcpyDeviceToHost, state_->stream));
                        }
                    });
                    profile.Transfer(static_cast<std::size_t>(batchSize) *
                        (sizeof(int) + sizeof(gpu::Quad) + steps * sizeof(gpu::Token)));
                }
                Check(cudaStreamSynchronize(
                    state_->stream)); // all group reads and compact metadata copies complete
                if (cancelled.load())
                {
                    output.cancelled = true;
                    output.lines.clear();
                    state_->inFlight.reset();
                    return output;
                }
                QElapsedTimer merge;
                if (profile.Enabled()) merge.start();
                for (int slot = 0; slot < groupSize; ++slot)
                {
                    const int tokenCount = hostCount[slot];
                    const auto quad = hostQuads[slot];
                    const auto *tokens = hostTokens + static_cast<std::size_t>(slot) * steps;
                    if (tokenCount < 0)
                        continue; // untrustworthy crop, same policy as CPU decoder
                    if (tokenCount > steps)
                        throw std::runtime_error("GPU CTC output exceeds capacity.");
                    PaddleQuad box{};
                    box.score = quad.score;
                    box.internalEdges = quad.internalEdges;
                    for (int k = 0; k < 4; ++k)
                        box.points[k] = {quad.xy[k * 2], quad.xy[k * 2 + 1]};
                    std::vector<PaddleToken> decoded;
                    decoded.reserve(tokenCount);
                    for (int k = 0; k < tokenCount; ++k)
                        decoded.push_back(
                            {tokens[k].id, tokens[k].first, tokens[k].last, tokens[k].confidence});
                    const int validSteps = std::min(
                        steps, static_cast<int>(std::ceil(steps * std::min(width, quad.validWidth) /
                                                          static_cast<double>(width))));
                    auto line = PaddleDecodeTokens(decoded, validSteps, box, assets_.dictionary);
                    PaddleMergeRecognition(output.lines, std::move(line), region,
                                           {image.width, image.height});
                }
                profile.Merge(profile.Enabled() ? merge.nsecsElapsed() / 1e6 : 0);
            }
            }
            std::sort(output.lines.begin(), output.lines.end(), [](const auto &a, const auto &b) {
                return a.words[0].boundsPx.top() < b.words[0].boundsPx.top();
            });
            output.ok = !cancelled.load();
            output.cancelled = cancelled.load();
            profile.Emit(state_->recognizerBatch);
            state_->inFlight.reset(); // final metadata copy proved all reads complete
            status_.detail = "PP-OCRv6 GPU preprocessing, detection, rectification, recognition "
                             "and CTC; CPU metadata publication; recognizer batch=" +
                             QString::number(state_->recognizerBatch) + ". " +
                             state_->batchPreparationDetail;
        }
        catch (const std::exception &error)
        {
            // Completion instrumentation/status publication can fail after
            // inference set ok. Never return those candidates as successful.
            output.ok = false;
            output.lines.clear();
            output.error = QString::fromUtf8(error.what());
            status_.detail = output.error;
            // Ownership/event admission failed before this input enqueued work.
            // Keep the healthy engines; a bad frame must not rebuild models.
            if (output.inputFault && state_)
                state_->inFlight.reset();
            else
                Retire();
        }
        return output;
    }

  private:
    void BeginPreparation(int device)
    {
        Retire();
        preparation_ = std::make_shared<Preparation>();
        preparation_->device = device;
        preparation_->retirementObserver = retirementObserver_;
        status_.detail = "Preparing GPU text engines; CPU OCR is used until ready.";
        const auto preparation = preparation_;
        const auto assets = assets_;
        const auto sdk = sdk_;
        const int recognizerBatch = recognizerBatch_;
        try
        {
            std::thread([preparation, assets, sdk, recognizerBatch] {
                try
                {
                    Check(cudaSetDevice(preparation->device));
                    cudaDeviceProp properties{};
                    Check(cudaGetDeviceProperties(&properties, preparation->device));
                    if (properties.major * 10 + properties.minor < 75)
                        throw std::runtime_error("TensorRT requires Turing or newer.");
                    auto state = std::make_shared<State>();
                    state->device = preparation->device;
                    {
                        std::lock_guard lock(preparation->mutex);
                        preparation->state = state;
                    }
                    int low{}, high{};
                    Check(cudaDeviceGetStreamPriorityRange(&low, &high));
                    Check(cudaStreamCreateWithPriority(&state->stream, cudaStreamNonBlocking, low));
                    state->libraries = std::make_unique<Libraries>(sdk);
                    state->detector = std::make_unique<Network>(
                        *state->libraries, state->logger, sdk, assets.detector, assets.detectorSha256, true, state->device,
                        state->stream, preparation->cancel);
                    if (preparation->cancel.load())
                        throw std::runtime_error("GPU preparation cancelled.");
                    int capacity = recognizerBatch;
                    for (;;)
                    {
                        try
                        {
                            state->recognizer = std::make_unique<Network>(
                                *state->libraries, state->logger, sdk, assets.recognizer,
                                assets.recognizerSha256, false, state->device,
                                state->stream, preparation->cancel, capacity);
                            state->recognizerBatch = capacity;
                            break;
                        }
                        catch (const std::exception &error)
                        {
                            if (capacity == 1 || preparation->cancel.load()) throw;
                            // Preserve a supported smaller GPU profile. Unknown
                            // completion still quarantines the whole state.
                            Check(cudaStreamSynchronize(state->stream));
                            if (preparation->cancel.load())
                                throw std::runtime_error("GPU preparation cancelled.");
                            state->batchPreparationDetail += QString("Batch-%1 preparation failed: ").arg(capacity) +
                                                             QString::fromUtf8(error.what()) + "; ";
                            capacity = capacity == 16 ? 8 : 1;
                        }
                    }
                    if (preparation->cancel.load())
                        throw std::runtime_error("GPU preparation cancelled.");
                    preparation->phase.store(1, std::memory_order_release);
                }
                catch (const std::exception &error)
                {
                    {
                        std::lock_guard lock(preparation->mutex);
                        preparation->error = QString::fromUtf8(error.what());
                    }
                    preparation->phase.store(2, std::memory_order_release);
                }
            }).detach();
        }
        catch (const std::exception &error)
        {
            preparation_->error = QString::fromUtf8(error.what());
            preparation_->phase = 2;
        }
    }
    bool Ensure(int device)
    {
        if (state_ && state_->device == device)
        {
            Check(cudaSetDevice(device));
            return true;
        }
        if (!preparation_ || preparation_->device != device)
            BeginPreparation(device);
        const int phase = preparation_->phase.load(std::memory_order_acquire);
        if (phase == 0)
            return false;
        if (phase == 2)
        {
            std::lock_guard lock(preparation_->mutex);
            throw std::runtime_error(preparation_->error.toStdString());
        }
        {
            std::lock_guard lock(preparation_->mutex);
            state_ = std::move(preparation_->state);
        }
        preparation_.reset();
        Check(cudaSetDevice(device));
        return true;
    }
    void Retire()
    {
        if (preparation_)
        {
            preparation_->cancel = true;
            preparation_.reset();
        }
        ObserveRetirement(retirementObserver_, RetireState(std::move(state_)));
    }
    QString root_, sdk_;
    ModelTier detectorTier_,recognizerTier_;
    TensorRtRetirementObserver retirementObserver_;
    int recognizerBatch_{1};
    bool invalidBatch_{};
    PaddleAssets assets_;
    BackendStatus status_;
    std::shared_ptr<State> state_;
    std::shared_ptr<Preparation> preparation_;
};
} // namespace
std::unique_ptr<Recognizer> CreateTensorRtRecognizer(const QString &root, const QString &sdk,ModelTier detector,ModelTier recognizer,
                                                   TensorRtRetirementObserver retirementObserver)
{
    QString selected = sdk.isEmpty() ? qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT") : sdk;
    if (selected.isEmpty())
        selected =
            QDir(qEnvironmentVariable("LOCALAPPDATA"))
                .filePath("OkuFlow/ocr-deps/TensorRT-11.4.0.106-cuda12.9/TensorRT-11.4.0.106");
    return std::make_unique<TensorRtRecognizer>(root.isEmpty() ? DefaultPaddleRoot() : root,
                                                selected,detector,recognizer,std::move(retirementObserver));
}
} // namespace okuflow::uct
#else
namespace okuflow::uct
{
std::unique_ptr<Recognizer> CreateTensorRtRecognizer(const QString &, const QString &,ModelTier,ModelTier,
                                                   TensorRtRetirementObserver)
{
    return {};
}
} // namespace okuflow::uct
#endif
