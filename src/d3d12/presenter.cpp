#ifdef _WIN32

#include "openzoom/d3d12/presenter.hpp"
#include "openzoom/d3d12/fence_wait.hpp"
#include "openzoom/d3d12/frame_readiness.hpp"

#include <QDebug>

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace openzoom {

namespace {

inline void ThrowIfFailed(HRESULT hr, const char* message)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(message) + " (hr=0x" + std::to_string(static_cast<unsigned long>(hr)) + ")");
    }
}

inline D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource,
                                                D3D12_RESOURCE_STATES before,
                                                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

constexpr UINT64 kRecordingPoolBudgetBytes =
    384ull * 1024ull * 1024ull;
constexpr std::size_t kRecordingPoolMaxSlots = 48;

Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(const char* source,
                                               const char* entryPoint,
                                               const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source,
                                      std::strlen(source),
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      entryPoint,
                                      target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                      0,
                                      &shader,
                                      &errors);
    if (FAILED(result)) {
        const std::string detail =
            errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                 errors->GetBufferSize())
                   : std::string();
        throw std::runtime_error(
            std::string("Failed to compile viewport shader: ") + detail);
    }
    return shader;
}

} // namespace

struct RecordingFramePoolState {
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> annotationTexture;
        Microsoft::WRL::ComPtr<ID3D12Resource> annotationUpload;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT annotationFootprint{};
        UINT8* annotationUploadMapped{};
        HANDLE textureHandle{nullptr};
        UINT width{};
        UINT height{};
        UINT64 allocationBytes{};
        bool supportsAnnotations{};
        bool inUse{};

        ~Slot()
        {
            if (annotationUpload && annotationUploadMapped) {
                annotationUpload->Unmap(0, nullptr);
            }
            if (textureHandle) {
                CloseHandle(textureHandle);
            }
        }
    };

    RecordingFramePoolState(ID3D12Device* device, ID3D12Fence* fence)
        : device_(device), fence_(fence)
    {
        ThrowIfFailed(
            device_->CreateSharedHandle(
                fence_.Get(),
                nullptr,
                GENERIC_ALL,
                nullptr,
                &fenceHandle_),
            "Failed to share GPU recording fence");
    }

    ~RecordingFramePoolState()
    {
        if (fenceHandle_) {
            CloseHandle(fenceHandle_);
        }
    }

    std::shared_ptr<Slot> Acquire(UINT width,
                                  UINT height,
                                  bool needsAnnotations)
    {
        std::lock_guard lock(mutex_);
        for (const auto& slot : slots_) {
            if (!slot->inUse &&
                slot->width == width &&
                slot->height == height &&
                (!needsAnnotations || slot->supportsAnnotations)) {
                slot->inUse = true;
                return slot;
            }
        }

        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDesc.Flags =
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        const UINT64 outputAllocationBytes =
            device_->GetResourceAllocationInfo(
                0, 1, &textureDesc).SizeInBytes;
        D3D12_RESOURCE_DESC annotationDesc = textureDesc;
        annotationDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT annotationFootprint{};
        UINT annotationRows = 0;
        UINT64 annotationRowBytes = 0;
        UINT64 annotationUploadBytes = 0;
        device_->GetCopyableFootprints(
            &annotationDesc,
            0,
            1,
            0,
            &annotationFootprint,
            &annotationRows,
            &annotationRowBytes,
            &annotationUploadBytes);
        const UINT64 annotationAllocationBytes =
            needsAnnotations
                ? device_->GetResourceAllocationInfo(
                      0, 1, &annotationDesc).SizeInBytes +
                      annotationUploadBytes
                : 0;
        const UINT64 allocationBytes =
            outputAllocationBytes + annotationAllocationBytes;

        // Free idle textures of obsolete dimensions before rejecting a new
        // recording canvas. In-use entries remain until the encoder releases
        // the corresponding Media Foundation sample.
        for (auto it = slots_.begin();
             it != slots_.end() &&
             (allocatedBytes_ + allocationBytes >
                  kRecordingPoolBudgetBytes ||
              slots_.size() >= kRecordingPoolMaxSlots);) {
            const auto& slot = *it;
            if (!slot->inUse &&
                (slot->width != width || slot->height != height ||
                 (needsAnnotations && !slot->supportsAnnotations))) {
                allocatedBytes_ -= slot->allocationBytes;
                it = slots_.erase(it);
            } else {
                ++it;
            }
        }
        if (allocatedBytes_ + allocationBytes >
                kRecordingPoolBudgetBytes ||
            slots_.size() >= kRecordingPoolMaxSlots) {
            return {};
        }

        auto slot = std::make_shared<Slot>();
        slot->width = width;
        slot->height = height;
        slot->allocationBytes = allocationBytes;
        slot->supportsAnnotations = needsAnnotations;
        slot->inUse = true;

        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = textureDesc.Format;
        clearValue.Color[3] = 1.0f;
        ThrowIfFailed(
            device_->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_SHARED,
                &textureDesc,
                D3D12_RESOURCE_STATE_COMMON,
                &clearValue,
                IID_PPV_ARGS(&slot->texture)),
            "Failed to create pooled GPU recording frame");
        ThrowIfFailed(
            device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&slot->allocator)),
            "Failed to create pooled recording command allocator");

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.NumDescriptors = needsAnnotations ? 2 : 1;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(
            device_->CreateDescriptorHeap(
                &srvHeapDesc, IID_PPV_ARGS(&slot->srvHeap)),
            "Failed to create pooled recording SRV heap");
        if (needsAnnotations) {
            D3D12_HEAP_PROPERTIES defaultHeap{};
            defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            ThrowIfFailed(
                device_->CreateCommittedResource(
                    &defaultHeap,
                    D3D12_HEAP_FLAG_NONE,
                    &annotationDesc,
                    D3D12_RESOURCE_STATE_COMMON,
                    nullptr,
                    IID_PPV_ARGS(&slot->annotationTexture)),
                "Failed to create recording annotation texture");

            D3D12_HEAP_PROPERTIES uploadHeap{};
            uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC uploadDesc{};
            uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            uploadDesc.Width = annotationUploadBytes;
            uploadDesc.Height = 1;
            uploadDesc.DepthOrArraySize = 1;
            uploadDesc.MipLevels = 1;
            uploadDesc.SampleDesc.Count = 1;
            uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ThrowIfFailed(
                device_->CreateCommittedResource(
                    &uploadHeap,
                    D3D12_HEAP_FLAG_NONE,
                    &uploadDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&slot->annotationUpload)),
                "Failed to create recording annotation upload buffer");
            ThrowIfFailed(
                slot->annotationUpload->Map(
                    0,
                    nullptr,
                    reinterpret_cast<void**>(
                        &slot->annotationUploadMapped)),
                "Failed to map recording annotation upload buffer");
            slot->annotationFootprint = annotationFootprint;
        }
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 1;
        ThrowIfFailed(
            device_->CreateDescriptorHeap(
                &rtvHeapDesc, IID_PPV_ARGS(&slot->rtvHeap)),
            "Failed to create pooled recording RTV heap");
        device_->CreateRenderTargetView(
            slot->texture.Get(),
            nullptr,
            slot->rtvHeap->GetCPUDescriptorHandleForHeapStart());
        ThrowIfFailed(
            device_->CreateSharedHandle(
                slot->texture.Get(),
                nullptr,
                GENERIC_ALL,
                nullptr,
                &slot->textureHandle),
            "Failed to share pooled GPU recording texture");

        allocatedBytes_ += allocationBytes;
        slots_.push_back(slot);
        return slot;
    }

    void Release(const std::shared_ptr<Slot>& slot)
    {
        std::lock_guard lock(mutex_);
        slot->inUse = false;
    }

    // A failed drain cannot safely free textures or invalidate encoder leases.
    // A self-reference preserves the complete pool without allocating during
    // teardown. The OS reclaims this terminal-fault quarantine at process exit.
    void RetainUntilProcessExit(const std::shared_ptr<RecordingFramePoolState>& self)
    {
        quarantineSelf_ = self;
    }

    HANDLE FenceHandle() const
    {
        return fenceHandle_;
    }

