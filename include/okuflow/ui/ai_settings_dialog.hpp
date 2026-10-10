#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QDialog>
#include <QJsonArray>
#include <QList>

#include "okuflow/app/settings_store.hpp"

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QEvent;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSlider;
class QToolButton;
#if OKUFLOW_HAS_TTS
class QTextToSpeech;
#endif
QT_END_NAMESPACE

namespace okuflow {

// Modal editor for Codex subscription permissions and OpenAI-compatible
// assistive configuration, including local servers such as LM Studio or
// Ollama so image-to-text can run fully offline. Only the selected
// provider's groups are shown, and hidden values are still saved unchanged;
// instructions, Read Aloud, and lecture notes apply to both providers.
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
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void UpdateProviderFields();
    void UpdateFormWrapping();
    void PopulateSpeechVoices();
    void UpdateSpeechRateLabel();
    void ApplySpeechPreviewSettings();
    void PreviewSpeech();
    void UpdateCodexReasoningOptions();

    // Fields the dialog does not edit, such as the protected credential id,
    // are carried through result() unchanged.
    okuflow::settings::AssistiveSettings initial_;
    QScrollArea* scrollArea_{};
    QList<QFormLayout*> forms_;
    QLabel* providerHintLabel_{};
    QGroupBox* codexGroup_{};
    QGroupBox* permissionsGroup_{};
    QGroupBox* serverGroup_{};
    QComboBox* providerCombo_{};
    QLineEdit* codexPathEdit_{};
    QComboBox* codexModelCombo_{};
    QComboBox* codexReasoningCombo_{};
    QCheckBox* codexInternetCheckbox_{};
    QCheckBox* codexCodingCheckbox_{};
    QLineEdit* codexWorkspaceEdit_{};
    QPushButton* codexWorkspaceBrowseButton_{};
    QPlainTextEdit* assistantInstructionsEdit_{};
    QToolButton* builtInInstructionsToggle_{};
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
