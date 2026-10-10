#ifdef _WIN32

#include "okuflow/ui/ai_settings_dialog.hpp"
#include "okuflow/common/codex_app_server_client.hpp"
#include "okuflow/ui/live_status_text.hpp"
#include "okuflow/ui/ui_translation.hpp"
#include "okuflow/ui/wheel_safe_combo_box.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#if OKUFLOW_HAS_TTS
#include <QTextToSpeech>
#include <QVoice>
#endif

namespace okuflow {

namespace {

#if OKUFLOW_HAS_TTS
QString VoiceLabel(const QVoice& voice)
{
    const QLocale locale = voice.locale();
    const QString language = QLocale::languageToString(locale.language());
    const QString territory = QLocale::territoryToString(locale.territory());
    if (territory.isEmpty() || locale.territory() == QLocale::AnyTerritory) {
        return QStringLiteral("%1 - %2").arg(voice.name(), language);
    }
    return QStringLiteral("%1 - %2 (%3)").arg(voice.name(), language, territory);
}
#endif

QString ReasoningLabel(const QString& effort)
{
    const QString normalized = effort.trimmed().toLower();
    if (normalized == QStringLiteral("xhigh")) {
        return QStringLiteral("Extra high");
    }
    if (normalized == QStringLiteral("none")) {
        return QStringLiteral("None");
    }
    if (normalized == QStringLiteral("minimal")) {
        return QStringLiteral("Minimal");
    }
    if (normalized.isEmpty()) {
        return QStringLiteral("Default");
    }
    if (normalized == QStringLiteral("low")) {
        return QStringLiteral("Low");
    }
    if (normalized == QStringLiteral("medium")) {
        return QStringLiteral("Medium");
    }
    if (normalized == QStringLiteral("high")) {
        return QStringLiteral("High");
    }
    return normalized;
}

} // namespace

AiSettingsDialog::AiSettingsDialog(const settings::AssistiveSettings& initial, QWidget* parent)
    : QDialog(parent),
      initial_(initial)
{
    setWindowTitle("AI Settings");
    // Form rows stack their labels above the fields at narrow widths, so a
    // small minimum stays usable on compact or highly scaled screens.
    setMinimumSize(480, 420);
    QSize preferredSize(760, 820);
    if (QScreen* screen = parent ? parent->screen() : QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        preferredSize = preferredSize.boundedTo(
            QSize(available.width() * 9 / 10, available.height() * 9 / 10));
    }
    resize(preferredSize.expandedTo(minimumSize()));

    // Large-ish fonts and clear focus outlines, matching the main window.
    // Framed groups separate provider, instruction, permission, voice, and
    // notes settings at a glance.
    setStyleSheet(QStringLiteral(R"(
        QWidget { font-size: 12pt; }
        QGroupBox {
            border: 2px solid palette(mid);
            border-radius: 8px;
            margin-top: 14px;
            padding: 16px 12px 12px 12px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            left: 12px;
            padding: 0 6px;
        }
        QLineEdit, QPlainTextEdit, QComboBox {
            padding: 6px;
            border: 2px solid palette(mid);
            border-radius: 6px;
            background: palette(base);
        }
        QLineEdit:focus, QPlainTextEdit:focus, QComboBox:focus {
            border-color: palette(highlight);
        }
        QPushButton { min-height: 32px; padding: 4px 14px; }
        QToolButton#aiSettingsDisclosure,
        QToolButton#aiSettingsDisclosure:checked {
            color: palette(window-text);
            background: transparent;
            border: 3px solid transparent;
            border-radius: 6px;
            padding: 2px 6px;
        }
        QToolButton#aiSettingsDisclosure:hover { background: palette(midlight); }
        QToolButton#aiSettingsDisclosure:focus { border-color: palette(highlight); }
        QCheckBox { spacing: 8px; }
        QCheckBox::indicator { width: 20px; height: 20px; }
    )"));

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setSpacing(12);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setAccessibleName(QStringLiteral("AI settings sections"));

    auto* scrollContent = new QWidget(scrollArea_);
    auto* contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(4, 4, 8, 4);
    contentLayout->setSpacing(14);

    const auto addForm = [this](QGroupBox* group) {
        auto* form = new QFormLayout(group);
        form->setSpacing(10);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        forms_.push_back(form);
        return form;
    };
    const auto pathRow = [](QLineEdit* edit, QPushButton* browseButton) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        row->addWidget(edit, 1);
        row->addWidget(browseButton);
        return row;
    };