private:
    std::shared_ptr<RecordingFramePoolState> quarantineSelf_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceHandle_{nullptr};
    std::mutex mutex_;
    std::vector<std::shared_ptr<Slot>> slots_;
    UINT64 allocatedBytes_{};
};

namespace {

struct RecordingFramePoolLease {
    std::shared_ptr<RecordingFramePoolState> pool;
    std::shared_ptr<RecordingFramePoolState::Slot> slot;

    ~RecordingFramePoolLease()
    {
        if (pool && slot) {
            pool->Release(slot);
        }
    }
};

} // namespace

D3D12Presenter::D3D12Presenter() = default;

D3D12Presenter::~D3D12Presenter()
{
    if (faulted_ || (commandQueue_ && fence_ && !WaitForGpu())) {
        QuarantineResources();
        return;
    }
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (uploadBuffers_[i]) {
            uploadBuffers_[i]->Unmap(0, nullptr);
            uploadBuffers_[i].Reset();
            uploadMappedPtrs_[i] = nullptr;
        }
    }
    // The drain above retired any in-flight async readbacks, so the ring
    // buffers and allocators can be released safely.
    for (auto& slot : asyncReadbackSlots_) {
        slot.buffer.Reset();
        slot.allocator.Reset();
        slot.inFlight = false;
    }
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    if (frameLatencyWaitableObject_) {
        CloseHandle(frameLatencyWaitableObject_);
        frameLatencyWaitableObject_ = nullptr;
    }
}

void D3D12Presenter::Initialize(HWND hwnd, UINT width, UINT height)
try
{
    if (faulted_) return;
    hwnd_ = hwnd;
    CreateDevice();
    CreateCommandObjects();
    CreateFenceObjects();
    CreateViewportPipeline();
    CreateSwapChain(width, height);
    if (!EnsureUploadBuffer(width, height)) return;
    initialized_ = true;
}
catch (const std::exception& error)
{
    qWarning() << "D3D12 initialization failed:" << error.what();
    MarkFenceFault("GPU initialization failed");
}
catch (...)
{
    MarkFenceFault("Unexpected GPU initialization failure");
}

bool D3D12Presenter::IsInitialized() const
{
    return initialized_;
}

bool D3D12Presenter::IsFaulted() const
{
    return faulted_;
}

bool D3D12Presenter::NeedsScenePresent() const
{
    return scenePresentNeeded_;
}

UINT D3D12Presenter::ViewportWidth() const
{
    return width_;
}

UINT D3D12Presenter::ViewportHeight() const
{
    return height_;
}

std::uint64_t D3D12Presenter::MissedPresentCount() const
{
    return missedPresentCount_;
}

void D3D12Presenter::Resize(UINT width, UINT height)
try
{
    if (!initialized_ || width == 0 || height == 0) {
        return;
    }

    if (!WaitForGpu()) return;
    // The drain retired any in-flight async readbacks; their contents are for
    // the old dimensions, so drop them. Callers simply never receive a
    // TryGetCompletedReadback result for those frames.
    for (auto& slot : asyncReadbackSlots_) {
        slot.inFlight = false;
    }
    for (auto& buffer : backBuffers_) {
        buffer.Reset();
    }

    ThrowIfFailed(swapChain_->ResizeBuffers(static_cast<UINT>(backBuffers_.size()),
                                            width,
                                            height,
                                            DXGI_FORMAT_B8G8R8A8_UNORM,
                                            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),
                  "Failed to resize swap chain buffers");

    AcquireBackBuffers();
    width_ = width;
    height_ = height;
    scenePresentNeeded_ = true;
}
catch (const std::exception& error)
{
    qWarning() << "D3D12 operation failed:" << error.what();
    MarkFenceFault("GPU operation failed");
    return;
}
catch (...)
{
    MarkFenceFault("Unexpected GPU operation failure");
    return;
}

void D3D12Presenter::Present(const uint8_t* data, UINT width, UINT height)
try
{
    if (!initialized_ || !data) {
        return;
    }

    if (width == 0 || height == 0) {
        return;
    }

    if (width != width_ || height != height_) {
        Resize(width, height);
    }

    if (!initialized_ || !EnsureUploadBuffer(width, height)) return;

    // Retry on a later UI tick if this slot's previous GPU work or the
    // swap-chain admission signal is not ready. Never wait to reuse resources.
    const UINT backIndex = swapChain_->GetCurrentBackBufferIndex();
    if (!TryAcquireFrameSlot(backIndex)) {
        scenePresentNeeded_ = !faulted_;
        return;
    }

    CopyToUpload(data, width, height, backIndex);

    ID3D12CommandAllocator* allocator = frameCommandAllocators_[backIndex].Get();
    ThrowIfFailed(allocator->Reset(), "Failed to reset command allocator");
    ThrowIfFailed(commandList_->Reset(allocator, nullptr), "Failed to reset command list");

    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer = backBuffers_[backIndex];

    auto barrierToCopy = TransitionBarrier(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList_->ResourceBarrier(1, &barrierToCopy);

    D3D12_TEXTURE_COPY_LOCATION dest{};
    dest.pResource = backBuffer.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dest.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = uploadBuffers_[backIndex].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = uploadFootprint_;

    commandList_->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);

    auto barrierToPresent = TransitionBarrier(backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    commandList_->ResourceBarrier(1, &barrierToPresent);

    ThrowIfFailed(commandList_->Close(), "Failed to close command list");

    ID3D12CommandList* lists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(static_cast<UINT>(std::size(lists)), lists);

    ThrowIfFailed(swapChain_->Present(1, 0), "Failed to present swap chain");

    const UINT64 signalValue = ++fenceValue_;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), signalValue), "Failed to signal fence");
    frameFenceValues_[backIndex] = signalValue;
    scenePresentNeeded_ = false;
}
catch (const std::exception& error)
{
    qWarning() << "D3D12 operation failed:" << error.what();
    MarkFenceFault("GPU operation failed");
    return;
}
catch (...)
{
    MarkFenceFault("Unexpected GPU operation failure");
    return;
}

