#ifdef _WIN32

#include "okuflow/cuda/cuda_interop.hpp"
#include "okuflow/common/gpu_copy_lease.hpp"

#include <d3d12.h>
#include <dxgi1_2.h>

#if OKUFLOW_HAS_CUDA_EXT_MEMORY

#include <d3d11.h>
#include <cuda_d3d11_interop.h>

#include "okuflow/cuda/cuda_kernels.hpp"
#include "okuflow/common/maxine_superres.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <mutex>
#include <vector>

#include <QDebug>
#include <QString>

namespace okuflow {

struct CudaInteropSurface::D3D11InteropState {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12Resource;
    cudaExternalMemory_t externalMemory{};
    cudaMipmappedArray_t mipArray{};
    cudaArray_t level0Array{};
    // Retained solely for the isolated legacy-driver diagnostic path. The
    // production camera path never creates a legacy registration.
    cudaGraphicsResource_t resource{};
    ID3D11Texture2D* identity{};
    unsigned int subresource{};
    cudaEvent_t copyComplete{};
    bool copyEventRecorded{};
    GpuCopyLease copyLease;
};

namespace {

bool gWarnedFp16Unsupported = false;
constexpr unsigned int kSuperResWarmupFrames = 10u;
constexpr unsigned int kSuperResTimingFrames = 60u;
constexpr float kSuperResLatencyTargetMs = 24.0f;
constexpr unsigned int kMaximumStabilizationPairs = 4096u;
constexpr unsigned int kTripodFocusSelectionFrames = 5u;
constexpr unsigned int kTripodKeyframeCount = 4u;
constexpr unsigned int kTripodRecoveryKeyframeCount =
    kTripodKeyframeCount - 1u;
constexpr unsigned int kTripodKeyframeAdmissionIntervalFrames = 300u;
constexpr unsigned int kTripodKeyframeMinimumValidFrames = 30u;
// Extra Stable enters its hold state on a larger transform step than the
// original 1.25-pixel gate, reducing premature holds by 20%. Recovery remains
// deliberately tighter so the live view returns only after it settles.
constexpr float kBumpHoldMotionEnterAnalysisPixels = 1.50f;
constexpr float kBumpHoldMotionExitAnalysisPixels = 0.55f;
// Multi-frame reference accumulation remains disabled until alignment quality
// can be rejected reliably. A misregistered accumulated keyframe performs
// worse than the selected sharp frame on real low-texture camera footage.
constexpr unsigned int kTripodReferenceAccumulationFrames = 0u;

std::string SuperResTimingStatus(const char* prefix, float averageMs)
{
    char buffer[160]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s %.1f ms average (%.1f ms target)",
                  prefix,
                  static_cast<double>(averageMs),
                  static_cast<double>(kSuperResLatencyTargetMs));
    return buffer;
}

void ThrowIfFailed(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        qWarning() << message << "hr=0x" << Qt::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(std::string(message) + " (hr=0x" + std::to_string(static_cast<unsigned long>(hr)) + ")");
    }
}

void ThrowIfCudaFailed(cudaError_t status, const char* message) {
    if (status != cudaSuccess) {
        const char* err = cudaGetErrorString(status);
        qWarning() << message << "cudaError=" << status << err;
        throw std::runtime_error(std::string(message) + ": " + err);
    }
}

class WindowsSecurityAttributes {
public:
    WindowsSecurityAttributes() {
        InitializeSecurityDescriptor(&securityDescriptor_, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&securityDescriptor_, TRUE, nullptr, FALSE);
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &securityDescriptor_;
        attributes_.bInheritHandle = FALSE;
    }

    SECURITY_ATTRIBUTES* get() { return &attributes_; }

private:
    SECURITY_ATTRIBUTES attributes_{};
    SECURITY_DESCRIPTOR securityDescriptor_{};
};

cudaChannelFormatDesc MakeChannelDescForFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return cudaCreateChannelDesc<uchar4>();
    default:
        throw std::runtime_error("Unsupported DXGI format for CUDA interop");
    }
}

constexpr int kMaxCudaBlurRadius = 50;

// Defensive ceiling for driver-supplied frame dimensions; anything above this
// is treated as a corrupt input descriptor (also keeps width*height*4 far away
// from 32-bit overflow).
constexpr unsigned int kMaxInputExtent = 16384;

// Keystone tuning. A snapshot of the small luma image is exported roughly once
// per second (at 30 fps); after kKeystoneStaleFrames without an accepted
// detection the warp eases back to identity.
constexpr unsigned int kKeystoneSnapshotPeriod = 30;
constexpr unsigned int kKeystoneStaleFrames = 150;   // ~5 s at 30 fps
constexpr float kKeystoneCornerLerp = 0.15f;         // per accepted detection
constexpr float kKeystoneIdentityReturnLerp = 0.03f; // per frame once stale
constexpr float kKeystoneThresholdSigmaK = 0.5f;
constexpr float kKeystoneMinAreaFraction = 0.15f;
constexpr float kKeystoneIdentityEpsilonPx = 0.75f;
constexpr size_t kKeystoneHistoryLimit = 32;

bool gWarnedBgraRotationIgnored = false;
bool gWarnedInvalidInput = false;

// Pick integer downsample factors (ceil division for the factor, then again
// for the resulting extent). Keystone stays inexpensive at 320x180, while
// stabilization retains twice the linear detail so small pre-zoom tremor is
// not averaged away before feature tracking.
void ComputeAnalysisDims(unsigned int width, unsigned int height,
                         unsigned int targetWidth, unsigned int targetHeight,
                         unsigned int& factorX, unsigned int& factorY,
                         unsigned int& smallWidth, unsigned int& smallHeight) {
    factorX = (width + targetWidth - 1) / targetWidth;
    factorY = (height + targetHeight - 1) / targetHeight;
    smallWidth = (width + factorX - 1) / factorX;
    smallHeight = (height + factorY - 1) / factorY;
}

void ComputeSmallLumaDims(unsigned int width, unsigned int height,
                          unsigned int& factorX, unsigned int& factorY,
                          unsigned int& smallWidth, unsigned int& smallHeight) {
    ComputeAnalysisDims(width, height, 320, 180, factorX, factorY,
                        smallWidth, smallHeight);
}

void ComputeStabilizationLumaDims(
    unsigned int width, unsigned int height,
    unsigned int& factorX, unsigned int& factorY,
    unsigned int& smallWidth, unsigned int& smallHeight) {
    ComputeAnalysisDims(width, height, 640, 360, factorX, factorY,
                        smallWidth, smallHeight);
}

struct KeystoneQuadDetection {
    float2 corners[4]{};  // TL, TR, BR, BL in small-image pixel coordinates
    bool valid{false};
};

float2 LerpPoint(const float2& a, const float2& b, float t) {
    return make_float2(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y));
}

float PointDistance(const float2& a, const float2& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// TL->TR->BR->BL must turn consistently clockwise (positive cross products in
// y-down image coordinates); near-degenerate quads are rejected too.
bool QuadIsConvexClockwise(const float2 quad[4]) {
    for (int i = 0; i < 4; ++i) {
        const float2& p0 = quad[i];
        const float2& p1 = quad[(i + 1) & 3];
        const float2& p2 = quad[(i + 2) & 3];
        const float cross = (p1.x - p0.x) * (p2.y - p1.y) - (p1.y - p0.y) * (p2.x - p1.x);
        if (cross <= 1.0f) {
            return false;
        }
    }
    return true;
}

// CPU detection of the bright projected-slide quadrilateral on the tiny
// (<=320x180) luma snapshot. Runs a few times per second on a frame that was
// copied out asynchronously, so it never touches the GPU timeline.
// Steps: threshold at mean + k*sigma, largest 4-connected bright component via
// flood fill, corners from the component's x+y / x-y extremes, then area,
// convexity and aspect validation.
KeystoneQuadDetection DetectProjectedQuad(const float* luma, int width, int height) {
    KeystoneQuadDetection detection{};
    if (luma == nullptr || width <= 0 || height <= 0) {
        return detection;
    }
    const int count = width * height;

    double sum = 0.0;
    double sumSq = 0.0;
    for (int i = 0; i < count; ++i) {
        sum += luma[i];
        sumSq += static_cast<double>(luma[i]) * luma[i];
    }
    const double mean = sum / count;
    const double variance = std::max(sumSq / count - mean * mean, 0.0);
    const double sigma = std::sqrt(variance);
    // k is deliberately small: when the slide dominates the frame the mean sits
    // close to the slide brightness and a large k would push the threshold
    // above it. The area/shape validation below rejects bad segmentations.
    const float threshold =
        static_cast<float>(std::min(mean + kKeystoneThresholdSigmaK * sigma, 250.0));

    // 0 = below threshold, 1 = bright & unvisited, 2 = visited.
    std::vector<uint8_t> state(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        state[static_cast<size_t>(i)] = (luma[i] >= threshold) ? 1u : 0u;
    }

    std::vector<int> stack;
    stack.reserve(1024);

    int bestArea = 0;
    float2 bestCorners[4]{};

    for (int seed = 0; seed < count; ++seed) {
        if (state[static_cast<size_t>(seed)] != 1u) {
            continue;
        }

        int area = 0;
        int minSum = std::numeric_limits<int>::max();
        int maxSum = std::numeric_limits<int>::min();
        int minDiff = std::numeric_limits<int>::max();
        int maxDiff = std::numeric_limits<int>::min();
        float2 tl{}, tr{}, br{}, bl{};

        state[static_cast<size_t>(seed)] = 2u;
        stack.clear();
        stack.push_back(seed);
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            const int x = idx % width;
            const int y = idx / width;
            ++area;

            // Cheap robust corner picker: extremes of x+y and x-y.
            const int s = x + y;
            const int d = x - y;
            const float2 p = make_float2(static_cast<float>(x), static_cast<float>(y));
            if (s < minSum) { minSum = s; tl = p; }
            if (s > maxSum) { maxSum = s; br = p; }
            if (d > maxDiff) { maxDiff = d; tr = p; }
            if (d < minDiff) { minDiff = d; bl = p; }

            if (x > 0 && state[static_cast<size_t>(idx - 1)] == 1u) {
                state[static_cast<size_t>(idx - 1)] = 2u;
                stack.push_back(idx - 1);
            }
            if (x + 1 < width && state[static_cast<size_t>(idx + 1)] == 1u) {
                state[static_cast<size_t>(idx + 1)] = 2u;
                stack.push_back(idx + 1);
            }
            if (y > 0 && state[static_cast<size_t>(idx - width)] == 1u) {
                state[static_cast<size_t>(idx - width)] = 2u;
                stack.push_back(idx - width);
            }
            if (y + 1 < height && state[static_cast<size_t>(idx + width)] == 1u) {
                state[static_cast<size_t>(idx + width)] = 2u;
                stack.push_back(idx + width);
            }
        }

        if (area > bestArea) {
            bestArea = area;
            bestCorners[0] = tl;
            bestCorners[1] = tr;
            bestCorners[2] = br;
            bestCorners[3] = bl;
        }
    }

    if (bestArea < static_cast<int>(kKeystoneMinAreaFraction * static_cast<float>(count))) {
        return detection;
    }
    if (!QuadIsConvexClockwise(bestCorners)) {
        return detection;
    }

    const float avgWidth = 0.5f * (PointDistance(bestCorners[0], bestCorners[1]) +
                                   PointDistance(bestCorners[3], bestCorners[2]));
    const float avgHeight = 0.5f * (PointDistance(bestCorners[0], bestCorners[3]) +
                                    PointDistance(bestCorners[1], bestCorners[2]));
    if (avgWidth < 0.25f * static_cast<float>(width) ||
        avgHeight < 0.25f * static_cast<float>(height)) {
        return detection;
    }
    const float aspect = avgWidth / std::max(avgHeight, 1.0f);
    if (aspect < 0.3f || aspect > 4.0f) {
        return detection;
    }

    for (int i = 0; i < 4; ++i) {
        detection.corners[i] = bestCorners[i];
    }
    detection.valid = true;
    return detection;
}

// 4-point DLT: homography H with H * (rect corner) ~ quad corner, where the
// rect corners are (0,0), (w-1,0), (w-1,h-1), (0,h-1) and h22 is fixed to 1.
// The 8x8 system is solved with Gauss-Jordan elimination and partial pivoting
// in double precision — 4 correspondences, so this costs microseconds on the
// host and needs no external dependency.
bool SolveRectToQuadHomography(float width, float height, const float2 quad[4], float out[9]) {
    const double rectX[4] = {0.0, static_cast<double>(width) - 1.0,
                             static_cast<double>(width) - 1.0, 0.0};
    const double rectY[4] = {0.0, 0.0,
                             static_cast<double>(height) - 1.0, static_cast<double>(height) - 1.0};

    double a[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        const double x = rectX[i];
        const double y = rectY[i];
        const double u = static_cast<double>(quad[i].x);
        const double v = static_cast<double>(quad[i].y);
        double* row0 = a[2 * i];
        double* row1 = a[2 * i + 1];
        row0[0] = x;  row0[1] = y;  row0[2] = 1.0;
        row0[6] = -u * x;  row0[7] = -u * y;  row0[8] = u;
        row1[3] = x;  row1[4] = y;  row1[5] = 1.0;
        row1[6] = -v * x;  row1[7] = -v * y;  row1[8] = v;
    }

    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 8; ++row) {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) {
                pivot = row;
            }
        }
        if (std::abs(a[pivot][col]) < 1e-9) {
            return false;
        }
        if (pivot != col) {
            for (int c = 0; c < 9; ++c) {
                std::swap(a[pivot][c], a[col][c]);
            }
        }
        const double inv = 1.0 / a[col][col];
        for (int c = col; c < 9; ++c) {
            a[col][c] *= inv;
        }
        for (int row = 0; row < 8; ++row) {
            if (row == col) {
                continue;
            }
            const double factor = a[row][col];
            if (factor == 0.0) {
                continue;
            }
            for (int c = col; c < 9; ++c) {
                a[row][c] -= factor * a[col][c];
            }
        }
    }

    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<float>(a[i][8]);
    }
    out[8] = 1.0f;
    return true;
}

bool EnsureCudaDriverInitialized()
{
    static std::once_flag initFlag;
    static CUresult initResult = CUDA_SUCCESS;
    std::call_once(initFlag, []() {
        initResult = cuInit(0);
    });
    if (initResult != CUDA_SUCCESS) {
        qWarning() << "cuInit failed" << static_cast<int>(initResult);
        return false;
    }
    return true;
}

bool QueryDeviceLuid(int deviceId, LUID& luidOut)
{
#if defined(CUDA_VERSION) && CUDA_VERSION >= 11030
    if (!EnsureCudaDriverInitialized()) {
        return false;
    }

    CUdevice cuDevice{};
    if (cuDeviceGet(&cuDevice, deviceId) != CUDA_SUCCESS) {
        return false;
    }

    char luidBuffer[sizeof(LUID)] = {};
    unsigned int nodeMask = 0;
    if (cuDeviceGetLuid(luidBuffer, &nodeMask, cuDevice) == CUDA_SUCCESS) {
        std::memcpy(&luidOut, luidBuffer, sizeof(LUID));
        return true;
    }
#endif
    return false;
}

// Callers must have proven the stream drained. A mapped mipmapped array is a
// separate allocation that cudaDestroyExternalMemory does not release, so it
// is freed after its surface object and before its external-memory import.
// The level-zero array is a non-owning view into the mipmapped array. The
// original recording image has no surface object, so it passes nullptr.
void ReleaseMappedExternalImage(cudaSurfaceObject_t* surfaceObject,
                                cudaMipmappedArray_t& mipArray,
                                cudaArray_t& level0Array,
                                cudaExternalMemory_t& externalMemory,
                                const char* label) noexcept
{
    if (surfaceObject != nullptr && *surfaceObject != 0) {
        cudaDestroySurfaceObject(*surfaceObject);
        *surfaceObject = 0;
    }
    level0Array = nullptr;
    if (mipArray != nullptr) {
        const cudaError_t result = cudaFreeMipmappedArray(mipArray);
        if (result != cudaSuccess) {
            qWarning() << "CUDA" << label << "external-memory array release failed:"
                       << cudaGetErrorString(result);
        }
        mipArray = nullptr;
    }
    if (externalMemory != nullptr) {
        cudaDestroyExternalMemory(externalMemory);
        externalMemory = nullptr;
    }
}

} // namespace

CudaInteropSurface::CudaInteropSurface(ID3D12Resource* texture,
                                       ID3D12Resource* superResTexture,
                                       ID3D12Fence* sharedFence,
                                       ID3D12Resource* originalTexture)
    : retainedMainTexture_(texture), retainedSuperResTexture_(superResTexture),
      retainedOriginalTexture_(originalTexture), retainedSharedFence_(sharedFence) {
    try {
        Initialize(texture, superResTexture, sharedFence, originalTexture);
        valid_ = true;
        lastError_.clear();
    } catch (const std::exception& e) {
        qWarning() << "CudaInteropSurface init failed:" << e.what();
        valid_ = false;
        lastError_ = e.what();
        if (!SynchronizeStream()) { QuarantineGpuResources(); return; }
        ReleaseMappedExternalImage(&surfaceObject_, mipArray_, level0Array_,
                                   externalMemory_, "scene");
        ReleaseMappedExternalImage(&superResSurfaceObject_, superResMipArray_,
                                   superResLevel0Array_, superResExternalMemory_,
                                   "SuperRes cache");
        ReleaseMappedExternalImage(nullptr, originalMipArray_,
                                   originalLevel0Array_, originalExternalMemory_,
                                   "original recording");
        if (externalSemaphore_ != nullptr) {
            cudaDestroyExternalSemaphore(externalSemaphore_);
            externalSemaphore_ = nullptr;
        }
        if (stream_ != nullptr) {
            cudaStreamDestroy(stream_);
            stream_ = nullptr;
        }
    }
}

// ProcessFrame() returns with kernels still queued on stream_ when fence sync is
// active, so every teardown path must drain the stream before releasing resources
// the queued work references (surface object, external memory, device buffers).
bool CudaInteropSurface::WaitForIdle() noexcept {
    return SynchronizeStream();
}

bool CudaInteropSurface::SynchronizeStream() noexcept {
    if (streamFaulted_) return false;
    if (stream_ == nullptr) return true;
    if (cudaDeviceId_ >= 0 && cudaSetDevice(cudaDeviceId_) != cudaSuccess) {
        streamFaulted_ = true;
        valid_ = false;
        return false;
    }
    const auto started = GetTickCount64();
    for (;;) {
        const cudaError_t status = cudaStreamQuery(stream_);
        if (status == cudaSuccess) return true;
        if (status != cudaErrorNotReady || GetTickCount64() - started >= 1000) {
            qWarning() << "CUDA stream drain failed or exceeded its deadline; retaining GPU resources";
            streamFaulted_ = true;
            valid_ = false;
            return false;
        }
        Sleep(1);
    }
}

void CudaInteropSurface::QuarantineGpuResources() noexcept {
    // Raw CUDA allocations/handles deliberately remain allocated. Preserve
    // owning C++ states and D3D storage too: releasing either is unsafe while
    // queued work may still use the destination or imported camera source.
    (void)d3d11Interop_.release();
    (void)maxineSuperRes_.release();
    d3d12Device_.Detach();
    retainedMainTexture_.Detach();
    retainedSuperResTexture_.Detach();
    retainedOriginalTexture_.Detach();
    retainedSharedFence_.Detach();
}

CudaInteropSurface::~CudaInteropSurface() {
    if (!SynchronizeStream()) { QuarantineGpuResources(); return; }
    ResetCaptureInterop();

    ReleaseSuperRes();
    ReleasePinnedUploadRing();
    ReleaseDeviceBuffers();

    if (processTimingStartEvent_) {
        cudaEventDestroy(processTimingStartEvent_);
        processTimingStartEvent_ = nullptr;
    }
    if (processTimingInputEvent_) {
        cudaEventDestroy(processTimingInputEvent_);
        processTimingInputEvent_ = nullptr;
    }
    if (processTimingGeometryEvent_) {
        cudaEventDestroy(processTimingGeometryEvent_);
        processTimingGeometryEvent_ = nullptr;
    }
    if (processTimingEffectsEvent_) {
        cudaEventDestroy(processTimingEffectsEvent_);
        processTimingEffectsEvent_ = nullptr;
    }
    if (processTimingStopEvent_) {
        cudaEventDestroy(processTimingStopEvent_);
        processTimingStopEvent_ = nullptr;
    }
    processTimingPending_ = false;

    ReleaseMappedExternalImage(&surfaceObject_, mipArray_, level0Array_,
                               externalMemory_, "scene");
    ReleaseMappedExternalImage(&superResSurfaceObject_, superResMipArray_,
                               superResLevel0Array_, superResExternalMemory_,
                               "SuperRes cache");
    ReleaseMappedExternalImage(nullptr, originalMipArray_,
                               originalLevel0Array_, originalExternalMemory_,
                               "original recording");

    if (externalSemaphore_ != nullptr) {
        cudaDestroyExternalSemaphore(externalSemaphore_);
        externalSemaphore_ = nullptr;
    }

    if (stream_ != nullptr) {
        cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }

}

void CudaInteropSurface::AbandonCaptureInterop() noexcept {
    // Losing producer quiescence must never enter CUDA/D3D release routines.
    // Preserve mappings, source storage, completion event, and session lease.
    (void)d3d11Interop_.release();
}

