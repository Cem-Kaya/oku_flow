#include "okuflow/common/maxine_superres.hpp"

#if defined(_WIN32) && OKUFLOW_ENABLE_CUDA

#include <windows.h>
#include <cuda_runtime_api.h>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include "okuflow/common/deadline_worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

// nvCVImage.h marks its C++ convenience wrapper dllexport under MSVC, which
// would make a runtime-only client emit link-time references to the SDK. Hide
// that annotation while preserving the C ABI declarations we resolve below.
#pragma push_macro("_MSC_VER")
#undef _MSC_VER
#include "nvVideoEffects.h"
#pragma pop_macro("_MSC_VER")

namespace okuflow {
namespace {

constexpr wchar_t kRuntimeRelativePath[] = L"NVIDIA Corporation\\NVIDIA Video Effects";

std::wstring ReadEnvironment(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length <= 1) {
        return {};
    }
    std::wstring value(length - 1, L'\0');
    GetEnvironmentVariableW(name, value.data(), length);
    return value;
}

bool HasRuntimeLibraries(const std::wstring& directory) {
    if (directory.empty()) {
        return false;
    }
    const std::filesystem::path root(directory);
    return std::filesystem::exists(root / L"NVVideoEffects.dll") &&
           std::filesystem::exists(root / L"NVCVImage.dll");
}

std::wstring FindRuntimeInUninstallRegistry(REGSAM view) {
    constexpr wchar_t kUninstallKey[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_READ | view, &root) != ERROR_SUCCESS) {
        return {};
    }

    DWORD index = 0;
    std::array<wchar_t, 256> subkeyName{};
    DWORD subkeyLength = static_cast<DWORD>(subkeyName.size());
    while (RegEnumKeyExW(root, index++, subkeyName.data(), &subkeyLength,
                         nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        HKEY subkey = nullptr;
        if (RegOpenKeyExW(root, subkeyName.data(), 0, KEY_READ | view, &subkey) == ERROR_SUCCESS) {
            std::array<wchar_t, 512> displayName{};
            DWORD displayBytes = static_cast<DWORD>(displayName.size() * sizeof(wchar_t));
            std::array<wchar_t, 1024> installLocation{};
            DWORD locationBytes = static_cast<DWORD>(installLocation.size() * sizeof(wchar_t));
            const bool matches =
                RegGetValueW(subkey, nullptr, L"DisplayName", RRF_RT_REG_SZ, nullptr,
                             displayName.data(), &displayBytes) == ERROR_SUCCESS &&
                std::wstring_view(displayName.data()).find(L"NVIDIA Video Effects") != std::wstring_view::npos;
            if (matches &&
                RegGetValueW(subkey, nullptr, L"InstallLocation", RRF_RT_REG_SZ, nullptr,
                             installLocation.data(), &locationBytes) == ERROR_SUCCESS &&
                HasRuntimeLibraries(installLocation.data())) {
                const std::wstring result(installLocation.data());
                RegCloseKey(subkey);
                RegCloseKey(root);
                return result;
            }
            RegCloseKey(subkey);
        }
        subkeyLength = static_cast<DWORD>(subkeyName.size());
    }
    RegCloseKey(root);
    return {};
}

std::string NarrowUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

template <typename Function>
Function Resolve(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

} // namespace

class MaxineSuperRes::Impl {
public:
    using CreateEffectFn = NvCV_Status(__cdecl*)(NvVFX_EffectSelector, NvVFX_Handle*);
    using DestroyEffectFn = void(__cdecl*)(NvVFX_Handle);
    using SetU32Fn = NvCV_Status(__cdecl*)(NvVFX_Handle, NvVFX_ParameterSelector, unsigned int);
    using SetF32Fn = NvCV_Status(__cdecl*)(NvVFX_Handle, NvVFX_ParameterSelector, float);
    using SetImageFn = NvCV_Status(__cdecl*)(NvVFX_Handle, NvVFX_ParameterSelector, NvCVImage*);
    using SetStringFn = NvCV_Status(__cdecl*)(NvVFX_Handle, NvVFX_ParameterSelector, const char*);
    using SetCudaStreamFn = NvCV_Status(__cdecl*)(NvVFX_Handle, NvVFX_ParameterSelector, CUstream);
    using LoadFn = NvCV_Status(__cdecl*)(NvVFX_Handle);
    using RunFn = NvCV_Status(__cdecl*)(NvVFX_Handle, int);
    using ImageAllocFn = NvCV_Status(__cdecl*)(NvCVImage*, unsigned int, unsigned int,
                                                NvCVImage_PixelFormat, NvCVImage_ComponentType,
                                                unsigned int, unsigned int, unsigned int);
    using ImageDeallocFn = void(__cdecl*)(NvCVImage*);
    using ImageTransferFn = NvCV_Status(__cdecl*)(const NvCVImage*, NvCVImage*, float,
                                                   CUstream, NvCVImage*);

