#include "okuflow/common/uct_paddle.hpp"
#include "uct_runtime_precision.hpp"
#include "uct_public_bf16_runtime.hpp"
#include "uct_precision_experiment.hpp"
#include "uct_bf16_detector.hpp"
#include "uct_runtime_paths.hpp"
#include "uct_engine_cache.hpp"
#include "uct_native_precision.hpp"
#include "uct_formula_internal.hpp"
#include <QCoreApplication>
#include <new>
#include <utility>

namespace okuflow::uct
{
bool TensorRtCropDiagnostic::TryCommit(Plan&& plan) noexcept
{
    if (plans_.size() >= kMaxSources) return false;
    try {
        plans_.push_back(std::move(plan));
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
} // namespace okuflow::uct

#if OKUFLOW_UCT_TRT
#include "okuflow/common/async_cleanup.hpp"
#include "okuflow/cuda/uct_analysis_pool.hpp"
#include "okuflow/cuda/uct_kernels.hpp"
#include "uct_tensorrt_profile.hpp"
#include "uct_frozen_upload.hpp"
#include "uct_recognition_groups.hpp"
#include "uct_recognizer_width.hpp"
#include "uct_tensorrt_identity.hpp"
#include "uct_tensorrt_failures.hpp"
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QTemporaryFile>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cuda_runtime.h>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace okuflow::uct
{
struct TensorRtCropDiagnosticAccess {
    static const TensorRtCropDiagnostic::Plan* Find(TensorRtCropDiagnostic& diagnostic,
                                                    const DeviceImage& image, const QByteArray& detector, bool curvedRibbon) {
        for (const auto& plan : diagnostic.plans_) {
            const auto& held = *plan.source;
            if (held.Data() == image.Data() && held.ValidationData() == image.ValidationData() &&
                held.width == image.width && held.height == image.height && held.pitch == image.pitch &&
                held.device == image.device && held.sourceEpoch == image.sourceEpoch &&
                held.frameSequence == image.frameSequence && held.processingGeneration == image.processingGeneration &&
                held.kind == image.kind && held.historyEpoch == image.historyEpoch &&
                plan.detectorSha256 == detector && plan.curvedRibbon == curvedRibbon) return &plan;
        }
        return nullptr;
    }
    static bool HasCapacity(const TensorRtCropDiagnostic& diagnostic) {
        return diagnostic.plans_.size() < TensorRtCropDiagnostic::kMaxSources;
    }
    static bool TryCommit(TensorRtCropDiagnostic& diagnostic, TensorRtCropDiagnostic::Plan&& plan) noexcept {
        return diagnostic.TryCommit(std::move(plan));
    }
    static void Replayed(TensorRtCropDiagnostic& diagnostic) { ++diagnostic.replayCalls_; }
};
namespace
{
using detail::CudaFailure;
using detail::TensorRtApiFailure;
void Check(cudaError_t status)
{
    if (status != cudaSuccess)
        throw CudaFailure(cudaGetErrorString(status));
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
    QByteArray tensorRtVersion;
    DllDirectory sdkDirectory, cudaDirectory;
    using Factory = void *(*)(void *, int);
    using ParserFactory = void *(*)(void *, void *, int);
    Factory builder{}, runtime{};
    ParserFactory createParser{};
    explicit Libraries(const QString &sdk)
    {
        const auto bin = QDir(sdk).filePath("bin").toStdWString();
        sdkDirectory.value = AddDllDirectory(bin.c_str());
        const auto cuda=runtime_paths::CudaBin(qEnvironmentVariable("OKUFLOW_UCT_CUDA_BIN"),sdk,
            qEnvironmentVariable("CUDA_PATH_V12_9"),qEnvironmentVariable("LOCALAPPDATA"));
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
        tensorrt_identity::Version actual{};
        for (int i = 0; i < 4; ++i)
        {
            const auto version =
                reinterpret_cast<Version>(GetProcAddress(inference, versionNames[i]));
            if (!version)
                throw std::runtime_error("TensorRT runtime version API is unavailable.");
            actual[i] = version();
        }
        tensorRtVersion = tensorrt_identity::VersionString(actual);
        if (actual[0] != 11 || !tensorrt_identity::RuntimeAtLeastMinimum(actual))
            throw std::runtime_error("TensorRT ABI 11 runtime at least 11.4.0.106 is required; found " +
                                     tensorRtVersion.toStdString());
        runtime =
            reinterpret_cast<Factory>(GetProcAddress(inference, "createInferRuntime_INTERNAL"));
        if (!runtime)
            throw TensorRtApiFailure("TensorRT runtime factory API is unavailable.");
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
            throw TensorRtApiFailure("TensorRT builder/parser factory API is unavailable.");
    }
    ~Libraries()
    {
        // TensorRT may retain internal worker/tactic resources. Keep these
        // exact modules loaded for process lifetime; do not unload late work.
    }
    QJsonObject Provenance() const
    {
        std::wstring modulePath(32768, L'\0');
        const auto length = GetModuleFileNameW(inference, modulePath.data(), DWORD(modulePath.size()));
        const auto path = length && length < modulePath.size()
            ? QString::fromWCharArray(modulePath.data(), int(length)) : QString();
        QFile file(path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const bool hashed = !path.isEmpty() && file.open(QIODevice::ReadOnly) && hash.addData(&file);
        return {{"tensorRtVersion", QString::fromLatin1(tensorRtVersion)},
                {"loadedInferencePath", path}, {"inferenceSha256", hashed ? QString::fromLatin1(hash.result().toHex()) : QString()},
                {"hashed", hashed}, {"scope", "actual loaded DLL provenance; no approval/hash equality gate"}};
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
    class PublicProgress final : public nvinfer1::IProgressMonitor {
    public:
        explicit PublicProgress(std::function<bool()> stop):stop_(std::move(stop)){}
        void phaseStart(const char*,const char*,int32_t) noexcept override {}
        void phaseFinish(const char*) noexcept override {}
        bool stepComplete(const char*,int32_t) noexcept override {
            try{return !stop_();}catch(...){return false;}
        }
    private:
        std::function<bool()> stop_;
    };
    struct PublicResources {
        QByteArray model; // parser/build may retain this exact bounded snapshot
        PublicProgress progress;
        native_precision::Weights nativeWeights;
        std::unique_ptr<nvinfer1::IBuilder> builder;
        std::unique_ptr<nvinfer1::INetworkDefinition> network;
        std::unique_ptr<nvonnxparser::IParser> parser;
        std::unique_ptr<nvinfer1::IBuilderConfig> config;
        std::unique_ptr<nvinfer1::IHostMemory> serialized;
        explicit PublicResources(QByteArray bytes,std::function<bool()> stop)
            :model(std::move(bytes)),progress(std::move(stop)){}
    };
  public:
    struct PublicTag {};
    // Allocate/attach this object to the retained State BEFORE any factory or
    // builder call. A throwing preparation must not unwind live GPU resources.
    explicit Network(PublicTag):maximumBatch_(8),maximumOutputFloats_(64ull*1024*1024){}
    void PreparePublic(Libraries& libraries,Logger& logger,const QString& sdk,QByteArray model,
                       const public_bf16::CachedPlan& cached,cudaStream_t stream,
                       const std::function<bool()>& stop,int classes) {
        using namespace public_bf16;
        publicResources_=std::make_unique<PublicResources>(std::move(model),stop);
        auto& resources=*publicResources_;
        runtime_.reset(static_cast<nvinfer1::IRuntime*>(libraries.runtime(&logger,NV_TENSORRT_VERSION)));
        Check(cudaPeekAtLastError());
        detail::RequireTensorRtApi(runtime_.get(),"public BF16 runtime creation failed");
        if(stop())throw Refusal(Reason::BuildRefused,"public BF16 preparation deadline before build");
        if(cached.bytes.isEmpty()) {
            try {libraries.PrepareBuilder(sdk);}
            catch(const CudaFailure&){throw;}
            catch(const TensorRtApiFailure&){throw;}
            catch(const std::bad_alloc&){throw;}
            catch(const std::runtime_error& error) {
                Check(cudaPeekAtLastError());
                throw Refusal(Reason::BuildRefused,error.what());
            }
            resources.builder.reset(static_cast<nvinfer1::IBuilder*>(libraries.builder(&logger,NV_TENSORRT_VERSION)));
            Check(cudaPeekAtLastError());
            detail::RequireTensorRtApi(resources.builder.get(),"public BF16 builder creation failed");
            resources.network.reset(resources.builder->createNetworkV2(0));
            detail::RequireTensorRtApi(resources.network.get(),"public BF16 network creation failed");
            resources.parser.reset(static_cast<nvonnxparser::IParser*>(libraries.createParser(
                resources.network.get(),&logger,NV_ONNX_PARSER_VERSION)));
            detail::RequireTensorRtApi(resources.parser.get(),"public BF16 parser creation failed");
            if(!resources.parser->parse(resources.model.constData(),resources.model.size())) {
                Check(cudaPeekAtLastError());
                throw Refusal(Reason::BuildRefused,"public BF16 graph refused by parser");
            }
            if(resources.network->getNbInputs()!=1 || resources.network->getNbOutputs()!=1)
                throw Refusal(Reason::Model,"public BF16 graph I/O mismatch");
            resources.config.reset(resources.builder->createBuilderConfig());
            detail::RequireTensorRtApi(resources.config.get(),"public BF16 builder configuration failed");
            resources.config->setProgressMonitor(&resources.progress);
            resources.config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE,256ull*1024*1024);
            resources.config->setMaxAuxStreams(0);
            resources.config->setBuilderOptimizationLevel(2);
            resources.config->setProfileStream(stream);
            auto* profile=resources.builder->createOptimizationProfile();
            detail::RequireTensorRtApi(profile,"public BF16 profile creation failed");
            const auto* name=resources.network->getInput(0)->getName();
            if(!profile->setDimensions(name,nvinfer1::OptProfileSelector::kMIN,nvinfer1::Dims4(1,3,48,1024)) ||
               !profile->setDimensions(name,nvinfer1::OptProfileSelector::kOPT,nvinfer1::Dims4(8,3,48,1024)) ||
               !profile->setDimensions(name,nvinfer1::OptProfileSelector::kMAX,nvinfer1::Dims4(8,3,48,1024)) ||
               resources.config->addOptimizationProfile(profile)<0)
                throw Refusal(Reason::BuildRefused,"public BF16 profile refused");
            resources.serialized.reset(resources.builder->buildSerializedNetwork(*resources.network,*resources.config));
            Check(cudaPeekAtLastError());
            if(!resources.serialized)throw Refusal(Reason::BuildRefused,"public BF16 local build refused");
            if(resources.serialized->size()<1 || resources.serialized->size()>=std::size_t(runtime_precision::kPlanLimit))
                throw Refusal(Reason::BuildRefused,"public BF16 serialized plan exceeds bound");
            publicPlan_=QByteArray(static_cast<const char*>(resources.serialized->data()),qsizetype(resources.serialized->size()));
        } else publicPlan_=cached.bytes;
        if(stop())throw Refusal(Reason::BuildRefused,"public BF16 preparation deadline after build");
        engine_.reset(runtime_->deserializeCudaEngine(publicPlan_.constData(),publicPlan_.size()));
        Check(cudaPeekAtLastError());
        if(!engine_)throw Refusal(Reason::BuildRefused,"public BF16 plan refused by actual runtime");
        EngineContract actual;
        actual.profiles=engine_->getNbOptimizationProfiles();
        if(engine_->getNbIOTensors()!=2)throw Refusal(Reason::Model,"public BF16 engine I/O mismatch");
        for(int i=0;i<2;++i) {
            const auto* name=engine_->getIOTensorName(i);
            if(!name)throw TensorRtApiFailure("public BF16 tensor name unavailable");
            const bool linear=engine_->getTensorDataType(name)==nvinfer1::DataType::kFLOAT &&
                engine_->getTensorLocation(name)==nvinfer1::TensorLocation::kDEVICE &&
                engine_->getTensorFormat(name)==nvinfer1::TensorFormat::kLINEAR;
            if(engine_->getTensorIOMode(name)==nvinfer1::TensorIOMode::kINPUT) {
                ++actual.inputs;actual.input=name;actual.floatDeviceLinearInput=linear;input_=name;
            } else if(engine_->getTensorIOMode(name)==nvinfer1::TensorIOMode::kOUTPUT) {
                ++actual.outputs;actual.output=name;actual.floatDeviceLinearOutput=linear;output_=name;
            }
        }
        if(actual.inputs!=1 || actual.outputs!=1 || actual.profiles!=1)
            throw Refusal(Reason::Model,"public BF16 engine tensor/profile inventory mismatch");
        const auto shape=[&](nvinfer1::OptProfileSelector selector) {
            const auto dimensions=engine_->getProfileShape(input_.c_str(),0,selector);
            precision_artifacts::Shape result{};
            if(dimensions.nbDims==4)std::copy_n(dimensions.d,4,result.begin());
            return result;
        };
        actual.profile={shape(nvinfer1::OptProfileSelector::kMIN),shape(nvinfer1::OptProfileSelector::kOPT),shape(nvinfer1::OptProfileSelector::kMAX)};
        Check(cudaPeekAtLastError());CheckEngine(actual);
        const auto outputShape=engine_->getTensorShape(output_.c_str());
        const auto contextBytes=engine_->getDeviceMemorySizeV2();
        if(outputShape.nbDims!=3 || outputShape.d[2]!=classes || contextBytes<0 || contextBytes>256ll*1024*1024)
            throw Refusal(Reason::Model,"public BF16 output ABI/context memory exceeds contract");
        context_.reset(engine_->createExecutionContext());
        Check(cudaPeekAtLastError());
        detail::RequireTensorRtApi(context_.get(),"public BF16 execution context creation failed");
        planPayloadSha256_=precision_artifacts::Digest(publicPlan_);
        planKey_="public-bf16-locally-attested";
    }
    const QByteArray& PublicPlan() const {return publicPlan_;}
    void CheckPublicDetector() const {
        const auto bad=[] {throw public_bf16::Refusal(public_bf16::Reason::Model,"public BF16 requires the selected FLOAT detector ABI/profile");};
        if(!detection_ || engine_->getNbIOTensors()!=2 || engine_->getNbOptimizationProfiles()!=1 ||
           input_!="x" || output_!="fetch_name_0")bad();
        for(const auto* name:{input_.c_str(),output_.c_str()})
            if(engine_->getTensorDataType(name)!=nvinfer1::DataType::kFLOAT ||
               engine_->getTensorLocation(name)!=nvinfer1::TensorLocation::kDEVICE ||
               engine_->getTensorFormat(name)!=nvinfer1::TensorFormat::kLINEAR)bad();
        const auto shape=[&](nvinfer1::OptProfileSelector selector,const precision_artifacts::Shape& expected) {
            const auto actual=engine_->getProfileShape(input_.c_str(),0,selector);
            return actual.nbDims==4 && std::equal(expected.begin(),expected.end(),actual.d);
        };
        if(!shape(nvinfer1::OptProfileSelector::kMIN,{1,3,32,32}) ||
           !shape(nvinfer1::OptProfileSelector::kOPT,{1,3,544,960}) ||
           !shape(nvinfer1::OptProfileSelector::kMAX,{1,3,960,960}))bad();
        const auto output=engine_->getTensorShape(output_.c_str());
        if(output.nbDims!=4 || output.d[1]!=1)bad();
        Check(cudaPeekAtLastError());
    }
    // Caller attaches this empty Network to its retained State before entry.
    void PreparePinned(Libraries &libraries, Logger &logger, const QString &sdk, const QString &model, const QByteArray& expected, bool detection, int device,
            cudaStream_t stream, const std::function<bool()>& stop, int maximumBatch = 1,
            bool variableWidth = false, bool allowPreparation = true,
            const QByteArray* precisionPlan = nullptr,const QString& explicitCacheRoot = {},
            const QByteArray& verifiedPlanKey = "verified-local-bf16",bool bf16Graph=false,int expectedClasses=0,
            bool nativeBf16=false)
    {
        detection_=detection;variableWidth_=variableWidth;maximumBatch_=maximumBatch;
        maximumOutputFloats_=!detection && maximumBatch==16 ? 256ull*1024*1024 : 64ull*1024*1024;
        const auto artifact=[&](const std::string& reason) {
            Check(cudaPeekAtLastError());
            if(bf16Graph)throw runtime_precision::ArtifactError(reason);
            throw std::runtime_error(reason);
        };
        QFile modelFile(model);
        if (!modelFile.open(QIODevice::ReadOnly))
            artifact("Cannot read pinned OCR graph.");
        const auto bytes = modelFile.readAll();
        if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != expected)
            artifact("Pinned OCR graph changed before preparation.");
        publicResources_=std::make_unique<PublicResources>(bytes,stop);
        auto& resources=*publicResources_;
        runtime_.reset(static_cast<nvinfer1::IRuntime *>(libraries.runtime(&logger,NV_TENSORRT_VERSION)));
        Check(cudaPeekAtLastError());
        detail::RequireTensorRtApi(runtime_.get(),"TensorRT runtime creation failed.");
        cudaDeviceProp properties{};
        Check(cudaGetDeviceProperties(&properties, device));
        const auto sourceHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        const auto graphHash = nativeBf16 ? native_precision::GraphIdentity(sourceHash) : sourceHash;
        const auto uuid = QByteArray(reinterpret_cast<const char *>(&properties.uuid), sizeof(properties.uuid));
        // A new build binds its actual builder version. Loading an older key
        // never requires that builder version to equal the loaded runtime.
        const auto key = bf16Graph ? bf16_runtime::PlanKey(graphHash,uuid,detection,maximumBatch,variableWidth_,libraries.tensorRtVersion) :
            tensorrt_identity::Fp32PlanKey(graphHash, uuid, detection,maximumBatch, variableWidth_, libraries.tensorRtVersion);
        QList<QByteArray> readKeys{key};
        const auto originalBuilderKey = bf16Graph ? bf16_runtime::PlanKey(graphHash,uuid,detection,maximumBatch,variableWidth_) :
            tensorrt_identity::Fp32PlanKey(graphHash, uuid,detection, maximumBatch, variableWidth_);
        if (!readKeys.contains(originalBuilderKey)) readKeys.push_back(originalBuilderKey);
        if(!bf16Graph)for (const int historicalDriver : {13050, 13040})
            readKeys.push_back(tensorrt_identity::LegacyFp32PlanKey(graphHash, uuid,
                detection, maximumBatch, variableWidth_, historicalDriver));
        const auto cache = explicitCacheRoot.isEmpty()
            ? QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/ocr-engines") : explicitCacheRoot;
        if (allowPreparation && !precisionPlan) QDir().mkpath(cache);
        const auto path = QDir(cache).filePath(QString::fromLatin1(key) + ".plan");
        if (precisionPlan) {
            engine_.reset(runtime_->deserializeCudaEngine(precisionPlan->constData(), precisionPlan->size()));
            Check(cudaPeekAtLastError());
            if (!engine_) {
                // Pre-inference refusal without a CUDA fault permits the
                // existing cache-only FP32 selection. Context faults do not.
                throw runtime_precision::ArtifactError("Local BF16 plan refused by TensorRT " +
                    libraries.tensorRtVersion.toStdString() + ": " + logger.Error());
            }
            planKey_ = verifiedPlanKey;
            planPayloadSha256_ = QCryptographicHash::hash(*precisionPlan, QCryptographicHash::Sha256).toHex();
        }
        else for (const auto& readKey : readKeys) {
            QFile cached(QDir(cache).filePath(QString::fromLatin1(readKey) + ".plan"));
            if (!cached.open(QIODevice::ReadOnly) || !engine_cache::FileFits(cached.size())) continue;
            const auto digest = cached.read(32), plan = cached.readAll();
            if (cached.error() != QFileDevice::NoError || !cached.atEnd() ||
                QCryptographicHash::hash(plan, QCryptographicHash::Sha256) != digest) continue;
            engine_.reset(runtime_->deserializeCudaEngine(plan.constData(), plan.size()));
            // A reported CUDA fault is terminal, even if another locator exists.
            Check(cudaPeekAtLastError());
            if (engine_) {
                planKey_ = readKey;
                planPayloadSha256_ = digest.toHex();
                cacheStatus_ = "loaded";
                break;
            }
        }
        if (!engine_)
        {
            // Width experiments never trigger an unreviewed cold build during
            // normal inference. One-time preparation is a separate opt-in.
            if (!allowPreparation)
                artifact("OCR plan is missing/invalid; explicit width-profile preparation is required.");
            try {libraries.PrepareBuilder(sdk);}
            catch(const CudaFailure&) {throw;}
            catch(const TensorRtApiFailure&) {throw;}
            catch(const std::bad_alloc&) {throw;}
            catch(const std::runtime_error& error) {artifact(error.what());}
            auto& builder=resources.builder;
            builder.reset(static_cast<nvinfer1::IBuilder *>(libraries.builder(&logger,NV_TENSORRT_VERSION)));
            if (!builder)
                detail::RequireTensorRtApi(builder.get(), "TensorRT builder creation failed: " + logger.Error());
            auto& network=resources.network;network.reset(builder->createNetworkV2(0));
            detail::RequireTensorRtApi(network.get(), "TensorRT network creation failed.");
            auto& parser=resources.parser;parser.reset(static_cast<nvonnxparser::IParser *>(
                libraries.createParser(network.get(), &logger, NV_ONNX_PARSER_VERSION)));
            detail::RequireTensorRtApi(parser.get(), "TensorRT parser creation failed.");
            if (!parser->parse(bytes.constData(), bytes.size()))
                artifact("PaddleOCR graph is unsupported by TensorRT: " + logger.Error());
            if (nativeBf16) {
                if (!bf16Graph) artifact("Native BF16 requires the precision preparation path.");
                try { native_precision::Apply(*network, resources.nativeWeights, stop); }
                catch (const TensorRtApiFailure&) { throw; }
                catch (const std::runtime_error& error) { artifact(error.what()); }
            }
            if (network->getNbInputs() != 1 || network->getNbOutputs() != 1)
                artifact("Unexpected TensorRT OCR graph I/O.");
            auto& config=resources.config;config.reset(builder->createBuilderConfig());
            detail::RequireTensorRtApi(config.get(), "TensorRT builder configuration failed.");
            config->setProgressMonitor(&resources.progress);
            config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 256ull * 1024 * 1024);
            config->setMaxAuxStreams(0);
            config->setBuilderOptimizationLevel(2);
            config->setProfileStream(stream);
            auto *profile = builder->createOptimizationProfile();
            detail::RequireTensorRtApi(profile, "TensorRT shape profile creation failed.");
            const char *name = network->getInput(0)->getName();
            const auto declared = network->getInput(0)->getDimensions();
            if (variableWidth_ && (declared.nbDims != 4 || declared.d[3] != -1))
                throw std::runtime_error("Pinned OCR graph does not declare dynamic width.");
            const nvinfer1::Dims4 minimum(1, 3, detection ? 32 : 48, detection ? 32 : variableWidth_ ? recognizer_width::kMinimum : 1024);
            const nvinfer1::Dims4 optimum(detection ? 1 : maximumBatch, 3,
                                         detection ? 544 : 48, detection ? 960 : variableWidth_ ? recognizer_width::kOptimum : 1024);
            const nvinfer1::Dims4 maximum(detection ? 1 : maximumBatch, 3,
                                         detection ? 960 : 48, detection ? 960 : 1024);
            if (!profile->setDimensions(name, nvinfer1::OptProfileSelector::kMIN, minimum) ||
                !profile->setDimensions(name, nvinfer1::OptProfileSelector::kOPT, optimum) ||
                !profile->setDimensions(name, nvinfer1::OptProfileSelector::kMAX, maximum) ||
                config->addOptimizationProfile(profile) < 0)
                artifact("Unsupported TensorRT OCR input profile.");
            auto& plan=resources.serialized;plan.reset(builder->buildSerializedNetwork(*network,*config));
            if (!plan)
                artifact("TensorRT OCR compilation failed: " + logger.Error());
            if (!engine_cache::PayloadFits(plan->size()))
                artifact("TensorRT serialized plan exceeds the digest-prefixed cache bound.");
            engine_.reset(runtime_->deserializeCudaEngine(plan->data(), plan->size()));
            Check(cudaPeekAtLastError());
            if (!engine_) {
                if(bf16Graph)artifact("BF16 engine refused by TensorRT.");
                throw TensorRtApiFailure("TensorRT OCR engine creation failed.");
            }
            const auto blob = QByteArray::fromRawData(static_cast<const char *>(plan->data()),
                                                      static_cast<qsizetype>(plan->size()));
            planKey_ = key;
            planPayloadSha256_ = QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toHex();
        }
        const auto reject = [&](const char* reason) {
            // A reported CUDA fault is never permission to retry another engine.
            if (precisionPlan || bf16Graph) {
                Check(cudaPeekAtLastError());
                throw runtime_precision::ArtifactError(reason);
            }
            throw std::runtime_error(reason);
        };
        if (engine_->getNbIOTensors() != 2)
            reject("Unexpected cached OCR engine I/O.");
        for (int i = 0; i < 2; ++i)
        {
            const auto *name = engine_->getIOTensorName(i);
            if (engine_->getTensorDataType(name) != nvinfer1::DataType::kFLOAT)
                reject("OCR engine must use float32 tensors.");
            if ((precisionPlan || bf16Graph) && (engine_->getTensorLocation(name) != nvinfer1::TensorLocation::kDEVICE ||
                                  engine_->getTensorFormat(name) != nvinfer1::TensorFormat::kLINEAR))
                reject("Verified precision plans require DEVICE LINEAR I/O.");
            if (engine_->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
                input_ = name;
            else
                output_ = name;
        }
        if (precisionPlan) {
            if (detection || variableWidth || maximumBatch != 8 || input_.empty() || output_.empty() ||
                engine_->getNbOptimizationProfiles() != 1)
                reject("Local BF16 requires the exact fixed1024/N8 recognition profile.");
            const auto minimum = engine_->getProfileShape(input_.c_str(),0,nvinfer1::OptProfileSelector::kMIN);
            const auto optimum = engine_->getProfileShape(input_.c_str(),0,nvinfer1::OptProfileSelector::kOPT);
            const auto maximum = engine_->getProfileShape(input_.c_str(),0,nvinfer1::OptProfileSelector::kMAX);
            if (!recognizer_width::MatchesProfile(minimum.d,minimum.nbDims,1,1024) ||
                !recognizer_width::MatchesProfile(optimum.d,optimum.nbDims,8,1024) ||
                !recognizer_width::MatchesProfile(maximum.d,maximum.nbDims,8,1024))
                reject("Local BF16 cached plan does not match its fixed1024/N8 profile.");
        }
        if (variableWidth_) {
            if (engine_->getNbOptimizationProfiles() != 1)
                throw std::runtime_error("Variable-width OCR requires one exact optimization profile.");
            const auto minimum = engine_->getProfileShape(input_.c_str(),0,nvinfer1::OptProfileSelector::kMIN);
            const auto optimum = engine_->getProfileShape(input_.c_str(),0,nvinfer1::OptProfileSelector::kOPT);
            const auto maximum = engine_->getProfileShape(input_.c_str(),0,nvinfer1::OptProfileSelector::kMAX);
            if (!recognizer_width::MatchesProfile(minimum.d,minimum.nbDims,1,recognizer_width::kMinimum) ||
                !recognizer_width::MatchesProfile(optimum.d,optimum.nbDims,maximumBatch_,recognizer_width::kOptimum) ||
                !recognizer_width::MatchesProfile(maximum.d,maximum.nbDims,maximumBatch_,recognizer_width::kMaximum))
                throw std::runtime_error("Cached OCR plan does not match the variable-width profile.");
        }
        if(bf16Graph) {
            if(detection)CheckPublicDetector();
            else {
                if(input_!="x" || output_!="fetch_name_0" || engine_->getNbOptimizationProfiles()!=1)
                    artifact("BF16 recognizer tensor/profile inventory differs");
                const auto shape=[&](nvinfer1::OptProfileSelector selector) {
                    const auto actual=engine_->getProfileShape(input_.c_str(),0,selector);
                    precision_artifacts::Shape result{};
                    if(actual.nbDims==4)std::copy_n(actual.d,4,result.begin());
                    return result;
                };
                bf16_runtime::CheckRecognizerProfile({shape(nvinfer1::OptProfileSelector::kMIN),shape(nvinfer1::OptProfileSelector::kOPT),
                    shape(nvinfer1::OptProfileSelector::kMAX)},maximumBatch);
                const auto output=engine_->getTensorShape(output_.c_str());
                if(expectedClasses<2 || output.nbDims!=3 || output.d[2]!=expectedClasses)
                    artifact("BF16 recognizer dictionary/output ABI differs");
            }
        }
        contextBytes_ = engine_->getDeviceMemorySizeV2();
        if (bf16Graph && (contextBytes_ < 0 || (!detection && !engine_cache::ContextFits(contextBytes_))))
            reject("BF16 recognizer context exceeds its supported memory bound.");
        context_.reset(engine_->createExecutionContext());
        Check(cudaPeekAtLastError());
        if (!context_)
            throw TensorRtApiFailure("TensorRT execution context creation failed.");
        if (resources.serialized) {
            const auto& plan = resources.serialized;
            const auto blob = QByteArray::fromRawData(static_cast<const char*>(plan->data()), qsizetype(plan->size()));
            QString error;
            cacheStatus_ = engine_cache::WriteNew(path, blob, error) ? "saved" : "not-saved: " + error;
        }
    }
    // Called only after the State's stream is confirmed complete.
    void ReleaseBuildResources() {publicResources_.reset();publicPlan_.clear();}
    QString PlanReceipt(const QString& role) const
    {
        return role + "PlanKey=" + QString::fromLatin1(planKey_) + ";" +
               role + "PlanPayloadSha256=" + QString::fromLatin1(planPayloadSha256_) + ";" +
               role + "PlanCache=" + cacheStatus_ + ";" +
               role + "ContextBytes=" + QString::number(contextBytes_) + "; ";
    }
    nvinfer1::Dims Run(float *input, int width, int height, cudaStream_t stream,
                       TensorRtProfile &profile, int batch = 1)
    {
        QElapsedTimer host;
        if (profile.Enabled()) host.start();
        int shapeSets = 0, addressSets = 0;
        if (variableWidth_ && (!recognizer_width::IsCanvas(width) || height != 48))
            throw std::runtime_error("Recognizer width is outside the prepared bucket policy.");
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
    // Declared first, destroyed last, after contexts/engines/output storage.
    std::unique_ptr<PublicResources> publicResources_;
    QByteArray publicPlan_;
    QByteArray planKey_, planPayloadSha256_;
    QString cacheStatus_{"external"};
    std::int64_t contextBytes_{};
    std::unique_ptr<nvinfer1::IRuntime> runtime_;
    std::unique_ptr<nvinfer1::ICudaEngine> engine_;
    std::unique_ptr<nvinfer1::IExecutionContext> context_;
    DeviceBuffer<float> outputBuffer_;
    std::string input_, output_;
    bool detection_{}, variableWidth_{};
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
    bool variableWidth{};
    QString batchPreparationDetail;
    QString precisionDetail;
    bool detectorBf16{};
    bool recognizerBf16{};
    QString detectorFallback;
    QJsonArray detectorSmoke;
    std::array<QByteArray,2> ownedAttempts; // only keys this preparation inserted
    TensorRtProfile profile;
    cudaStream_t stream{};
    Logger logger;
    std::unique_ptr<Libraries> libraries;
    std::unique_ptr<Network> detector, recognizer;
    DeviceBuffer<float> detectorInput, recognizerInput, scores;
    DeviceBuffer<int> labels, count, ids, tokenCount, groupWidths, ribbonOffsets;
    DeviceBuffer<gpu::Component> components;
    DeviceBuffer<gpu::Quad> detectedQuads, quads;
    DeviceBuffer<gpu::Token> tokens;
    HostBuffer<int> hostCount, hostGroupWidths;
    HostBuffer<gpu::Quad> hostQuad;
    HostBuffer<unsigned char> goldenPixels;
    HostBuffer<float> detectorSmokeMap;
    public_bf16::Publication publicCompletion;
    QJsonObject publicGoldenRecord;
    QJsonObject publicEvidenceIdentity;
    std::vector<public_bf16::Observation> publicObservations;
    bool publicGoldenStarted{};
    // Two recognition-group metadata slots: one being merged on the host,
    // one possibly still written by the next group's queued copies.
    HostBuffer<int> groupCount[2];
    HostBuffer<gpu::Quad> groupQuad[2];
    HostBuffer<gpu::Token> groupTokens[2];
    cudaEvent_t groupEvents[2]{};
    cudaEvent_t GroupEvent(int slot)
    {
        if (!groupEvents[slot])
            Check(cudaEventCreateWithFlags(&groupEvents[slot], cudaEventDisableTiming));
        return groupEvents[slot];
    }
    DeviceBuffer<unsigned char> uploaded;
    QImage uploadPixels;
    AnalysisPool uploadPool;
    SharedDeviceImage inFlight;
    // Pre-existing empty slot: final retirement can self-quarantine without
    // allocation if asynchronous cleanup cannot accept the resource closure.
    std::shared_ptr<State> retirementQuarantine;
    ~State()
    {
        for (auto event : groupEvents)
            if (event)
                cudaEventDestroy(event);
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
    ~Preparation() noexcept
    {
        cancel = true;
        ObserveRetirement(retirementObserver, detail::RetireOwnedState(state, RetireState));
    }
};
// Worker-only bounded polling. A deadline/cancellation/fault keeps the complete
// State attached to Preparation and reaches its ordinary drain/quarantine.
void ConfirmPublicCompletion(State& state,const std::function<bool()>& stop)
{
    state.publicCompletion.Queued();
    Check(cudaEventRecord(state.GroupEvent(0),state.stream));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    for(;;) {
        const auto result=cudaEventQuery(state.groupEvents[0]);
        if(result==cudaSuccess)break;
        if(result!=cudaErrorNotReady)Check(result);
        if(stop() || std::chrono::steady_clock::now()>=deadline)
            throw std::runtime_error("public BF16 completion cancelled/unknown at deadline; retire/quarantine");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    state.publicCompletion.Confirmed();
}
void ReportPublicPartial(State& state,const public_bf16::AttestationObserver& observer,
                         const char* completion,const char* error="") noexcept
{
    if(!state.publicGoldenStarted)return;
    try {
        if(state.publicGoldenRecord.value("complete").toBool())return;
        state.publicGoldenRecord=public_bf16::PartialRecord(state.publicEvidenceIdentity,state.publicObservations,completion,QString::fromUtf8(error));
        if(observer) {try{observer(state.publicGoldenRecord);}catch(...) {}}
    }catch(...) {} // evidence allocation failure cannot release/fallback GPU ownership
}
void SmokeDetector(State& state,const std::function<bool()>& stop) {
    // Tensor-only compatibility check, not text-region/quality attestation.
    for(const auto size:{QSize(32,32),QSize(960,544),QSize(960,960),QSize(32,960)}) {
        public_bf16::RefuseStoppedCandidate(state.publicCompletion,stop(),"BF16 detector deadline before smoke submission");
        auto* input=state.detectorInput.Reserve(std::size_t(size.width())*size.height()*3);
        state.publicCompletion.Queued();
        Check(cudaMemsetAsync(input,0,std::size_t(size.width())*size.height()*3*sizeof(float),state.stream));
        const auto shape=state.detector->Run(input,size.width(),size.height(),state.stream,state.profile);
        if(shape.nbDims!=4 || shape.d[0]!=1 || shape.d[1]!=1 || shape.d[2]<1 || shape.d[2]>960 || shape.d[3]<1 || shape.d[3]>960)
            throw TensorRtApiFailure("BF16 detector actual output violates detector-map ABI");
        const auto count=std::size_t(shape.d[2])*std::size_t(shape.d[3]);
        auto* host=state.detectorSmokeMap.Reserve(count);
        Check(cudaMemcpyAsync(host,state.detector->Output(),count*sizeof(float),cudaMemcpyDeviceToHost,state.stream));
        ConfirmPublicCompletion(state,stop);
        bf16_detector::CheckMapSize(size.width(),size.height(),int(shape.d[3]),int(shape.d[2]));
        const auto summary=bf16_detector::CheckMap({host,count});
        state.detectorSmoke.push_back(QJsonObject{{"width",size.width()},{"height",size.height()},
            {"mapWidth",int(shape.d[3])},{"mapHeight",int(shape.d[2])},{"validProbabilities",true},
            {"maximumProbability",summary.maximum},{"meanProbability",summary.mean}});
    }
}
void RunPublicGolden(State& state,const public_bf16::Corpus& corpus,
    const QStringList& dictionary,const std::function<bool()>& stop,const public_bf16::AttestationObserver& observer,bool includeBatch8=true)
{
    using namespace public_bf16;
    constexpr int atlasWidth=1024,atlasHeight=8*48,maximumSteps=512;
    auto* pixels=state.goldenPixels.Reserve(atlasWidth*atlasHeight*4);
    std::fill_n(pixels,atlasWidth*atlasHeight*4,0);
    auto* quads=state.hostQuad.Reserve(8);
    for(int i=0;i<8;++i) {
        const auto& crop=corpus.Crops()[i];
        const float width=float(crop.width),top=float(i*48),bottom=top+48;
        quads[i]={{0,top,width,top,width,bottom,0,bottom},1.f,crop.width,0};
        for(int y=0;y<48;++y)for(int x=0;x<crop.width;++x) {
            auto* pixel=pixels+((i*48+y)*atlasWidth+x)*4;
            pixel[0]=pixel[1]=pixel[2]=static_cast<unsigned char>(crop.pixels[y*crop.width+x]);pixel[3]=255;
        }
    }
    // Allocate once, before queuing anything. All upload/copy destinations and
    // model/parser/output owners remain in State, including partial failures.
    auto* devicePixels=state.uploaded.Reserve(atlasWidth*atlasHeight*4);
    auto* deviceQuads=state.quads.Reserve(8);
    auto* input=state.recognizerInput.Reserve(8ull*3*48*1024);
    auto* ids=state.ids.Reserve(8*maximumSteps);
    auto* scores=state.scores.Reserve(8*maximumSteps);
    auto* tokens=state.tokens.Reserve(8*maximumSteps);
    auto* counts=state.tokenCount.Reserve(8);
    auto* hostTokens=state.groupTokens[0].Reserve(8*maximumSteps);
    auto* hostCounts=state.groupCount[0].Reserve(8);
    state.publicCompletion.Queued();
    Check(cudaMemcpyAsync(devicePixels,pixels,atlasWidth*atlasHeight*4,cudaMemcpyHostToDevice,state.stream));
    Check(cudaMemcpyAsync(deviceQuads,quads,8*sizeof(gpu::Quad),cudaMemcpyHostToDevice,state.stream));
    state.publicObservations.reserve(kGoldenObservations);
    const auto run=[&](int first,int batch) {
        RefuseStoppedCandidate(state.publicCompletion,stop(),"public BF16 golden deadline before submission");
        state.publicCompletion.Queued();
        // The actual unchanged runtime rectification/zero-padding and batched
        // argmax/compaction kernels, with eight distinct pinned input crops.
        gpu::RectifyBatch(devicePixels,atlasWidth*4,atlasWidth,atlasHeight,deviceQuads,first,batch,input,1024,state.stream);
        Check(cudaGetLastError());
        const auto shape=state.recognizer->Run(input,1024,48,state.stream,state.profile,batch);
        if(shape.nbDims!=3 || shape.d[0]!=batch || shape.d[1]<1 || shape.d[1]>maximumSteps ||
           shape.d[2]!=corpus.ModelIdentity()["classes"].toInt() || shape.d[2]!=dictionary.size())
            throw TensorRtApiFailure("public BF16 actual output tensor does not satisfy CTC ABI");
        const int steps=int(shape.d[1]),classes=int(shape.d[2]); // bounded above before conversion
        gpu::DecodeBatch(state.recognizer->Output(),batch,steps,classes,deviceQuads,first,1024,
            ids,scores,tokens,counts,state.stream);
        Check(cudaGetLastError());
        Check(cudaMemcpyAsync(hostCounts,counts,batch*sizeof(int),cudaMemcpyDeviceToHost,state.stream));
        Check(cudaMemcpyAsync(hostTokens,tokens,batch*steps*sizeof(gpu::Token),cudaMemcpyDeviceToHost,state.stream));
        ConfirmPublicCompletion(state,stop); // serial shape changes, no output resize while in use
        for(int i=0;i<batch;++i) {
            const auto& crop=corpus.Crops()[first+i];
            const int n=hostCounts[i];
            if(n>steps)throw TensorRtApiFailure("public BF16 compacted output exceeds actual tensor");
            QString text;
            if(n>=0) {
                std::vector<PaddleToken> decoded;decoded.reserve(n);
                for(int t=0;t<n;++t) {
                    const auto& token=hostTokens[i*steps+t];
                    decoded.push_back({token.id,token.first,token.last,token.confidence});
                }
                const int validSteps=std::min(steps,int(std::ceil(steps*std::min(1024,crop.width)/1024.0)));
                text=RawText(decoded,validSteps,dictionary);
            }
            state.publicObservations.push_back({crop.id,batch,steps,classes,crop.pixelsSha256,text});
        }
        ReportPublicPartial(state,observer,"confirmed-through-reported-rows");
    };
    for(int i=0;i<8;++i)run(i,1);
    if(includeBatch8)run(0,8); // production always requires real full N8
}
void RecordFp32Control(State& state,const PaddleAssets& assets,const public_bf16::Request& request,
    ModelTier tier,const std::function<bool()>& cancelled,const public_bf16::AttestationObserver& observer) {
    using namespace public_bf16;
    if(request.use!=CatalogUse::CandidateValidation || state.recognizerBf16 || state.detectorBf16)
        throw std::runtime_error("FP32 control is private and cannot run with BF16 engines");
    const auto corpus=LoadCorpus(request,tier);
    ConfirmPublicCompletion(state,cancelled);
    RunPublicGolden(state,corpus,assets.dictionary,cancelled,{},state.recognizerBatch>=8);
    QJsonArray rows;
    for(const auto& row:state.publicObservations) {
        const auto expected=std::find_if(corpus.Crops().begin(),corpus.Crops().end(),[&](const auto& crop){return crop.id==row.crop;});
        if(expected==corpus.Crops().end())throw std::runtime_error("FP32 control crop identity changed");
        rows.push_back(QJsonObject{{"crop",row.crop},{"batch",row.batch},{"T",row.steps},{"classes",row.classes},
            {"pixelsSha256",QString::fromLatin1(row.pixelsSha256)},{"text",row.text},{"expected",expected->expected},
            {"exactText",row.text==expected->expected}});
    }
    // Never call CheckGolden/ReceiptBytes/PublishNew for this record. It is a
    // measurement of the FP32 model's errors, not permission to accept BF16.
    if(observer)observer(QJsonObject{{"role","recognizer"},{"precision","FP32"},{"fp32Control",true},
        {"complete",true},{"passed",false},{"tier",ModelTierToken(tier)},
        {"sourceSha256",QString::fromLatin1(assets.recognizerSha256)},
        {"corpusSha256",QString::fromLatin1(corpus.Sha256())},{"rows",rows},
        {"acceptedPlan",state.recognizer->PlanReceipt("recognizer")}});
}
void PrepareRuntimeRecognizer(State& state,const PaddleAssets& assets,const QString& sdk,
    const public_bf16::Request& request,ModelTier detector,ModelTier tier,int capacity,const QByteArray& uuid,
    const std::function<bool()>& stop,const std::function<bool()>& cancelled,const public_bf16::AttestationObserver& observer) {
    using namespace public_bf16;
    const auto spec=Model(tier);
    const bool native = request.use == CatalogUse::Production;
    if (!native) ReadModel(request.modelRoot,assets,tier); // explicit historical graph diagnostic only
    const auto graphIdentity = native ? native_precision::GraphIdentity(QByteArray::fromHex(assets.recognizerSha256))
                                      : QByteArray::fromHex(spec.graphSha256);
    const auto key=bf16_runtime::PlanKey(graphIdentity,uuid,false,capacity,false,state.libraries->tensorRtVersion);
    const auto attempt="runtime-recognizer:"+key;
    RefuseRememberedFailure(request.cacheRoot,attempt);
    if(!SessionAttempts().Begin(attempt))throw Refusal(Reason::BuildRefused,"BF16 recognizer already failed/in flight this session");
    state.ownedAttempts[1]=attempt;
    state.recognizer=std::make_unique<Network>(Network::PublicTag{});
    state.publicCompletion.Queued();
    state.recognizer->PreparePinned(*state.libraries,state.logger,sdk,
        native ? assets.recognizer : QDir(request.modelRoot).filePath(GraphRelativePath(tier)),
        native ? assets.recognizerSha256 : QByteArray(spec.graphSha256),false,state.device,state.stream,
        stop,capacity,false,true,nullptr,request.cacheRoot,{},true,spec.classes,native);
    ConfirmPublicCompletion(state,cancelled);
    RefuseStoppedCandidate(state.publicCompletion,stop(),"BF16 recognizer build deadline");
    const auto elements=std::size_t(capacity)*3*48*1024;
    auto* input=state.recognizerInput.Reserve(elements);
    state.publicCompletion.Queued();
    Check(cudaMemsetAsync(input,0,elements*sizeof(float),state.stream));
    const auto shape=state.recognizer->Run(input,1024,48,state.stream,state.profile,capacity);
    if(shape.nbDims!=3 || shape.d[0]!=capacity || shape.d[1]<1 || shape.d[1]>512 || shape.d[2]!=assets.dictionary.size())
        throw TensorRtApiFailure("BF16 recognizer actual output violates CTC tensor ABI");
    ConfirmPublicCompletion(state,stop);
    RefuseStoppedCandidate(state.publicCompletion,stop(),"BF16 recognizer deadline after compatibility check");
    state.recognizerBf16=true;state.recognizerBatch=capacity;state.variableWidth=false;
    SessionAttempts().Passed(attempt);state.ownedAttempts[1].clear();
    state.precisionDetail=runtime_precision::ActualStatus(true,{},state.detectorBf16,detector)+
        "BF16 model selected; no accuracy or word-agreement startup requirement. Recognizer graphIdentity="+QString::fromLatin1(graphIdentity.toHex())+". "+
        (native ? QString::fromLatin1(native_precision::kRecipe)+"; original graph converted inside OkuFlow. " : QString{});
    if(observer) {try {observer(QJsonObject{{"role","recognizer"},{"tier",ModelTierToken(tier)},
        {"effectivePrecision","BF16-linear"},{"runtimePreference",true},{"expectedTextChecked",false},
        {"batch",capacity},{"T",int(shape.d[1])},{"classes",spec.classes},{"graphSha256",QString::fromLatin1(graphIdentity.toHex())},
        {"nativePreparation",native},
        {"acceptedPlan",state.recognizer->PlanReceipt("recognizer")}});}catch(...) {}}
}
void PreparePublicCandidate(State& state,const PaddleAssets& assets,const QString& sdk,
    const public_bf16::Request& request,precision_artifacts::Runtime runtime,
    ModelTier detector,ModelTier recognizer,int capacity,bool variableWidth,bool prepareWidth,
    const std::function<bool()>& stop,const std::function<bool()>& cancelled,const public_bf16::AttestationObserver& observer)
{
    using namespace public_bf16;
    CheckEligibility(detector,recognizer,capacity,variableWidth,prepareWidth,runtime,Catalog(request));
    runtime.tensorRtSha256=state.libraries->Provenance()["inferenceSha256"].toString().toLatin1();
    const auto model=ReadModel(request.modelRoot,assets,recognizer);
    const auto corpus=LoadCorpus(request,recognizer);
    state.detector->CheckPublicDetector();
    LocalBuild build{runtime.uuid,runtime.tensorRtVersion,{},runtime.major,runtime.minor};
    const auto cached=FindPlan(request,build,corpus);
    CheckPreparationSpace(cached);
    const auto attempt=CacheKey(build,corpus)+":"+(cached.bytes.isEmpty()?QByteArray("missing"):precision_artifacts::Digest(cached.bytes));
    RefuseRememberedFailure(request.cacheRoot,attempt);
    if(!SessionAttempts().Begin(attempt))throw Refusal(Reason::BuildRefused,"public BF16 identity already failed/in flight; no automatic retry");
    state.ownedAttempts[1]=attempt;
    state.publicEvidenceIdentity=BuildIdentity(build,corpus);
    state.publicGoldenStarted=true;
    state.recognizer=std::make_unique<Network>(Network::PublicTag{});
    state.publicCompletion.Queued();
    state.recognizer->PreparePublic(*state.libraries,state.logger,sdk,model,cached,state.stream,stop,int(assets.dictionary.size()));
    // Builder may have used the profile stream. Confirm before allocations or
    // N1 shape changes, and before any typed healthy-stream rejection cleanup.
    ConfirmPublicCompletion(state,cancelled);
    build.planFileSha256=precision_artifacts::Digest(state.recognizer->PublicPlan());
    state.publicEvidenceIdentity["planFileSha256"]=QString::fromLatin1(build.planFileSha256);
    const auto goldenDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    const auto goldenStop=[&]{return stop() || std::chrono::steady_clock::now()>=goldenDeadline;};
    RunPublicGolden(state,corpus,assets.dictionary,goldenStop,observer);
    RefuseStoppedCandidate(state.publicCompletion,goldenStop(),"public BF16 golden deadline after final N8");
    state.publicCompletion.RequireConfirmed();
    const auto golden=CheckGolden(corpus,build,state.publicObservations,Completion::Confirmed);
    state.publicGoldenRecord=golden.Record(); // retain actual failure outputs too
    if(observer) {try{observer(state.publicGoldenRecord);}catch(...) {}}
    RefuseStoppedCandidate(state.publicCompletion,stop(),"public BF16 deadline before publication");
    // A receipt never authorizes expected text and a cache hit is reattested.
    const auto receipt=ReceiptBytes(build,corpus,golden,runtime,state.publicCompletion);
    if(cached.bytes.isEmpty() && !PublishNew(request,build,corpus,golden,runtime,state.publicCompletion,state.recognizer->PublicPlan()))
        throw Refusal(Reason::BuildRefused,"public BF16 publication raced an existing generation; no replacement/retry");
    SessionAttempts().Passed(attempt);
    state.ownedAttempts[1].clear();
    state.recognizerBatch=capacity;state.variableWidth=false;state.recognizerBf16=true;
    state.precisionDetail=runtime_precision::ActualStatus(true,{},state.detectorBf16,detector)+
        (request.use==CatalogUse::CandidateValidation ? "Candidate validation only; production catalog remains unavailable. " : "Public model locally attested. ")+
        "Session attestation receipt SHA256="+QString::fromLatin1(precision_artifacts::Digest(receipt))+". ";
}
class TensorRtRecognizer final : public Recognizer
{
  public:
    TensorRtRecognizer(QString root, QString sdk,ModelTier detector,ModelTier recognizer,
                       TensorRtRetirementObserver retirementObserver,
                       std::shared_ptr<TensorRtCropDiagnostic> cropDiagnostic,
                       runtime_precision::Request precisionRequest = {},
                       public_bf16::Request publicRequest = {},public_bf16::AttestationObserver attestationObserver = {},
                       std::shared_ptr<const PrecisionExperimentPlan> experiment = {})
        : root_(std::move(root)), sdk_(std::move(sdk)),detectorTier_(detector),recognizerTier_(recognizer),
          retirementObserver_(std::move(retirementObserver)), cropDiagnostic_(std::move(cropDiagnostic)),
          precisionRequest_(std::move(precisionRequest)),publicRequest_(std::move(publicRequest)),
          attestationObserver_(std::move(attestationObserver)),experiment_(std::move(experiment))
    {
        // Medium/Small N8 retained text/dispositions under the
        // owner's 10-source-pixel geometry tolerance. Explicit overrides win.
        const auto batch = qEnvironmentVariable("OKUFLOW_UCT_TRT_BATCH");
        invalidBatch_ = !batch.isEmpty() && batch != "1" && batch != "8" && batch != "16";
        recognizerBatch_ = batch.isEmpty() ? DefaultTensorRtBatchCapacity(recognizer)
                                         : batch == "16" ? 16 : batch == "8" ? 8 : 1;
        const auto width = qEnvironmentVariable("OKUFLOW_UCT_TRT_WIDTH");
        variableWidth_ = width == recognizer_width::kBucketedToken;
        invalidWidth_ = !width.isEmpty() && width != recognizer_width::kFixedToken && !variableWidth_;
        const auto prepareWidth = qEnvironmentVariable("OKUFLOW_UCT_TRT_PREPARE_WIDTH");
        allowWidthPreparation_ = prepareWidth == "1";
        invalidWidth_ = invalidWidth_ || (!prepareWidth.isEmpty() && prepareWidth != "0" && prepareWidth != "1") ||
                        (allowWidthPreparation_ && !variableWidth_);
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
        if (invalidWidth_) {
            status_.detail = "OKUFLOW_UCT_TRT_WIDTH must be fixed1024 or bucketed; explicit PREPARE_WIDTH=1 requires bucketed.";
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
            status_.detail = "TensorRT ABI 11 runtime (minimum 11.4.0.106) is not installed.";
            return status_;
        }
        int count = 0;
        const auto devices=cudaGetDeviceCount(&count);
        if (devices != cudaSuccess || count == 0)
        {
            status_.detail = devices==cudaSuccess ? "No CUDA device is available." :
                QString("CUDA unavailable (%1): %2; CPU OCR remains available.").arg(cudaGetErrorName(devices),cudaGetErrorString(devices));
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
        status_.detailFocus = true;
        status_.detail = precisionRequest_.Requested()
            ? "Loading cache-only GPU text engines; local BF16 request is not yet validated."
            : "Preparing GPU text engines; CPU OCR is used until ready.";
        return status_;
    }
    BackendStatus Status() const override
    {
        auto status = status_;
        if (preparation_ && preparation_->phase.load(std::memory_order_acquire) == 1) {
            std::lock_guard lock(preparation_->mutex);
            status.detail = preparation_->state->precisionDetail + "GPU text engines ready.";
        }
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
    void SetDetailFocus(const std::optional<DetailFocus> &focus) override
    {
        detail_ = focus;
    }
    void SetCurvedRibbonInput(bool allowed) override {
        curvedRibbon_ = PaddleRibbonEnabled(allowed, qEnvironmentVariable("OKUFLOW_UCT_CURVED_RIBBON"));
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
        detail::FrozenUploadRetirement frozenUpload;
        const auto cancelOwnedInput = [&] {
            ObserveRetirement(retirementObserver_, frozenUpload.Cancel(state_, [](std::shared_ptr<State> state) {
                return RetireState(std::move(state));
            }));
        };
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
            // Frozen crop replay/capture describes whole-image regions only.
            const auto* frozen = cropDiagnostic_ && !detail_
                ? TensorRtCropDiagnosticAccess::Find(*cropDiagnostic_, image, assets_.detectorSha256, curvedRibbon_) : nullptr;
            TensorRtCropDiagnostic::Plan captured;
            bool captureCrops = cropDiagnostic_ && !frozen && !detail_ &&
                TensorRtCropDiagnosticAccess::HasCapacity(*cropDiagnostic_);
            if (captureCrops) {
                captured.source = state_->inFlight;
                captured.detectorSha256 = assets_.detectorSha256; captured.curvedRibbon=curvedRibbon_;
            }
            QVector<QRect> regions;
            if (frozen) for (const auto& region : frozen->regions) regions.push_back(region.bounds);
            else regions = PaddleRecognitionRegions({image.width,image.height}, detail_);
            if (detail_ && regions.isEmpty()) {
                // A view outside the image drops this request; engines stay healthy.
                state_->inFlight.reset();
                output.inputFault = true;
                output.error = QStringLiteral("Visible-detail region is outside the image.");
                return output;
            }
            const double magnification = detail_ ? PaddleDetailMagnification(*detail_) : 1.0;
            if (detail_) output.detailRegionPx = regions.first();
            int regionIndex = 0;
            for (const QRect& region : regions) {
            if (cancelled.load()) {
                output.cancelled=true;output.lines.clear();cancelOwnedInput();return output;
            }
            // A strided subview of the owned image, never a host pixel copy.
            const auto* cropPixels=static_cast<const unsigned char*>(image.Data())+
                std::size_t(region.y())*image.pitch+std::size_t(region.x())*4;
            auto *hostCount = state_->hostCount.Reserve(submissionGroup);
            const int maximumGroups = (recognizer_width::kMaximumCrops + submissionGroup - 1) / submissionGroup;
            auto* hostWidths = state_->variableWidth ? state_->hostGroupWidths.Reserve(maximumGroups) : nullptr;
            int count{};
            if (frozen) {
                const auto& recorded = frozen->regions[std::size_t(regionIndex)];
                if (recorded.crops.size() > std::size_t(recognizer_width::kMaximumCrops))
                    throw std::runtime_error("Frozen OCR crop metadata exceeds capacity.");
                std::vector<gpu::Quad> quads;
                quads.reserve(recorded.crops.size());
                for (const auto& crop : recorded.crops) {
                    gpu::Quad q{};
                    std::copy(crop.xy.begin(), crop.xy.end(), q.xy);
                    q.score = crop.score; q.validWidth = crop.validWidth; q.internalEdges = crop.internalEdges; q.ribbon=crop.ribbon;
                    quads.push_back(q);
                }
                count = int(quads.size());
                if (hostWidths)
                    for (int first = 0; first < count; first += submissionGroup)
                        hostWidths[first/submissionGroup] = recognizer_width::OrderedGroup(
                            quads.data()+first,std::min(submissionGroup,count-first));
                if (count) {
                    // Compact descriptor upload only. Retain pinned storage
                    // until the existing first recognition-group wait.
                    auto* upload = state_->hostQuad.Reserve(std::max(submissionGroup, count));
                    std::copy(quads.begin(), quads.end(), upload);
                    Check(cudaMemcpyAsync(state_->quads.Reserve(128*9), upload,
                                          quads.size()*sizeof(gpu::Quad), cudaMemcpyHostToDevice, state_->stream));
                    frozenUpload.Queued();
                }
            } else {
            const QSize size = PaddleDetectionSize(region.size(), magnification, detail_ ? detail_->detectorLongEdge : 960);
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
            profile.Measure(TensorRtProfile::Regions, state_->stream, [&] {
                gpu::Detect(state_->detector->Output(), w, h, region.width(), region.height(),
                            state_->labels.Reserve(w * h), state_->components.Reserve(w * h),
                            state_->detectedQuads.Reserve(128), state_->count.Reserve(1), state_->stream,
                            curvedRibbon_ ? state_->ribbonOffsets.Reserve((w*h+255)/256) : nullptr);
                gpu::RecognitionWindows(state_->detectedQuads.Get(),state_->count.Get(),
                                         state_->quads.Reserve(128*9),state_->stream);
                if (state_->variableWidth)
                    gpu::RecognitionGroupWidths(state_->quads.Get(),state_->count.Get(),submissionGroup,
                        state_->groupWidths.Reserve(maximumGroups),state_->stream);
                Check(cudaGetLastError());
            });
            profile.Measure(TensorRtProfile::Metadata, state_->stream, [&] {
                Check(cudaMemcpyAsync(hostCount, state_->count.Get(), sizeof(int),
                                      cudaMemcpyDeviceToHost, state_->stream));
                if (hostWidths)
                    Check(cudaMemcpyAsync(hostWidths,state_->groupWidths.Get(),maximumGroups*sizeof(int),
                                          cudaMemcpyDeviceToHost,state_->stream));
            });
            profile.Transfer(sizeof(int) + (hostWidths ? maximumGroups*sizeof(int) : 0));
            profile.Wait(true, [&] { Check(cudaStreamSynchronize(state_->stream)); }); // existing worker-only count wait
            count = *hostCount;
            }
            if (count < 0 || count > 128*9)
                throw std::runtime_error("GPU detector region count is invalid.");
            if (captureCrops) {
                // The detector count wait proved completion. Only this optional
                // compact record may be dropped; CUDA failures still propagate.
                try {
                    captured.regions.push_back({region, {}});
                } catch (const std::bad_alloc&) {
                    captureCrops = false;
                    captured = {};
                }
            }
            // Recognition groups stay in stream order on one context. The next
            // group with the identical canvas/batch shape is enqueued before
            // the host waits on this group's own event, so its inference
            // overlaps this group's host wait and merge. Merge order, inputs
            // and per-element arithmetic are unchanged. A shape change, cancellation or
            // fault never leaves enqueued work behind a released input lease.
            struct IssuedGroup {
                int first{}, width{}, size{}, steps{}, slot{};
                int* counts{}; gpu::Quad* quads{}; gpu::Token* tokens{};
            };
            detail::RecognitionGroups groupLifetime;
            const auto groupWidth = [&](int first) {
                const int width = hostWidths ? hostWidths[first/submissionGroup] : 1024;
                if (!recognizer_width::IsCanvas(width))
                    throw std::runtime_error("Invalid ordered-group recognizer canvas.");
                return width;
            };
            const auto issueGroup = [&](int first, int slot) {
                groupLifetime.Queue(slot);
                IssuedGroup group;
                group.first = first;
                group.slot = slot;
                group.width = groupWidth(first);
                group.size = std::min(submissionGroup, count - first);
                const int width = group.width, groupSize = group.size;
                const std::size_t samplePixels = static_cast<std::size_t>(width) * 48 * 3;
                auto *crop = state_->recognizerInput.Reserve(
                    samplePixels * state_->recognizerBatch);
                group.counts = state_->groupCount[slot].Reserve(submissionGroup);
                group.quads = state_->groupQuad[slot].Reserve(submissionGroup);
                int steps{};
                for (int batchFirst = 0; batchFirst < groupSize; batchFirst += state_->recognizerBatch)
                {
                    const int batchSize = std::min(state_->recognizerBatch, groupSize - batchFirst);
                    profile.Measure(TensorRtProfile::Rectify, state_->stream, [&] {
                        gpu::RectifyBatch(cropPixels, image.pitch, region.width(), region.height(),
                                          state_->quads.Get(), first + batchFirst, batchSize,
                                          crop, width, state_->stream);
                        Check(cudaGetLastError());
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
                    if (!group.tokens)
                        group.tokens = state_->groupTokens[slot].Reserve(
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
                        gpu::DecodeBatch(state_->recognizer->Output(), batchSize, steps,
                                         assets_.dictionary.size(), state_->quads.Get(), first + batchFirst,
                                         width, ids, scores, tokens, tokenCounts, state_->stream);
                        Check(cudaGetLastError());
                    });
                    profile.Measure(TensorRtProfile::Metadata, state_->stream, [&] {
                        // Three bulk copies into this slot only. The opposite
                        // pinned slot may still be in flight; device scratch is
                        // reused only later on this same stream, after copies.
                        Check(cudaMemcpyAsync(group.counts + batchFirst, tokenCounts,
                                              batchSize * sizeof(int), cudaMemcpyDeviceToHost, state_->stream));
                        Check(cudaMemcpyAsync(group.quads + batchFirst, state_->quads.Get() + first + batchFirst,
                                              batchSize * sizeof(gpu::Quad), cudaMemcpyDeviceToHost, state_->stream));
                        Check(cudaMemcpyAsync(group.tokens + static_cast<std::size_t>(batchFirst) * steps, tokens,
                                              static_cast<std::size_t>(batchSize) * steps * sizeof(gpu::Token),
                                              cudaMemcpyDeviceToHost, state_->stream));
                    });
                    profile.Transfer(static_cast<std::size_t>(batchSize) *
                        (sizeof(int) + sizeof(gpu::Quad) + steps * sizeof(gpu::Token)));
                    profile.RecognitionHelpers(1, 2, 3);
                }
                group.steps = steps;
                Check(cudaEventRecord(state_->GroupEvent(slot), state_->stream));
                return group;
            };
            // Drains enqueued look-ahead before an early return releases the
            // input lease; a failed drain propagates to normal retirement.
            const auto cancelGroups = [&] {
                groupLifetime.Cancel([&] { Check(cudaStreamSynchronize(state_->stream)); }, cancelOwnedInput);
                output.cancelled = true;
                output.lines.clear();
            };
            if (count > 0 && cancelled.load())
            {
                cancelGroups();
                return output;
            }
            std::optional<IssuedGroup> current;
            if (count > 0) current = issueGroup(0, 0);
            while (current)
            {
                const IssuedGroup group = *current;
                current.reset();
                const int next = group.first + submissionGroup;
                if (next < count && !cancelled.load() && groupWidth(next) == group.width &&
                    std::min(submissionGroup, count - next) == group.size)
                {
                    current = issueGroup(next, 1 - group.slot);
                }
                profile.Wait(false, [&] { Check(cudaEventSynchronize(state_->groupEvents[group.slot])); }); // this group's completion
                groupLifetime.Completed(group.slot);
                frozenUpload.Completed();
                if (cancelled.load())
                {
                    cancelGroups();
                    return output;
                }
                QElapsedTimer merge;
                if (profile.Enabled()) merge.start();
                const int width = group.width, steps = group.steps;
                for (int slot = 0; slot < group.size; ++slot)
                {
                    const int tokenCount = group.counts[slot];
                    const auto quad = group.quads[slot];
                    if (captureCrops) {
                        TensorRtCropDiagnostic::Crop record;
                        std::copy(std::begin(quad.xy), std::end(quad.xy), record.xy.begin());
                        record.score = quad.score; record.validWidth = quad.validWidth;
                        record.internalEdges = quad.internalEdges; record.ribbon=quad.ribbon;
                        // This group's CUDA completion was already confirmed.
                        try {
                            captured.regions.back().crops.push_back(record);
                        } catch (const std::bad_alloc&) {
                            captureCrops = false;
                            captured = {};
                        }
                    }
                    profile.CropDescriptor(quad.xy, quad.score, quad.validWidth,
                                           quad.internalEdges, region, width, &quad.ribbon);
                    const auto *tokens = group.tokens + static_cast<std::size_t>(slot) * steps;
                    if (tokenCount < 0)
                        continue; // untrustworthy crop, same policy as CPU decoder
                    if (tokenCount > steps)
                        throw std::runtime_error("GPU CTC output exceeds capacity.");
                    PaddleQuad box{};
                    box.score = quad.score;
                    box.internalEdges = quad.internalEdges; box.ribbon=quad.ribbon;
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
                groupLifetime.Consumed(group.slot);
                if (!current && next < count)
                {
                    // Shape change: the stream is idle, as before this change.
                    if (cancelled.load())
                    {
                        cancelGroups();
                        return output;
                    }
                    current = issueGroup(next, 1 - group.slot);
                }
            }
            ++regionIndex;
            }
            std::sort(output.lines.begin(), output.lines.end(), [](const auto &a, const auto &b) {
                return a.words[0].boundsPx.top() < b.words[0].boundsPx.top();
            });
            output.ok = !cancelled.load();
            output.cancelled = cancelled.load();
            profile.Emit(state_->recognizerBatch,state_->variableWidth);
            if (cropDiagnostic_) {
                if (frozen) TensorRtCropDiagnosticAccess::Replayed(*cropDiagnostic_);
                else if (captureCrops)
                    TensorRtCropDiagnosticAccess::TryCommit(*cropDiagnostic_, std::move(captured));
            }
            state_->inFlight.reset(); // final metadata copy proved all reads complete
            status_.detail = state_->precisionDetail + "PP-OCRv6 GPU preprocessing, detection, rectification, recognition "
                             "and CTC; CPU metadata publication; recognizer batch=" +
                             QString::number(state_->recognizerBatch) + ". " +
                             (state_->variableWidth ? QStringLiteral("Experimental width=bucketed. ") : QString()) +
                             state_->batchPreparationDetail;
        }
        catch (const std::exception &error)
        {
            // Completion instrumentation/status publication can fail after
            // inference set ok. Never return those candidates as successful.
            output.ok = false;
            output.lines.clear();
            // Ownership/event admission failed before this input enqueued work.
            // Keep the healthy engines; a bad frame must not rebuild models.
            if (output.inputFault && state_)
                state_->inFlight.reset();
            else
                Retire();
            // Seal resource ownership before fallible diagnostic formatting.
            output.error = QString::fromUtf8(error.what());
            status_.detail = output.error;
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
        status_.detail = precisionRequest_.Requested()
            ? "Loading cache-only GPU text engines; local BF16 request is not yet validated."
            : "Preparing GPU text engines; CPU OCR is used until ready.";
        const auto preparation = preparation_;
        const auto assets = assets_;
        const auto sdk = sdk_;
        const auto precisionRequest = precisionRequest_;
        const auto publicRequest = publicRequest_;
        const auto attestationObserver = attestationObserver_;
        const auto experiment = experiment_;
        const auto detectorTier = detectorTier_, recognizerTier = recognizerTier_;
        const int recognizerBatch = recognizerBatch_;
        const bool variableWidth = variableWidth_, allowWidthPreparation = allowWidthPreparation_;
        try
        {
            std::thread([preparation, assets, sdk, recognizerBatch, variableWidth, allowWidthPreparation,
                         precisionRequest, publicRequest, attestationObserver, experiment, detectorTier, recognizerTier] {
                try
                {
                    const auto baselinePolicy=public_bf16::BaselinePolicy(publicRequest);
                    const std::weak_ptr<Preparation> weak=preparation;
                    const auto cancelled=[weak] {
                        const auto owner=weak.lock();
                        return !owner || owner->cancel.load();
                    };
                    const auto baselineCache=baselinePolicy.root;
                    Check(cudaSetDevice(preparation->device));
                    cudaDeviceProp properties{};
                    Check(cudaGetDeviceProperties(&properties, preparation->device));
                    if (properties.major * 10 + properties.minor < 75)
                        throw std::runtime_error("TensorRT requires Turing or newer.");
                    auto state = std::make_shared<State>();
                    state->device = preparation->device;
                    state->variableWidth = variableWidth;
                    state->precisionDetail = runtime_precision::ActualStatus(false);
                    {
                        std::lock_guard lock(preparation->mutex);
                        preparation->state = state;
                    }
                    int low{}, high{};
                    Check(cudaDeviceGetStreamPriorityRange(&low, &high));
                    Check(cudaStreamCreateWithPriority(&state->stream, cudaStreamNonBlocking, low));
                    state->libraries = std::make_unique<Libraries>(sdk);
                    int driver{};
                    Check(cudaDriverGetVersion(&driver));
                    const auto precisionDetail = [&](bool bf16, const QString& fallback = QString()) {
                        return runtime_precision::ActualStatus(bf16, fallback,state->detectorBf16,detectorTier) +
                            QString("TensorRT runtime=%1; CUDA driver API=%2. ")
                                .arg(QString::fromLatin1(state->libraries->tensorRtVersion)).arg(driver);
                    };
                    QByteArray precisionPlan;
                    QString precisionFallback;
                    if (precisionRequest.Requested()) {
                        const runtime_precision::Device device{
                            QByteArray(reinterpret_cast<const char*>(&properties.uuid), sizeof(properties.uuid)),
                            driver, properties.major, state->libraries->tensorRtVersion};
                        precisionFallback = runtime_precision::Unsupported(detectorTier, recognizerTier,
                            recognizerBatch, variableWidth, allowWidthPreparation, device);
                        if (precisionFallback.isEmpty()) {
                            try {
                                const auto selection = runtime_precision::LoadManifest(precisionRequest, assets.detector);
                                precisionPlan = runtime_precision::ReadPlan(selection, device);
                            } catch (const runtime_precision::ArtifactError& error) {
                                precisionFallback = QString::fromUtf8(error.what());
                            }
                        }
                    }
                    state->precisionDetail = precisionDetail(false, precisionFallback);
                    if(experiment) {
                        const auto refusal=PrecisionExperimentRefusal(*experiment,assets,recognizerTier,
                            recognizerBatch,variableWidth,allowWidthPreparation,
                            QByteArray(reinterpret_cast<const char*>(&properties.uuid),sizeof(properties.uuid)),
                            properties.major,state->libraries->tensorRtVersion);
                        if(!refusal.isEmpty())throw std::runtime_error(refusal.toStdString());
                        precisionPlan=experiment->bytes;
                    }
                    if(cancelled())throw std::runtime_error("GPU preparation cancelled before FP32 detector");
                    if(publicRequest.enabled && publicRequest.preferDetector && !publicRequest.fp32Control && !experiment && !precisionRequest.Requested() && properties.major>=8) {
                        const auto detectorDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(600);
                        const auto detectorStop=[cancelled,detectorDeadline]{return cancelled() || std::chrono::steady_clock::now()>=detectorDeadline;};
                        QByteArray attempt;
                        bool validating=false;
                        try {
                            const bool native = publicRequest.use == public_bf16::CatalogUse::Production;
                            const auto path=native ? assets.detector : bf16_detector::ReadGraphPath(publicRequest.modelRoot,assets,detectorTier);
                            const auto expected=native ? assets.detectorSha256 : QByteArray(bf16_detector::GraphHash(detectorTier));
                            const auto identity=native ? native_precision::GraphIdentity(QByteArray::fromHex(expected)) : QByteArray::fromHex(expected);
                            attempt="detector:"+bf16_detector::PlanKey(identity,
                                QByteArray(reinterpret_cast<const char*>(&properties.uuid),sizeof(properties.uuid)),state->libraries->tensorRtVersion);
                            public_bf16::RefuseRememberedFailure(publicRequest.cacheRoot,attempt);
                            if(!public_bf16::SessionAttempts().Begin(attempt))throw runtime_precision::ArtifactError("BF16 detector already failed or in flight this session");
                            state->ownedAttempts[0]=attempt;
                            state->detector=std::make_unique<Network>(Network::PublicTag{});
                            state->publicCompletion.Queued();
                            state->detector->PreparePinned(*state->libraries,state->logger,sdk,path,expected,true,state->device,
                                state->stream,detectorStop,1,false,true,nullptr,baselineCache,{},true,0,native);
                            ConfirmPublicCompletion(*state,cancelled);
                            public_bf16::RefuseStoppedCandidate(state->publicCompletion,detectorStop(),"BF16 detector build deadline");
                            validating=true;
                            SmokeDetector(*state,detectorStop);
                            public_bf16::RefuseStoppedCandidate(state->publicCompletion,detectorStop(),"BF16 detector deadline after smoke check");
                            state->detectorBf16=true;
                            public_bf16::SessionAttempts().Passed(attempt);
                            state->ownedAttempts[0].clear();
                        } catch(const std::exception& error) {
                            if(!detail::AllowsTensorRtCapacityFallback(std::current_exception()))throw;
                            // Actual inference/ABI/deadline faults are terminal;
                            // only an explicitly typed completed map refusal may
                            // choose FP32 after smoke execution has started.
                            if(validating && !dynamic_cast<const runtime_precision::ArtifactError*>(&error))throw;
                            Check(cudaPeekAtLastError());
                            if(cancelled())throw std::runtime_error("BF16 detector cancelled before FP32 fallback");
                            if(state->detector) {
                                ConfirmPublicCompletion(*state,cancelled);
                                public_bf16::DiscardAfterCompletion(state->publicCompletion,[&]{state->detector.reset();});
                            }
                            state->detectorFallback=QString::fromUtf8(error.what());
                            if(!state->ownedAttempts[0].isEmpty()) {
                                try {public_bf16::WriteFailure(publicRequest.cacheRoot,state->ownedAttempts[0],state->detectorFallback,state->publicCompletion);}
                                catch(const runtime_precision::ArtifactError& memo) {state->detectorFallback+="; failure memo unavailable: "+QString::fromUtf8(memo.what());}
                            }
                            state->ownedAttempts[0].clear(); // failed latch stays; this attempt is no longer in flight
                        }
                    }
                    if(!state->detector) {
                        state->detector=std::make_unique<Network>(Network::PublicTag{});
                        state->detector->PreparePinned(*state->libraries, state->logger, sdk, assets.detector, assets.detectorSha256, true, state->device,
                            state->stream,cancelled,1,false,
                            baselinePolicy.allowPreparation && !experiment && !precisionRequest.Requested() && (!variableWidth || allowWidthPreparation),nullptr,baselineCache);
                    }
                    state->precisionDetail=precisionDetail(false,precisionFallback);
                    if(attestationObserver && publicRequest.enabled) {
                        try {attestationObserver(QJsonObject{{"role","detector"},{"tier",ModelTierToken(detectorTier)},
                            {"requestedBf16",publicRequest.preferDetector},{"effectivePrecision",state->detectorBf16 ? "BF16-linear" : "FP32"},
                            {"graphSha256",state->detectorBf16 && publicRequest.use!=public_bf16::CatalogUse::Production
                                ? QString::fromLatin1(bf16_detector::GraphHash(detectorTier)) : QString::fromLatin1(assets.detectorSha256)},
                            {"nativePreparation",state->detectorBf16 && publicRequest.use==public_bf16::CatalogUse::Production},
                            {"smoke",state->detectorSmoke},{"qualityAttested",false},{"fallback",state->detectorFallback},
                            {"acceptedPlan",state->detector->PlanReceipt("detector")}});}catch(...) {}
                    }
                    if (preparation->cancel.load())
                        throw std::runtime_error("GPU preparation cancelled.");
                    int capacity = recognizerBatch;
                    if(publicRequest.enabled && publicRequest.runtimePreference && !publicRequest.fp32Control && !experiment && !precisionRequest.Requested()) {
                        if(properties.major<8 || variableWidth) {
                            state->precisionDetail+="BF16 recognizer unavailable for this GPU/width profile; selected-tier FP32 remains selected. ";
                        } else {
                            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(600);
                            const auto stop=[cancelled,deadline]{return cancelled() || std::chrono::steady_clock::now()>=deadline;};
                            try {
                                PrepareRuntimeRecognizer(*state,assets,sdk,publicRequest,detectorTier,recognizerTier,capacity,
                                    QByteArray(reinterpret_cast<const char*>(&properties.uuid),sizeof(properties.uuid)),stop,cancelled,attestationObserver);
                            } catch(const runtime_precision::ArtifactError& error) {
                                Check(cudaPeekAtLastError());
                                if(cancelled())throw std::runtime_error("BF16 recognizer cancelled before FP32 fallback");
                                if(state->recognizer) {
                                    ConfirmPublicCompletion(*state,cancelled);
                                    public_bf16::DiscardAfterCompletion(state->publicCompletion,[&]{state->recognizer.reset();});
                                }
                                QString memoDetail;
                                if(!state->ownedAttempts[1].isEmpty()) {
                                    try {public_bf16::WriteFailure(publicRequest.cacheRoot,state->ownedAttempts[1],QString::fromUtf8(error.what()),state->publicCompletion);}
                                    catch(const runtime_precision::ArtifactError& memo) {memoDetail="; failure memo unavailable: "+QString::fromUtf8(memo.what());}
                                }
                                state->ownedAttempts[1].clear();
                                state->precisionDetail=precisionDetail(false)+"Runtime BF16 unavailable: "+QString::fromUtf8(error.what())+memoDetail+"; selected-tier FP32 fallback. ";
                            }
                        }
                    }
                    if(publicRequest.enabled && !publicRequest.runtimePreference && !publicRequest.fp32Control && !experiment && !precisionRequest.Requested()) {
                        if(cancelled())throw std::runtime_error("public BF16 preparation cancelled before eligibility");
                        const precision_artifacts::Runtime runtime{
                            QByteArray(reinterpret_cast<const char*>(&properties.uuid),sizeof(properties.uuid)),
                            driver,properties.major,properties.minor,state->libraries->tensorRtVersion,{}, {}, {}};
                        std::function<bool()> publicStop=cancelled;
                        try {
                            // Ineligible/unavailable requests do not start a BF16
                            // attempt or put a deadline on ordinary FP32 work.
                            public_bf16::CheckEligibility(detectorTier,recognizerTier,recognizerBatch,
                                variableWidth,allowWidthPreparation,runtime,public_bf16::Catalog(publicRequest));
                            const auto publicDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(600);
                            publicStop=[cancelled,publicDeadline] {
                                return cancelled() || std::chrono::steady_clock::now()>=publicDeadline;
                            };
                            PreparePublicCandidate(*state,assets,sdk,publicRequest,runtime,detectorTier,recognizerTier,
                                recognizerBatch,variableWidth,allowWidthPreparation,publicStop,cancelled,attestationObserver);
                        } catch(const runtime_precision::ArtifactError& error) {
                            // Only a typed, confirmed healthy-stream refusal may
                            // retain FP32. Actual API/CUDA/context/enqueue/copy/
                            // event faults never enter this alternate-engine path.
                            Check(cudaPeekAtLastError());
                            if(state->recognizer) {
                                ConfirmPublicCompletion(*state,cancelled);
                                public_bf16::DiscardAfterCompletion(state->publicCompletion,[&]{state->recognizer.reset();});
                            }
                            ReportPublicPartial(*state,attestationObserver,"confirmed-incomplete",error.what());
                            if(cancelled())throw std::runtime_error("public BF16 cancelled before FP32 fallback");
                            QString memoDetail;
                            if(!state->ownedAttempts[1].isEmpty()) {
                                try {public_bf16::WriteFailure(publicRequest.cacheRoot,state->ownedAttempts[1],QString::fromUtf8(error.what()),state->publicCompletion);}
                                catch(const runtime_precision::ArtifactError& memo) {memoDetail="BF16 failure memo unavailable: "+QString::fromUtf8(memo.what())+". ";}
                            }
                            state->ownedAttempts[1].clear(); // cancellation of later FP32 work cannot erase this failure
                            const auto* refusal=dynamic_cast<const public_bf16::Refusal*>(&error);
                            const auto reason=refusal?refusal->Code():public_bf16::Reason::Model;
                            state->precisionDetail=precisionDetail(false)+QString("Public BF16 unavailable (typed reason=%1 %2): ")
                                .arg(int(reason)).arg(public_bf16::ReasonName(reason))+QString::fromUtf8(error.what())+". "+memoDetail;
                        }
                    }
                    if (!precisionPlan.isEmpty()) {
                        try {
                            state->recognizer = std::make_unique<Network>(Network::PublicTag{});
                            state->recognizer->PreparePinned(
                                *state->libraries, state->logger, sdk, assets.recognizer,
                                assets.recognizerSha256, false, state->device,
                                state->stream,cancelled,8,false,false,&precisionPlan,QString{},
                                experiment ? "verified-experimental-bf16" : "verified-local-bf16");
                            state->recognizerBatch = 8;
                            state->recognizerBf16=true;
                            state->precisionDetail = precisionDetail(true);
                            if(experiment)state->precisionDetail+="Diagnostic experiment="+ModelTierToken(experiment->tier)+
                                "; graphSha256="+QString::fromLatin1(experiment->graphSha256)+"; no production approval. ";
                        } catch (const runtime_precision::ArtifactError& error) {
                            // Only pre-inference artifact rejection permits FP32.
                            // API/allocation/context faults reach the outer failure path.
                            Check(cudaStreamSynchronize(state->stream));
                            state->recognizer.reset();
                            if(experiment)throw std::runtime_error(std::string("Experimental BF16 plan refused; no alternate-engine retry: ")+error.what());
                            state->precisionDetail = precisionDetail(false, QString::fromUtf8(error.what()));
                        }
                    }
                    if (!state->recognizer)
                    for (;;)
                    {
                        try
                        {
                            state->recognizer = std::make_unique<Network>(Network::PublicTag{});
                            state->recognizer->PreparePinned(
                                *state->libraries, state->logger, sdk, assets.recognizer,
                                assets.recognizerSha256, false, state->device,
                                state->stream,cancelled,capacity,variableWidth,
                                baselinePolicy.allowPreparation && !experiment && !precisionRequest.Requested() && (!variableWidth || allowWidthPreparation),nullptr,baselineCache);
                            state->recognizerBatch = capacity;
                            break;
                        }
                        catch (const std::exception &error)
                        {
                            if (!detail::AllowsTensorRtCapacityFallback(std::current_exception())) throw;
                            // An explicit width-profile preparation authorizes
                            // this N only; never cascade into extra new plans.
                            if (precisionRequest.Requested() || capacity == 1 || preparation->cancel.load() ||
                                (variableWidth && allowWidthPreparation)) throw;
                            // Preserve a supported smaller GPU profile. Unknown
                            // completion still quarantines the whole state.
                            Check(cudaStreamSynchronize(state->stream));
                            state->recognizer.reset();
                            if (preparation->cancel.load())
                                throw std::runtime_error("GPU preparation cancelled.");
                            state->batchPreparationDetail += QString("Batch-%1 preparation failed: ").arg(capacity) +
                                                             QString::fromUtf8(error.what()) + "; ";
                            capacity = capacity == 16 ? 8 : 1;
                        }
                    }
                    if (preparation->cancel.load())
                        throw std::runtime_error("GPU preparation cancelled.");
                    if(publicRequest.fp32Control) {
                        RecordFp32Control(*state,assets,publicRequest,recognizerTier,cancelled,attestationObserver);
                        state->precisionDetail+="FP32 control only; no BF16 activation or publication. ";
                    }
                    ConfirmPublicCompletion(*state,cancelled);
                    state->detector->ReleaseBuildResources();state->recognizer->ReleaseBuildResources();
                    state->precisionDetail+=QString("Selected detector=%1/%2; selected recognizer=%3; attempted BF16 detector tensor smoke=%4/4. ")
                        .arg(ModelTierToken(detectorTier),state->detectorBf16 ? "BF16-linear" : "FP32",ModelTierToken(recognizerTier))
                        .arg(state->detectorSmoke.size());
                    if(!state->detectorFallback.isEmpty())state->precisionDetail+="Detector FP32 fallback: "+state->detectorFallback+". ";
                    if(state->detectorBf16)state->precisionDetail+= publicRequest.use==public_bf16::CatalogUse::Production
                        ? "Detector sourceGraphSha256="+QString::fromLatin1(assets.detectorSha256)+"; "+QString::fromLatin1(native_precision::kRecipe)+". "
                        : "Detector graphSha256="+QString::fromLatin1(bf16_detector::GraphHash(detectorTier))+". ";
                    state->precisionDetail += state->detector->PlanReceipt("detector") +
                                              state->recognizer->PlanReceipt("recognizer");
                    preparation->phase.store(1, std::memory_order_release);
                }
                catch (const std::exception &error)
                {
                    // A user abort is not a failed model. Release only this
                    // preparation's latch keys after a fresh bounded drain;
                    // unknown completion or real CUDA/API faults keep them held.
                    if(preparation->cancel.load() && preparation->state &&
                       detail::AllowsTensorRtCapacityFallback(std::current_exception())) {
                        try {
                            auto& state=*preparation->state;Check(cudaPeekAtLastError());
                            ConfirmPublicCompletion(state,[]{return false;});
                            for(auto& key:state.ownedAttempts)if(!key.isEmpty()) {
                                public_bf16::SessionAttempts().Cancelled(key,public_bf16::Completion::Confirmed);key.clear();
                            }
                        }catch(...) {} // preserve original failure and retained ownership
                    }
                    if(preparation->state)ReportPublicPartial(*preparation->state,attestationObserver,"unknown",error.what());
                    {
                        std::lock_guard lock(preparation->mutex);
                        preparation->error = (preparation->state ? preparation->state->precisionDetail :
                            precisionRequest.Requested() ? QStringLiteral("Local BF16 request unavailable. ") : QString()) +
                            QString::fromUtf8(error.what());
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
        status_.detail = state_->precisionDetail + "GPU text engines ready.";
        if(publicRequest_.enabled && !precisionRequest_.Requested())
            status_.name=QString("PaddleOCR / TensorRT CUDA [%1 / %2]")
                .arg(state_->detectorBf16 ? "BF16" : "FP32",state_->recognizerBf16 ? "BF16" : "FP32");
        return true;
    }
    void Retire() noexcept
    {
        if (preparation_)
        {
            preparation_->cancel = true;
            preparation_.reset();
        }
        ObserveRetirement(retirementObserver_, detail::RetireOwnedState(state_, RetireState));
    }
    QString root_, sdk_;
    ModelTier detectorTier_,recognizerTier_;
    TensorRtRetirementObserver retirementObserver_;
    int recognizerBatch_{1};
    bool invalidBatch_{}, invalidWidth_{}, variableWidth_{}, allowWidthPreparation_{};
    std::shared_ptr<TensorRtCropDiagnostic> cropDiagnostic_;
    const runtime_precision::Request precisionRequest_;
    const public_bf16::Request publicRequest_;
    const public_bf16::AttestationObserver attestationObserver_;
    const std::shared_ptr<const PrecisionExperimentPlan> experiment_;
    std::optional<DetailFocus> detail_;
    bool curvedRibbon_{}; // worker-confined, one recognition at a time
    PaddleAssets assets_;
    BackendStatus status_;
    std::shared_ptr<State> state_;
    std::shared_ptr<Preparation> preparation_;
};
#include "uct_formula_tensorrt.hpp"
} // namespace

formula::Prepared formula::Prepare(formula::Model model, const QString& sourceRoot, const QString& cacheRoot,
                                   const std::atomic<bool>& cancelled,
                                   std::chrono::steady_clock::time_point deadline)
{
    formula::Prepared result;
    try {
        const auto definition=formula::Describe(model);
        formula::VerifySources(definition,sourceRoot,cancelled);
        auto engine=std::make_unique<NativeFormulaEngine>(definition);
        engine->Prepare(sourceRoot,cacheRoot,cancelled,deadline);
        const std::vector<float> neutral(768*768,(1.f-.7931f)/.1738f);
        const auto probe=engine->Recognize(neutral,cancelled,deadline,2);
        if(cancelled.load() || !probe.compatible)throw std::runtime_error("Formula execution compatibility check failed.");
        result.engine=std::move(engine);
        result.detail="Native TensorRT; FP32 graph, TF32 permitted. Compatibility is checked separately from text accuracy.";
    } catch(const std::exception& error) {
        result.cancelled=cancelled.load();
        result.code=result.cancelled ? "cancelled" : "formula_setup_failed";
        result.detail=QString::fromUtf8(error.what());
    }
    return result;
}
// Private diagnostic seam: identical Libraries admission/loading, no factory
// invocation, context, CUDA call, cache access or model preparation.
std::unique_ptr<Recognizer> CreatePrecisionExperimentRecognizer(const QString& root,const QString& sdk,
    ModelTier tier,std::shared_ptr<const PrecisionExperimentPlan> plan,TensorRtRetirementObserver observer)
{
    if(!plan)throw std::invalid_argument("A verified diagnostic payload is required.");
    return std::make_unique<TensorRtRecognizer>(root,sdk,ModelTier::Medium,tier,std::move(observer),
        std::shared_ptr<TensorRtCropDiagnostic>{},runtime_precision::Request{},public_bf16::Request{},
        public_bf16::AttestationObserver{},std::move(plan));
}
QJsonObject TensorRtRuntimeForDiagnostic(const QString& sdk)
{
    return Libraries(sdk).Provenance();
}
std::unique_ptr<Recognizer> CreateTensorRtRecognizer(const QString &root, const QString &sdk,ModelTier detector,ModelTier recognizer,
                                                   TensorRtRetirementObserver retirementObserver,
                                                   std::shared_ptr<TensorRtCropDiagnostic> cropDiagnostic)
{
    const auto selected=runtime_paths::TensorRtRoot(sdk.isEmpty()?qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT"):sdk,
        QCoreApplication::applicationDirPath(),qEnvironmentVariable("LOCALAPPDATA"));
    return std::make_unique<TensorRtRecognizer>(root.isEmpty() ? DefaultPaddleRoot() : root,
                                                selected,detector,recognizer,std::move(retirementObserver),std::move(cropDiagnostic));
}
std::unique_ptr<Recognizer> CreateProductionTensorRtRecognizer(ModelTier detector, ModelTier recognizer,
                                                              TensorRtRetirementObserver retirementObserver,
                                                              const QString& preparationCache)
{
    const auto sdk=runtime_paths::TensorRtRoot(qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT"),
        QCoreApplication::applicationDirPath(),qEnvironmentVariable("LOCALAPPDATA"));
    const auto modelRoot=QDir(QCoreApplication::applicationDirPath()).filePath("models");
    const public_bf16::Request publicRequest{modelRoot,{},
        preparationCache.isEmpty() ? QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/model-cache/engines")
                                   : preparationCache,public_bf16::CatalogUse::Production,true,true,false,true};
    return std::make_unique<TensorRtRecognizer>(DefaultPaddleRoot(), sdk, detector, recognizer,
        std::move(retirementObserver), std::shared_ptr<TensorRtCropDiagnostic>{},
        runtime_precision::ReadApplicationRequest(),publicRequest);
}
std::unique_ptr<Recognizer> CreatePublicBf16ValidationRecognizer(const QString& root,const QString& sdk,
    const QString& modelRoot,const QString& corpusRoot,const QString& cacheRoot,
    public_bf16::AttestationObserver observer,TensorRtRetirementObserver retirementObserver,
    ModelTier detector,ModelTier recognizer,bool preferDetector,bool fp32Control,bool runtimePreference)
{
    // Explicit private caller only: never reads the schema-1 selector and never
    // changes the compiled production catalog or public diagnostic defaults.
    return std::make_unique<TensorRtRecognizer>(root,sdk,detector,recognizer,
        std::move(retirementObserver),std::shared_ptr<TensorRtCropDiagnostic>{},runtime_precision::Request{},
        public_bf16::Request{modelRoot,corpusRoot,cacheRoot,public_bf16::CatalogUse::CandidateValidation,true,preferDetector,fp32Control,runtimePreference},std::move(observer));
}
} // namespace okuflow::uct
#else
namespace okuflow::uct
{
formula::Prepared formula::Prepare(formula::Model, const QString&, const QString&, const std::atomic<bool>&,
                                   std::chrono::steady_clock::time_point)
{
    return {{},"formula_gpu_unavailable","Math requires the native TensorRT CUDA build.",false};
}
std::unique_ptr<Recognizer> CreatePrecisionExperimentRecognizer(const QString&,const QString&,ModelTier,
    std::shared_ptr<const PrecisionExperimentPlan>,TensorRtRetirementObserver)
{
    return {};
}
std::unique_ptr<Recognizer> CreatePublicBf16ValidationRecognizer(const QString&,const QString&,
    const QString&,const QString&,const QString&,public_bf16::AttestationObserver,TensorRtRetirementObserver,ModelTier,ModelTier,bool,bool,bool)
{
    return {};
}
std::unique_ptr<Recognizer> CreateProductionTensorRtRecognizer(ModelTier detector, ModelTier recognizer,
                                                              TensorRtRetirementObserver retirementObserver,
                                                              const QString&)
{
    return CreateTensorRtRecognizer({}, {}, detector, recognizer, std::move(retirementObserver));
}
std::unique_ptr<Recognizer> CreateTensorRtRecognizer(const QString &, const QString &,ModelTier,ModelTier,
                                                   TensorRtRetirementObserver, std::shared_ptr<TensorRtCropDiagnostic>)
{
    return {};
}
} // namespace okuflow::uct
#endif
