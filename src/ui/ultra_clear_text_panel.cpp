#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include "okuflow/common/ultra_clear_text.hpp"

#if defined(_WIN32)

#include "okuflow/ui/live_status_text.hpp"
#include "okuflow/ui/ui_translation.hpp"
#include "okuflow/ui/wheel_safe_combo_box.hpp"

#include <QCheckBox>
#include <QApplication>
#include <QAccessible>
#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCursor>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace okuflow {
namespace {

// Maps one document offset from the previous pane text onto refreshed text
// through their common prefix and suffix. Offsets in the unchanged prefix
// keep their value, offsets in the unchanged suffix shift by the length
// delta, and offsets inside the replaced middle keep their relative
// position clamped to the new middle. The mapping is monotonic, so mapping
// anchor and position independently preserves selection direction.
int RemapReadingOffset(int offset, qsizetype prefix, qsizetype suffix,
                       qsizetype previousLength, qsizetype newLength)
{
    if (offset <= prefix) {
        return offset;
    }
    if (offset >= previousLength - suffix) {
        return static_cast<int>(offset + (newLength - previousLength));
    }
    const qsizetype newMiddle = newLength - prefix - suffix;
    return static_cast<int>(prefix + std::min<qsizetype>(offset - prefix, newMiddle));
}

} // namespace

UltraClearTextPanel::UltraClearTextPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ultraClearTextPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    enableCheckbox_ = new QCheckBox(QStringLiteral("Ultra Clear Text (experimental)"));
    enableCheckbox_->setObjectName(QStringLiteral("ultraClearTextCheckbox"));
    enableCheckbox_->setChecked(false);
    enableCheckbox_->setAccessibleName(QStringLiteral("Ultra Clear Text (experimental)"));
    enableCheckbox_->setAccessibleDescription(QStringLiteral(
        "Recognizes steady printed text on this computer and draws sharp letters over it. "
        "Recognition can be wrong. Use Show original to compare with the camera image."));
    enableCheckbox_->setToolTip(enableCheckbox_->accessibleDescription());
    layout->addWidget(enableCheckbox_);

    showOriginalCheckbox_ = new QCheckBox(QStringLiteral("Show original camera text"));
    showOriginalCheckbox_->setObjectName(QStringLiteral("ultraClearTextShowOriginal"));
    showOriginalCheckbox_->setAccessibleName(QStringLiteral("Show original camera text"));
    showOriginalCheckbox_->setAccessibleDescription(QStringLiteral(
        "Temporarily hides the sharp letters so you can compare with the camera image."));
    showOriginalCheckbox_->setToolTip(showOriginalCheckbox_->accessibleDescription());
    layout->addWidget(showOriginalCheckbox_);

    auto* advanced = new QToolButton(this);
    advancedToggle_=advanced;
    advanced->setObjectName(QStringLiteral("uctAdvancedSettings"));
    advanced->setText(QStringLiteral("UCT advanced settings"));
    advanced->setAccessibleName(advanced->text());
    advanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced->setArrowType(Qt::RightArrow);
    advanced->setCheckable(true);
    advanced->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    layout->addWidget(advanced);
    auto* modelControls = new QWidget(this);
    modelControls->setObjectName(QStringLiteral("uctAdvancedContents"));
    auto* modelLayout = new QVBoxLayout(modelControls);
    modelLayout->setContentsMargins(12,0,0,0);
    const auto addModel = [&](const QString& label,const char* name) {
        auto* caption=new QLabel(label,modelControls);
        caption->setWordWrap(true);
        caption->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        auto* combo=new WheelSafeComboBox(modelControls);
        combo->setObjectName(QString::fromLatin1(name));
        combo->setAccessibleName(label);
        combo->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        for(const auto tier:{uct::ModelTier::Tiny,uct::ModelTier::Small,uct::ModelTier::Medium}) {
            const auto token=uct::ModelTierToken(tier);
            combo->addItem(QStringLiteral("PP-OCRv6 ")+token,token);
        }
        caption->setBuddy(combo);
        modelLayout->addWidget(caption);modelLayout->addWidget(combo);
        connect(combo,&QComboBox::currentIndexChanged,this,[this]{emit modelSettingsChanged();});
        return combo;
    };
    gpuDetectorCombo_=addModel(QStringLiteral("GPU text detector"),"uctGpuDetector");
    gpuRecognizerCombo_=addModel(QStringLiteral("GPU text recognizer"),"uctGpuRecognizer");
    cpuDetectorCombo_=addModel(QStringLiteral("CPU text detector"),"uctCpuDetector");
    cpuRecognizerCombo_=addModel(QStringLiteral("CPU text recognizer"),"uctCpuRecognizer");
    SetModelSettings({});
    layout->addWidget(modelControls);modelControls->hide();
    connect(advanced,&QToolButton::toggled,this,[advanced,modelControls](bool expanded){
        if(!expanded && modelControls->isAncestorOf(QApplication::focusWidget()))
            advanced->setFocus(Qt::OtherFocusReason);
        advanced->setArrowType(expanded?Qt::DownArrow:Qt::RightArrow);
        modelControls->setVisible(expanded);
    });

    backendLabel_ = new QLabel(this);
    backendLabel_->setObjectName(QStringLiteral("ultraClearTextBackend"));
    backendLabel_->setWordWrap(true);
    backendLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    backendLabel_->setFocusPolicy(Qt::TabFocus);
    backendLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    modelLayout->addWidget(backendLabel_);
    backendLabel_->hide();

    statusLabel_ = new QLabel();
    statusLabel_->setObjectName(QStringLiteral("ultraClearTextStatus"));
    statusLabel_->setWordWrap(true);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    statusLabel_->setFocusPolicy(Qt::TabFocus);
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    modelLayout->addWidget(statusLabel_);

    auto* paneLabel = new QLabel(QStringLiteral("Recognized text"));
    readingPane_ = new QPlainTextEdit();
    readingPane_->setObjectName(QStringLiteral("ultraClearTextReadingPane"));
    readingPane_->setReadOnly(true);
    readingPane_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    readingPane_->setTabChangesFocus(true);
    readingPane_->setAccessibleName(QStringLiteral("Recognized text"));
    readingPane_->setAccessibleDescription(QStringLiteral(
        "Text accepted by Ultra Clear Text, in reading order. An ellipsis marks words left as the original image."));
    readingPane_->setMinimumHeight(96);
    readingPane_->setMaximumHeight(220);
    paneLabel->setBuddy(readingPane_);
    modelLayout->addWidget(paneLabel);
    modelLayout->addWidget(readingPane_);

    connect(enableCheckbox_, &QCheckBox::toggled, this, [this](bool checked) {
        UpdateDependentState();
        emit enabledChanged(checked);
    });
    connect(showOriginalCheckbox_, &QCheckBox::toggled, this, &UltraClearTextPanel::showOriginalChanged);
    SetStatus(QStringLiteral("Ultra Clear Text is off."));
    UpdateDependentState();
}

