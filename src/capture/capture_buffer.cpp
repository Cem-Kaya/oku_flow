#ifdef _WIN32
#include "okuflow/capture/capture_buffer.hpp"
#include "okuflow/capture/media_capture.hpp"

#include <wrl/client.h>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace okuflow {
namespace {
struct BufferUnlock {
    IMFMediaBuffer* linear{};
    IMF2DBuffer* surface{};
    ~BufferUnlock()
    {
        if (surface) surface->Unlock2D();
        if (linear) linear->Unlock();
    }
};
} // namespace

std::size_t EnqueueCaptureFrame(std::deque<MediaFrame>& pending, MediaFrame&& frame,
                               bool retainBurst, std::size_t burstCapacity,
                               std::deque<MediaFrame>& retired)
{
    std::size_t dropped = 0;
    if (!retainBurst || pending.size() >= burstCapacity) {
        dropped = pending.size();
        while (!pending.empty()) {
            retired.push_back(std::move(pending.front()));
            pending.pop_front();
        }
    }
    pending.push_back(std::move(frame));
    return dropped;
}

bool IsSupportedCaptureLayout(const GUID& subtype, UINT width, UINT height,
                              LONG stride)
{
    const bool nv12 = IsEqualGUID(subtype, MFVideoFormat_NV12);
    const bool yuy2 = IsEqualGUID(subtype, MFVideoFormat_YUY2);
    const bool rgb = IsEqualGUID(subtype, MFVideoFormat_RGB32) ||
                     IsEqualGUID(subtype, MFVideoFormat_ARGB32);
    const std::uint64_t bytes = std::uint64_t(width) * (rgb ? 4 : yuy2 ? 2 : 1);
    const std::uint64_t pitch = stride < 0 ? -std::int64_t(stride) : stride;
    return width && height && (nv12 || yuy2 || rgb) &&
        (!(nv12 || yuy2) || !(width & 1)) && (!nv12 || !(height & 1)) &&
        (rgb || stride > 0) && pitch >= bytes &&
        bytes <= std::numeric_limits<LONG>::max();
}

bool CopyMappedCaptureTexture(const D3D11_MAPPED_SUBRESOURCE& mapped,
                              DXGI_FORMAT format, UINT allocationWidth,
                              UINT allocationHeight, UINT width, UINT height,
                              std::vector<std::uint8_t>& output, LONG& stride,
                              GUID& subtype)
{
    GUID packedSubtype{};
    if (format == DXGI_FORMAT_NV12) packedSubtype = MFVideoFormat_NV12;
    else if (format == DXGI_FORMAT_YUY2) packedSubtype = MFVideoFormat_YUY2;
    else if (format == DXGI_FORMAT_B8G8R8A8_UNORM) packedSubtype = MFVideoFormat_ARGB32;
    else if (format == DXGI_FORMAT_R8G8B8A8_UNORM) packedSubtype = MFVideoFormat_RGB32;
    else return false;
    if (!mapped.pData || width > allocationWidth || height > allocationHeight ||
        mapped.RowPitch > std::numeric_limits<LONG>::max() ||
        !IsSupportedCaptureLayout(packedSubtype, width, height, static_cast<LONG>(mapped.RowPitch))) return false;
    const bool nv12 = format == DXGI_FORMAT_NV12;
    if (nv12 && (allocationHeight & 1)) return false;
    const std::size_t bytes = std::size_t(width) * (nv12 ? 1 : format == DXGI_FORMAT_YUY2 ? 2 : 4);
    const std::size_t rows = std::size_t(height) + (nv12 ? height / 2 : 0);
    if (rows > std::numeric_limits<std::size_t>::max() / bytes) return false;
    const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
    std::vector<std::uint8_t> packed(bytes * rows);
    for (std::size_t row = 0; row < rows; ++row) {
        const std::size_t sourceRow = nv12 && row >= height ? allocationHeight + row - height : row;
        auto* destination = packed.data() + row * bytes;
        std::memcpy(destination, source + sourceRow * mapped.RowPitch, bytes);
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            for (UINT x = 0; x < width; ++x) std::swap(destination[x * 4], destination[x * 4 + 2]);
        }
    }
    output = std::move(packed);
    stride = static_cast<LONG>(bytes);
    subtype = packedSubtype;
    return true;
}

