#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QWidget>
#include "okuflow/common/uct_model_settings.hpp"

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
QT_END_NAMESPACE

namespace okuflow {

// Native, screen-reader accessible Ultra Clear Text controls: an opt-in
// switch, an always-available "show original" comparison switch, a polite
// status line and a selectable read-only pane with the accepted text.
class UltraClearTextPanel final : public QWidget {
    Q_OBJECT
public:
    explicit UltraClearTextPanel(QWidget* parent = nullptr);

    QCheckBox* enableCheckbox() const { return enableCheckbox_; }
    QCheckBox* showOriginalCheckbox() const { return showOriginalCheckbox_; }
    QLabel* statusLabel() const { return statusLabel_; }
    QPlainTextEdit* readingPane() const { return readingPane_; }
    QComboBox* gpuDetectorCombo() const { return gpuDetectorCombo_; }
    QComboBox* gpuRecognizerCombo() const { return gpuRecognizerCombo_; }
    QComboBox* cpuDetectorCombo() const { return cpuDetectorCombo_; }
    QComboBox* cpuRecognizerCombo() const { return cpuRecognizerCombo_; }
    uct::ModelSettings ModelSettings() const;
    void SetModelSettings(const uct::ModelSettings& models);

    bool IsEnabledByUser() const;
    bool IsShowingOriginal() const;
    // Status is English source text routed through the translation catalog.
    // Visible text and accessible name update together without a timer;
    // important controls announcements, not accessibility-tree freshness.
    void SetStatus(const QString& sourceText, bool important = false);
    // Replaces the pane text only when it changed. The reader's caret and
    // selection (in either direction) are remapped through the unchanged
    // prefix/suffix of the refreshed text, and scroll is restored clamped,
    // so a recognition refresh never loses the reading position.
    void SetReadingText(const QString& text);

signals:
    void enabledChanged(bool enabled);
    void showOriginalChanged(bool showOriginal);
    void modelSettingsChanged();

private:
    void UpdateDependentState();

    QCheckBox* enableCheckbox_{};
    QCheckBox* showOriginalCheckbox_{};
    QLabel* statusLabel_{};
    QPlainTextEdit* readingPane_{};
    QComboBox *gpuDetectorCombo_{}, *gpuRecognizerCombo_{}, *cpuDetectorCombo_{}, *cpuRecognizerCombo_{};
    QString statusSource_;
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
