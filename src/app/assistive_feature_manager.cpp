#ifdef _WIN32

#include "okuflow/app/assistive_feature_manager.hpp"

#include "okuflow/ui/main_window.hpp"

#include <QDebug>
#include <QObject>
#include <QWidget>

#include <utility>

namespace okuflow {

namespace {

constexpr qint64 kAssistiveIntervalMs = 1600;
const QString kFocusWarning =
    QStringLiteral("Image out of focus. Tap the phone screen to refocus before reading text.");

} // namespace

AssistiveFeatureManager::AssistiveFeatureManager(QWidget& renderWidget,
                                                 QObject& runtimeParent,
                                                 QuestionHandler questionHandler,
                                                 NewChatHandler newChatHandler,
                                                 const UserDataPaths& userDataPaths)
    : runtime_(std::make_unique<AssistiveRuntime>(&runtimeParent)),
      overlay_(new AssistiveOverlay(&renderWidget)),
      userDataPaths_(&userDataPaths) {
    QObject::connect(runtime_.get(), &AssistiveRuntime::OverlayUpdated,
                     overlay_, [this](const QString& title,
                                      const QString& body,
                                      bool visible) {
                         focusWarningShown_ = false;
                         overlay_->SetContent(title, body, visible && overlayEnabled_);
                     });
    QObject::connect(overlay_, &AssistiveOverlay::Dismissed,
                     runtime_.get(), &AssistiveRuntime::DismissOverlay);
    QObject::connect(overlay_, &AssistiveOverlay::ReadAloudRequested,
                     runtime_.get(), &AssistiveRuntime::ReadAloud);
    QObject::connect(overlay_, &AssistiveOverlay::QuestionSubmitted,
                     overlay_, [handler = std::move(questionHandler)](const QString& question) {
                         if (handler) {
                             handler(question);
                         }
                     });
    QObject::connect(overlay_, &AssistiveOverlay::NewChatRequested,
                     overlay_, [handler = std::move(newChatHandler)]() {
                         if (handler) {
                             handler();
                         }
                     });
    analysisTimer_.invalidate();
}

AssistiveFeatureManager::~AssistiveFeatureManager() {
    // AssistiveRuntime may synchronously finish child processes while it is
    // destroyed. Disconnect the lambda that captures this manager before that
    // shutdown can emit another overlay update.
    QObject::disconnect(runtime_.get(), nullptr, overlay_, nullptr);
}

AssistiveRuntime& AssistiveFeatureManager::Runtime() {
    return *runtime_;
}

const AssistiveRuntime& AssistiveFeatureManager::Runtime() const {
    return *runtime_;
}

AssistiveOverlay& AssistiveFeatureManager::Overlay() {
    return *overlay_;
}

const AssistiveOverlay& AssistiveFeatureManager::Overlay() const {
    return *overlay_;
}

void AssistiveFeatureManager::SetModes(bool vlmEnabled,
                                       bool overlayEnabled) {
    overlayEnabled_ = overlayEnabled;
    runtime_->SetModes(vlmEnabled && overlayEnabled_);
    // Runtime emits OverlayUpdated synchronously; SetContent applies Hide UI
    // suppression and remains the single authority for visibility.
}

void AssistiveFeatureManager::ApplySettings(const settings::AssistiveSettings& settings) {
    runtime_->SetConfig(BuildRuntimeConfig(settings));
}

bool AssistiveFeatureManager::WantsPeriodicReadback(bool debugViewEnabled) const {
    return overlayEnabled_ &&
           !debugViewEnabled &&
           runtime_->WantsAnalysis() &&
           !runtime_->IsBusy() &&
           AnalysisDue();
}

void AssistiveFeatureManager::MaybeRequestAnalysis(const std::uint8_t* data,
                                                   unsigned int width,
                                                   unsigned int height,
                                                   bool debugViewEnabled,
                                                   bool focusGateEnabled,
                                                   bool focusAcceptable) {
    if (!WantsPeriodicReadback(debugViewEnabled) || !data || width == 0 || height == 0) {
        return;
    }
    if (focusGateEnabled && !focusAcceptable) {
        ShowFocusWarning();
        return;
    }

    analysisTimer_.restart();
    runtime_->SubmitFrame(data, static_cast<int>(width), static_cast<int>(height));
}

void AssistiveFeatureManager::ShowFocusWarning() {
    focusWarningShown_ = true;
    overlay_->SetContent(QStringLiteral("Focus"), kFocusWarning, true);
}

void AssistiveFeatureManager::ClearFocusWarning() {
    if (!focusWarningShown_) return;
    focusWarningShown_ = false;
    overlay_->SetContent({}, {}, false);
}

void AssistiveFeatureManager::RestoreOverlayGeometry(const QRect& geometry) {
    if (geometry.isValid()) {
        overlay_->RestoreRelativeGeometry(geometry);
    }
}

QRect AssistiveFeatureManager::OverlayGeometry() const {
    return overlay_->RelativeGeometry();
}

AssistiveRuntimeConfig AssistiveFeatureManager::BuildRuntimeConfig(
    const settings::AssistiveSettings& assistive) const {
    AssistiveRuntimeConfig cfg;
    cfg.aiProvider = assistive.aiProvider;
    cfg.codexExecutablePath = assistive.codexExecutablePath;
    cfg.codexModel = assistive.codexModel;
    cfg.codexReasoningEffort = assistive.codexReasoningEffort;
    cfg.codexInternetEnabled = assistive.codexInternetEnabled;
    cfg.codexCodingEnabled = assistive.codexCodingEnabled;
    cfg.codexWorkspaceDirectory = assistive.codexWorkspaceDirectory;
    cfg.assistantInstructions = assistive.assistantInstructions;
    cfg.vlmApiUrl = assistive.vlmApiUrl;
    cfg.vlmApiKey = assistive.vlmApiKey;
    cfg.vlmModel = assistive.vlmModel;
    cfg.vlmPrompt = assistive.vlmPrompt;
    cfg.ttsEngine = assistive.ttsEngine;
    cfg.ttsVoiceName = assistive.ttsVoiceName;
    cfg.ttsVoiceLocale = assistive.ttsVoiceLocale;
    cfg.ttsRate = assistive.ttsRate;
    cfg.lectureNotesEnabled = assistive.lectureNotesEnabled;
    QString notesError;
    cfg.notesDirectory = userDataPaths_
                             ? userDataPaths_->Notes(&notesError)
                             : QString();
    if (!notesError.isEmpty()) {
        qWarning() << notesError;
    }
    return cfg;
}

bool AssistiveFeatureManager::AnalysisDue() const {
    return !analysisTimer_.isValid() ||
           analysisTimer_.elapsed() >= kAssistiveIntervalMs;
}

} // namespace okuflow

#endif