    // 1. Where Read, Explain, and Assistant requests are sent.
    auto* providerGroup = new QGroupBox(QStringLiteral("AI service"));
    auto* providerForm = addForm(providerGroup);
    providerCombo_ = new WheelSafeComboBox();
    providerCombo_->addItem("ChatGPT subscription (Codex)", QStringLiteral("codex"));
    providerCombo_->addItem("OpenAI-compatible server", QStringLiteral("openai-compatible"));
    providerCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const int providerIndex = providerCombo_->findData(initial.aiProvider);
    providerCombo_->setCurrentIndex(providerIndex >= 0 ? providerIndex : 0);
    providerForm->addRow("AI provider:", providerCombo_);
    providerHintLabel_ = new QLabel();
    providerHintLabel_->setObjectName(QStringLiteral("aiProviderHint"));
    providerHintLabel_->setTextFormat(Qt::PlainText);
    providerHintLabel_->setWordWrap(true);
    providerForm->addRow(providerHintLabel_);
    contentLayout->addWidget(providerGroup);

    // 2a. Codex connection, shown only for the subscription provider.
    codexGroup_ = new QGroupBox(QStringLiteral("ChatGPT subscription (Codex)"));
    auto* codexForm = addForm(codexGroup_);

    codexModelCombo_ = new WheelSafeComboBox();
    SetComboItemsAreData(codexModelCombo_);
    codexModelCombo_->setMinimumContentsLength(24);
    codexModelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    codexModelCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const QString configuredModel = initial.codexModel.trimmed().isEmpty()
                                        ? QStringLiteral("gpt-6-luna")
                                        : initial.codexModel.trimmed();
    codexModelCombo_->addItem(configuredModel, configuredModel);
    codexForm->addRow("Codex model:", codexModelCombo_);

    codexReasoningCombo_ = new WheelSafeComboBox();
    // Items are relabeled from their effort data in changeEvent; the generic
    // pass would otherwise capture an already translated label as source.
    SetComboItemsAreData(codexReasoningCombo_);
    codexReasoningCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    preferredReasoningEffort_ = initial.codexReasoningEffort.trimmed().toLower();
    for (const QString& effort : {QStringLiteral("low"), QStringLiteral("medium"),
                                  QStringLiteral("high"), QStringLiteral("xhigh")}) {
        codexReasoningCombo_->addItem(TranslateUi(ReasoningLabel(effort)),
                                      effort);
    }
    const int reasoningIndex =
        codexReasoningCombo_->findData(preferredReasoningEffort_);
    codexReasoningCombo_->setCurrentIndex(reasoningIndex >= 0 ? reasoningIndex : 0);
    codexForm->addRow("Reasoning:", codexReasoningCombo_);