void D3D12Presenter::PresentFromTexture(ID3D12Resource* texture,
                                        UINT width,
                                        UINT height,
                                        const FenceSyncParams* fenceSync)
{
    const ViewTransform transform =
        ComputeViewTransform(width,
                             height,
                             width_,
                             height_,
                             1.0f,
                             0.5f,
                             0.5f,
                             ViewportFitMode::kFill);
    PresentSceneTexture(texture, width, height, transform, fenceSync);
}

bool D3D12Presenter::PresentSceneTexture(ID3D12Resource* texture,
                                        UINT sourceWidth,
                                        UINT sourceHeight,
                                        const ViewTransform& transform,
                                        const FenceSyncParams* fenceSync,
                                        const ViewportPresentationOptions* options,
                                        UINT64* outReadbackRequestId)
try
{
    if (outReadbackRequestId) {
        *outReadbackRequestId = 0;
    }
    if (!initialized_ || !texture || sourceWidth == 0 || sourceHeight == 0 ||
        width_ == 0 || height_ == 0 || !transform.valid) {
        return false;
    }

    const bool useFenceSync = fenceSync && fenceSync->enable && fence_.Get() != nullptr;
    const UINT backIndex = swapChain_->GetCurrentBackBufferIndex();
    if (!TryAcquireFrameSlot(backIndex)) {
        scenePresentNeeded_ = !faulted_;
        return false;
    }
    AsyncReadbackSlot* readbackSlot =
        options && options->requestReadback
            ? PrepareAsyncReadbackSlot(width_, height_)
            : nullptr;

    ID3D12CommandAllocator* allocator = frameCommandAllocators_[backIndex].Get();
    ThrowIfFailed(allocator->Reset(), "Failed to reset command allocator");
    ThrowIfFailed(commandList_->Reset(allocator, nullptr), "Failed to reset command list");

    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer = backBuffers_[backIndex];

    if (useFenceSync && fenceSync->waitValue > 0) {
        ThrowIfFailed(commandQueue_->Wait(fence_.Get(), fenceSync->waitValue),
                      "Failed to queue wait on shared fence");
    }

    auto transitionSourceToSample = TransitionBarrier(
        texture,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList_->ResourceBarrier(1, &transitionSourceToSample);

    auto transitionDestToRender = TransitionBarrier(backBuffer.Get(),
                                                    D3D12_RESOURCE_STATE_PRESENT,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList_->ResourceBarrier(1, &transitionDestToRender);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu =
        sceneSrvHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(backIndex) * sceneSrvDescriptorSize_;
    device_->CreateShaderResourceView(texture, &srvDesc, srvCpu);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        renderTargetHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(backIndex) * renderTargetDescriptorSize_;

    D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    D3D12_RECT scissor{
        0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    commandList_->SetGraphicsRootSignature(sceneRootSignature_.Get());
    ID3D12DescriptorHeap* descriptorHeaps[] = {sceneSrvHeap_.Get()};
    commandList_->SetDescriptorHeaps(1, descriptorHeaps);
    const float constants[16] = {
        transform.sourceX,
        transform.sourceY,
        transform.sourceWidth,
        transform.sourceHeight,
        transform.destinationX,
        transform.destinationY,
        transform.destinationWidth,
        transform.destinationHeight,
        options ? options->focusX : 0.5f,
        options ? options->focusY : 0.5f,
        static_cast<float>(width_),
        static_cast<float>(height_),
        options && options->drawFocusMarker ? 1.0f : 0.0f,
        0.0f,
        0.0f,
        0.0f,
    };
    commandList_->SetGraphicsRoot32BitConstants(0, 16, constants, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu =
        sceneSrvHeap_->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += static_cast<UINT64>(backIndex) * sceneSrvDescriptorSize_;
    commandList_->SetGraphicsRootDescriptorTable(1, srvGpu);
    commandList_->RSSetViewports(1, &viewport);
    commandList_->RSSetScissorRects(1, &scissor);
    commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commandList_->SetPipelineState(scenePipelineState_.Get());
    commandList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList_->DrawInstanced(3, 1, 0, 0);

    if (readbackSlot) {
        auto transitionDestToCopy = TransitionBarrier(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList_->ResourceBarrier(1, &transitionDestToCopy);

        D3D12_TEXTURE_COPY_LOCATION readbackDestination{};
        readbackDestination.pResource = readbackSlot->buffer.Get();
        readbackDestination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        readbackDestination.PlacedFootprint = readbackSlot->footprint;

        D3D12_TEXTURE_COPY_LOCATION renderedSource{};
        renderedSource.pResource = backBuffer.Get();
        renderedSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        renderedSource.SubresourceIndex = 0;
        commandList_->CopyTextureRegion(
            &readbackDestination, 0, 0, 0, &renderedSource, nullptr);

        auto transitionDestToPresent = TransitionBarrier(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_PRESENT);
        commandList_->ResourceBarrier(1, &transitionDestToPresent);
    } else {
        auto transitionDestToPresent = TransitionBarrier(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        commandList_->ResourceBarrier(1, &transitionDestToPresent);
    }

    auto transitionSourceToCommon = TransitionBarrier(texture,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                      D3D12_RESOURCE_STATE_COMMON);
    commandList_->ResourceBarrier(1, &transitionSourceToCommon);

    ThrowIfFailed(commandList_->Close(), "Failed to close command list");

    ID3D12CommandList* lists[] = { commandList_.Get() };
    if (!KeepSubmittedResource(texture)) return false;
    commandQueue_->ExecuteCommandLists(static_cast<UINT>(std::size(lists)), lists);

    if (useFenceSync) {
        // Signal the shared fence as soon as the sampled draw is queued so
        // CUDA can begin its next frame once texture reads have retired.
        const UINT64 textureSignal =
            std::max({fenceValue_ + 1,
                      fenceSync->waitValue + 1,
                      fenceSync->signalValue});
        ThrowIfFailed(commandQueue_->Signal(fence_.Get(), textureSignal),
                      "Failed to signal shared fence");
        fenceValue_ = textureSignal;

        ThrowIfFailed(swapChain_->Present(1, 0), "Failed to present swap chain");

        // Extra internal signal after Present paces this frame slot's reuse.
        const UINT64 slotSignal = ++fenceValue_;
        ThrowIfFailed(commandQueue_->Signal(fence_.Get(), slotSignal), "Failed to signal fence");
        frameFenceValues_[backIndex] = slotSignal;
        if (readbackSlot) {
            readbackSlot->fenceValue = slotSignal;
            readbackSlot->inFlight = true;
            if (outReadbackRequestId) {
                *outReadbackRequestId = slotSignal;
            }
        }
    } else {
        // Without the shared external semaphore there is no cross-API sync:
        // the caller's CUDA stream may write the source texture again as soon
        // as we return, so the queued draw must fully complete first.
        if (!WaitForGpu()) return false;
        ThrowIfFailed(swapChain_->Present(1, 0), "Failed to present swap chain");
        frameFenceValues_[backIndex] = fenceValue_;
        if (readbackSlot) {
            readbackSlot->fenceValue = fenceValue_;
            readbackSlot->inFlight = true;
            if (outReadbackRequestId) {
                *outReadbackRequestId = fenceValue_;
            }
        }
    }
    scenePresentNeeded_ = false;
    return true;
}
catch (const std::exception& error)
{
    qWarning() << "D3D12 operation failed:" << error.what();
    MarkFenceFault("GPU operation failed");
    return false;
}
catch (...)
{
    MarkFenceFault("Unexpected GPU operation failure");
    return false;
}

ID3D12Device* D3D12Presenter::GetDevice() const
{
    return device_.Get();
}

ID3D12Fence* D3D12Presenter::GetFence() const
{
    return fence_.Get();
}

UINT64 D3D12Presenter::GetLastSignaledFenceValue() const
{
    return fenceValue_;
}

void D3D12Presenter::CreateDevice()
{
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)),
                  "Failed to create DXGI factory");
    factory_ = factory;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;

    HRESULT hr = factory->EnumAdapterByGpuPreference(0,
                                                     DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                     IID_PPV_ARGS(&adapter));
    if (FAILED(hr) || !adapter) {
        for (UINT adapterIndex = 0;
             factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND;
             ++adapterIndex) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                            _uuidof(ID3D12Device), nullptr))) {
                qInfo() << "Selected DXGI adapter" << QString::fromWCharArray(desc.Description);
                break;
            }
        }
    } else {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter.Reset();
        } else {
            qInfo() << "Selected high-performance DXGI adapter" << QString::fromWCharArray(desc.Description);
        }
    }

    if (!adapter) {
        ThrowIfFailed(E_FAIL, "Failed to locate hardware adapter for D3D12");
    }

    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
                  "Failed to create D3D12 device");
}

