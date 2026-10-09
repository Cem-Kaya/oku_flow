#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include "openzoom/app/settings_store.hpp"
#include "openzoom/app/user_data_paths.hpp"
#include "openzoom/common/assistive_runtime.hpp"

#include <QElapsedTimer>
#include <QRect>

#include <cstdint>
#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace openzoom {

class AssistiveOverlay;

// Owns the assistive runtime, floating result overlay, and periodic-analysis
// cadence. The application supplies frame data and the hardware-derived focus
// decision; this class owns all assistive policy after that boundary.
class AssistiveFeatureManager final {
public:
    using QuestionHandler = std::function<void(const QString&)>;
    using NewChatHandler = std::function<void()>;

    AssistiveFeatureManager(QWidget& renderWidget,
                            QObject& runtimeParent,
                            QuestionHandler questionHandler,
                            NewChatHandler newChatHandler,
                            const UserDataPaths& userDataPaths);
    ~AssistiveFeatureManager();

    AssistiveFeatureManager(const AssistiveFeatureManager&) = delete;
    AssistiveFeatureManager& operator=(const AssistiveFeatureManager&) = delete;

    AssistiveRuntime& Runtime();
    const AssistiveRuntime& Runtime() const;
    AssistiveOverlay& Overlay();
    const AssistiveOverlay& Overlay() const;

    void SetModes(bool vlmEnabled, bool overlayEnabled);
    void ApplySettings(const settings::AssistiveSettings& settings);

    bool WantsPeriodicReadback(bool debugViewEnabled) const;
    void MaybeRequestAnalysis(const std::uint8_t* data,
                              unsigned int width,
                              unsigned int height,
                              bool debugViewEnabled,
                              bool focusGateEnabled,
                              bool focusAcceptable);
    void ShowFocusWarning();

    void RestoreOverlayGeometry(const QRect& geometry);
    QRect OverlayGeometry() const;

private:
    AssistiveRuntimeConfig BuildRuntimeConfig(
        const settings::AssistiveSettings& settings) const;
    bool AnalysisDue() const;

    std::unique_ptr<AssistiveRuntime> runtime_;
    AssistiveOverlay* overlay_{};
    QElapsedTimer analysisTimer_;
    const UserDataPaths* userDataPaths_{};
    bool overlayEnabled_{true};
};

} // namespace openzoom

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
