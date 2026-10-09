#ifdef _WIN32
#include "okuflow/capture/capture_buffer.hpp"

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