    codexPathEdit_ = new QLineEdit(initial.codexExecutablePath);
    codexPathEdit_->setPlaceholderText("Auto-detect codex.exe");
    auto* codexBrowseButton = new QPushButton("Browse...");
    connect(codexBrowseButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, "Select Codex CLI", codexPathEdit_->text(),
            "Codex CLI (codex.exe);;Executables (*.exe);;All Files (*)");
        if (!path.isEmpty()) {
            codexPathEdit_->setText(path);
        }
    });
    codexForm->addRow("Codex CLI path:", pathRow(codexPathEdit_, codexBrowseButton));

    // The fixed OkuFlow prompt is reference material; it stays collapsed so
    // the editable settings remain prominent.
    builtInInstructionsToggle_ = new QToolButton();
    builtInInstructionsToggle_->setObjectName(QStringLiteral("aiSettingsDisclosure"));
    builtInInstructionsToggle_->setText("Built-in Codex Prompt");
    builtInInstructionsToggle_->setCheckable(true);
    builtInInstructionsToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    const Qt::ArrowType collapsedArrow =
        layoutDirection() == Qt::RightToLeft ? Qt::LeftArrow : Qt::RightArrow;
    builtInInstructionsToggle_->setArrowType(collapsedArrow);
    codexForm->addRow(builtInInstructionsToggle_);
    builtInInstructionsEdit_ = new QPlainTextEdit(
        CodexAppServerClient::BuiltInAssistantInstructions());
    builtInInstructionsEdit_->setReadOnly(true);
    builtInInstructionsEdit_->setTextInteractionFlags(Qt::TextSelectableByKeyboard |
                                                      Qt::TextSelectableByMouse);
    builtInInstructionsEdit_->setTabChangesFocus(true);
    builtInInstructionsEdit_->setMaximumHeight(140);
    builtInInstructionsEdit_->setToolTip(
        QStringLiteral("OkuFlow always sends this instruction to Codex, followed by "
                       "your instructions and the permission rules."));
    codexForm->addRow(builtInInstructionsEdit_);
    codexForm->setRowVisible(builtInInstructionsEdit_, false);
    connect(builtInInstructionsToggle_, &QToolButton::toggled, this,
            [this, codexForm, collapsedArrow](bool expanded) {
                builtInInstructionsToggle_->setArrowType(expanded ? Qt::DownArrow
                                                                  : collapsedArrow);
                codexForm->setRowVisible(builtInInstructionsEdit_, expanded);
                if (expanded) {
                    QTimer::singleShot(0, this, [this]() {
                        scrollArea_->ensureWidgetVisible(builtInInstructionsEdit_);
                    });
                }
            });
    contentLayout->addWidget(codexGroup_);

    // 2b. OpenAI-compatible connection, shown only for that provider.
    serverGroup_ = new QGroupBox(QStringLiteral("OpenAI-compatible vision server"));
    auto* serverForm = addForm(serverGroup_);

    apiUrlEdit_ = new QLineEdit(initial.vlmApiUrl);
    apiUrlEdit_->setPlaceholderText(
        "https://api.openai.com/v1/chat/completions or http://localhost:11434/v1/chat/completions");
    serverForm->addRow("Server URL:", apiUrlEdit_);

    apiKeyEdit_ = new QLineEdit(initial.vlmApiKey);
    apiKeyEdit_->setEchoMode(QLineEdit::Password);
    apiKeyEdit_->setPlaceholderText("Optional for local servers");
    serverForm->addRow("API key:", apiKeyEdit_);

    modelEdit_ = new QLineEdit(initial.vlmModel);
    modelEdit_->setPlaceholderText("gpt-4o-mini or llava");
    serverForm->addRow("Vision model:", modelEdit_);
    contentLayout->addWidget(serverGroup_);

    // 3. Answer preferences. Both providers use them, so they never hide.
    auto* instructionsGroup = new QGroupBox(QStringLiteral("Assistant instructions"));
    auto* instructionsForm = addForm(instructionsGroup);

    assistantInstructionsEdit_ = new QPlainTextEdit(initial.assistantInstructions);
    assistantInstructionsEdit_->setPlaceholderText(
        "Example: Always answer in Turkish. Use short sentences and explain technical terms.");
    assistantInstructionsEdit_->setTabChangesFocus(true);
    assistantInstructionsEdit_->setMaximumHeight(120);
    instructionsForm->addRow("Your instructions:", assistantInstructionsEdit_);

    promptEdit_ = new QPlainTextEdit(initial.vlmPrompt);
    promptEdit_->setPlaceholderText(
        "Instructions for the vision model, e.g. describe the lecture slide briefly.");
    promptEdit_->setTabChangesFocus(true);
    promptEdit_->setMaximumHeight(110);
    instructionsForm->addRow("Explain prompt:", promptEdit_);
    contentLayout->addWidget(instructionsGroup);

    // 4. Global Advanced Assistant permissions, Codex only.
    permissionsGroup_ = new QGroupBox(QStringLiteral("Advanced Assistant permissions"));
    auto* permissionsForm = addForm(permissionsGroup_);
    auto* permissionsNote = new QLabel(QStringLiteral(
        "These apply only to saved Assistant conversations, including questions "
        "asked from the floating Assistant. Read and Explain always stay restricted."));
    permissionsNote->setTextFormat(Qt::PlainText);
    permissionsNote->setWordWrap(true);
    permissionsForm->addRow(permissionsNote);

    codexInternetCheckbox_ = new QCheckBox("Allow internet access");
    codexInternetCheckbox_->setChecked(initial.codexInternetEnabled);
    permissionsForm->addRow(codexInternetCheckbox_);

    codexCodingCheckbox_ = new QCheckBox("Allow coding commands and file changes");
    codexCodingCheckbox_->setChecked(initial.codexCodingEnabled);
    permissionsForm->addRow(codexCodingCheckbox_);

    codexWorkspaceEdit_ = new QLineEdit(initial.codexWorkspaceDirectory);
    codexWorkspaceEdit_->setPlaceholderText("Required when coding is enabled");
    codexWorkspaceBrowseButton_ = new QPushButton("Browse...");
    connect(codexWorkspaceBrowseButton_, &QPushButton::clicked, this, [this]() {
        QString initialDirectory = codexWorkspaceEdit_->text().trimmed();
        if (initialDirectory.isEmpty()) {
            initialDirectory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        }
        const QString path = QFileDialog::getExistingDirectory(
            this, "Select Assistant Coding Workspace", initialDirectory,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!path.isEmpty()) {
            codexWorkspaceEdit_->setText(QDir::toNativeSeparators(QDir::cleanPath(path)));
        }
    });
    permissionsForm->addRow("Coding workspace:",
                            pathRow(codexWorkspaceEdit_, codexWorkspaceBrowseButton_));
    contentLayout->addWidget(permissionsGroup_);

    connect(providerCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { UpdateProviderFields(); });
    connect(codexCodingCheckbox_, &QCheckBox::toggled,
            this, [this]() { UpdateProviderFields(); });
    connect(codexModelCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { UpdateCodexReasoningOptions(); });
    connect(codexReasoningCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() {
                preferredReasoningEffort_ =
                    codexReasoningCombo_->currentData().toString();
            });
    UpdateProviderFields();

    preferredVoiceName_ = initial.ttsVoiceName;
    preferredVoiceLocale_ = initial.ttsVoiceLocale;
    ttsEngine_ = initial.ttsEngine;

    // 5. Read Aloud voice. Speech still starts only on request.
    auto* speechGroup = new QGroupBox(QStringLiteral("Read aloud"));
    auto* speechForm = addForm(speechGroup);

    ttsVoiceCombo_ = new WheelSafeComboBox();
    SetComboItemsAreData(ttsVoiceCombo_);
    ttsVoiceCombo_->setMinimumContentsLength(24);
    ttsVoiceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    ttsVoiceCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    speechForm->addRow("Voice:", ttsVoiceCombo_);

    ttsRateSlider_ = new WheelSafeSlider(Qt::Horizontal);
    ttsRateSlider_->setRange(-100, 100);
    ttsRateSlider_->setSingleStep(10);
    ttsRateSlider_->setPageStep(25);
    ttsRateSlider_->setValue(std::clamp(
        static_cast<int>(std::lround(initial.ttsRate * 100.0)), -100, 100));
    ttsRateValueLabel_ = new QLabel();
    ttsRateValueLabel_->setMinimumWidth(105);
    ttsRateValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ttsPreviewButton_ = new QPushButton("Preview");
    ttsPreviewButton_->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));

    auto* speechRateRow = new QHBoxLayout();
    speechRateRow->setSpacing(8);
    speechRateRow->addWidget(ttsRateSlider_, 1);
    speechRateRow->addWidget(ttsRateValueLabel_);
    speechForm->addRow("Speed:", speechRateRow);
    // Preview has its own row so the speed track keeps its width when the
    // dialog is narrow.
    speechForm->addRow(QString(), ttsPreviewButton_);
    contentLayout->addWidget(speechGroup);

    connect(ttsRateSlider_, &QSlider::valueChanged, this, [this]() {
        UpdateSpeechRateLabel();
        ApplySpeechPreviewSettings();
    });
    connect(ttsVoiceCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { ApplySpeechPreviewSettings(); });
    connect(ttsPreviewButton_, &QPushButton::clicked,
            this, &AiSettingsDialog::PreviewSpeech);
    UpdateSpeechRateLabel();

