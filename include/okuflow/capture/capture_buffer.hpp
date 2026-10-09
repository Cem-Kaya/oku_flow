#pragma once

#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <cstdint>
#include <vector>

namespace okuflow {

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
