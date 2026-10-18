// Window-free GPU ownership fixture for MediaGpuScene.
//
// Creates private D3D12/D3D11 devices on the exact CUDA adapter (matched by
// LUID), feeds owned NT-shareable BGRA D3D11 textures and padded CPU BGRA rows
// through the real MediaGpuScene::Prepare/Process async poll contract, and
// consumes the private producer fence from a private D3D12 command queue
// that copies the scene texture into private readback staging. No window,
// presenter, camera, browser, or installed Maxine runtime is involved; the
// Maxine runtime search path is pinned to the private fake SDK directory
// guard so an owner-installed runtime can never be probed.
//
// Exit code 77 is reserved for missing hardware prerequisites only (no CUDA
// device, no adapter matching the CUDA LUID, non-NVIDIA/software adapter).
// Once CUDA hardware is present, every failure is a real failure.
#include "okuflow/app/media_gpu_scene.hpp"
#include "okuflow/common/uct_device_image.hpp"
#include "maxine_fake_sdk.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Status = okuflow::MediaGpuScene::Status;

constexpr UINT kWidth = 64;
constexpr UINT kHeight = 48;
constexpr auto kPollStep = std::chrono::milliseconds(1);

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }
void Require(bool condition, const char* message)
{
    if (!condition) Fail(message);
}
void Hr(HRESULT result, const char* message) { Require(SUCCEEDED(result), message); }
void Cuda(cudaError_t result, const char* message)
{
    if (result != cudaSuccess)
        Fail(std::string(message) + ": " + cudaGetErrorString(result));
}

struct Bgra {
    std::uint8_t b{}, g{}, r{}, a{255};
};
bool operator==(const Bgra& lhs, const Bgra& rhs)
{
    return lhs.b == rhs.b && lhs.g == rhs.g && lhs.r == rhs.r && lhs.a == rhs.a;
}

// Deterministic position-coded pattern: every pixel is distinguishable and
// channel values sweep both sides of the black/white threshold.
Bgra PatternPixel(UINT x, UINT y, unsigned seed)
{
    return Bgra{static_cast<std::uint8_t>((x * 4 + seed * 29) % 256),
                static_cast<std::uint8_t>((y * 5 + seed * 11) % 256),
                static_cast<std::uint8_t>(((x + y) * 2 + seed * 7) % 256), 255};
}
std::vector<Bgra> Pattern(UINT width, UINT height, unsigned seed)
{
    std::vector<Bgra> pixels(static_cast<size_t>(width) * height);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
            pixels[static_cast<size_t>(y) * width + x] = PatternPixel(x, y, seed);
    return pixels;
}

// Host-side reference for the documented kernel arithmetic (truncating
// float-to-byte conversion, Rec.601-style luma weights used by the kernels).
std::uint8_t FloatToByte(float value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}
std::uint8_t LumaByte(const Bgra& pixel)
{
    return FloatToByte(0.299f * (pixel.r / 255.0f) + 0.587f * (pixel.g / 255.0f) +
                       0.114f * (pixel.b / 255.0f));
}
Bgra RotatePixel(const std::vector<Bgra>& source, UINT srcWidth, UINT srcHeight, int turns,
                 UINT ox, UINT oy)
{
    UINT ix = ox, iy = oy;
    switch (turns) {
    case 1: ix = oy; iy = srcHeight - 1 - ox; break;             // 90 CW
    case 2: ix = srcWidth - 1 - ox; iy = srcHeight - 1 - oy; break;
    case 3: ix = srcWidth - 1 - oy; iy = ox; break;              // 270 CW
    default: break;
    }
    return source[static_cast<size_t>(iy) * srcWidth + ix];
}

void ComparePixels(const std::vector<Bgra>& actual, const std::vector<Bgra>& expected,
                   UINT width, int tolerance, const char* what)
{
    Require(actual.size() == expected.size(), "readback extent differs from expectation");
    for (size_t i = 0; i < actual.size(); ++i) {
        const int db = std::abs(int(actual[i].b) - int(expected[i].b));
        const int dg = std::abs(int(actual[i].g) - int(expected[i].g));
        const int dr = std::abs(int(actual[i].r) - int(expected[i].r));
        if (db > tolerance || dg > tolerance || dr > tolerance || actual[i].a != 255) {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                          "%s: pixel (%u,%u) actual BGRA(%u,%u,%u,%u) expected (%u,%u,%u,255)",
                          what, unsigned(i % width), unsigned(i / width), actual[i].b,
                          actual[i].g, actual[i].r, actual[i].a, expected[i].b, expected[i].g,
                          expected[i].r);
            Fail(buffer);
        }
    }
}

// Owned decoder-style source storage. The shared_ptr<void> lease in the
// published GpuImage is the only thing that may keep this alive after the
// fixture drops its own reference, which is how lease retention is proven.
struct SourceStorage {
    ComPtr<ID3D11Texture2D> texture;
};