#if OKUFLOW_HAS_TTS
    const QStringList engines = QTextToSpeech::availableEngines();
    auto resolveEngine = [&engines](const QString& requested) {
        for (const QString& engine : engines) {
            if (engine.compare(requested, Qt::CaseInsensitive) == 0) {
                return engine;
            }
        }
        return QString();
    };
    ttsEngine_ = resolveEngine(ttsEngine_);
    if (ttsEngine_.isEmpty()) {
        ttsEngine_ = resolveEngine(QStringLiteral("winrt"));
    }
    if (ttsEngine_.isEmpty()) {
        ttsEngine_ = resolveEngine(QStringLiteral("sapi"));
    }
    speechPreview_ = ttsEngine_.isEmpty() ? new QTextToSpeech(this)
                                          : new QTextToSpeech(ttsEngine_, this);
    ttsEngine_ = speechPreview_->engine();
    PopulateSpeechVoices();
    connect(speechPreview_, &QTextToSpeech::stateChanged, this,
            [this](QTextToSpeech::State state) {
                if (!speechVoicesLoaded_ &&
                    (state == QTextToSpeech::Ready || state == QTextToSpeech::Error)) {
                    PopulateSpeechVoices();
                }
            });
    QTimer::singleShot(0, this, &AiSettingsDialog::PopulateSpeechVoices);
#else
    ttsVoiceCombo_->addItem(
        TranslateUi(QStringLiteral("Text-to-speech is unavailable in this build")));
    ttsVoiceCombo_->setEnabled(false);
    ttsRateSlider_->setEnabled(false);
    ttsPreviewButton_->setEnabled(false);
