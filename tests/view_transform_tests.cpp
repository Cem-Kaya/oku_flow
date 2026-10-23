#include "okuflow/common/view_transform.hpp"
#include "okuflow/d3d12/shared_timeline.hpp"

#include <QtTest/QTest>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace okuflow {

namespace {

std::vector<std::uint8_t> BuildCheckerboardCirclePattern(
    std::uint32_t width,
    std::uint32_t height) {
    std::vector<std::uint8_t> pattern(
        static_cast<std::size_t>(width) * height);
    const float centerX = static_cast<float>(width) * 0.5f;
    const float centerY = static_cast<float>(height) * 0.5f;
    const float radius = static_cast<float>(std::min(width, height)) / 6.0f;
    const float radiusSquared = radius * radius;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool checker = ((x / 16u) + (y / 16u)) % 2u != 0u;
            const float dx = (static_cast<float>(x) + 0.5f) - centerX;
            const float dy = (static_cast<float>(y) + 0.5f) - centerY;
            pattern[static_cast<std::size_t>(y) * width + x] =
                dx * dx + dy * dy <= radiusSquared
                    ? 255u
                    : (checker ? 64u : 0u);
        }
    }
    return pattern;
}

std::vector<std::uint8_t> PresentPattern(
    const std::vector<std::uint8_t>& source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    const PixelViewMapping& mapping) {
    std::vector<std::uint8_t> presented(
        static_cast<std::size_t>(mapping.targetWidth) *
        mapping.targetHeight);
    for (std::uint32_t y = 0; y < mapping.activeHeight; ++y) {
        const auto sourceY = static_cast<std::uint32_t>(std::clamp(
            static_cast<int>(std::lround(
                mapping.startY + static_cast<float>(y) * mapping.stepY)),
            0,
            static_cast<int>(sourceHeight) - 1));
        for (std::uint32_t x = 0; x < mapping.activeWidth; ++x) {
            const auto sourceX = static_cast<std::uint32_t>(std::clamp(
                static_cast<int>(std::lround(
                    mapping.startX + static_cast<float>(x) * mapping.stepX)),
                0,
                static_cast<int>(sourceWidth) - 1));
            presented[
                static_cast<std::size_t>(mapping.offsetY + y) *
                    mapping.targetWidth +
                mapping.offsetX + x] =
                source[static_cast<std::size_t>(sourceY) * sourceWidth +
                       sourceX];
        }
    }
    return presented;
}

} // namespace

class ViewTransformTests final : public QObject {
    Q_OBJECT

private slots:
    void wideAndPortraitZoomOutFitsAndStaysUniform();
    void zoomStepsStopAtFillWithoutSticking();
    void partialLetterboxHoldReconstructsExactScale();
    void zoomAnchorPreservesActualViewportAtClampedEdges();
    void fillPreservesUniformScaleAcrossAspectRatios();
    void checkerboardCirclePatternStaysRound();
    void rotationAdjustedDimensionsPreserveUniformScale();
    void fillAppliesZoomAndClampsFocus();
    void fitShowsWholeSourceWithSymmetricBars();
    void pixelMappingUsesCanonicalTransform();
    void sixteenXViewportAndCpuMappingShareCrop();
    void cachedRoiRemapsWithoutChangingDestinationGeometry();
    void cachedRoiRejectsUncoveredViewport();
    void superResCanonicalViewportFitsFreshIntegerCrop();
    void superResDefault720pViewportRemainsUniform();
    void sharedTimelineAdoptsPresenterSlotSignals();
    void sharedTimelineIgnoresMissedAdmission();
};