    ~Impl() { Teardown(); }

    NvCVImage* SourceFloat() { return reinterpret_cast<NvCVImage*>(sourceFloat_.data()); }
    NvCVImage* DestinationFloat() { return reinterpret_cast<NvCVImage*>(destinationFloat_.data()); }
    NvCVImage* TransferTemp() { return reinterpret_cast<NvCVImage*>(transferTemp_.data()); }
    NvCVImage* SourceView() { return reinterpret_cast<NvCVImage*>(sourceView_.data()); }
    NvCVImage* DestinationView() { return reinterpret_cast<NvCVImage*>(destinationView_.data()); }

    bool LoadRuntime() {
        if (videoEffectsModule_ && imageModule_) {
            return true;
        }
        if (runtimeProbeAttempted_) {
            return false;
        }
        runtimeProbeAttempted_ = true;
        runtimeDirectory_ = MaxineSuperRes::FindRuntimeDirectory();
        if (runtimeDirectory_.empty()) {
            lastError_ = "NVIDIA Video Effects runtime is not installed";
            return false;
        }

        const std::filesystem::path root(runtimeDirectory_);
        imageModule_ = LoadLibraryExW((root / L"NVCVImage.dll").c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        videoEffectsModule_ = LoadLibraryExW((root / L"NVVideoEffects.dll").c_str(), nullptr,
                                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!imageModule_ || !videoEffectsModule_) {
            lastError_ = "NVIDIA Video Effects runtime libraries could not be loaded";
            UnloadModules();
            return false;
        }

        createEffect_ = Resolve<CreateEffectFn>(videoEffectsModule_, "NvVFX_CreateEffect");
        destroyEffect_ = Resolve<DestroyEffectFn>(videoEffectsModule_, "NvVFX_DestroyEffect");
        setU32_ = Resolve<SetU32Fn>(videoEffectsModule_, "NvVFX_SetU32");
        setF32_ = Resolve<SetF32Fn>(videoEffectsModule_, "NvVFX_SetF32");
        setImage_ = Resolve<SetImageFn>(videoEffectsModule_, "NvVFX_SetImage");
        setString_ = Resolve<SetStringFn>(videoEffectsModule_, "NvVFX_SetString");
        setCudaStream_ = Resolve<SetCudaStreamFn>(videoEffectsModule_, "NvVFX_SetCudaStream");
        load_ = Resolve<LoadFn>(videoEffectsModule_, "NvVFX_Load");
        run_ = Resolve<RunFn>(videoEffectsModule_, "NvVFX_Run");
        imageAlloc_ = Resolve<ImageAllocFn>(imageModule_, "NvCVImage_Alloc");
        imageDealloc_ = Resolve<ImageDeallocFn>(imageModule_, "NvCVImage_Dealloc");
        imageTransfer_ = Resolve<ImageTransferFn>(imageModule_, "NvCVImage_Transfer");

        if (!createEffect_ || !destroyEffect_ || !setU32_ || !setF32_ || !setImage_ ||
            !setString_ || !setCudaStream_ || !load_ || !run_ || !imageAlloc_ ||
            !imageDealloc_ || !imageTransfer_) {
            lastError_ = "NVIDIA Video Effects runtime is missing required entry points";
            UnloadModules();
            return false;
        }
        return true;
    }

    bool Ensure(unsigned int sourceWidth,
                unsigned int sourceHeight,
                unsigned int destinationWidth,
                unsigned int destinationHeight,
                void* cudaStream) {
        if (sourceWidth == 0 || sourceHeight == 0 || destinationWidth == 0 || destinationHeight == 0) {
            lastError_ = "Invalid SuperRes image dimensions";
            return false;
        }
        if (effect_ && sourceWidth_ == sourceWidth && sourceHeight_ == sourceHeight &&
            destinationWidth_ == destinationWidth && destinationHeight_ == destinationHeight &&
            stream_ == cudaStream && loadedStrength_ == strength_) {
            return true;
        }

        TeardownEffect();
        if (!LoadRuntime()) {
            return false;
        }

        std::memset(SourceFloat(), 0, sizeof(NvCVImage));
        std::memset(DestinationFloat(), 0, sizeof(NvCVImage));
        std::memset(TransferTemp(), 0, sizeof(NvCVImage));
        if (createEffect_(NVVFX_FX_SUPER_RES, &effect_) != NVCV_SUCCESS || !effect_) {
            lastError_ = "NVIDIA SuperRes effect could not be created";
            TeardownEffect();
            return false;
        }

        // Models live in the `models` subdirectory of the runtime install
        // (e.g. "...\NVIDIA Video Effects\models"). Prefer that; if no such
        // directory sits beside the DLLs, fall back to the runtime directory
        // itself so relocated installs still have a chance to load.
        std::error_code modelDirError;
        std::filesystem::path modelDirectoryPath =
            std::filesystem::path(runtimeDirectory_) / L"models";
        if (!std::filesystem::is_directory(modelDirectoryPath, modelDirError)) {
            modelDirectoryPath = std::filesystem::path(runtimeDirectory_);
        }
        const std::string modelDirectory = NarrowUtf8(modelDirectoryPath.wstring());
        const auto failSetup = [this](const char* step, NvCV_Status status) {
            lastError_ = std::string("NVIDIA SuperRes ") + step +
                         " failed (code " + std::to_string(static_cast<int>(status)) + ")";
            TeardownEffect();
            return false;
        };
        NvCV_Status status = setString_(effect_, NVVFX_MODEL_DIRECTORY,
                                        modelDirectory.c_str());
        if (status != NVCV_SUCCESS) return failSetup("model directory setup", status);
        status = imageAlloc_(SourceFloat(), sourceWidth, sourceHeight, NVCV_BGR, NVCV_F32,
                             NVCV_PLANAR, NVCV_GPU, 1);
        if (status != NVCV_SUCCESS) return failSetup("input allocation", status);
        status = imageAlloc_(DestinationFloat(), destinationWidth, destinationHeight,
                             NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_GPU, 1);
        if (status != NVCV_SUCCESS) return failSetup("output allocation", status);
        status = setImage_(effect_, NVVFX_INPUT_IMAGE, SourceFloat());
        if (status != NVCV_SUCCESS) return failSetup("input binding", status);
        status = setImage_(effect_, NVVFX_OUTPUT_IMAGE, DestinationFloat());
        if (status != NVCV_SUCCESS) return failSetup("output binding", status);
        status = setCudaStream_(effect_, NVVFX_CUDA_STREAM,
                                reinterpret_cast<CUstream>(cudaStream));
        if (status != NVCV_SUCCESS) return failSetup("CUDA stream binding", status);
        status = setF32_(effect_, NVVFX_STRENGTH, strength_);
        loadedStrength_ = strength_;
        if (status != NVCV_SUCCESS) return failSetup("strength setup", status);
        status = setU32_(effect_, NVVFX_MODE, strength_ >= 0.66f ? 1u : 0u);
        if (status != NVCV_SUCCESS) return failSetup("mode setup", status);
        status = load_(effect_);
        if (status != NVCV_SUCCESS) return failSetup("model initialization", status);

        sourceWidth_ = sourceWidth;
        sourceHeight_ = sourceHeight;
        destinationWidth_ = destinationWidth;
        destinationHeight_ = destinationHeight;
        stream_ = cudaStream;
        lastError_.clear();
        return true;
    }

    static void WrapBgra(NvCVImage* image,
                         const void* pixels,
                         std::size_t pitch,
                         unsigned int width,
                         unsigned int height) {
        std::memset(image, 0, sizeof(*image));
        image->width = width;
        image->height = height;
        image->pitch = static_cast<int>(pitch);
        image->pixelFormat = NVCV_BGRA;
        image->componentType = NVCV_U8;
        image->pixelBytes = 4;
        image->componentBytes = 1;
        image->numComponents = 4;
        image->planar = NVCV_INTERLEAVED;
        image->gpuMem = NVCV_GPU;
        image->pixels = const_cast<void*>(pixels);
        image->bufferBytes = height == 0
                                 ? 0
                                 : pitch * (height - 1u) +
                                       static_cast<std::size_t>(width) * image->pixelBytes;
    }

    bool Run(const void* sourceDevicePixels,
             std::size_t sourcePitchBytes,
             void* destinationDevicePixels,
             std::size_t destinationPitchBytes) {
        if (!effect_ || !sourceDevicePixels || !destinationDevicePixels) {
            lastError_ = "NVIDIA SuperRes is not ready";
            return false;
        }
        if (sourcePitchBytes < static_cast<std::size_t>(sourceWidth_) * 4u ||
            destinationPitchBytes < static_cast<std::size_t>(destinationWidth_) * 4u) {
            lastError_ = "NVIDIA SuperRes image pitch is invalid";
            return false;
        }
        WrapBgra(SourceView(), sourceDevicePixels, sourcePitchBytes, sourceWidth_, sourceHeight_);
        WrapBgra(DestinationView(), destinationDevicePixels, destinationPitchBytes,
                 destinationWidth_, destinationHeight_);
        if (imageTransfer_(SourceView(), SourceFloat(), 1.0f / 255.0f,
                           reinterpret_cast<CUstream>(stream_), TransferTemp()) != NVCV_SUCCESS ||
            // NVIDIA's own SuperRes samples use synchronous execution. Async
            // can expose the previous frame while the output is consumed on
            // the application stream, which looks like a moving ghost layer.
            run_(effect_, 0) != NVCV_SUCCESS ||
            imageTransfer_(DestinationFloat(), DestinationView(), 255.0f,
                           reinterpret_cast<CUstream>(stream_), TransferTemp()) != NVCV_SUCCESS) {
            lastError_ = "NVIDIA SuperRes frame processing failed";
            return false;
        }
        lastError_.clear();
        return true;
    }

    void TeardownEffect() {
        if (imageDealloc_) {
            imageDealloc_(TransferTemp());
            imageDealloc_(DestinationFloat());
            imageDealloc_(SourceFloat());
        }
        std::memset(TransferTemp(), 0, sizeof(NvCVImage));
        std::memset(DestinationFloat(), 0, sizeof(NvCVImage));
        std::memset(SourceFloat(), 0, sizeof(NvCVImage));
        if (effect_ && destroyEffect_) {
            destroyEffect_(effect_);
        }
        effect_ = nullptr;
        sourceWidth_ = sourceHeight_ = destinationWidth_ = destinationHeight_ = 0;
        stream_ = nullptr;
    }

    void UnloadModules() {
        if (videoEffectsModule_) {
            FreeLibrary(videoEffectsModule_);
            videoEffectsModule_ = nullptr;
        }
        if (imageModule_) {
            FreeLibrary(imageModule_);
            imageModule_ = nullptr;
        }
        createEffect_ = nullptr;
        destroyEffect_ = nullptr;
        setU32_ = nullptr;
        setF32_ = nullptr;
        setImage_ = nullptr;
        setString_ = nullptr;
        setCudaStream_ = nullptr;
        load_ = nullptr;
        run_ = nullptr;
        imageAlloc_ = nullptr;
        imageDealloc_ = nullptr;
        imageTransfer_ = nullptr;
    }

    void Teardown() {
        TeardownEffect();
        UnloadModules();
    }

    HMODULE videoEffectsModule_{};
    HMODULE imageModule_{};
    NvVFX_Handle effect_{};
    CreateEffectFn createEffect_{};
    DestroyEffectFn destroyEffect_{};
    SetU32Fn setU32_{};
    SetF32Fn setF32_{};
    SetImageFn setImage_{};
    SetStringFn setString_{};
    SetCudaStreamFn setCudaStream_{};
    LoadFn load_{};
    RunFn run_{};
    ImageAllocFn imageAlloc_{};
    ImageDeallocFn imageDealloc_{};
    ImageTransferFn imageTransfer_{};
    alignas(NvCVImage) std::array<std::byte, sizeof(NvCVImage)> sourceFloat_{};
    alignas(NvCVImage) std::array<std::byte, sizeof(NvCVImage)> destinationFloat_{};
    alignas(NvCVImage) std::array<std::byte, sizeof(NvCVImage)> transferTemp_{};
    alignas(NvCVImage) std::array<std::byte, sizeof(NvCVImage)> sourceView_{};
    alignas(NvCVImage) std::array<std::byte, sizeof(NvCVImage)> destinationView_{};
    unsigned int sourceWidth_{};
    unsigned int sourceHeight_{};
    unsigned int destinationWidth_{};
    unsigned int destinationHeight_{};
    void* stream_{};
    float strength_{0.65f};
    float loadedStrength_{-1.0f};
    std::wstring runtimeDirectory_;
    std::string lastError_;
    bool runtimeProbeAttempted_{};
};

class MaxineSuperRes::Worker {
public:
    Worker() {
        if (qEnvironmentVariableIntValue("OKUFLOW_MAXINE_PROFILE") == 1)
            profile_ = std::make_unique<Profile>();
    }
    ~Worker() {
        if (profile_) profile_->Report();
    }
    DeadlineWorker worker;
    int device{};
    bool Execute(const std::function<bool()>& operation, const char* stage, std::string& error) {
        auto success = std::make_shared<bool>(false);
        auto deviceReady = std::make_shared<bool>(false);
        const auto timing = profile_ ? std::make_shared<CallTiming>() : nullptr;
        const auto started = std::chrono::steady_clock::now();
        const auto result = worker.Execute([operation, success, deviceReady, timing, started, device = device] {
            if (timing) timing->dispatchUs.store(MicrosecondsSince(started));
            *deviceReady = cudaSetDevice(device) == cudaSuccess;
            if (*deviceReady) {
                const auto callStarted = timing ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                *success = operation();
                if (timing) timing->operationUs.store(MicrosecondsSince(callStarted));
            }
        }, std::chrono::milliseconds(1000));
        const auto elapsedUs = MicrosecondsSince(started);
        const auto elapsed = elapsedUs / 1000;
        if (profile_) profile_->Record({stage, result, elapsedUs,
            timing->dispatchUs.load(), timing->operationUs.load(),
            result == DeadlineWorker::Result::Completed && *success});
        if (result != DeadlineWorker::Result::Completed) {
            error = std::string("NVIDIA SuperRes ") + stage +
                " did not complete safely; restart OkuFlow to resume video";
            qWarning() << error.c_str() << "host time" << elapsed << "ms";
            return false;
        }
        if (elapsed >= 50) qInfo() << "NVIDIA SuperRes" << stage << "host time" << elapsed << "ms";
        if (!*deviceReady) error = "NVIDIA SuperRes could not select its CUDA device";
        return *success;
    }
private:
    using Clock = std::chrono::steady_clock;
    static qint64 MicrosecondsSince(Clock::time_point started) {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
    }
    struct CallTiming {
        // A timed-out task may publish these after the caller has returned.
        std::atomic<qint64> dispatchUs{-1}, operationUs{-1};
    };
    struct Sample {
        const char* stage;
        DeadlineWorker::Result outcome;
        qint64 waitUs, dispatchUs, operationUs;
        bool success;
    };
    struct Profile {
        static constexpr size_t Capacity = 16384;
        std::vector<Sample> samples;
        size_t calls{};
        Profile() { samples.reserve(Capacity); }
        void Record(Sample sample) {
            if (samples.size() < Capacity) samples.push_back(sample);
            else samples[calls % Capacity] = sample;
            ++calls;
        }
        static QJsonObject Distribution(std::vector<qint64> values) {
            if (values.empty()) return {{"count", 0}};
            std::sort(values.begin(), values.end());
            const auto rank = [&](double p) {
                return values[static_cast<size_t>(std::ceil(p * values.size())) - 1];
            };
            return {{"count", static_cast<qint64>(values.size())},
                {"p50_us", rank(.50)}, {"p95_us", rank(.95)},
                {"p99_us", rank(.99)}, {"max_us", values.back()}};
        }
        void Report() const {
            QJsonObject stages;
            for (const char* stage : {"model load", "frame processing", "teardown"}) {
                std::vector<qint64> wait, dispatch, operation;
                int completed = 0, timedOut = 0, rejected = 0, threw = 0, succeeded = 0;
                for (const auto& sample : samples) {
                    if (std::string_view(sample.stage) != stage) continue;
                    wait.push_back(sample.waitUs);
                    if (sample.dispatchUs >= 0) dispatch.push_back(sample.dispatchUs);
                    // Timed-out calls are censored, not successful operation samples.
                    if (sample.outcome == DeadlineWorker::Result::Completed && sample.operationUs >= 0)
                        operation.push_back(sample.operationUs);
                    completed += sample.outcome == DeadlineWorker::Result::Completed;
                    timedOut += sample.outcome == DeadlineWorker::Result::TimedOut;
                    rejected += sample.outcome == DeadlineWorker::Result::Rejected;
                    threw += sample.outcome == DeadlineWorker::Result::Threw;
                    succeeded += sample.success;
                }
                stages.insert(QString::fromLatin1(stage), QJsonObject{
                    {"completed", completed}, {"timed_out", timedOut}, {"rejected", rejected},
                    {"threw", threw}, {"sdk_success", succeeded},
                    {"caller_wait", Distribution(std::move(wait))},
                    {"worker_dispatch_delay", Distribution(std::move(dispatch))},
                    {"completed_sdk_host_call", Distribution(std::move(operation))}});
            }
            const QJsonObject report{{"schema", 1}, {"scope", "host-calls; not-gpu-completion-or-pan-to-present"},
                {"total_calls", static_cast<qint64>(calls)},
                {"retained_last_calls", static_cast<qint64>(samples.size())},
                {"omitted_earlier_calls", static_cast<qint64>(calls - samples.size())},
                {"stage_statistics_scope", "retained_last_calls"}, {"stages", stages}};
            qInfo().noquote() << "OKUFLOW_MAXINE_PROFILE" << QJsonDocument(report).toJson(QJsonDocument::Compact);
        }
    };
    std::unique_ptr<Profile> profile_;
};

MaxineSuperRes::MaxineSuperRes() : impl_(std::make_shared<Impl>()), worker_(std::make_unique<Worker>()) {
    if (cudaGetDevice(&worker_->device) != cudaSuccess)
        workerError_ = "NVIDIA SuperRes could not determine the CUDA device";
}
MaxineSuperRes::~MaxineSuperRes() {
    if (!IsFaulted()) Teardown();
    if (IsFaulted()) {
        // A timed-out SDK host call may still use its stream and buffers. The
        // surface quarantines borrowed allocations; retain SDK ownership too.
        (void)new std::shared_ptr<Impl>(std::move(impl_));
    }
}

bool MaxineSuperRes::Ensure(unsigned int sourceWidth,
                            unsigned int sourceHeight,
                            unsigned int destinationWidth,
                            unsigned int destinationHeight,
                            void* cudaStream) {
    if (IsFaulted()) return false;
    // Avoid dispatch for an unchanged configuration; the previous call has
    // completed before any Impl state is read from the caller thread.
    if (impl_->effect_ && impl_->sourceWidth_ == sourceWidth && impl_->sourceHeight_ == sourceHeight &&
        impl_->destinationWidth_ == destinationWidth && impl_->destinationHeight_ == destinationHeight &&
        impl_->stream_ == cudaStream && impl_->loadedStrength_ == impl_->strength_) return true;
    qInfo() << "NVIDIA SuperRes loading" << sourceWidth << "x" << sourceHeight
            << "to" << destinationWidth << "x" << destinationHeight;
    return worker_->Execute([owner = impl_, sourceWidth, sourceHeight, destinationWidth, destinationHeight, cudaStream] {
        return owner->Ensure(sourceWidth, sourceHeight, destinationWidth, destinationHeight, cudaStream);
    }, "model load", workerError_);
}

bool MaxineSuperRes::Ensure(int sourceWidth, int sourceHeight, int scale, void* cudaStream) {
    if (sourceWidth <= 0 || sourceHeight <= 0 || scale <= 0) {
        return false;
    }
    return Ensure(static_cast<unsigned int>(sourceWidth),
                  static_cast<unsigned int>(sourceHeight),
                  static_cast<unsigned int>(sourceWidth * scale),
                  static_cast<unsigned int>(sourceHeight * scale), cudaStream);
}

bool MaxineSuperRes::Run(const void* sourceDevicePixels,
                         std::size_t sourcePitchBytes,
                         void* destinationDevicePixels,
                         std::size_t destinationPitchBytes) {
    if (IsFaulted()) return false;
    return worker_->Execute([owner = impl_, sourceDevicePixels, sourcePitchBytes,
                            destinationDevicePixels, destinationPitchBytes] {
        return owner->Run(sourceDevicePixels, sourcePitchBytes, destinationDevicePixels, destinationPitchBytes);
    }, "frame processing", workerError_);
}

void MaxineSuperRes::SetStrength(float strength) {
    if (!IsFaulted()) impl_->strength_ = std::clamp(strength, 0.0f, 1.0f);
}

void MaxineSuperRes::Teardown() {
    if (!IsFaulted()) worker_->Execute([owner = impl_] { owner->Teardown(); return true; }, "teardown", workerError_);
}
bool MaxineSuperRes::IsReady() const { return !IsFaulted() && impl_->effect_ != nullptr; }
bool MaxineSuperRes::IsFaulted() const { return !workerError_.empty(); }
const std::string& MaxineSuperRes::LastError() const { return IsFaulted() ? workerError_ : impl_->lastError_; }
const std::wstring& MaxineSuperRes::RuntimeDirectory() const {
    static const std::wstring unavailable;
    return IsFaulted() ? unavailable : impl_->runtimeDirectory_;
}

std::wstring MaxineSuperRes::FindRuntimeDirectory(const std::wstring& overrideDirectory) {
    const std::array<std::wstring, 3> explicitCandidates{
        overrideDirectory,
        ReadEnvironment(L"OKUFLOW_MAXINE_PATH"),
        ReadEnvironment(L"NV_VIDEO_EFFECTS_PATH"),
    };
    for (const std::wstring& candidate : explicitCandidates) {
        if (!candidate.empty() && candidate != L"USE_APP_PATH" && HasRuntimeLibraries(candidate)) {
            return candidate;
        }
    }

    const std::wstring programFiles = ReadEnvironment(L"ProgramFiles");
    if (!programFiles.empty()) {
        const std::wstring candidate =
            (std::filesystem::path(programFiles) / kRuntimeRelativePath).wstring();
        if (HasRuntimeLibraries(candidate)) {
            return candidate;
        }
    }
    std::wstring registryPath = FindRuntimeInUninstallRegistry(KEY_WOW64_64KEY);
    if (registryPath.empty()) {
        registryPath = FindRuntimeInUninstallRegistry(KEY_WOW64_32KEY);
    }
    return registryPath;
}

bool MaxineSuperRes::IsRuntimeInstalled(const std::wstring& overrideDirectory) {
    return !FindRuntimeDirectory(overrideDirectory).empty();
}

} // namespace okuflow

#else

namespace okuflow {

class MaxineSuperRes::Impl {
public:
    std::string error{"NVIDIA SuperRes requires the Windows CUDA build"};
    std::wstring runtimeDirectory;
};
class MaxineSuperRes::Worker {};

MaxineSuperRes::MaxineSuperRes() : impl_(std::make_unique<Impl>()) {}
MaxineSuperRes::~MaxineSuperRes() = default;
bool MaxineSuperRes::Ensure(unsigned int, unsigned int, unsigned int, unsigned int, void*) { return false; }
bool MaxineSuperRes::Ensure(int, int, int, void*) { return false; }
bool MaxineSuperRes::Run(const void*, std::size_t, void*, std::size_t) { return false; }
void MaxineSuperRes::SetStrength(float) {}
void MaxineSuperRes::Teardown() {}
bool MaxineSuperRes::IsReady() const { return false; }
bool MaxineSuperRes::IsFaulted() const { return false; }
const std::string& MaxineSuperRes::LastError() const { return impl_->error; }
const std::wstring& MaxineSuperRes::RuntimeDirectory() const { return impl_->runtimeDirectory; }
std::wstring MaxineSuperRes::FindRuntimeDirectory(const std::wstring&) { return {}; }
bool MaxineSuperRes::IsRuntimeInstalled(const std::wstring&) { return false; }

} // namespace okuflow

#endif