void D3D12Presenter::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue_)),
                  "Failed to create D3D12 command queue");

    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&frameCommandAllocators_[i])),
                      "Failed to create frame command allocator");
    }
    ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&readbackCommandAllocator_)),
                  "Failed to create readback command allocator");
    for (auto& slot : asyncReadbackSlots_) {
        ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&slot.allocator)),
                      "Failed to create async readback command allocator");
    }

    ThrowIfFailed(device_->CreateCommandList(0,
                                             D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             frameCommandAllocators_[0].Get(),
                                             nullptr,
                                             IID_PPV_ARGS(&commandList_)),
                  "Failed to create command list");
    commandList_->Close();
}

void D3D12Presenter::CreateFenceObjects()
{
    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_)),
                  "Failed to create fence");
    fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "Failed to create fence event");
    }
}

void D3D12Presenter::CreateSwapChain(UINT width, UINT height)
{
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    // The old back buffer may outlive the HWND size by one resize tick. Keep
    // its pixel geometry instead of stretching it into the new aspect ratio.
    scDesc.Scaling = DXGI_SCALING_NONE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory_->CreateSwapChainForHwnd(commandQueue_.Get(),
                                                   hwnd_,
                                                   &scDesc,
                                                   nullptr,
                                                   nullptr,
                                                   &swapChain1),
                  "Failed to create swap chain");

    ThrowIfFailed(factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER),
                  "Failed to disable Alt-Enter");

    ThrowIfFailed(swapChain1.As(&swapChain_), "Failed to query IDXGISwapChain3");
    Microsoft::WRL::ComPtr<IDXGISwapChain2> latencySwapChain;
    if (SUCCEEDED(swapChain_.As(&latencySwapChain)) && latencySwapChain) {
        ThrowIfFailed(latencySwapChain->SetMaximumFrameLatency(1),
                      "Failed to set swap-chain frame latency");
        frameLatencyWaitableObject_ =
            latencySwapChain->GetFrameLatencyWaitableObject();
    }

    backBuffers_.resize(scDesc.BufferCount);
    AcquireBackBuffers();
    width_ = width;
    height_ = height;
}

void D3D12Presenter::CreateViewportPipeline()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device_->CreateDescriptorHeap(
                      &rtvHeapDesc, IID_PPV_ARGS(&renderTargetHeap_)),
                  "Failed to create viewport render-target heap");
    renderTargetDescriptorSize_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = kFrameCount;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device_->CreateDescriptorHeap(
                      &srvHeapDesc, IID_PPV_ARGS(&sceneSrvHeap_)),
                  "Failed to create viewport shader-resource heap");
    sceneSrvDescriptorSize_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[2]{};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[0].Constants.ShaderRegister = 0;
    rootParameters[0].Constants.Num32BitValues = 16;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(std::size(rootParameters));
    rootDesc.pParameters = rootParameters;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    Microsoft::WRL::ComPtr<ID3DBlob> serializedRoot;
    Microsoft::WRL::ComPtr<ID3DBlob> rootErrors;
    const HRESULT rootResult = D3D12SerializeRootSignature(
        &rootDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRoot,
        &rootErrors);
    if (FAILED(rootResult)) {
        const std::string detail =
            rootErrors
                ? std::string(
                      static_cast<const char*>(rootErrors->GetBufferPointer()),
                      rootErrors->GetBufferSize())
                : std::string();
        throw std::runtime_error(
            std::string("Failed to serialize viewport root signature: ") + detail);
    }
    ThrowIfFailed(device_->CreateRootSignature(
                      0,
                      serializedRoot->GetBufferPointer(),
                      serializedRoot->GetBufferSize(),
                      IID_PPV_ARGS(&sceneRootSignature_)),
                  "Failed to create viewport root signature");

    static constexpr char kSceneShader[] = R"(
cbuffer ViewConstants : register(b0) {
    float4 sourceRect;
    float4 destinationRect;
    float4 overlayData;
    float4 overlayOptions;
};
Texture2D<float4> sceneTexture : register(t0);
SamplerState sceneSampler : register(s0);