#endif

    // 6. Lecture notes.
    auto* notesGroup = new QGroupBox(QStringLiteral("Lecture notes"));
    auto* notesLayout = new QVBoxLayout(notesGroup);
    lectureNotesCheckbox_ = new QCheckBox("Write lecture notes file");
    lectureNotesCheckbox_->setChecked(initial.lectureNotesEnabled);
    notesLayout->addWidget(lectureNotesCheckbox_);
    contentLayout->addWidget(notesGroup);
    contentLayout->addStretch(1);

    scrollArea_->setWidget(scrollContent);
    scrollArea_->viewport()->installEventFilter(this);
    outerLayout->addWidget(scrollArea_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        // Only an active Codex coding permission needs a workspace. Hidden
        // Codex preferences stay saved as entered and never block saving an
        // OpenAI-compatible configuration; Codex rechecks the folder at use.
        if (providerCombo_->currentData().toString() == QStringLiteral("codex") &&
            codexCodingCheckbox_->isChecked()) {
            // A relative path would resolve against the app's working folder.
            const QFileInfo workspace(codexWorkspaceEdit_->text().trimmed());
            if (!workspace.isAbsolute() || !workspace.isDir()) {
                QMessageBox::warning(this,
                                     QStringLiteral("Coding Workspace Required"),
                                     QStringLiteral("Choose an existing workspace folder before enabling coding."));
                scrollArea_->ensureWidgetVisible(codexWorkspaceEdit_);
                codexWorkspaceEdit_->setFocus();
                return;
            }
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outerLayout->addWidget(buttons);

    // Keyboard order follows the visual order. Qt's focus navigation skips
    // the hidden provider group and the collapsed built-in prompt.
    const std::array<QWidget*, 22> tabOrder{
        providerCombo_, codexModelCombo_, codexReasoningCombo_, codexPathEdit_,
        codexBrowseButton, builtInInstructionsToggle_, builtInInstructionsEdit_,
        apiUrlEdit_, apiKeyEdit_, modelEdit_, assistantInstructionsEdit_, promptEdit_,
        codexInternetCheckbox_, codexCodingCheckbox_, codexWorkspaceEdit_,
        codexWorkspaceBrowseButton_, ttsVoiceCombo_, ttsRateSlider_, ttsPreviewButton_,
        lectureNotesCheckbox_, buttons->button(QDialogButtonBox::Ok),
        buttons->button(QDialogButtonBox::Cancel)};
    for (size_t index = 1; index < tabOrder.size(); ++index) {
        if (tabOrder[index - 1] && tabOrder[index]) {
            QWidget::setTabOrder(tabOrder[index - 1], tabOrder[index]);
        }
    }

    auto setA11y = [](QWidget* widget, const QString& name, const QString& description) {
        widget->setAccessibleName(name);
        widget->setAccessibleDescription(description);
    };
    setA11y(providerCombo_, "AI Provider",
            "Choose ChatGPT through Codex or an OpenAI-compatible server");
    setA11y(codexPathEdit_, "Codex CLI Path",
            "Optional location of codex.exe; leave blank for automatic detection");
    setA11y(codexBrowseButton, "Browse for Codex CLI",
            "Pick the Codex command line executable from disk");
    setA11y(codexModelCombo_, "Codex Model",
            "Choose an image-capable model reported by the connected Codex app server");
    setA11y(codexReasoningCombo_, "Codex Reasoning",
            "Choose how much reasoning Codex uses. Higher levels can take longer to answer.");
    setA11y(codexInternetCheckbox_, "Allow Assistant Internet Access",
            "Allow persistent Advanced Assistant conversations to use web search and network access");
    setA11y(codexCodingCheckbox_, "Allow Assistant Coding",
            "Allow persistent Advanced Assistant conversations to run commands and change files in the selected workspace");
    setA11y(codexWorkspaceEdit_, "Assistant Coding Workspace",
            "Folder that Codex may inspect and modify when coding is enabled");
    setA11y(codexWorkspaceBrowseButton_, "Browse for Coding Workspace",
            "Choose the folder available to Codex coding commands and file changes");
    setA11y(assistantInstructionsEdit_, "Assistant Instructions",
            "Set the response language, tone, detail, and other response preferences");
    setA11y(builtInInstructionsToggle_, "Built-in Codex Prompt",
            "Show or hide the read-only instruction OkuFlow always sends to Codex");
    setA11y(builtInInstructionsEdit_, "Built-in Codex Prompt",
            "Read-only OkuFlow instruction always sent to Codex before your instructions");
    setA11y(apiUrlEdit_, "VLM Server URL",
            "Address of the OpenAI-compatible chat completions endpoint");
    setA11y(apiKeyEdit_, "API Key",
            "Secret key for the vision server, optional for local servers");
    setA11y(modelEdit_, "Model",
            "Name of the vision model, for example gpt-4o-mini or llava");
    setA11y(promptEdit_, "Explain Prompt",
            "Optional instructions sent with the camera view when you press Explain");
    setA11y(ttsVoiceCombo_, "Read Aloud Voice",
            "Choose an installed Windows voice for the Read Aloud button");
    setA11y(ttsRateSlider_, "Read Aloud Speed",
            "Adjust how slowly or quickly OkuFlow reads a result");
    setA11y(ttsPreviewButton_, "Preview Read Aloud Voice",
            "Speak a short sample with the selected voice and speed");
    setA11y(lectureNotesCheckbox_, "Write Lecture Notes File",
            "Append text readings and scene descriptions to lecture notes");
}

void AiSettingsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (!event || event->type() != QEvent::LanguageChange) {
        return;
    }

    const QString selectedEffort =
        codexReasoningCombo_->currentData().toString();
    for (int i = 0; i < codexReasoningCombo_->count(); ++i) {
        codexReasoningCombo_->setItemText(
            i, TranslateUi(ReasoningLabel(
                   codexReasoningCombo_->itemData(i).toString())));
    }
    const int selectedIndex =
        codexReasoningCombo_->findData(selectedEffort);
    if (selectedIndex >= 0) {
        codexReasoningCombo_->setCurrentIndex(selectedIndex);
    }
    UpdateSpeechRateLabel();
    PopulateSpeechVoices();
    // Translated labels change width; recheck whether rows still fit.
    UpdateFormWrapping();
}