class Harness {
public:
    Harness()
    {
        Require(cuInit(0) == CUDA_SUCCESS, "CUDA driver initialization failed");
        ComPtr<IDXGIFactory4> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory creation failed");

        // Exact adapter identity: enumerate CUDA devices, keep the ordinal
        // whose LUID resolves to a real NVIDIA DXGI adapter.
        int deviceCount = 0;
        Cuda(cudaGetDeviceCount(&deviceCount), "CUDA device enumeration failed");
        ComPtr<IDXGIAdapter1> adapter;
        for (int ordinal = 0; ordinal < deviceCount && !adapter; ++ordinal) {
            CUdevice device{};
            if (cuDeviceGet(&device, ordinal) != CUDA_SUCCESS) continue;
            LUID luid{};
            unsigned int nodeMask{};
            if (cuDeviceGetLuid(reinterpret_cast<char*>(&luid), &nodeMask, device) != CUDA_SUCCESS)
                continue;
            ComPtr<IDXGIAdapter1> candidate;
            if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&candidate)))) continue;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) continue;
            if (desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            adapter = candidate;
            cudaDevice_ = ordinal;
            adapterLuid_ = desc.AdapterLuid;
        }
        if (!adapter) std::exit(77); // no CUDA device with a matching D3D adapter

        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12_)),
           "D3D12 device creation on the CUDA adapter failed");
        const LUID d3d12Luid = d3d12_->GetAdapterLuid();
        Require(d3d12Luid.HighPart == adapterLuid_.HighPart &&
                    d3d12Luid.LowPart == adapterLuid_.LowPart,
                "D3D12 device is not on the CUDA adapter");

        Hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                             nullptr, 0, D3D11_SDK_VERSION, &d3d11_, nullptr, &d3d11Context_),
           "D3D11 device creation on the CUDA adapter failed");
        ComPtr<IDXGIDevice> dxgiDevice;
        Hr(d3d11_.As(&dxgiDevice), "D3D11 device does not expose IDXGIDevice");
        ComPtr<IDXGIAdapter> d3d11Adapter;
        Hr(dxgiDevice->GetAdapter(&d3d11Adapter), "D3D11 adapter lookup failed");
        DXGI_ADAPTER_DESC d3d11Desc{};
        Hr(d3d11Adapter->GetDesc(&d3d11Desc), "D3D11 adapter description failed");
        Require(d3d11Desc.AdapterLuid.HighPart == adapterLuid_.HighPart &&
                    d3d11Desc.AdapterLuid.LowPart == adapterLuid_.LowPart,
                "D3D11 device is not on the CUDA adapter");

        // Private consumer queue: stands in for the presenter's read signal.
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(d3d12_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)),
           "consumer queue creation failed");
        Hr(d3d12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&allocator_)),
           "consumer allocator creation failed");
        Hr(d3d12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(),
                                      nullptr, IID_PPV_ARGS(&commands_)),
           "consumer command list creation failed");
        Hr(commands_->Close(), "initial consumer list close failed");
        completed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Require(completed_ != nullptr, "consumer completion event creation failed");

        // One readback staging buffer sized for the largest scene used here.
        D3D12_RESOURCE_DESC maxDesc{};
        maxDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        maxDesc.Width = maxDesc.Height = 512;
        maxDesc.DepthOrArraySize = maxDesc.MipLevels = 1;
        maxDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        maxDesc.SampleDesc.Count = 1;
        UINT64 stagingBytes = 0;
        d3d12_->GetCopyableFootprints(&maxDesc, 0, 1, 0, nullptr, nullptr, nullptr, &stagingBytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = stagingBytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(d3d12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&readback_)),
           "readback staging creation failed");
    }
    ~Harness()
    {
        if (pending_) {
            // Unknown GPU completion: leak the whole closure rather than
            // releasing resources a queued copy may still reference.
            d3d12_.Detach(); d3d11_.Detach(); d3d11Context_.Detach();
            queue_.Detach(); allocator_.Detach(); commands_.Detach(); readback_.Detach();
            completed_ = nullptr;
        }
        if (completed_) CloseHandle(completed_);
    }

    ID3D12Device* Device() const { return d3d12_.Get(); }
    int CudaDevice() const { return cudaDevice_; }

    // Fresh owned NT-shareable BGRA source filled with known pixels; the
    // D3D11 fill is proven complete with an event query before publication.
    okuflow::VideoPlaybackFrame MakeFrame(UINT width, UINT height, unsigned seed,
                                           std::uint64_t sampleId,
                                           std::weak_ptr<SourceStorage>* leaseProbe = nullptr)
    {
        auto storage = std::make_shared<SourceStorage>();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        Hr(d3d11_->CreateTexture2D(&desc, nullptr, &storage->texture),
           "shareable BGRA source creation failed");

        ComPtr<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.MiscFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Hr(d3d11_->CreateTexture2D(&stagingDesc, nullptr, &staging),
           "source staging creation failed");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Hr(d3d11Context_->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped),
           "source staging map failed");
        const auto pixels = Pattern(width, height, seed);
        for (UINT y = 0; y < height; ++y)
            std::memcpy(static_cast<std::uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        pixels.data() + size_t(y) * width, size_t(width) * sizeof(Bgra));
        d3d11Context_->Unmap(staging.Get(), 0);
        d3d11Context_->CopyResource(storage->texture.Get(), staging.Get());
        ComPtr<ID3D11Query> query;
        D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
        Hr(d3d11_->CreateQuery(&queryDesc, &query), "source fill query creation failed");
        d3d11Context_->End(query.Get());
        d3d11Context_->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (;;) {
            BOOL done = FALSE;
            const HRESULT hr = d3d11Context_->GetData(query.Get(), &done, sizeof(done), 0);
            if (hr == S_OK && done) break;
            Require(hr == S_OK || hr == S_FALSE, "source fill completion query failed");
            Require(std::chrono::steady_clock::now() < deadline, "source fill timed out");
            Sleep(1);
        }

        auto image = std::make_shared<okuflow::VideoPlaybackGpuImage>();
        image->texture = storage->texture.Get();
        image->storage = storage;
        image->bytes = size_t(width) * height * 4;
        if (leaseProbe) *leaseProbe = storage;

        okuflow::VideoPlaybackFrame frame;
        frame.gpu = image;
        frame.width = width;
        frame.height = height;
        frame.strideBytes = width * 4;
        frame.sampleId = sampleId;
        frame.pts100ns = static_cast<std::int64_t>(sampleId) * 333333;
        return frame;
    }

    // Queue a real graphics-completion signal, deferred behind an owned
    // fixture fence. Production RecordPresented only sees a queued signal.
    ComPtr<ID3D12Fence> BlockConsumer(okuflow::MediaGpuScene& scene)
    {
        ComPtr<ID3D12Fence> gate;
        Hr(d3d12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "consumer gate creation failed");
        const auto sync = scene.PresentationSync();
        Require(sync.enable && sync.presentationFence, "consumer gate needs a ready scene");
        Hr(queue_->Wait(gate.Get(), 1), "consumer gate wait failed");
        Hr(queue_->Wait(sync.presentationFence, sync.waitValue), "consumer producer wait failed");
        Hr(queue_->Signal(sync.presentationFence, sync.signalValue), "deferred consumer signal failed");
        scene.RecordPresented(sync.signalValue);
        return gate;
    }

    struct ReadResult {
        std::vector<Bgra> pixels;
        std::uint64_t waitValue{};
        std::uint64_t signalValue{};
    };
    // One view-only consumer read: wait the private producer fence on the
    // private queue, copy scene -> staging, signal the reserved value, then
    // record it exactly as a successful presentation would.
    ReadResult ReadScene(okuflow::MediaGpuScene& scene, bool recordByValue = false, bool cache = false)
    {
        ID3D12Resource* texture = cache ? scene.SuperResTexture() : scene.Texture();
        Require(texture != nullptr, "scene texture missing for consumer read");
        const auto sync = scene.PresentationSync();
        Require(sync.enable && sync.presentationFence && sync.signalValue != 0,
                "presentation sync missing for a ready scene");

        const auto desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        d3d12_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);

        Hr(allocator_->Reset(), "consumer allocator reset failed");
        Hr(commands_->Reset(allocator_.Get(), nullptr), "consumer list reset failed");
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
        destination.PlacedFootprint = footprint;
        commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands_->ResourceBarrier(1, &barrier);
        Hr(commands_->Close(), "consumer list close failed");

        ID3D12CommandList* lists[]{commands_.Get()};
        pending_ = true;
        Hr(queue_->Wait(sync.presentationFence, sync.waitValue),
           "consumer wait on the private producer fence failed");
        queue_->ExecuteCommandLists(1, lists);
        Hr(queue_->Signal(sync.presentationFence, sync.signalValue),
           "consumer signal on the private producer fence failed");
        if (recordByValue) scene.RecordPresented(sync.signalValue);
        else scene.RecordPresented();
        Hr(sync.presentationFence->SetEventOnCompletion(sync.signalValue, completed_),
           "consumer completion event failed");
        Require(WaitForSingleObject(completed_, 3000) == WAIT_OBJECT_0 &&
                    sync.presentationFence->GetCompletedValue() >= sync.signalValue &&
                    sync.presentationFence->GetCompletedValue() != UINT64_MAX,
                "consumer copy did not complete on the private fence");
        pending_ = false;

        ReadResult result;
        result.waitValue = sync.waitValue;
        result.signalValue = sync.signalValue;
        result.pixels.resize(size_t(desc.Width) * desc.Height);
        std::uint8_t* mapped = nullptr;
        const D3D12_RANGE range{static_cast<SIZE_T>(footprint.Offset),
                                static_cast<SIZE_T>(footprint.Offset +
                                                    footprint.Footprint.RowPitch * desc.Height)};
        Hr(readback_->Map(0, &range, reinterpret_cast<void**>(&mapped)), "staging map failed");
        for (UINT64 y = 0; y < desc.Height; ++y)
            std::memcpy(result.pixels.data() + size_t(y) * desc.Width,
                        mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                        size_t(desc.Width) * sizeof(Bgra));
        const D3D12_RANGE noWrites{0, 0};
        readback_->Unmap(0, &noWrites);
        return result;
    }

private:
    ComPtr<ID3D12Device> d3d12_;
    ComPtr<ID3D11Device> d3d11_;
    ComPtr<ID3D11DeviceContext> d3d11Context_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12Resource> readback_;
    HANDLE completed_{};
    LUID adapterLuid_{};
    int cudaDevice_{-1};
    bool pending_{};
};

Status PrepareUntilReady(okuflow::MediaGpuScene& scene, ID3D12Device* device, UINT width,
                         UINT height, std::chrono::seconds budget = std::chrono::seconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        const auto started = std::chrono::steady_clock::now();
        const Status status = scene.Prepare(device, width, height);
        Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
                "Prepare blocked the calling thread");
        if (status == Status::Ready) return status;
        if (status == Status::Failed)
            Fail("Prepare failed: " + scene.Error());
        Require(std::chrono::steady_clock::now() < deadline, "Prepare never became ready");
        Sleep(1);
    }
}

void ProcessUntilReady(okuflow::MediaGpuScene& scene, const okuflow::VideoPlaybackFrame& frame,
                       int turns, const okuflow::ProcessingSettings& settings,
                       std::uint64_t epoch, std::uint64_t appearance, bool enableUct)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto started = std::chrono::steady_clock::now();
        const Status status = scene.Process(frame, turns, settings, epoch, appearance, enableUct);
        Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
                "Process blocked the calling thread");
        if (status == Status::Ready) return;
        if (status == Status::Failed)
            Fail("Process failed: " + scene.Error());
        Require(std::chrono::steady_clock::now() < deadline,
                "Process stayed pending past its retry budget");
        Sleep(1);
    }
}

// CPU decoder compatibility still runs the complete production CUDA chain.
// Padding deliberately has unrelated bytes, including an omitted final-row
// padding tail: only actual pixels may be read/copied into owned pinned slots.
okuflow::VideoPlaybackFrame MakeCpuFrame(UINT width, UINT height, unsigned seed,
                                        std::uint64_t sampleId, UINT padding = 0)
{
    const UINT stride = width * sizeof(Bgra) + padding;
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(
        size_t(stride) * (height - 1) + size_t(width) * sizeof(Bgra), 0xa5);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        const Bgra pixel = PatternPixel(x, y, seed);
        std::memcpy(bytes->data() + size_t(y) * stride + x * sizeof(Bgra), &pixel, sizeof(pixel));
    }
    okuflow::VideoPlaybackFrame frame;
    frame.bgra = std::move(bytes);
    frame.width = width; frame.height = height; frame.strideBytes = stride;
    frame.sampleId = sampleId; frame.pts100ns = std::int64_t(sampleId) * 333333;
    return frame;
}