void CudaInteropSurface::ResetCaptureInterop(bool atProcessExit) {
    if (!d3d11Interop_) {
        return;
    }
    if (cudaDeviceId_ >= 0) {
        const cudaError_t deviceStatus = cudaSetDevice(cudaDeviceId_);
        if (deviceStatus != cudaSuccess) {
            qWarning() << "cudaSetDevice while releasing camera interop failed:"
                       << cudaGetErrorString(deviceStatus);
        }
    }
    if (!PollCaptureCopy()) return;
    if (atProcessExit && d3d11Interop_->resource) {
        // The installed NVIDIA driver faults inside the otherwise balanced
        // legacy unregister call after a VideoProcessor-written texture has
        // been used for capture. Keep both the registration and its texture
        // alive until Windows reclaims the process. Releasing only the ComPtr
        // here would violate CUDA's registered-resource lifetime contract.
        qWarning() << "Retaining the legacy CUDA/D3D11 camera registration "
                      "until process exit due to an NVIDIA driver workaround";
        (void)d3d11Interop_.release();
        return;
    }
    if (d3d11Interop_->resource) {
        qInfo() << "Releasing CUDA/D3D11 camera interop registration";
        const cudaError_t result =
            cudaGraphicsUnregisterResource(d3d11Interop_->resource);
        if (result != cudaSuccess) {
            qWarning() << "CUDA camera texture unregister failed:"
                       << cudaGetErrorString(result);
        } else {
            qInfo() << "CUDA/D3D11 camera interop registration released";
        }
        d3d11Interop_->resource = nullptr;
    }
    if (d3d11Interop_->mipArray) {
        const cudaError_t result =
            cudaFreeMipmappedArray(d3d11Interop_->mipArray);
        if (result != cudaSuccess) {
            qWarning() << "CUDA camera external-memory array release failed:"
                       << cudaGetErrorString(result);
        }
        d3d11Interop_->mipArray = nullptr;
        d3d11Interop_->level0Array = nullptr;
    }
    if (d3d11Interop_->externalMemory) {
        const cudaError_t result =
            cudaDestroyExternalMemory(d3d11Interop_->externalMemory);
        if (result != cudaSuccess) {
            qWarning() << "CUDA camera external-memory release failed:"
                       << cudaGetErrorString(result);
        }
        d3d11Interop_->externalMemory = nullptr;
    }
    if (d3d11Interop_->copyComplete) {
        cudaEventDestroy(d3d11Interop_->copyComplete);
        d3d11Interop_->copyComplete = nullptr;
    }
    d3d11Interop_.reset();
}

bool CudaInteropSurface::SelectCudaDeviceMatching(LUID adapterLuid) {
    int deviceCount = 0;
    ThrowIfCudaFailed(cudaGetDeviceCount(&deviceCount), "cudaGetDeviceCount failed");

    bool matched = false;
    cudaDeviceProp matchedProps{};

    for (int deviceId = 0; deviceId < deviceCount; ++deviceId) {
        cudaDeviceProp properties{};
        ThrowIfCudaFailed(cudaGetDeviceProperties(&properties, deviceId), "cudaGetDeviceProperties failed");

        LUID deviceLuid{};
        if (QueryDeviceLuid(deviceId, deviceLuid) &&
            std::memcmp(&adapterLuid, &deviceLuid, sizeof(LUID)) == 0) {
            matched = true;
            matchedProps = properties;
            ThrowIfCudaFailed(cudaSetDevice(deviceId), "cudaSetDevice failed");
            cudaDeviceId_ = deviceId;
            qInfo() << "CUDA device" << deviceId << properties.name << "matches DXGI adapter";
            break;
        }
    }

    if (!matched) {
        lastError_ =
            deviceCount > 0
                ? "No CUDA device matches the D3D12 adapter. GPU processing "
                  "was disabled to prevent cross-adapter interop corruption."
                : "No CUDA devices are available.";
        qWarning() << QString::fromStdString(lastError_);
        return false;
    }

    qInfo() << "Using CUDA device" << cudaDeviceId_ << matchedProps.name;
    return true;
}

bool CudaInteropSurface::CreateSurfaceFromResource(ID3D12Device* device, ID3D12Resource* texture) {
    WindowsSecurityAttributes securityAttributes;

    HANDLE sharedHandle = nullptr;
    ThrowIfFailed(device->CreateSharedHandle(texture,
                                             securityAttributes.get(),
                                             GENERIC_ALL,
                                             nullptr,
                                             &sharedHandle),
                  "Failed to create shared handle for D3D12 resource");
    qInfo() << "Created shared D3D12 resource handle for CUDA interop";

    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    width_ = static_cast<UINT>(desc.Width);
    height_ = static_cast<UINT>(desc.Height);
    format_ = desc.Format;

    D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = device->GetResourceAllocationInfo(0, 1, &desc);

    cudaExternalMemoryHandleDesc memoryDesc{};
    memoryDesc.type = cudaExternalMemoryHandleTypeD3D12Resource;
    memoryDesc.handle.win32.handle = sharedHandle;
    memoryDesc.size = allocationInfo.SizeInBytes;
    memoryDesc.flags = cudaExternalMemoryDedicated;

    auto cleanupHandle = [&sharedHandle]() {
        if (sharedHandle) {
            CloseHandle(sharedHandle);
            sharedHandle = nullptr;
        }
    };

    try {
        qInfo() << "Importing external memory (size" << static_cast<unsigned long long>(allocationInfo.SizeInBytes)
                << ", flags=cudaExternalMemoryDedicated)";
        ThrowIfCudaFailed(cudaImportExternalMemory(&externalMemory_, &memoryDesc),
                          "cudaImportExternalMemory failed");
        qInfo() << "Imported external memory for CUDA (size" << static_cast<unsigned long long>(allocationInfo.SizeInBytes) << ")";
        cleanupHandle();

        cudaExternalMemoryMipmappedArrayDesc arrayDesc{};
        arrayDesc.offset = 0;
        arrayDesc.numLevels = 1;
        arrayDesc.extent = make_cudaExtent(width_, height_, 1);
        arrayDesc.formatDesc = MakeChannelDescForFormat(format_);
        arrayDesc.flags = cudaArraySurfaceLoadStore | cudaArrayColorAttachment;

        ThrowIfCudaFailed(cudaExternalMemoryGetMappedMipmappedArray(&mipArray_, externalMemory_, &arrayDesc),
                          "cudaExternalMemoryGetMappedMipmappedArray failed");
        ThrowIfCudaFailed(cudaGetMipmappedArrayLevel(&level0Array_, mipArray_, 0),
                          "cudaGetMipmappedArrayLevel failed");

        cudaResourceDesc resourceDesc{};
        resourceDesc.resType = cudaResourceTypeArray;
        resourceDesc.res.array.array = level0Array_;
        ThrowIfCudaFailed(cudaCreateSurfaceObject(&surfaceObject_, &resourceDesc),
                          "cudaCreateSurfaceObject failed");

        ThrowIfCudaFailed(cudaStreamCreate(&stream_), "cudaStreamCreate failed");
    } catch (const std::exception& e) {
        lastError_ = e.what();
        qWarning() << "CreateSurfaceFromResource exception:" << e.what();
        cleanupHandle();
        throw;
    }

    cachedKernelRadius_ = -1;
    cachedKernelSigma_ = 0.0f;
    kernelUploaded_ = false;
    qInfo() << "CUDA external memory imported successfully (" << width_ << "x" << height_ << ")";
    return true;
}

bool CudaInteropSurface::CreateSuperResSurfaceFromResource(
    ID3D12Device* device,
    ID3D12Resource* texture) {
    if (!texture) {
        return true;
    }

    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    if (desc.Format != format_) {
        throw std::invalid_argument(
            "SuperRes cache texture format must match the primary CUDA surface");
    }
    superResWidth_ = static_cast<UINT>(desc.Width);
    superResHeight_ = desc.Height;

    WindowsSecurityAttributes securityAttributes;
    HANDLE sharedHandle = nullptr;
    ThrowIfFailed(device->CreateSharedHandle(texture,
                                             securityAttributes.get(),
                                             GENERIC_ALL,
                                             nullptr,
                                             &sharedHandle),
                  "Failed to create shared handle for SuperRes cache");
    const D3D12_RESOURCE_ALLOCATION_INFO allocationInfo =
        device->GetResourceAllocationInfo(0, 1, &desc);

    cudaExternalMemoryHandleDesc memoryDesc{};
    memoryDesc.type = cudaExternalMemoryHandleTypeD3D12Resource;
    memoryDesc.handle.win32.handle = sharedHandle;
    memoryDesc.size = allocationInfo.SizeInBytes;
    memoryDesc.flags = cudaExternalMemoryDedicated;

    const auto closeHandle = [&sharedHandle]() {
        if (sharedHandle) {
            CloseHandle(sharedHandle);
            sharedHandle = nullptr;
        }
    };

    try {
        ThrowIfCudaFailed(
            cudaImportExternalMemory(&superResExternalMemory_, &memoryDesc),
            "cudaImportExternalMemory for SuperRes cache failed");
        closeHandle();

        cudaExternalMemoryMipmappedArrayDesc arrayDesc{};
        arrayDesc.offset = 0;
        arrayDesc.numLevels = 1;
        arrayDesc.extent = make_cudaExtent(superResWidth_, superResHeight_, 1);
        arrayDesc.formatDesc = MakeChannelDescForFormat(format_);
        arrayDesc.flags =
            cudaArraySurfaceLoadStore | cudaArrayColorAttachment;
        ThrowIfCudaFailed(
            cudaExternalMemoryGetMappedMipmappedArray(
                &superResMipArray_, superResExternalMemory_, &arrayDesc),
            "cudaExternalMemoryGetMappedMipmappedArray for SuperRes cache failed");
        ThrowIfCudaFailed(
            cudaGetMipmappedArrayLevel(
                &superResLevel0Array_, superResMipArray_, 0),
            "cudaGetMipmappedArrayLevel for SuperRes cache failed");

        cudaResourceDesc resourceDesc{};
        resourceDesc.resType = cudaResourceTypeArray;
        resourceDesc.res.array.array = superResLevel0Array_;
        ThrowIfCudaFailed(
            cudaCreateSurfaceObject(&superResSurfaceObject_, &resourceDesc),
            "cudaCreateSurfaceObject for SuperRes cache failed");
    } catch (...) {
        closeHandle();
        throw;
    }

    qInfo() << "CUDA SuperRes cache surface imported successfully ("
            << superResWidth_ << "x" << superResHeight_ << ")";
    return true;
}

bool CudaInteropSurface::CreateOriginalSurfaceFromResource(
    ID3D12Device* device,
    ID3D12Resource* texture) {
    if (!texture) {
        return true;
    }

    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    if (desc.Format != format_ ||
        desc.Width != width_ ||
        desc.Height != height_) {
        throw std::invalid_argument(
            "Original recording texture must match the primary CUDA surface");
    }
    originalWidth_ = static_cast<UINT>(desc.Width);
    originalHeight_ = desc.Height;

    WindowsSecurityAttributes securityAttributes;
    HANDLE sharedHandle = nullptr;
    ThrowIfFailed(device->CreateSharedHandle(texture,
                                             securityAttributes.get(),
                                             GENERIC_ALL,
                                             nullptr,
                                             &sharedHandle),
                  "Failed to create shared handle for original recording surface");
    const D3D12_RESOURCE_ALLOCATION_INFO allocationInfo =
        device->GetResourceAllocationInfo(0, 1, &desc);

    cudaExternalMemoryHandleDesc memoryDesc{};
    memoryDesc.type = cudaExternalMemoryHandleTypeD3D12Resource;
    memoryDesc.handle.win32.handle = sharedHandle;
    memoryDesc.size = allocationInfo.SizeInBytes;
    memoryDesc.flags = cudaExternalMemoryDedicated;

    const auto closeHandle = [&sharedHandle]() {
        if (sharedHandle) {
            CloseHandle(sharedHandle);
            sharedHandle = nullptr;
        }
    };

    try {
        ThrowIfCudaFailed(
            cudaImportExternalMemory(&originalExternalMemory_, &memoryDesc),
            "cudaImportExternalMemory for original recording surface failed");
        closeHandle();

        cudaExternalMemoryMipmappedArrayDesc arrayDesc{};
        arrayDesc.offset = 0;
        arrayDesc.numLevels = 1;
        arrayDesc.extent =
            make_cudaExtent(originalWidth_, originalHeight_, 1);
        arrayDesc.formatDesc = MakeChannelDescForFormat(format_);
        arrayDesc.flags =
            cudaArraySurfaceLoadStore | cudaArrayColorAttachment;
        ThrowIfCudaFailed(
            cudaExternalMemoryGetMappedMipmappedArray(
                &originalMipArray_, originalExternalMemory_, &arrayDesc),
            "cudaExternalMemoryGetMappedMipmappedArray for original recording surface failed");
        ThrowIfCudaFailed(
            cudaGetMipmappedArrayLevel(
                &originalLevel0Array_, originalMipArray_, 0),
            "cudaGetMipmappedArrayLevel for original recording surface failed");
    } catch (...) {
        closeHandle();
        throw;
    }

    qInfo() << "CUDA original recording surface imported successfully ("
            << originalWidth_ << "x" << originalHeight_ << ")";
    return true;
}

void CudaInteropSurface::Initialize(ID3D12Resource* texture,
                                    ID3D12Resource* superResTexture,
                                    ID3D12Fence* sharedFence,
                                    ID3D12Resource* originalTexture) {
    if (!texture) {
        throw std::invalid_argument("Cannot initialize CUDA interop with null resource");
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    ThrowIfFailed(texture->GetDevice(IID_PPV_ARGS(&device)), "Failed to query ID3D12Device from resource");
    d3d12Device_ = device;

    LUID adapterLuid = device->GetAdapterLuid();
    if (!SelectCudaDeviceMatching(adapterLuid)) {
        throw std::runtime_error("No CUDA device matches the D3D12 adapter LUID");
    }

    CreateSurfaceFromResource(device.Get(), texture);
    CreateSuperResSurfaceFromResource(device.Get(), superResTexture);
    CreateOriginalSurfaceFromResource(device.Get(), originalTexture);

    if (sharedFence != nullptr) {
        ImportFenceSemaphore(device.Get(), sharedFence);
    }
}

bool CudaInteropSurface::EnsureDeviceBuffers(unsigned int width, unsigned int height, CudaBufferFormat format) {
    CudaBufferFormat resolvedFormat = format;
    if (format == CudaBufferFormat::kRgba16F) {
        if (!gWarnedFp16Unsupported) {
            qWarning() << "CUDA staging format RGBA16F not yet implemented; falling back to RGBA8";
            gWarnedFp16Unsupported = true;
        }
        resolvedFormat = CudaBufferFormat::kRgba8;
    }

    if (deviceBufferA_ && deviceBufferB_ &&
        deviceWidth_ == width && deviceHeight_ == height &&
        bufferFormat_ == resolvedFormat) {
        return true;
    }

    ReleaseDeviceBuffers();
    if (streamFaulted_) return false;

    size_t pitch = 0;
    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&deviceBufferA_), &devicePitchA_,
                                      static_cast<size_t>(width) * sizeof(uchar4), height),
                      "cudaMallocPitch buffer A failed");
    ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&deviceBufferB_), &devicePitchB_,
                                      static_cast<size_t>(width) * sizeof(uchar4), height),
                      "cudaMallocPitch buffer B failed");
    ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&deviceScratch_), &devicePitchScratch_,
                                      static_cast<size_t>(width) * sizeof(uchar4), height),
                      "cudaMallocPitch scratch buffer failed");

    deviceWidth_ = width;
    deviceHeight_ = height;
    bufferFormat_ = resolvedFormat;
    return true;
}

void CudaInteropSurface::ImportFenceSemaphore(ID3D12Device* device, ID3D12Fence* fence) {
    if (!device || !fence) {
        return;
    }

    WindowsSecurityAttributes securityAttributes;
    HANDLE fenceHandle = nullptr;
    ThrowIfFailed(device->CreateSharedHandle(fence,
                                            securityAttributes.get(),
                                            GENERIC_ALL,
                                            nullptr,
                                            &fenceHandle),
                  "Failed to create shared handle for D3D12 fence");

    auto cleanupHandle = [&]() {
        if (fenceHandle) {
            CloseHandle(fenceHandle);
            fenceHandle = nullptr;
        }
    };

    cudaExternalSemaphoreHandleDesc semaphoreDesc{};
    semaphoreDesc.type = cudaExternalSemaphoreHandleTypeD3D12Fence;
    semaphoreDesc.handle.win32.handle = fenceHandle;
    semaphoreDesc.flags = 0;

    try {
        ThrowIfCudaFailed(cudaImportExternalSemaphore(&externalSemaphore_, &semaphoreDesc),
                          "cudaImportExternalSemaphore failed");
        qInfo() << "Imported shared D3D12 fence into CUDA";
    } catch (...) {
        cleanupHandle();
        throw;
    }

    cleanupHandle();
}

void CudaInteropSurface::ReleaseDeviceBuffers() {
    if (deviceBufferA_ || deviceBufferB_ || deviceScratch_ || deviceTemporalHistory_) {
        if (!SynchronizeStream()) return;
    }
    if (deviceBufferA_) {
        cudaFree(deviceBufferA_);
        deviceBufferA_ = nullptr;
    }
    if (deviceBufferB_) {
        cudaFree(deviceBufferB_);
        deviceBufferB_ = nullptr;
    }
    if (deviceScratch_) {
        cudaFree(deviceScratch_);
        deviceScratch_ = nullptr;
    }
    devicePitchA_ = 0;
    devicePitchB_ = 0;
    devicePitchScratch_ = 0;
    deviceWidth_ = 0;
    deviceHeight_ = 0;
    bufferFormat_ = CudaBufferFormat::kRgba8;
    ReleaseTemporalHistory();
    ReleaseStabilization();
    ReleaseRawInput();
    ReleaseKeystone();
    ReleaseAutoContrast();
    ReleaseTextClarity();
    ReleaseSuperRes();
    kernelUploaded_ = false;
}

bool CudaInteropSurface::EnsureTemporalHistory(unsigned int width, unsigned int height) {
    if (deviceTemporalHistory_ && historyWidth_ == width && historyHeight_ == height) {
        return true;
    }

    ReleaseTemporalHistory();
    if (streamFaulted_) return false;

    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&deviceTemporalHistory_), &devicePitchHistory_,
                                      static_cast<size_t>(width) * sizeof(float4), height),
                      "cudaMallocPitch history buffer failed");
    historyWidth_ = width;
    historyHeight_ = height;
    temporalHistoryValid_ = false;
    return true;
}

void CudaInteropSurface::ReleaseTemporalHistory() {
    if (deviceTemporalHistory_) {
        if (!SynchronizeStream()) return;
        cudaFree(deviceTemporalHistory_);
        deviceTemporalHistory_ = nullptr;
    }
    devicePitchHistory_ = 0;
    historyWidth_ = 0;
    historyHeight_ = 0;
    temporalHistoryValid_ = false;
}

void CudaInteropSurface::ResetTemporalHistory() {
    temporalHistoryValid_ = false;
}

