#include "okuflow/cuda/cuda_interop.hpp"
#include "okuflow/common/maxine_superres.hpp"
#include "maxine_fake_sdk.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <dxgi1_4.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT kExtent = 512;
constexpr unsigned int kNeuralBgra = 0xff4bb123u;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Hr(HRESULT result, const char* message) { Require(SUCCEEDED(result), message); }
void Cuda(cudaError_t result) {
    if (result != cudaSuccess) throw std::runtime_error(cudaGetErrorString(result));
}
struct FakeModule {
    HMODULE handle{};
    MaxineFakeRead read{};
    MaxineFakeReset reset{};
    MaxineFakeFillOutput fill{};
    explicit FakeModule(const QString& path) {
        handle = LoadLibraryExW(path.toStdWString().c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(handle != nullptr, "private fake DLL could not load");
        read = reinterpret_cast<MaxineFakeRead>(GetProcAddress(handle, "OkuFlowFakeRead"));
        reset = reinterpret_cast<MaxineFakeReset>(GetProcAddress(handle, "OkuFlowFakeReset"));
        fill = reinterpret_cast<MaxineFakeFillOutput>(GetProcAddress(handle, "OkuFlowFakeFillOutput"));
        Require(read && reset && fill, "private fake control exports absent");
        reset();
    }
    ~FakeModule() { if (handle) FreeLibrary(handle); }
    MaxineFakeSnapshot Stats() const { MaxineFakeSnapshot result; read(&result); return result; }
};

// All textures, queues and host buffers are privately owned. Unknown GPU
// completion retains their complete closure; it never authorizes release.
class Harness {
public:
    Harness() {
        Require(cuInit(0) == CUDA_SUCCESS, "CUDA driver initialization failed");
        CUdevice cudaDevice{};
        Require(cuDeviceGet(&cudaDevice, 0) == CUDA_SUCCESS, "CUDA device lookup failed");
        LUID luid{};
        unsigned int nodeMask{};
        Require(cuDeviceGetLuid(reinterpret_cast<char*>(&luid), &nodeMask, cudaDevice) == CUDA_SUCCESS,
                "CUDA device has no D3D adapter identity");
        ComPtr<IDXGIFactory4> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory creation failed");
        ComPtr<IDXGIAdapter1> adapter;
        Hr(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "matching CUDA/D3D adapter absent");
        DXGI_ADAPTER_DESC1 adapterDesc{};
        Hr(adapter->GetDesc1(&adapterDesc), "adapter description failed");
        Require(adapterDesc.VendorId == 0x10de && !(adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE),
                "matching adapter is not NVIDIA hardware");
        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
           "D3D12 device creation failed");
        scene_ = SharedTexture();
        sr_ = SharedTexture();
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)), "copy queue creation failed");
        Hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)),
           "copy allocator creation failed");
        Hr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                      IID_PPV_ARGS(&commands_)), "copy command list creation failed");
        Hr(commands_->Close(), "initial copy list close failed");
        Hr(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_)), "readback fence creation failed");
        completed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(completed_ != nullptr, "readback completion event creation failed");
        const auto desc = sr_->GetDesc();
        UINT64 bytes{};
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint_, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)), "readback buffer creation failed");
        surface = std::make_unique<okuflow::CudaInteropSurface>(scene_.Get(), sr_.Get(), fence_.Get());
        Require(surface->IsValid(), surface->LastError().c_str());
        Require(surface->HasExternalSemaphore(), "fixture did not import its private graphics fence");
    }
    ~Harness() {
        if (pending_) {
            (void)surface.release();
            device_.Detach(); scene_.Detach(); sr_.Detach(); queue_.Detach(); allocator_.Detach();
            commands_.Detach(); fence_.Detach(); readback_.Detach();
            // The queued completion may still reference this event.
            completed_ = nullptr;
        } else surface.reset();
        if (completed_) CloseHandle(completed_);
    }
    void Frame(unsigned char value, const okuflow::ProcessingSettings& settings) {
        pixels_.assign(kExtent * kExtent * 4, value);
        for (size_t i = 3; i < pixels_.size(); i += 4) pixels_[i] = 255;
        okuflow::ProcessingInput input;
        input.hostPixels = pixels_.data();
        input.hostStrideBytes = kExtent * 4;
        input.pixelSizeBytes = 4;
        input.width = input.height = kExtent;
        const bool processed = surface->ProcessFrame(input, settings, {});
        if (!processed) throw std::runtime_error(surface->LastError());
        Require(surface->WaitForIdle() && !surface->IsFaulted(), "CUDA frame did not retire safely");
    }
    std::vector<unsigned char> Read(bool spatialCache) {
        Hr(allocator_->Reset(), "copy allocator reset failed");
        Hr(commands_->Reset(allocator_.Get(), nullptr), "copy command list reset failed");
        auto* texture = spatialCache ? sr_.Get() : scene_.Get();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands_->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = texture;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback_.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint_;
        commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands_->ResourceBarrier(1, &barrier);
        Hr(commands_->Close(), "copy command list close failed");
        ID3D12CommandList* lists[]{commands_.Get()};
        pending_ = true;
        Hr(queue_->Wait(fence_.Get(), fenceValue_), "graphics reader dependency failed");
        queue_->ExecuteCommandLists(1, lists);
        Hr(queue_->Signal(fence_.Get(), ++fenceValue_), "readback fence signal failed");
        Hr(fence_->SetEventOnCompletion(fenceValue_, completed_), "readback fence event failed");
        Require(WaitForSingleObject(completed_, 3000) == WAIT_OBJECT_0 &&
                fence_->GetCompletedValue() >= fenceValue_, "readback GPU deadline failed");
        pending_ = false;
        unsigned char* mapped{};
        const D3D12_RANGE readRange{static_cast<SIZE_T>(footprint_.Offset),
            static_cast<SIZE_T>(footprint_.Offset + footprint_.Footprint.RowPitch * kExtent)};
        Hr(readback_->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "readback map failed");
        std::vector<unsigned char> result(kExtent * kExtent * 4);
        for (UINT row = 0; row < kExtent; ++row)
            std::memcpy(result.data() + row * kExtent * 4,
                mapped + footprint_.Offset + row * footprint_.Footprint.RowPitch, kExtent * 4);
        const D3D12_RANGE noWrites{0, 0};
        readback_->Unmap(0, &noWrites);
        return result;
    }
    void AwaitNeural() {
        const auto deadline = GetTickCount64() + 2500;
        while (!surface->IsSuperResActive() && GetTickCount64() < deadline) {
            const auto result = surface->RefreshSuperRes({true, fenceValue_, fenceValue_ + 1});
            if (result == okuflow::SuperResRefreshResult::Submitted || result == okuflow::SuperResRefreshResult::Published)
                ++fenceValue_;
            Require(result != okuflow::SuperResRefreshResult::Failed, "private output publication failed");
            if (result != okuflow::SuperResRefreshResult::NoWork)
                Require(surface->WaitForIdle(), "private consumer copy failed to retire");
            Sleep(1);
        }
        Require(surface->IsSuperResActive(), "asynchronous result never published for the exact current scene");
    }
    std::unique_ptr<okuflow::CudaInteropSurface> surface;
