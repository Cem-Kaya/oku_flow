#pragma once
#include "okuflow/d3d12/shared_timeline.hpp"

#ifdef _WIN32

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "okuflow/common/media_writer.hpp"
#include "okuflow/common/recording_contract.hpp"
#include "okuflow/common/view_transform.hpp"
#include "okuflow/cuda/cuda_interop.hpp"

struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12Resource;
struct IDXGIFactory6;
struct IDXGISwapChain3;

namespace okuflow {

struct RecordingFramePoolState;

struct ViewportPresentationOptions {
    bool drawFocusMarker{false};
    float focusX{0.5f};
    float focusY{0.5f};
    bool requestReadback{false};
};

class D3D12Presenter {
public:
    D3D12Presenter();
    ~D3D12Presenter();

    void Initialize(HWND hwnd, UINT width, UINT height);
    bool IsInitialized() const;
    bool IsFaulted() const;
    bool NeedsScenePresent() const;
    UINT ViewportWidth() const;
    UINT ViewportHeight() const;
    std::uint64_t MissedPresentCount() const;
    void Resize(UINT width, UINT height);
    void Present(const uint8_t* data, UINT width, UINT height);
    void PresentFromTexture(ID3D12Resource* texture,
                            UINT width,
                            UINT height,
                            const FenceSyncParams* fenceSync = nullptr);
    bool PresentSceneTexture(ID3D12Resource* texture,
                             UINT sourceWidth,
                             UINT sourceHeight,
                             const ViewTransform& transform,
                             const FenceSyncParams* fenceSync = nullptr,
                             const ViewportPresentationOptions* options = nullptr,
                             UINT64* outReadbackRequestId = nullptr);

    // Copy a GPU texture into a CPU buffer (BGRA8). Blocking; first waits on
    // waitFenceValue on the GPU queue, then waits for the copy to complete.
    bool ReadbackTexture(ID3D12Resource* texture,
                         UINT width,
                         UINT height,
                         std::vector<uint8_t>& outBgra,
                         UINT64 waitFenceValue = 0);

    // Enqueue an async copy of texture into a ring slot; returns false if no
    // slot is free (both in flight) — caller just skips this frame's readback.
    // Never blocks the CPU; the result surfaces one or more frames later via
    // TryGetCompletedReadback.
    bool RequestReadback(ID3D12Resource* texture,
                         UINT width,
                         UINT height,
                         UINT64* outRequestId = nullptr);
    // Renders the canonical recording transform into a shareable, fixed-size
    // BGRA texture. The returned frame is immediately queueable: D3D11 waits
    // on its shared fence on-GPU, so neither the UI nor recording worker has
    // to block for completion.
    // waitFenceValue is the source CUDA producer signal; it is honored even
    // when viewport admission skipped its draw and therefore queued no wait.
    GpuVideoFrame RequestRecordingFrame(
        ID3D12Resource* texture,
        UINT sourceWidth,
        UINT sourceHeight,
        const RecordingViewTransform& transform,
        UINT targetWidth,
        UINT targetHeight,
        const uint8_t* annotationBgra = nullptr,
        std::size_t annotationStrideBytes = 0,
        bool* outPoolExhausted = nullptr,
        UINT64 waitFenceValue = 0);

    // If a previously requested readback has completed, move its pixels into
    // outBgra (BGRA8 tightly packed) and return true. Returns the OLDEST
    // completed request; at most one result per call, so poll every tick to
    // drain. Pending requests are silently dropped by Resize (dimensions are
    // changing anyway) — callers get no result for those frames.
    bool TryGetCompletedReadback(std::vector<uint8_t>& outBgra,
                                 UINT& outWidth,
                                 UINT& outHeight,
                                 UINT64* outRequestId = nullptr);

    ID3D12Device* GetDevice() const;
    ID3D12Fence* GetFence() const;
    UINT64 GetLastSignaledFenceValue() const;
    UINT64 ReserveExternalSignal();
    bool CommitExternalSignal(UINT64 value);
    bool CancelExternalSignal(UINT64 value);
    UINT64 LastGraphicsSignal() const;
    UINT64 LastExternalSignal() const;