void CpuBgraTests(Harness& harness)
{
    Require(!okuflow::ProcessingInput{}.rotateBgraOnGpu, "camera host BGRA rotation default changed");
    for (const UINT padding : {0u, 13u}) for (int turns = 0; turns < 4; ++turns) {
        constexpr UINT width = 59, height = 37;
        const UINT outWidth = (turns & 1) ? height : width;
        const UINT outHeight = (turns & 1) ? width : height;
        const unsigned seed = 20 + turns;
        const auto pattern = Pattern(width, height, seed);
        std::vector<Bgra> expected(size_t(outWidth) * outHeight);
        for (UINT y = 0; y < outHeight; ++y) for (UINT x = 0; x < outWidth; ++x)
            expected[size_t(y) * outWidth + x] = RotatePixel(pattern, width, height, turns, x, y);
        okuflow::MediaGpuScene scene;
        PrepareUntilReady(scene, harness.Device(), outWidth, outHeight);
        auto frame = MakeCpuFrame(width, height, seed, 2000 + turns, padding);
        okuflow::ProcessingSettings settings;
        settings.enableZoom = true; settings.zoomAmount = 2;
        ProcessUntilReady(scene, frame, turns, settings, 500, 1, true);
        ComparePixels(harness.ReadScene(scene).pixels, expected, outWidth, 0, "CPU BGRA rotation/padded stride");
        auto analysis = scene.TakeUctAnalysis();
        Require(analysis && analysis->sourceEpoch == 500 && analysis->frameSequence == frame.sampleId &&
                    analysis->processingGeneration == 1 && analysis->width == int(outWidth) &&
                    analysis->height == int(outHeight), "CPU BGRA UCT lost exact rotated identity");
        std::atomic<bool> cancelled{false}; QString error;
        const auto image = analysis->Readback(cancelled, error);
        Require(!image.isNull(), "CPU BGRA UCT readback failed");
        std::vector<Bgra> analysisPixels(expected.size());
        for (UINT y = 0; y < outHeight; ++y)
            std::memcpy(analysisPixels.data() + size_t(y) * outWidth, image.constScanLine(int(y)), outWidth * sizeof(Bgra));
        ComparePixels(analysisPixels, expected, outWidth, 0, "CPU BGRA post-rotation UCT pixels");
        const auto generation = scene.SceneGeneration(), resets = scene.HistoryResetCount();
        ProcessUntilReady(scene, frame, turns, settings, 500, 1, true);
        Require(scene.SceneGeneration() == generation && scene.HistoryResetCount() == resets,
                "same CPU frame retry reran geometry/history");
        settings.enableBlackWhite = true; settings.blackWhiteThreshold = .5f;
        ProcessUntilReady(scene, frame, turns, settings, 500, 2, false);
        auto effect = expected;
        for (auto& p : effect) {
            const std::uint8_t v = (LumaByte(p) / 255.0f) >= .5f ? 255 : 0;
            p = {v, v, v, 255};
        }
        ComparePixels(harness.ReadScene(scene).pixels, effect, outWidth, 0, "CPU BGRA CUDA black/white effect");
        Require(scene.HistoryResetCount() == resets, "CPU appearance edit reset history");
        settings.zoomCenterX = .7f;
        ProcessUntilReady(scene, frame, turns, settings, 500, 3, false);
        ComparePixels(harness.ReadScene(scene).pixels, effect, outWidth, 0, "CPU BGRA effect/view replay");
        const auto stats = scene.SourceStatistics();
        Require(stats.asyncPreparations == 0 && stats.importsCompleted == 0 && stats.cachedImports == 0,
                "CPU BGRA path claimed a D3D11 source import");
    }

    okuflow::MediaGpuScene scene;
    PrepareUntilReady(scene, harness.Device(), kWidth, kHeight);
    auto a = MakeCpuFrame(kWidth, kHeight, 30, 3000, 7);
    auto b = MakeCpuFrame(kWidth, kHeight, 31, 3001, 7);
    okuflow::ProcessingSettings settings;
    settings.enableTemporalSmoothing = true; settings.temporalSmoothingAlpha = .25f;
    ProcessUntilReady(scene, a, 0, settings, 501, 1, false);
    harness.ReadScene(scene);
    auto resets = scene.HistoryResetCount();
    ProcessUntilReady(scene, b, 0, settings, 501, 1, false);
    auto expected = Pattern(kWidth, kHeight, 31); const auto prior = Pattern(kWidth, kHeight, 30);
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i].b = std::uint8_t(.25f * expected[i].b + .75f * prior[i].b + .5f);
        expected[i].g = std::uint8_t(.25f * expected[i].g + .75f * prior[i].g + .5f);
        expected[i].r = std::uint8_t(.25f * expected[i].r + .75f * prior[i].r + .5f);
    }
    ComparePixels(harness.ReadScene(scene).pixels, expected, kWidth, 0, "CPU BGRA temporal history");
    settings.zoomCenterX = .8f;
    ProcessUntilReady(scene, b, 0, settings, 501, 2, false);
    ComparePixels(harness.ReadScene(scene).pixels, expected, kWidth, 0, "CPU replay double-blended history");
    Require(scene.HistoryResetCount() == resets, "forward CPU frame/edit reset history");
    Require(!scene.UctCoordinatesReliable() && !scene.TakeUctAnalysis(), "CPU temporal composite exposed UCT authority");

    auto changedStride = MakeCpuFrame(kWidth, kHeight, 32, 3002, 19);
    ProcessUntilReady(scene, changedStride, 0, settings, 501, 3, false);
    Require(scene.HistoryResetCount() == ++resets, "CPU stride transition retained old history");
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 32), kWidth, 0, "CPU layout transition seed");
    auto gpu = harness.MakeFrame(kWidth, kHeight, 33, 3003);
    ProcessUntilReady(scene, gpu, 0, settings, 501, 4, false);
    Require(scene.HistoryResetCount() == ++resets, "CPU to GPU transition retained old history");
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 33), kWidth, 0, "CPU to GPU transition seed");
    auto cpu = MakeCpuFrame(kWidth, kHeight, 34, 3004, 19);
    ProcessUntilReady(scene, cpu, 0, settings, 501, 5, false);
    Require(scene.HistoryResetCount() == ++resets, "GPU to CPU transition retained old history");
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 34), kWidth, 0, "GPU to CPU transition seed");
    // Same tickets/layout with different immutable storage must not replay the
    // former pixels; expired CPU identities must never match recycled memory.
    auto replacement = MakeCpuFrame(kWidth, kHeight, 35, cpu.sampleId, 19);
    const std::weak_ptr<const std::vector<std::uint8_t>> oldStorage = cpu.bgra;
    cpu = {};
    Require(oldStorage.expired(), "media scene retained borrowed CPU decoded storage");
    ProcessUntilReady(scene, replacement, 0, settings, 501, 6, false);
    Require(scene.HistoryResetCount() == ++resets, "replacement CPU storage was treated as same frame");
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 35), kWidth, 0, "CPU replacement identity");

    const auto generation = scene.SceneGeneration();
    const auto sync = scene.PresentationSync();
    auto invalid = replacement;
    auto refuses = [&](const char* message) {
        Require(scene.Process(invalid, 0, settings, 501, 7, true) == Status::Failed, message);
        Require(scene.SceneGeneration() == generation && scene.HistoryResetCount() == resets &&
                    scene.PresentationSync().signalValue == sync.signalValue, "bad CPU layout changed scene/fence identity");
    };
    invalid.bgra.reset(); refuses("missing CPU pixels accepted");
    invalid = replacement; invalid.strideBytes = kWidth * 4 - 1; refuses("short CPU stride accepted");
    invalid = replacement;
    invalid.bgra = std::make_shared<const std::vector<std::uint8_t>>(replacement.bgra->size() - 1);
    refuses("truncated final CPU row accepted");
    invalid = replacement; invalid.strideBytes = UINT32_MAX; refuses("oversized CPU layout accepted");
    invalid = replacement; invalid.width = 16385; refuses("unbounded CPU dimensions accepted");
    invalid = replacement; invalid.height = 0; refuses("empty CPU extent accepted");
    // A valid BGRA vector cannot hide a malformed claimed GPU lease.
    invalid = replacement; invalid.gpu = std::make_shared<const okuflow::VideoPlaybackGpuImage>();
    refuses("malformed GPU lease was silently processed as CPU");

    // Hold an actually queued private graphics signal behind a fixture gate.
    // Copies must own pinned rows even after the CPU frame lease is released.
    // One in-flight pass bounds seek latency even when upload slots are free;
    // orientation changes and newer frames must remain nonblocking Pending.
    auto gate = harness.BlockConsumer(scene);
    struct ReleaseGate { ComPtr<ID3D12Fence> fence; ~ReleaseGate() { fence->Signal(1); } } release{gate};
    auto queued = MakeCpuFrame(kWidth, kHeight, 36, 4000, 19);
    const std::weak_ptr<const std::vector<std::uint8_t>> queuedStorage = queued.bgra;
    auto status = scene.Process(queued, 0, {}, 502, 1, false);
    queued = {};
    Require(status == Status::Ready && queuedStorage.expired(), "queued upload borrowed CPU frame lifetime");
    auto turned = MakeCpuFrame(kHeight, kWidth, 37, 4001, 9);
    const auto beforeResizeResets = scene.HistoryResetCount();
    const auto started = std::chrono::steady_clock::now();
    status = scene.Process(turned, 1, {}, 502, 2, false);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Require(status == Status::Pending && scene.HistoryResetCount() == beforeResizeResets &&
                elapsed < std::chrono::milliseconds(500),
            "CPU orientation resize waited on pending CUDA/graphics work");
    const auto admittedGeneration = scene.SceneGeneration();
    const auto admittedSync = scene.PresentationSync();
    for (unsigned i = 0; i < 2; ++i) {
        auto next = MakeCpuFrame(kWidth, kHeight, 38 + i, 4002 + i, 19);
        const auto began = std::chrono::steady_clock::now();
        Require(scene.Process(next, 0, {}, 502, 3 + i, false) == Status::Pending &&
                    scene.SceneGeneration() == admittedGeneration &&
                    scene.PresentationSync().signalValue == admittedSync.signalValue &&
                    scene.HistoryResetCount() == beforeResizeResets &&
                    std::chrono::steady_clock::now() - began < std::chrono::milliseconds(500),
                "busy media pass queued a stale CPU frame or blocked");
    }
    auto blocked = MakeCpuFrame(kWidth, kHeight, 40, 4004, 19);
    const auto beforeBusy = scene.SceneGeneration();
    Require(scene.Process(blocked, 0, {}, 502, 5, false) == Status::Pending &&
                scene.SceneGeneration() == beforeBusy, "busy CPU upload ring changed scene identity");
    Hr(gate->Signal(1), "fixture consumer gate release failed");
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 36), kWidth, 0, "owned pinned queued CPU pixels");
    ProcessUntilReady(scene, blocked, 0, {}, 502, 5, false);
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 40), kWidth, 0, "CPU upload ring retry");
}

