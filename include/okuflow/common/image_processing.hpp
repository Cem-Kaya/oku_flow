#pragma once

#ifdef _WIN32

#include <Windows.h>
#include "okuflow/common/yuv_color.hpp"

#include <cstdint>
#include <vector>

namespace okuflow {
namespace processing {

void CopyArgbToBgra(const uint8_t* src,
                    UINT srcStride,
                    UINT width,
                    UINT height,
                    std::vector<uint8_t>& dst);

void CopyRgbxToBgra(const uint8_t* src,
                    UINT srcStride,
                    UINT width,
                    UINT height,
                    std::vector<uint8_t>& dst);

bool ConvertNv12ToBgra(const uint8_t* src,
                       size_t srcSize,
                       UINT strideY,
                       UINT width,
                       UINT height,
                       std::vector<uint8_t>& dst,
                       YuvColorInfo color = {});

bool ConvertYuy2ToBgra(const uint8_t* src,
                       size_t srcSize,
                       UINT stride,
                       UINT width,
                       UINT height,
                       std::vector<uint8_t>& dst,
                       YuvColorInfo color = {});

void ApplyBlackWhite(const std::vector<uint8_t>& src,
                     std::vector<uint8_t>& dst,
                     float threshold);

void ApplyZoom(const std::vector<uint8_t>& src,
               std::vector<uint8_t>& dst,
               UINT width,
               UINT height,
               float zoomAmount,
               float centerXNormalized,
               float centerYNormalized);

void ApplyGaussianBlur(const std::vector<uint8_t>& src,
                       std::vector<uint8_t>& scratch,
                       std::vector<uint8_t>& dst,
                       UINT width,
                       UINT height,
                       int radius,
                       float sigma);

void ApplyTemporalSmoothCpu(std::vector<uint8_t>& frame,
                            std::vector<float>& temporalHistory,
                            bool& historyValid,
                            UINT width,
                            UINT height,
                            float temporalSmoothAlpha,
                            bool temporalSmoothEnabled);

} // namespace processing
} // namespace okuflow

#endif // _WIN32
