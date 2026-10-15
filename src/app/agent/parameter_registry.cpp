#include "okuflow/app/agent/parameter_registry.hpp"
#include <algorithm>
#include <array>
#include <vector>
namespace okuflow::agent {
namespace {
constexpr ParameterDescriptor kParameters[] = {
    {"view.zoom.enabled", ParameterGroup::View, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnZoomToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"view.zoom.amount", ParameterGroup::View,
     R"parameter({"type":"number","minimum":1,"maximum":16})parameter", "magnification", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "SetZoom", "present_control; effective hardware capability must be reported separately"},
    {"view.center.x", ParameterGroup::View, R"parameter({"type":"number","minimum":0,"maximum":1})parameter",
     "normalized_scene", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy,
     false, "src/app/app_controls.cpp", "SetZoomCenter",
     "present_control; effective hardware capability must be reported separately"},
    {"view.center.y", ParameterGroup::View, R"parameter({"type":"number","minimum":0,"maximum":1})parameter",
     "normalized_scene", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy,
     false, "src/app/app_controls.cpp", "SetZoomCenter",
     "present_control; effective hardware capability must be reported separately"},
    {"view.focusMarker", ParameterGroup::View, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnFocusMarkerToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"view.fit", ParameterGroup::View, R"parameter({"type":"string","enum":["fill","fit"]})parameter", "none",
     "shared_saved_immediate", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "SetViewportFitMode",
     "present_control; effective hardware capability must be reported separately"},
    {"view.presentationRate", ParameterGroup::View,
     R"parameter({"type":"string","enum":["auto-up-to-120","60","90","120","match-display"]})parameter",
     "fps_cap", "shared_saved_immediate", ParameterPermission::View, ParameterAccess::WritableWithPolicy,
     false, "src/app/app_controls.cpp", "PipelineOrchestrator::SetViewportRateMode (bootstrap lambda)",
     "presentation cap only; does not change negotiated camera FPS"},
    {"view.virtualJoystick", ParameterGroup::View, R"parameter({"type":"boolean"})parameter", "none",
     "shared_saved_immediate", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnVirtualJoystickToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"view.zoomWheelAcceleration", ParameterGroup::View, R"parameter({"type":"boolean"})parameter", "none",
     "shared_saved_immediate", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_bootstrap.cpp", "zoomWheelAccelerationCheckbox toggled lambda",
     "present_control; effective hardware capability must be reported separately"},
    {"image.blackWhite.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnBlackWhiteToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"image.blackWhite.threshold", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0,"maximum":1})parameter", "normalized_luma", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnBlackWhiteThresholdChanged",
     "present_control; effective hardware capability must be reported separately"},
    {"image.contrast", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0.25,"maximum":4})parameter", "none", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnContrastChanged", "present_control; effective hardware capability must be reported separately"},
    {"image.brightness", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":-1,"maximum":1})parameter", "none", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnBrightnessChanged", "present_control; effective hardware capability must be reported separately"},
    {"image.autoContrast.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnAutoContrastToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"image.autoContrast.strength", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0,"maximum":1})parameter", "normalized_strength", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnAutoContrastStrengthChanged",
     "present_control; effective hardware capability must be reported separately"},
    {"image.blur.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnBlurToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"image.blur.sigma", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0.1,"maximum":5})parameter", "sigma_pixels", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnBlurSigmaChanged", "present_control; effective hardware capability must be reported separately"},
    {"image.blur.radius", ParameterGroup::Image,
     R"parameter({"type":"integer","enum":[0,1,2,3,4,5,6,7,8,9,10,15,20,25,30,35,40,45,50]})parameter",
     "pixels", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnBlurRadiusChanged",
     "present_control; effective hardware capability must be reported separately"},
    {"image.temporal.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTemporalSmoothToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"image.temporal.alpha", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0.05,"maximum":1})parameter", "blend_factor", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnTemporalSmoothStrengthChanged",
     "present_control; effective hardware capability must be reported separately"},
    {"image.spatial.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnSpatialSharpenToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"image.spatial.backend", ParameterGroup::Image,
     R"parameter({"type":"string","enum":["fsr1","nis"]})parameter", "none", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnSpatialUpscalerChanged",
     "present_control; effective hardware capability must be reported separately"},
    {"image.spatial.sharpness", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0,"maximum":1})parameter", "normalized_strength", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnSpatialSharpnessChanged",
     "present_control; effective hardware capability must be reported separately"},
    {"image.stabilization.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnStabilizationToggled",
     "requested preference may persist; effective full-strength CUDA only"},
    {"image.keystone.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnKeystoneToggled",
     "requested preference may persist; effective CUDA tracking only"},
    {"image.debugView", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnDebugViewToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"image.textClarity.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.flatten.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.flatten.strength", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0,"maximum":1})parameter", "normalized_strength", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.adaptive.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.adaptive.sauvolaStrength", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0.1,"maximum":0.5})parameter", "none", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.adaptive.softness", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0,"maximum":0.25})parameter", "none", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.polarity", ParameterGroup::Image,
     R"parameter({"type":"string","enum":["auto","dark-on-light","light-on-dark"]})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.strokeWeight", ParameterGroup::Image,
     R"parameter({"type":"integer","minimum":-3,"maximum":3})parameter", "signed_stroke_steps",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.smartSharpen.enabled", ParameterGroup::Image,
     R"parameter({"type":"boolean"})parameter", "none", "live_profile", ParameterPermission::View,
     ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.smartSharpen.strength", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0,"maximum":1})parameter", "normalized_strength", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.clahe.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.clahe.clipLimit", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":1,"maximum":8})parameter", "none", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.twoColor", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.hysteresis.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.hysteresis.strength", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0,"maximum":0.25})parameter", "normalized_strength",
     "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.selectiveSharpen", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.focusDetection.enabled", ParameterGroup::Image,
     R"parameter({"type":"boolean"})parameter", "none", "live_profile", ParameterPermission::View,
     ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.focusDetection.threshold", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0.001,"maximum":0.1})parameter",
     "normalized_viewport_focus_score", "live_profile", ParameterPermission::View,
     ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.glare.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.textClarity.glare.strength", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0,"maximum":1})parameter", "normalized_strength", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnTextClarityControlsChanged",
     "fine tuning retained; MainWindow::refreshTextClarityUi gates refinements while master off; CPU/CUDA "
     "support must be truthful"},
    {"image.superResolution.enabled", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, true,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "OKUFLOW_ENABLE_TEXT_SR + installed Maxine runtime + CUDA; status/performance fallback separate from "
     "requested preference"},
    {"image.superResolution.strength", ParameterGroup::Image,
     R"parameter({"type":"number","minimum":0,"maximum":1})parameter", "normalized_strength", "live_profile",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, true, "src/app/app_controls.cpp",
     "OnTextClarityControlsChanged",
     "OKUFLOW_ENABLE_TEXT_SR + installed Maxine runtime + CUDA; status/performance fallback separate from "
     "requested preference"},
    {"image.superResolution.prefer2x", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, true,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "OKUFLOW_ENABLE_TEXT_SR + installed Maxine runtime + CUDA; status/performance fallback separate from "
     "requested preference"},
    {"image.superResolution.ultra1440p", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, true,
     "src/app/app_controls.cpp", "OnTextClarityControlsChanged",
     "OKUFLOW_ENABLE_TEXT_SR + installed Maxine runtime + CUDA; status/performance fallback separate from "
     "requested preference"},
    {"image.superResolution.performanceOverride", ParameterGroup::Image,
     R"parameter({"type":"boolean"})parameter", "ignore_24ms_limit", "session_only",
     ParameterPermission::View, ParameterAccess::WritableWithPolicy, true, "src/app/app_controls.cpp",
     "SetSuperResPerformanceOverride",
     "only when SuperRes enabled and CUDA surface available; UI shown on measured limit"},
    {"image.stabilization.bumpHold", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "session_only", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnBumpHoldToggled",
     "stabilizationEnabled required; effective CUDA only; no saved field"},
    {"image.keystone.paused", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "session_only", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "CudaInteropSurface::SetKeystoneTrackingPaused",
     "keystoneEnabled + CUDA tracking"},
    {"image.colors.schemeId", ParameterGroup::Image,
     R"parameter({"type":"string","enum":["normal","inverted","white-black","yellow-black","black-yellow","cyan-black","black-cyan","green-black","black-green","amber-black","black-amber","white-blue","blue-white","yellow-blue","blue-yellow","white-dark-red","dark-red-white","magenta-black","black-white","grayscale","yellow-tint","sepia","posterize-6","custom"]})parameter",
     "none", "live_profile", ParameterPermission::View, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnDisplayColorSchemeChanged",
     "present_control; effective hardware capability must be reported separately"},
    {"image.colors.custom.mode", ParameterGroup::Image,
     R"parameter({"type":"string","enum":["duotone","posterize","gradient"]})parameter", "none",
     "editor_draft_until_apply_then_live_profile", ParameterPermission::View,
     ParameterAccess::WritableWithPolicy, false, "src/ui/color_scheme_picker.cpp",
     "ApplyCustomScheme -> schemeChanged -> OkuFlowApp::OnDisplayColorSchemeChanged",
     "custom editor; atomically apply typed color object, not independent invalid draft components"},
    {"image.colors.custom.stopCount", ParameterGroup::Image,
     R"parameter({"type":"integer","minimum":2,"maximum":8})parameter", "none",
     "editor_draft_until_apply_then_live_profile", ParameterPermission::View,
     ParameterAccess::WritableWithPolicy, false, "src/ui/color_scheme_picker.cpp",
     "ApplyCustomScheme -> schemeChanged -> OkuFlowApp::OnDisplayColorSchemeChanged",
     "custom editor; atomically apply typed color object, not independent invalid draft components"},
    {"image.colors.custom.stops", ParameterGroup::Image,
     R"parameter({"type":"array","minItems":2,"maxItems":8,"items":{"type":"string","pattern":"^#[0-9a-fA-F]{6}$","minLength":7,"maxLength":7}})parameter",
     "none", "editor_draft_until_apply_then_live_profile", ParameterPermission::View,
     ParameterAccess::WritableWithPolicy, false, "src/ui/color_scheme_picker.cpp",
     "ApplyCustomScheme -> schemeChanged -> OkuFlowApp::OnDisplayColorSchemeChanged",
     "custom editor; atomically apply typed color object, not independent invalid draft components"},
    {"image.colors.custom.stepped", ParameterGroup::Image, R"parameter({"type":"boolean"})parameter", "none",
     "editor_draft_until_apply_then_live_profile", ParameterPermission::View,
     ParameterAccess::WritableWithPolicy, false, "src/ui/color_scheme_picker.cpp",
     "ApplyCustomScheme -> schemeChanged -> OkuFlowApp::OnDisplayColorSchemeChanged",
     "custom editor; atomically apply typed color object, not independent invalid draft components"},
    {"camera.orientation", ParameterGroup::Camera,
     R"parameter({"type":"integer","enum":[0,90,180,270]})parameter", "degrees_clockwise",
     "live_profile_and_shared_rotation", ParameterPermission::Source, ParameterAccess::WritableWithPolicy,
     false, "src/app/app_controls.cpp", "SetRotationQuarterTurns", "must deny during recording"},
    {"camera.selection", ParameterGroup::Camera,
     R"parameter({"type":"string","pattern":"^cam-[a-f0-9]{32}$","maxLength":256})parameter", "none",
     "manual_saves_source_intent; agent_temporary_selection_preserves_saved_intent",
     ParameterPermission::Source, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnCameraSelectionChanged",
     "generated runtime camera handles and readiness-aware agentSelectable; production source integration "
     "newly written but compilation/device acceptance pending"},
    {"camera.format", ParameterGroup::Camera,
     R"parameter({"type":"string","maxLength":256,"pattern":"^(automatic|fmt-[a-f0-9]{32})$"})parameter",
     "none", "manual_saved_format; agent_temporary_format_must_preserve_saved_intent",
     ParameterPermission::Source, ParameterAccess::WritableWithPolicy, false, "src/app/app_controls.cpp",
     "OnCameraFormatChanged",
     "enumerated advertised format or automatic; never synthesize resolution/FPS; source reopen and "
     "recording exclusion required"},
    {"camera.acceleration", ParameterGroup::Camera,
     R"parameter({"type":"string","enum":["automatic","accelerated","compatibility"]})parameter", "none",
     "shared_saved", ParameterPermission::Source, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_controls.cpp", "OnCameraAccelerationModeChanged",
     "per-camera current identity; confirmed capability only; automatic fallback status read-only"},
    {"camera.phoneTorch", ParameterGroup::Camera, R"parameter({"type":"boolean"})parameter", "none",
     "session_only_device_state", ParameterPermission::Source, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_bootstrap.cpp", "phoneLightButton clicked -> MediaCapture::SetPhoneTorch",
     "selected connected phone + PhoneTorchAvailable; cannot assume physical phone supports torch"},
    {"recording.microphone", ParameterGroup::Recording,
     R"parameter({"type":"string","maxLength":256,"pattern":"^(none|mic-[a-f0-9]{32})$"})parameter", "none",
     "shared_saved_immediate", ParameterPermission::CaptureConfiguration, ParameterAccess::WritableWithPolicy,
     true, "src/app/app_controls.cpp", "OnMicrophoneSelectionChanged",
     "enumerated installed endpoint or no-microphone; __none__ native token must become safe enum/handle"},
    {"recording.canvas", ParameterGroup::Recording,
     R"parameter({"type":"string","enum":["source","360p","480p","720p","1080p","1440p","2160p"]})parameter",
     "processed_mp4_pixels", "shared_saved_immediate", ParameterPermission::CaptureConfiguration,
     ParameterAccess::WritableWithPolicy, false, "src/app/app_bootstrap.cpp",
     "RecordingManager::SetCanvasMode (bootstrap lambda)",
     "source size or640x360,854x480,1280x720,1920x1080,2560x1440,3840x2160"},
    {"assistant_preferences.sceneExplain", ParameterGroup::Assistant,
     R"parameter({"type":"boolean"})parameter", "none", "context_only", ParameterPermission::FrameEgress,
     ParameterAccess::WritableWithPolicy, false, "src/app/app_agent_control.cpp", "SetAgentSceneEnabled",
     "bounded live context frame grant and Observe budgets; preserves unrelated native periodic activity"},
    {"assistant_preferences.overlayEnabled", ParameterGroup::Assistant,
     R"parameter({"type":"boolean"})parameter", "none", "live_profile",
     ParameterPermission::AssistantPreference, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_assistant.cpp", "OnAssistiveOverlayToggled",
     "present_control; effective hardware capability must be reported separately"},
    {"assistant_preferences.provider", ParameterGroup::Assistant,
     R"parameter({"type":"string","enum":["codex","openai-compatible"]})parameter", "none",
     "dialog_draft_until_OK_then_saved", ParameterPermission::ProviderConfiguration,
     ParameterAccess::WritableWithPolicy, true, "src/ui/ai_settings_dialog.cpp",
     "Settings + app/app_assistant.cpp OpenAiSettingsDialog",
     "existing native UI; string fields have no universal native widget maxLength; wire must use explicit "
     "finite bound"},
    {"assistant_preferences.codexModel", ParameterGroup::Assistant,
     R"parameter({"type":"string","maxLength":256})parameter", "none", "dialog_draft_until_OK_then_saved",
     ParameterPermission::AssistantPreference, ParameterAccess::WritableWithPolicy, true,
     "src/ui/ai_settings_dialog.cpp", "Settings + app/app_assistant.cpp OpenAiSettingsDialog",
     "existing native UI; string fields have no universal native widget maxLength; wire must use explicit "
     "finite bound"},
    {"assistant_preferences.codexReasoning", ParameterGroup::Assistant,
     R"parameter({"type":"string","maxLength":256})parameter", "none", "dialog_draft_until_OK_then_saved",
     ParameterPermission::AssistantPreference, ParameterAccess::WritableWithPolicy, true,
     "src/ui/ai_settings_dialog.cpp", "Settings + app/app_assistant.cpp OpenAiSettingsDialog",
     "initial low/medium/high/xhigh; model-reported options replace list; never assume advertised support"},
    {"assistant_preferences.httpModel", ParameterGroup::Assistant,
     R"parameter({"type":"string","maxLength":256})parameter", "none", "dialog_draft_until_OK_then_saved",
     ParameterPermission::AssistantPreference, ParameterAccess::WritableWithPolicy, true,
     "src/ui/ai_settings_dialog.cpp", "Settings + app/app_assistant.cpp OpenAiSettingsDialog",
     "existing native UI; string fields have no universal native widget maxLength; wire must use explicit "
     "finite bound"},
    {"assistant_preferences.instructions", ParameterGroup::Assistant,
     R"parameter({"type":"string","maxLength":4096})parameter", "none", "dialog_draft_until_OK_then_saved",
     ParameterPermission::AssistantPreference, ParameterAccess::WritableWithPolicy, true,
     "src/ui/ai_settings_dialog.cpp", "Settings + app/app_assistant.cpp OpenAiSettingsDialog",
     "existing native UI; string fields have no universal native widget maxLength; wire must use explicit "
     "finite bound"},
    {"assistant_preferences.explainPrompt", ParameterGroup::Assistant,
     R"parameter({"type":"string","maxLength":4096})parameter", "none", "dialog_draft_until_OK_then_saved",
     ParameterPermission::AssistantPreference, ParameterAccess::WritableWithPolicy, true,
     "src/ui/ai_settings_dialog.cpp", "Settings + app/app_assistant.cpp OpenAiSettingsDialog",
     "existing native UI; string fields have no universal native widget maxLength; wire must use explicit "
     "finite bound"},
    {"assistant_preferences.ttsVoice", ParameterGroup::Assistant,
     R"parameter({"type":"string","maxLength":256,"pattern":"^(system|voice-[a-f0-9]{32})$"})parameter",
     "none", "dialog_draft_until_OK_then_saved", ParameterPermission::AssistantPreference,
     ParameterAccess::WritableWithPolicy, false, "src/ui/ai_settings_dialog.cpp",
     "Settings + app/app_assistant.cpp OpenAiSettingsDialog",
     "installed Windows voice + OKUFLOW_HAS_TTS; missing voice retained honestly"},
    {"assistant_preferences.ttsRate", ParameterGroup::Assistant,
     R"parameter({"type":"number","minimum":-1,"maximum":1})parameter", "normalized_speech_rate",
     "dialog_draft_until_OK_then_saved", ParameterPermission::AssistantPreference,
     ParameterAccess::WritableWithPolicy, false, "src/ui/ai_settings_dialog.cpp",
     "Settings + app/app_assistant.cpp OpenAiSettingsDialog",
     "existing native UI; string fields have no universal native widget maxLength; wire must use explicit "
     "finite bound"},
    {"notes_output.lectureNotes", ParameterGroup::Notes, R"parameter({"type":"boolean"})parameter", "none",
     "dialog_draft_until_OK_then_saved", ParameterPermission::AssistantPreference,
     ParameterAccess::WritableWithPolicy, false, "src/app/app_assistant.cpp",
     "AiSettingsDialog::Settings -> AssistiveFeatureManager::ApplySettings",
     "present_control; effective hardware capability must be reported separately"},
    {"notes_output.root", ParameterGroup::Notes,
     R"parameter({"type":"string","minLength":36,"maxLength":36,"pattern":"^dir-[a-f0-9]{32}$"})parameter",
     "none", "shared_saved", ParameterPermission::OutputDestination, ParameterAccess::WritableWithPolicy,
     true, "src/app/app_controls.cpp", "ChangeUserDataFolder",
     "pending trusted directory-handle integration: Documents/current/owner-approved roots only; safe "
     "labels; no model path"},
    {"ui.mode", ParameterGroup::Ui, R"parameter({"type":"string","enum":["simple","advanced"]})parameter",
     "none", "shared_saved_on_explicit_Save_or_shutdown", ParameterPermission::UiPreference,
     ParameterAccess::WritableWithPolicy, false, "src/ui/main_window.cpp",
     "MainWindow::setSimpleMode + bootstrap lambdas",
     "present_control; effective hardware capability must be reported separately"},
    {"ui.inspectorWidth", ParameterGroup::Ui,
     R"parameter({"type":"integer","minimum":360,"maximum":1200})parameter", "logical_pixels", "shared_saved",
     ParameterPermission::UiPreference, ParameterAccess::WritableWithPolicy, false, "src/ui/main_window.cpp",
     "MainWindow::setAdvancedPanelWidth",
     "preferred width bounded360..1200; effective width further limited by current window"},
    {"ui.language", ParameterGroup::Ui,
     R"parameter({"type":"string","enum":["system","en","tr","de","fr","es","it","pt-BR","ru","uk","ar","hi","ja","ko","zh-Hans","nl","pl","cs","sv","id","vi","th","bn","fa","he","ur"]})parameter",
     "none", "shared_saved", ParameterPermission::UiPreference, ParameterAccess::WritableWithPolicy, false,
     "src/app/app_bootstrap.cpp",
     "applicationLanguageRequested lambda -> "
     "NormalizeLanguagePreference/ResolveLanguagePreference/LanguageManager::SetLanguage",
     "present_control; effective hardware capability must be reported separately"},
    {"ui.hidden", ParameterGroup::Ui, R"parameter({"type":"boolean"})parameter", "none", "session_only",
     ParameterPermission::UiPreference, ParameterAccess::WritableWithPolicy, false, "src/ui/main_window.cpp",
     "MainWindow::setUiHidden", "present_control; effective hardware capability must be reported separately"},
    {"ui.sectionExpanded", ParameterGroup::Ui,
     R"parameter({"type":"object","properties":{"magnification":{"type":"boolean"},"readability":{"type":"boolean"},"textClarity":{"type":"boolean"},"textClarityFine":{"type":"boolean"},"stability":{"type":"boolean"},"screenFix":{"type":"boolean"},"sharpening":{"type":"boolean"},"assistant":{"type":"boolean"},"diagnostics":{"type":"boolean"},"device":{"type":"boolean"},"deviceMore":{"type":"boolean"},"recording":{"type":"boolean"},"notesFiles":{"type":"boolean"},"application":{"type":"boolean"},"aiDownloads":{"type":"boolean"},"troubleshooting":{"type":"boolean"}},"additionalProperties":false})parameter",
     "none", "shared_saved", ParameterPermission::UiPreference, ParameterAccess::WritableWithPolicy, false,
     "src/ui/main_window.cpp", "MainWindow::setSectionStates",
     "only actual makeSection stable ids; unknown keys rejected"},
    {"ui.assistiveDock", ParameterGroup::Ui,
     R"parameter({"type":"string","enum":["floating","left","right"]})parameter", "none", "shared_saved",
     ParameterPermission::UiPreference, ParameterAccess::WritableWithPolicy, false,
     "src/ui/assistive_overlay.cpp", "SetDockPosition",
     "present_control; effective hardware capability must be reported separately"},
    {"ui.assistiveGeometry", ParameterGroup::Ui,
     R"parameter({"type":"object","properties":{"x":{"type":"integer","minimum":-32768,"maximum":32768},"y":{"type":"integer","minimum":-32768,"maximum":32768},"width":{"type":"integer","minimum":1,"maximum":8192},"height":{"type":"integer","minimum":1,"maximum":8192}},"required":["x","y","width","height"],"additionalProperties":false})parameter",
     "logical_pixels_relative_to_render_target", "shared_saved", ParameterPermission::UiPreference,
     ParameterAccess::WritableWithPolicy, false, "src/ui/assistive_overlay.cpp",
     "AssistiveOverlay::RestoreRelativeGeometry / AssistiveFeatureManager::RestoreOverlayGeometry",
     "floating QRect relative to render target; native UpdatePlacement work-area checks; wire coordinates "
     "and size require explicit bounded validation"},
    {"annotation.color", ParameterGroup::Annotation,
     R"parameter({"type":"string","pattern":"^#[0-9a-fA-F]{6}$","maxLength":256})parameter", "none",
     "shared_saved", ParameterPermission::AnnotationPreference, ParameterAccess::WritableWithPolicy, false,
     "src/ui/annotation_overlay.cpp",
     "AnnotationOverlay::SetPreferences -> PreferencesChanged -> bootstrap save lambda",
     "UI six preset colors #ff4bd8,#fff000,#55ff55,#00e5ff,#2979ff,#ffffff; SetPreferences accepts valid "
     "QColor RGB"},
    {"annotation.width", ParameterGroup::Annotation,
     R"parameter({"type":"integer","minimum":2,"maximum":24})parameter", "logical_pixels", "shared_saved",
     ParameterPermission::AnnotationPreference, ParameterAccess::WritableWithPolicy, false,
     "src/ui/annotation_overlay.cpp",
     "AnnotationOverlay::SetPreferences -> PreferencesChanged -> bootstrap save lambda",
     "existing native control"},
    {"annotation.textSize", ParameterGroup::Annotation,
     R"parameter({"type":"integer","minimum":12,"maximum":72})parameter", "logical_pixels", "shared_saved",
     ParameterPermission::AnnotationPreference, ParameterAccess::WritableWithPolicy, false,
     "src/ui/annotation_overlay.cpp",
     "AnnotationOverlay::SetPreferences -> PreferencesChanged -> bootstrap save lambda",
     "existing native control"},
    {"annotation.dashed", ParameterGroup::Annotation, R"parameter({"type":"boolean"})parameter", "none",
     "shared_saved", ParameterPermission::AnnotationPreference, ParameterAccess::WritableWithPolicy, false,
     "src/ui/annotation_overlay.cpp",
     "AnnotationOverlay::SetPreferences -> PreferencesChanged -> bootstrap save lambda",
     "existing native control"},
    {"annotation.shape", ParameterGroup::Annotation,
     R"parameter({"type":"string","enum":["rectangle","ellipse"]})parameter", "none", "shared_saved",
     ParameterPermission::AnnotationPreference, ParameterAccess::WritableWithPolicy, false,
     "src/ui/annotation_overlay.cpp",
     "AnnotationOverlay::SetPreferences -> PreferencesChanged -> bootstrap save lambda",
     "existing native control"},
    {"annotation.captureOnExit", ParameterGroup::Annotation, R"parameter({"type":"boolean"})parameter",
     "none", "shared_saved", ParameterPermission::PhotoEgress, ParameterAccess::WritableWithPolicy, false,
     "src/ui/annotation_overlay.cpp",
     "AnnotationOverlay::SetPreferences -> PreferencesChanged -> bootstrap save lambda",
     "existing native control"},
    {"annotation.tool", ParameterGroup::Annotation,
     R"parameter({"type":"string","enum":["pointer","pen","line","shape","text","eraser"]})parameter", "none",
     "session_only", ParameterPermission::AnnotationEdit, ParameterAccess::WritableWithPolicy, false,
     "src/ui/annotation_overlay.cpp", "AnnotationOverlay::SetTool",
     "present_control; effective hardware capability must be reported separately"},
    {"annotation.enabled", ParameterGroup::Annotation, R"parameter({"type":"boolean"})parameter", "none",
     "session_only", ParameterPermission::AnnotationEdit, ParameterAccess::WritableWithPolicy, true,
     "src/ui/annotation_overlay.cpp", "MainWindow::setAnnotationMode",
     "present_control; effective hardware capability must be reported separately"},
    {"ui.setupPromptDeclined", ParameterGroup::Ui, R"parameter({"type":"boolean"})parameter", "none",
     "shared_saved_immediate", ParameterPermission::UiPreference, ParameterAccess::WritableWithPolicy, false,
     "src/app/setup_assistant.cpp",
     "SetupAssistantDialog::DeclinePreferenceChanged -> bootstrap persistence lambda",
     "existing native user control"},
    {"ui.activeTab", ParameterGroup::Ui,
     R"parameter({"type":"string","enum":["image","assistant","settings"]})parameter", "none", "session_only",
     ParameterPermission::UiPreference, ParameterAccess::WritableWithPolicy, false, "src/ui/main_window.cpp",
     "MainWindow advancedTabs_->setCurrentIndex", "existing native user control"},
};
constexpr ProtectedControlDescriptor kProtected[] = {
    {"assistant.controlEnabled", "trusted_ui_master", false},
    {"assistant.externalMcpEnabled", "trusted_ui_master", false},
    {"assistant.internetEnabled", "privilege", false},
    {"assistant.codingEnabled", "privilege", false},
    {"assistant.workspace", "privilege_and_host_path", false},
    {"assistant.codexExecutable", "execution_identity", false},
    {"assistant.apiUrl", "network_destination", false},
    {"assistant.apiKey", "secret", true},
    {"assistant.credentialId", "secret_locator", true},
    {"assistant.attachCurrentView", "frame_grant", false},
    {"assistant.photosGrant", "capture_grant", false},
    {"assistant.recordingGrant", "capture_grant", false},
    {"assistant.externalLeaseBundle", "ui_lease_issuance", false},
    {"assistant.externalLease.frames", "ui_lease_issuance", false},
    {"assistant.externalLease.source", "ui_lease_issuance", false},
    {"assistant.externalLease.photos", "ui_lease_issuance", false},
    {"assistant.externalLease.recording", "ui_lease_issuance", false},
};
constexpr ControlActionDescriptor kActions[] = {
    {"camera.keystone.back", "native-ui-only", "source_exact_approval"},
    {"camera.keystone.next", "native-ui-only", "source_exact_approval"},
    {"camera.acceleration.test", "native-ui-only", "source_exact_approval"},
    {"mode.activate", "existing-tool-runtime-gated", "view_with_per_field_sensitivity"},
    {"mode.save", "native-ui-only", "mode_library_exact_approval"},
    {"mode.reset", "native-ui-only", "mode_reset_exact_approval"},
    {"capture.photo.take", "existing-tool-runtime-gated", "photos"},
    {"capture.recording.start", "existing-tool-runtime-gated", "recording"},
    {"capture.recording.stop", "existing-tool-runtime-gated", "recording_exact_session"},
    {"annotation.snapshot", "native-ui-only", "photos"},
    {"notes.open", "native-ui-only", "trusted_UI"},
    {"output.open", "native-ui-only", "trusted_UI"},
    {"annotation.clear", "native-ui-only", "annotation_edit_and_photos"},
    {"annotation.undo", "native-ui-only", "annotation_edit"},
    {"annotation.redo", "native-ui-only", "annotation_edit"},
    {"annotation.cleanPhoto", "native-ui-only", "photos"},
    {"annotation.selection.edit", "native-ui-only", "annotation_edit"},
    {"assistant.connect", "native-ui-only", "explicit_UI_action"},
    {"assistant.send", "native-ui-only", "explicit_UI_action"},
    {"assistant.read", "native-ui-only", "explicit_UI_action"},
    {"assistant.explain", "native-ui-only", "explicit_UI_action"},
    {"assistant.readAloud", "native-ui-only", "explicit_UI_action"},
    {"assistant.stop", "native-ui-only", "explicit_UI_action"},
    {"assistant.new", "native-ui-only", "explicit_UI_action"},
    {"assistant.history.select", "native-ui-only", "explicit_UI_action"},
    {"assistant.history.rename", "native-ui-only", "explicit_UI_action"},
    {"assistant.history.export", "native-ui-only", "explicit_UI_action"},
    {"assistant.history.delete", "native-ui-only", "explicit_UI_action"},
    {"assistant.capability.test", "native-ui-only", "explicit_UI_action"},
    {"setup.download", "native-ui-only", "explicit_UI_action"},
    {"assistant.ttsPreview", "native-ui-only", "explicit_UI_action"},
    {"consent.approve", "native-ui-only", "explicit_UI_action"},
    {"consent.revoke", "native-ui-only", "explicit_UI_action"},
    {"consent.stopAndUndo", "native-ui-only", "explicit_UI_action"},
    {"assistant.controlConversation.start", "native-ui-only", "explicit_UI_action"},
    {"assistant.controlConversation.sendWithoutControl", "native-ui-only", "explicit_UI_action"},
    {"assistant.externalLease.issue", "native-ui-only", "ui_lease_issuance"},
    {"ui.help.open", "native-ui-only", "explicit_UI_action"},
    {"ui.assistive.dismiss", "native-ui-only", "explicit_UI_action"},
};
std::string Quote(std::string_view value) {
    std::string out = "\"";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += char(ch);
        } else if (ch == '\n')
            out += "\\n";
        else if (ch == '\r')
            out += "\\r";
        else if (ch == '\t')
            out += "\\t";
        else if (ch < 32) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[ch >> 4];
            out += hex[ch & 15];
        } else
            out += char(ch);
    }
    return out + '"';
}
std::string_view ActionGroup(std::string_view id) {
    if (id.starts_with("camera."))
        return "camera";
    if (id.starts_with("capture."))
        return "recording";
    if (id.starts_with("annotation."))
        return "annotation";
    if (id.starts_with("assistant."))
        return "assistant_preferences";
    if (id.starts_with("notes.") || id.starts_with("output."))
        return "notes_output";
    if (id.starts_with("mode."))
        return "image";
    return "ui";
}
std::string Properties(ParameterGroup group, bool writable) {
    std::string out = "{";
    bool first = true;
    for (const auto &field : kParameters) {
        if (field.group != group || (writable && field.access == ParameterAccess::UiOnly))
            continue;
        if (!first)
            out += ',';
        first = false;
        out += Quote(field.id) + ':' + std::string(field.schema);
    }
    return out + '}';
}
} // namespace
std::span<const ParameterDescriptor> ParameterRegistry() { return kParameters; }
const ParameterDescriptor *FindParameter(std::string_view id) {
    for (const auto &p : kParameters)
        if (p.id == id)
            return &p;
    return nullptr;
}
std::span<const ProtectedControlDescriptor> ProtectedControlRegistry() { return kProtected; }
std::span<const ControlActionDescriptor> ControlActionRegistry() { return kActions; }
std::string_view ParameterGroupName(ParameterGroup g) {
    constexpr std::array names{"image",        "view", "camera",    "recording", "assistant_preferences",
                               "notes_output", "ui",   "annotation"};
    const auto i = static_cast<std::size_t>(g);
    return i < names.size() ? names[i] : "invalid";
}
std::string_view ParameterPermissionName(ParameterPermission p) {
    constexpr std::array names{"view",
                               "source",
                               "capture-configuration",
                               "assistant-preference",
                               "provider-configuration",
                               "frame-egress",
                               "output-destination",
                               "ui-preference",
                               "annotation-preference",
                               "annotation-edit",
                               "photo-egress"};
    const auto i = static_cast<std::size_t>(p);
    return i < names.size() ? names[i] : "invalid";
}
bool ParameterGroupForTool(ToolId tool, ParameterGroup &group, bool &setter) {
    if (tool < ToolId::ImageSettingsGet || tool > ToolId::AnnotationPreferencesSet)
        return false;
    const int index = static_cast<int>(tool) - static_cast<int>(ToolId::ImageSettingsGet);
    group = static_cast<ParameterGroup>(index / 2);
    setter = (index % 2) != 0;
    return true;
}
std::string ParameterGetSchema(ParameterGroup group) {
    std::string choices = "[";
    bool first = true;
    for (const auto &p : kParameters)
        if (p.group == group) {
            if (!first)
                choices += ',';
            first = false;
            choices += Quote(p.id);
        }
    choices += ']';
    const auto projection = group == ParameterGroup::Assistant
                                ? ",\"valueKind\":{\"type\":\"string\",\"enum\":[\"all\",\"requested\","
                                  "\"saved\"],\"default\":\"all\"}"
                                : "";
    return "{\"$schema\":\"http://json-schema.org/draft-07/"
           "schema#\",\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"fields\":{"
           "\"type\":\"array\",\"minItems\":1,\"maxItems\":16,\"uniqueItems\":true,\"items\":{\"type\":"
           "\"string\",\"enum\":" +
           choices + "}}" + projection + "}}";
}
std::string ParameterSetSchema(ParameterGroup group) {
    std::size_t count = 0;
    for (const auto &p : kParameters)
        count += p.group == group && p.access != ParameterAccess::UiOnly;
    return "{\"$schema\":\"http://json-schema.org/draft-07/"
           "schema#\",\"type\":\"object\",\"additionalProperties\":false,\"minProperties\":1,"
           "\"maxProperties\":" +
           std::to_string(count) + ",\"properties\":" + Properties(group, true) + '}';
}
std::string ParameterMetadataPage(std::string_view group, std::string_view afterField, std::size_t limit,
                                  CatalogEdition edition) {
    std::vector<std::pair<std::string_view, std::string>> items;
    for (const auto &p : kParameters) {
        const auto g = ParameterGroupName(p.group);
        if (!group.empty() && group != g)
            continue;
        auto dto = "{\"id\":" + Quote(p.id) + ",\"group\":" + Quote(g) +
                   ",\"kind\":\"parameter\",\"schema\":" + std::string(p.schema) +
                   ",\"units\":" + Quote(p.units) + ",\"persistence\":" + Quote(p.persistence) +
                   ",\"permission\":" + Quote(ParameterPermissionName(p.permission)) + ",\"requiresUi\":" +
                   (p.requiresUiApproval || p.access == ParameterAccess::UiOnly ? "true" : "false") +
                   ",\"writable\":" + (p.access == ParameterAccess::UiOnly ? "false" : "true") +
                   ",\"source\":" + Quote(p.nativeSource) + ",\"handler\":" + Quote(p.nativeHandler) +
                   ",\"availability\":" + Quote(p.availability) +
                   ",\"implementation\":\"target-integration-required\"}";
        items.emplace_back(p.id, std::move(dto));
    }
    for (const auto &p : kProtected)
        if (group.empty() || group == "assistant_preferences") {
            items.emplace_back(
                p.id, "{\"id\":" + Quote(p.id) +
                          ",\"group\":\"assistant_preferences\",\"kind\":\"protected\",\"category\":" +
                          Quote(p.category) + ",\"requiresUi\":true,\"writable\":false}");
        }
    for (const auto &action : kActions) {
        const auto g = ActionGroup(action.id);
        if (!group.empty() && group != g)
            continue;
        items.emplace_back(action.id, "{\"id\":" + Quote(action.id) + ",\"group\":" + Quote(g) +
                                          ",\"kind\":\"action\",\"status\":" + Quote(action.status) +
                                          ",\"permission\":" + Quote(action.permission) + ",\"requiresUi\":" +
                                          (action.status == "native-ui-only" ? "true" : "false") + "}");
    }
    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    if (!group.empty() &&
        std::none_of(kParameters, kParameters + std::size(kParameters),
                     [group](const auto &p) { return ParameterGroupName(p.group) == group; }))
        return R"({"ok":false,"error":{"code":"invalid_argument","retryable":false}})";
    if (limit < 1 || limit > 16)
        return R"({"ok":false,"error":{"code":"invalid_argument","retryable":false}})";
    std::size_t offset = 0;
    if (!afterField.empty()) {
        const auto found = std::find_if(items.begin(), items.end(),
                                        [afterField](const auto &p) { return p.first == afterField; });
        if (found == items.end())
            return R"({"ok":false,"error":{"code":"invalid_argument","retryable":false}})";
        offset = static_cast<std::size_t>(found - items.begin()) + 1;
    }
    const auto end = std::min(offset + limit, items.size());
    std::string out = "{\"ok\":true,\"items\":[";
    for (auto i = offset; i < end; ++i) {
        if (i != offset)
            out += ',';
        out += items[i].second;
    }
    out += "],\"catalogEdition\":" + Quote(CatalogEditionName(edition)) +
           ",\"hasMore\":" + std::string(end < items.size() ? "true" : "false");
    if (end > offset && end < items.size())
        out += ",\"afterField\":" + Quote(items[end - 1].first);
    return out + '}';
}
} // namespace okuflow::agent
