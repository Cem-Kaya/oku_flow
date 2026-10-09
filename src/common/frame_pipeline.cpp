#ifdef _WIN32

#include "okuflow/common/frame_pipeline.hpp"

#include "okuflow/common/image_processing.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace okuflow::processing {

namespace {

constexpr UINT kMaxFrameExtent = 16384;

bool PackedFrameIsValid(const std::vector<uint8_t>& frame,
                        std::size_t availableBytes,
                        UINT width,
                        UINT height,
                        UINT stride,
                        std::size_t minimumStride)
{
    if (frame.empty() || width == 0 || height == 0 ||
        width > kMaxFrameExtent || height > kMaxFrameExtent ||
        static_cast<std::size_t>(stride) < minimumStride) {
        return false;
    }
    if (static_cast<std::size_t>(stride) >
        std::numeric_limits<std::size_t>::max() / height) {
        return false;
    }
    return static_cast<std::size_t>(stride) * height <= availableBytes;
}

bool AbsoluteStride(LONG stride, UINT fallback, UINT& result)
{
    if (stride == 0) {
        result = fallback;
        return true;
    }
    const std::int64_t signedStride = stride;
    const std::uint64_t magnitude =
        signedStride < 0
            ? static_cast<std::uint64_t>(-signedStride)
            : static_cast<std::uint64_t>(signedStride);
    if (magnitude > std::numeric_limits<UINT>::max()) {
        return false;
    }
    result = static_cast<UINT>(magnitude);
    return true;
}

} // namespace

bool CpuFramePipeline::ConvertFrameToBgra(const std::vector<uint8_t>& frame,
                                          const GUID& subtype,
                                          UINT width,
                                          UINT height,
                                          LONG stride,
                                          std::size_t dataSize,
                                          YuvColorInfo color)
{
    auto rejectFrame = [this]() {
        stageRaw_.clear();
        rawWidth_ = 0;
        rawHeight_ = 0;
        return false;
    };

    if (width == 0 || height == 0 || width > kMaxFrameExtent ||
        height > kMaxFrameExtent || frame.empty()) {
        return rejectFrame();
    }

    const std::size_t availableBytes =
        dataSize == 0 ? frame.size() : std::min(frame.size(), dataSize);
    UINT effectiveStride = 0;
    if (!AbsoluteStride(stride, width * 4u, effectiveStride)) {
        return rejectFrame();
    }

    if (IsEqualGUID(subtype, MFVideoFormat_ARGB32)) {
        if (!PackedFrameIsValid(frame, availableBytes, width, height,
                                effectiveStride, static_cast<std::size_t>(width) * 4u)) {
            return rejectFrame();
        }
        if (stride < 0) {
            std::vector<uint8_t> topDown(frame.size());
            for (UINT y = 0; y < height; ++y) {
                std::memcpy(
                    topDown.data() + static_cast<std::size_t>(y) * effectiveStride,
                    frame.data() +
                        static_cast<std::size_t>(height - 1 - y) *
                            effectiveStride,
                    effectiveStride);
            }
            CopyArgbToBgra(
                topDown.data(), effectiveStride, width, height, stageRaw_);
        } else {
            CopyArgbToBgra(
                frame.data(), effectiveStride, width, height, stageRaw_);
        }
        rawWidth_ = width;
        rawHeight_ = height;
        return true;
    }

    if (IsEqualGUID(subtype, MFVideoFormat_RGB32)) {
        if (!PackedFrameIsValid(frame, availableBytes, width, height,
                                effectiveStride, static_cast<std::size_t>(width) * 4u)) {
            return rejectFrame();
        }
        if (stride < 0) {
            std::vector<uint8_t> topDown(frame.size());
            for (UINT y = 0; y < height; ++y) {
                std::memcpy(
                    topDown.data() + static_cast<std::size_t>(y) * effectiveStride,
                    frame.data() +
                        static_cast<std::size_t>(height - 1 - y) *
                            effectiveStride,
                    effectiveStride);
            }
            CopyRgbxToBgra(
                topDown.data(), effectiveStride, width, height, stageRaw_);
        } else {
            CopyRgbxToBgra(
                frame.data(), effectiveStride, width, height, stageRaw_);
        }
        rawWidth_ = width;
        rawHeight_ = height;
        return true;
    }

    if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
        UINT nv12Stride = 0;
        if (!AbsoluteStride(stride, width, nv12Stride)) {
            return rejectFrame();
        }
        const bool ok = ConvertNv12ToBgra(frame.data(), availableBytes,
                                          nv12Stride,
                                          width, height, stageRaw_, color);
        if (ok) {
            rawWidth_ = width;
            rawHeight_ = height;
        } else {
            return rejectFrame();
        }
        return ok;
    }

    if (IsEqualGUID(subtype, MFVideoFormat_YUY2)) {
        UINT packedStride = 0;
        if (!AbsoluteStride(stride, width * 2u, packedStride)) {
            return rejectFrame();
        }
        const bool ok = ConvertYuy2ToBgra(frame.data(), availableBytes,
                                          packedStride, width, height, stageRaw_, color);
        if (ok) {
            rawWidth_ = width;
            rawHeight_ = height;
        } else {
            return rejectFrame();
        }
        return ok;
    }

    return rejectFrame();
}

