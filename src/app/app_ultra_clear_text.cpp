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
    const auto models=settingsController_->Settings().ultraClearTextModels;
    mainWindow_->ultraClearTextPanel()->SetModelSettings(models);
    // Restore intent separately from the Text Clarity master gate.
    ultraClearText_ = std::make_unique<UltraClearTextController>(
        mainWindow_->ultraClearTextOverlay(), mainWindow_->ultraClearTextPanel(),
        [models] { return uct::CreateDefaultRecognizer(models); });
    ultraClearText_->SetTextClarityEnabled(autoTextClarityEnabled_);
    ultraClearText_->SetEnabled(settingsController_->Settings().ultraClearTextEnabled);
    connect(mainWindow_->ultraClearTextPanel(), &UltraClearTextPanel::enabledChanged,
            this, [this](bool enabled) {
                settingsController_->MutableSettings().ultraClearTextEnabled = enabled;
                SavePersistentSettings();
            });
    connect(mainWindow_->ultraClearTextPanel(),&UltraClearTextPanel::modelSettingsChanged,
            this,&OkuFlowApp::OnUltraClearTextModelsChanged);
    if (assistiveManager_ && mainWindow_->ultraClearTextOverlay()) {
        mainWindow_->ultraClearTextOverlay()->SetExcludedWidget(&assistiveManager_->Overlay());
    }
}

void OkuFlowApp::OnUltraClearTextModelsChanged()
{
    if(!mainWindow_ || !ultraClearText_)return;
    const auto models=mainWindow_->ultraClearTextPanel()->ModelSettings();
    settingsController_->MutableSettings().ultraClearTextModels=models;
    // Capture an immutable value, never an app/settings pointer on the OCR worker.
    ultraClearText_->ReplaceRecognizerFactory([models]{return uct::CreateDefaultRecognizer(models);});
    SavePersistentSettings();
}

void OkuFlowApp::ApplyUltraClearTextDisplayColors(int transform, std::uint64_t lutGeneration)
{
    if (!mainWindow_ || !mainWindow_->ultraClearTextOverlay()) return;
    // A cached camera scene can still belong to an earlier palette. Do not
    // attach a newer requested LUT to that successfully presented old scene.
    // Its old overlay mapping stays in place until the new scene succeeds.
    if (transform == 2 && lutGeneration != displayColorLutGeneration_) return;
    using Colors = UltraClearTextOverlay::DisplayColors;
    mainWindow_->ultraClearTextOverlay()->SetDisplayColors(
        transform == 2 ? Colors::LumaLut : transform == 1 ? Colors::Inverted : Colors::Source,
        displayColorLut_);
}

// What the user asks for now, as the CUDA path would apply it. The CPU path
// narrows this in UctSceneProvenance::RequestFor.
UctSceneSettings OkuFlowApp::UltraClearTextRequest() const
{
    UctSceneSettings request;
    request.sourceEpoch = cameraSessionId_;
    request.rotationQuarterTurns = rotationQuarterTurns_;
    request.keystone = keystoneEnabled_;
    request.stabilization = stabilizationEnabled_;
    request.warpRevision = uctWarpRevision_;
    // "normal" leaves pixels untouched; every other scheme recolors them, and
    // the LUT generation changes whenever its colors do.
    request.colorScheme =
        displayColorScheme_.id == QStringLiteral("normal")
            ? 0u
            : displayColorLutGeneration_;
    return request;
}

UctFrameContext OkuFlowApp::UltraClearTextContext(bool cudaScene, const ViewTransform& transform,
                                                  UINT viewportWidth, UINT viewportHeight) const
{
    // Composites the path would produce for the current request: CUDA runs
    // Temporal Smooth and Extra Stable; the CPU fallback only blends in the
    // legacy debug view.
    const bool requestedComposite =
        cudaScene ? (temporalSmoothEnabled_ || (stabilizationEnabled_ && bumpHoldEnabled_))
                  : (debugViewEnabled_ && temporalSmoothEnabled_);
    // Displayed-composite truth above stays for annotations/observations.
    // UCT analysis is unsupported only for the CPU debug composite, whose
    // pixels are the blend itself. CUDA Temporal Smooth gets a neutral paired
    // capture; CUDA Extra Stable captures carry their frame's hold record.
    const bool requestedUnsupportedComposite =
        !cudaScene && debugViewEnabled_ && temporalSmoothEnabled_;
    const bool requestedHeldFrames = cudaScene && stabilizationEnabled_ && bumpHoldEnabled_;
    return uctProvenance_.Context(cudaScene, UltraClearTextRequest(), requestedComposite, transform,
                                  viewportWidth, viewportHeight, cameraActive_ && cameraFramePresented_,
                                  strokeWeight_, requestedUnsupportedComposite, requestedHeldFrames);
}

} // namespace okuflow