    // Wait at most 1000 ms for submitted work. False latches a terminal fault:
    // callers must retain CUDA/shared resources, stop submitting, and offer an
    // application restart. False never grants permission to release resources.
    bool WaitForIdle() noexcept;

private:
    // Frames in flight; matches the swap-chain buffer count so the current
    // back-buffer index doubles as the per-frame resource slot.
    static constexpr UINT kFrameCount = 2;

    void CreateDevice();
    void CreateCommandObjects();
    void CreateFenceObjects();
    void CreateSwapChain(UINT width, UINT height);
    void CreateViewportPipeline();
    void AcquireBackBuffers();
    bool EnsureUploadBuffer(UINT width, UINT height);
    void CopyToUpload(const uint8_t* data, UINT width, UINT height, UINT slot);
    bool WaitForGpu() noexcept;
    bool WaitForFenceValue(UINT64 value) noexcept;
    bool MarkFenceFault(const char* reason) noexcept;
    void QuarantineResources() noexcept;
    bool KeepSubmittedResource(ID3D12Resource* texture);
    bool TryAcquireFrameSlot(UINT slot);

    HWND hwnd_{};
    UINT width_{};
    UINT height_{};
    bool initialized_{};
    bool faulted_{};
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> submittedSourceTextures_;
    bool scenePresentNeeded_{true};
    std::uint64_t missedPresentCount_{};

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> frameCommandAllocators_[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> readbackCommandAllocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> backBuffers_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> renderTargetHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sceneSrvHeap_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> sceneRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> scenePipelineState_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>
        annotationPipelineState_;
    UINT renderTargetDescriptorSize_{};
    UINT sceneSrvDescriptorSize_{};

    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    UINT64 fenceValue_{}; // Last successfully submitted graphics signal.
    SharedFenceTimeline sharedTimeline_;
    bool QueueExternalDependency() noexcept;
    UINT64 ReserveGraphicsSignal();
    void GraphicsSignalSubmitted(UINT64 value) noexcept;
    UINT64 frameFenceValues_[kFrameCount]{};
    HANDLE fenceEvent_{nullptr};
    HANDLE frameLatencyWaitableObject_{nullptr};

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffers_[kFrameCount];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT uploadFootprint_{};
    UINT uploadNumRows_{};
    UINT64 uploadRowSizeInBytes_{};
    UINT64 uploadTotalBytes_{};
    uint8_t* uploadMappedPtrs_[kFrameCount]{};
    UINT uploadWidth_{};
    UINT uploadHeight_{};

    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint_{};
    UINT readbackNumRows_{};
    UINT64 readbackRowSizeInBytes_{};
    UINT64 readbackTotalBytes_{};
    UINT readbackWidth_{};
    UINT readbackHeight_{};

    // Async readback ring. Each slot owns its buffer and command allocator so
    // an in-flight copy never blocks presentation or the synchronous readback
    // path. A slot's allocator is only Reset while the slot is free, i.e.
    // after its previous fence value passed (or after a full queue drain).
    struct AsyncReadbackSlot {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 totalBytes{};
        UINT width{};
        UINT height{};
        UINT64 fenceValue{};
        bool inFlight{};
    };
    // Recording reads the processed scene independently from viewport/photo
    // readbacks. Four slots allow both copies to overlap two presented frames
    // without forcing the preview thread to wait.
    static constexpr UINT kAsyncReadbackSlotCount = 4;
    AsyncReadbackSlot* PrepareAsyncReadbackSlot(UINT width, UINT height);
    AsyncReadbackSlot asyncReadbackSlots_[kAsyncReadbackSlotCount];

    // Shareable recording textures are leased until Media Foundation releases
    // its sample. The pool is bounded so an encoder backlog produces an
    // accounted frame drop rather than an allocation spike or preview stall.
    std::shared_ptr<RecordingFramePoolState> recordingFramePool_;
};

} // namespace okuflow

#endif // _WIN32

