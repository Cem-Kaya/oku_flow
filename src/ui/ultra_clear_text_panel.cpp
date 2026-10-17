#include "okuflow/ui/ultra_clear_text_panel.hpp"

#if defined(_WIN32)

#include "okuflow/ui/live_status_text.hpp"

#include <QCheckBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextCursor>
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
}

void UltraClearTextPanel::UpdateDependentState()
{
    const bool enabled = enableCheckbox_->isChecked();
    showOriginalCheckbox_->setEnabled(enabled);
    readingPane_->setEnabled(true);
}

} // namespace okuflow

#endif // _WIN32
