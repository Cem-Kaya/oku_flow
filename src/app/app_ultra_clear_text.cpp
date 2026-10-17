#include "okuflow/app/app.hpp"

#include "okuflow/ui/main_window.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"

namespace okuflow {

// Ultra Clear Text attaches to the existing presenter readback ring and the
// annotation scene transform. It adds no capture or render pipeline.
void OkuFlowApp::InitializeUltraClearText()
{
    if (!mainWindow_ || ultraClearText_) {
        return;
    }
    // Off by default and session-only: the user opts in each session.
    ultraClearText_ = std::make_unique<UltraClearTextController>(
        mainWindow_->ultraClearTextOverlay(), mainWindow_->ultraClearTextPanel(),
        [] { return uct::CreateDefaultRecognizer(); });
    if (assistiveManager_ && mainWindow_->ultraClearTextOverlay()) {
        mainWindow_->ultraClearTextOverlay()->SetExcludedWidget(&assistiveManager_->Overlay());
    }
}

UctFrameContext OkuFlowApp::UltraClearTextContext(const ViewTransform& transform,
                                                  UINT sceneWidth, UINT sceneHeight,
                                                  UINT viewportWidth, UINT viewportHeight) const
{
    UctFrameContext context;
    context.geometry.sourceEpoch = cameraSessionId_;
    context.geometry.sceneWidth = sceneWidth;
    context.geometry.sceneHeight = sceneHeight;
    context.geometry.rotationQuarterTurns = rotationQuarterTurns_;
    context.geometry.keystoneEnabled = keystoneEnabled_;
    context.geometry.stabilizationEnabled = stabilizationEnabled_;
    context.frameSeq = currentCameraFrameSequence_;
    context.transform = transform;
    context.viewportWidth = viewportWidth;
    context.viewportHeight = viewportHeight;
    context.cameraActive = cameraActive_ && cameraFramePresented_;
    context.compositeFrames = temporalSmoothEnabled_ || (stabilizationEnabled_ && bumpHoldEnabled_);
    context.strokeWeight = strokeWeight_;
    return context;
}

} // namespace okuflow