private:
    ComPtr<ID3D12Resource> SharedTexture() {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = kExtent;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        ComPtr<ID3D12Resource> texture;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)), "shareable BGRA texture creation failed");
        return texture;
    }
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12Resource> scene_, sr_, readback_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12Fence> fence_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    HANDLE completed_{};
    UINT64 fenceValue_{};
    bool pending_{};
    std::vector<unsigned char> pixels_;
};

void CheckPixels(const std::vector<unsigned char>& pixels, unsigned int expected, int tolerance) {
    Require(pixels.size() == kExtent * kExtent * 4, "wrong readback extent");
    for (size_t i = 0; i < pixels.size(); ++i) {
        const int channel = static_cast<int>((expected >> ((i % 4) * 8)) & 0xff);
        Require(std::abs(static_cast<int>(pixels[i]) - channel) <= (i % 4 == 3 ? 0 : tolerance),
                "readback contains stale, uninitialized or wrong-frame pixels");
    }
}
unsigned int Gray(unsigned char value) { return 0xff000000u | value * 0x010101u; }
void CheckFallback(Harness& harness, unsigned char value, std::uint64_t previousGeneration) {
    const auto roi = harness.surface->SuperResRoi();
    Require(!harness.surface->IsSuperResActive() && roi.valid && roi.generation > previousGeneration,
            "current-frame spatial fallback metadata missing");
    Require(roi.outputWidth == kExtent && roi.outputHeight == kExtent, "spatial fallback extent differs");
    CheckPixels(harness.Read(false), Gray(value), 0);
    CheckPixels(harness.Read(true), Gray(value), 2);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        int devices{};
        const auto availability = cudaGetDeviceCount(&devices);
        if (availability == cudaErrorNoDevice || (availability == cudaSuccess && devices == 0)) return 77;
        Cuda(availability);
        const QString imagePath = QString::fromUtf8(OKUFLOW_MAXINE_FAKE_IMAGE);
        const QString effectsPath = QString::fromUtf8(OKUFLOW_MAXINE_FAKE_EFFECTS);
        const auto directory = QFileInfo(imagePath).absolutePath();
        Require(directory == QFileInfo(effectsPath).absolutePath(), "private fake runtime DLLs separated");
        qputenv("OKUFLOW_MAXINE_PATH", directory.toUtf8());
        FakeModule image(imagePath), effects(effectsPath);
        image.fill(true, kNeuralBgra);
        Require(QDir(QString::fromStdWString(okuflow::MaxineSuperRes::FindRuntimeDirectory())) == QDir(directory),
                "adapter did not select private fake runtime");
        {
            Harness harness;
            okuflow::ProcessingSettings settings;
            settings.enableMlSuperRes = true;
            settings.zoomAmount = 2.0f;
            settings.enableSpatialSharpen = true;
            settings.spatialSharpness = 0.0f;
            settings.spatialViewportWidth = settings.spatialViewportHeight = kExtent;
            settings.spatialViewTransform = okuflow::ComputeViewTransform(kExtent, kExtent, kExtent, kExtent,
                2.0f, 0.5f, 0.5f, okuflow::ViewportFitMode::kFill);
            settings.deferMlSuperResForInteraction = true;
            harness.Frame(37, settings);
            CheckFallback(harness, 37, 0);
            Require(effects.Stats().created == 0 && effects.Stats().loaded == 0 && effects.Stats().ran == 0 &&
                    image.Stats().allocated == 0, "navigation admitted SDK initialization or inference");

            settings.deferMlSuperResForInteraction = false;
            harness.Frame(71, settings);
            harness.AwaitNeural();
            Require(harness.surface->IsSuperResActive() && harness.surface->SuperResRoi().valid,
                    "idle scene did not activate the neural route");
            Require(effects.Stats().created == 1 && effects.Stats().loaded == 1 && effects.Stats().ran == 1,
                    "idle scene did not load/run exactly once");
            CheckPixels(harness.Read(true), kNeuralBgra, 0);
            auto generation = harness.surface->SuperResRoi().generation;
            harness.surface->InvalidateSuperResRequest(); // native edit, before another frame
            Require(!harness.surface->IsSuperResActive() && !harness.surface->SuperResRoi().valid &&
                    !harness.surface->HasSuperResWork() && !harness.surface->NeedsSuperResPolling(),
                    "native settings edit left old neural publication authority alive");
            Require(effects.Stats().loaded == 1 && effects.Stats().destroyed == 0,
                    "request invalidation retired the healthy private model");

            settings.deferMlSuperResForInteraction = true;
            harness.Frame(181, settings);
            CheckFallback(harness, 181, generation);
            Require(effects.Stats().ran == 1 && effects.Stats().loaded == 1 && effects.Stats().destroyed == 0 &&
                    image.Stats().allocated == 2 && image.Stats().deallocated == 0,
                    "navigation ran SDK or retired its ready model");
            generation = harness.surface->SuperResRoi().generation;

            settings.enableMlSuperRes = false;
            settings.deferMlSuperResForInteraction = false;
            harness.Frame(211, settings);
            CheckFallback(harness, 211, generation);
            Require(effects.Stats().ran == 1 && effects.Stats().destroyed == 0, "disabled SR ran or retired SDK");

            settings.enableMlSuperRes = true;
            harness.Frame(97, settings);
            harness.AwaitNeural();
            Require(harness.surface->IsSuperResActive() && effects.Stats().created == 1 &&
                    effects.Stats().loaded == 1 && effects.Stats().ran == 2 && image.Stats().allocated == 2,
                    "resuming SR replaced the retained model or failed to run");
            CheckPixels(harness.Read(true), kNeuralBgra, 0);
        }
        const auto cleanupDeadline = GetTickCount64() + 2500;
        while ((effects.Stats().created != effects.Stats().destroyed || image.Stats().allocated != image.Stats().deallocated) &&
               GetTickCount64() < cleanupDeadline) Sleep(1);
        Require(effects.Stats().created == effects.Stats().destroyed && image.Stats().allocated == image.Stats().deallocated,
                "asynchronous teardown did not retire private SDK ownership");
        std::puts("PASS: actual CUDA surface defers SDK for navigation, publishes current-frame fallback, and reuses ready model");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
