#include "okuflow/ui/ultra_clear_text_panel.hpp"

#if defined(_WIN32)

#include "okuflow/ui/live_status_text.hpp"

#include <QCheckBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>

namespace okuflow {

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
        "Uncertain words, handwriting, math and drawings stay as the original camera image."));
    enableCheckbox_->setToolTip(enableCheckbox_->accessibleDescription());
    layout->addWidget(enableCheckbox_);

    showOriginalCheckbox_ = new QCheckBox(QStringLiteral("Show original camera text"));
    showOriginalCheckbox_->setObjectName(QStringLiteral("ultraClearTextShowOriginal"));
    showOriginalCheckbox_->setAccessibleName(QStringLiteral("Show original camera text"));
    showOriginalCheckbox_->setAccessibleDescription(QStringLiteral(
        "Temporarily hides the sharp letters so you can compare with the camera image."));
    showOriginalCheckbox_->setToolTip(showOriginalCheckbox_->accessibleDescription());
    layout->addWidget(showOriginalCheckbox_);

    statusLabel_ = new QLabel();
    statusLabel_->setObjectName(QStringLiteral("ultraClearTextStatus"));
    statusLabel_->setWordWrap(true);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    statusLabel_->setFocusPolicy(Qt::TabFocus);
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(statusLabel_);

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
    layout->addWidget(paneLabel);
    layout->addWidget(readingPane_);

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

bool UltraClearTextPanel::IsShowingOriginal() const
{
    return showOriginalCheckbox_->isChecked();
}

void UltraClearTextPanel::SetStatus(const QString& sourceText, bool important)
{
    if (statusSource_ == sourceText) {
        return;
    }
    statusSource_ = sourceText;
    if (important) {
        SetLiveText(statusLabel_, sourceText, LivePoliteness::kPolite);
    } else {
        // Routine transitions coalesce silently instead of interrupting speech.
        SetLiveTextCoalesced(statusLabel_, sourceText, LivePoliteness::kSilent);
    }
}

void UltraClearTextPanel::SetReadingText(const QString& text)
{
    if (readingPane_->toPlainText() == text) {
        return;
    }
    QScrollBar* bar = readingPane_->verticalScrollBar();
    const int scroll = bar ? bar->value() : 0;
    readingPane_->setPlainText(text);
    if (bar) {
        bar->setValue(std::min(scroll, bar->maximum()));
    }
}

void UltraClearTextPanel::UpdateDependentState()
{
    const bool enabled = enableCheckbox_->isChecked();
    showOriginalCheckbox_->setEnabled(enabled);
    readingPane_->setEnabled(true);
}

} // namespace okuflow

#endif // _WIN32