bool UltraClearTextPanel::IsEnabledByUser() const
{
    return enableCheckbox_->isChecked();
}

void UltraClearTextPanel::SetEnabledByUser(bool enabled)
{
    const QSignalBlocker blocker(enableCheckbox_);
    enableCheckbox_->setChecked(enabled);
    UpdateDependentState();
}

void UltraClearTextPanel::SetTextClarityEnabled(bool enabled)
{
    textClarityEnabled_ = enabled;
    UpdateDependentState();
}

bool UltraClearTextPanel::IsShowingOriginal() const
{
    return showOriginalCheckbox_->isChecked();
}

uct::ModelSettings UltraClearTextPanel::ModelSettings() const {
    uct::ModelSettings models;
    const auto tier=[](QComboBox* combo,uct::ModelTier fallback){
        return uct::ParseModelTier(combo->currentData().toString()).value_or(fallback);
    };
    models.gpuDetector=tier(gpuDetectorCombo_,models.gpuDetector);
    models.gpuRecognizer=tier(gpuRecognizerCombo_,models.gpuRecognizer);
    models.cpuDetector=tier(cpuDetectorCombo_,models.cpuDetector);
    models.cpuRecognizer=tier(cpuRecognizerCombo_,models.cpuRecognizer);
    return models;
}
void UltraClearTextPanel::SetModelSettings(const uct::ModelSettings& models) {
    const auto set=[](QComboBox* combo,uct::ModelTier tier){
        const int index=combo->findData(uct::ModelTierToken(tier));if(index<0)return;
        const QSignalBlocker blocker(combo);combo->setCurrentIndex(index);
    };
    set(gpuDetectorCombo_,models.gpuDetector);set(gpuRecognizerCombo_,models.gpuRecognizer);
    set(cpuDetectorCombo_,models.cpuDetector);set(cpuRecognizerCombo_,models.cpuRecognizer);
}

