#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QDialog>
#include <QJsonArray>

#include "okuflow/app/settings_store.hpp"

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
#if OKUFLOW_HAS_TTS
class QTextToSpeech;
#endif
QT_END_NAMESPACE

namespace okuflow {

// Modal editor for Codex subscription permissions and OpenAI-compatible
// assistive configuration, including local servers such as LM Studio or
// Ollama so image-to-text can run fully offline.
class AiSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AiSettingsDialog(const okuflow::settings::AssistiveSettings& initial,
                              QWidget* parent = nullptr);

    okuflow::settings::AssistiveSettings result() const;
    void SetCodexModelCatalog(const QJsonArray& models,
                              const QString& selectedModel);

protected:
    void changeEvent(QEvent* event) override;

private:
    void UpdateProviderFields();
    void PopulateSpeechVoices();
    void UpdateSpeechRateLabel();
    void ApplySpeechPreviewSettings();
    void PreviewSpeech();
    void UpdateCodexReasoningOptions();

    QComboBox* providerCombo_{};
    QLineEdit* codexPathEdit_{};
    QComboBox* codexModelCombo_{};
    QComboBox* codexReasoningCombo_{};
    QCheckBox* codexInternetCheckbox_{};
    QCheckBox* codexCodingCheckbox_{};
    QLineEdit* codexWorkspaceEdit_{};
    QPushButton* codexWorkspaceBrowseButton_{};
    QPlainTextEdit* assistantInstructionsEdit_{};
    QPlainTextEdit* builtInInstructionsEdit_{};
    QJsonArray codexModelCatalog_;
    QString preferredReasoningEffort_;
    QLineEdit* apiUrlEdit_{};
    QLineEdit* apiKeyEdit_{};
    QLineEdit* modelEdit_{};
    QPlainTextEdit* promptEdit_{};
    QComboBox* ttsVoiceCombo_{};
    QSlider* ttsRateSlider_{};
    QLabel* ttsRateValueLabel_{};
    QPushButton* ttsPreviewButton_{};
    QString ttsEngine_;
    QString preferredVoiceName_;
    QString preferredVoiceLocale_;
    bool speechVoicesLoaded_{false};
    bool populatingSpeechVoices_{false};
#if OKUFLOW_HAS_TTS
    QTextToSpeech* speechPreview_{};
#endif
    QCheckBox* lectureNotesCheckbox_{};
};

} // namespace okuflow

#endif // _WIN32