bool CpuFramePipeline::RotateRawBuffer(int quarterTurns, UINT& width, UINT& height)
{
    if (stageRaw_.empty() || width == 0 || height == 0) {
        rotatedStageBuffer_.clear();
        return false;
    }

    int turns = quarterTurns % 4;
    if (turns < 0) {
        turns += 4;
    }

    if (turns == 0) {
        rotatedStageBuffer_.clear();
        return false;
    }

    const UINT srcWidth = width;
    const UINT srcHeight = height;
    const UINT dstWidth = (turns % 2 == 0) ? srcWidth : srcHeight;
    const UINT dstHeight = (turns % 2 == 0) ? srcHeight : srcWidth;
    rotatedStageBuffer_.assign(static_cast<std::size_t>(dstWidth) * dstHeight * 4u, 0u);

    const uint8_t* src = stageRaw_.data();
    uint8_t* dst = rotatedStageBuffer_.data();
    const std::size_t srcStride = static_cast<std::size_t>(srcWidth) * 4u;
    const std::size_t dstStride = static_cast<std::size_t>(dstWidth) * 4u;

    if (turns == 1) {
        for (UINT y = 0; y < dstHeight; ++y) {
            uint8_t* dstRow = dst + static_cast<std::size_t>(y) * dstStride;
            for (UINT x = 0; x < dstWidth; ++x) {
                const UINT srcX = y;
                const UINT srcY = srcHeight - 1u - x;
                const uint8_t* srcPixel = src + static_cast<std::size_t>(srcY) * srcStride + static_cast<std::size_t>(srcX) * 4u;
                std::memcpy(dstRow + static_cast<std::size_t>(x) * 4u, srcPixel, 4u);
            }
        }
    } else if (turns == 2) {
        for (UINT y = 0; y < dstHeight; ++y) {
            uint8_t* dstRow = dst + static_cast<std::size_t>(y) * dstStride;
            const UINT srcY = srcHeight - 1u - y;
            for (UINT x = 0; x < dstWidth; ++x) {
                const UINT srcX = srcWidth - 1u - x;
                const uint8_t* srcPixel = src + static_cast<std::size_t>(srcY) * srcStride + static_cast<std::size_t>(srcX) * 4u;
                std::memcpy(dstRow + static_cast<std::size_t>(x) * 4u, srcPixel, 4u);
            }
        }
    } else { // turns == 3
        for (UINT y = 0; y < dstHeight; ++y) {
            uint8_t* dstRow = dst + static_cast<std::size_t>(y) * dstStride;
            for (UINT x = 0; x < dstWidth; ++x) {
                const UINT srcX = srcWidth - 1u - y;
                const UINT srcY = x;
                const uint8_t* srcPixel = src + static_cast<std::size_t>(srcY) * srcStride + static_cast<std::size_t>(srcX) * 4u;
                std::memcpy(dstRow + static_cast<std::size_t>(x) * 4u, srcPixel, 4u);
            }
        }
    }

    stageRaw_ = std::move(rotatedStageBuffer_);
    rotatedStageBuffer_.clear();
    width = dstWidth;
    height = dstHeight;
    rawWidth_ = dstWidth;
    rawHeight_ = dstHeight;
    return true;
}