bool AiSettingsDialog::eventFilter(QObject* watched, QEvent* event)
{
    // The viewport, not the dialog, reports the width the groups actually
    // receive, including the initial show and vertical scroll bar changes.
    if (scrollArea_ && watched == scrollArea_->viewport() &&
        event->type() == QEvent::Resize) {
        UpdateFormWrapping();
    }
    return QDialog::eventFilter(watched, event);
}

void AiSettingsDialog::UpdateFormWrapping()
{
    if (!scrollArea_) {
        return;
    }
    int labelWidth = 0;
    for (QFormLayout* form : std::as_const(forms_)) {
        for (int row = 0; row < form->rowCount(); ++row) {
            const QLayoutItem* item = form->itemAt(row, QFormLayout::LabelRole);
            if (item && item->widget()) {
                labelWidth = std::max(labelWidth, item->widget()->sizeHint().width());
            }
        }
    }
    // Side-by-side rows need the widest label plus a comfortably wide field.
    // Otherwise every label moves above its field, so all groups stay aligned
    // and long voice, model, and path values keep their full width.
    constexpr int kMinimumFieldWidth = 320;
    constexpr int kGroupChrome = 64;
    const bool stacked = scrollArea_->viewport()->width() <
                         labelWidth + kMinimumFieldWidth + kGroupChrome;
    const QFormLayout::RowWrapPolicy policy =
        stacked ? QFormLayout::WrapAllRows : QFormLayout::DontWrapRows;
    for (QFormLayout* form : std::as_const(forms_)) {
        if (form->rowWrapPolicy() != policy) {
            form->setRowWrapPolicy(policy);
        }
    }
}

settings::AssistiveSettings AiSettingsDialog::result() const
{
    // Start from the initial values so the Credential Manager id survives;
    // SettingsController needs it to replace or delete the stored API key.
    settings::AssistiveSettings out = initial_;
    out.aiProvider = providerCombo_->currentData().toString();
    out.codexExecutablePath = codexPathEdit_->text().trimmed();
    out.codexModel = codexModelCombo_->currentData().toString().trimmed();
    out.codexReasoningEffort = codexReasoningCombo_->currentData().toString();
    out.codexInternetEnabled = codexInternetCheckbox_->isChecked();
    out.codexCodingEnabled = codexCodingCheckbox_->isChecked();
    out.codexWorkspaceDirectory = QDir::toNativeSeparators(
        QDir::cleanPath(codexWorkspaceEdit_->text().trimmed()));
    if (codexWorkspaceEdit_->text().trimmed().isEmpty()) {
        out.codexWorkspaceDirectory.clear();
    }
    out.assistantInstructions = assistantInstructionsEdit_->toPlainText().trimmed();
    out.vlmApiUrl = apiUrlEdit_->text().trimmed();
    out.vlmApiKey = apiKeyEdit_->text();
    out.vlmModel = modelEdit_->text().trimmed();
    out.vlmPrompt = promptEdit_->toPlainText();
    out.ttsEngine = ttsEngine_;
    out.ttsRate = static_cast<double>(ttsRateSlider_->value()) / 100.0;
#if OKUFLOW_HAS_TTS
    const QVoice voice = ttsVoiceCombo_->currentData().value<QVoice>();
    out.ttsVoiceName = voice.name();
    out.ttsVoiceLocale = voice.locale().name();
#else
    out.ttsVoiceName = preferredVoiceName_;
    out.ttsVoiceLocale = preferredVoiceLocale_;
#endif
    out.lectureNotesEnabled = lectureNotesCheckbox_->isChecked();
    return out;
}

