#include "openzoom/common/view_transform.hpp"
#include "openzoom/app/pipeline_orchestrator.hpp"

#include <QtTest/QTest>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace openzoom {

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
    void fillPreservesUniformScaleAcrossAspectRatios();
    void checkerboardCirclePatternStaysRound();
    void rotationAdjustedDimensionsPreserveUniformScale();
    void fillAppliesZoomAndClampsFocus();
    void fitShowsWholeSourceWithSymmetricBars();
    void pixelMappingUsesCanonicalTransform();
    void cachedRoiRemapsWithoutChangingDestinationGeometry();
    void cachedRoiRejectsUncoveredViewport();
    void fenceSequencerAdoptsPresenterSlotSignals();
    void fenceSequencerIgnoresMissedAdmission();
};

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

void ViewTransformTests::fenceSequencerAdoptsPresenterSlotSignals() {
    FenceSequencer sequencer;
    sequencer.Reset(10);

    const FenceSequencer::CudaTicket cuda = sequencer.BeginCudaFrame(10);
    QCOMPARE(cuda.waitValue, 10u);
    QCOMPARE(cuda.signalValue, 11u);
    sequencer.CudaSignaled();
    QCOMPARE(sequencer.LastCudaSignal(), 11u);

    const std::uint64_t requestedGraphics =
        sequencer.BeginGraphicsFrame(10);
    QCOMPARE(requestedGraphics, 12u);

    // The presenter emits the requested shared-texture signal followed by a
    // second signal used to retire its back-buffer slot.
    sequencer.GraphicsSignaled(13);

    // A viewport-only present must reserve beyond that internal slot signal.
    const std::uint64_t viewportOnlyGraphics =
        sequencer.BeginGraphicsFrame(13);
    QCOMPARE(viewportOnlyGraphics, 14u);
    sequencer.GraphicsSignaled(15);

    // The next CUDA frame waits for the newest completed texture reader and
    // receives a value that has never already been signaled.
    const FenceSequencer::CudaTicket nextCuda =
        sequencer.BeginCudaFrame(15);
    QCOMPARE(nextCuda.waitValue, 15u);
    QCOMPARE(nextCuda.signalValue, 16u);
}

void ViewTransformTests::fenceSequencerIgnoresMissedAdmission() {
    FenceSequencer sequencer;
    sequencer.Reset(10);
    const auto first = sequencer.BeginCudaFrame(10);
    QCOMPARE(first.signalValue, 11u);
    sequencer.CudaSignaled();
    QCOMPARE(sequencer.BeginGraphicsFrame(10), 12u);
    // Admission was busy: no draw or signal was submitted. The next CUDA
    // frame must not wait for that unsubmitted graphics value.
    const auto next = sequencer.BeginCudaFrame(10);
    QCOMPARE(next.waitValue, 10u);
    QCOMPARE(next.signalValue, 12u);
    sequencer.CudaSignaled();
    // Independent recording waits on CUDA 12 and signals graphics 13 even
    // when the viewport remains busy. That real reader must be respected.
    sequencer.GraphicsSignaled(13);
    const auto afterRecording = sequencer.BeginCudaFrame(13);
    QCOMPARE(afterRecording.waitValue, 13u);
    QCOMPARE(afterRecording.signalValue, 14u);
}

} // namespace openzoom

QTEST_GUILESS_MAIN(openzoom::ViewTransformTests)

#include "view_transform_tests.moc"