struct VertexOutput {
    float4 position : SV_Position;
    float2 viewportUv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    const float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.viewportUv = uvs[vertexId];
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target {
    const float2 destinationMin = destinationRect.xy;
    const float2 destinationMax = destinationRect.xy + destinationRect.zw;
    if (any(input.viewportUv < destinationMin) ||
        any(input.viewportUv > destinationMax)) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 localUv =
        (input.viewportUv - destinationRect.xy) / destinationRect.zw;
    const float2 sourceUv = sourceRect.xy + localUv * sourceRect.zw;
    float4 color = sceneTexture.Sample(sceneSampler, sourceUv);

    if (overlayOptions.x > 0.5) {
        const float2 focusLocal =
            (overlayData.xy - sourceRect.xy) / sourceRect.zw;
        const float2 focusViewport =
            destinationRect.xy + focusLocal * destinationRect.zw;
        const float2 deltaPixels =
            (input.viewportUv - focusViewport) * overlayData.zw;
        const float distancePixels = length(deltaPixels);
        if (distancePixels <= 18.0) {
            color = distancePixels <= 6.0
                        ? float4(1.0, 1.0, 1.0, 1.0)
                        : float4(0.95, 0.08, 0.08, 1.0);
        }
    }
    return color;
}
)";

    const Microsoft::WRL::ComPtr<ID3DBlob> vertexShader =
        CompileShader(kSceneShader, "VSMain", "vs_5_0");
    const Microsoft::WRL::ComPtr<ID3DBlob> pixelShader =
        CompileShader(kSceneShader, "PSMain", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = sceneRootSignature_.Get();
    pipelineDesc.VS = {
        vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    pipelineDesc.PS = {
        pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    pipelineDesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pipelineDesc.SampleMask = UINT_MAX;
    pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
    pipelineDesc.DepthStencilState.DepthEnable = FALSE;
    pipelineDesc.DepthStencilState.StencilEnable = FALSE;
    pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDesc.NumRenderTargets = 1;
    pipelineDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pipelineDesc.SampleDesc.Count = 1;
    ThrowIfFailed(device_->CreateGraphicsPipelineState(
                      &pipelineDesc, IID_PPV_ARGS(&scenePipelineState_)),
                  "Failed to create viewport pipeline state");

    D3D12_RENDER_TARGET_BLEND_DESC& annotationBlend =
        pipelineDesc.BlendState.RenderTarget[0];
    annotationBlend.BlendEnable = TRUE;
    annotationBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    annotationBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    annotationBlend.BlendOp = D3D12_BLEND_OP_ADD;
    annotationBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    annotationBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    annotationBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(
            &pipelineDesc,
            IID_PPV_ARGS(&annotationPipelineState_)),
        "Failed to create annotation recording pipeline state");
}

void D3D12Presenter::AcquireBackBuffers()
{
    for (UINT i = 0; i < backBuffers_.size(); ++i) {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])),
                      "Failed to retrieve swap chain buffer");
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            renderTargetHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(i) * renderTargetDescriptorSize_;
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, rtv);
    }
}

bool D3D12Presenter::EnsureUploadBuffer(UINT width, UINT height)
{
    if (uploadBuffers_[0] && uploadWidth_ == width && uploadHeight_ == height) {
        return true;
    }

    if (uploadBuffers_[0]) {
        // In-flight frames may still be copying from the old buffers.
        if (!WaitForGpu()) return false;
        for (UINT i = 0; i < kFrameCount; ++i) {
            if (uploadBuffers_[i]) {
                uploadBuffers_[i]->Unmap(0, nullptr);
                uploadBuffers_[i].Reset();
                uploadMappedPtrs_[i] = nullptr;
            }
        }
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    device_->GetCopyableFootprints(&textureDesc,
                                   0,
                                   1,
                                   0,
                                   &uploadFootprint_,
                                   &uploadNumRows_,
                                   &uploadRowSizeInBytes_,
                                   &uploadTotalBytes_);

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = uploadTotalBytes_;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(device_->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &bufferDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(&uploadBuffers_[i])),
                      "Failed to create upload buffer");

        ThrowIfFailed(uploadBuffers_[i]->Map(0, nullptr, reinterpret_cast<void**>(&uploadMappedPtrs_[i])),
                      "Failed to map upload buffer");
    }

    uploadWidth_ = width;
    uploadHeight_ = height;
    return true;
}

bool D3D12Presenter::ReadbackTexture(ID3D12Resource* texture,
                                     UINT width,
                                     UINT height,
                                     std::vector<uint8_t>& outBgra,
                                     UINT64 waitFenceValue)
try
{
    if (!initialized_ || !texture || width == 0 || height == 0) {
        return false;
    }

    if (width != readbackWidth_ || height != readbackHeight_ || !readbackBuffer_) {
        // Allocate readback buffer
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        device_->GetCopyableFootprints(&desc,
                                       0,
                                       1,
                                       0,
                                       &readbackFootprint_,
                                       &readbackNumRows_,
                                       &readbackRowSizeInBytes_,
                                       &readbackTotalBytes_);

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = readbackTotalBytes_;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        readbackBuffer_.Reset();
        ThrowIfFailed(device_->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       IID_PPV_ARGS(&readbackBuffer_)),
                      "Failed to create readback buffer");
        readbackWidth_ = width;
        readbackHeight_ = height;
    }

    // Dedicated allocator: every readback ends with a full WaitForGpu, so this
    // allocator is always idle on entry even while present frames are in flight.
    ThrowIfFailed(readbackCommandAllocator_->Reset(), "Failed to reset readback command allocator");
    ThrowIfFailed(commandList_->Reset(readbackCommandAllocator_.Get(), nullptr), "Failed to reset command list");

    auto transitionSourceToCopy = TransitionBarrier(texture,
                                                    D3D12_RESOURCE_STATE_COMMON,
                                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList_->ResourceBarrier(1, &transitionSourceToCopy);

    D3D12_TEXTURE_COPY_LOCATION dest{};
    dest.pResource = readbackBuffer_.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dest.PlacedFootprint = readbackFootprint_;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    commandList_->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);

    auto transitionSourceToCommon = TransitionBarrier(texture,
                                                      D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                      D3D12_RESOURCE_STATE_COMMON);
    commandList_->ResourceBarrier(1, &transitionSourceToCommon);

    ThrowIfFailed(commandList_->Close(), "Failed to close command list");

    if (waitFenceValue > 0 && fence_) {
        // CUDA signals this shared fence after its writes retire. Queue the
        // dependency before the copy instead of blocking the CPU or racing a
        // still-active CUDA stream.
        ThrowIfFailed(commandQueue_->Wait(fence_.Get(), waitFenceValue),
                      "Failed to queue CUDA wait before texture readback");
        fenceValue_ = std::max(fenceValue_, waitFenceValue);
    }

    ID3D12CommandList* lists[] = { commandList_.Get() };
    if (!KeepSubmittedResource(texture)) return false;
    commandQueue_->ExecuteCommandLists(static_cast<UINT>(std::size(lists)), lists);

    if (!WaitForGpu()) return false;

    outBgra.resize(static_cast<size_t>(width) * height * 4u);
    uint8_t* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(readbackTotalBytes_)};
    ThrowIfFailed(readbackBuffer_->Map(0, &range, reinterpret_cast<void**>(&mapped)),
                  "Failed to map readback buffer");

    const uint8_t* srcData = mapped + readbackFootprint_.Offset;
    const size_t srcPitch = readbackFootprint_.Footprint.RowPitch;
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* srcRow = srcData + static_cast<size_t>(y) * srcPitch;
        uint8_t* dstRow = outBgra.data() + static_cast<size_t>(y) * width * 4u;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(width) * 4u);
    }
    readbackBuffer_->Unmap(0, nullptr);
    return true;
}
catch (const std::exception& error)
{
    qWarning() << "D3D12 operation failed:" << error.what();
    MarkFenceFault("GPU operation failed");
    return false;
}
catch (...)
{
    MarkFenceFault("Unexpected GPU operation failure");
    return false;
}