bool CudaInteropSurface::EnsureStabilizationBuffers(unsigned int width,
                                                    unsigned int height) {
    if (deviceStabState_ && stabFullWidth_ == width &&
        stabFullHeight_ == height) {
        return true;
    }

    ReleaseStabilization();
    if (streamFaulted_) return false;

    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    unsigned int factorX = 0;
    unsigned int factorY = 0;
    unsigned int smallWidth = 0;
    unsigned int smallHeight = 0;
    ComputeStabilizationLumaDims(
        width, height, factorX, factorY, smallWidth, smallHeight);

    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabLuma_),
                                 static_cast<size_t>(smallWidth) * smallHeight * sizeof(float)),
                      "cudaMalloc stabilization luma buffer failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabLumaPrevious_),
                                 static_cast<size_t>(smallWidth) * smallHeight * sizeof(float)),
                      "cudaMalloc stabilization previous luma buffer failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabLumaReference_),
                                 static_cast<size_t>(smallWidth) * smallHeight * sizeof(float)),
                      "cudaMalloc stabilization reference luma buffer failed");
    const unsigned int pyramid1Width = (smallWidth + 1u) / 2u;
    const unsigned int pyramid1Height = (smallHeight + 1u) / 2u;
    const unsigned int pyramid2Width = (pyramid1Width + 1u) / 2u;
    const unsigned int pyramid2Height = (pyramid1Height + 1u) / 2u;
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceTripodCurrentPyramid1_),
                          static_cast<size_t>(pyramid1Width) * pyramid1Height *
                              sizeof(float)),
                      "cudaMalloc tripod current pyramid level 1 failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceTripodCurrentPyramid2_),
                          static_cast<size_t>(pyramid2Width) * pyramid2Height *
                              sizeof(float)),
                      "cudaMalloc tripod current pyramid level 2 failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceTripodReferencePyramid1_),
                          static_cast<size_t>(pyramid1Width) * pyramid1Height *
                              sizeof(float)),
                      "cudaMalloc tripod reference pyramid level 1 failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceTripodReferencePyramid2_),
                          static_cast<size_t>(pyramid2Width) * pyramid2Height *
                              sizeof(float)),
                      "cudaMalloc tripod reference pyramid level 2 failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceTripodReferenceFeatures_),
                          static_cast<size_t>(kMaximumStabilizationPairs) *
                              sizeof(TripodReferenceFeature)),
                      "cudaMalloc tripod prepared reference features failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodReferenceFeatureCount_),
                          sizeof(unsigned int)),
                      "cudaMalloc tripod prepared feature count failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceTripodRecoveryLuma_),
                          static_cast<size_t>(kTripodRecoveryKeyframeCount) *
                              smallWidth * smallHeight * sizeof(float)),
                      "cudaMalloc tripod recovery luma failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodRecoveryPyramid1_),
                          static_cast<size_t>(kTripodRecoveryKeyframeCount) *
                              pyramid1Width * pyramid1Height * sizeof(float)),
                      "cudaMalloc tripod recovery pyramid level 1 failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodRecoveryPyramid2_),
                          static_cast<size_t>(kTripodRecoveryKeyframeCount) *
                              pyramid2Width * pyramid2Height * sizeof(float)),
                      "cudaMalloc tripod recovery pyramid level 2 failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodRecoveryFeatures_),
                          static_cast<size_t>(kTripodRecoveryKeyframeCount) *
                              kMaximumStabilizationPairs *
                              sizeof(TripodReferenceFeature)),
                      "cudaMalloc tripod recovery features failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodRecoveryFeatureCounts_),
                          kTripodRecoveryKeyframeCount *
                              sizeof(unsigned int)),
                      "cudaMalloc tripod recovery feature counts failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodKeyframeOrigins_),
                          kTripodKeyframeCount * sizeof(float4)),
                      "cudaMalloc tripod keyframe origins failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodKeyframeValid_),
                          kTripodKeyframeCount * sizeof(unsigned int)),
                      "cudaMalloc tripod keyframe validity failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodMatchCandidates_),
                          kTripodKeyframeCount *
                              sizeof(TripodMatchCandidate)),
                      "cudaMalloc tripod match candidates failed");
    ThrowIfCudaFailed(
        cudaMemsetAsync(deviceTripodKeyframeValid_, 0,
                        kTripodKeyframeCount * sizeof(unsigned int), stream_),
        "cudaMemsetAsync tripod keyframe validity failed");
    ThrowIfCudaFailed(
        cudaMemsetAsync(deviceTripodMatchCandidates_, 0,
                        kTripodKeyframeCount *
                            sizeof(TripodMatchCandidate),
                        stream_),
        "cudaMemsetAsync tripod match candidates failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodReferenceAccumulator_),
                          static_cast<size_t>(smallWidth) * smallHeight *
                              sizeof(float)),
                      "cudaMalloc tripod reference accumulator failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(
                              &deviceTripodReferenceSampleCounts_),
                          static_cast<size_t>(smallWidth) * smallHeight *
                              sizeof(unsigned int)),
                      "cudaMalloc tripod reference sample counts failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceTripodFocusScore_),
                          sizeof(float)),
                      "cudaMalloc tripod focus score failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceTripodBestFocusScore_),
                          sizeof(float)),
                      "cudaMalloc tripod best focus score failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceTripodSelectionFlag_),
                          sizeof(unsigned int)),
                      "cudaMalloc tripod selection flag failed");
    ThrowIfCudaFailed(
        cudaMallocPitch(reinterpret_cast<void**>(&deviceBumpHoldFrame_),
                        &deviceBumpHoldPitch_,
                        static_cast<size_t>(width) * sizeof(uchar4), height),
        "cudaMallocPitch bump hold frame failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabColProjCurr_),
                                 static_cast<size_t>(smallWidth) * sizeof(float)),
                      "cudaMalloc stabilization column projection failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabRowProjCurr_),
                                 static_cast<size_t>(smallHeight) * sizeof(float)),
                      "cudaMalloc stabilization row projection failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabColProjPrev_),
                                 static_cast<size_t>(smallWidth) * sizeof(float)),
                      "cudaMalloc stabilization previous column projection failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabRowProjPrev_),
                                 static_cast<size_t>(smallHeight) * sizeof(float)),
                      "cudaMalloc stabilization previous row projection failed");
    // Allocated last so a partially built set is re-released on the next call.
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabState_),
                                 sizeof(StabilizationState)),
                      "cudaMalloc stabilization state failed");
    ThrowIfCudaFailed(cudaMalloc(
                          reinterpret_cast<void**>(&deviceBumpHoldState_),
                          sizeof(BumpHoldState)),
                      "cudaMalloc bump hold state failed");
    LaunchResetBumpHoldState(deviceBumpHoldState_, stream_);
    ThrowIfCudaFailed(cudaMallocHost(
                          reinterpret_cast<void**>(&hostStabDiagnostics_),
                          5 * sizeof(float4)),
                      "cudaMallocHost stabilization diagnostics failed");
    std::memset(hostStabDiagnostics_, 0, 5 * sizeof(float4));
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabPairs_),
                                 static_cast<size_t>(kMaximumStabilizationPairs) *
                                     sizeof(float4)),
                      "cudaMalloc stabilization motion pairs failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceStabPairCount_),
                                 sizeof(unsigned int)),
                      "cudaMalloc stabilization pair count failed");

    stabSmallWidth_ = smallWidth;
    stabSmallHeight_ = smallHeight;
    stabFactorX_ = factorX;
    stabFactorY_ = factorY;
    stabFullWidth_ = width;
    stabFullHeight_ = height;
    stabPrevValid_ = false;
    tripodReferenceValid_ = false;
    tripodRelockPending_ = false;
    virtualTripodActive_ = false;
    bumpHoldActive_ = false;
    tripodReferencePrepared_ = false;
    tripodReferenceBuildFrame_ = 0;
    tripodReferenceAccumulationFrame_ = 0;
    tripodKeyframeAdmissionFrames_ = 0;
    tripodNextRecoverySlot_ = 1u;
    return true;
}

void CudaInteropSurface::ReleaseStabilization() {
    if (deviceStabLuma_ || deviceStabLumaPrevious_ || deviceStabLumaReference_ ||
        deviceTripodCurrentPyramid1_ || deviceTripodCurrentPyramid2_ ||
        deviceTripodReferencePyramid1_ || deviceTripodReferencePyramid2_ ||
        deviceTripodReferenceFeatures_ ||
        deviceTripodReferenceFeatureCount_ ||
        deviceTripodRecoveryLuma_ ||
        deviceTripodRecoveryPyramid1_ ||
        deviceTripodRecoveryPyramid2_ ||
        deviceTripodRecoveryFeatures_ ||
        deviceTripodRecoveryFeatureCounts_ ||
        deviceTripodKeyframeOrigins_ ||
        deviceTripodKeyframeValid_ ||
        deviceTripodMatchCandidates_ ||
        deviceTripodReferenceAccumulator_ ||
        deviceTripodReferenceSampleCounts_ || deviceTripodFocusScore_ ||
        deviceTripodBestFocusScore_ || deviceTripodSelectionFlag_ ||
        deviceBumpHoldFrame_ || deviceBumpHoldState_ ||
        deviceStabColProjCurr_ || deviceStabRowProjCurr_ ||
        deviceStabColProjPrev_ || deviceStabRowProjPrev_ || deviceStabState_ ||
        hostStabDiagnostics_ ||
        deviceStabPairs_ || deviceStabPairCount_) {
        if (!SynchronizeStream()) return;
    }
    if (deviceStabLuma_) {
        cudaFree(deviceStabLuma_);
        deviceStabLuma_ = nullptr;
    }
    if (deviceStabLumaPrevious_) {
        cudaFree(deviceStabLumaPrevious_);
        deviceStabLumaPrevious_ = nullptr;
    }
    if (deviceStabLumaReference_) {
        cudaFree(deviceStabLumaReference_);
        deviceStabLumaReference_ = nullptr;
    }
    if (deviceTripodCurrentPyramid1_) {
        cudaFree(deviceTripodCurrentPyramid1_);
        deviceTripodCurrentPyramid1_ = nullptr;
    }
    if (deviceTripodCurrentPyramid2_) {
        cudaFree(deviceTripodCurrentPyramid2_);
        deviceTripodCurrentPyramid2_ = nullptr;
    }
    if (deviceTripodReferencePyramid1_) {
        cudaFree(deviceTripodReferencePyramid1_);
        deviceTripodReferencePyramid1_ = nullptr;
    }
    if (deviceTripodReferencePyramid2_) {
        cudaFree(deviceTripodReferencePyramid2_);
        deviceTripodReferencePyramid2_ = nullptr;
    }
    if (deviceTripodReferenceFeatures_) {
        cudaFree(deviceTripodReferenceFeatures_);
        deviceTripodReferenceFeatures_ = nullptr;
    }
    if (deviceTripodReferenceFeatureCount_) {
        cudaFree(deviceTripodReferenceFeatureCount_);
        deviceTripodReferenceFeatureCount_ = nullptr;
    }
    if (deviceTripodRecoveryLuma_) {
        cudaFree(deviceTripodRecoveryLuma_);
        deviceTripodRecoveryLuma_ = nullptr;
    }
    if (deviceTripodRecoveryPyramid1_) {
        cudaFree(deviceTripodRecoveryPyramid1_);
        deviceTripodRecoveryPyramid1_ = nullptr;
    }
    if (deviceTripodRecoveryPyramid2_) {
        cudaFree(deviceTripodRecoveryPyramid2_);
        deviceTripodRecoveryPyramid2_ = nullptr;
    }
    if (deviceTripodRecoveryFeatures_) {
        cudaFree(deviceTripodRecoveryFeatures_);
        deviceTripodRecoveryFeatures_ = nullptr;
    }
    if (deviceTripodRecoveryFeatureCounts_) {
        cudaFree(deviceTripodRecoveryFeatureCounts_);
        deviceTripodRecoveryFeatureCounts_ = nullptr;
    }
    if (deviceTripodKeyframeOrigins_) {
        cudaFree(deviceTripodKeyframeOrigins_);
        deviceTripodKeyframeOrigins_ = nullptr;
    }
    if (deviceTripodKeyframeValid_) {
        cudaFree(deviceTripodKeyframeValid_);
        deviceTripodKeyframeValid_ = nullptr;
    }
    if (deviceTripodMatchCandidates_) {
        cudaFree(deviceTripodMatchCandidates_);
        deviceTripodMatchCandidates_ = nullptr;
    }
    if (deviceTripodReferenceAccumulator_) {
        cudaFree(deviceTripodReferenceAccumulator_);
        deviceTripodReferenceAccumulator_ = nullptr;
    }
    if (deviceTripodReferenceSampleCounts_) {
        cudaFree(deviceTripodReferenceSampleCounts_);
        deviceTripodReferenceSampleCounts_ = nullptr;
    }
    if (deviceTripodFocusScore_) {
        cudaFree(deviceTripodFocusScore_);
        deviceTripodFocusScore_ = nullptr;
    }
    if (deviceTripodBestFocusScore_) {
        cudaFree(deviceTripodBestFocusScore_);
        deviceTripodBestFocusScore_ = nullptr;
    }
    if (deviceTripodSelectionFlag_) {
        cudaFree(deviceTripodSelectionFlag_);
        deviceTripodSelectionFlag_ = nullptr;
    }
    if (deviceBumpHoldFrame_) {
        cudaFree(deviceBumpHoldFrame_);
        deviceBumpHoldFrame_ = nullptr;
    }
    deviceBumpHoldPitch_ = 0;
    if (deviceBumpHoldState_) {
        cudaFree(deviceBumpHoldState_);
        deviceBumpHoldState_ = nullptr;
    }
    if (deviceStabColProjCurr_) {
        cudaFree(deviceStabColProjCurr_);
        deviceStabColProjCurr_ = nullptr;
    }
    if (deviceStabRowProjCurr_) {
        cudaFree(deviceStabRowProjCurr_);
        deviceStabRowProjCurr_ = nullptr;
    }
    if (deviceStabColProjPrev_) {
        cudaFree(deviceStabColProjPrev_);
        deviceStabColProjPrev_ = nullptr;
    }
    if (deviceStabRowProjPrev_) {
        cudaFree(deviceStabRowProjPrev_);
        deviceStabRowProjPrev_ = nullptr;
    }
    if (deviceStabState_) {
        cudaFree(deviceStabState_);
        deviceStabState_ = nullptr;
    }
    if (hostStabDiagnostics_) {
        cudaFreeHost(hostStabDiagnostics_);
        hostStabDiagnostics_ = nullptr;
    }
    if (deviceStabPairs_) {
        cudaFree(deviceStabPairs_);
        deviceStabPairs_ = nullptr;
    }
    if (deviceStabPairCount_) {
        cudaFree(deviceStabPairCount_);
        deviceStabPairCount_ = nullptr;
    }
    if (stabilizerTimingStartEvent_) {
        cudaEventDestroy(stabilizerTimingStartEvent_);
        stabilizerTimingStartEvent_ = nullptr;
    }
    if (stabilizerTimingStopEvent_) {
        cudaEventDestroy(stabilizerTimingStopEvent_);
        stabilizerTimingStopEvent_ = nullptr;
    }
    stabilizerTimingPending_ = false;
    stabilizerTimingFrameCounter_ = 0;
    lastStabilizerMs_ = -1.0f;
    stabSmallWidth_ = 0;
    stabSmallHeight_ = 0;
    stabFactorX_ = 0;
    stabFactorY_ = 0;
    stabFullWidth_ = 0;
    stabFullHeight_ = 0;
    stabPrevValid_ = false;
    tripodReferenceValid_ = false;
    tripodRelockPending_ = false;
    virtualTripodActive_ = false;
    bumpHoldActive_ = false;
    tripodReferencePrepared_ = false;
    tripodReferenceBuildFrame_ = 0;
    tripodReferenceAccumulationFrame_ = 0;
    tripodKeyframeAdmissionFrames_ = 0;
    tripodNextRecoverySlot_ = 1u;
    activeStabilizerEngine_ = -1;
    stabilizerStatus_ = "Stabilizer off";
}

// Host-side flag only: with previousValid false the estimate kernel zeroes the
// motion state and re-seeds the previous projections on the next frame, so no
// device work (and no stream sync) is needed here.
void CudaInteropSurface::ResetStabilization() {
    stabPrevValid_ = false;
    tripodReferenceValid_ = false;
    tripodRelockPending_ = false;
    virtualTripodActive_ = false;
    bumpHoldActive_ = false;
    tripodReferencePrepared_ = false;
    tripodReferenceBuildFrame_ = 0;
    tripodReferenceAccumulationFrame_ = 0;
    tripodKeyframeAdmissionFrames_ = 0;
    tripodNextRecoverySlot_ = 1u;
    activeStabilizerEngine_ = -1;
    lastStabilizerMs_ = -1.0f;
    stabilizerStatus_ = "Stabilizer resetting";
}

// Device staging for raw camera formats (NV12 / YUY2). Plane row widths:
//   NV12  plane1 = Y (width bytes), plane2 = interleaved UV (2*ceil(w/2) bytes,
//          ceil(h/2) rows)
//   YUY2  plane1 = packed Y0 U Y1 V (4*ceil(w/2) bytes), plane2 unused
bool CudaInteropSurface::EnsureRawInputBuffers(unsigned int width, unsigned int height, int format) {
    if (deviceRawPlane1_ && rawWidth_ == width && rawHeight_ == height && rawFormat_ == format) {
        return true;
    }

    ReleaseRawInput();
    if (streamFaulted_) return false;

    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    if (format == 1) {
        ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&deviceRawPlane1_), &rawPlane1Pitch_,
                                          static_cast<size_t>(width), height),
                          "cudaMallocPitch NV12 Y plane failed");
        const size_t uvRowBytes = (static_cast<size_t>(width) + 1) / 2 * 2;
        const size_t uvRows = (static_cast<size_t>(height) + 1) / 2;
        ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&deviceRawPlane2_), &rawPlane2Pitch_,
                                          uvRowBytes, uvRows),
                          "cudaMallocPitch NV12 UV plane failed");
    } else {
        const size_t rowBytes = (static_cast<size_t>(width) + 1) / 2 * 4;
        ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&deviceRawPlane1_), &rawPlane1Pitch_,
                                          rowBytes, height),
                          "cudaMallocPitch YUY2 buffer failed");
    }

    rawWidth_ = width;
    rawHeight_ = height;
    rawFormat_ = format;
    return true;
}

bool CudaInteropSurface::EnsurePinnedUploadRing(size_t requiredBytes) {
    if (requiredBytes == 0) {
        return false;
    }
    if (pinnedUploadCapacity_ >= requiredBytes &&
        std::all_of(pinnedUploadSlots_.begin(), pinnedUploadSlots_.end(),
                    [](const PinnedUploadSlot& slot) { return slot.data != nullptr; })) {
        return true;
    }

    // Reallocation is resolution/format-change work, never steady-state work.
    // Drain the stream because a queued H2D copy may still reference a slot.
    if (!SynchronizeStream()) return false;
    ReleasePinnedUploadRing();

    try {
        for (PinnedUploadSlot& slot : pinnedUploadSlots_) {
            ThrowIfCudaFailed(cudaMallocHost(reinterpret_cast<void**>(&slot.data), requiredBytes),
                              "cudaMallocHost upload staging slot failed");
            ThrowIfCudaFailed(cudaEventCreateWithFlags(&slot.uploadComplete, cudaEventDisableTiming),
                              "cudaEventCreate upload staging slot failed");
        }
    } catch (...) {
        ReleasePinnedUploadRing();
        throw;
    }
    pinnedUploadCapacity_ = requiredBytes;
    pinnedUploadNextSlot_ = 0;
    return true;
}

void CudaInteropSurface::ReleasePinnedUploadRing() {
    for (PinnedUploadSlot& slot : pinnedUploadSlots_) {
        if (slot.uploadComplete) {
            cudaEventDestroy(slot.uploadComplete);
            slot.uploadComplete = nullptr;
        }
        if (slot.data) {
            cudaFreeHost(slot.data);
            slot.data = nullptr;
        }
        slot.uploadPending = false;
    }
    pinnedUploadCapacity_ = 0;
    pinnedUploadNextSlot_ = 0;
}

// Conversion target at pre-rotation extent; only allocated when the GPU also
// rotates (for turns == 0 the converter writes straight into buffer A).
bool CudaInteropSurface::EnsurePreRotateBuffer(unsigned int width, unsigned int height) {
    if (devicePreRotate_ && preRotateWidth_ == width && preRotateHeight_ == height) {
        return true;
    }

    if (devicePreRotate_) {
        if (!SynchronizeStream()) return false;
        cudaFree(devicePreRotate_);
        devicePreRotate_ = nullptr;
    }

    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&devicePreRotate_), &devicePitchPreRotate_,
                                      static_cast<size_t>(width) * sizeof(uchar4), height),
                      "cudaMallocPitch pre-rotate buffer failed");
    preRotateWidth_ = width;
    preRotateHeight_ = height;
    return true;
}

void CudaInteropSurface::ReleaseRawInput() {
    if (deviceRawPlane1_ || deviceRawPlane2_ || devicePreRotate_) {
        if (!SynchronizeStream()) return;
    }
    if (deviceRawPlane1_) {
        cudaFree(deviceRawPlane1_);
        deviceRawPlane1_ = nullptr;
    }
    if (deviceRawPlane2_) {
        cudaFree(deviceRawPlane2_);
        deviceRawPlane2_ = nullptr;
    }
    if (devicePreRotate_) {
        cudaFree(devicePreRotate_);
        devicePreRotate_ = nullptr;
    }
    rawPlane1Pitch_ = 0;
    rawPlane2Pitch_ = 0;
    rawWidth_ = 0;
    rawHeight_ = 0;
    rawFormat_ = -1;
    devicePitchPreRotate_ = 0;
    preRotateWidth_ = 0;
    preRotateHeight_ = 0;
}

bool CudaInteropSurface::EnsureKeystoneResources(unsigned int width, unsigned int height) {
    if (deviceKeystoneLuma_ && keystoneFullWidth_ == width && keystoneFullHeight_ == height) {
        return true;
    }

    ReleaseKeystone();
    if (streamFaulted_) return false;

    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    unsigned int factorX = 0;
    unsigned int factorY = 0;
    unsigned int smallWidth = 0;
    unsigned int smallHeight = 0;
    ComputeSmallLumaDims(width, height, factorX, factorY, smallWidth, smallHeight);

    const size_t lumaBytes = static_cast<size_t>(smallWidth) * smallHeight * sizeof(float);
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceKeystoneLuma_), lumaBytes),
                      "cudaMalloc keystone luma buffer failed");
    // Pinned so the device->host snapshot copy can run truly asynchronously.
    ThrowIfCudaFailed(cudaMallocHost(reinterpret_cast<void**>(&hostKeystoneLuma_), lumaBytes),
                      "cudaMallocHost keystone snapshot buffer failed");
    ThrowIfCudaFailed(cudaEventCreateWithFlags(&keystoneCopyEvent_, cudaEventDisableTiming),
                      "cudaEventCreate keystone event failed");

    keystoneSmallWidth_ = smallWidth;
    keystoneSmallHeight_ = smallHeight;
    keystoneFactorX_ = factorX;
    keystoneFactorY_ = factorY;
    keystoneFullWidth_ = width;
    keystoneFullHeight_ = height;
    ResetKeystone();
    return true;
}

