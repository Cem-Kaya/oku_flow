#pragma once

#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <d3d11.h>
#include <cstdint>
#include <vector>
#include <deque>
#include <cstddef>

namespace okuflow {

struct MediaFrame;
// Caller holds the ingress lock. Retired frames are released after that lock.
// Retain short recording bursts; overflow catches up to the newest arrival.
std::size_t EnqueueCaptureFrame(std::deque<MediaFrame>& pending, MediaFrame&& frame,
                               bool retainBurst, std::size_t burstCapacity,
                               std::deque<MediaFrame>& retired);

bool IsSupportedCaptureLayout(const GUID& subtype, UINT width, UINT height,
                              LONG stride);
struct CaptureAperture {
    UINT x{}, y{}, width{}, height{};
};
// Decoder frame size describes its allocation, which may include codec padding.
// Prefer its explicit display aperture; otherwise use the negotiated phone size.
bool ResolveNv12DisplayAperture(IMFMediaType* type, UINT allocationWidth,
                               UINT allocationHeight, UINT expectedWidth,
                               UINT expectedHeight, CaptureAperture& aperture);
// Packs a mapped GPU subresource, using allocationHeight for the NV12 UV
// origin. Only explicit RGBA textures swap red/blue into MF RGB32 order.
bool CopyMappedCaptureTexture(const D3D11_MAPPED_SUBRESOURCE& mapped,
                              DXGI_FORMAT format, UINT allocationWidth,
                              UINT allocationHeight, UINT width, UINT height,
                              std::vector<std::uint8_t>& output, LONG& stride,
                              GUID& subtype, UINT sourceX = 0, UINT sourceY = 0);

// Copies visible pixels into tightly packed, top-down storage. Actual 2D
// pitch takes precedence over negotiatedStride. Plain buffers use that signed
// negotiated stride; their Lock pointer is the lowest address, not scanline 0.
// NV12 has consecutive Y/UV planes with identical positive pitch. Returns false
// for unsupported, truncated, or invalid layouts; output is unchanged on failure.
bool CopyCaptureBuffer(IMFMediaBuffer* buffer, const GUID& subtype,
                       UINT width, UINT height, LONG negotiatedStride,
                       std::vector<std::uint8_t>& output, LONG& outputStride);

} // namespace okuflow
#endif