void ViewTransformTests::wideAndPortraitZoomOutFitsAndStaysUniform() {
    for (const auto source : {QSize(2560,1080), QSize(1080,1920), QSize(100,30000)}) {
        for (const auto viewport : {QSize(1920,1080), QSize(700,900), QSize(3440,1440)}) {
            const float fit = ComputeFitZoom(source.width(),source.height(),viewport.width(),viewport.height());
            QVERIFY(fit > 0 && fit <= 1);
            const auto whole = ComputeViewTransform(source.width(),source.height(),viewport.width(),viewport.height(),
                kWholeSourceZoom,0.0f,1.0f,ViewportFitMode::kFill);
            const auto fitted = ComputeViewTransform(source.width(),source.height(),viewport.width(),viewport.height(),
                1,0.5f,0.5f,ViewportFitMode::kFit);
            QVERIFY(std::abs(whole.sourceWidth-1)<1e-5f && std::abs(whole.sourceHeight-1)<1e-5f);
            QVERIFY(std::abs(whole.destinationWidth-fitted.destinationWidth)<1e-5f);
            QVERIFY(std::abs(whole.destinationHeight-fitted.destinationHeight)<1e-5f);
            for (const float zoom : {fit, (fit+1)*0.5f, 1.0f, 1.33f, 2.0f, 16.0f}) {
                const auto view = ComputeViewTransform(source.width(),source.height(),viewport.width(),viewport.height(),
                    zoom,0.05f,0.95f,ViewportFitMode::kFill);
                const float x = view.sourceWidth*source.width()/(view.destinationWidth*viewport.width());
                const float y = view.sourceHeight*source.height()/(view.destinationHeight*viewport.height());
                QVERIFY(std::abs(x-y)<1e-4f*std::max(x,y));
                QVERIFY(view.sourceX >= 0 && view.sourceY >= 0);
                QVERIFY(view.sourceX+view.sourceWidth<=1.00001f && view.sourceY+view.sourceHeight<=1.00001f);
                if (zoom>=1) {
                    QCOMPARE(view.destinationX,0.0f);QCOMPARE(view.destinationY,0.0f);
                    QCOMPARE(view.destinationWidth,1.0f);QCOMPARE(view.destinationHeight,1.0f);
                }
            }
        }
    }
}

void ViewTransformTests::zoomStepsStopAtFillWithoutSticking() {
    const float fit = ComputeFitZoom(1080,1920,1920,1080);
    QCOMPARE(StepFillZoom(0.99f,1,1,fit),1.0f);
    QCOMPARE(StepFillZoom(1.01f,-1,1,fit),1.0f);
    QVERIFY(StepFillZoom(1,1,1,fit)>1);
    QVERIFY(StepFillZoom(1,-1,1,fit)<1);
    QCOMPARE(StepFillZoom(fit,-1,1,fit),kWholeSourceZoom);
    QVERIFY(StepFillZoom(kWholeSourceZoom,1,1,fit)>fit);
    QCOMPARE(EffectiveFillZoom(kWholeSourceZoom,fit),fit);
    QCOMPARE(EffectiveFillZoom(kWholeSourceZoom,0.001f),0.001f);
    QCOMPARE(ComputeFitZoom(0,1920,1920,1080),1.0f);
    QCOMPARE(EffectiveFillZoom(std::numeric_limits<float>::quiet_NaN(),fit),1.0f);
}

void ViewTransformTests::partialLetterboxHoldReconstructsExactScale() {
    for (const auto source : {QSize(2560,1080),QSize(1080,1920)}) {
        const float fit = ComputeFitZoom(source.width(),source.height(),1920,1080);
        for (const float zoom : {fit,(fit+1)*0.5f,1.0f,2.0f}) {
            const auto before = ComputeViewTransform(source.width(),source.height(),1920,1080,
                zoom,0.23f,0.78f,ViewportFitMode::kFill);
            const float recovered = FillZoomFromTransform(before);
            QVERIFY(std::abs(recovered-zoom)<1e-5f);
            const auto after = ComputeViewTransform(source.width(),source.height(),1920,1080,recovered,
                before.sourceX+before.sourceWidth/2,before.sourceY+before.sourceHeight/2,ViewportFitMode::kFill);
            QVERIFY(std::abs(after.sourceX-before.sourceX)<1e-5f);
            QVERIFY(std::abs(after.sourceY-before.sourceY)<1e-5f);
            QVERIFY(std::abs(after.destinationWidth-before.destinationWidth)<1e-5f);
            QVERIFY(std::abs(after.destinationHeight-before.destinationHeight)<1e-5f);
        }
    }
}