void CudaInteropSurface::ReleaseSuperRes() {
    if (deviceSuperResOutput_) {
        cudaFree(deviceSuperResOutput_);
        deviceSuperResOutput_ = nullptr;
    }
    devicePitchSuperResOutput_ = 0;
    deviceSuperResOutputWidth_ = 0;
    deviceSuperResOutputHeight_ = 0;
    if (maxineSuperRes_) {
        maxineSuperRes_->Teardown();
        maxineSuperRes_.reset();
    }
    if (superResStartEvent_) {
        cudaEventDestroy(superResStartEvent_);
        superResStartEvent_ = nullptr;
    }
    if (superResStopEvent_) {
        cudaEventDestroy(superResStopEvent_);
        superResStopEvent_ = nullptr;
    }
    superResTimingPending_ = false;
    superResAutoDisabled_ = false;
    superResFailureLatched_ = false;
    superResFailSrcWidth_ = 0;
    superResFailSrcHeight_ = 0;
    superResFailFactorNum_ = 0;
    superResFailFactorDen_ = 0;
    superResRequestedLastFrame_ = false;
    superResActive_ = false;
    superResSourceWidth_ = 0;
    superResSourceHeight_ = 0;
    superResFactorValue_ = 0.0f;
    superResRoi_ = {};
    superResWarmupSamples_ = 0;
    superResTimingSamples_ = 0;
    superResTimingTotalMs_ = 0.0f;
    superResLastAverageMs_ = -1.0f;
    superResStatus_.clear();
}

bool CudaInteropSurface::EnsureSuperResOutputBuffer(unsigned int width,
                                                   unsigned int height) {
    if (deviceSuperResOutput_ &&
        deviceSuperResOutputWidth_ == width &&
        deviceSuperResOutputHeight_ == height) {
        return true;
    }
    if (deviceSuperResOutput_) {
        cudaFree(deviceSuperResOutput_);
        deviceSuperResOutput_ = nullptr;
    }
    devicePitchSuperResOutput_ = 0;
    deviceSuperResOutputWidth_ = 0;
    deviceSuperResOutputHeight_ = 0;
    ThrowIfCudaFailed(
        cudaMallocPitch(
            reinterpret_cast<void**>(&deviceSuperResOutput_),
            &devicePitchSuperResOutput_,
            static_cast<size_t>(width) * sizeof(uchar4),
            height),
        "cudaMallocPitch SuperRes output failed");
    deviceSuperResOutputWidth_ = width;
    deviceSuperResOutputHeight_ = height;
    return true;
}

void CudaInteropSurface::SetSuperResPerformanceOverride(bool enabled) {
    superResPerformanceOverride_ = enabled;
    if (enabled) {
        superResAutoDisabled_ = false;
        superResWarmupSamples_ = 0;
        superResTimingSamples_ = 0;
        superResTimingTotalMs_ = 0.0f;
        superResStatus_ = "SuperRes performance guard overridden by user";
    } else if (superResLastAverageMs_ > kSuperResLatencyTargetMs) {
        superResAutoDisabled_ = true;
        superResStatus_ = SuperResTimingStatus(
            "SuperRes exceeded the performance target - using NIS;",
            superResLastAverageMs_);
    }
}

void CudaInteropSurface::ResetSuperRes() {
    if (!SynchronizeStream()) return;
    ReleaseSuperRes();
}

void CudaInteropSurface::ConsumeSuperResTiming() {
    if (!superResTimingPending_ || !superResStopEvent_) {
        return;
    }
    const cudaError_t query = cudaEventQuery(superResStopEvent_);
    if (query == cudaErrorNotReady) {
        return;
    }
    if (query != cudaSuccess) {
        superResTimingPending_ = false;
        return;
    }
    float elapsedMs = 0.0f;
    if (cudaEventElapsedTime(&elapsedMs, superResStartEvent_, superResStopEvent_) == cudaSuccess) {
        if (superResWarmupSamples_ < kSuperResWarmupFrames) {
            ++superResWarmupSamples_;
        } else {
            superResTimingTotalMs_ += elapsedMs;
            ++superResTimingSamples_;
        }
        if (superResTimingSamples_ >= kSuperResTimingFrames) {
            const float averageMs = superResTimingTotalMs_ / static_cast<float>(superResTimingSamples_);
            superResLastAverageMs_ = averageMs;
            if (averageMs > kSuperResLatencyTargetMs && !superResPerformanceOverride_) {
                superResAutoDisabled_ = true;
                superResStatus_ = SuperResTimingStatus(
                    "SuperRes exceeded the performance target - using NIS;", averageMs);
            } else {
                superResStatus_ = averageMs > kSuperResLatencyTargetMs
                                      ? SuperResTimingStatus(
                                            "SuperRes active by user override;", averageMs)
                                      : SuperResTimingStatus("SuperRes active;", averageMs);
            }
            superResTimingSamples_ = 0;
            superResTimingTotalMs_ = 0.0f;
        }
    }
    superResTimingPending_ = false;
}

void CudaInteropSurface::UpdateSuperResCache(
    const uchar4* source,
    size_t sourcePitch,
    uchar4* destination,
    size_t destinationPitch,
    unsigned int width,
    unsigned int height,
    const ProcessingSettings& settings) {
    ConsumeSuperResTiming();
    superResActive_ = false;
    superResRoi_.valid = false;

    // SuperRes is a camera-clock cache, not the viewport transform itself.
    // settings.zoomAmount/center describe the requested viewport ROI even
    // though settings.enableZoom remains false for the full-scene pipeline.
    const bool ultraNeedsUpscale =
        settings.mlSuperResUltra1440p &&
        (superResWidth_ > width || superResHeight_ > height);
    const bool requested =
        settings.enableMlSuperRes && superResLevel0Array_ != nullptr &&
        (ultraNeedsUpscale ||
         (!settings.mlSuperResUltra1440p &&
          settings.zoomAmount >= 1.33f));
    if (!requested && superResRequestedLastFrame_) {
        superResAutoDisabled_ = false;
        superResFailureLatched_ = false;
        superResWarmupSamples_ = 0;
        superResTimingSamples_ = 0;
        superResTimingTotalMs_ = 0.0f;
        superResLastAverageMs_ = -1.0f;
        superResStatus_.clear();
    }
    superResRequestedLastFrame_ = requested;
    if (!requested) {
        if (settings.enableMlSuperRes && settings.mlSuperResUltra1440p) {
            superResStatus_ =
                "Ultra uses the native full frame; the camera is already 1440p or higher";
        }
        return;
    }
    if (superResAutoDisabled_) {
        return;
    }

#if OKUFLOW_ENABLE_TEXT_SR
    struct SuperResScale {
        unsigned int numerator;
        unsigned int denominator;
    };
    // In viewport-target mode, descending order selects the strongest
    // supported factor that does not exceed the user's requested
    // magnification. Ultra mode instead matches the separately allocated
    // full-frame cache exactly (720p -> 1440p at 2x, 1080p -> 1440p at 4/3x).
    constexpr SuperResScale kSupportedScales[] = {
        {4u, 1u}, {3u, 1u}, {2u, 1u}, {3u, 2u}, {4u, 3u}};
    unsigned int factorNum = 0;
    unsigned int factorDen = 0;
    unsigned int sourceWidth = 0;
    unsigned int sourceHeight = 0;
    const unsigned int outputWidth = superResWidth_;
    const unsigned int outputHeight = superResHeight_;
    for (const SuperResScale& scale : kSupportedScales) {
        const float factor =
            static_cast<float>(scale.numerator) /
            static_cast<float>(scale.denominator);
        unsigned int candidateWidth = 0;
        unsigned int candidateHeight = 0;
        if (settings.mlSuperResUltra1440p) {
            if (static_cast<std::uint64_t>(width) * scale.numerator !=
                    static_cast<std::uint64_t>(outputWidth) *
                        scale.denominator ||
                static_cast<std::uint64_t>(height) * scale.numerator !=
                    static_cast<std::uint64_t>(outputHeight) *
                        scale.denominator) {
                continue;
            }
            candidateWidth = width;
            candidateHeight = height;
        } else {
            if (settings.zoomAmount + 0.005f < factor ||
                (outputWidth * scale.denominator) % scale.numerator != 0u ||
                (outputHeight * scale.denominator) % scale.numerator != 0u) {
                continue;
            }
            candidateWidth =
                outputWidth * scale.denominator / scale.numerator;
            candidateHeight =
                outputHeight * scale.denominator / scale.numerator;
        }
        if (candidateWidth > width || candidateHeight > height) {
            continue;
        }
        if ((candidateWidth & 1u) != 0u ||
            (candidateHeight & 1u) != 0u ||
            candidateWidth < 160u || candidateHeight < 90u) {
            continue;
        }
        factorNum = scale.numerator;
        factorDen = scale.denominator;
        sourceWidth = candidateWidth;
        sourceHeight = candidateHeight;
        break;
    }

    if (superResFailureLatched_ &&
        (superResFailSrcWidth_ != sourceWidth ||
         superResFailSrcHeight_ != sourceHeight ||
         superResFailFactorNum_ != factorNum ||
         superResFailFactorDen_ != factorDen)) {
        superResFailureLatched_ = false;
    }
    const auto latchFailure = [&]() {
        superResFailureLatched_ = true;
        superResFailSrcWidth_ = sourceWidth;
        superResFailSrcHeight_ = sourceHeight;
        superResFailFactorNum_ = factorNum;
        superResFailFactorDen_ = factorDen;
    };
    if (superResFailureLatched_) {
        return;
    }
    if (factorNum == 0u) {
        superResStatus_ =
            "Scene dimensions do not support an exact NVIDIA SuperRes scale - using NIS";
        latchFailure();
        return;
    }

    const float centerX =
        std::clamp(settings.zoomCenterX, 0.0f, 1.0f) *
        static_cast<float>(width - 1u);
    const float centerY =
        std::clamp(settings.zoomCenterY, 0.0f, 1.0f) *
        static_cast<float>(height - 1u);
    const unsigned int sourceX = settings.mlSuperResUltra1440p
        ? 0u
        : static_cast<unsigned int>(std::clamp(
              static_cast<int>(std::lround(
                  centerX -
                  (static_cast<float>(sourceWidth) - 1.0f) * 0.5f)),
              0,
              static_cast<int>(width - sourceWidth)));
    const unsigned int sourceY = settings.mlSuperResUltra1440p
        ? 0u
        : static_cast<unsigned int>(std::clamp(
              static_cast<int>(std::lround(
                  centerY -
                  (static_cast<float>(sourceHeight) - 1.0f) * 0.5f)),
              0,
              static_cast<int>(height - sourceHeight)));
    const auto* sourceRoi =
        reinterpret_cast<const unsigned char*>(source) +
        static_cast<size_t>(sourceY) * sourcePitch +
        static_cast<size_t>(sourceX) * sizeof(uchar4);

    if (!maxineSuperRes_) {
        maxineSuperRes_ = std::make_unique<MaxineSuperRes>();
    }
    maxineSuperRes_->SetStrength(
        std::clamp(settings.mlSuperResStrength, 0.0f, 1.0f));
    if (!maxineSuperRes_->Ensure(sourceWidth,
                                 sourceHeight,
                                 outputWidth,
                                 outputHeight,
                                 reinterpret_cast<void*>(stream_))) {
        superResStatus_ =
            maxineSuperRes_->LastError() + " - using NIS";
        latchFailure();
        return;
    }

    if (!superResStartEvent_) {
        ThrowIfCudaFailed(cudaEventCreate(&superResStartEvent_),
                          "cudaEventCreate SuperRes start failed");
        ThrowIfCudaFailed(cudaEventCreate(&superResStopEvent_),
                          "cudaEventCreate SuperRes stop failed");
    }
    const bool measure = !superResTimingPending_;
    if (measure) {
        ThrowIfCudaFailed(cudaEventRecord(superResStartEvent_, stream_),
                          "cudaEventRecord SuperRes start failed");
    }
    uchar4* superResDestination = destination;
    size_t superResDestinationPitch = destinationPitch;
    if (outputWidth != width || outputHeight != height) {
        EnsureSuperResOutputBuffer(outputWidth, outputHeight);
        superResDestination = deviceSuperResOutput_;
        superResDestinationPitch = devicePitchSuperResOutput_;
    }
    // NvCVImage_Transfer writes the complete BGRA destination after inference,
    // so clearing the destination first only consumes bandwidth.
    if (!maxineSuperRes_->Run(
            sourceRoi,
            sourcePitch,
            superResDestination,
            superResDestinationPitch)) {
        superResStatus_ =
            maxineSuperRes_->LastError() + " - using NIS";
        latchFailure();
        return;
    }
    if (measure) {
        ThrowIfCudaFailed(cudaEventRecord(superResStopEvent_, stream_),
                          "cudaEventRecord SuperRes stop failed");
        superResTimingPending_ = true;
    }
    ThrowIfCudaFailed(
        cudaMemcpy2DToArrayAsync(superResLevel0Array_,
                                 0,
                                 0,
                                 superResDestination,
                                 superResDestinationPitch,
                                 static_cast<size_t>(outputWidth) * sizeof(uchar4),
                                 outputHeight,
                                 cudaMemcpyDeviceToDevice,
                                 stream_),
        "cudaMemcpy2DToArrayAsync SuperRes cache failed");

    const float factorValue =
        static_cast<float>(factorNum) / static_cast<float>(factorDen);
    superResActive_ = true;
    superResSourceWidth_ = sourceWidth;
    superResSourceHeight_ = sourceHeight;
    superResFactorValue_ = factorValue;
    superResRoi_ = {
        true,
        ++superResGeneration_,
        static_cast<float>(sourceX) / static_cast<float>(width),
        static_cast<float>(sourceY) / static_cast<float>(height),
        static_cast<float>(sourceWidth) / static_cast<float>(width),
        static_cast<float>(sourceHeight) / static_cast<float>(height),
        outputWidth,
        outputHeight,
        factorValue,
    };
    if (superResLastAverageMs_ < 0.0f) {
        superResStatus_ = superResWarmupSamples_ < kSuperResWarmupFrames
                              ? "NVIDIA Video Effects SuperRes warming up"
                              : "NVIDIA Video Effects SuperRes measuring performance";
    } else if (superResPerformanceOverride_ &&
               superResLastAverageMs_ > kSuperResLatencyTargetMs) {
        superResStatus_ = SuperResTimingStatus(
            "SuperRes active by user override;", superResLastAverageMs_);
    } else {
        superResStatus_ = SuperResTimingStatus(
            "NVIDIA Video Effects SuperRes active;", superResLastAverageMs_);
    }
#else
    superResStatus_ =
        "SuperRes support is disabled in this build - using NIS";
#endif
}

void CudaInteropSurface::UpdateSpatialCache(
    const uchar4* source, size_t sourcePitch,
    uchar4* destination, size_t destinationPitch,
    unsigned int width, unsigned int height,
    const SpatialCacheGeometry& geometry,
    const ProcessingSettings& settings) {
    if (!geometry.valid || !superResLevel0Array_ || superResRoi_.valid) {
        return;
    }
    const auto* roi = reinterpret_cast<const uchar4*>(
        reinterpret_cast<const unsigned char*>(source) +
        static_cast<size_t>(geometry.sourceY) * sourcePitch) + geometry.sourceX;
    if (settings.spatialUpscaler == SpatialUpscaler::kNis) {
        LaunchNisLinear(destination, destinationPitch, roi, sourcePitch,
                        geometry.sourceWidth, geometry.sourceHeight,
                        geometry.outputWidth, geometry.outputHeight,
                        settings.spatialSharpness, stream_);
    } else {
        LaunchFsrEasuRcasLinear(destination, destinationPitch,
                                deviceScratch_, devicePitchScratch_,
                                roi, sourcePitch,
                                geometry.sourceWidth, geometry.sourceHeight,
                                geometry.outputWidth, geometry.outputHeight,
                                settings.spatialSharpness, stream_);
    }
    ThrowIfCudaFailed(cudaMemcpy2DToArrayAsync(
        superResLevel0Array_, 0, 0, destination, destinationPitch,
        static_cast<size_t>(geometry.outputWidth) * sizeof(uchar4),
        geometry.outputHeight, cudaMemcpyDeviceToDevice, stream_),
        "cudaMemcpy2DToArrayAsync spatial cache failed");
    PadSpatialCacheBorder(superResLevel0Array_, destination, destinationPitch,
                          geometry.outputWidth, geometry.outputHeight,
                          superResWidth_, superResHeight_, stream_);
    superResRoi_ = {
        true, ++superResGeneration_,
        static_cast<float>(geometry.sourceX) / width,
        static_cast<float>(geometry.sourceY) / height,
        static_cast<float>(geometry.sourceWidth) / width,
        static_cast<float>(geometry.sourceHeight) / height,
        geometry.outputWidth, geometry.outputHeight,
        static_cast<float>(geometry.outputWidth) / geometry.sourceWidth};
    // IsSuperResActive describes the AI backend, not shared cache validity.
}

// P8 GPU timing consumer: polls (never waits on) the stop event recorded by a
// previous sampled frame and folds the elapsed time into lastGpuFrameMs_.
void CudaInteropSurface::ConsumeProcessTiming() {
    if (!processTimingPending_ || !processTimingStopEvent_) {
        return;
    }
    const cudaError_t query = cudaEventQuery(processTimingStopEvent_);
    if (query == cudaErrorNotReady) {
        return;
    }
    processTimingPending_ = false;
    if (query != cudaSuccess) {
        return;
    }
    float elapsedMs = 0.0f;
    if (cudaEventElapsedTime(&elapsedMs, processTimingStartEvent_,
                             processTimingStopEvent_) == cudaSuccess) {
        lastGpuFrameMs_ = elapsedMs;
    }
    float inputMs = 0.0f;
    float geometryMs = 0.0f;
    float effectsMs = 0.0f;
    float outputMs = 0.0f;
    if (processTimingInputEvent_ && processTimingGeometryEvent_ &&
        processTimingEffectsEvent_ &&
        cudaEventElapsedTime(&inputMs,
                             processTimingStartEvent_,
                             processTimingInputEvent_) == cudaSuccess &&
        cudaEventElapsedTime(&geometryMs,
                             processTimingInputEvent_,
                             processTimingGeometryEvent_) == cudaSuccess &&
        cudaEventElapsedTime(&effectsMs,
                             processTimingGeometryEvent_,
                             processTimingEffectsEvent_) == cudaSuccess &&
        cudaEventElapsedTime(&outputMs,
                             processTimingEffectsEvent_,
                             processTimingStopEvent_) == cudaSuccess) {
        lastGpuStageTimings_ = {
            inputMs,
            geometryMs,
            effectsMs,
            outputMs,
            lastGpuStageTimings_.generation + 1,
        };
    }
}

void CudaInteropSurface::ConsumeStabilizerTiming() {
    if (!stabilizerTimingPending_ || !stabilizerTimingStopEvent_) {
        return;
    }
    const cudaError_t query = cudaEventQuery(stabilizerTimingStopEvent_);
    if (query == cudaErrorNotReady) {
        return;
    }
    stabilizerTimingPending_ = false;
    if (query != cudaSuccess) {
        return;
    }
    float elapsedMs = 0.0f;
    if (cudaEventElapsedTime(&elapsedMs, stabilizerTimingStartEvent_,
                             stabilizerTimingStopEvent_) == cudaSuccess) {
        lastStabilizerMs_ = elapsedMs;
    }
    if (!hostStabDiagnostics_) {
        return;
    }
    const float4 diagnostics = hostStabDiagnostics_[0];
    const float4 correction = hostStabDiagnostics_[1];
    const float4 frameMotion = hostStabDiagnostics_[2];
    const float4 tripodDiagnostics = hostStabDiagnostics_[3];
    const float4 bumpHoldDiagnostics = hostStabDiagnostics_[4];
    const int estimatorTag = static_cast<int>(std::lround(diagnostics.w));
    activeStabilizerEngine_ =
        estimatorTag >= 1 && estimatorTag <= 4 ? estimatorTag - 1 : -1;
    const char* engineName = activeStabilizerEngine_ == 3
                                 ? "CUDA fixed reference"
                                 : "Tracking confidence too low";
    const float correctionPixels =
        std::hypot(correction.x, correction.y);
    char status[320]{};
    if (virtualTripodActive_) {
        if (tripodDiagnostics.z >= 3.5f) {
            std::snprintf(
                status, sizeof(status),
                "Virtual Tripod recovered through reference map | %.0f inliers | correction %.1f px",
                static_cast<double>(diagnostics.x),
                static_cast<double>(correctionPixels));
        } else if (tripodDiagnostics.z >= 2.5f) {
            std::snprintf(
                status, sizeof(status),
                "Virtual Tripod crop limit reached | %.0f inliers | correction %.1f px",
                static_cast<double>(diagnostics.x),
                static_cast<double>(correctionPixels));
        } else if (tripodDiagnostics.z >= 1.5f) {
            std::snprintf(
                status, sizeof(status),
                "Virtual Tripod recovering from a brief reference loss | %.0f rejected frames",
                static_cast<double>(tripodDiagnostics.y));
        } else {
            std::snprintf(
                status, sizeof(status),
                "Virtual Tripod locked | %.0f inliers | correction %.1f px, %.2f deg",
                static_cast<double>(diagnostics.x),
                static_cast<double>(correctionPixels),
                static_cast<double>(correction.z * 57.2957795f));
        }
    } else {
        std::snprintf(
            status, sizeof(status),
            "%s | %.0f inliers | correction %.1f px, %.2f deg",
            engineName, static_cast<double>(diagnostics.x),
            static_cast<double>(correctionPixels),
            static_cast<double>(correction.z * 57.2957795f));
    }
    if (bumpHoldActive_ && bumpHoldDiagnostics.w >= 0.5f) {
        const int bumpMode =
            static_cast<int>(std::lround(bumpHoldDiagnostics.x));
        if (bumpHoldDiagnostics.w < 1.5f) {
            std::snprintf(
                status, sizeof(status),
                "Extra Stable: preparing a sharp, settled frame | sharpness %.0f%%",
                static_cast<double>(bumpHoldDiagnostics.y * 100.0f));
        } else if (bumpMode == 1) {
            std::snprintf(
                status, sizeof(status),
                "Extra Stable: showing the last sharp view | sharpness %.0f%% | motion step %.1f px",
                static_cast<double>(bumpHoldDiagnostics.y * 100.0f),
                static_cast<double>(bumpHoldDiagnostics.z));
        } else if (bumpMode == 2) {
            std::snprintf(
                status, sizeof(status),
                "Extra Stable: returning to the live view | sharpness %.0f%%",
                static_cast<double>(bumpHoldDiagnostics.y * 100.0f));
        } else {
            const std::string tripodStatus(status);
            std::snprintf(status, sizeof(status),
                          "Extra Stable armed (live) | %s",
                          tripodStatus.c_str());
        }
    }
    stabilizerStatus_ = status;

    QString diagnosticLine =
        QString(
            "Stabilizer sample | engine %1 | analysis %2x%3 (%4x%5 source "
            "pixels/sample) | inliers %6 | residual %7 px^2 | "
            "motion %8,%9 px %10 deg | correction %11,%12 px %13 deg | "
            "tripod valid/invalid %14/%15 state %16 | %17 ms")
            .arg(engineName)
            .arg(stabSmallWidth_)
            .arg(stabSmallHeight_)
            .arg(stabFactorX_)
            .arg(stabFactorY_)
            .arg(diagnostics.x, 0, 'f', 0)
            .arg(diagnostics.z, 0, 'f', 3)
            .arg(frameMotion.x, 0, 'f', 2)
            .arg(frameMotion.y, 0, 'f', 2)
            .arg(frameMotion.z * 57.2957795f, 0, 'f', 3)
            .arg(correction.x, 0, 'f', 2)
            .arg(correction.y, 0, 'f', 2)
            .arg(correction.z * 57.2957795f, 0, 'f', 3)
            .arg(tripodDiagnostics.x, 0, 'f', 0)
            .arg(tripodDiagnostics.y, 0, 'f', 0)
            .arg(tripodDiagnostics.z, 0, 'f', 0)
            .arg(lastStabilizerMs_, 0, 'f', 2);
    if (bumpHoldActive_) {
        diagnosticLine +=
            QString(" | bump mode %1 sharp %2% step %3 px")
                .arg(bumpHoldDiagnostics.x, 0, 'f', 0)
                .arg(bumpHoldDiagnostics.y * 100.0f, 0, 'f', 0)
                .arg(bumpHoldDiagnostics.z, 0, 'f', 2);
    }
    qInfo().noquote() << diagnosticLine;
}