void UltraClearTextPanel::SetStatus(const QString& sourceText, bool important)
{
    if (statusSource_ == sourceText) {
        return;
    }
    statusSource_ = sourceText;
    SetLiveAccessibleDescription(advancedToggle_,sourceText);
    const bool collapsed=!advancedToggle_->isChecked();
    if(important && collapsed) {
        QAccessibleAnnouncementEvent event(advancedToggle_,TranslateUi(sourceText));
        event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
        QAccessible::updateAccessibility(&event);
    }
    // Semantic state is current immediately, including for an already cached
    // accessibility interface. Silence controls unsolicited speech, not tree
    // freshness; only rapidly changing numerical diagnostics need coalescing.
    SetLiveText(statusLabel_, sourceText,
                important && !collapsed ? LivePoliteness::kPolite : LivePoliteness::kSilent);
}

void UltraClearTextPanel::SetBackendStatus(const uct::BackendStatus& status)
{
    const QString source = status.state == uct::BackendState::kReady && !status.name.isEmpty()
        ? (status.usesGpu ? QStringLiteral("Using GPU backend: %1")
                          : QStringLiteral("Using CPU backend: %1")).arg(status.name)
        : QString{};
    if (backendSource_ != source) {
        backendSource_ = source;
        SetLiveText(backendLabel_, source, LivePoliteness::kSilent);
    }
    backendLabel_->setVisible(textClarityEnabled_ && enableCheckbox_->isChecked() && !backendSource_.isEmpty());
}

void UltraClearTextPanel::SetReadingText(const QString& text)
{
    const QString previous = readingPane_->toPlainText();
    if (previous == text) {
        return;
    }
    const QTextCursor previousCursor = readingPane_->textCursor();
    // QTextCursor copies stay attached to the document and move when
    // setPlainText replaces it. Snapshot integer endpoints before that edit.
    const int previousAnchor = previousCursor.anchor();
    const int previousPosition = previousCursor.position();
    QScrollBar* verticalBar = readingPane_->verticalScrollBar();
    QScrollBar* horizontalBar = readingPane_->horizontalScrollBar();
    const int verticalScroll = verticalBar ? verticalBar->value() : 0;
    const int horizontalScroll = horizontalBar ? horizontalBar->value() : 0;

    const qsizetype commonLimit = std::min(previous.size(), text.size());
    qsizetype prefix = 0;
    while (prefix < commonLimit && previous.at(prefix) == text.at(prefix)) {
        ++prefix;
    }
    qsizetype suffix = 0;
    while (suffix < commonLimit - prefix &&
           previous.at(previous.size() - 1 - suffix) == text.at(text.size() - 1 - suffix)) {
        ++suffix;
    }
    // A remapped offset must not land between a surrogate pair.
    if (prefix > 0 && prefix < previous.size() && previous.at(prefix - 1).isHighSurrogate() &&
        previous.at(prefix).isLowSurrogate()) {
        --prefix;
    }
    const qsizetype suffixStart = previous.size() - suffix;
    if (suffix > 0 && suffixStart > 0 && suffixStart < previous.size() &&
        previous.at(suffixStart - 1).isHighSurrogate() && previous.at(suffixStart).isLowSurrogate()) {
        --suffix;
    }

    readingPane_->setPlainText(text);
    const auto remap = [&](int offset) {
        int mapped = RemapReadingOffset(offset, prefix, suffix, previous.size(), text.size());
        // A replaced middle can introduce a new supplementary character,
        // even when both unchanged boundaries were valid UTF-16 offsets.
        if (mapped > 0 && mapped < text.size() && text.at(mapped - 1).isHighSurrogate() &&
            text.at(mapped).isLowSurrogate()) {
            --mapped;
        }
        return mapped;
    };
    QTextCursor restored(readingPane_->document());
    restored.setPosition(remap(previousAnchor));
    restored.setPosition(remap(previousPosition), QTextCursor::KeepAnchor);
    readingPane_->setTextCursor(restored);
    // Restore scroll after the caret so the reader's viewport wins over any
    // ensure-cursor-visible scrolling.
    if (verticalBar) {
        verticalBar->setValue(std::min(verticalScroll, verticalBar->maximum()));
    }
    if (horizontalBar) {
        horizontalBar->setValue(std::min(horizontalScroll, horizontalBar->maximum()));
    }
    if (uct::timing::Enabled() && timingTicket_.admission.producer)
        uct::timing::RecordEvent(uct::timing::Event::PaneAssigned, timingTicket_.admission,
            {0, static_cast<std::uint32_t>(text.size()), timingTicket_.flags, 0, timingTicket_.revision});
}

void UltraClearTextPanel::UpdateDependentState()
{
    enableCheckbox_->setEnabled(textClarityEnabled_);
    const bool enabled = textClarityEnabled_ && enableCheckbox_->isChecked();
    showOriginalCheckbox_->setEnabled(enabled);
    readingPane_->setEnabled(true);
    backendLabel_->setVisible(enabled && !backendSource_.isEmpty());
}

} // namespace okuflow

#endif // _WIN32