void ViewTransformTests::zoomAnchorPreservesActualViewportAtClampedEdges() {
    for (const auto [sourceWidth,sourceHeight] : {std::pair{1600u,900u},std::pair{900u,1600u},std::pair{1000u,1000u}})
        for (const auto [viewWidth,viewHeight] : {std::pair{1000u,1000u},std::pair{1280u,720u},std::pair{720u,1280u}}) {
            const auto before = ComputeViewTransform(sourceWidth,sourceHeight,viewWidth,viewHeight,2,0.05f,0.05f,ViewportFitMode::kFill);
            const float anchorX = before.sourceX+before.sourceWidth*0.7f;
            const float anchorY = before.sourceY+before.sourceHeight*0.45f;
            const auto candidate = ComputeViewTransform(sourceWidth,sourceHeight,viewWidth,viewHeight,4,0.05f,0.05f,ViewportFitMode::kFill);
            float x = 0, y = 0;
            QVERIFY(ComputeZoomAnchorCenter(before,candidate,anchorX,anchorY,x,y));
            const auto after = ComputeViewTransform(sourceWidth,sourceHeight,viewWidth,viewHeight,4,x,y,ViewportFitMode::kFill);
            QVERIFY(std::abs((anchorX-after.sourceX)/after.sourceWidth-0.7f)<0.0001f);
            QVERIFY(std::abs((anchorY-after.sourceY)/after.sourceHeight-0.45f)<0.0001f);
        }
    const auto valid = ComputeViewTransform(1000,1000,1000,1000,2,0.5,0.5,ViewportFitMode::kFill);
    float x = 0.4f, y = 0.6f;
    QVERIFY(!ComputeZoomAnchorCenter(valid,valid,std::numeric_limits<float>::quiet_NaN(),0.5,x,y));
    QCOMPARE(x,0.4f); QCOMPARE(y,0.6f);
    QVERIFY(!ComputeZoomAnchorCenter(valid,valid,0,0,x,y)); // outside visible crop
    auto invalid = valid; invalid.destinationWidth = 0;
    QVERIFY(!ComputeZoomAnchorCenter(valid,invalid,0.5,0.5,x,y));
}

void ViewTransformTests::fillPreservesUniformScaleAcrossAspectRatios() {
    for (const auto [width, height] :
         {std::pair{1280u, 720u},
          std::pair{1024u, 768u},
          std::pair{800u, 800u},
          std::pair{720u, 1280u},
          std::pair{2100u, 900u}}) {
        const ViewTransform transform =
            ComputeViewTransform(1280,
                                 720,
                                 width,
                                 height,
                                 1.0f,
                                 0.5f,
                                 0.5f,
                                 ViewportFitMode::kFill);
        QVERIFY(transform.valid);
        const float sampledPixelsPerViewportPixelX =
            transform.sourceWidth * 1280.0f / static_cast<float>(width);
        const float sampledPixelsPerViewportPixelY =
            transform.sourceHeight * 720.0f / static_cast<float>(height);
        QVERIFY(std::abs(sampledPixelsPerViewportPixelX -
                         sampledPixelsPerViewportPixelY) < 0.0001f);
        QCOMPARE(transform.destinationX, 0.0f);
        QCOMPARE(transform.destinationY, 0.0f);
        QCOMPARE(transform.destinationWidth, 1.0f);
        QCOMPARE(transform.destinationHeight, 1.0f);
    }
}

void ViewTransformTests::checkerboardCirclePatternStaysRound() {
    constexpr std::uint32_t kSourceWidth = 320;
    constexpr std::uint32_t kSourceHeight = 180;
    const std::vector<std::uint8_t> pattern =
        BuildCheckerboardCirclePattern(kSourceWidth, kSourceHeight);

    for (const auto [width, height] :
         {std::pair{160u, 90u},
          std::pair{160u, 120u},
          std::pair{120u, 120u},
          std::pair{90u, 160u},
          std::pair{210u, 90u}}) {
        const ViewTransform transform =
            ComputeViewTransform(kSourceWidth,
                                 kSourceHeight,
                                 width,
                                 height,
                                 1.0f,
                                 0.5f,
                                 0.5f,
                                 ViewportFitMode::kFill);
        const PixelViewMapping mapping =
            ComputePixelViewMapping(
                transform, kSourceWidth, kSourceHeight, width, height);
        QVERIFY(mapping.valid);
        const std::vector<std::uint8_t> presented =
            PresentPattern(
                pattern, kSourceWidth, kSourceHeight, mapping);

        std::uint32_t minX = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t minY = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t maxX = 0;
        std::uint32_t maxY = 0;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                if (presented[static_cast<std::size_t>(y) * width + x] <
                    255u) {
                    continue;
                }
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
        QVERIFY(minX != std::numeric_limits<std::uint32_t>::max());
        const int circleWidth = static_cast<int>(maxX - minX + 1u);
        const int circleHeight = static_cast<int>(maxY - minY + 1u);
        QVERIFY(std::abs(circleWidth - circleHeight) <= 2);
    }
}