D3D12Presenter::AsyncReadbackSlot*
D3D12Presenter::PrepareAsyncReadbackSlot(UINT width, UINT height)
{
    if (!initialized_ || width == 0 || height == 0) {
        return nullptr;
    }
    AsyncReadbackSlot* slot = nullptr;
    for (auto& candidate : asyncReadbackSlots_) {
        if (!candidate.inFlight) {
            slot = &candidate;
            break;
        }
    }
    if (!slot) {
        return nullptr;
    }

    if (!slot->buffer || slot->width != width || slot->height != height) {
        // Safe to recreate: the slot is free, so its previous copy (if any)
        // has fully retired on the GPU.
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        device_->GetCopyableFootprints(&desc,
                                       0,
                                       1,
                                       0,
                                       &slot->footprint,
                                       &numRows,
                                       &rowSizeInBytes,
                                       &slot->totalBytes);

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = slot->totalBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        slot->buffer.Reset();
        ThrowIfFailed(device_->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr,
                                                       IID_PPV_ARGS(&slot->buffer)),
                      "Failed to create async readback buffer");
        slot->width = width;
        slot->height = height;
    }
    return slot;
}

bool D3D12Presenter::RequestReadback(ID3D12Resource* texture,
                                     UINT width,
                                     UINT height,
                                     UINT64* outRequestId)
try
{
    if (!initialized_ || !texture || width == 0 || height == 0) {
        return false;
    }

    AsyncReadbackSlot* slot = PrepareAsyncReadbackSlot(width, height);
    if (!slot) {
        // Both copies still in flight; the caller skips this frame's readback.
        return false;
    }

    // The slot's previous submission retired before it became free, so the
    // allocator is idle and safe to reset without any CPU wait.
    ThrowIfFailed(slot->allocator->Reset(), "Failed to reset async readback command allocator");
    ThrowIfFailed(commandList_->Reset(slot->allocator.Get(), nullptr), "Failed to reset command list");

    auto transitionSourceToCopy = TransitionBarrier(texture,
                                                    D3D12_RESOURCE_STATE_COMMON,
                                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList_->ResourceBarrier(1, &transitionSourceToCopy);

    D3D12_TEXTURE_COPY_LOCATION dest{};
    dest.pResource = slot->buffer.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dest.PlacedFootprint = slot->footprint;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    commandList_->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);

    auto transitionSourceToCommon = TransitionBarrier(texture,
                                                      D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                      D3D12_RESOURCE_STATE_COMMON);
    commandList_->ResourceBarrier(1, &transitionSourceToCommon);

    ThrowIfFailed(commandList_->Close(), "Failed to close command list");

    ID3D12CommandList* lists[] = { commandList_.Get() };
    if (!KeepSubmittedResource(texture)) return false;
    commandQueue_->ExecuteCommandLists(static_cast<UINT>(std::size(lists)), lists);

    const UINT64 signalValue = ++fenceValue_;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), signalValue), "Failed to signal fence");
    slot->fenceValue = signalValue;
    slot->inFlight = true;
    if (outRequestId) {
        *outRequestId = signalValue;
    }
    return true;
}
catch (const std::exception& error)
{
    qWarning() << "D3D12 operation failed:" << error.what();
    MarkFenceFault("GPU operation failed");
    return false;
}
catch (...)
{
    MarkFenceFault("Unexpected GPU operation failure");
    return false;
}