void CudaInteropSurface::ReleaseKeystone() {
    if (deviceKeystoneLuma_ || hostKeystoneLuma_ || keystoneCopyEvent_ != nullptr) {
        // Drains the in-flight snapshot copy (if any) before its buffers vanish.
        if (!SynchronizeStream()) return;
    }
    if (deviceKeystoneLuma_) {
        cudaFree(deviceKeystoneLuma_);
        deviceKeystoneLuma_ = nullptr;
    }
    if (hostKeystoneLuma_) {
        cudaFreeHost(hostKeystoneLuma_);
        hostKeystoneLuma_ = nullptr;
    }
    if (keystoneCopyEvent_ != nullptr) {
        cudaEventDestroy(keystoneCopyEvent_);
        keystoneCopyEvent_ = nullptr;
    }
    keystoneCopyPending_ = false;
    keystoneSmallWidth_ = 0;
    keystoneSmallHeight_ = 0;
    keystoneFactorX_ = 0;
    keystoneFactorY_ = 0;
    keystoneFullWidth_ = 0;
    keystoneFullHeight_ = 0;
    keystoneFrameCounter_ = 0;
    keystoneFramesSinceDetection_ = 0;
    keystoneHistory_.clear();
    keystoneHistoryIndex_ = -1;
    keystoneTrackingPaused_ = false;
    keystoneSingleStepRequested_ = false;
    keystoneSingleStepInFlight_ = false;
}

void CudaInteropSurface::ResetKeystoneCornersToIdentity() {
    const float w = static_cast<float>(keystoneFullWidth_);
    const float h = static_cast<float>(keystoneFullHeight_);
    if (keystoneFullWidth_ == 0 || keystoneFullHeight_ == 0) {
        return;
    }
    keystoneCorners_[0] = make_float2(0.0f, 0.0f);
    keystoneCorners_[1] = make_float2(w - 1.0f, 0.0f);
    keystoneCorners_[2] = make_float2(w - 1.0f, h - 1.0f);
    keystoneCorners_[3] = make_float2(0.0f, h - 1.0f);
}

// Host-side state only: corners snap back to identity and any in-flight
// snapshot result is discarded. The stream-ordered device->host copy itself is
// left to finish harmlessly (its pinned buffer stays allocated), so no stream
// sync is needed here.
void CudaInteropSurface::ResetKeystone() {
    keystoneCopyPending_ = false;
    keystoneFrameCounter_ = 0;
    keystoneFramesSinceDetection_ = 0;
    keystoneTrackingPaused_ = false;
    keystoneSingleStepRequested_ = false;
    keystoneSingleStepInFlight_ = false;
    keystoneHistory_.clear();
    keystoneHistoryIndex_ = -1;
    ResetKeystoneCornersToIdentity();
    RememberKeystoneCorrection();
}

void CudaInteropSurface::RememberKeystoneCorrection() {
    if (keystoneFullWidth_ == 0 || keystoneFullHeight_ == 0) {
        return;
    }

    std::array<float2, 4> correction{};
    for (int i = 0; i < 4; ++i) {
        correction[static_cast<size_t>(i)] = keystoneCorners_[i];
    }

    if (keystoneHistoryIndex_ >= 0 &&
        keystoneHistoryIndex_ < static_cast<int>(keystoneHistory_.size())) {
        float maxDelta = 0.0f;
        const auto& current = keystoneHistory_[static_cast<size_t>(keystoneHistoryIndex_)];
        for (int i = 0; i < 4; ++i) {
            maxDelta = std::max(maxDelta,
                                PointDistance(current[static_cast<size_t>(i)],
                                              correction[static_cast<size_t>(i)]));
        }
        if (maxDelta < kKeystoneIdentityEpsilonPx) {
            return;
        }
    }

    const size_t next = static_cast<size_t>(std::max(keystoneHistoryIndex_ + 1, 0));
    if (next < keystoneHistory_.size()) {
        keystoneHistory_.erase(keystoneHistory_.begin() + static_cast<ptrdiff_t>(next),
                               keystoneHistory_.end());
    }
    keystoneHistory_.push_back(correction);
    if (keystoneHistory_.size() > kKeystoneHistoryLimit) {
        keystoneHistory_.erase(keystoneHistory_.begin());
    }
    keystoneHistoryIndex_ = static_cast<int>(keystoneHistory_.size()) - 1;
}

void CudaInteropSurface::RestoreKeystoneCorrection() {
    if (keystoneHistoryIndex_ < 0 ||
        keystoneHistoryIndex_ >= static_cast<int>(keystoneHistory_.size())) {
        return;
    }
    const auto& correction = keystoneHistory_[static_cast<size_t>(keystoneHistoryIndex_)];
    for (int i = 0; i < 4; ++i) {
        keystoneCorners_[i] = correction[static_cast<size_t>(i)];
    }
    keystoneFramesSinceDetection_ = 0;
}

void CudaInteropSurface::SetKeystoneTrackingPaused(bool paused) {
    if (paused && !keystoneTrackingPaused_) {
        RememberKeystoneCorrection();
    }
    keystoneTrackingPaused_ = paused;
    keystoneSingleStepRequested_ = false;
}

bool CudaInteropSurface::StepKeystoneCorrection(int direction) {
    if (direction == 0 || keystoneFullWidth_ == 0 || keystoneFullHeight_ == 0) {
        return false;
    }
    if (!keystoneTrackingPaused_) {
        RememberKeystoneCorrection();
        keystoneTrackingPaused_ = true;
    }

    if (direction < 0) {
        keystoneSingleStepRequested_ = false;
        if (keystoneHistoryIndex_ <= 0) {
            return false;
        }
        --keystoneHistoryIndex_;
        RestoreKeystoneCorrection();
        return true;
    }

    if (keystoneHistoryIndex_ + 1 < static_cast<int>(keystoneHistory_.size())) {
        ++keystoneHistoryIndex_;
        RestoreKeystoneCorrection();
        return true;
    }
    if (keystoneCopyPending_ || keystoneSingleStepRequested_ || keystoneSingleStepInFlight_) {
        return false;
    }
    keystoneSingleStepRequested_ = true;
    return true;
}

KeystoneTrackingState CudaInteropSurface::GetKeystoneTrackingState() const {
    KeystoneTrackingState state{};
    state.paused = keystoneTrackingPaused_;
    state.canStepBack = keystoneHistoryIndex_ > 0;
    state.stepPending = keystoneSingleStepRequested_ || keystoneSingleStepInFlight_;
    state.canStepForward = keystoneTrackingPaused_ &&
                           (keystoneHistoryIndex_ + 1 < static_cast<int>(keystoneHistory_.size()) ||
                            (!keystoneCopyPending_ && !state.stepPending && keystoneFullWidth_ != 0));
    state.position = keystoneHistoryIndex_ + 1;
    state.count = static_cast<int>(keystoneHistory_.size());
    return state;
}

bool CudaInteropSurface::EnsureAutoContrastBuffers() {
    if (deviceHistogram_ && deviceAutoLevels_) {
        return true;
    }

    ReleaseAutoContrast();
    if (streamFaulted_) return false;

    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceHistogram_),
                                 256 * sizeof(unsigned int)),
                      "cudaMalloc auto-contrast histogram failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceAutoLevels_), sizeof(float2)),
                      "cudaMalloc auto-contrast levels failed");
    autoLevelsValid_ = false;
    return true;
}

void CudaInteropSurface::ReleaseAutoContrast() {
    if (deviceHistogram_ || deviceAutoLevels_) {
        if (!SynchronizeStream()) return;
    }
    if (deviceHistogram_) {
        cudaFree(deviceHistogram_);
        deviceHistogram_ = nullptr;
    }
    if (deviceAutoLevels_) {
        cudaFree(deviceAutoLevels_);
        deviceAutoLevels_ = nullptr;
    }
    autoLevelsValid_ = false;
}

bool CudaInteropSurface::EnsureTextClarityBuffers(unsigned int width, unsigned int height) {
    if (deviceTextLuma_ && textWidth_ == width && textHeight_ == height) {
        return true;
    }

    ReleaseTextClarity();
    if (streamFaulted_) return false;
    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    // Allocate the five float planes and three mask planes as two pitched
    // slabs. Every plane in a slab consequently has exactly the same pitch.
    ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&deviceTextLuma_),
                                      &deviceTextFloatPitch_,
                                      static_cast<size_t>(width) * sizeof(float),
                                      static_cast<size_t>(height) * 5),
                      "cudaMallocPitch text statistics failed");
    auto* floatBase = reinterpret_cast<unsigned char*>(deviceTextLuma_);
    const size_t floatPlaneBytes = deviceTextFloatPitch_ * height;
    deviceTextHorizontal_ = reinterpret_cast<float*>(floatBase + floatPlaneBytes);
    deviceTextMean_ = reinterpret_cast<float*>(floatBase + floatPlaneBytes * 2);
    deviceTextSqHorizontal_ = reinterpret_cast<float*>(floatBase + floatPlaneBytes * 3);
    deviceTextSqMean_ = reinterpret_cast<float*>(floatBase + floatPlaneBytes * 4);

    ThrowIfCudaFailed(cudaMallocPitch(reinterpret_cast<void**>(&deviceTextMaskA_),
                                      &deviceTextMaskPitch_, width,
                                      static_cast<size_t>(height) * 3),
                      "cudaMallocPitch text masks failed");
    const size_t maskPlaneBytes = deviceTextMaskPitch_ * height;
    deviceTextMaskB_ = deviceTextMaskA_ + maskPlaneBytes;
    deviceTextMaskHistory_ = deviceTextMaskA_ + maskPlaneBytes * 2;

    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceClaheHistogram_),
                                 64 * 256 * sizeof(unsigned int)),
                      "cudaMalloc CLAHE histograms failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceClaheMap_),
                                 64 * 256 * sizeof(float)),
                      "cudaMalloc CLAHE maps failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceTextAnalysis_), sizeof(int4)),
                      "cudaMalloc text analysis failed");
    ThrowIfCudaFailed(cudaMalloc(reinterpret_cast<void**>(&deviceFocusStats_), sizeof(float2)),
                      "cudaMalloc focus statistics failed");
    ThrowIfCudaFailed(cudaMallocHost(reinterpret_cast<void**>(&hostFocusStats_), sizeof(float2)),
                      "cudaMallocHost focus statistics failed");
    ThrowIfCudaFailed(cudaEventCreateWithFlags(&focusCopyEvent_, cudaEventDisableTiming),
                      "cudaEventCreate focus statistics failed");

    textWidth_ = width;
    textHeight_ = height;
    focusFrameCounter_ = 0;
    textMaskHistoryValid_ = false;
    focusCopyPending_ = false;
    discardFocusCopy_ = false;
    focusScoreValid_ = false;
    latestFocusScore_ = 0.0f;
    return true;
}

void CudaInteropSurface::ReleaseTextClarity() {
    if (deviceTextLuma_ || deviceTextMaskA_ || deviceClaheHistogram_ ||
        deviceClaheMap_ || deviceTextAnalysis_ || deviceFocusStats_) {
        if (!SynchronizeStream()) return;
    }
    if (focusCopyEvent_) cudaEventDestroy(focusCopyEvent_);
    if (hostFocusStats_) cudaFreeHost(hostFocusStats_);
    if (deviceFocusStats_) cudaFree(deviceFocusStats_);
    if (deviceTextAnalysis_) cudaFree(deviceTextAnalysis_);
    if (deviceClaheMap_) cudaFree(deviceClaheMap_);
    if (deviceClaheHistogram_) cudaFree(deviceClaheHistogram_);
    if (deviceTextMaskA_) cudaFree(deviceTextMaskA_);
    if (deviceTextLuma_) cudaFree(deviceTextLuma_);
    deviceTextLuma_ = nullptr;
    deviceTextHorizontal_ = nullptr;
    deviceTextMean_ = nullptr;
    deviceTextSqHorizontal_ = nullptr;
    deviceTextSqMean_ = nullptr;
    deviceTextMaskA_ = nullptr;
    deviceTextMaskB_ = nullptr;
    deviceTextMaskHistory_ = nullptr;
    deviceClaheHistogram_ = nullptr;
    deviceClaheMap_ = nullptr;
    deviceTextAnalysis_ = nullptr;
    deviceFocusStats_ = nullptr;
    hostFocusStats_ = nullptr;
    focusCopyEvent_ = nullptr;
    deviceTextFloatPitch_ = 0;
    deviceTextMaskPitch_ = 0;
    textWidth_ = 0;
    textHeight_ = 0;
    focusFrameCounter_ = 0;
    textMaskHistoryValid_ = false;
    focusCopyPending_ = false;
    discardFocusCopy_ = false;
    focusScoreValid_ = false;
    latestFocusScore_ = 0.0f;
}

void CudaInteropSurface::ResetTextClarityHistory() {
    textMaskHistoryValid_ = false;
}

// Folds a finished small-luma snapshot into the smoothed corner state. Runs on
// the host, on a frame captured a few frames ago — never blocks the stream.
void CudaInteropSurface::ConsumeKeystoneDetection() {
    const KeystoneQuadDetection detection =
        DetectProjectedQuad(hostKeystoneLuma_,
                            static_cast<int>(keystoneSmallWidth_),
                            static_cast<int>(keystoneSmallHeight_));
    if (!detection.valid) {
        // Keep the previous corners (and thus the previous homography).
        return;
    }

    for (int i = 0; i < 4; ++i) {
        // Small-image cell centers -> full-resolution pixels.
        const float2 full = make_float2(
            (detection.corners[i].x + 0.5f) * static_cast<float>(keystoneFactorX_),
            (detection.corners[i].y + 0.5f) * static_cast<float>(keystoneFactorY_));
        keystoneCorners_[i] = LerpPoint(keystoneCorners_[i], full, kKeystoneCornerLerp);
    }
    keystoneFramesSinceDetection_ = 0;
    RememberKeystoneCorrection();
}

// Keystone stage body. Ordering guarantees the render loop never stalls:
//  - the snapshot copy is enqueued on stream_ and only *queried* (cudaEventQuery)
//    on later frames, never waited on;
//  - detection runs on the pinned host copy;
//  - the per-frame homography upload is 9 floats of kernel arguments.
void CudaInteropSurface::RunKeystoneStage(uchar4*& current, uchar4*& alternate,
                                          size_t& currentPitch, size_t& alternatePitch) {
    ++keystoneFrameCounter_;

    if (keystoneCopyPending_) {
        const cudaError_t status = cudaEventQuery(keystoneCopyEvent_);
        if (status == cudaSuccess) {
            keystoneCopyPending_ = false;
            if (!keystoneTrackingPaused_ || keystoneSingleStepInFlight_) {
                ConsumeKeystoneDetection();
            }
            keystoneSingleStepInFlight_ = false;
        } else if (status != cudaErrorNotReady) {
            ThrowIfCudaFailed(status, "cudaEventQuery keystone snapshot failed");
        }
    }

    const bool periodicCapture = !keystoneTrackingPaused_ &&
                                 (keystoneFrameCounter_ % kKeystoneSnapshotPeriod) == 0;
    if (!keystoneCopyPending_ && (periodicCapture || keystoneSingleStepRequested_)) {
        // Snapshot the *stabilized* frame so detection sees the same image the
        // warp will run on.
        LaunchStabilizationLumaDownsample(deviceKeystoneLuma_,
                                          static_cast<int>(keystoneSmallWidth_),
                                          static_cast<int>(keystoneSmallHeight_),
                                          current, currentPitch,
                                          static_cast<int>(keystoneFullWidth_),
                                          static_cast<int>(keystoneFullHeight_),
                                          static_cast<int>(keystoneFactorX_),
                                          static_cast<int>(keystoneFactorY_),
                                          stream_);
        const size_t lumaBytes =
            static_cast<size_t>(keystoneSmallWidth_) * keystoneSmallHeight_ * sizeof(float);
        ThrowIfCudaFailed(cudaMemcpyAsync(hostKeystoneLuma_, deviceKeystoneLuma_, lumaBytes,
                                          cudaMemcpyDeviceToHost, stream_),
                          "cudaMemcpyAsync keystone snapshot failed");
        ThrowIfCudaFailed(cudaEventRecord(keystoneCopyEvent_, stream_),
                          "cudaEventRecord keystone snapshot failed");
        keystoneCopyPending_ = true;
        keystoneSingleStepInFlight_ = keystoneSingleStepRequested_;
        keystoneSingleStepRequested_ = false;
    }

    if (!keystoneTrackingPaused_) {
        ++keystoneFramesSinceDetection_;
    }
    if (!keystoneTrackingPaused_ && keystoneFramesSinceDetection_ > kKeystoneStaleFrames) {
        // Detection has been failing (scene changed, lights on, ...): ease the
        // warp back to identity instead of holding a stale perspective.
        const float w = static_cast<float>(keystoneFullWidth_);
        const float h = static_cast<float>(keystoneFullHeight_);
        const float2 identity[4] = {make_float2(0.0f, 0.0f), make_float2(w - 1.0f, 0.0f),
                                    make_float2(w - 1.0f, h - 1.0f), make_float2(0.0f, h - 1.0f)};
        for (int i = 0; i < 4; ++i) {
            keystoneCorners_[i] =
                LerpPoint(keystoneCorners_[i], identity[i], kKeystoneIdentityReturnLerp);
        }
    }

    // Skip the warp entirely while the corners sit on the identity rectangle
    // (startup, or eased back) — no kernel launch for a no-op stage.
    const float w = static_cast<float>(keystoneFullWidth_);
    const float h = static_cast<float>(keystoneFullHeight_);
    const float2 identity[4] = {make_float2(0.0f, 0.0f), make_float2(w - 1.0f, 0.0f),
                                make_float2(w - 1.0f, h - 1.0f), make_float2(0.0f, h - 1.0f)};
    float maxDeviation = 0.0f;
    for (int i = 0; i < 4; ++i) {
        maxDeviation = std::max(maxDeviation, PointDistance(keystoneCorners_[i], identity[i]));
    }
    if (maxDeviation < kKeystoneIdentityEpsilonPx) {
        return;
    }

    float homography[9];
    if (!SolveRectToQuadHomography(w, h, keystoneCorners_, homography)) {
        return;  // degenerate corner state; leave the frame unwarped
    }

    LaunchKeystoneWarp(alternate, alternatePitch,
                       current, currentPitch,
                       static_cast<int>(keystoneFullWidth_),
                       static_cast<int>(keystoneFullHeight_),
                       homography,
                       stream_);
    std::swap(current, alternate);
    std::swap(currentPitch, alternatePitch);
}

bool CudaInteropSurface::EnsureGaussianKernel(int radius, float sigma) {
    const int clampedRadius = std::min(std::max(radius, 1), kMaxCudaBlurRadius);
    const float clampedSigma = std::max(sigma, 0.001f);

    if (kernelUploaded_ && cachedKernelRadius_ == clampedRadius &&
        std::abs(cachedKernelSigma_ - clampedSigma) < 1e-4f) {
        return true;
    }

    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    if (!UploadGaussianKernel(clampedRadius, clampedSigma, stream_)) {
        kernelUploaded_ = false;
        return false;
    }

    cachedKernelRadius_ = clampedRadius;
    cachedKernelSigma_ = clampedSigma;
    kernelUploaded_ = true;
    return true;
}