void AiSettingsDialog::PopulateSpeechVoices()
{
#if OKUFLOW_HAS_TTS
    if (!speechPreview_ || populatingSpeechVoices_) {
        return;
    }
    QScopedValueRollback<bool> populationGuard(populatingSpeechVoices_, true);

    // availableVoices() is limited to the active locale. The settings picker
    // needs every voice the selected Windows engine exposes.
    QList<QVoice> voices = speechPreview_->findVoices();
    const QTextToSpeech::State state = speechPreview_->state();
    if (voices.isEmpty() && speechPreview_->engine().compare(
                                QStringLiteral("winrt"), Qt::CaseInsensitive) == 0 &&
        state == QTextToSpeech::Error) {
        const QStringList engines = QTextToSpeech::availableEngines();
        for (const QString& engine : engines) {
            if (engine.compare(QStringLiteral("sapi"), Qt::CaseInsensitive) == 0 &&
                speechPreview_->setEngine(engine)) {
                ttsEngine_ = speechPreview_->engine();
                voices = speechPreview_->findVoices();
                break;
            }
        }
    }
    const QLocale appLocale;
    std::sort(voices.begin(), voices.end(),
              [&appLocale](const QVoice& lhs, const QVoice& rhs) {
        const auto rank = [&appLocale](const QVoice& voice) {
            if (voice.locale().language() != appLocale.language()) {
                return 2;
            }
            return voice.locale().territory() == appLocale.territory()
                       ? 0
                       : 1;
        };
        const int lhsRank = rank(lhs);
        const int rhsRank = rank(rhs);
        if (lhsRank != rhsRank) {
            return lhsRank < rhsRank;
        }
        return VoiceLabel(lhs).localeAwareCompare(VoiceLabel(rhs)) < 0;
    });

    QSignalBlocker blocker(ttsVoiceCombo_);
    ttsVoiceCombo_->clear();
    int selectedIndex = -1;
    for (const QVoice& voice : voices) {
        const int index = ttsVoiceCombo_->count();
        ttsVoiceCombo_->addItem(VoiceLabel(voice), QVariant::fromValue(voice));
        const bool nameMatches = voice.name() == preferredVoiceName_;
        const bool localeMatches = preferredVoiceLocale_.isEmpty() ||
                                   voice.locale().name() == preferredVoiceLocale_;
        if (selectedIndex < 0 && nameMatches && localeMatches) {
            selectedIndex = index;
        }
    }

    speechVoicesLoaded_ = !voices.isEmpty();
    if (!speechVoicesLoaded_) {
        const bool stillLoading = speechPreview_->state() != QTextToSpeech::Ready &&
                                  speechPreview_->state() != QTextToSpeech::Error;
        ttsVoiceCombo_->addItem(
            stillLoading
                ? TranslateUi(QStringLiteral("Loading installed Windows voices..."))
                : TranslateUi(QStringLiteral("No installed Windows voices found")));
        ttsVoiceCombo_->setEnabled(false);
        ttsPreviewButton_->setEnabled(false);
        return;
    }

    if (selectedIndex < 0) {
        const QVoice currentVoice = speechPreview_->voice();
        for (int i = 0; i < ttsVoiceCombo_->count(); ++i) {
            if (ttsVoiceCombo_->itemData(i).value<QVoice>() == currentVoice) {
                selectedIndex = i;
                break;
            }
        }
    }
    ttsVoiceCombo_->setEnabled(true);
    ttsPreviewButton_->setEnabled(true);
    ttsVoiceCombo_->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    const QVoice selectedVoice = ttsVoiceCombo_->currentData().value<QVoice>();
    preferredVoiceName_ = selectedVoice.name();
    preferredVoiceLocale_ = selectedVoice.locale().name();
    ApplySpeechPreviewSettings();
#else
    // The build-time placeholder is the combo's only entry; changeEvent
    // routes LanguageChange here, so re-resolve its translation (the combo
    // is marked as data, which exempts it from the automatic item pass).
    if (ttsVoiceCombo_ && ttsVoiceCombo_->count() == 1) {
        ttsVoiceCombo_->setItemText(
            0,
            TranslateUi(QStringLiteral(
                "Text-to-speech is unavailable in this build")));
    }
#endif
}

void AiSettingsDialog::UpdateSpeechRateLabel()
{
    const int rate = ttsRateSlider_->value();
    QString text;
    if (rate == 0) {
        text = TranslateUi(QStringLiteral("Normal"));
    } else if (rate < 0) {
        text = TranslateUi(QStringLiteral("%1% slower")).arg(-rate);
    } else {
        text = TranslateUi(QStringLiteral("%1% faster")).arg(rate);
    }
    SetLiveText(ttsRateValueLabel_, text, LivePoliteness::kSilent,
                TranslateUi(QStringLiteral("Speech rate")));
}

void AiSettingsDialog::ApplySpeechPreviewSettings()
{
#if OKUFLOW_HAS_TTS
    if (!speechPreview_ || !speechVoicesLoaded_) {
        return;
    }
    speechPreview_->setRate(static_cast<double>(ttsRateSlider_->value()) / 100.0);
    const QVoice voice = ttsVoiceCombo_->currentData().value<QVoice>();
    if (!voice.name().isEmpty()) {
        speechPreview_->setVoice(voice);
        preferredVoiceName_ = voice.name();
        preferredVoiceLocale_ = voice.locale().name();
    }
#endif
}

void AiSettingsDialog::PreviewSpeech()
{
#if OKUFLOW_HAS_TTS
    if (!speechPreview_) {
        return;
    }
    ApplySpeechPreviewSettings();
    speechPreview_->stop();
    speechPreview_->say(TranslateUi(QStringLiteral(
        "OkuFlow will read this result using the selected voice.")));
#endif
}

