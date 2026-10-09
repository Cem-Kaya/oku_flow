#pragma once
#ifdef _WIN32
#include "openzoom/common/yuv_color.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <d3d11.h>

namespace openzoom {

// Missing or explicitly unknown values use YuvColorInfo's documented defaults.
// Explicit unsupported matrices/ranges are rejected instead of mislabeled.
inline bool ReadCaptureYuvColor(IMFAttributes* attributes, YuvColorInfo& output)
{
    YuvColorInfo color;
    const UINT32 matrix = MFGetAttributeUINT32(attributes, MF_MT_YUV_MATRIX,
                                               MFVideoTransferMatrix_Unknown);
    switch (matrix) {
    case MFVideoTransferMatrix_Unknown:
    case MFVideoTransferMatrix_BT601: break;
    case MFVideoTransferMatrix_BT709: color.matrix = YuvMatrix::Bt709; break;
    default: return false;
    }
    const UINT32 range = MFGetAttributeUINT32(attributes, MF_MT_VIDEO_NOMINAL_RANGE,
                                              MFNominalRange_Unknown);
    switch (range) {
    case MFNominalRange_Unknown:
    case MFNominalRange_16_235: break;
    case MFNominalRange_0_255: color.range = YuvRange::Full; break;
    default: return false;
    }
    output = color;
    return true;
}

inline D3D11_VIDEO_PROCESSOR_COLOR_SPACE CaptureVideoColorSpace(YuvColorInfo color)
{
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE result{};
    result.YCbCr_Matrix = color.matrix == YuvMatrix::Bt709 ? 1 : 0;
    result.Nominal_Range = color.range == YuvRange::Full
        ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
        : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    return result;
}
} // namespace openzoom
#endif