bool CudaInteropSurface::EnsureDisplayColorLut(const std::uint32_t* lut,
                                                std::uint64_t generation) {
    if (!lut || generation == 0) {
        return false;
    }
    if (cachedDisplayColorLutGeneration_ == generation) {
        return true;
    }
    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }
    if (!UploadDisplayColorLut(lut, stream_)) {
        return false;
    }
    cachedDisplayColorLutGeneration_ = generation;
    qInfo() << "Display color LUT uploaded for generation" << generation;
    return true;
}

void CudaInteropSurface::RunGradientDemoKernel(unsigned int width, unsigned int height, float timeSeconds) {
    if (!valid_) {
        return;
    }

    if (cudaDeviceId_ >= 0) {
        ThrowIfCudaFailed(cudaSetDevice(cudaDeviceId_), "cudaSetDevice failed");
    }

    const unsigned int targetWidth = (width == 0) ? width_ : width;
    const unsigned int targetHeight = (height == 0) ? height_ : height;

    LaunchGradientKernel(surfaceObject_, static_cast<int>(targetWidth), static_cast<int>(targetHeight), timeSeconds);
    ThrowIfCudaFailed(cudaGetLastError(), "Gradient kernel launch failed");
    if (!SynchronizeStream()) return;
}

bool CudaInteropSurface::PollCaptureCopy() {
    if (streamFaulted_) return false;
    if (!d3d11Interop_ || !d3d11Interop_->copyLease.Pending()) return true;
    cudaError_t status = cudaDeviceId_ >= 0 ? cudaSetDevice(cudaDeviceId_) : cudaSuccess;
    if (status == cudaSuccess)
        status = d3d11Interop_->copyEventRecorded ?
            cudaEventQuery(d3d11Interop_->copyComplete) : cudaErrorUnknown;
    const auto completion = status == cudaSuccess ? GpuCopyCompletion::Complete :
        status == cudaErrorNotReady ? GpuCopyCompletion::Pending : GpuCopyCompletion::Failed;
    const bool complete = d3d11Interop_->copyLease.Poll(completion, GetTickCount64());
    if (d3d11Interop_->copyLease.Failed()) {
        streamFaulted_ = true;
        valid_ = false;
        captureInteropFailed_ = false;
        lastError_ = "Camera GPU copy failed or timed out; GPU resources retained until restart";
    }
    return complete;
}