void AiSettingsDialog::UpdateProviderFields()
{
    const bool codex = providerCombo_->currentData().toString() == QStringLiteral("codex");
    // Show only the selected provider's connection and permission settings.
    // Instructions and the Explain prompt stay visible because both providers
    // use them. Hidden values are kept and saved unchanged.
    codexGroup_->setVisible(codex);
    permissionsGroup_->setVisible(codex);
    serverGroup_->setVisible(!codex);
    const bool coding = codex && codexCodingCheckbox_->isChecked();
    codexWorkspaceEdit_->setEnabled(coding);
    codexWorkspaceBrowseButton_->setEnabled(coding);
    SetLiveText(providerHintLabel_,
                codex ? QStringLiteral(
                            "Read, Explain, and Assistant use your ChatGPT subscription "
                            "through the Codex CLI. Sign in with Connect ChatGPT on the "
                            "Assistant tab in Advanced mode.")
                      : QStringLiteral(
                            "Read and Explain send the camera view to the server URL "
                            "below. Images stay on this computer only if that server "
                            "runs on it, such as LM Studio or Ollama at localhost. Saved "
                            "Assistant conversations need the ChatGPT subscription."),
                LivePoliteness::kSilent);
}

void AiSettingsDialog::SetCodexModelCatalog(const QJsonArray& models,
                                            const QString& selectedModel)
{
    codexModelCatalog_ = models;
    QString wantedModel = codexModelCombo_->currentData().toString();
    if (wantedModel.isEmpty()) {
        wantedModel = selectedModel.trimmed();
    }

    QSignalBlocker blocker(codexModelCombo_);
    codexModelCombo_->clear();
    int selectedIndex = -1;
    for (const QJsonValue& value : codexModelCatalog_) {
        const QJsonObject model = value.toObject();
        const QString id = model.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty()) {
            continue;
        }
        const QString displayName =
            model.value(QStringLiteral("displayName")).toString().trimmed();
        const QString label = displayName.isEmpty() || displayName == id
                                  ? id
                                  : QStringLiteral("%1 (%2)").arg(displayName, id);
        const int index = codexModelCombo_->count();
        codexModelCombo_->addItem(label, id);
        codexModelCombo_->setItemData(
            index,
            QStringLiteral("Model ID: %1").arg(id),
            Qt::ToolTipRole);
        if (id == wantedModel ||
            (selectedIndex < 0 && wantedModel.isEmpty() && id == selectedModel)) {
            selectedIndex = index;
        }
    }

    if (!wantedModel.isEmpty() && codexModelCombo_->findData(wantedModel) < 0) {
        codexModelCombo_->addItem(
            QStringLiteral("%1 (current setting; unavailable in catalog)")
                .arg(wantedModel),
            wantedModel);
        selectedIndex = codexModelCombo_->count() - 1;
    }
    if (codexModelCombo_->count() == 0) {
        const QString fallback = wantedModel.isEmpty()
                                     ? QStringLiteral("gpt-6-luna")
                                     : wantedModel;
        codexModelCombo_->addItem(fallback, fallback);
        selectedIndex = 0;
    }
    codexModelCombo_->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    blocker.unblock();
    UpdateCodexReasoningOptions();
}

void AiSettingsDialog::UpdateCodexReasoningOptions()
{
    const QString modelId = codexModelCombo_->currentData().toString();
    QJsonObject selectedModel;
    for (const QJsonValue& value : codexModelCatalog_) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("id")).toString() == modelId) {
            selectedModel = candidate;
            break;
        }
    }

    struct EffortOption {
        QString effort;
        QString description;
    };
    QList<EffortOption> options;
    const QJsonArray supported =
        selectedModel.value(QStringLiteral("supportedReasoningEfforts")).toArray();
    for (const QJsonValue& value : supported) {
        QString effort;
        QString description;
        if (value.isString()) {
            effort = value.toString();
        } else {
            const QJsonObject object = value.toObject();
            effort = object.value(QStringLiteral("reasoningEffort")).toString();
            description = object.value(QStringLiteral("description")).toString();
        }
        effort = effort.trimmed().toLower();
        if (!effort.isEmpty()) {
            options.push_back({effort, description});
        }
    }
    if (options.isEmpty()) {
        for (const QString& effort : {QStringLiteral("low"),
                                      QStringLiteral("medium"),
                                      QStringLiteral("high"),
                                      QStringLiteral("xhigh")}) {
            options.push_back({effort, {}});
        }
    }

    QString wantedEffort = preferredReasoningEffort_;
    const QString defaultEffort =
        selectedModel.value(QStringLiteral("defaultReasoningEffort"))
            .toString()
            .trimmed()
            .toLower();
    QSignalBlocker blocker(codexReasoningCombo_);
    codexReasoningCombo_->clear();
    for (const EffortOption& option : options) {
        const int index = codexReasoningCombo_->count();
        codexReasoningCombo_->addItem(TranslateUi(ReasoningLabel(option.effort)),
                                      option.effort);
        if (!option.description.isEmpty()) {
            codexReasoningCombo_->setItemData(index,
                                              option.description,
                                              Qt::ToolTipRole);
        }
    }
    int index = codexReasoningCombo_->findData(wantedEffort);
    if (index < 0) {
        index = codexReasoningCombo_->findData(defaultEffort);
    }
    codexReasoningCombo_->setCurrentIndex(index >= 0 ? index : 0);
    preferredReasoningEffort_ =
        codexReasoningCombo_->currentData().toString();
}

} // namespace okuflow

#endif // _WIN32