GpuVideoFrame D3D12Presenter::RequestRecordingFrame(
    ID3D12Resource* texture,
    UINT sourceWidth,
    UINT sourceHeight,
    const RecordingViewTransform& transform,
    UINT targetWidth,
    UINT targetHeight,
    const uint8_t* annotationBgra,
    std::size_t annotationStrideBytes,
    bool* outPoolExhausted,
    UINT64 waitFenceValue)
{
    GpuVideoFrame frame;
    if (outPoolExhausted) {
        *outPoolExhausted = false;
    }
    if (!initialized_ || !texture || !fence_ ||
        sourceWidth == 0 || sourceHeight == 0 ||
        targetWidth == 0 || targetHeight == 0 ||
        !transform.valid) {
        return frame;
    }

    try {
        if (!recordingFramePool_) {
            recordingFramePool_ =
                std::make_shared<RecordingFramePoolState>(
                    device_.Get(), fence_.Get());
        }
        const bool hasAnnotations =
            annotationBgra != nullptr &&
            annotationStrideBytes >=
                static_cast<std::size_t>(targetWidth) * 4u;
        auto slot = recordingFramePool_->Acquire(
            targetWidth, targetHeight, hasAnnotations);
        if (!slot) {
            if (outPoolExhausted) {
                *outPoolExhausted = true;
            }
            return {};
        }
        auto lease = std::make_shared<RecordingFramePoolLease>();
        lease->pool = recordingFramePool_;
        lease->slot = slot;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(
            texture,
            &srvDesc,
            slot->srvHeap->GetCPUDescriptorHandleForHeapStart());
        if (hasAnnotations) {
            const std::size_t copyBytes =
                static_cast<std::size_t>(targetWidth) * 4u;
            for (UINT row = 0; row < targetHeight; ++row) {
                std::memcpy(
                    slot->annotationUploadMapped +
                        static_cast<std::size_t>(row) *
                            slot->annotationFootprint.Footprint.RowPitch,
                    annotationBgra +
                        static_cast<std::size_t>(row) *
                            annotationStrideBytes,
                    copyBytes);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE annotationSrv =
                slot->srvHeap->GetCPUDescriptorHandleForHeapStart();
            annotationSrv.ptr += sceneSrvDescriptorSize_;
            device_->CreateShaderResourceView(
                slot->annotationTexture.Get(),
                &srvDesc,
                annotationSrv);
        }

        ThrowIfFailed(
            slot->allocator->Reset(),
            "Failed to reset recording command allocator");
        ThrowIfFailed(
            commandList_->Reset(slot->allocator.Get(), nullptr),
            "Failed to reset recording command list");
        auto sourceToSample = TransitionBarrier(
            texture,
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList_->ResourceBarrier(1, &sourceToSample);
        auto destinationToRenderTarget = TransitionBarrier(
            slot->texture.Get(),
            D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList_->ResourceBarrier(1, &destinationToRenderTarget);
        if (hasAnnotations) {
            auto annotationToCopy = TransitionBarrier(
                slot->annotationTexture.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
            commandList_->ResourceBarrier(1, &annotationToCopy);
            D3D12_TEXTURE_COPY_LOCATION annotationDestination{};
            annotationDestination.pResource =
                slot->annotationTexture.Get();
            annotationDestination.Type =
                D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION annotationSource{};
            annotationSource.pResource = slot->annotationUpload.Get();
            annotationSource.Type =
                D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            annotationSource.PlacedFootprint =
                slot->annotationFootprint;
            commandList_->CopyTextureRegion(
                &annotationDestination,
                0,
                0,
                0,
                &annotationSource,
                nullptr);
            auto annotationToSample = TransitionBarrier(
                slot->annotationTexture.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList_->ResourceBarrier(1, &annotationToSample);
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            slot->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        commandList_->ClearRenderTargetView(
            rtv, clearColor, 0, nullptr);
        const D3D12_VIEWPORT viewport{
            0.0f,
            0.0f,
            static_cast<float>(targetWidth),
            static_cast<float>(targetHeight),
            0.0f,
            1.0f};
        const D3D12_RECT scissor{
            0,
            0,
            static_cast<LONG>(targetWidth),
            static_cast<LONG>(targetHeight)};
        commandList_->SetGraphicsRootSignature(sceneRootSignature_.Get());
        ID3D12DescriptorHeap* heaps[] = {slot->srvHeap.Get()};
        commandList_->SetDescriptorHeaps(1, heaps);
        const float constants[16] = {
            transform.sourceX,
            transform.sourceY,
            transform.sourceWidth,
            transform.sourceHeight,
            transform.targetX,
            transform.targetY,
            transform.targetWidth,
            transform.targetHeight,
            0.5f,
            0.5f,
            static_cast<float>(targetWidth),
            static_cast<float>(targetHeight),
            0.0f,
            0.0f,
            0.0f,
            0.0f,
        };
        commandList_->SetGraphicsRoot32BitConstants(
            0, 16, constants, 0);
        commandList_->SetGraphicsRootDescriptorTable(
            1,
            slot->srvHeap->GetGPUDescriptorHandleForHeapStart());
        commandList_->RSSetViewports(1, &viewport);
        commandList_->RSSetScissorRects(1, &scissor);
        commandList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commandList_->SetPipelineState(scenePipelineState_.Get());
        commandList_->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList_->DrawInstanced(3, 1, 0, 0);
        if (hasAnnotations) {
            const float annotationConstants[16] = {
                0.0f, 0.0f, 1.0f, 1.0f,
                0.0f, 0.0f, 1.0f, 1.0f,
                0.5f, 0.5f,
                static_cast<float>(targetWidth),
                static_cast<float>(targetHeight),
                0.0f, 0.0f, 0.0f, 0.0f,
            };
            commandList_->SetGraphicsRoot32BitConstants(
                0, 16, annotationConstants, 0);
            D3D12_GPU_DESCRIPTOR_HANDLE annotationSrv =
                slot->srvHeap->GetGPUDescriptorHandleForHeapStart();
            annotationSrv.ptr += sceneSrvDescriptorSize_;
            commandList_->SetGraphicsRootDescriptorTable(
                1, annotationSrv);
            commandList_->SetPipelineState(
                annotationPipelineState_.Get());
            commandList_->DrawInstanced(3, 1, 0, 0);
        }

        auto destinationToCommon = TransitionBarrier(
            slot->texture.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COMMON);
        commandList_->ResourceBarrier(1, &destinationToCommon);
        auto sourceToCommon = TransitionBarrier(
            texture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON);
        commandList_->ResourceBarrier(1, &sourceToCommon);
        if (hasAnnotations) {
            auto annotationToCommon = TransitionBarrier(
                slot->annotationTexture.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
            commandList_->ResourceBarrier(1, &annotationToCommon);
        }
        ThrowIfFailed(
            commandList_->Close(),
            "Failed to close recording command list");
        ID3D12CommandList* lists[] = {commandList_.Get()};
        if (!KeepSubmittedResource(texture)) return {};
        if (waitFenceValue > 0) {
            ThrowIfFailed(commandQueue_->Wait(fence_.Get(), waitFenceValue),
                          "Failed to queue recording wait on CUDA fence");
        }
        commandQueue_->ExecuteCommandLists(1, lists);
        const UINT64 readyValue = std::max(fenceValue_, waitFenceValue) + 1;
        fenceValue_ = readyValue;
        ThrowIfFailed(
            commandQueue_->Signal(fence_.Get(), readyValue),
            "Failed to signal GPU recording frame");

        frame.textureSharedHandle = slot->textureHandle;
        frame.fenceSharedHandle = recordingFramePool_->FenceHandle();
        frame.readyFenceValue = readyValue;
        frame.width = targetWidth;
        frame.height = targetHeight;
        frame.lifetime = std::move(lease);
        return frame;
    } catch (const std::exception& error) {
        qWarning() << "GPU recording frame unavailable:" << error.what();
        MarkFenceFault("GPU recording submission failed");
    }
    return {};
}

bool D3D12Presenter::TryGetCompletedReadback(std::vector<uint8_t>& outBgra,
                                             UINT& outWidth,
                                             UINT& outHeight,
                                             UINT64* outRequestId)
try
{
    if (!initialized_ || !fence_) {
        return false;
    }

    const UINT64 completedValue = fence_->GetCompletedValue();
    if (completedValue == UINT64_MAX || FAILED(device_->GetDeviceRemovedReason()))
        return MarkFenceFault("Device lost while polling readback fence");
    AsyncReadbackSlot* oldest = nullptr;
    for (auto& slot : asyncReadbackSlots_) {
        if (slot.inFlight && slot.fenceValue <= completedValue &&
            (!oldest || slot.fenceValue < oldest->fenceValue)) {
            oldest = &slot;
        }
    }
    if (!oldest) {
        return false;
    }

    const UINT width = oldest->width;
    const UINT height = oldest->height;
    outBgra.resize(static_cast<size_t>(width) * height * 4u);
    uint8_t* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(oldest->totalBytes)};
    ThrowIfFailed(oldest->buffer->Map(0, &range, reinterpret_cast<void**>(&mapped)),
                  "Failed to map async readback buffer");

    const uint8_t* srcData = mapped + oldest->footprint.Offset;
    const size_t srcPitch = oldest->footprint.Footprint.RowPitch;
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* srcRow = srcData + static_cast<size_t>(y) * srcPitch;
        uint8_t* dstRow = outBgra.data() + static_cast<size_t>(y) * width * 4u;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(width) * 4u);
    }
    D3D12_RANGE emptyRange{0, 0};
    oldest->buffer->Unmap(0, &emptyRange);

    oldest->inFlight = false;
    outWidth = width;
    outHeight = height;
    if (outRequestId) {
        *outRequestId = oldest->fenceValue;
    }
    return true;
}
catch (const std::exception& error)
{
    qWarning() << "D3D12 operation failed:" << error.what();
    MarkFenceFault("GPU operation failed");
    return false;
}
catch (...)
{
    MarkFenceFault("Unexpected GPU operation failure");
    return false;
}

void D3D12Presenter::CopyToUpload(const uint8_t* data, UINT width, UINT height, UINT slot)
{
    uint8_t* mapped = uploadMappedPtrs_[slot];
    const UINT rowPitch = uploadFootprint_.Footprint.RowPitch;
    const UINT srcPitch = width * 4;
    for (UINT row = 0; row < height; ++row) {
        std::memcpy(mapped + static_cast<size_t>(row) * rowPitch,
                    data + static_cast<size_t>(row) * srcPitch,
                    srcPitch);
        if (rowPitch > srcPitch) {
            std::memset(mapped + static_cast<size_t>(row) * rowPitch + srcPitch,
                        0,
                        rowPitch - srcPitch);
        }
    }
}

bool D3D12Presenter::MarkFenceFault(const char* reason) noexcept
{
    if (!faulted_) qWarning() << "D3D12 presenter stopped:" << reason;
    faulted_ = true;
    initialized_ = false;
    scenePresentNeeded_ = false;
    return false;
}

bool D3D12Presenter::WaitForGpu() noexcept
{
    if (faulted_) return false;
    if (!commandQueue_ || !fence_ || !fenceEvent_)
        return MarkFenceFault("Fence objects unavailable");
    if (fenceValue_ >= UINT64_MAX - 1)
        return MarkFenceFault("Fence timeline exhausted");
    const UINT64 value = fenceValue_ + 1;
    if (FAILED(commandQueue_->Signal(fence_.Get(), value)))
        return MarkFenceFault("Failed to signal drain fence");
    fenceValue_ = value;
    if (!WaitForFenceValue(value)) return false;
    submittedSourceTextures_.clear();
    return true;
}

bool D3D12Presenter::WaitForFenceValue(UINT64 value) noexcept
{
    if (faulted_) return false;
    if (!fence_ || !device_ || !fenceEvent_)
        return MarkFenceFault("Fence objects unavailable");
    const auto completed = fence_->GetCompletedValue();
    if (completed == UINT64_MAX || FAILED(device_->GetDeviceRemovedReason()))
        return MarkFenceFault("Device removed before fence completion");
    if (completed >= value) return true;
    if (FAILED(fence_->SetEventOnCompletion(value, fenceEvent_)))
        return MarkFenceFault("Failed to set fence completion event");
    const auto result = WaitForFenceDeadline(value, 1000,
        [this] { return fence_->GetCompletedValue(); },
        [this] { return FAILED(device_->GetDeviceRemovedReason()); },
        [this](std::uint64_t milliseconds) {
            const DWORD status = WaitForSingleObject(fenceEvent_, static_cast<DWORD>(milliseconds));
            return status == WAIT_OBJECT_0 ? FenceEventResult::Signaled :
                   status == WAIT_TIMEOUT ? FenceEventResult::TimedOut : FenceEventResult::Failed;
        }, [] { return GetTickCount64(); });
    if (result != FenceWaitResult::Completed)
        return MarkFenceFault(result == FenceWaitResult::TimedOut ?
            "Fence deadline expired; resources retained until restart" :
            "Fence wait failed or device was removed; resources retained until restart");
    return true;
}

bool D3D12Presenter::TryAcquireFrameSlot(UINT slot)
{
    if (faulted_) return false;
    if (!fence_ || !device_ || slot >= kFrameCount)
        return MarkFenceFault("Frame-slot objects unavailable");
    const auto readiness = PollFrameReadiness(frameFenceValues_[slot],
        [this] { return fence_->GetCompletedValue(); },
        [this] { return FAILED(device_->GetDeviceRemovedReason()); },
        [this] {
            if (!frameLatencyWaitableObject_) return FenceEventResult::Signaled;
            const DWORD result = WaitForSingleObject(frameLatencyWaitableObject_, 0);
            return result == WAIT_OBJECT_0 ? FenceEventResult::Signaled :
                   result == WAIT_TIMEOUT ? FenceEventResult::TimedOut : FenceEventResult::Failed;
        });
    switch (readiness) {
    case FrameReadiness::Ready:
        return true;
    case FrameReadiness::Busy:
        ++missedPresentCount_;
        return false;
    case FrameReadiness::DeviceLost:
        return MarkFenceFault("Device removed before frame-slot reuse");
    case FrameReadiness::WaitFailed:
        return MarkFenceFault("Swap-chain latency poll failed");
    }
    return false;
}

bool D3D12Presenter::WaitForIdle() noexcept
{
    if (faulted_) return false;
    if (!initialized_) return true;
    return WaitForGpu();
}

bool D3D12Presenter::KeepSubmittedResource(ID3D12Resource* texture)
{
    for (const auto& resource : submittedSourceTextures_)
        if (resource.Get() == texture) return true;
    // Scene resources normally remain stable until a full drain. Bound even
    // unexpected source churn, and retain every resource already submitted.
    if (submittedSourceTextures_.size() >= 128)
        return MarkFenceFault("Too many unretired source resources");
    submittedSourceTextures_.emplace_back(texture);
    return true;
}

void D3D12Presenter::QuarantineResources() noexcept
{
    // No Unmap, CloseHandle, Release, or synthetic fence signal on an unknown
    // GPU completion state. Detach reference ownership allocation-free; a
    // terminal fault requires process restart, which reclaims this graph.
    factory_.Detach();
    device_.Detach();
    commandQueue_.Detach();
    for (auto& allocator : frameCommandAllocators_) allocator.Detach();
    readbackCommandAllocator_.Detach();
    commandList_.Detach();
    swapChain_.Detach();
    for (auto& buffer : backBuffers_) buffer.Detach();
    renderTargetHeap_.Detach();
    sceneSrvHeap_.Detach();
    sceneRootSignature_.Detach();
    scenePipelineState_.Detach();
    annotationPipelineState_.Detach();
    fence_.Detach();
    for (auto& buffer : uploadBuffers_) buffer.Detach();
    readbackBuffer_.Detach();
    for (auto& slot : asyncReadbackSlots_) {
        slot.buffer.Detach();
        slot.allocator.Detach();
    }
    for (auto& texture : submittedSourceTextures_) texture.Detach();
    if (recordingFramePool_) recordingFramePool_->RetainUntilProcessExit(recordingFramePool_);
    fenceEvent_ = nullptr;
    frameLatencyWaitableObject_ = nullptr;
}

} // namespace openzoom

#endif // _WIN32