bool CopyCaptureBuffer(IMFMediaBuffer* buffer, const GUID& subtype,
                       UINT width, UINT height, LONG negotiatedStride,
                       std::vector<std::uint8_t>& output, LONG& outputStride)
{
    const bool nv12 = IsEqualGUID(subtype, MFVideoFormat_NV12);
    const bool yuy2 = IsEqualGUID(subtype, MFVideoFormat_YUY2);
    const bool rgb = IsEqualGUID(subtype, MFVideoFormat_RGB32) ||
                     IsEqualGUID(subtype, MFVideoFormat_ARGB32);
    if (!buffer || !width || !height || (!nv12 && !yuy2 && !rgb) ||
        ((nv12 || yuy2) && (width & 1)) || (nv12 && (height & 1))) {
        return false;
    }
    const std::uint64_t rowBytes = std::uint64_t(width) * (rgb ? 4 : yuy2 ? 2 : 1);
    const std::uint64_t rows = std::uint64_t(height) + (nv12 ? height / 2 : 0);
    if (rowBytes > std::numeric_limits<LONG>::max() ||
        rows > std::numeric_limits<DWORD>::max() / rowBytes) {
        return false;
    }

    Microsoft::WRL::ComPtr<IMF2DBuffer2> surface2;
    Microsoft::WRL::ComPtr<IMF2DBuffer> surface;
    BYTE* scanline = nullptr;
    BYTE* start = nullptr;
    DWORD extent = 0;
    LONG pitch = negotiatedStride;
    bool bounded = false;
    BufferUnlock unlock;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(surface2.GetAddressOf())))) {
        if (FAILED(surface2->Lock2DSize(MF2DBuffer_LockFlags_Read,
                                        &scanline, &pitch, &start, &extent))) {
            return false;
        }
        unlock.surface = surface2.Get();
        bounded = true;
    } else if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(surface.GetAddressOf())))) {
        if (FAILED(surface->Lock2D(&scanline, &pitch))) return false;
        unlock.surface = surface.Get();
        // IMF2DBuffer guarantees the surface, but exposes no allocation bounds.
        // GetCurrentLength/GetMaxLength describe its contiguous representation
        // and MUST NOT be used to validate the native surface.
    } else {
        if (FAILED(buffer->Lock(&start, nullptr, &extent))) return false;
        unlock.linear = buffer;
        bounded = true;
        scanline = start;
    }
    const std::uint64_t absPitch = pitch < 0 ? -std::int64_t(pitch) : pitch;
    if (!scanline || !pitch || absPitch < rowBytes || (!rgb && pitch < 0)) {
        return false;
    }
    const std::uint64_t displacement = (rows - 1) * absPitch;
    if (bounded) {
        if (!start || displacement > extent || rowBytes > extent - displacement) {
            return false;
        }
        if (unlock.linear && pitch < 0) {
            scanline = start + static_cast<std::size_t>(displacement);
        }
        const auto begin = reinterpret_cast<std::uintptr_t>(start);
        const auto top = reinterpret_cast<std::uintptr_t>(scanline);
        if (top < begin || top - begin > extent) return false;
        const std::uint64_t offset = top - begin;
        if (pitch < 0) {
            if (offset < displacement || rowBytes > extent - offset) return false;
        } else if (displacement + rowBytes > extent - offset) {
            return false;
        }
    }

    std::vector<std::uint8_t> packed(static_cast<std::size_t>(rowBytes * rows));
    // For NV12, the UV plane starts at pitch * height, not width * height.
    // Copy only visible bytes: native surface padding need not be accessible.
    for (std::uint64_t row = 0; row < rows; ++row) {
        std::memcpy(packed.data() + static_cast<std::size_t>(row * rowBytes),
                    scanline + static_cast<std::ptrdiff_t>(row) * pitch,
                    static_cast<std::size_t>(rowBytes));
    }
    output = std::move(packed);
    outputStride = static_cast<LONG>(rowBytes);
    return true;
}
} // namespace okuflow
#endif