std::vector<Bgra> BlurReference(std::vector<Bgra> pixels, int radius, float sigma)
{
    std::vector<float> weights(size_t(2 * radius + 1));
    float sum = 0;
    for (int i = -radius; i <= radius; ++i) {
        weights[size_t(i + radius)] = std::exp(-float(i * i) / (2 * sigma * sigma));
        sum += weights[size_t(i + radius)];
    }
    for (auto& value : weights) value /= sum;
    auto pass = [&](const std::vector<Bgra>& source, bool vertical) {
        auto output = source;
        for (int y = 0; y < int(kHeight); ++y) for (int x = 0; x < int(kWidth); ++x) {
            float b = 0, g = 0, r = 0;
            for (int i = -radius; i <= radius; ++i) {
                const int sx = std::clamp(x + (vertical ? 0 : i), 0, int(kWidth) - 1);
                const int sy = std::clamp(y + (vertical ? i : 0), 0, int(kHeight) - 1);
                const auto& p = source[size_t(sy) * kWidth + sx];
                b += p.b * weights[size_t(i + radius)];
                g += p.g * weights[size_t(i + radius)];
                r += p.r * weights[size_t(i + radius)];
            }
            output[size_t(y) * kWidth + x] = {FloatToByte(b / 255), FloatToByte(g / 255), FloatToByte(r / 255), 255};
        }
        return output;
    };
    return pass(pass(pixels, false), true);
}

void EffectAndHistoryTests(Harness& harness)
{
    okuflow::MediaGpuScene scene;
    PrepareUntilReady(scene, harness.Device(), kWidth, kHeight);
    auto a = harness.MakeFrame(kWidth, kHeight, 12, 1000);
    auto b = harness.MakeFrame(kWidth, kHeight, 13, 1001);
    okuflow::ProcessingSettings settings;
    settings.enableBlur = true; settings.blurRadius = 3; settings.blurSigma = 1.7f;
    ProcessUntilReady(scene, a, 0, settings, 300, 1, false);
    ComparePixels(harness.ReadScene(scene).pixels, BlurReference(Pattern(kWidth, kHeight, 12), 3, 1.7f),
                  kWidth, 2, "Gaussian blur reference");

    settings = {}; settings.enableSpatialSharpen = true; settings.spatialSharpness = .9f;
    for (const auto upscaler : {okuflow::SpatialUpscaler::kNis, okuflow::SpatialUpscaler::kFsrEasuRcas}) {
        settings.spatialUpscaler = upscaler;
        ProcessUntilReady(scene, a, 0, settings, 300, 2 + int(upscaler), false);
        const auto sharpened = harness.ReadScene(scene).pixels;
        Require(sharpened != Pattern(kWidth, kHeight, 12), "spatial sharpening was cleared for video");
        ProcessUntilReady(scene, a, 0, settings, 300, 5 + int(upscaler), false);
        ComparePixels(harness.ReadScene(scene).pixels, sharpened, kWidth, 0, "same-frame sharpen replay");
    }

    settings = {}; settings.enableAutoTextClarity = true;
    settings.enableAdaptiveBinarization = true; settings.enableTwoColorText = true;
    settings.binarizationSoftness = 0;
    settings.enableTextHysteresis = true; settings.textHysteresisStrength = .2f;
    settings.textForegroundBgra = 0xff112233u; settings.textBackgroundBgra = 0xffddeeffu;
    ProcessUntilReady(scene, a, 0, settings, 301, 10, false);
    auto text = harness.ReadScene(scene).pixels;
    bool foreground = false, background = false;
    for (const auto& p : text) {
        const bool fg = p == Bgra{0x33, 0x22, 0x11, 255};
        const bool bg = p == Bgra{0xff, 0xee, 0xdd, 255};
        // Sauvola deliberately retains soft edge coverage (minimum .002),
        // so every pixel must lie on the exact chosen two-color ramp.
        Require(p.a == 255 && p.b >= 0x33 && p.b <= 0xff &&
                std::abs(int(p.b) - int(p.g) - 17) <= 1 &&
                std::abs(int(p.g) - int(p.r) - 17) <= 1,
                "automatic Text Clarity colors were not processed on GPU");
        foreground |= fg; background |= bg;
    }
    Require(foreground && background, "Text Clarity fixture produced only one color");
    ProcessUntilReady(scene, a, 0, settings, 301, 11, false);
    ComparePixels(harness.ReadScene(scene).pixels, text, kWidth, 0, "text hysteresis same-frame replay");

    settings = {}; settings.enableAutoContrast = true; settings.autoContrastStrength = 1;
    ProcessUntilReady(scene, a, 0, settings, 302, 20, false);
    const auto contrast = harness.ReadScene(scene).pixels;
    Require(contrast != Pattern(kWidth, kHeight, 12), "auto contrast was cleared for video");
    auto contrastExpected = Pattern(kWidth, kHeight, 12);
    std::array<unsigned, 256> histogram{};
    for (const auto& p : contrastExpected)
        ++histogram[size_t(std::clamp(int(std::nearbyint(.299f * p.r + .587f * p.g + .114f * p.b)), 0, 255))];
    unsigned cumulative = 0; int lo = -1, hi = 255;
    for (int bin = 0; bin < 256; ++bin) {
        cumulative += histogram[size_t(bin)];
        if (lo < 0 && cumulative > contrastExpected.size() * 2 / 100) lo = bin;
        if (cumulative >= contrastExpected.size() * 98 / 100) { hi = bin; break; }
    }
    for (auto& p : contrastExpected) {
        p.b = FloatToByte((float(p.b) - lo) / std::max(hi - lo, 1));
        p.g = FloatToByte((float(p.g) - lo) / std::max(hi - lo, 1));
        p.r = FloatToByte((float(p.r) - lo) / std::max(hi - lo, 1));
    }
    ComparePixels(contrast, contrastExpected, kWidth, 1, "auto contrast percentile reference");
    ProcessUntilReady(scene, a, 0, settings, 302, 21, false);
    ComparePixels(harness.ReadScene(scene).pixels, contrast, kWidth, 0, "auto-contrast same-frame replay");

    settings = {}; settings.enableTemporalSmoothing = true; settings.temporalSmoothingAlpha = .25f;
    ProcessUntilReady(scene, a, 0, settings, 303, 30, true);
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 12), kWidth, 0, "history seed");
    const auto resets = scene.HistoryResetCount();
    ProcessUntilReady(scene, b, 0, settings, 303, 30, true);
    Require(scene.HistoryResetCount() == resets, "ordinary forward frame reset temporal history");
    auto expected = Pattern(kWidth, kHeight, 13);
    const auto prior = Pattern(kWidth, kHeight, 12);
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i].b = std::uint8_t(.25f * expected[i].b + .75f * prior[i].b + .5f);
        expected[i].g = std::uint8_t(.25f * expected[i].g + .75f * prior[i].g + .5f);
        expected[i].r = std::uint8_t(.25f * expected[i].r + .75f * prior[i].r + .5f);
    }
    ComparePixels(harness.ReadScene(scene).pixels, expected, kWidth, 0, "forward temporal blend");
    Require(!scene.UctCoordinatesReliable() && !scene.TakeUctAnalysis(), "composite frame exposed unverifiable UCT geometry");
    const auto generation = scene.SceneGeneration();
    for (unsigned retry = 0; retry < 3; ++retry)
        ProcessUntilReady(scene, b, 0, settings, 303, 30, false);
    Require(scene.SceneGeneration() == generation, "same-frame retry republished/reprocessed the base scene");
    settings.zoomCenterX = .7f;
    ProcessUntilReady(scene, b, 0, settings, 303, 31, false);
    ComparePixels(harness.ReadScene(scene).pixels, expected, kWidth, 0, "paused view edit double-blended history");
    ProcessUntilReady(scene, a, 0, settings, 303, 32, false); // backwards PTS
    Require(scene.HistoryResetCount() == resets + 1, "backwards PTS did not reset state");
    ComparePixels(harness.ReadScene(scene).pixels, prior, kWidth, 0, "backwards PTS leaked future history");
    ProcessUntilReady(scene, b, 0, settings, 304, 33, false); // seek epoch
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 13), kWidth, 0, "seek retained old history");
    auto reticketed = b; ++reticketed.requestGeneration;
    ProcessUntilReady(scene, reticketed, 0, settings, 304, 34, false);
    Require(scene.HistoryResetCount() == resets + 3, "same-image seek generation failed to reset state");

    // Stabilization and keystone are production stages with observable state.
    // Pan and analysis retries cannot advance either stage; seek clears their
    // history rather than carrying a projected quad/reference into the new cut.
    settings = {}; settings.enableStabilization = true; settings.enableKeystone = true;
    settings.controlFrame.sourceEpoch = 305;
    ProcessUntilReady(scene, a, 0, settings, 305, 40, true);
    (void)harness.ReadScene(scene);
    const auto tracking = scene.GetKeystoneTrackingState();
    Require(tracking.count > 0, "keystone resources were not created for video");
    scene.SetKeystoneTrackingPaused(true);
    Require(scene.GetKeystoneTrackingState().paused, "keystone pause was not forwarded");
    const auto historyGeneration = scene.HistoryResetCount();
    settings.zoomCenterX = .6f;
    ProcessUntilReady(scene, a, 0, settings, 305, 41, true);
    Require(scene.GetKeystoneTrackingState().paused && scene.HistoryResetCount() == historyGeneration,
            "same-frame pan advanced/reset geometry history");
    Require(scene.StepKeystoneCorrection(1), "paused keystone step was not admitted");
    const auto stepDeadline = GetTickCount64() + 2000;
    do {
        ProcessUntilReady(scene, a, 0, settings, 305, 41, true);
        (void)harness.ReadScene(scene);
        Require(GetTickCount64() < stepDeadline, "paused same-frame keystone step never completed");
    } while (scene.GetKeystoneTrackingState().stepPending);
    Require(scene.HistoryResetCount() == historyGeneration,
            "explicit paused keystone step advanced stabilization/source history");
    ProcessUntilReady(scene, b, 0, settings, 306, 42, true);
    Require(!scene.GetKeystoneTrackingState().paused && scene.HistoryResetCount() == historyGeneration + 1,
            "source epoch did not reset keystone/stabilization state");
    (void)harness.ReadScene(scene);
}