void ViewTransformTests::rotationAdjustedDimensionsPreserveUniformScale() {
    for (const auto [width, height] :
         {std::pair{1280u, 720u},
          std::pair{1024u, 768u},
          std::pair{720u, 1280u}}) {
        const ViewTransform transform =
            ComputeViewTransform(720,
                                 1280,
                                 width,
                                 height,
                                 1.0f,
                                 0.5f,
                                 0.5f,
                                 ViewportFitMode::kFill);
        QVERIFY(transform.valid);
        const float sourcePixelsPerViewportPixelX =
            transform.sourceWidth * 720.0f / static_cast<float>(width);
        const float sourcePixelsPerViewportPixelY =
            transform.sourceHeight * 1280.0f / static_cast<float>(height);
        QVERIFY(std::abs(sourcePixelsPerViewportPixelX -
                         sourcePixelsPerViewportPixelY) < 0.0001f);
    }
}

void ViewTransformTests::fillAppliesZoomAndClampsFocus() {
    const ViewTransform centered =
        ComputeViewTransform(1280,
                             720,
                             1280,
                             720,
                             2.0f,
                             0.5f,
                             0.5f,
                             ViewportFitMode::kFill);
    QVERIFY(centered.valid);
    QVERIFY(std::abs(centered.sourceWidth - 0.5f) < 0.0001f);
    QVERIFY(std::abs(centered.sourceHeight - 0.5f) < 0.0001f);
    QVERIFY(std::abs(centered.sourceX - 0.25f) < 0.0001f);
    QVERIFY(std::abs(centered.sourceY - 0.25f) < 0.0001f);

    const ViewTransform edge =
        ComputeViewTransform(1280,
                             720,
                             1280,
                             720,
                             2.0f,
                             1.0f,
                             1.0f,
                             ViewportFitMode::kFill);
    QVERIFY(std::abs(edge.sourceX - 0.5f) < 0.0001f);
    QVERIFY(std::abs(edge.sourceY - 0.5f) < 0.0001f);
}

void ViewTransformTests::fitShowsWholeSourceWithSymmetricBars() {
    const ViewTransform transform =
        ComputeViewTransform(1280,
                             720,
                             720,
                             1280,
                             1.0f,
                             0.5f,
                             0.5f,
                             ViewportFitMode::kFit);
    QVERIFY(transform.valid);
    QCOMPARE(transform.sourceX, 0.0f);
    QCOMPARE(transform.sourceY, 0.0f);
    QCOMPARE(transform.sourceWidth, 1.0f);
    QCOMPARE(transform.sourceHeight, 1.0f);
    QVERIFY(std::abs(transform.destinationX) < 0.0001f);
    QVERIFY(std::abs((1.0f - transform.destinationHeight) * 0.5f -
                     transform.destinationY) < 0.0001f);
}

void ViewTransformTests::pixelMappingUsesCanonicalTransform() {
    const ViewTransform transform =
        ComputeViewTransform(1280,
                             720,
                             720,
                             1280,
                             2.0f,
                             0.75f,
                             0.25f,
                             ViewportFitMode::kFill);
    const PixelViewMapping mapping =
        ComputePixelViewMapping(transform, 1280, 720, 720, 1280);
    QVERIFY(mapping.valid);
    QCOMPARE(mapping.targetWidth, 720u);
    QCOMPARE(mapping.targetHeight, 1280u);
    QCOMPARE(mapping.activeWidth, 720u);
    QCOMPARE(mapping.activeHeight, 1280u);
    const float sampledPixelsPerViewportPixelX =
        transform.sourceWidth * 1280.0f /
        static_cast<float>(mapping.activeWidth);
    const float sampledPixelsPerViewportPixelY =
        transform.sourceHeight * 720.0f /
        static_cast<float>(mapping.activeHeight);
    QVERIFY(std::abs(mapping.stepX - sampledPixelsPerViewportPixelX) < 0.0001f);
    QVERIFY(std::abs(mapping.stepY - sampledPixelsPerViewportPixelY) < 0.0001f);
    QVERIFY(std::abs(mapping.stepX - mapping.stepY) < 0.0001f);
}

void ViewTransformTests::cachedRoiRemapsWithoutChangingDestinationGeometry() {
    const ViewTransform requested =
        ComputeViewTransform(1280,
                             720,
                             1024,
                             768,
                             2.0f,
                             0.5f,
                             0.5f,
                             ViewportFitMode::kFill);
    const NormalizedSourceRect roi{0.25f, 0.25f, 0.5f, 0.5f};
    ViewTransform remapped;
    QVERIFY(RemapViewTransformToSourceRect(requested, roi, remapped));
    QVERIFY(remapped.valid);
    QCOMPARE(remapped.destinationX, requested.destinationX);
    QCOMPARE(remapped.destinationY, requested.destinationY);
    QCOMPARE(remapped.destinationWidth, requested.destinationWidth);
    QCOMPARE(remapped.destinationHeight, requested.destinationHeight);
    QVERIFY(std::abs(remapped.sourceWidth -
                     requested.sourceWidth / roi.width) < 0.0001f);
    QVERIFY(std::abs(remapped.sourceHeight -
                     requested.sourceHeight / roi.height) < 0.0001f);
}