CpuPipelineOutput CpuFramePipeline::BuildStages(UINT width,
                                                UINT height,
                                                const CpuPipelineConfig& config,
                                                bool debugViewEnabled)
{
    CpuPipelineOutput output{};

    if (stageRaw_.empty() || width == 0 || height == 0) {
        stageFinal_.clear();
        compositeBuffer_.clear();
        return output;
    }

    stageBw_ = stageRaw_;
    if (config.enableBlackWhite) {
        ApplyBlackWhite(stageRaw_, stageBw_, config.blackWhiteThreshold);
    }

    const std::vector<uint8_t>* currentStage = config.enableBlackWhite ? &stageBw_ : &stageRaw_;

    const std::vector<uint8_t>& zoomSource = config.enableBlackWhite ? stageBw_ : stageRaw_;
    stageZoom_ = zoomSource;
    if (config.enableZoom) {
        ApplyZoom(zoomSource,
                  stageZoom_,
                  width,
                  height,
                  config.zoomAmount,
                  config.zoomCenterX,
                  config.zoomCenterY);
        currentStage = &stageZoom_;
    }

    if (config.enableBlur) {
        ApplyGaussianBlur(*currentStage,
                          blurScratch_,
                          stageBlur_,
                          width,
                          height,
                          config.blurRadius,
                          config.blurSigma);
        currentStage = &stageBlur_;
    } else {
        stageBlur_.clear();
    }

    stageFinal_ = *currentStage;
    ApplyTemporalSmoothCpu(stageFinal_,
                           temporalHistoryCpu_,
                           temporalHistoryValid_,
                           width,
                           height,
                           config.temporalSmoothAlpha,
                           config.enableTemporalSmooth);

    if (stageFinal_.empty()) {
        compositeBuffer_.clear();
        return output;
    }

    const uint8_t* displayData = stageFinal_.data();
    UINT displayWidth = width;
    UINT displayHeight = height;
    bool useComposite = false;

    if (debugViewEnabled) {
        std::vector<const std::vector<uint8_t>*> debugStages;
        debugStages.reserve(5);
        debugStages.push_back(&stageRaw_);
        if (config.enableBlackWhite) {
            debugStages.push_back(&stageBw_);
        }
        if (config.enableZoom) {
            debugStages.push_back(&stageZoom_);
        }
        if (config.enableBlur && !stageBlur_.empty()) {
            debugStages.push_back(&stageBlur_);
        }
        debugStages.push_back(&stageFinal_);

        if (!debugStages.empty()) {
            const std::size_t stageCount = debugStages.size();
            const std::size_t columns = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(stageCount))));
            const std::size_t rows = (stageCount + columns - 1u) / columns;

            const UINT compositeWidth = width * static_cast<UINT>(columns);
            const UINT compositeHeight = height * static_cast<UINT>(rows);
            const UINT compositeStride = compositeWidth * 4u;
            compositeBuffer_.assign(static_cast<std::size_t>(compositeStride) * compositeHeight, 0u);

            const int frameWidthInt = static_cast<int>(width);
            const int frameHeightInt = static_cast<int>(height);
            const float dominantExtent = static_cast<float>(std::max(width, height));
            const int paddingCandidate = static_cast<int>(std::roundf(dominantExtent * 0.05f));
            const int maxPaddingWidth = std::max(0, (frameWidthInt - 1) / 2);
            const int maxPaddingHeight = std::max(0, (frameHeightInt - 1) / 2);
            const int padding = std::clamp(paddingCandidate, 0, std::min(maxPaddingWidth, maxPaddingHeight));
            const UINT paddingU = static_cast<UINT>(padding);
            const UINT innerWidth = static_cast<UINT>(std::max(1, frameWidthInt - 2 * padding));
            const UINT innerHeight = static_cast<UINT>(std::max(1, frameHeightInt - 2 * padding));

            auto blitScaled = [&](const std::vector<uint8_t>& src, UINT destX, UINT destY) {
                if (innerWidth == 0 || innerHeight == 0) {
                    return;
                }

                const UINT stride = width * 4u;
                const float scaleX = static_cast<float>(width) / static_cast<float>(innerWidth);
                const float scaleY = static_cast<float>(height) / static_cast<float>(innerHeight);

                for (UINT y = 0; y < innerHeight; ++y) {
                    const UINT srcY = std::min(height - 1u, static_cast<UINT>(std::lroundf(static_cast<float>(y) * scaleY)));
                    uint8_t* dstRow = compositeBuffer_.data() +
                                      (static_cast<std::size_t>(destY + paddingU + y) * compositeStride) +
                                      (destX + paddingU) * 4u;
                    const uint8_t* srcRow = src.data() + static_cast<std::size_t>(srcY) * stride;
                    for (UINT x = 0; x < innerWidth; ++x) {
                        const UINT srcX = std::min(width - 1u, static_cast<UINT>(std::lroundf(static_cast<float>(x) * scaleX)));
                        const uint8_t* srcPixel = srcRow + srcX * 4u;
                        uint8_t* dstPixel = dstRow + x * 4u;
                        dstPixel[0] = srcPixel[0];
                        dstPixel[1] = srcPixel[1];
                        dstPixel[2] = srcPixel[2];
                        dstPixel[3] = srcPixel[3];
                    }
                }
            };

            for (std::size_t index = 0; index < stageCount; ++index) {
                const std::size_t row = index / columns;
                const std::size_t column = index % columns;
                const UINT destX = static_cast<UINT>(column) * width;
                const UINT destY = static_cast<UINT>(row) * height;
                const auto* stage = debugStages[index];
                if (stage && !stage->empty()) {
                    blitScaled(*stage, destX, destY);
                }
            }

            displayData = compositeBuffer_.data();
            displayWidth = compositeWidth;
            displayHeight = compositeHeight;
            useComposite = true;
        } else {
            compositeBuffer_.clear();
        }
    } else {
        compositeBuffer_.clear();
    }

    output.data = displayData;
    output.width = displayWidth;
    output.height = displayHeight;
    output.isComposite = useComposite;
    return output;
}