bool CudaInteropSurface::UploadD3D11Frame(
    const ProcessingInput& input,
    uchar4* destination,
    size_t destinationPitch) {
    if (!PollCaptureCopy()) return false;
    if (!input.d3d11TextureLease || !input.d3d11Texture || !destination || stream_ == nullptr ||
        !d3d12Device_) {
        captureInteropFailed_ = true;
        lastError_ =
            "Camera external-memory interop: missing texture, CUDA "
            "destination, or D3D12 device";
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    input.d3d11Texture->GetDesc(&description);
    if (description.Width < input.width ||
        description.Height < input.height ||
        description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        captureInteropFailed_ = true;
        lastError_ =
            "Camera external-memory interop: expected a BGRA8 texture "
            "matching the camera frame";
        return false;
    }
    if (input.d3d11Subresource != 0) {
        captureInteropFailed_ = true;
        lastError_ =
            "Camera external-memory interop: shared conversion texture must "
            "use subresource zero";
        return false;
    }

    HANDLE d3d11SharedHandle = nullptr;
    HANDLE d3d12SharedHandle = nullptr;
    const auto closeSharedHandles = [&]() {
        if (d3d12SharedHandle) {
            CloseHandle(d3d12SharedHandle);
            d3d12SharedHandle = nullptr;
        }
        if (d3d11SharedHandle) {
            CloseHandle(d3d11SharedHandle);
            d3d11SharedHandle = nullptr;
        }
    };

    try {
        if (cudaDeviceId_ >= 0) {
            ThrowIfCudaFailed(
                cudaSetDevice(cudaDeviceId_),
                "Camera external-memory cudaSetDevice failed");
        }
        if (!d3d11Interop_ ||
            d3d11Interop_->identity != input.d3d11Texture) {
            ResetCaptureInterop();
            d3d11Interop_ = std::make_unique<D3D11InteropState>();
            d3d11Interop_->texture = input.d3d11Texture;
            d3d11Interop_->identity = input.d3d11Texture;
            d3d11Interop_->subresource = input.d3d11Subresource;

            Microsoft::WRL::ComPtr<IDXGIResource1> dxgiResource;
            ThrowIfFailed(
                input.d3d11Texture->QueryInterface(
                    IID_PPV_ARGS(dxgiResource.GetAddressOf())),
                "Camera shared texture does not expose IDXGIResource1");
            ThrowIfFailed(
                dxgiResource->CreateSharedHandle(
                    nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr,
                    &d3d11SharedHandle),
                "Failed to create the D3D11 camera NT shared handle");
            ThrowIfFailed(
                d3d12Device_->OpenSharedHandle(
                    d3d11SharedHandle,
                    IID_PPV_ARGS(
                        d3d11Interop_->d3d12Resource.GetAddressOf())),
                "Failed to open the camera texture on D3D12");

            WindowsSecurityAttributes securityAttributes;
            ThrowIfFailed(
                d3d12Device_->CreateSharedHandle(
                    d3d11Interop_->d3d12Resource.Get(),
                    securityAttributes.get(),
                    GENERIC_ALL,
                    nullptr,
                    &d3d12SharedHandle),
                "Failed to create the D3D12 camera shared handle");

            const D3D12_RESOURCE_DESC resourceDescription =
                d3d11Interop_->d3d12Resource->GetDesc();
            if (resourceDescription.Dimension !=
                    D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                resourceDescription.Width != description.Width ||
                resourceDescription.Height != description.Height ||
                resourceDescription.Format != description.Format) {
                throw std::runtime_error(
                    "D3D12 opened camera texture does not match its D3D11 "
                    "allocation");
            }
            const D3D12_RESOURCE_ALLOCATION_INFO allocationInfo =
                d3d12Device_->GetResourceAllocationInfo(
                    0, 1, &resourceDescription);
            if (allocationInfo.SizeInBytes == 0 ||
                allocationInfo.SizeInBytes == UINT64_MAX) {
                throw std::runtime_error(
                    "D3D12 could not report the camera allocation size");
            }

            cudaExternalMemoryHandleDesc memoryDescription{};
            memoryDescription.type =
                cudaExternalMemoryHandleTypeD3D12Resource;
            memoryDescription.handle.win32.handle = d3d12SharedHandle;
            memoryDescription.size = allocationInfo.SizeInBytes;
            memoryDescription.flags = cudaExternalMemoryDedicated;
            ThrowIfCudaFailed(
                cudaImportExternalMemory(
                    &d3d11Interop_->externalMemory,
                    &memoryDescription),
                "CUDA camera external-memory import failed");

            cudaExternalMemoryMipmappedArrayDesc arrayDescription{};
            arrayDescription.offset = 0;
            arrayDescription.numLevels = 1;
            arrayDescription.extent =
                make_cudaExtent(description.Width, description.Height, 1);
            arrayDescription.formatDesc =
                MakeChannelDescForFormat(description.Format);
            arrayDescription.flags =
                cudaArraySurfaceLoadStore | cudaArrayColorAttachment;
            ThrowIfCudaFailed(
                cudaExternalMemoryGetMappedMipmappedArray(
                    &d3d11Interop_->mipArray,
                    d3d11Interop_->externalMemory,
                    &arrayDescription),
                "CUDA camera external-memory array mapping failed");
            ThrowIfCudaFailed(
                cudaGetMipmappedArrayLevel(
                    &d3d11Interop_->level0Array,
                    d3d11Interop_->mipArray,
                    0),
                "CUDA camera external-memory level query failed");
            closeSharedHandles();
            qInfo() << "Camera texture imported through "
                       "D3D11 -> D3D12 -> CUDA external memory ("
                    << description.Width << "x" << description.Height << ")";
        }

        if (!d3d11Interop_->copyComplete) {
            ThrowIfCudaFailed(cudaEventCreateWithFlags(&d3d11Interop_->copyComplete,
                                                       cudaEventDisableTiming),
                              "Camera copy event creation failed");
        }
        if (!d3d11Interop_->copyLease.Begin(input.d3d11TextureLease, GetTickCount64()))
            return false;
        d3d11Interop_->copyEventRecorded = false;
        ThrowIfCudaFailed(
            cudaMemcpy2DFromArrayAsync(
                destination,
                destinationPitch,
                d3d11Interop_->level0Array,
                0,
                0,
                static_cast<size_t>(input.width) * sizeof(uchar4),
                input.height,
                cudaMemcpyDeviceToDevice,
                stream_),
            "Camera external-memory device copy failed");
        // The lease protects the producer texture while this copy is queued.
        // Later enhancement kernels stay asynchronous; only the copy event is
        // queried before capture attempts another conversion into this slot.
        ThrowIfCudaFailed(cudaEventRecord(d3d11Interop_->copyComplete, stream_),
                          "Camera copy completion event record failed");
        d3d11Interop_->copyEventRecorded = true;
        return true;
    } catch (const std::exception& error) {
        closeSharedHandles();
        const bool copyMayBeInFlight = d3d11Interop_ && d3d11Interop_->copyLease.Pending();
        captureInteropFailed_ = !copyMayBeInFlight;
        if (copyMayBeInFlight) {
            d3d11Interop_->copyLease.Poll(GpuCopyCompletion::Failed, GetTickCount64());
            streamFaulted_ = true;
            valid_ = false;
        }
        lastError_ =
            std::string("Camera external-memory interop: ") + error.what();
        qWarning() << QString::fromStdString(lastError_);
        ResetCaptureInterop();
        // Import and device-copy failures are handled by the capture fallback
        // ladder. Clear CUDA's per-thread error slot so this frame can retry
        // through the pinned host-copy rung.
        (void)cudaGetLastError();
        return false;
    }
}

bool CudaInteropSurface::ProcessFrame(const ProcessingInput& input,
                                      const ProcessingSettings& requestedSettings,
                                      const FenceSyncParams& fenceSync) {
    ProcessingSettings settings = requestedSettings;
    settings.ApplyTextClarityMaster();
    if (lastTextClarityMasterEnabled_ != settings.enableAutoTextClarity) {
        lastTextClarityMasterEnabled_ = settings.enableAutoTextClarity;
        ResetTemporalHistory(); // Text-colored pixels cannot linger after a master edge.
        ResetTextClarityHistory();
        focusScoreValid_ = false;
        focusFrameCounter_ = 0;
        // Keep the pending slot owned until its event completes, then discard
        // its old measurement; never reuse host/device storage while busy.
        discardFocusCopy_ = focusCopyPending_;
    }
    if (!settings.enableAutoTextClarity) focusScoreValid_ = false;
    const bool deviceResidentInput = input.d3d11Texture != nullptr;
    captureInteropFailed_ = false;
    if (!valid_ || (!deviceResidentInput && !input.hostPixels) ||
        input.width == 0 || input.height == 0) {
        return false;
    }

    // ---- Defensive input validation (dims/strides come from camera drivers).
    const int format = input.inputFormat;
    if (format < 0 || format > 2) {
        if (!gWarnedInvalidInput) {
            qWarning() << "ProcessFrame aborted: unknown inputFormat" << format;
            gWarnedInvalidInput = true;
        }
        return false;
    }
    if (input.width > kMaxInputExtent || input.height > kMaxInputExtent) {
        if (!gWarnedInvalidInput) {
            qWarning() << "ProcessFrame aborted: implausible input dims"
                       << input.width << "x" << input.height;
            gWarnedInvalidInput = true;
        }
        return false;
    }

    size_t minStride = 0;
    switch (format) {
    case 1:  // NV12: hostPixels = Y plane, hostPlane2 = interleaved UV plane
        minStride = static_cast<size_t>(input.width);
        if (!deviceResidentInput &&
            (input.hostPlane2 == nullptr ||
            static_cast<size_t>(input.hostPlane2StrideBytes) <
                (static_cast<size_t>(input.width) + 1) / 2 * 2)) {
            if (!gWarnedInvalidInput) {
                qWarning() << "ProcessFrame aborted: invalid NV12 UV plane";
                gWarnedInvalidInput = true;
            }
            return false;
        }
        break;
    case 2:  // YUY2: 4 bytes per pixel pair
        minStride = (static_cast<size_t>(input.width) + 1) / 2 * 4;
        break;
    default:  // BGRA8
        minStride = static_cast<size_t>(input.width) * 4;
        if (input.pixelSizeBytes == 0) {
            qWarning() << "ProcessFrame aborted: invalid pixel size";
            return false;
        }
        break;
    }
    if (!deviceResidentInput &&
        static_cast<size_t>(input.hostStrideBytes) < minStride) {
        if (!gWarnedInvalidInput) {
            qWarning() << "ProcessFrame aborted: stride" << input.hostStrideBytes
                       << "smaller than row size" << static_cast<unsigned long long>(minStride);
            gWarnedInvalidInput = true;
        }
        return false;
    }

    // CPU BGRA inputs arrive pre-rotated. Retained D3D11 BGRA textures have
    // not crossed the CPU conversion path, so they use the same GPU rotation
    // stage as raw NV12/YUY2 inputs.
    int turns = 0;
    if (format == 0 && !deviceResidentInput) {
        if (input.rotationQuarterTurns != 0 && !gWarnedBgraRotationIgnored) {
            qWarning() << "ProcessFrame: rotationQuarterTurns ignored for BGRA input"
                          " (CPU path rotates before upload)";
            gWarnedBgraRotationIgnored = true;
        }
    } else {
        turns = input.rotationQuarterTurns & 3;
    }

    // input.width/height describe the host layout (pre-rotation). All device
    // stages and the interop surface run at the post-rotation extent.
    const unsigned int procWidth = ((turns & 1) != 0) ? input.height : input.width;
    const unsigned int procHeight = ((turns & 1) != 0) ? input.width : input.height;
    if (procWidth != width_ || procHeight != height_) {
        return false;
    }

    const size_t pixelSize = static_cast<size_t>(input.pixelSizeBytes);

    CudaBufferFormat resolvedFormat = settings.stagingFormat;
    if (resolvedFormat == CudaBufferFormat::kRgba16F) {
        resolvedFormat = CudaBufferFormat::kRgba8;
    }

    try {
        if (fenceSync.enable && externalSemaphore_ != nullptr) {
            cudaExternalSemaphoreWaitParams waitParams{};
            waitParams.params.fence.value = fenceSync.waitValue;
            waitParams.flags = 0;
            ThrowIfCudaFailed(cudaWaitExternalSemaphoresAsync(&externalSemaphore_, &waitParams, 1, stream_),
                              "cudaWaitExternalSemaphoresAsync failed");
        }

        // P8 GPU timing (plan 11 Wave 1): bracket the kernel chain with a
        // cudaEvent pair on every 30th frame. The start event is enqueued
        // after the fence wait so the measurement covers upload + kernels,
        // not time spent waiting for the graphics queue. Results are polled
        // by ConsumeProcessTiming() on later frames; nothing here blocks.
        ConsumeProcessTiming();
        bool sampleGpuTiming = false;
        if (!processTimingPending_ && ++processTimingFrameCounter_ >= 30u) {
            processTimingFrameCounter_ = 0;
            if (!processTimingStartEvent_) {
                ThrowIfCudaFailed(cudaEventCreate(&processTimingStartEvent_),
                                  "cudaEventCreate frame timing start failed");
                ThrowIfCudaFailed(cudaEventCreate(&processTimingInputEvent_),
                                  "cudaEventCreate input timing boundary failed");
                ThrowIfCudaFailed(cudaEventCreate(&processTimingGeometryEvent_),
                                  "cudaEventCreate geometry timing boundary failed");
                ThrowIfCudaFailed(cudaEventCreate(&processTimingEffectsEvent_),
                                  "cudaEventCreate effects timing boundary failed");
                ThrowIfCudaFailed(cudaEventCreate(&processTimingStopEvent_),
                                  "cudaEventCreate frame timing stop failed");
            }
            ThrowIfCudaFailed(cudaEventRecord(processTimingStartEvent_, stream_),
                              "cudaEventRecord frame timing start failed");
            sampleGpuTiming = true;
        }

        if (!EnsureDeviceBuffers(procWidth, procHeight, resolvedFormat)) {
            return false;
        }
        if (settings.displayColorTransform == DisplayColorTransform::kLumaLut &&
            !EnsureDisplayColorLut(settings.displayColorLut,
                                   settings.displayColorLutGeneration)) {
            lastError_ = "Could not upload display-color LUT";
            return false;
        }

        PinnedUploadSlot* uploadSlot = nullptr;
        auto acquireUploadSlot = [&](size_t requiredBytes) -> PinnedUploadSlot* {
            if (!EnsurePinnedUploadRing(requiredBytes)) {
                throw std::runtime_error("Could not allocate pinned upload staging ring");
            }
            for (size_t offset = 0; offset < pinnedUploadSlots_.size(); ++offset) {
                const size_t index =
                    (pinnedUploadNextSlot_ + offset) % pinnedUploadSlots_.size();
                PinnedUploadSlot& slot = pinnedUploadSlots_[index];
                if (slot.uploadPending) {
                    const cudaError_t queryStatus =
                        cudaEventQuery(slot.uploadComplete);
                    if (queryStatus == cudaErrorNotReady) {
                        continue;
                    }
                    ThrowIfCudaFailed(queryStatus,
                                      "cudaEventQuery upload staging slot failed");
                    slot.uploadPending = false;
                }
                pinnedUploadNextSlot_ =
                    (index + 1) % pinnedUploadSlots_.size();
                uploadSlot = &slot;
                return &slot;
            }
            lastError_ =
                "CUDA upload staging ring busy; skipped one camera frame";
            return nullptr;
        };
        auto copyRows = [](unsigned char* destination, size_t destinationStride,
                           const void* source, size_t sourceStride,
                           size_t rowBytes, size_t rows) {
            const auto* sourceBytes = static_cast<const unsigned char*>(source);
            for (size_t row = 0; row < rows; ++row) {
                std::memcpy(destination + row * destinationStride,
                            sourceBytes + row * sourceStride,
                            rowBytes);
            }
        };

        if (deviceResidentInput) {
            uchar4* uploadTarget = deviceBufferA_;
            size_t uploadPitch = devicePitchA_;
            if (turns != 0) {
                if (!EnsurePreRotateBuffer(input.width, input.height)) {
                    return false;
                }
                uploadTarget = devicePreRotate_;
                uploadPitch = devicePitchPreRotate_;
            }
            if (!UploadD3D11Frame(input, uploadTarget, uploadPitch)) {
                return false;
            }
            if (turns != 0) {
                LaunchRotateQuarterLinear(
                    deviceBufferA_, devicePitchA_,
                    devicePreRotate_, devicePitchPreRotate_,
                    static_cast<int>(input.width),
                    static_cast<int>(input.height),
                    turns, stream_);
            }
        } else if (format == 0) {
            // BGRA path: identical to the historical behavior.
            if (pixelSize != sizeof(uchar4)) {
                if (!gWarnedFp16Unsupported) {
                    qWarning() << "ProcessFrame RGBA16F upload requested but GPU kernel path expects RGBA8; using RGBA8 upload";
                    gWarnedFp16Unsupported = true;
                }
            }

            const size_t rowBytes = static_cast<size_t>(input.width) * sizeof(uchar4);
            PinnedUploadSlot* slot =
                acquireUploadSlot(rowBytes * input.height);
            if (!slot) {
                return false;
            }
            copyRows(slot->data, rowBytes, input.hostPixels, input.hostStrideBytes,
                     rowBytes, input.height);
            ThrowIfCudaFailed(cudaMemcpy2DAsync(deviceBufferA_, devicePitchA_,
                                                slot->data, rowBytes,
                                                rowBytes, input.height,
                                                cudaMemcpyHostToDevice, stream_),
                              "cudaMemcpy2DAsync host->device failed");
        } else {
            // Raw NV12/YUY2 path: upload the compact planes (1.5-2 B/px instead
            // of 4), convert to BGRA on the GPU, then rotate if requested. With
            // no rotation the converter writes straight into buffer A.
            if (!EnsureRawInputBuffers(input.width, input.height, format)) {
                return false;
            }

            uchar4* convertTarget = deviceBufferA_;
            size_t convertPitch = devicePitchA_;
            if (turns != 0) {
                if (!EnsurePreRotateBuffer(input.width, input.height)) {
                    return false;
                }
                convertTarget = devicePreRotate_;
                convertPitch = devicePitchPreRotate_;
            }

            if (format == 1) {
                const size_t yRowBytes = static_cast<size_t>(input.width);
                const size_t yBytes = yRowBytes * input.height;
                const size_t uvRowBytes = (static_cast<size_t>(input.width) + 1) / 2 * 2;
                const size_t uvRows = (static_cast<size_t>(input.height) + 1) / 2;
                PinnedUploadSlot* slot =
                    acquireUploadSlot(yBytes + uvRowBytes * uvRows);
                if (!slot) {
                    return false;
                }
                copyRows(slot->data, yRowBytes, input.hostPixels, input.hostStrideBytes,
                         yRowBytes, input.height);
                unsigned char* uvStaging = slot->data + yBytes;
                copyRows(uvStaging, uvRowBytes, input.hostPlane2, input.hostPlane2StrideBytes,
                         uvRowBytes, uvRows);
                ThrowIfCudaFailed(cudaMemcpy2DAsync(deviceRawPlane1_, rawPlane1Pitch_,
                                                    slot->data, yRowBytes,
                                                    yRowBytes, input.height,
                                                    cudaMemcpyHostToDevice, stream_),
                                  "cudaMemcpy2DAsync NV12 Y plane failed");
                ThrowIfCudaFailed(cudaMemcpy2DAsync(deviceRawPlane2_, rawPlane2Pitch_,
                                                    uvStaging, uvRowBytes,
                                                    uvRowBytes, uvRows,
                                                    cudaMemcpyHostToDevice, stream_),
                                  "cudaMemcpy2DAsync NV12 UV plane failed");
                ThrowIfCudaFailed(cudaEventRecord(uploadSlot->uploadComplete, stream_),
                                  "cudaEventRecord NV12 upload staging slot failed");
                uploadSlot->uploadPending = true;
                LaunchNv12ToBgraLinear(convertTarget, convertPitch,
                                       deviceRawPlane1_, rawPlane1Pitch_,
                                       deviceRawPlane2_, rawPlane2Pitch_,
                                       static_cast<int>(input.width), static_cast<int>(input.height),
                                       stream_, input.yuvColor);
            } else {
                const size_t rowBytes = (static_cast<size_t>(input.width) + 1) / 2 * 4;
                PinnedUploadSlot* slot =
                    acquireUploadSlot(rowBytes * input.height);
                if (!slot) {
                    return false;
                }
                copyRows(slot->data, rowBytes, input.hostPixels, input.hostStrideBytes,
                         rowBytes, input.height);
                ThrowIfCudaFailed(cudaMemcpy2DAsync(deviceRawPlane1_, rawPlane1Pitch_,
                                                    slot->data, rowBytes,
                                                    rowBytes, input.height,
                                                    cudaMemcpyHostToDevice, stream_),
                                  "cudaMemcpy2DAsync YUY2 failed");
                ThrowIfCudaFailed(cudaEventRecord(uploadSlot->uploadComplete, stream_),
                                  "cudaEventRecord YUY2 upload staging slot failed");
                uploadSlot->uploadPending = true;
                LaunchYuy2ToBgraLinear(convertTarget, convertPitch,
                                       deviceRawPlane1_, rawPlane1Pitch_,
                                       static_cast<int>(input.width), static_cast<int>(input.height),
                                       stream_, input.yuvColor);
            }

            if (turns != 0) {
                LaunchRotateQuarterLinear(deviceBufferA_, devicePitchA_,
                                          devicePreRotate_, devicePitchPreRotate_,
                                          static_cast<int>(input.width), static_cast<int>(input.height),
                                          turns, stream_);
            }
        }

        if (!deviceResidentInput && format == 0) {
            ThrowIfCudaFailed(cudaEventRecord(uploadSlot->uploadComplete, stream_),
                              "cudaEventRecord upload staging slot failed");
            uploadSlot->uploadPending = true;
        }

        if (input.publishOriginalFrame) {
            if (!originalLevel0Array_ ||
                originalWidth_ != procWidth ||
                originalHeight_ != procHeight) {
                lastError_ =
                    "Original recording surface is unavailable or has the wrong dimensions";
                return false;
            }
            ThrowIfCudaFailed(
                cudaMemcpy2DToArrayAsync(
                    originalLevel0Array_, 0, 0,
                    deviceBufferA_, devicePitchA_,
                    static_cast<size_t>(procWidth) * sizeof(uchar4),
                    procHeight,
                    cudaMemcpyDeviceToDevice,
                    stream_),
                "cudaMemcpy2DToArrayAsync for original recording frame failed");
        }

        uchar4* current = deviceBufferA_;
        uchar4* alternate = deviceBufferB_;
        size_t currentPitch = devicePitchA_;
        size_t alternatePitch = devicePitchB_;

        auto swapBuffers = [&]() {
            std::swap(current, alternate);
            std::swap(currentPitch, alternatePitch);
        };
        if (sampleGpuTiming) {
            ThrowIfCudaFailed(
                cudaEventRecord(processTimingInputEvent_, stream_),
                "cudaEventRecord input timing boundary failed");
        }

        // Stabilization runs first so every later stage sees one fixed-reference
        // CUDA geometry. Tracking, recovery, path state, and the affine warp
        // remain device-resident.
        if (settings.enableStabilization) {
            if (!EnsureStabilizationBuffers(procWidth, procHeight)) {
                return false;
            }

            ConsumeStabilizerTiming();
            const bool bumpHoldRequested = settings.enableBumpHold;
            if (bumpHoldRequested != bumpHoldActive_) {
                bumpHoldActive_ = bumpHoldRequested;
                LaunchResetBumpHoldState(deviceBumpHoldState_, stream_);
            }
            bool sampleStabilizerTiming = false;
            if (!stabilizerTimingPending_ &&
                ++stabilizerTimingFrameCounter_ >= 30u) {
                stabilizerTimingFrameCounter_ = 0;
                if (!stabilizerTimingStartEvent_) {
                    ThrowIfCudaFailed(
                        cudaEventCreate(&stabilizerTimingStartEvent_),
                        "cudaEventCreate stabilizer timing start failed");
                    ThrowIfCudaFailed(
                        cudaEventCreate(&stabilizerTimingStopEvent_),
                        "cudaEventCreate stabilizer timing stop failed");
                }
                ThrowIfCudaFailed(
                    cudaEventRecord(stabilizerTimingStartEvent_, stream_),
                    "cudaEventRecord stabilizer timing start failed");
                sampleStabilizerTiming = true;
            }

            LaunchStabilizationLumaDownsample(deviceStabLuma_,
                                              static_cast<int>(stabSmallWidth_), static_cast<int>(stabSmallHeight_),
                                              current, currentPitch,
                                              static_cast<int>(procWidth), static_cast<int>(procHeight),
                                              static_cast<int>(stabFactorX_), static_cast<int>(stabFactorY_),
                                              stream_);

            constexpr float strength = 1.0f;
            // Presentation can magnify the scene without enabling CUDA's
            // legacy zoom stage. Tracking still needs the viewing scale for
            // its visible ROI, matching tolerance, and display-pixel deadband.
            const float zoomAmount =
                settings.EffectiveViewingMagnification();
            const float zoomCropReserve =
                0.5f * (1.0f - 1.0f / zoomAmount);
            const float maxCorrectionFraction =
                std::clamp(std::max(0.06f, zoomCropReserve), 0.06f, 0.45f);
            const float virtualTripodCorrectionFraction =
                maxCorrectionFraction +
                strength * (0.45f - maxCorrectionFraction);
            if (!virtualTripodActive_) {
                virtualTripodActive_ = true;
                LaunchResetBumpHoldState(deviceBumpHoldState_, stream_);
                tripodReferenceValid_ = false;
                tripodReferencePrepared_ = false;
                tripodReferenceBuildFrame_ = 0;
                tripodReferenceAccumulationFrame_ = 0;
                tripodRelockPending_ = true;
                stabPrevValid_ = false;
            }

            if (virtualTripodActive_) {
                const int pyramid1Width =
                    static_cast<int>((stabSmallWidth_ + 1u) / 2u);
                const int pyramid1Height =
                    static_cast<int>((stabSmallHeight_ + 1u) / 2u);
                const int pyramid2Width = (pyramid1Width + 1) / 2;
                const int pyramid2Height = (pyramid1Height + 1) / 2;
                const int tripodPixelCount =
                    static_cast<int>(stabSmallWidth_ * stabSmallHeight_);
                const size_t tripodLevel0Stride =
                    static_cast<size_t>(stabSmallWidth_) * stabSmallHeight_;
                const size_t tripodLevel1Stride =
                    static_cast<size_t>(pyramid1Width) * pyramid1Height;
                const size_t tripodLevel2Stride =
                    static_cast<size_t>(pyramid2Width) * pyramid2Height;
                auto keyframeLevel0 = [&](unsigned int slot) -> float* {
                    return slot == 0u
                               ? deviceStabLumaReference_
                               : deviceTripodRecoveryLuma_ +
                                     (slot - 1u) * tripodLevel0Stride;
                };
                auto keyframeLevel1 = [&](unsigned int slot) -> float* {
                    return slot == 0u
                               ? deviceTripodReferencePyramid1_
                               : deviceTripodRecoveryPyramid1_ +
                                     (slot - 1u) * tripodLevel1Stride;
                };
                auto keyframeLevel2 = [&](unsigned int slot) -> float* {
                    return slot == 0u
                               ? deviceTripodReferencePyramid2_
                               : deviceTripodRecoveryPyramid2_ +
                                     (slot - 1u) * tripodLevel2Stride;
                };
                auto keyframeFeatures =
                    [&](unsigned int slot) -> TripodReferenceFeature* {
                    return slot == 0u
                               ? deviceTripodReferenceFeatures_
                               : deviceTripodRecoveryFeatures_ +
                                     (slot - 1u) *
                                         kMaximumStabilizationPairs;
                };
                auto keyframeFeatureCount =
                    [&](unsigned int slot) -> unsigned int* {
                    return slot == 0u
                               ? deviceTripodReferenceFeatureCount_
                               : deviceTripodRecoveryFeatureCounts_ +
                                     (slot - 1u);
                };
                auto buildKeyframe = [&](unsigned int slot) {
                    LaunchStabilizationLumaPyramid(
                        keyframeLevel0(slot),
                        static_cast<int>(stabSmallWidth_),
                        static_cast<int>(stabSmallHeight_),
                        keyframeLevel1(slot),
                        pyramid1Width, pyramid1Height,
                        keyframeLevel2(slot),
                        pyramid2Width, pyramid2Height, stream_);
                    LaunchPrepareVirtualTripodReference(
                        keyframeLevel0(slot),
                        static_cast<int>(stabSmallWidth_),
                        static_cast<int>(stabSmallHeight_),
                        keyframeLevel1(slot),
                        pyramid1Width, pyramid1Height,
                        keyframeLevel2(slot),
                        pyramid2Width, pyramid2Height,
                        keyframeFeatures(slot),
                        keyframeFeatureCount(slot),
                        kMaximumStabilizationPairs, stream_);
                    if (slot == 0u) {
                        // The primary anchor is immutable until a manual
                        // re-lock, so its wide-range projection profiles are
                        // prepared once and reused after a rejected match.
                        LaunchStabilizationProjections(
                            keyframeLevel0(slot),
                            static_cast<int>(stabSmallWidth_),
                            static_cast<int>(stabSmallHeight_),
                            deviceStabColProjPrev_,
                            deviceStabRowProjPrev_, stream_);
                    }
                };
                auto estimateReferenceMap = [&]() {
                    LaunchStabilizationProjections(
                        deviceStabLuma_,
                        static_cast<int>(stabSmallWidth_),
                        static_cast<int>(stabSmallHeight_),
                        deviceStabColProjCurr_,
                        deviceStabRowProjCurr_, stream_);
                    // This kernel changes only the next LK prediction, and
                    // only after the preceding absolute model was rejected.
                    // LK plus RANSAC still decides whether the camera moved.
                    LaunchVirtualTripodProjectionSeed(
                        deviceStabColProjCurr_,
                        deviceStabRowProjCurr_,
                        deviceStabColProjPrev_,
                        deviceStabRowProjPrev_,
                        static_cast<int>(stabSmallWidth_),
                        static_cast<int>(stabSmallHeight_),
                        static_cast<float>(stabFactorX_),
                        static_cast<float>(stabFactorY_),
                        deviceTripodKeyframeOrigins_,
                        deviceTripodKeyframeValid_, 0u,
                        deviceStabState_, stream_);
                    LaunchStabilizationLumaPyramid(
                        deviceStabLuma_,
                        static_cast<int>(stabSmallWidth_),
                        static_cast<int>(stabSmallHeight_),
                        deviceTripodCurrentPyramid1_,
                        pyramid1Width, pyramid1Height,
                        deviceTripodCurrentPyramid2_,
                        pyramid2Width, pyramid2Height, stream_);
                    const float inlierThreshold =
                        std::max(
                            2.0f,
                            1.0f * static_cast<float>(
                                       std::max(stabFactorX_, stabFactorY_)) /
                                zoomAmount);
                    ThrowIfCudaFailed(
                        cudaMemsetAsync(
                            deviceTripodMatchCandidates_, 0,
                            kTripodKeyframeCount *
                                sizeof(TripodMatchCandidate),
                            stream_),
                        "cudaMemsetAsync tripod match candidates failed");
                    for (unsigned int slot = 0u;
                         slot < kTripodKeyframeCount; ++slot) {
                        LaunchPreparedVirtualTripodKeyframePairs(
                            deviceStabLuma_,
                            static_cast<int>(stabSmallWidth_),
                            static_cast<int>(stabSmallHeight_),
                            deviceTripodCurrentPyramid1_,
                            pyramid1Width, pyramid1Height,
                            deviceTripodCurrentPyramid2_,
                            pyramid2Width, pyramid2Height,
                            keyframeLevel0(slot),
                            keyframeLevel1(slot),
                            keyframeLevel2(slot),
                            keyframeFeatures(slot),
                            keyframeFeatureCount(slot),
                            static_cast<float>(stabFactorX_),
                            static_cast<float>(stabFactorY_),
                            static_cast<int>(procWidth),
                            static_cast<int>(procHeight),
                            deviceStabPairs_, deviceStabPairCount_,
                            kMaximumStabilizationPairs,
                            deviceStabState_,
                            deviceTripodKeyframeOrigins_,
                            deviceTripodKeyframeValid_, slot,
                            deviceTripodMatchCandidates_, stream_);
                        LaunchVirtualTripodMatchCandidate(
                            deviceStabPairs_, deviceStabPairCount_,
                            kMaximumStabilizationPairs,
                            static_cast<int>(procWidth),
                            static_cast<int>(procHeight),
                            inlierThreshold,
                            std::clamp(settings.zoomCenterX, 0.0f, 1.0f),
                            std::clamp(settings.zoomCenterY, 0.0f, 1.0f),
                            zoomAmount, deviceTripodKeyframeOrigins_,
                            deviceTripodKeyframeValid_, slot,
                            deviceTripodMatchCandidates_, stream_);
                    }
                    LaunchSelectVirtualTripodMatch(
                        deviceTripodMatchCandidates_,
                        kTripodKeyframeCount, strength,
                        virtualTripodCorrectionFraction, zoomAmount,
                        static_cast<int>(procWidth),
                        static_cast<int>(procHeight),
                        deviceStabState_, stream_);

                    // A short pairwise bridge prevents a stale fixed correction
                    // from making a brief occlusion worse. It is bounded, never
                    // becomes a keyframe origin, and is cleared by the next
                    // accepted absolute match.
                    if (stabPrevValid_) {
                        LaunchStabilizationFeaturePairs(
                            deviceStabLuma_, deviceStabLumaPrevious_,
                            static_cast<int>(stabSmallWidth_),
                            static_cast<int>(stabSmallHeight_),
                            static_cast<float>(stabFactorX_),
                            static_cast<float>(stabFactorY_),
                            deviceStabPairs_, deviceStabPairCount_,
                            kMaximumStabilizationPairs,
                            deviceStabState_, stream_);
                    } else {
                        ThrowIfCudaFailed(
                            cudaMemsetAsync(
                                deviceStabPairCount_, 0,
                                sizeof(unsigned int), stream_),
                            "cudaMemsetAsync tripod fallback pair count failed");
                    }
                    LaunchVirtualTripodRelativeFallback(
                        deviceStabPairs_, deviceStabPairCount_,
                        kMaximumStabilizationPairs,
                        static_cast<int>(procWidth),
                        static_cast<int>(procHeight),
                        1.5f * static_cast<float>(
                                   std::max(stabFactorX_, stabFactorY_)),
                        virtualTripodCorrectionFraction,
                        deviceStabState_, stream_);

                    ++tripodKeyframeAdmissionFrames_;
                    if (tripodKeyframeAdmissionFrames_ >=
                        kTripodKeyframeAdmissionIntervalFrames) {
                        tripodKeyframeAdmissionFrames_ = 0u;
                        const unsigned int slot =
                            tripodNextRecoverySlot_;
                        LaunchCaptureVirtualTripodKeyframe(
                            deviceStabLuma_, keyframeLevel0(slot),
                            tripodPixelCount, deviceStabState_,
                            deviceTripodKeyframeOrigins_,
                            deviceTripodKeyframeValid_, slot,
                            kTripodKeyframeMinimumValidFrames,
                            stream_);
                        buildKeyframe(slot);
                        tripodNextRecoverySlot_ =
                            slot >= kTripodKeyframeCount - 1u
                                ? 1u
                                : slot + 1u;
                    }
                };

                if (tripodRelockPending_) {
                    LaunchResetBumpHoldState(deviceBumpHoldState_, stream_);
                    LaunchResetVirtualTripodState(
                        deviceStabState_, false, stream_);
                    ThrowIfCudaFailed(
                        cudaMemsetAsync(deviceTripodBestFocusScore_, 0,
                                        sizeof(float), stream_),
                        "cudaMemsetAsync tripod best focus score failed");
                    tripodReferenceValid_ = false;
                    tripodReferencePrepared_ = false;
                    tripodReferenceBuildFrame_ = 0;
                    tripodReferenceAccumulationFrame_ = 0;
                    tripodKeyframeAdmissionFrames_ = 0;
                    tripodNextRecoverySlot_ = 1u;
                    ThrowIfCudaFailed(
                        cudaMemsetAsync(
                            deviceTripodKeyframeValid_, 0,
                            kTripodKeyframeCount *
                                sizeof(unsigned int),
                            stream_),
                        "cudaMemsetAsync tripod keyframe reset failed");
                    ThrowIfCudaFailed(
                        cudaMemsetAsync(
                            deviceTripodMatchCandidates_, 0,
                            kTripodKeyframeCount *
                                sizeof(TripodMatchCandidate),
                            stream_),
                        "cudaMemsetAsync tripod candidates reset failed");
                    tripodRelockPending_ = false;
                    stabilizerStatus_ =
                        "Virtual Tripod selecting sharp reference";
                }

                if (tripodReferenceBuildFrame_ <
                    kTripodFocusSelectionFrames) {
                    LaunchMeasureVirtualTripodFocus(
                        deviceStabLuma_,
                        static_cast<int>(stabSmallWidth_),
                        static_cast<int>(stabSmallHeight_),
                        deviceTripodFocusScore_, stream_);
                    LaunchSelectSharperVirtualTripodReference(
                        deviceStabLuma_, deviceStabLumaReference_,
                        tripodPixelCount, deviceTripodFocusScore_,
                        deviceTripodBestFocusScore_,
                        deviceTripodSelectionFlag_, stream_);
                    ++tripodReferenceBuildFrame_;
                    stabilizerStatus_ =
                        "Virtual Tripod selecting sharp reference (" +
                        std::to_string(tripodReferenceBuildFrame_) + "/" +
                        std::to_string(kTripodFocusSelectionFrames) + ")";
                    if (tripodReferenceBuildFrame_ ==
                        kTripodFocusSelectionFrames) {
                        buildKeyframe(0u);
                        LaunchInitializeVirtualTripodKeyframe(
                            deviceTripodKeyframeOrigins_,
                            deviceTripodKeyframeValid_, 0u, stream_);
                        tripodReferencePrepared_ = true;
                        if constexpr (
                            kTripodReferenceAccumulationFrames > 0u) {
                            LaunchInitializeVirtualTripodAccumulator(
                                deviceStabLumaReference_,
                                deviceTripodReferenceAccumulator_,
                                deviceTripodReferenceSampleCounts_,
                                tripodPixelCount, stream_);
                            stabilizerStatus_ =
                                "Virtual Tripod aligning reference frames";
                        } else {
                            tripodReferenceValid_ = true;
                            stabilizerStatus_ =
                                "Virtual Tripod locked (sharp reference)";
                        }
                    }
                } else if (tripodReferenceAccumulationFrame_ <
                           kTripodReferenceAccumulationFrames) {
                    estimateReferenceMap();
                    LaunchMeasureVirtualTripodFocus(
                        deviceStabLuma_,
                        static_cast<int>(stabSmallWidth_),
                        static_cast<int>(stabSmallHeight_),
                        deviceTripodFocusScore_, stream_);
                    LaunchAccumulateVirtualTripodReference(
                        deviceStabLuma_, deviceStabLumaReference_,
                        static_cast<int>(stabSmallWidth_),
                        static_cast<int>(stabSmallHeight_),
                        static_cast<float>(stabFactorX_),
                        static_cast<float>(stabFactorY_),
                        deviceTripodFocusScore_,
                        deviceTripodBestFocusScore_,
                        deviceStabState_,
                        deviceTripodReferenceAccumulator_,
                        deviceTripodReferenceSampleCounts_, stream_);
                    ++tripodReferenceAccumulationFrame_;
                    if (tripodReferenceAccumulationFrame_ ==
                        kTripodReferenceAccumulationFrames) {
                        LaunchFinalizeVirtualTripodReference(
                            deviceStabLumaReference_,
                            deviceTripodReferenceAccumulator_,
                            deviceTripodReferenceSampleCounts_,
                            tripodPixelCount, stream_);
                        buildKeyframe(0u);
                        LaunchInitializeVirtualTripodKeyframe(
                            deviceTripodKeyframeOrigins_,
                            deviceTripodKeyframeValid_, 0u, stream_);
                        tripodReferenceValid_ = true;
                        tripodReferencePrepared_ = true;
                        stabilizerStatus_ =
                            "Virtual Tripod locked (prepared multi-frame reference)";
                    }
                } else if (tripodReferenceValid_ &&
                           tripodReferencePrepared_) {
                    estimateReferenceMap();
                }
            }

            if (bumpHoldActive_ && tripodReferenceValid_ &&
                tripodReferencePrepared_) {
                // The sharpness gate is measured on the same analysis frame
                // used by the absolute tracker. It stays entirely on the
                // stabilization stream and never stalls for a host readback.
                LaunchMeasureVirtualTripodFocus(
                    deviceStabLuma_, static_cast<int>(stabSmallWidth_),
                    static_cast<int>(stabSmallHeight_),
                    deviceTripodFocusScore_, stream_);
            }

            ThrowIfCudaFailed(
                cudaMemcpyAsync(
                    deviceStabLumaPrevious_, deviceStabLuma_,
                    static_cast<size_t>(stabSmallWidth_) * stabSmallHeight_ *
                        sizeof(float),
                    cudaMemcpyDeviceToDevice, stream_),
                "cudaMemcpyAsync stabilization previous luma failed");
            stabPrevValid_ = true;

            LaunchStabilizationWarp(alternate, alternatePitch,
                                    current, currentPitch,
                                    static_cast<int>(procWidth), static_cast<int>(procHeight),
                                    deviceStabState_, stream_);
            swapBuffers();

            if (bumpHoldActive_) {
                const float analysisScale = static_cast<float>(
                    std::max(stabFactorX_, stabFactorY_));
                LaunchUpdateBumpHoldState(
                    deviceBumpHoldState_, deviceStabState_,
                    deviceTripodFocusScore_, deviceTripodBestFocusScore_,
                    tripodReferenceValid_ && tripodReferencePrepared_,
                    static_cast<int>(procWidth), static_cast<int>(procHeight),
                    kBumpHoldMotionEnterAnalysisPixels * analysisScale,
                    kBumpHoldMotionExitAnalysisPixels * analysisScale,
                    stream_);
                LaunchApplyBumpHold(
                    current, currentPitch, deviceBumpHoldFrame_,
                    deviceBumpHoldPitch_, static_cast<int>(procWidth),
                    static_cast<int>(procHeight), deviceBumpHoldState_, stream_);
            }

            if (sampleStabilizerTiming) {
                const auto* diagnosticsAddress =
                    reinterpret_cast<const char*>(deviceStabState_) +
                    offsetof(StabilizationState, diagnostics);
                ThrowIfCudaFailed(
                    cudaMemcpyAsync(hostStabDiagnostics_, diagnosticsAddress,
                                    sizeof(float4), cudaMemcpyDeviceToHost,
                                    stream_),
                    "cudaMemcpyAsync stabilization diagnostics failed");
                const auto* correctionAddress =
                    reinterpret_cast<const char*>(deviceStabState_) +
                    offsetof(StabilizationState, correction);
                ThrowIfCudaFailed(
                    cudaMemcpyAsync(hostStabDiagnostics_ + 1,
                                    correctionAddress,
                                    sizeof(float4), cudaMemcpyDeviceToHost,
                                    stream_),
                    "cudaMemcpyAsync stabilization correction failed");
                const auto* motionAddress =
                    reinterpret_cast<const char*>(deviceStabState_) +
                    offsetof(StabilizationState, lastFrameMotion);
                ThrowIfCudaFailed(
                    cudaMemcpyAsync(hostStabDiagnostics_ + 2,
                                    motionAddress,
                                    sizeof(float4), cudaMemcpyDeviceToHost,
                                    stream_),
                    "cudaMemcpyAsync stabilization motion failed");
                const auto* tripodDiagnosticsAddress =
                    reinterpret_cast<const char*>(deviceStabState_) +
                    offsetof(StabilizationState, tripodDiagnostics);
                ThrowIfCudaFailed(
                    cudaMemcpyAsync(hostStabDiagnostics_ + 3,
                                    tripodDiagnosticsAddress,
                                    sizeof(float4), cudaMemcpyDeviceToHost,
                                    stream_),
                    "cudaMemcpyAsync virtual tripod diagnostics failed");
                const auto* bumpHoldDiagnosticsAddress =
                    reinterpret_cast<const char*>(deviceBumpHoldState_) +
                    offsetof(BumpHoldState, diagnostics);
                ThrowIfCudaFailed(
                    cudaMemcpyAsync(hostStabDiagnostics_ + 4,
                                    bumpHoldDiagnosticsAddress,
                                    sizeof(float4), cudaMemcpyDeviceToHost,
                                    stream_),
                    "cudaMemcpyAsync bump hold diagnostics failed");
                ThrowIfCudaFailed(
                    cudaEventRecord(stabilizerTimingStopEvent_, stream_),
                    "cudaEventRecord stabilizer timing stop failed");
                stabilizerTimingPending_ = true;
            }
            if (activeStabilizerEngine_ < 0) {
                stabilizerStatus_ = "CUDA fixed-reference stabilizer preparing";
            }
        } else {
            if (stabPrevValid_) {
                ResetStabilization();
            }
            activeStabilizerEngine_ = -1;
            stabilizerStatus_ = "Stabilizer off";
        }

        // Keystone follows stabilization (so the detected quad is jitter-free)
        // and precedes every readability stage, letting the straightened slide
        // fill the frame before black/white, sharpening and auto contrast.
        if (settings.enableKeystone) {
            if (!EnsureKeystoneResources(procWidth, procHeight)) {
                return false;
            }
            RunKeystoneStage(current, alternate, currentPitch, alternatePitch);
        } else if (keystoneFullWidth_ != 0) {
            // Stage is off, so an eased return would be invisible anyway: drop
            // any pending snapshot and re-enable cleanly from identity.
            ResetKeystone();
        }
        if (sampleGpuTiming) {
            ThrowIfCudaFailed(
                cudaEventRecord(processTimingGeometryEvent_, stream_),
                "cudaEventRecord geometry timing boundary failed");
        }

        const bool textMaster = settings.enableAutoTextClarity;
        const bool backgroundActive = settings.enableBackgroundFlatten || textMaster;
        const bool binarizationActive = settings.enableAdaptiveBinarization || textMaster;
        const bool smartSharpenActive = settings.enableSmartSharpen || textMaster;
        const bool hysteresisActive = settings.enableTextHysteresis || textMaster;
        const bool focusActive = settings.enableFocusDetection || textMaster;
        const bool glareActive = settings.enableGlareSuppression || textMaster;
        const bool textPipelineActive = backgroundActive || binarizationActive ||
                                        smartSharpenActive || settings.enableClahe ||
                                        focusActive || glareActive;
        const unsigned char* activeTextMask = nullptr;

        if (textPipelineActive) {
            if (!EnsureTextClarityBuffers(procWidth, procHeight) ||
                !EnsureAutoContrastBuffers()) {
                return false;
            }

            LaunchAutoContrastHistogram(deviceHistogram_, current, currentPitch,
                                        static_cast<int>(procWidth), static_cast<int>(procHeight), stream_);
            LaunchTextSceneAnalysis(deviceHistogram_, static_cast<int>(procWidth * procHeight),
                                    deviceTextAnalysis_, stream_);
            const int localRadius = std::clamp(static_cast<int>(procWidth / 32u), 8, 128);
            LaunchTextLocalStatistics(deviceTextLuma_, deviceTextFloatPitch_,
                                      deviceTextHorizontal_, deviceTextMean_,
                                      deviceTextSqHorizontal_, deviceTextSqMean_,
                                      current, currentPitch,
                                      static_cast<int>(procWidth), static_cast<int>(procHeight),
                                      localRadius, stream_);

            // Focus is reduced on-device every frame but copied as two floats
            // only about twice per second. Event polling never stalls rendering.
            if (focusCopyPending_ && cudaEventQuery(focusCopyEvent_) == cudaSuccess) {
                if (!discardFocusCopy_) {
                    const int sampleWidth = static_cast<int>(procWidth / 4u);
                    const int sampleHeight = static_cast<int>(procHeight / 4u);
                    const float count = static_cast<float>(std::max(sampleWidth * sampleHeight, 1));
                    const float meanLap = hostFocusStats_->x / count;
                    latestFocusScore_ = std::max(hostFocusStats_->y / count - meanLap * meanLap, 0.0f);
                    focusScoreValid_ = true;
                }
                discardFocusCopy_ = false;
                focusCopyPending_ = false;
            }
            if (focusActive && !focusCopyPending_ && (++focusFrameCounter_ % 15u) == 0u) {
                LaunchFocusMetric(deviceTextLuma_, deviceTextFloatPitch_,
                                  static_cast<int>(procWidth), static_cast<int>(procHeight),
                                  deviceFocusStats_, stream_);
                ThrowIfCudaFailed(cudaMemcpyAsync(hostFocusStats_, deviceFocusStats_, sizeof(float2),
                                                   cudaMemcpyDeviceToHost, stream_),
                                  "cudaMemcpyAsync focus statistics failed");
                ThrowIfCudaFailed(cudaEventRecord(focusCopyEvent_, stream_),
                                  "cudaEventRecord focus statistics failed");
                focusCopyPending_ = true;
            }

            if (backgroundActive || glareActive) {
                LaunchBackgroundFlattenLinear(alternate, alternatePitch, current, currentPitch,
                                              deviceTextLuma_, deviceTextMean_, deviceTextFloatPitch_,
                                              static_cast<int>(procWidth), static_cast<int>(procHeight),
                                              std::clamp(settings.backgroundFlattenStrength, 0.0f, 1.0f),
                                              glareActive,
                                              std::clamp(settings.glareSuppressionStrength, 0.0f, 1.0f),
                                              deviceTextAnalysis_,
                                              stream_);
                swapBuffers();
            }

            if (settings.enableClahe) {
                LaunchClaheLinear(alternate, alternatePitch, current, currentPitch,
                                  deviceClaheHistogram_, deviceClaheMap_,
                                  static_cast<int>(procWidth), static_cast<int>(procHeight),
                                  std::clamp(settings.claheClipLimit, 1.0f, 8.0f), stream_);
                swapBuffers();
            }

            if (backgroundActive || glareActive || settings.enableClahe) {
                LaunchTextLocalStatistics(deviceTextLuma_, deviceTextFloatPitch_,
                                          deviceTextHorizontal_, deviceTextMean_,
                                          deviceTextSqHorizontal_, deviceTextSqMean_,
                                          current, currentPitch,
                                          static_cast<int>(procWidth), static_cast<int>(procHeight),
                                          localRadius, stream_);
            }

            if (binarizationActive || settings.enableSelectiveSharpen) {
                LaunchSauvolaMask(deviceTextMaskA_, deviceTextMaskPitch_,
                                  deviceTextLuma_, deviceTextMean_, deviceTextSqMean_,
                                  deviceTextFloatPitch_,
                                  static_cast<int>(procWidth), static_cast<int>(procHeight),
                                  std::clamp(settings.sauvolaStrength, 0.1f, 0.5f),
                                  std::clamp(settings.binarizationSoftness, 0.0f, 0.25f),
                                  std::clamp(settings.textPolarityMode, 0, 2),
                                  deviceTextAnalysis_, stream_);
                activeTextMask = deviceTextMaskA_;
                if (settings.strokeWeight != 0) {
                    LaunchStrokeWeight(deviceTextMaskB_, deviceTextMaskPitch_,
                                       deviceTextMaskA_, deviceTextMaskPitch_,
                                       static_cast<int>(procWidth), static_cast<int>(procHeight),
                                       std::clamp(settings.strokeWeight, -3, 3), stream_);
                    activeTextMask = deviceTextMaskB_;
                }
                if (hysteresisActive) {
                    // Normalize the active mask into A before in-place temporal
                    // hysteresis when morphology produced B.
                    if (activeTextMask == deviceTextMaskB_) {
                        ThrowIfCudaFailed(cudaMemcpy2DAsync(deviceTextMaskA_, deviceTextMaskPitch_,
                                                           deviceTextMaskB_, deviceTextMaskPitch_,
                                                           procWidth, procHeight,
                                                           cudaMemcpyDeviceToDevice, stream_),
                                          "cudaMemcpy2DAsync text mask failed");
                        activeTextMask = deviceTextMaskA_;
                    }
                    LaunchTextMaskHysteresis(deviceTextMaskA_, deviceTextMaskPitch_,
                                             deviceTextMaskHistory_,
                                             static_cast<int>(procWidth), static_cast<int>(procHeight),
                                             std::clamp(settings.textHysteresisStrength, 0.0f, 0.25f),
                                             textMaskHistoryValid_, stream_);
                    textMaskHistoryValid_ = true;
                } else {
                    textMaskHistoryValid_ = false;
                }
            } else {
                textMaskHistoryValid_ = false;
            }

            if (smartSharpenActive) {
                LaunchSmartSharpenLinear(alternate, alternatePitch,
                                         deviceScratch_, devicePitchScratch_,
                                         current, currentPitch,
                                         activeTextMask, deviceTextMaskPitch_,
                                         static_cast<int>(procWidth), static_cast<int>(procHeight),
                                         std::clamp(settings.smartSharpenStrength, 0.0f, 1.0f),
                                         settings.enableSelectiveSharpen || textMaster,
                                         stream_);
                swapBuffers();
            }

            if (binarizationActive && activeTextMask) {
                LaunchTextMaskComposite(alternate, alternatePitch, current, currentPitch,
                                        activeTextMask, deviceTextMaskPitch_,
                                        static_cast<int>(procWidth), static_cast<int>(procHeight),
                                        settings.enableTwoColorText
                                            ? settings.textForegroundBgra : 0xff000000u,
                                        settings.enableTwoColorText
                                            ? settings.textBackgroundBgra : 0xffffffffu,
                                        settings.enableAdaptiveBinarization ? 1 : 2,
                                        deviceTextAnalysis_, stream_);
                swapBuffers();
            }
        } else {
            textMaskHistoryValid_ = false;
        }

        if (settings.enableBlackWhite && !binarizationActive) {
            LaunchBlackWhiteLinear(alternate, alternatePitch, current, currentPitch,
                                   static_cast<int>(procWidth), static_cast<int>(procHeight),
                                   settings.blackWhiteThreshold, stream_);
            swapBuffers();
        }

        const SpatialCacheGeometry spatialGeometry =
            settings.enableSpatialSharpen && !settings.enableZoom
                ? ComputeSpatialCacheGeometry(
                      settings.spatialViewTransform, procWidth, procHeight,
                      settings.spatialViewportWidth, settings.spatialViewportHeight,
                      superResWidth_, superResHeight_)
                : SpatialCacheGeometry{};
        if (settings.enableSpatialSharpen && !spatialGeometry.valid) {
            if (settings.spatialUpscaler == SpatialUpscaler::kNis) {
                LaunchNisLinear(alternate, alternatePitch,
                                current, currentPitch,
                                static_cast<int>(procWidth), static_cast<int>(procHeight),
                                static_cast<int>(procWidth), static_cast<int>(procHeight),
                                settings.spatialSharpness,
                                stream_);
            } else {
                LaunchFsrEasuRcasLinear(alternate, alternatePitch,
                                        deviceScratch_, devicePitchScratch_,
                                        current, currentPitch,
                                        static_cast<int>(procWidth), static_cast<int>(procHeight),
                                        static_cast<int>(procWidth), static_cast<int>(procHeight),
                                        settings.spatialSharpness,
                                        stream_);
            }
            swapBuffers();
        }

        if (settings.enableZoom) {
            LaunchZoomLinear(alternate, alternatePitch, current, currentPitch,
                             static_cast<int>(procWidth), static_cast<int>(procHeight),
                             settings.zoomAmount, settings.zoomCenterX, settings.zoomCenterY, stream_);
            swapBuffers();
        }

        if (settings.enableBlur && settings.blurRadius > 0 && settings.blurSigma > 0.0f) {
            if (!EnsureGaussianKernel(settings.blurRadius, settings.blurSigma)) {
                kernelUploaded_ = false;
                return false;
            }

            LaunchGaussianBlurLinear(alternate, alternatePitch,
                                     deviceScratch_, devicePitchScratch_,
                                     current, currentPitch,
                                     static_cast<int>(procWidth), static_cast<int>(procHeight),
                                     stream_);
            swapBuffers();
        }

        if (settings.enableTemporalSmoothing && resolvedFormat == CudaBufferFormat::kRgba8) {
            if (!EnsureTemporalHistory(procWidth, procHeight)) {
                return false;
            }

            const float alpha = std::clamp(settings.temporalSmoothingAlpha, 0.0f, 1.0f);
            LaunchTemporalSmoothLinear(alternate, alternatePitch,
                                       current, currentPitch,
                                       deviceTemporalHistory_, devicePitchHistory_,
                                       static_cast<int>(procWidth), static_cast<int>(procHeight),
                                       alpha,
                                       temporalHistoryValid_,
                                       stream_);
            temporalHistoryValid_ = true;
            swapBuffers();
        } else {
            temporalHistoryValid_ = false;
        }
        if (sampleGpuTiming) {
            ThrowIfCudaFailed(
                cudaEventRecord(processTimingEffectsEvent_, stream_),
                "cudaEventRecord effects timing boundary failed");
        }

        // Auto-contrast measurement feeds the grade kernel below within the
        // same frame; both the histogram and the smoothed lo/hi levels live in
        // device memory, so there is no readback and no stall. Runs on the
        // post-keystone image so the straightened slide dominates the
        // statistics.
        const bool autoContrastActive =
            settings.enableAutoContrast && settings.autoContrastStrength > 0.0f;
        if (autoContrastActive) {
            if (!EnsureAutoContrastBuffers()) {
                return false;
            }
            LaunchAutoContrastHistogram(deviceHistogram_,
                                        current, currentPitch,
                                        static_cast<int>(procWidth), static_cast<int>(procHeight),
                                        stream_);
            LaunchAutoContrastAnalysis(deviceHistogram_,
                                       static_cast<int>(procWidth * procHeight),
                                       deviceAutoLevels_,
                                       autoLevelsValid_,
                                       stream_);
            autoLevelsValid_ = true;
        } else {
            autoLevelsValid_ = false;
        }

        const DisplayColorTransform gradeColorTransform =
            (binarizationActive && settings.enableTwoColorText)
                ? DisplayColorTransform::kNone
                : settings.displayColorTransform;
        if (gradeColorTransform != DisplayColorTransform::kNone ||
            settings.contrast != 1.0f ||
            settings.brightness != 0.0f ||
            autoContrastActive) {
            const float contrast = std::clamp(settings.contrast, 0.25f, 4.0f);
            const float brightness = std::clamp(settings.brightness, -1.0f, 1.0f);
            // In place: the kernel touches only its own pixel, so no swap needed.
            LaunchDisplayColorGradeLinear(current, currentPitch,
                                          static_cast<int>(procWidth), static_cast<int>(procHeight),
                                          static_cast<int>(gradeColorTransform), contrast, brightness,
                                          autoContrastActive ? deviceAutoLevels_ : nullptr,
                                          std::clamp(settings.autoContrastStrength, 0.0f, 1.0f),
                                          stream_);
        }

        if (settings.drawFocusMarker) {
            LaunchFocusMarkerLinear(current, currentPitch,
                                    static_cast<int>(procWidth), static_cast<int>(procHeight),
                                    settings.zoomCenterX, settings.zoomCenterY, stream_);
        }

        ThrowIfCudaFailed(cudaMemcpy2DToArrayAsync(level0Array_, 0, 0,
                                                   current, currentPitch,
                                                   static_cast<size_t>(procWidth) * sizeof(uchar4), procHeight,
                                                   cudaMemcpyDeviceToDevice, stream_),
                          "cudaMemcpy2DToArrayAsync failed");

        UpdateSuperResCache(current,
                            currentPitch,
                            alternate,
                            alternatePitch,
                            procWidth,
                            procHeight,
                            settings);
        // Only a new camera frame reaches this point. Temporal effects have
        // already completed on the full scene; viewport-only presents reuse
        // the cache or fall back to the scene when panning outside its ROI.
        UpdateSpatialCache(current, currentPitch, alternate, alternatePitch,
                           procWidth, procHeight, spatialGeometry, settings);

        if (sampleGpuTiming) {
            ThrowIfCudaFailed(cudaEventRecord(processTimingStopEvent_, stream_),
                              "cudaEventRecord frame timing stop failed");
            processTimingPending_ = true;
        }

        if (fenceSync.enable && externalSemaphore_ != nullptr) {
            cudaExternalSemaphoreSignalParams signalParams{};
            signalParams.params.fence.value = fenceSync.signalValue;
            signalParams.flags = 0;
            ThrowIfCudaFailed(cudaSignalExternalSemaphoresAsync(&externalSemaphore_, &signalParams, 1, stream_),
                              "cudaSignalExternalSemaphoresAsync failed");
        } else {
            if (!SynchronizeStream()) return false;
        }
        lastError_.clear();
        return true;
    } catch (const std::exception& e) {
        lastError_ = e.what();
        qWarning() << "ProcessFrame exception:" << e.what();
        temporalHistoryValid_ = false;
        stabPrevValid_ = false;
        keystoneCopyPending_ = false;
        autoLevelsValid_ = false;
        return false;
    } catch (...) {
        lastError_ = "Unknown CUDA exception during ProcessFrame";
        qWarning() << "ProcessFrame encountered unknown exception";
        temporalHistoryValid_ = false;
        stabPrevValid_ = false;
        keystoneCopyPending_ = false;
        autoLevelsValid_ = false;
        return false;
    }
}

} // namespace okuflow

#endif // OKUFLOW_HAS_CUDA_EXT_MEMORY

#endif // _WIN32