void ViewTransformTests::cachedRoiRejectsUncoveredViewport() {
    const ViewTransform requested =
        ComputeViewTransform(1280,
                             720,
                             1280,
                             720,
                             2.0f,
                             0.9f,
                             0.5f,
                             ViewportFitMode::kFill);
    const NormalizedSourceRect staleCenterRoi{
        0.25f, 0.25f, 0.5f, 0.5f};
    ViewTransform remapped;
    QVERIFY(!RemapViewTransformToSourceRect(
        requested, staleCenterRoi, remapped));
    QVERIFY(!remapped.valid);
}

void ViewTransformTests::sixteenXViewportAndCpuMappingShareCrop() {
    for (const auto [width, height] :
         {std::pair{1280u, 720u}, std::pair{1600u, 840u},
          std::pair{720u, 1280u}}) {
        for (const float focus : {0.0f, 0.24f, 1.0f}) {
            const ViewTransform native = ComputeViewTransform(
                1280, 720, width, height, 1.0f, focus, focus,
                ViewportFitMode::kFill);
            // This is the transform consumed by the D3D12 GPU viewport.
            const ViewTransform magnified = ComputeViewTransform(
                1280, 720, width, height, 16.0f, focus, focus,
                ViewportFitMode::kFill);
            QVERIFY(magnified.valid);
            QVERIFY(std::abs(magnified.sourceWidth * 16.0f -
                             native.sourceWidth) < 0.00001f);
            QVERIFY(std::abs(magnified.sourceHeight * 16.0f -
                             native.sourceHeight) < 0.00001f);
            QVERIFY(magnified.sourceX >= 0.0f);
            QVERIFY(magnified.sourceY >= 0.0f);
            QVERIFY(magnified.sourceX + magnified.sourceWidth <= 1.00001f);
            QVERIFY(magnified.sourceY + magnified.sourceHeight <= 1.00001f);
            // CPU passthrough, pointer mapping and capture use the same crop.
            const PixelViewMapping cpu = ComputePixelViewMapping(
                magnified, 1280, 720, width, height);
            QVERIFY(cpu.valid);
            QCOMPARE(cpu.activeWidth, width);
            QCOMPARE(cpu.activeHeight, height);
            QVERIFY(std::abs(cpu.stepX - cpu.stepY) < 0.00001f);
            QVERIFY(std::abs(cpu.stepX * width -
                             magnified.sourceWidth * 1280.0f) < 0.0001f);
            QVERIFY(std::abs(cpu.stepY * height -
                             magnified.sourceHeight * 720.0f) < 0.0001f);
        }
    }
}

void ViewTransformTests::superResCanonicalViewportFitsFreshIntegerCrop() {
    for (float zoom : {1.33f, 1.5f, 2.0f, 3.0f, 4.0f}) {
        for (float focus : {0.0f, 0.253f, 0.317f, 0.5f, 0.999f, 1.0f}) {
            const auto requested = ComputeViewTransform(1920, 1080, 1920, 1080,
                zoom, focus, focus, ViewportFitMode::kFill);
            const auto canonical = CanonicalizeSuperResViewTransform(
                requested, 1920, 1080, 1920, 1080, zoom);
            const unsigned int numerator = zoom >= 4.0f ? 4u : zoom >= 3.0f ? 3u :
                zoom >= 2.0f ? 2u : zoom >= 1.5f ? 3u : 4u;
            const unsigned int denominator = zoom >= 2.0f ? 1u : zoom >= 1.5f ? 2u : 3u;
            const auto cropW = 1920u * denominator / numerator;
            const auto cropH = 1080u * denominator / numerator;
            unsigned int x = 0, y = 0;
            QVERIFY(ComputeContainingSourceCrop(canonical, 1920, 1080, cropW, cropH, x, y));
            const NormalizedSourceRect roi{float(x)/1920.0f, float(y)/1080.0f,
                float(cropW)/1920.0f, float(cropH)/1080.0f};
            ViewTransform remapped;
            QVERIFY(RemapViewTransformToSourceRect(canonical, roi, remapped));
            QCOMPARE(remapped.destinationWidth, requested.destinationWidth);
            QCOMPARE(remapped.destinationHeight, requested.destinationHeight);
            QVERIFY(std::abs(canonical.sourceX-requested.sourceX) <= 1.0f/1920.0f + 0.005f);
        }
    }
}