bool CpuFramePipeline::ResampleToFill(UINT targetWidth,
                                      UINT targetHeight,
                                      float centerXNorm,
                                      float centerYNorm)
{
    if (stageRaw_.empty() || rawWidth_ == 0 || rawHeight_ == 0 ||
        targetWidth == 0 || targetHeight == 0) {
        return false;
    }

    std::vector<uint8_t> destination(static_cast<std::size_t>(targetWidth) * targetHeight * 4u, 0u);

    const float srcWidthF = static_cast<float>(rawWidth_);
    const float srcHeightF = static_cast<float>(rawHeight_);
    const float targetAspect = static_cast<float>(targetWidth) / static_cast<float>(targetHeight);
    const float srcAspect = srcWidthF / srcHeightF;

    float cropWidth = srcWidthF;
    float cropHeight = srcHeightF;

    if (targetAspect > srcAspect) {
        cropHeight = srcWidthF / targetAspect;
        cropHeight = std::min(cropHeight, srcHeightF);
    } else {
        cropWidth = srcHeightF * targetAspect;
        cropWidth = std::min(cropWidth, srcWidthF);
    }

    cropWidth = std::clamp(cropWidth, 1.0f, srcWidthF);
    cropHeight = std::clamp(cropHeight, 1.0f, srcHeightF);

    float centerX = std::clamp(centerXNorm, 0.0f, 1.0f) * (srcWidthF - 1.0f);
    float centerY = std::clamp(centerYNorm, 0.0f, 1.0f) * (srcHeightF - 1.0f);

    const float halfCropWidth = cropWidth * 0.5f;
    const float halfCropHeight = cropHeight * 0.5f;

    const float minCenterX = std::max(0.0f, halfCropWidth - 0.5f);
    const float maxCenterX = std::max(minCenterX, (srcWidthF - 1.0f) - (halfCropWidth - 0.5f));
    const float minCenterY = std::max(0.0f, halfCropHeight - 0.5f);
    const float maxCenterY = std::max(minCenterY, (srcHeightF - 1.0f) - (halfCropHeight - 0.5f));

    if (minCenterX <= maxCenterX) {
        centerX = std::clamp(centerX, minCenterX, maxCenterX);
    } else {
        centerX = (srcWidthF - 1.0f) * 0.5f;
    }

    if (minCenterY <= maxCenterY) {
        centerY = std::clamp(centerY, minCenterY, maxCenterY);
    } else {
        centerY = (srcHeightF - 1.0f) * 0.5f;
    }

    float startX = centerX - halfCropWidth + 0.5f;
    float startY = centerY - halfCropHeight + 0.5f;
    startX = std::clamp(startX, 0.0f, srcWidthF - cropWidth);
    startY = std::clamp(startY, 0.0f, srcHeightF - cropHeight);

    const float stepX = cropWidth / static_cast<float>(targetWidth);
    const float stepY = cropHeight / static_cast<float>(targetHeight);
    const std::size_t srcStride = static_cast<std::size_t>(rawWidth_) * 4u;

    for (UINT y = 0; y < targetHeight; ++y) {
        const float sampleY = startY + static_cast<float>(y) * stepY;
        int srcYIndex = static_cast<int>(std::lround(sampleY));
        srcYIndex = std::clamp(srcYIndex, 0, static_cast<int>(rawHeight_) - 1);
        const uint8_t* srcRow = stageRaw_.data() + static_cast<std::size_t>(srcYIndex) * srcStride;
        uint8_t* dstRow = destination.data() + static_cast<std::size_t>(y) * targetWidth * 4u;

        for (UINT x = 0; x < targetWidth; ++x) {
            const float sampleX = startX + static_cast<float>(x) * stepX;
            int srcXIndex = static_cast<int>(std::lround(sampleX));
            srcXIndex = std::clamp(srcXIndex, 0, static_cast<int>(rawWidth_) - 1);
            const uint8_t* srcPixel = srcRow + static_cast<std::size_t>(srcXIndex) * 4u;
            uint8_t* dstPixel = dstRow + static_cast<std::size_t>(x) * 4u;
            dstPixel[0] = srcPixel[0];
            dstPixel[1] = srcPixel[1];
            dstPixel[2] = srcPixel[2];
            dstPixel[3] = srcPixel[3];
        }
    }

    stageRaw_.swap(destination);
    rawWidth_ = targetWidth;
    rawHeight_ = targetHeight;
    return true;
}

void CpuFramePipeline::ResetTemporalHistory()
{
    temporalHistoryCpu_.clear();
    temporalHistoryValid_ = false;
}

} // namespace okuflow::processing

#endif // _WIN32