struct FakeModule {
    HMODULE module{};
    MaxineFakeRead read{};
    MaxineFakeFillOutput fill{};
    MaxineFakeBlockRun block{};
    explicit FakeModule(const char* path) {
        module = LoadLibraryExW(QFileInfo(QString::fromUtf8(path)).absoluteFilePath().toStdWString().c_str(),
            nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(module != nullptr, "private fake SDK DLL did not load");
        read = reinterpret_cast<MaxineFakeRead>(GetProcAddress(module, "OkuFlowFakeRead"));
        fill = reinterpret_cast<MaxineFakeFillOutput>(GetProcAddress(module, "OkuFlowFakeFillOutput"));
        block = reinterpret_cast<MaxineFakeBlockRun>(GetProcAddress(module, "OkuFlowFakeBlockRun"));
        Require(read != nullptr, "private fake SDK controls are absent");
    }
    ~FakeModule() { if (module) FreeLibrary(module); }
    MaxineFakeSnapshot Stats() const { MaxineFakeSnapshot result; read(&result); return result; }
};

struct SdkGate {
    HANDLE entered{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE returned{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    bool armed{};
    ~SdkGate() {
        SetEvent(release);
        if (armed && WaitForSingleObject(returned, 2500) != WAIT_OBJECT_0) return;
        CloseHandle(entered); CloseHandle(release); CloseHandle(returned);
    }
};

void SpatialAndNeuralTests(Harness& harness)
{
#if defined(OKUFLOW_MAXINE_FAKE_IMAGE) && defined(OKUFLOW_MAXINE_FAKE_EFFECTS)
    FakeModule image(OKUFLOW_MAXINE_FAKE_IMAGE), effects(OKUFLOW_MAXINE_FAKE_EFFECTS);
    Require(image.fill && effects.block, "private fake SDK pixel/gate controls missing");
    constexpr unsigned color = 0xff4bb123u;
    image.fill(true, color);
    okuflow::MediaGpuScene scene;
    PrepareUntilReady(scene, harness.Device(), 512, 512);
    Require(scene.SuperResWidth() == 512 && scene.SuperResHeight() == 512,
            "media spatial cache allocation has the wrong extent");
    auto frame = harness.MakeFrame(512, 512, 17, 1100);
    okuflow::ProcessingSettings settings;
    settings.zoomAmount = 2;
    settings.enableSpatialSharpen = true;
    settings.spatialViewportWidth = settings.spatialViewportHeight = 512;
    settings.spatialViewTransform = okuflow::ComputeViewTransform(512, 512, 512, 512, 2, .5f, .5f,
                                                                okuflow::ViewportFitMode::kFill);
    ProcessUntilReady(scene, frame, 0, settings, 400, 1, true);
    const auto roi = scene.SuperResRoi();
    Require(scene.SuperResTexture() && roi.valid && roi.outputWidth == 512 && roi.outputHeight == 512 &&
            !scene.IsSuperResActive(), "spatial sharpening did not produce the media ROI cache");
    const auto spatial = harness.ReadScene(scene, false, true).pixels;
    Require(spatial != Pattern(512, 512, 17), "spatial cache did not crop/upscale the requested video view");
    auto awaitNeural = [&] {
        const auto deadline = GetTickCount64() + 5000;
        while (!scene.IsSuperResActive() && GetTickCount64() < deadline) {
            Require(scene.RefreshSuperRes() != okuflow::SuperResRefreshResult::Failed,
                    "private neural publication failed");
            Sleep(1);
        }
        if (!scene.IsSuperResActive()) Fail("paused neural publication timed out: " + scene.SuperResStatus());
    };
    settings.enableMlSuperRes = true;
    std::fprintf(stderr, "Media fixture: paused neural publication\n");
    ProcessUntilReady(scene, frame, 0, settings, 400, 2, true);
    const auto baseGeneration = scene.SceneGeneration();
    // An abandoned presentation reservation must never become a CUDA wait.
    const auto abandoned = scene.PresentationSync();
    awaitNeural();
    Require(scene.SceneFrameSequence() == frame.sampleId && scene.SceneAppearance() == 2 &&
            scene.SceneGeneration() > baseGeneration && scene.SuperResStatistics().submitted == 1,
            "same-frame neural publication lost its exact identity or submitted twice");
    Require(scene.PresentationSync().signalValue != abandoned.signalValue &&
            !scene.UctCoordinatesReliable() && !scene.TakeUctAnalysis(),
            "neural publication retained a stale reservation or unproven text overlay");
    const auto neural = harness.ReadScene(scene, true, true).pixels;
    for (const auto& p : neural)
        Require(p == Bgra{0x23, 0xb1, 0x4b, 255}, "private SDK output did not reach the GPU cache through the private fence");
    ComparePixels(harness.ReadScene(scene).pixels, Pattern(512, 512, 17), 512, 0,
                  "neural publication overwrote the full-resolution base scene");
    Require(effects.Stats().lastThread != GetCurrentThreadId(), "SDK inference ran on the caller thread");
    const auto loads = effects.Stats().loaded;
    std::fprintf(stderr, "Media fixture: retain model across source invalidation\n");
    scene.Invalidate();
    PrepareUntilReady(scene, harness.Device(), 512, 512);
    ProcessUntilReady(scene, frame, 0, settings, 401, 3, true);
    awaitNeural();
    Require(effects.Stats().loaded == loads, "normal source invalidation reloaded the private neural model");
    (void)harness.ReadScene(scene, false, true);

    // Hold the SDK for frame A, replace it with frame B and keep polling
    // without any playback clock advance. Only B may publish.
    SdkGate gate;
    std::fprintf(stderr, "Media fixture: block obsolete inference\n");
    effects.block(gate.entered, gate.release, gate.returned); gate.armed = true;
    auto stale = harness.MakeFrame(512, 512, 18, 1101);
    ProcessUntilReady(scene, stale, 0, settings, 401, 4, false);
    const auto deadline = GetTickCount64() + 5000;
    while (WaitForSingleObject(gate.entered, 0) != WAIT_OBJECT_0 && GetTickCount64() < deadline) {
        (void)scene.RefreshSuperRes(); Sleep(1);
    }
    Require(WaitForSingleObject(gate.entered, 0) == WAIT_OBJECT_0, "private inference gate was never entered");
    std::fprintf(stderr, "Media fixture: replace blocked inference frame\n");
    auto latest = harness.MakeFrame(512, 512, 19, 1102);
    ProcessUntilReady(scene, latest, 0, settings, 401, 5, false);
    std::fprintf(stderr, "Media fixture: release obsolete inference and publish latest\n");
    Require(!scene.IsSuperResActive(), "previous frame neural pixels survived a newer video frame");
    SetEvent(gate.release);
    awaitNeural();
    Require(scene.SceneFrameSequence() == latest.sampleId && scene.SceneAppearance() == 5 &&
            scene.SuperResStatistics().staleDiscarded >= 1,
            "obsolete neural result published under the newer frame identity");
    effects.block(nullptr, nullptr, nullptr);
    (void)harness.ReadScene(scene, false, true);
    scene.ResetSuperRes();
    Require(!scene.IsSuperResActive() && !scene.SuperResRoi().valid && !scene.NeedsSuperResPolling(),
            "explicit media neural reset retained an active/pending presentation");
    std::fprintf(stderr, "Media fixture: neural tests complete\n");
    image.fill(false, 0);
#else
    Fail("media GPU fixture is missing its private fake SDK prerequisite");
#endif
}

// The production destructor deliberately hands cleanup to detached workers.
// This private console process must keep cudart loaded until those exact host
// workers exit; otherwise the CRT unload races their cudaFree/context queries.
// Wait only for this process's C++ workers, never an owner process or NVIDIA's
// persistent driver threads. This is fixture shutdown, not a UI-thread wait.
void AwaitOwnedWorkers()
{
    using QueryStart = LONG (WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    const auto query = reinterpret_cast<QueryStart>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                 "NtQueryInformationThread"));
    Require(query != nullptr, "fixture thread provenance query is unavailable");
    const HMODULE image = GetModuleHandleW(nullptr);
    const HMODULE crt = GetModuleHandleW(L"ucrtbase.dll");
    const auto deadline = GetTickCount64() + 5000;
    for (;;) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        Require(snapshot != INVALID_HANDLE_VALUE, "fixture worker inventory failed");
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
        std::vector<HANDLE> workers;
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ThreadID);
            if (!thread) continue; // already exited
            void* start{}; HMODULE module{};
            if (query(thread, 9, &start, sizeof(start), nullptr) == 0 &&
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(start), &module) && (module == image || module == crt))
                workers.push_back(thread);
            else CloseHandle(thread);
        }
        CloseHandle(snapshot);
        if (workers.empty()) return;
        bool failed = false;
        for (HANDLE thread : workers) {
            const auto now = GetTickCount64();
            const DWORD wait = WaitForSingleObject(thread, now < deadline ? DWORD(deadline - now) : 0);
            failed |= wait != WAIT_OBJECT_0;
            CloseHandle(thread);
        }
        Require(!failed, "fixture-owned asynchronous GPU/SDK cleanup did not finish before runtime unload");
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        int devices = 0;
        const auto availability = cudaGetDeviceCount(&devices);
        if (availability == cudaErrorNoDevice || (availability == cudaSuccess && devices == 0))
            return 77;
        Cuda(availability, "CUDA availability probe failed");