void ViewTransformTests::superResDefault720pViewportRemainsUniform() {
    for (float zoom : {1.33f, 2.0f}) {
    const float cropFraction = zoom == 1.33f ? 0.75f : 0.5f;
    const unsigned int cropWidth = zoom == 1.33f ? 960u : 640u;
    const unsigned int cropHeight = zoom == 1.33f ? 540u : 360u;
    for (const auto viewport : {std::pair{1280u, 720u}, std::pair{1024u, 768u},
                                std::pair{720u, 1280u}}) {
        for (float focus : {0.0f, 0.253f, 0.317f, 0.5f, 0.999f, 1.0f}) {
            const auto requested = ComputeViewTransform(1280, 720,
                viewport.first, viewport.second, zoom, focus, focus, ViewportFitMode::kFill);
            const auto canonical = CanonicalizeSuperResViewTransform(
                requested, 1280, 720, 1280, 720, zoom);
            unsigned int x = 0, y = 0;
            QVERIFY(ComputeContainingSourceCrop(canonical, 1280, 720, cropWidth, cropHeight, x, y));
            ViewTransform remapped;
            QVERIFY(RemapViewTransformToSourceRect(canonical,
                {float(x)/1280.0f, float(y)/720.0f, cropFraction, cropFraction}, remapped));
            const float stepX = canonical.sourceWidth * 1280.0f / viewport.first;
            const float stepY = canonical.sourceHeight * 720.0f / viewport.second;
            QVERIFY(std::abs(stepX-stepY) < 0.0001f);
            const auto moved = ComputeViewTransform(1280, 720,
                viewport.first, viewport.second, zoom, focus < 0.5f ? 1.0f : 0.0f,
                focus < 0.5f ? 1.0f : 0.0f, ViewportFitMode::kFill);
            if (viewport.first == 1280) {
                QVERIFY(!RemapViewTransformToSourceRect(moved,
                    {float(x)/1280.0f, float(y)/720.0f, cropFraction, cropFraction}, remapped));
            }
        }
    }
    }
}

void ViewTransformTests::sharedTimelineAdoptsPresenterSlotSignals() {
    SharedFenceTimeline timeline;
    const auto firstCuda = timeline.ReserveExternal();
    QVERIFY(timeline.CommitExternal(firstCuda));
    QVERIFY(timeline.QueueDependency([](auto) { return true; }));
    const auto sceneReader = timeline.ReserveGraphics();
    timeline.GraphicsSubmitted(sceneReader);
    const auto backBufferSlot = timeline.ReserveGraphics();
    timeline.GraphicsSubmitted(backBufferSlot);
    const auto viewportReader = timeline.ReserveGraphics();
    timeline.GraphicsSubmitted(viewportReader);
    const auto nextCudaWait = timeline.LastGraphics();
    const auto nextCudaSignal = timeline.ReserveExternal();
    QCOMPARE(nextCudaWait, viewportReader);
    QVERIFY(nextCudaSignal > viewportReader);
    QVERIFY(viewportReader > backBufferSlot && backBufferSlot > sceneReader);
}

void ViewTransformTests::sharedTimelineIgnoresMissedAdmission() {
    SharedFenceTimeline timeline;
    const auto first = timeline.ReserveExternal();
    QVERIFY(timeline.CommitExternal(first));
    // Busy admission creates no graphics wait target, while a recording clone
    // submitted independently must become the next CUDA reader dependency.
    QCOMPARE(timeline.LastGraphics(), std::uint64_t{0});
    const auto second = timeline.ReserveExternal();
    QVERIFY(second > first);
    QVERIFY(timeline.CommitExternal(second));
    std::uint64_t waited = 0;
    QVERIFY(timeline.QueueDependency([&](auto value) { waited = value; return true; }));
    QCOMPARE(waited, second);
    const auto clone = timeline.ReserveGraphics();
    timeline.GraphicsSubmitted(clone);
    QCOMPARE(timeline.LastGraphics(), clone);
    QVERIFY(timeline.ReserveExternal() > clone);
}

} // namespace okuflow

QTEST_GUILESS_MAIN(okuflow::ViewTransformTests)

#include "view_transform_tests.moc"
