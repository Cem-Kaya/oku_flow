#pragma once

#ifdef _WIN32

#include <mfapi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace openzoom::processing {

struct CpuPipelineConfig {
    bool enableBlackWhite{false};
    float blackWhiteThreshold{0.5f};
    bool enableZoom{false};
    float zoomAmount{1.0f};
    float zoomCenterX{0.5f};
    float zoomCenterY{0.5f};
    bool enableBlur{false};
    int blurRadius{3};
    float blurSigma{1.0f};
    bool enableTemporalSmooth{false};
    float temporalSmoothAlpha{0.25f};
};

struct CpuPipelineOutput {
    const uint8_t* data{nullptr};
    UINT width{0};
    UINT height{0};
    bool isComposite{false};
};

class CpuFramePipeline {
public:
    CpuFramePipeline() = default;

    bool ConvertFrameToBgra(const std::vector<uint8_t>& frame,
                            const GUID& subtype,
                            UINT width,
                            UINT height,
                            LONG stride,
                            std::size_t dataSize);

    bool RotateRawBuffer(int quarterTurns, UINT& width, UINT& height);

    CpuPipelineOutput BuildStages(UINT width,
                                  UINT height,
                                  const CpuPipelineConfig& config,
                                  bool debugViewEnabled);

    bool ResampleToFill(UINT targetWidth,
                        UINT targetHeight,
                        float centerXNorm,
                        float centerYNorm);

    void ResetTemporalHistory();

    const std::vector<uint8_t>& StageRaw() const { return stageRaw_; }

    UINT RawWidth() const { return rawWidth_; }
    UINT RawHeight() const { return rawHeight_; }

private:
    std::vector<uint8_t> stageRaw_;
    std::vector<uint8_t> stageBw_;
    std::vector<uint8_t> stageZoom_;
    std::vector<uint8_t> stageBlur_;
    std::vector<uint8_t> stageFinal_;
    std::vector<uint8_t> compositeBuffer_;
    std::vector<uint8_t> blurScratch_;
    std::vector<uint8_t> rotatedStageBuffer_;
    std::vector<float> temporalHistoryCpu_;
    bool temporalHistoryValid_{false};
    UINT rawWidth_{0};
    UINT rawHeight_{0};
};

} // namespace openzoom::processing

#endif // _WIN32