#ifdef OKUFLOW_MAXINE_FAKE_IMAGE
        // Pin the private fake runtime so no installed SDK can be probed.
        qputenv("OKUFLOW_MAXINE_PATH",
                QFileInfo(QString::fromUtf8(OKUFLOW_MAXINE_FAKE_IMAGE)).absolutePath().toUtf8());
#endif

        {
        Harness harness;
        okuflow::MediaGpuScene scene;

        // Unprepared surface: everything reads empty and safely.
        Require(scene.Texture() == nullptr && !scene.SceneReady() && scene.Width() == 0 &&
                    scene.Height() == 0 && scene.SceneEpoch() == 0 &&
                    !scene.PresentationSync().enable && !scene.TakeUctAnalysis() &&
                    !scene.UctAnalysisFailed(),
                "unprepared scene exposed state");
        Require(scene.SourceStatistics().cachedImports == 0 &&
                    scene.SourceStatistics().importsCompleted == 0,
                "unprepared scene reported source imports");
        Require(scene.Prepare(nullptr, kWidth, kHeight) == Status::Failed &&
                    scene.Prepare(harness.Device(), 0, kHeight) == Status::Failed &&
                    !scene.Error().empty(),
                "invalid Prepare arguments were not refused");
        {
            auto frame = harness.MakeFrame(kWidth, kHeight, 1, 100);
            Require(scene.Process(frame, 0, {}, 7, 11, false) == Status::Pending,
                    "Process before Prepare was not pending");
        }

        PrepareUntilReady(scene, harness.Device(), kWidth, kHeight);
        Require(scene.Width() == kWidth && scene.Height() == kHeight && !scene.SceneReady(),
                "prepared scene extent or readiness is wrong");

        // Frame 1: identity pixels. Legacy CUDA zoom is still suppressed
        // because the presenter owns it; production effects are tested below.
        std::weak_ptr<SourceStorage> firstLease;
        std::uint64_t lastReadSignal = 0;
        {
            auto frame = harness.MakeFrame(kWidth, kHeight, 1, 101, &firstLease);
            okuflow::ProcessingSettings settings;
            settings.enableZoom = true;
            settings.zoomAmount = 2.0f;
            ProcessUntilReady(scene, frame, 0, settings, 7, 11, false);
            frame = {}; // drop the fixture's lease; the CUDA copy must retain it
            Require(!firstLease.expired(),
                    "frame lease was released before its CUDA copy retired");
            Require(scene.SceneReady() && scene.SceneEpoch() == 7 &&
                        scene.SceneFrameSequence() == 101 && scene.SceneAppearance() == 11,
                    "scene identity does not match the processed frame");
            Require(!scene.TakeUctAnalysis(), "UCT analysis appeared while disabled");

            // Reservation contract: reissued until recorded; a bogus value
            // records nothing.
            const auto syncA = scene.PresentationSync();
            const auto syncB = scene.PresentationSync();
            Require(syncA.enable && syncA.signalValue == syncB.signalValue &&
                        syncA.waitValue == syncB.waitValue && syncA.waitValue != 0,
                    "unrecorded presentation reservation was not reissued");
            scene.RecordPresented(syncA.signalValue + 999);
            Require(scene.PresentationSync().signalValue == syncA.signalValue,
                    "a bogus independent-read value committed the reservation");

            const auto expected = Pattern(kWidth, kHeight, 1);
            auto first = harness.ReadScene(scene);
            Require(first.waitValue == syncA.waitValue && first.signalValue == syncA.signalValue,
                    "consumer read did not consume the reserved sync");
            ComparePixels(first.pixels, expected, kWidth, 0, "identity frame");
            Require(!firstLease.expired(),
                    "frame lease was released by a consumer read instead of copy retirement");

            // Repeated view-only reads: unique increasing signals, same pixels.
            auto second = harness.ReadScene(scene, true);
            auto third = harness.ReadScene(scene);
            Require(second.signalValue > first.signalValue &&
                        third.signalValue > second.signalValue &&
                        second.waitValue == first.waitValue && third.waitValue == first.waitValue,
                    "repeated reads did not receive unique private read signals");
            ComparePixels(second.pixels, expected, kWidth, 0, "repeated read 2");
            ComparePixels(third.pixels, expected, kWidth, 0, "repeated read 3");
            lastReadSignal = third.signalValue;
        }

        // Frame 2: a new Process waits the exact last recorded read; the
        // first lease retires once its copy is polled complete.
        {
            auto frame = harness.MakeFrame(kWidth, kHeight, 2, 102);
            ProcessUntilReady(scene, frame, 0, {}, 7, 12, false);
            Require(firstLease.expired(),
                    "previous frame lease was not released after its copy retired");
            const auto sync = scene.PresentationSync();
            Require(sync.waitValue > lastReadSignal,
                    "new producer signal does not follow the last recorded read");
            auto read = harness.ReadScene(scene);
            ComparePixels(read.pixels, Pattern(kWidth, kHeight, 2), kWidth, 0, "second frame");
            lastReadSignal = read.signalValue;
        }

        // Rotating decoder slots: three distinct source textures cycle
        // twice. The first cycle must pend while the one preparation
        // worker imports each slot exactly once (never on this thread and
        // never restamping the previous scene); the second cycle must be
        // immediate cache hits with exact pixels and no new import.
        std::vector<okuflow::VideoPlaybackFrame> rotating;
        {
            const auto statsBefore = scene.SourceStatistics();
            const auto thisThread = static_cast<std::uint64_t>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
            for (unsigned slot = 0; slot < 3; ++slot)
                rotating.push_back(harness.MakeFrame(kWidth, kHeight, 21 + slot, 501 + slot));
            std::uint64_t previousSequence = scene.SceneFrameSequence();
            for (unsigned slot = 0; slot < 3; ++slot) {
                Require(scene.Process(rotating[slot], 0, {}, 7, 60 + slot, false) ==
                            Status::Pending,
                        "first use of a decoder slot did not pend for async import");
                Require(scene.SceneFrameSequence() == previousSequence,
                        "pending import restamped the previous scene");
                ProcessUntilReady(scene, rotating[slot], 0, {}, 7, 60 + slot, false);
                ComparePixels(harness.ReadScene(scene).pixels,
                              Pattern(kWidth, kHeight, 21 + slot), kWidth, 0,
                              "rotating slot first cycle");
                previousSequence = 501 + slot;
            }
            const auto statsAfterFirst = scene.SourceStatistics();
            Require(statsAfterFirst.importsCompleted - statsBefore.importsCompleted == 3 &&
                        statsAfterFirst.importsFailed == statsBefore.importsFailed &&
                        statsAfterFirst.asyncImportThread != 0 &&
                        statsAfterFirst.asyncImportThread != thisThread,
                    "decoder slot imports did not run exactly once on the worker");
            for (unsigned slot = 0; slot < 3; ++slot) {
                Require(scene.Process(rotating[slot], 0, {}, 7, 70 + slot, false) ==
                            Status::Ready,
                        "cached decoder slot pended instead of reusing its import");
                Require(scene.SceneFrameSequence() == 501 + slot &&
                            scene.SceneAppearance() == 70 + slot,
                        "cached slot scene identity is wrong");
                ComparePixels(harness.ReadScene(scene).pixels,
                              Pattern(kWidth, kHeight, 21 + slot), kWidth, 0,
                              "rotating slot second cycle");
            }
            const auto statsAfterSecond = scene.SourceStatistics();
            Require(statsAfterSecond.importsCompleted == statsAfterFirst.importsCompleted &&
                        statsAfterSecond.cacheHits - statsAfterFirst.cacheHits >= 3,
                    "second rotating cycle re-imported a cached slot");
        }

        // Bounded cache: nine further distinct slots exceed the 8-import
        // ceiling, evicting least-recently-used imports (destroyed off this
        // thread). The cache never grows past the bound, and the evicted
        // first rotating slot must pend for a fresh worker import on reuse.
        {
            const auto statsBefore = scene.SourceStatistics();
            std::vector<okuflow::VideoPlaybackFrame> extra;
            for (unsigned slot = 0; slot < 9; ++slot) {
                extra.push_back(harness.MakeFrame(kWidth, kHeight, 31 + slot, 601 + slot));
                ProcessUntilReady(scene, extra.back(), 0, {}, 7, 80 + slot, false);
                ComparePixels(harness.ReadScene(scene).pixels,
                              Pattern(kWidth, kHeight, 31 + slot), kWidth, 0,
                              "cache bound cycle");
                Require(scene.SourceStatistics().cachedImports <= 8,
                        "source import cache exceeded its bound");
            }
            Require(scene.SourceStatistics().importsCompleted - statsBefore.importsCompleted == 9,
                    "cache bound cycle did not import each new slot exactly once");
            Require(scene.Process(rotating[0], 0, {}, 7, 90, false) == Status::Pending,
                    "evicted decoder slot did not pend for re-import");
            ProcessUntilReady(scene, rotating[0], 0, {}, 7, 90, false);
            ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 21),
                          kWidth, 0, "re-imported evicted slot");
        }
        rotating.clear();

        // Rotation needs the post-rotation bundle: mismatched Process is
        // Pending, Prepare replaces the size asynchronously, then 90 CW and
        // -1 (270 CW) land exact pixels.
        {
            auto frame = harness.MakeFrame(kWidth, kHeight, 3, 103);
            Require(scene.Process(frame, 1, {}, 7, 13, false) == Status::Pending,
                    "size-mismatched Process was not pending");
            PrepareUntilReady(scene, harness.Device(), kHeight, kWidth);
            Require(scene.Width() == kHeight && scene.Height() == kWidth,
                    "size replacement did not adopt the post-rotation extent");
            ProcessUntilReady(scene, frame, 1, {}, 7, 13, false);
            const auto source = Pattern(kWidth, kHeight, 3);
            std::vector<Bgra> expected(size_t(kHeight) * kWidth);
            for (UINT y = 0; y < kWidth; ++y)
                for (UINT x = 0; x < kHeight; ++x)
                    expected[size_t(y) * kHeight + x] = RotatePixel(source, kWidth, kHeight, 1, x, y);
            ComparePixels(harness.ReadScene(scene).pixels, expected, kHeight, 0, "rotate 90 CW");

            ProcessUntilReady(scene, frame, -1, {}, 7, 14, false);
            for (UINT y = 0; y < kWidth; ++y)
                for (UINT x = 0; x < kHeight; ++x)
                    expected[size_t(y) * kHeight + x] = RotatePixel(source, kWidth, kHeight, 3, x, y);
            ComparePixels(harness.ReadScene(scene).pixels, expected, kHeight, 0, "rotate 270 CW");
        }
        PrepareUntilReady(scene, harness.Device(), kWidth, kHeight);

        // 180 rotation at the restored extent.
        {
            auto frame = harness.MakeFrame(kWidth, kHeight, 4, 104);
            ProcessUntilReady(scene, frame, 2, {}, 8, 15, false);
            const auto source = Pattern(kWidth, kHeight, 4);
            std::vector<Bgra> expected(source.size());
            for (UINT y = 0; y < kHeight; ++y)
                for (UINT x = 0; x < kWidth; ++x)
                    expected[size_t(y) * kWidth + x] = RotatePixel(source, kWidth, kHeight, 2, x, y);
            ComparePixels(harness.ReadScene(scene).pixels, expected, kWidth, 0, "rotate 180");
        }

        // Contrast/brightness grade.
        {
            auto frame = harness.MakeFrame(kWidth, kHeight, 5, 105);
            okuflow::ProcessingSettings settings;
            settings.contrast = 2.0f;
            settings.brightness = 0.1f;
            ProcessUntilReady(scene, frame, 0, settings, 8, 16, false);
            auto expected = Pattern(kWidth, kHeight, 5);
            for (auto& pixel : expected) {
                pixel.b = FloatToByte((pixel.b / 255.0f - 0.5f) * 2.0f + 0.5f + 0.1f);
                pixel.g = FloatToByte((pixel.g / 255.0f - 0.5f) * 2.0f + 0.5f + 0.1f);
                pixel.r = FloatToByte((pixel.r / 255.0f - 0.5f) * 2.0f + 0.5f + 0.1f);
            }
            const auto read = harness.ReadScene(scene);
            ComparePixels(read.pixels, expected, kWidth, 1, "contrast/brightness");
            Require(read.pixels != Pattern(kWidth, kHeight, 5),
                    "contrast/brightness left pixels unchanged");
        }

        // Black & white threshold.
        {
            auto frame = harness.MakeFrame(kWidth, kHeight, 6, 106);
            okuflow::ProcessingSettings settings;
            settings.enableBlackWhite = true;
            settings.blackWhiteThreshold = 0.5f;
            ProcessUntilReady(scene, frame, 0, settings, 8, 17, false);
            const auto read = harness.ReadScene(scene);
            const auto source = Pattern(kWidth, kHeight, 6);
            bool sawBlack = false, sawWhite = false;
            for (size_t i = 0; i < source.size(); ++i) {
                Require(read.pixels[i].b == read.pixels[i].g && read.pixels[i].g == read.pixels[i].r &&
                            (read.pixels[i].b == 0 || read.pixels[i].b == 255),
                        "black/white output is not pure two-level gray");
                const int luma = LumaByte(source[i]);
                sawBlack |= read.pixels[i].b == 0;
                sawWhite |= read.pixels[i].b == 255;
                if (std::abs(luma - 128) > 2) // stay off the float boundary
                    Require(read.pixels[i].b == (luma >= 128 ? 255 : 0),
                            "black/white threshold side is wrong");
            }
            Require(sawBlack && sawWhite, "black/white output collapsed to one level");
        }

        // Luma LUT display mapping, then invert.
        {
            std::array<std::uint32_t, 256> lut{};
            for (std::uint32_t i = 0; i < 256; ++i)
                lut[i] = (255u - i) | (i << 8) | (128u << 16) | (255u << 24);
            auto frame = harness.MakeFrame(kWidth, kHeight, 7, 107);
            okuflow::ProcessingSettings settings;
            settings.displayColorTransform = okuflow::DisplayColorTransform::kLumaLut;
            settings.displayColorLut = lut.data();
            settings.displayColorLutGeneration = 42;
            ProcessUntilReady(scene, frame, 0, settings, 8, 18, false);
            auto expected = Pattern(kWidth, kHeight, 7);
            for (auto& pixel : expected) {
                const std::uint32_t mapped = lut[LumaByte(pixel)];
                pixel = Bgra{std::uint8_t(mapped & 0xff), std::uint8_t((mapped >> 8) & 0xff),
                             std::uint8_t((mapped >> 16) & 0xff), 255};
            }
            ComparePixels(harness.ReadScene(scene).pixels, expected, kWidth, 1, "luma LUT");

            settings = {};
            settings.displayColorTransform = okuflow::DisplayColorTransform::kInvert;
            ProcessUntilReady(scene, frame, 0, settings, 8, 19, false);
            expected = Pattern(kWidth, kHeight, 7);
            for (auto& pixel : expected) {
                pixel.b = std::uint8_t(255 - pixel.b);
                pixel.g = std::uint8_t(255 - pixel.g);
                pixel.r = std::uint8_t(255 - pixel.r);
            }
            ComparePixels(harness.ReadScene(scene).pixels, expected, kWidth, 1, "invert");
        }

        // UCT: exact device-image identity for the current scene, holding
        // pre-grade pixels even though the scene itself is contrast-graded.
        {
            auto frame = harness.MakeFrame(kWidth, kHeight, 8, 201);
            okuflow::ProcessingSettings settings;
            settings.contrast = 2.0f;
            ProcessUntilReady(scene, frame, 0, settings, 9, 21, true);
            Require(!scene.UctAnalysisFailed(), "UCT analysis pool reported failure");
            auto analysis = scene.TakeUctAnalysis();
            Require(analysis != nullptr, "UCT analysis missing for the first enabled frame");
            Require(analysis->sourceEpoch == 9 && analysis->frameSequence == 201 &&
                        analysis->processingGeneration == 21 && analysis->width == int(kWidth) &&
                        analysis->height == int(kHeight) && analysis->device == harness.CudaDevice() &&
                        analysis->Data() != nullptr && analysis->pitch >= kWidth * 4,
                    "UCT analysis identity does not match the exact scene/device");
            Require(!scene.TakeUctAnalysis(), "UCT analysis was handed out twice");
            std::atomic<bool> cancelled{false};
            QString error;
            const QImage image = analysis->Readback(cancelled, error);
            Require(!image.isNull(), "UCT analysis readback failed");
            Require(image.width() == int(kWidth) && image.height() == int(kHeight),
                    "UCT analysis readback extent is wrong");
            std::vector<Bgra> actual(size_t(kWidth) * kHeight);
            for (UINT y = 0; y < kHeight; ++y)
                std::memcpy(actual.data() + size_t(y) * kWidth, image.constScanLine(int(y)),
                            size_t(kWidth) * sizeof(Bgra));
            ComparePixels(actual, Pattern(kWidth, kHeight, 8), kWidth, 0,
                          "UCT pre-grade analysis pixels");
            // The graded scene must differ from the neutral analysis image.
            Require(harness.ReadScene(scene).pixels != Pattern(kWidth, kHeight, 8),
                    "graded scene unexpectedly matches the neutral analysis pixels");
            const auto baseGeneration = scene.SceneGeneration();
            Sleep(90); // Retry the independent 80 ms capture admission.
            ProcessUntilReady(scene, frame, 0, settings, 9, 21, true);
            auto retried = scene.TakeUctAnalysis();
            Require(retried && retried->sourceEpoch == 9 && retried->frameSequence == 201 &&
                    retried->processingGeneration == 21 && scene.SceneGeneration() == baseGeneration,
                    "paused UCT retry advanced history or lost the exact analysis identity");
        }

        // Seek-epoch replacement: an untaken analysis from the old epoch can
        // never be stamped onto the seeked frame.
        {
            Sleep(90); // let the surface's 80 ms analysis throttle reopen
            auto oldFrame = harness.MakeFrame(kWidth, kHeight, 9, 300);
            ProcessUntilReady(scene, oldFrame, 0, {}, 100, 31, true);
            auto seeked = harness.MakeFrame(kWidth, kHeight, 10, 301);
            ProcessUntilReady(scene, seeked, 0, {}, 101, 32, true);
            Require(scene.SceneEpoch() == 101 && scene.SceneFrameSequence() == 301,
                    "seek epoch did not replace scene identity");
            if (auto analysis = scene.TakeUctAnalysis())
                Require(analysis->sourceEpoch == 101 && analysis->frameSequence == 301 &&
                            analysis->processingGeneration == 32,
                        "stale pre-seek analysis was stamped onto the seeked frame");
            ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 10), kWidth, 0,
                          "seeked frame");
        }

        // Invalid inputs fail safely and the bundle stays usable.
        {
            okuflow::VideoPlaybackFrame invalid;
            Require(scene.Process(invalid, 0, {}, 101, 33, false) == Status::Failed &&
                        !scene.Error().empty(),
                    "frame without GPU image was not refused");
            auto broken = harness.MakeFrame(kWidth, kHeight, 11, 302);
            auto brokenImage = std::make_shared<okuflow::VideoPlaybackGpuImage>(*broken.gpu);
            brokenImage->texture = nullptr;
            broken.gpu = brokenImage;
            Require(scene.Process(broken, 0, {}, 101, 33, false) == Status::Failed,
                    "frame without texture was not refused");
            broken = harness.MakeFrame(kWidth, kHeight, 11, 303);
            broken.width = 0;
            Require(scene.Process(broken, 0, {}, 101, 33, false) == Status::Failed,
                    "zero-width frame was not refused");
            auto wrongSize = harness.MakeFrame(32, 32, 11, 304);
            Require(scene.Process(wrongSize, 0, {}, 101, 33, false) == Status::Pending,
                    "wrong-size frame was not pending pending Prepare");
            auto recovery = harness.MakeFrame(kWidth, kHeight, 11, 305);
            ProcessUntilReady(scene, recovery, 0, {}, 101, 34, false);
            ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 11), kWidth, 0,
                          "recovery after invalid inputs");
        }

        // Invalidate is immediate and nonblocking; Prepare re-arms after it.
        {
            const auto started = std::chrono::steady_clock::now();
            scene.Invalidate();
            Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
                    "Invalidate blocked the calling thread");
            Require(scene.Texture() == nullptr && scene.SceneEpoch() == 0 &&
                        !scene.PresentationSync().enable && !scene.TakeUctAnalysis(),
                    "Invalidate left scene state behind");
            PrepareUntilReady(scene, harness.Device(), kWidth, kHeight);
            auto frame = harness.MakeFrame(kWidth, kHeight, 12, 306);
            ProcessUntilReady(scene, frame, 0, {}, 200, 41, false);
            ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 12), kWidth, 0,
                          "frame after invalidate/re-prepare");
        }

        // Cancel during source preparation: Invalidate while the import
        // worker is in flight retires the whole bundle (slot included) off
        // this thread; the same frame then imports cleanly on the new one.
        {
            auto frame = harness.MakeFrame(kWidth, kHeight, 41, 701);
            Require(scene.Process(frame, 0, {}, 200, 42, false) == Status::Pending,
                    "new slot on a fresh bundle did not pend for async import");
            const auto started = std::chrono::steady_clock::now();
            scene.Invalidate();
            Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
                    "Invalidate during source preparation blocked");
            PrepareUntilReady(scene, harness.Device(), kWidth, kHeight);
            ProcessUntilReady(scene, frame, 0, {}, 200, 43, false);
            ComparePixels(harness.ReadScene(scene).pixels, Pattern(kWidth, kHeight, 41),
                          kWidth, 0, "frame after cancel during preparation");
        }

        // Superseded initialization: a bundle cancelled by Invalidate before
        // adoption must retire, never replace the newer desired size. The
        // destructor likewise hands off without waiting.
        {
            okuflow::MediaGpuScene second;
            (void)second.Prepare(harness.Device(), 32, 24); // worker starts
            const auto started = std::chrono::steady_clock::now();
            second.Invalidate(); // supersedes the in-flight 32x24 bundle
            Require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
                    "cancelling initialization blocked the calling thread");
            PrepareUntilReady(second, harness.Device(), 40, 30);
            Require(second.Width() == 40 && second.Height() == 30,
                    "superseded initialization replaced the desired size");
            auto frame = harness.MakeFrame(40, 30, 13, 401);
            ProcessUntilReady(second, frame, 0, {}, 300, 51, false);
            ComparePixels(harness.ReadScene(second).pixels, Pattern(40, 30, 13), 40, 0,
                          "superseded-init scene");
            auto doomed = std::make_unique<okuflow::MediaGpuScene>();
            PrepareUntilReady(*doomed, harness.Device(), 40, 30);
            const auto destroyStart = std::chrono::steady_clock::now();
            doomed.reset();
            Require(std::chrono::steady_clock::now() - destroyStart < std::chrono::seconds(2),
                    "destructor blocked the calling thread");
        }

        CpuBgraTests(harness);
        EffectAndHistoryTests(harness);
        SpatialAndNeuralTests(harness);
        } // Queue destruction of all fixture-owned GPU scenes first.
        AwaitOwnedWorkers();
        std::puts("PASS: MediaGpuScene exact-adapter GPU ownership, private fence reads, "
                  "rotation/effects pixels, UCT identity, lease and lifecycle contracts, "
                  "rotating-slot async import cache, CPU BGRA rotation/layout/pinned ownership, "
                  "history replay/reset, spatial/async neural publication");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        try { AwaitOwnedWorkers(); }
        catch (const std::exception& cleanup) { std::fprintf(stderr, "Cleanup: %s\n", cleanup.what()); }
        return 1;
    }
}
