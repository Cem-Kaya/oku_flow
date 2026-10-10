#ifdef _WIN32

#include "okuflow/ui/main_window.hpp"
#include "okuflow/ui/assistive_overlay.hpp"
#include "okuflow/ui/annotation_overlay.hpp"
#include "okuflow/ui/joystick_overlay.hpp"

#include "okuflow/app/app.hpp"
#include "okuflow/app/constants.hpp"
#include "okuflow/app/language_manager.hpp"
#include "okuflow/d3d12/presenter.hpp"
#include "okuflow/ui/color_scheme_picker.hpp"
#include "okuflow/ui/collapsible_section.hpp"
#include "okuflow/ui/live_status_text.hpp"
#include "okuflow/ui/responsive_slider_row.hpp"
#include "okuflow/ui/wheel_safe_combo_box.hpp"
#include "okuflow/ui/ui_translation.hpp"

#include <QAbstractItemModel>
#include <QAbstractButton>
#include <QAction>
#include <QAccessible>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFocusEvent>
#include <QAbstractItemView>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QParallelAnimationGroup>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegion>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTabWidget>
#include <QTabBar>
#include <QScopedValueRollback>
#include <QLayout>
#include <QBoxLayout>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>
#include <utility>
#include <vector>

#include <windows.h>

namespace okuflow {

using namespace app_constants;

namespace {

constexpr int kSimpleChromeIdleMs = 5000;
constexpr int kSimpleChromeFadeMs = 260;
constexpr int kModeToastMs = 1200;
constexpr int kSimpleShortcutCount = 9;
constexpr int kAdvancedPanelMinimumWidth = 360;
constexpr int kAdvancedPanelDefaultWidth = 520;

QString PlainPresetLabel(const QString& original)
{
    if (original == QStringLiteral("Reading")) return QStringLiteral("Read a Page");
    if (original == QStringLiteral("High Contrast")) return QStringLiteral("High Contrast");
    if (original == QStringLiteral("Steady Text")) return QStringLiteral("Keep It Steady");
    if (original == QStringLiteral("Sharp Text")) return QStringLiteral("Sharpen Text");
    if (original == QStringLiteral("Large Zoom")) return QStringLiteral("Zoom In More");
    if (original == QStringLiteral("Low Light")) return QStringLiteral("See in Low Light");
    if (original == QStringLiteral("Scene Explain")) return QStringLiteral("Describe the Scene");
    return original;
}

// Current-mode carousel button. The label elides while painting, so live
// retranslation (which restores the full source text on Show/Polish) can never
// leave a clipped label, and the shortcut number is a separate trailing badge
// that is never elided away.
class ModeCarouselButton final : public QPushButton {
public:
    using QPushButton::QPushButton;

    void setShortcutNumber(int number)
    {
        if (shortcutNumber_ == number) {
            return;
        }
        shortcutNumber_ = number;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        QSize hint = QPushButton::sizeHint();
        if (shortcutNumber_ > 0) {
            hint.rwidth() += BadgeSize().width() + kBadgeGap;
        }
        return hint;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);
        const QString label = option.text;
        option.text.clear();
        painter.drawControl(QStyle::CE_PushButton, option);

        QRect contents =
            style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
        const QColor textColor = palette().color(
            isEnabled() ? QPalette::Active : QPalette::Disabled,
            QPalette::ButtonText);
        if (shortcutNumber_ > 0) {
            const QSize badgeSize = BadgeSize();
            QRect badge(QPoint(contents.right() - badgeSize.width() + 1,
                               contents.center().y() - badgeSize.height() / 2),
                        badgeSize);
            badge = QStyle::visualRect(layoutDirection(), contents, badge);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(textColor, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(badge).adjusted(1, 1, -1, -1), 6, 6);
            painter.setFont(BadgeFont());
            painter.drawText(badge, Qt::AlignCenter, QString::number(shortcutNumber_));
            const int reserved = badgeSize.width() + kBadgeGap;
            contents = layoutDirection() == Qt::RightToLeft
                           ? contents.adjusted(reserved, 0, 0, 0)
                           : contents.adjusted(0, 0, -reserved, 0);
        }
        painter.setFont(font());
        painter.setPen(textColor);
        painter.drawText(contents, Qt::AlignCenter,
                         fontMetrics().elidedText(label, Qt::ElideRight,
                                                  contents.width()));
    }

private:
    static constexpr int kBadgeGap = 10;

    QFont BadgeFont() const
    {
        QFont badgeFont = font();
        badgeFont.setPointSizeF(std::max(9.0, badgeFont.pointSizeF() * 0.75));
        badgeFont.setBold(true);
        return badgeFont;
    }

    QSize BadgeSize() const
    {
        const QFontMetrics metrics(BadgeFont());
        const int height = metrics.height() + 6;
        const int width = std::max(
            height, metrics.horizontalAdvance(QString::number(shortcutNumber_)) + 14);
        return {width, height};
    }

    int shortcutNumber_{};
};

// Keeps a companion widget's enabled state in step with a source widget, so a
// slider's value readout dims exactly when the slider does.
class EnabledMirror final : public QObject {
public:
    EnabledMirror(QWidget* source, QWidget* target)
        : QObject(target), target_(target)
    {
        source->installEventFilter(this);
        target_->setEnabled(source->isEnabled());
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::EnabledChange) {
            target_->setEnabled(static_cast<QWidget*>(watched)->isEnabled());
        }
        return false;
    }

private:
    QWidget* target_{};
};

// A wrapping companion retains the complete setting label when the native
// checkbox's single-line size hint would widen the inspector.
class CheckBoxLabel final : public QLabel {
public:
    explicit CheckBoxLabel(QCheckBox* checkbox)
        : QLabel(checkbox->text()), checkbox_(checkbox)
    {
        setWordWrap(true);
        setBuddy(checkbox);
        setToolTip(checkbox->toolTip());
        new EnabledMirror(checkbox, this);
    }
protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && isEnabled()) {
            checkbox_->setFocus(Qt::MouseFocusReason);
            checkbox_->click();
            event->accept();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }
private:
    QCheckBox* checkbox_{};
};

class TrackingButtonRow final : public QWidget {
public:
    TrackingButtonRow(const QString& caption, const std::array<QPushButton*, 3>& buttons)
        : buttons_(buttons)
    {
        layout_ = new QGridLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(8);
        layout_->setSizeConstraint(QLayout::SetNoConstraint);
        caption_ = new QLabel(caption);
        caption_->setWordWrap(true);
        Reflow(3);
    }
    QSize minimumSizeHint() const override
    {
        int minimumWidth = caption_->minimumSizeHint().width();
        for (auto* button : buttons_) {
            minimumWidth = std::max(minimumWidth,
                button->minimumSizeHint().expandedTo(button->minimumSize()).width());
        }
        return QSize(minimumWidth, layout_->minimumSize().height());
    }
protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        for (int columns : {3, 2, 1}) {
            std::array<int, 3> widths{};
            for (int index = 0; index < 3; ++index) {
                widths[index % columns] = std::max(widths[index % columns],
                    buttons_[index]->sizeHint().expandedTo(buttons_[index]->minimumSize()).width());
            }
            int required = (columns - 1) * layout_->horizontalSpacing();
            for (int column = 0; column < columns; ++column) required += widths[column];
            if (required <= event->size().width() || columns == 1) {
                Reflow(columns);
                break;
            }
        }
    }
private:
    void Reflow(int columns)
    {
        if (columns_ == columns) return;
        columns_ = columns;
        layout_->removeWidget(caption_);
        for (auto* button : buttons_) layout_->removeWidget(button);
        layout_->addWidget(caption_, 0, 0, 1, columns);
        for (int index = 0; index < 3; ++index) {
            layout_->addWidget(buttons_[index], 1 + index / columns, index % columns);
        }
        updateGeometry();
    }
    std::array<QPushButton*, 3> buttons_{};
    int columns_{};
    QLabel* caption_{};
    QGridLayout* layout_{};
};

// Slider readouts in display units, formatted for the active locale.
QString FormatPercent(int value)
{
    const QLocale locale;
    const QString number = locale.toString(value);
    switch (locale.language()) {
    case QLocale::Turkish:
        return QStringLiteral("%") + number;
    case QLocale::German:
        return number + QStringLiteral(" %");
    default:
        return number + QStringLiteral("%");
    }
}

QString FormatScaled(int value, double divisor, int decimals)
{
    return QLocale().toString(value / divisor, 'f', decimals);
}

QString FormatSigned(int value)
{
    const QString number = QLocale().toString(value);
    return value > 0 ? QStringLiteral("+") + number : number;
}

bool HasEditableTextFocus()
{
    QWidget* focus = QApplication::focusWidget();
    while (focus) {
        if (qobject_cast<QLineEdit*>(focus) ||
            qobject_cast<QPlainTextEdit*>(focus)) {
            return true;
        }
        if (auto* combo = qobject_cast<QComboBox*>(focus); combo && combo->isEditable()) {
            return true;
        }
        focus = focus->parentWidget();
    }
    return false;
}

}  // namespace

MainWindow::MainWindow()
{
    setWindowTitle("OkuFlow");
    resize(1280, 720);
    QPalette readablePalette = palette();
    const QColor disabledText(QStringLiteral("#8f8f8f"));
    readablePalette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    readablePalette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    readablePalette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    // The default dark-theme placeholder is too faint for low-vision users.
    readablePalette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#b3b3b3")));
    setPalette(readablePalette);

    // Whole-app readability: bigger base font, a strong visible focus outline
    // on every interactive control, and chunky slider handles. Palette roles
    // are used throughout so the native light/dark theme is preserved.
    setStyleSheet(QStringLiteral(R"(
        QWidget { font-size: 12pt; }
        QPushButton, QToolButton {
            min-height: 32px;
            padding: 4px 14px;
            border: 2px solid palette(mid);
            border-radius: 6px;
            background: palette(button);
        }
        QPushButton:hover, QToolButton:hover { background: palette(midlight); }
        QPushButton:checked, QToolButton:checked {
            background: palette(highlight);
            color: palette(highlighted-text);
        }
        QPushButton:focus, QToolButton:focus { border: 3px solid palette(highlight); }
        QPushButton:disabled, QToolButton:disabled { color: #8f8f8f; }
        QComboBox {
            min-height: 32px;
            padding: 2px 10px;
            border: 2px solid palette(mid);
            border-radius: 6px;
            background: palette(button);
        }
        QComboBox:focus { border: 3px solid palette(highlight); }
        QLineEdit {
            padding: 6px 8px;
            border: 2px solid palette(mid);
            border-radius: 6px;
            background: palette(base);
        }
        QLineEdit:focus { border: 3px solid palette(highlight); }
        QCheckBox { spacing: 8px; padding: 2px; border: 3px solid transparent; border-radius: 6px; }
        QCheckBox:focus { border-color: palette(highlight); }
        QCheckBox::indicator { width: 20px; height: 20px; }
        QSlider { min-height: 30px; border: 3px solid transparent; border-radius: 6px; }
        QSlider:focus { border-color: palette(highlight); }
        QSlider::groove:horizontal { height: 8px; border-radius: 4px; background: palette(mid); }
        QSlider::sub-page:horizontal { background: palette(highlight); border-radius: 4px; }
        QSlider::handle:horizontal {
            width: 22px;
            margin: -8px 0;
            border-radius: 11px;
            background: palette(button);
            border: 2px solid palette(dark);
        }
        QSlider::handle:horizontal:hover { background: palette(midlight); }
        QListWidget { border: 2px solid palette(mid); border-radius: 6px; }
        QListWidget:focus { border: 3px solid palette(highlight); }
        QListWidget::item { padding: 6px; }
        QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }
        QToolButton#collapsibleHeader {
            text-align: left;
            font-weight: 600;
            border-radius: 4px;
            background: palette(button);
        }
        QLabel#scopeSubtitle { color: #b3b3b3; }
        QLabel#currentModeCaption { font-size: 11pt; font-weight: 600; color: #e6e6e6; }
        QLabel#currentModeSummaryLabel { font-size: 15pt; font-weight: 700; color: #ffffff; }
        QLabel#formatWarningLabel { color: #ffd166; font-weight: 600; }
        QPushButton#simpleModeButton, QPushButton#advancedModeButton {
            font-size: 13pt;
            font-weight: 600;
            min-height: 36px;
            padding: 2px 20px;
        }
        QCheckBox#simpleTextClarityToggle {
            font-size: 13pt;
            font-weight: 600;
            min-height: 36px;
            padding: 2px 16px 2px 12px;
            spacing: 10px;
            border: 2px solid palette(mid);
            border-radius: 6px;
            background: palette(button);
        }
        QCheckBox#simpleTextClarityToggle:hover { background: palette(midlight); }
        QCheckBox#simpleTextClarityToggle:checked {
            background: palette(highlight);
            color: palette(highlighted-text);
            border-color: palette(highlight);
        }
        QCheckBox#simpleTextClarityToggle:focus { border: 3px solid palette(highlight); }
        QCheckBox#simpleTextClarityToggle:checked:focus { border: 3px solid #ffffff; }
        QCheckBox#simpleTextClarityToggle::indicator { width: 24px; height: 24px; }
        QPushButton#simpleModeButton {
            border-top-right-radius: 0;
            border-bottom-right-radius: 0;
        }
        QPushButton#advancedModeButton {
            border-top-left-radius: 0;
            border-bottom-left-radius: 0;
        }
        QWidget#topLeftPanel, QWidget#bottomLeftPanel, QWidget#keystoneTrackingPanel, QWidget#bottomRightPanel,
        QWidget#modeGridPopup, QWidget#modeToast, QWidget#uiVisibilityPanel {
            background: #111111;
            border: 3px solid #f4f4f4;
        }
        QWidget#topLeftPanel { border-top: 0; border-left: 0; border-bottom-right-radius: 8px; }
        QWidget#bottomLeftPanel { border-bottom: 0; border-left: 0; border-top-right-radius: 8px; }
        QWidget#keystoneTrackingPanel { border-bottom: 0; border-top-left-radius: 8px; border-top-right-radius: 8px; }
        QWidget#bottomRightPanel { border-bottom: 0; border-right: 0; border-top-left-radius: 8px; }
        QWidget#modeGridPopup {
            border-left: 0;
            border-top-left-radius: 0;
            border-bottom-left-radius: 0;
            border-top-right-radius: 8px;
            border-bottom-right-radius: 8px;
        }
        QWidget#modeToast { border-color: palette(highlight); border-radius: 8px; }
        QWidget#cameraPlaceholder {
            background: #111111;
            border: 3px solid #f4f4f4;
            border-radius: 8px;
        }
        QLabel#cameraPlaceholderTitle { font-size: 22pt; font-weight: 700; color: #ffffff; }
        QLabel#cameraPlaceholderDetail { font-size: 13pt; color: #e6e6e6; }
        QLabel#brandLabel { font-size: 15pt; font-weight: 700; color: #ffffff; }
        QLabel#processingStatusLabel { font-size: 11pt; font-weight: 700; }
        QLabel#modeToastTitle { font-size: 34pt; font-weight: 800; color: #ffffff; }
        QLabel#modeToastSubtitle { font-size: 16pt; font-weight: 600; color: #ffffff; }
        QPushButton#currentModeButton { font-size: 15pt; font-weight: 700; }
        QPushButton#previousModeButton, QPushButton#nextModeButton,
        QToolButton#modeGridButton { min-width: 52px; min-height: 52px; padding: 2px; }
        QWidget#keystoneTrackingPanel QPushButton { min-width: 52px; min-height: 52px; padding: 2px; }
        QWidget#bottomRightPanel QPushButton { min-width: 88px; min-height: 58px; font-size: 11pt; }
        QToolButton#advancedTabArrow {
            min-width: 40px;
            max-width: 40px;
            min-height: 40px;
            max-height: 40px;
            padding: 5px;
        }
        QPushButton#advancedNavButton {
            min-height: 40px;
            max-height: 40px;
            padding: 5px 10px;
        }
        QListWidget#presetList {
            background: #111111;
            border: 0;
            border-radius: 0;
            font-size: 13pt;
            font-weight: 650;
            outline: 0;
        }
        QListWidget#presetList::item {
            background: #1c1c1c;
            color: #ffffff;
            border: 2px solid #f4f4f4;
            border-radius: 6px;
            padding: 10px;
        }
        QListWidget#presetList::item:selected {
            background: palette(highlight);
            color: palette(highlighted-text);
            border: 3px solid #ffffff;
        }
        QListWidget#presetList::item:focus { border: 3px solid palette(highlight); }
        QTextBrowser, QPlainTextEdit {
            padding: 7px;
            border: 2px solid palette(mid);
            border-radius: 6px;
            background: palette(base);
        }
        QTextBrowser:focus, QPlainTextEdit:focus { border: 3px solid palette(highlight); }
        QTabBar::tab { min-height: 34px; min-width: 92px; padding: 4px 10px; }
        /* Separate tabs with a gap and give the selected tab a solid fill so
           the current section is obvious. Horizontal cost stays small (about
           10 px per tab) so the elided strip still fits a 360 px inspector. */
        QTabBar#advancedTabBar::tab {
            min-width: 0px;
            padding: 4px 4px;
            margin-right: 2px;
            border: 2px solid palette(mid);
            border-bottom: 0;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
            background: palette(button);
            color: palette(button-text);
        }
        QTabBar#advancedTabBar::tab:hover:!selected { background: palette(midlight); }
        QTabBar#advancedTabBar::tab:selected {
            background: palette(highlight);
            color: palette(highlighted-text);
            border-color: #ffffff;
            font-weight: 700;
        }
        QLabel#sectionLabel {
            font-size: 13pt;
            font-weight: 600;
            padding: 8px 0 4px 0;
            color: #d79ae6;
        }
        QLabel#featureStatusLabel {
            font-size: 10pt;
            font-weight: 600;
            padding: 2px 0 4px 30px;
        }
        QSplitter#advancedContentSplitter::handle:horizontal {
            width: 10px;
            background: palette(mid);
            border-left: 2px solid palette(dark);
            border-right: 2px solid palette(light);
        }
        QSplitter#advancedContentSplitter::handle:horizontal:hover,
        QSplitter#advancedContentSplitter::handle:horizontal:pressed {
            background: palette(highlight);
        }
    )"));

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // These controls are parented into solid overlays after the render widget
    // is created. Advanced keeps the same inspector and reuses the mode switch.
    simpleModeButton_ = new QPushButton("Simple");
    simpleModeButton_->setObjectName("simpleModeButton");
    simpleModeButton_->setCheckable(true);
    simpleModeButton_->setChecked(true);
    advancedModeButton_ = new QPushButton("Advanced");
    advancedModeButton_->setObjectName("advancedModeButton");
    advancedModeButton_->setCheckable(true);
    auto* modeGroup = new QButtonGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addButton(simpleModeButton_);
    modeGroup->addButton(advancedModeButton_);

    processingStatusLabel_ = new QLabel("Processing: CPU");
    processingStatusLabel_->setObjectName("processingStatusLabel");
    processingStatusLabel_->setWordWrap(true);
    processingStatusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                     Qt::TextSelectableByKeyboard);
    processingStatusLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    processingStatusLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    performanceDiagnosticsLabel_ =
        new QLabel(QStringLiteral("Collecting timing samples\u2026"));
    performanceDiagnosticsLabel_->setObjectName(
        QStringLiteral("performanceDiagnosticsLabel"));
    performanceDiagnosticsLabel_->setWordWrap(true);
    performanceDiagnosticsLabel_->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    performanceDiagnosticsLabel_->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Preferred);

    presetList_ = new QListWidget();
    presetList_->setObjectName("presetList");
    presetList_->setSelectionMode(QAbstractItemView::SingleSelection);
    presetList_->setFocusPolicy(Qt::StrongFocus);
    presetList_->setFlow(QListView::LeftToRight);
    presetList_->setViewMode(QListView::IconMode);
    presetList_->setWrapping(true);
    presetList_->setMovement(QListView::Static);
    presetList_->setResizeMode(QListView::Adjust);
    presetList_->setWordWrap(true);
    presetList_->setTextElideMode(Qt::ElideNone);
    presetList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    presetList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    presetList_->setGridSize(QSize(205, 102));
    presetList_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    presetList_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    presetList_->setSpacing(4);

    presetDescriptionLabel_ = new QLabel();
    presetDescriptionLabel_->setObjectName("presetDescriptionLabel");
    presetDescriptionLabel_->hide();

    capturePhotoButton_ = new QPushButton("Photo");
    recordButton_ = new QPushButton("Record");
    recordButton_->setCheckable(true);
    explainNowButton_ = new QPushButton("Explain");
    readTextButton_ = new QPushButton("Read");
    annotationButton_ = new QPushButton(QStringLiteral("Draw"));
    annotationButton_->setCheckable(true);
    capturePhotoButton_->setToolTip("Capture original and processed photos");
    recordButton_->setToolTip("Record original and processed videos");
    explainNowButton_->setToolTip("Explain the current view");
    readTextButton_->setToolTip("Read text in the current view");

    // Advanced is a right-side inspector where each tab answers one question.
    // Image holds mode-owned tuning that quick modes save; Settings holds the
    // shared setup no quick mode changes. Every control keeps its binding and
    // persistence path; only its placement differs between the two tabs.
    const auto sectionLayout = [](CollapsibleSection* section) {
        return qobject_cast<QVBoxLayout*>(section->contentWidget()->layout());
    };
    const auto makeSection = [this](const QString& title, const QString& key,
                                     bool expanded) {
        auto* section = new CollapsibleSection(title);
        section->setPersistKey(key);
        // First-run disclosure; restored states replace it later.
        section->setExpanded(expanded);
        connect(section, &CollapsibleSection::expandedChanged,
                this, &MainWindow::sectionStatesChanged);
        return section;
    };
    const auto makeSearchEdit = [](const QString& objectName,
                                   const QString& placeholder,
                                   const QString& accessibleName) {
        auto* edit = new QLineEdit();
        edit->setObjectName(objectName);
        edit->setPlaceholderText(placeholder);
        edit->setClearButtonEnabled(true);
        edit->setAccessibleName(accessibleName);
        edit->setAccessibleDescription(QStringLiteral(
            "Type part of a setting name to reveal and focus matching controls."));
        edit->addAction(QIcon(QStringLiteral(":/okuflow/icons/search.svg")),
                        QLineEdit::LeadingPosition);
        return edit;
    };
    // Visible counterpart of the match count the search field already reports
    // through its accessible description, so it stays silent.
    const auto makeSearchStatus = [](const QString& objectName) {
        auto* label = new QLabel();
        label->setObjectName(objectName);
        label->setWordWrap(true);
        label->setVisible(false);
        return label;
    };
    const auto makeScopeSubtitle = [](const QString& text) {
        auto* label = new QLabel(text);
        label->setObjectName(QStringLiteral("scopeSubtitle"));
        label->setWordWrap(true);
        return label;
    };
    const auto makeSettingsScroll = [](const QString& objectName, QWidget* content) {
        auto* scroll = new QScrollArea();
        scroll->setObjectName(objectName);
        scroll->setWidget(content);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFocusPolicy(Qt::NoFocus);
        scroll->setMinimumWidth(0);
        scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        return scroll;
    };

    // Every inspector slider shows its value in display units next to it.
    const auto addSliderReadout = [this](QSlider* slider,
                                         std::function<QString(int)> format) {
        auto* readout = new QLabel();
        readout->setObjectName(QStringLiteral("sliderValueLabel"));
        readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        readout->setMinimumWidth(64);
        new EnabledMirror(slider, readout);
        const auto refresh = [readout, slider, format]() {
            readout->setText(format(slider->value()));
        };
        connect(slider, &QSlider::valueChanged, readout, refresh);
        refresh();
        sliderReadoutRefreshers_.push_back(refresh);
        return readout;
    };
    // Readouts the application writes itself. They share the inspector look,
    // and are re-rendered here only when the locale changes.
    const auto adoptAppReadout = [this](QLabel* readout, QSlider* slider,
                                        const QString& role,
                                        std::function<QString(int)> format) {
        readout->setObjectName(QStringLiteral("sliderValueLabel"));
        readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        readout->setMinimumWidth(64);
        sliderReadoutRefreshers_.push_back([readout, slider, role, format]() {
            SetLiveText(readout, format(slider->value()),
                        LivePoliteness::kSilent, role);
        });
        return readout;
    };
    const auto percent = [](int value) { return FormatPercent(value); };
    const auto makeSlider = [](int minimum, int maximum, int value, int pageStep) {
        auto* slider = new WheelSafeSlider(Qt::Horizontal);
        slider->setRange(minimum, maximum);
        slider->setPageStep(pageStep);
        slider->setValue(value);
        return slider;
    };
    const auto wrapCheckBox = [](QCheckBox* checkbox) {
        auto* row = new QWidget();
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        auto* label = new CheckBoxLabel(checkbox);
        checkbox->setText(QString());
        layout->addWidget(checkbox, 0, Qt::AlignTop);
        layout->addWidget(label, 1);
        return row;
    };
    const auto labelFor = [](const QString& text, QWidget* buddy) {
        auto* label = new QLabel(text);
        label->setBuddy(buddy);
        new EnabledMirror(buddy, label);
        return label;
    };

    // ---- Image tab: pinned header -------------------------------------
    imageTabPage_ = new QWidget();
    imageTabPage_->setObjectName(QStringLiteral("imageTabPage"));
    auto* imageTabLayout = new QVBoxLayout(imageTabPage_);
    imageTabLayout->setContentsMargins(10, 8, 10, 0);
    imageTabLayout->setSpacing(6);

    imageSearchEdit_ = makeSearchEdit(QStringLiteral("imageSettingsSearch"),
                                      QStringLiteral("Search image settings…"),
                                      QStringLiteral("Search image settings"));
    imageSearchStatusLabel_ = makeSearchStatus(QStringLiteral("imageSettingsSearchStatus"));
    imageTabLayout->addWidget(imageSearchEdit_);
    imageTabLayout->addWidget(imageSearchStatusLabel_);

    // The mode being edited is the key fact for Save / Reset, so it reads as
    // a heading under a small caption rather than as a plain status line.
    auto* currentModeCaption = new QLabel(QStringLiteral("Current quick mode"));
    currentModeCaption->setObjectName(QStringLiteral("currentModeCaption"));
    currentModeCaption->setWordWrap(true);
    imageTabLayout->addWidget(currentModeCaption);
    currentModeSummaryLabel_ = new QLabel();
    currentModeSummaryLabel_->setObjectName(QStringLiteral("currentModeSummaryLabel"));
    currentModeSummaryLabel_->setWordWrap(true);
    currentModeCaption->setBuddy(currentModeSummaryLabel_);
    imageTabLayout->addWidget(currentModeSummaryLabel_);
    imageTabLayout->addWidget(
        makeScopeSubtitle(QStringLiteral("Changes apply now. Save to keep them.")));

    // The old all-or-nothing Advanced Tuning gate is retained as a hidden
    // compatibility accessor while individual sections own disclosure.
    controlsToggleButton_ = new QToolButton(imageTabPage_);
    controlsToggleButton_->setText("Advanced Tuning");
    controlsToggleButton_->setCheckable(true);
    controlsToggleButton_->setChecked(true);
    controlsToggleButton_->hide();

    promotePresetButton_ = new QPushButton(QStringLiteral("Save as Quick Mode"));
    promotePresetButton_->setObjectName(QStringLiteral("promotePresetButton"));
    promotePresetButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    resetProfileButton_ = new QPushButton(QStringLiteral("Reset Mode"));
    resetProfileButton_->setObjectName(QStringLiteral("resetProfileButton"));
    resetProfileButton_->setToolTip(QStringLiteral(
        "Resets this mode's image and assistant settings. Settings on the "
        "Settings tab are not changed."));
    resetProfileButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // Side by side when there is room; stacked on a narrow inspector so
    // neither label is clipped (see UpdateProfileButtonLayout).
    profileButtonsLayout_ = new QBoxLayout(QBoxLayout::LeftToRight);
    profileButtonsLayout_->setSpacing(6);
    profileButtonsLayout_->addWidget(promotePresetButton_);
    profileButtonsLayout_->addWidget(resetProfileButton_);
    imageTabLayout->addLayout(profileButtonsLayout_);
    connect(resetProfileButton_, &QPushButton::clicked,
            this, &MainWindow::resetCurrentProfileRequested);

    controlsContainer_ = new QWidget();
    controlsContainer_->setObjectName(QStringLiteral("imageSettingsContent"));
    auto* controlsLayout = new QVBoxLayout(controlsContainer_);
    controlsLayout->setContentsMargins(0, 4, 0, 12);
    controlsLayout->setSpacing(8);

    // ---- 1. Zoom -------------------------------------------------------
    magnificationSection_ =
        makeSection(QStringLiteral("Zoom"), QStringLiteral("magnification"), true);
    auto* magnificationLayout = sectionLayout(magnificationSection_);
    controlsLayout->addWidget(magnificationSection_);

    zoomCheckbox_ = new QCheckBox("Zoom");
    zoomSlider_ = makeSlider(kZoomSliderScale, kZoomSliderMaxMultiplier * kZoomSliderScale,
                             kZoomSliderScale, 10);
    zoomSlider_->setEnabled(false);
    magnificationLayout->addWidget(new ResponsiveSliderRow(
        zoomCheckbox_, zoomSlider_, addSliderReadout(zoomSlider_, [](int value) {
            return FormatScaled(value, kZoomSliderScale, 2) + QStringLiteral("×");
        })));

    zoomCenterXSlider_ = makeSlider(0, kZoomFocusSliderScale, kZoomFocusSliderScale / 2, 5);
    magnificationLayout->addWidget(new ResponsiveSliderRow(
        labelFor(QStringLiteral("Horizontal position"), zoomCenterXSlider_),
        zoomCenterXSlider_, addSliderReadout(zoomCenterXSlider_, percent)));
    zoomCenterYSlider_ = makeSlider(0, kZoomFocusSliderScale, kZoomFocusSliderScale / 2, 5);
    magnificationLayout->addWidget(new ResponsiveSliderRow(
        labelFor(QStringLiteral("Vertical position"), zoomCenterYSlider_),
        zoomCenterYSlider_, addSliderReadout(zoomCenterYSlider_, percent)));

    focusMarkerCheckbox_ = new QCheckBox("Show Focus Point");
    focusMarkerCheckbox_->setChecked(false);
    focusMarkerCheckbox_->setToolTip("Overlay a red marker at the current zoom focus");
    magnificationLayout->addWidget(focusMarkerCheckbox_);

    // ---- 2. Colors and contrast ----------------------------------------
    readabilitySection_ = makeSection(QStringLiteral("Colors and contrast"),
                                      QStringLiteral("readability"), true);
    auto* readabilityLayout = sectionLayout(readabilitySection_);
    controlsLayout->addWidget(readabilitySection_);

    displayColorPicker_ = new ColorSchemePicker();
    auto* displayColorLabel = new QLabel("Display colors");
    displayColorLabel->setBuddy(displayColorPicker_);
    readabilityLayout->addWidget(displayColorLabel);
    readabilityLayout->addWidget(displayColorPicker_);
    contrastSlider_ = makeSlider(25, 400, 100, 25);
    readabilityLayout->addWidget(new ResponsiveSliderRow(
        labelFor(QStringLiteral("Contrast"), contrastSlider_), contrastSlider_,
        addSliderReadout(contrastSlider_, percent)));
    brightnessSlider_ = makeSlider(-100, 100, 0, 10);
    readabilityLayout->addWidget(new ResponsiveSliderRow(
        labelFor(QStringLiteral("Brightness"), brightnessSlider_), brightnessSlider_,
        addSliderReadout(brightnessSlider_, FormatSigned)));

    autoContrastCheckbox_ = new QCheckBox("Auto Contrast");
    autoContrastStrengthSlider_ = makeSlider(0, 100, 70, 5);
    autoContrastStrengthSlider_->setEnabled(false);
    readabilityLayout->addWidget(new ResponsiveSliderRow(
        autoContrastCheckbox_, autoContrastStrengthSlider_,
        addSliderReadout(autoContrastStrengthSlider_, percent)));

    bwCheckbox_ = new QCheckBox("Black && White");
    bwSlider_ = makeSlider(0, 255, 128, 8);
    bwSlider_->setEnabled(false);
    readabilityLayout->addWidget(new ResponsiveSliderRow(
        bwCheckbox_, bwSlider_, addSliderReadout(
            bwSlider_, [](int value) { return QLocale().toString(value); })));

    // ---- 3. Text clarity (master switch + nested expert controls) -------
    textClaritySection_ = makeSection(QStringLiteral("Text clarity"),
                                      QStringLiteral("textClarity"), true);
    auto* textClaritySectionLayout = sectionLayout(textClaritySection_);
    controlsLayout->addWidget(textClaritySection_);
    textClarityCheckbox_ = new QCheckBox("Text Clarity");
    textClaritySectionLayout->addWidget(textClarityCheckbox_);

    textClarityFineSection_ = makeSection(QStringLiteral("Fine-tune text"),
                                          QStringLiteral("textClarityFine"), false);
    auto* textClarityLayout = sectionLayout(textClarityFineSection_);
    auto* refinements = new QWidget();
    refinements->setObjectName(QStringLiteral("textClarityRefinements"));
    refinements->setStyleSheet(QStringLiteral(
        "QWidget#textClarityRefinements { border-left: 2px solid palette(mid); }"));
    auto* refinementsLayout = new QVBoxLayout(refinements);
    refinementsLayout->setContentsMargins(12, 4, 0, 4);
    textClarityHelp_ = new QLabel(QStringLiteral(
        "Text Clarity applies automatic enhancement. Fine-tune options apply only while Text Clarity is on."));
    textClarityHelp_->setObjectName(QStringLiteral("textClarityHelp"));
    textClarityHelp_->setWordWrap(true);
    textClarityHelp_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    refinementsLayout->addWidget(textClarityHelp_);
    refinementsLayout->addWidget(textClarityFineSection_);
    textClaritySectionLayout->addWidget(refinements);
    connect(textClarityCheckbox_, &QCheckBox::toggled, this,
            &MainWindow::refreshTextClarityUi);

    auto addTextSliderRow = [textClarityLayout, &addSliderReadout](
                                QCheckBox*& checkbox,
                                const QString& label,
                                QSlider*& slider,
                                int minimum, int maximum, int value,
                                std::function<QString(int)> format) {
        checkbox = new QCheckBox(label);
        slider = new WheelSafeSlider(Qt::Horizontal);
        slider->setRange(minimum, maximum);
        slider->setPageStep(std::max(1, (maximum - minimum) / 10));
        slider->setValue(value);
        textClarityLayout->addWidget(new ResponsiveSliderRow(
            checkbox, slider, addSliderReadout(slider, std::move(format))));
    };
    addTextSliderRow(backgroundFlattenCheckbox_, "Flatten Background",
                     backgroundFlattenStrengthSlider_, 0, 100, 80, percent);
    addTextSliderRow(adaptiveBinarizationCheckbox_, "Adaptive Text",
                     sauvolaStrengthSlider_, 10, 50, 28,
                     [](int value) { return FormatScaled(value, 100.0, 2); });

    binarizationSoftnessSlider_ = makeSlider(0, 25, 6, 3);
    textClarityLayout->addWidget(new ResponsiveSliderRow(
        labelFor(QStringLiteral("Edge softness"), binarizationSoftnessSlider_),
        binarizationSoftnessSlider_,
        addSliderReadout(binarizationSoftnessSlider_, [](int value) {
            return FormatScaled(value, 100.0, 2);
        })));

    textPolarityCombo_ = new WheelSafeComboBox();
    textPolarityCombo_->addItems({"Auto", "Dark on light", "Light on dark"});
    textClarityLayout->addWidget(labelFor(QStringLiteral("Text polarity"), textPolarityCombo_));
    textClarityLayout->addWidget(textPolarityCombo_);

    strokeWeightSlider_ = makeSlider(-3, 3, 0, 1);
    strokeWeightSlider_->setTickInterval(1);
    strokeWeightSlider_->setTickPosition(QSlider::TicksBelow);
    textClarityLayout->addWidget(new ResponsiveSliderRow(
        labelFor(QStringLiteral("Stroke weight"), strokeWeightSlider_),
        strokeWeightSlider_, addSliderReadout(strokeWeightSlider_, FormatSigned)));

    addTextSliderRow(smartSharpenCheckbox_, "Smart Sharpen",
                     smartSharpenStrengthSlider_, 0, 100, 45, percent);
    addTextSliderRow(claheCheckbox_, "Local Contrast (CLAHE)",
                     claheClipLimitSlider_, 10, 80, 20,
                     [](int value) { return FormatScaled(value, 10.0, 1); });
    twoColorTextCheckbox_ = new QCheckBox("Two-Color Reading");
    textClarityLayout->addWidget(twoColorTextCheckbox_);
    addTextSliderRow(textHysteresisCheckbox_, "Steady Text Edges",
                     textHysteresisStrengthSlider_, 0, 25, 8,
                     [](int value) { return FormatScaled(value, 100.0, 2); });
    selectiveSharpenCheckbox_ = new QCheckBox("Sharpen Text Only");
    textClarityLayout->addWidget(selectiveSharpenCheckbox_);
    addTextSliderRow(focusDetectionCheckbox_, "Warn When Out of Focus",
                     focusThresholdSlider_, 1, 100, 12,
                     [](int value) { return FormatScaled(value, 1000.0, 3); });
    addTextSliderRow(glareSuppressionCheckbox_, "Suppress Glare",
                     glareSuppressionStrengthSlider_, 0, 100, 50, percent);

    refreshTextClarityUi();

    // ---- 4. Steady image -----------------------------------------------
    stabilitySection_ = makeSection(QStringLiteral("Steady image"),
                                    QStringLiteral("stability"), false);
    auto* stabilityLayout = sectionLayout(stabilitySection_);
    controlsLayout->addWidget(stabilitySection_);
    stabilizationCheckbox_ = new QCheckBox("Stabilize Image");
    stabilizationCheckbox_->setToolTip(
        "Lock the mounted camera view using full-strength CUDA stabilization");
    stabilityLayout->addWidget(stabilizationCheckbox_);
    bumpHoldCheckbox_ = new QCheckBox("Extra Stable (hold on shake)");
    bumpHoldCheckbox_->setChecked(false);
    bumpHoldCheckbox_->setEnabled(false);
    bumpHoldCheckbox_->setToolTip(
        "Freeze on the last sharp frame during a bump, then smoothly return to live video");
    stabilityLayout->addWidget(wrapCheckBox(bumpHoldCheckbox_));
    temporalSmoothCheckbox_ = new QCheckBox("Temporal Smooth");
    temporalSmoothCheckbox_->setChecked(false);
    temporalSmoothSlider_ = makeSlider(5, 100, 25, 5);
    temporalSmoothSlider_->setEnabled(false);
    temporalSmoothValueLabel_ = new QLabel(QLocale().toString(0.25, 'f', 2));
    stabilityLayout->addWidget(new ResponsiveSliderRow(
        temporalSmoothCheckbox_, temporalSmoothSlider_,
        adoptAppReadout(temporalSmoothValueLabel_, temporalSmoothSlider_,
                        QStringLiteral("Temporal blend"),
                        [](int value) { return FormatScaled(value, 100.0, 2); })));

    // ---- 5. Straighten screen ------------------------------------------
    screenFixSection_ = makeSection(QStringLiteral("Straighten screen"),
                                    QStringLiteral("screenFix"), false);
    auto* screenFixLayout = sectionLayout(screenFixSection_);
    controlsLayout->addWidget(screenFixSection_);
    keystoneCheckbox_ = new QCheckBox("Straighten Screen (Keystone)");
    screenFixLayout->addWidget(wrapCheckBox(keystoneCheckbox_));

    advancedKeystoneBackButton_ = new QPushButton("Back");
    advancedKeystonePauseButton_ = new QPushButton("Stop");
    advancedKeystoneNextButton_ = new QPushButton("Next");
    advancedKeystoneBackButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/step-back.svg")));
    advancedKeystonePauseButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/pause.svg")));
    advancedKeystoneNextButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/step-forward.svg")));
    for (auto* button : {advancedKeystoneBackButton_, advancedKeystonePauseButton_, advancedKeystoneNextButton_}) {
        button->setIconSize(QSize(22, 22));
    }
    advancedKeystoneTrackingRow_ = new TrackingButtonRow(QStringLiteral("Correction tracking:"),
        {advancedKeystoneBackButton_, advancedKeystonePauseButton_, advancedKeystoneNextButton_});
    advancedKeystoneTrackingRow_->setEnabled(false);
    screenFixLayout->addWidget(advancedKeystoneTrackingRow_);

    // ---- 6. Sharpness --------------------------------------------------
    sharpeningSection_ = makeSection(QStringLiteral("Sharpness"),
                                     QStringLiteral("sharpening"), false);
    auto* sharpeningLayout = sectionLayout(sharpeningSection_);
    controlsLayout->addWidget(sharpeningSection_);
    spatialSharpenCheckbox_ = new QCheckBox("Spatial Sharpen");
    spatialSharpenCheckbox_->setChecked(false);
    sharpeningLayout->addWidget(spatialSharpenCheckbox_);
    spatialBackendCombo_ = new WheelSafeComboBox();
    spatialBackendCombo_->addItem("AMD FSR 1.0 (EASU + RCAS)");
    spatialBackendCombo_->addItem("NVIDIA Image Scaling (default)");
    spatialBackendCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    spatialBackendCombo_->setMinimumContentsLength(12);
    spatialBackendCombo_->setEnabled(false);
    sharpeningLayout->addWidget(labelFor(QStringLiteral("Backend"), spatialBackendCombo_));
    sharpeningLayout->addWidget(spatialBackendCombo_);
    spatialSharpnessSlider_ = makeSlider(0, 100, 25, 5);
    spatialSharpnessSlider_->setEnabled(false);
    spatialSharpnessValueLabel_ = new QLabel(QLocale().toString(0.25, 'f', 2));
    sharpeningLayout->addWidget(new ResponsiveSliderRow(
        labelFor(QStringLiteral("Sharpness"), spatialSharpnessSlider_),
        spatialSharpnessSlider_,
        adoptAppReadout(spatialSharpnessValueLabel_, spatialSharpnessSlider_,
                        QStringLiteral("Sharpness"),
                        [](int value) { return FormatScaled(value, 100.0, 2); })));

    mlTextSuperResolutionCheckbox_ = new QCheckBox("NVIDIA Super Resolution");
    mlTextSuperResolutionStrengthSlider_ = makeSlider(0, 100, 65, 10);
    sharpeningLayout->addWidget(new ResponsiveSliderRow(
        mlTextSuperResolutionCheckbox_, mlTextSuperResolutionStrengthSlider_,
        addSliderReadout(mlTextSuperResolutionStrengthSlider_, percent)));
    mlTextSuperResolutionUltra1440pCheckbox_ =
        new QCheckBox(QStringLiteral("Ultra quality (full frame, up to 1440p)"));
    mlTextSuperResolutionUltra1440pCheckbox_->setToolTip(
        QStringLiteral("Build a separate high-resolution AI scene from the full "
                       "camera frame, then apply viewport zoom and cropping. "
                       "720p cameras upscale 2x to 1440p; 1080p cameras upscale "
                       "4/3x to 1440p; 1440p cameras remain native."));
    sharpeningLayout->addWidget(wrapCheckBox(mlTextSuperResolutionUltra1440pCheckbox_));
    mlTextSuperResolutionPrefer2xCheckbox_ =
        new QCheckBox(QStringLiteral("Faster 2x mode (narrower view)"));
    mlTextSuperResolutionPrefer2xCheckbox_->setToolTip(
        QStringLiteral("Optional speed mode. Raises magnification to at least "
                       "2x, narrowing the visible source crop from 960x540 to "
                       "640x360 for a 1280x720 target. Leave off for maximum "
                       "source detail and a wider view."));
    sharpeningLayout->addWidget(wrapCheckBox(mlTextSuperResolutionPrefer2xCheckbox_));
    mlTextSuperResolutionStatusLabel_ = new QLabel(QStringLiteral("Off"));
    mlTextSuperResolutionStatusLabel_->setObjectName(QStringLiteral("featureStatusLabel"));
    mlTextSuperResolutionStatusLabel_->setWordWrap(true);
    mlTextSuperResolutionStatusLabel_->setMinimumWidth(0);
    mlTextSuperResolutionStatusLabel_->setSizePolicy(QSizePolicy::Ignored,
                                                     QSizePolicy::Preferred);
    mlTextSuperResolutionStatusLabel_->setAccessibleName(
        QStringLiteral("NVIDIA Super Resolution status"));
    sharpeningLayout->addWidget(mlTextSuperResolutionStatusLabel_);
    mlTextSuperResolutionOverrideCheckbox_ =
        new QCheckBox(QStringLiteral("Ignore 24 ms performance limit"));
    mlTextSuperResolutionOverrideCheckbox_->setVisible(false);
    mlTextSuperResolutionOverrideCheckbox_->setAccessibleName(
        QStringLiteral("Ignore NVIDIA Super Resolution performance limit"));
    mlTextSuperResolutionOverrideCheckbox_->setAccessibleDescription(
        QStringLiteral("Keep NVIDIA Super Resolution active when its measured "
                       "latency exceeds 24 milliseconds. This can reduce camera frame rate."));
    mlTextSuperResolutionOverrideCheckbox_->setToolTip(
        QStringLiteral("Keep SuperRes active even when its average GPU time exceeds 24 ms"));
    connect(mlTextSuperResolutionOverrideCheckbox_,
            &QCheckBox::toggled,
            this,
            &MainWindow::superResPerformanceOverrideChanged);
    sharpeningLayout->addWidget(mlTextSuperResolutionOverrideCheckbox_);
#if OKUFLOW_ENABLE_TEXT_SR
    mlTextSuperResolutionCheckbox_->setToolTip(
        "Use NVIDIA Video Effects SuperRes at 1.33x zoom and above; falls back to NIS automatically");
    mlTextSuperResolutionStrengthSlider_->setToolTip(
        "Set the NVIDIA SuperRes enhancement strength for this preset");
#else
    mlTextSuperResolutionCheckbox_->setEnabled(false);
    mlTextSuperResolutionStrengthSlider_->setEnabled(false);
    mlTextSuperResolutionUltra1440pCheckbox_->setEnabled(false);
    mlTextSuperResolutionPrefer2xCheckbox_->setEnabled(false);
    mlTextSuperResolutionCheckbox_->setToolTip(
        "Unavailable in this build; requires OKUFLOW_ENABLE_TEXT_SR");
#endif

    blurCheckbox_ = new QCheckBox("Soften image (blur)");
    sharpeningLayout->addWidget(blurCheckbox_);
    blurSigmaSlider_ = makeSlider(kBlurSigmaSliderMin, kBlurSigmaSliderMax, 10, 2);
    blurSigmaSlider_->setSingleStep(1);
    blurSigmaSlider_->setEnabled(false);
    blurSigmaValueLabel_ = new QLabel(QLocale().toString(1.0, 'f', 1));
    sharpeningLayout->addWidget(new ResponsiveSliderRow(
        labelFor(QStringLiteral("Sigma"), blurSigmaSlider_), blurSigmaSlider_,
        adoptAppReadout(blurSigmaValueLabel_, blurSigmaSlider_,
                        QStringLiteral("Blur sigma"),
                        [](int value) { return FormatScaled(value, 10.0, 1); })));
    blurRadiusSlider_ = makeSlider(kSupportedBlurRadii.front(), kSupportedBlurRadii.back(), 3, 1);
    blurRadiusSlider_->setSingleStep(1);
    blurRadiusSlider_->setTickInterval(1);
    blurRadiusSlider_->setTickPosition(QSlider::TicksBelow);
    blurRadiusSlider_->setEnabled(false);
    blurRadiusValueLabel_ = new QLabel(QLocale().toString(3));
    sharpeningLayout->addWidget(new ResponsiveSliderRow(
        labelFor(QStringLiteral("Radius"), blurRadiusSlider_), blurRadiusSlider_,
        adoptAppReadout(blurRadiusValueLabel_, blurRadiusSlider_,
                        QStringLiteral("Blur radius"),
                        [](int value) { return QLocale().toString(value); })));

    // ---- 7. Assistant behavior -----------------------------------------
    assistantSection_ = makeSection(QStringLiteral("Assistant behavior"),
                                    QStringLiteral("assistant"), false);
    auto* assistantSectionLayout = sectionLayout(assistantSection_);
    controlsLayout->addWidget(assistantSection_);
    vlmAssistCheckbox_ = new QCheckBox("Scene Explain");
    assistiveOverlayCheckbox_ = new QCheckBox("Assistive Overlay");
    assistiveOverlayCheckbox_->setChecked(true);
    assistantSectionLayout->addWidget(vlmAssistCheckbox_);
    assistantSectionLayout->addWidget(assistiveOverlayCheckbox_);

    // ---- 8. Diagnostics ------------------------------------------------
    diagnosticsSection_ = makeSection(QStringLiteral("Diagnostics"),
                                      QStringLiteral("diagnostics"), false);
    auto* diagnosticsLayout = sectionLayout(diagnosticsSection_);
    controlsLayout->addWidget(diagnosticsSection_);
    debugButton_ = new QPushButton("Debug View");
    debugButton_->setCheckable(true);
    debugButton_->setChecked(false);
    auto* debugLayout = new QHBoxLayout();
    debugLayout->addWidget(debugButton_);
    debugLayout->addStretch(1);
    diagnosticsLayout->addLayout(debugLayout);
    auto* pipelineStatusCaption = new QLabel(QStringLiteral("Pipeline status"));
    pipelineStatusCaption->setBuddy(processingStatusLabel_);
    diagnosticsLayout->addWidget(pipelineStatusCaption);
    diagnosticsLayout->addWidget(processingStatusLabel_);
    auto* performanceCaption = new QLabel(QStringLiteral("Performance percentiles"));
    performanceCaption->setBuddy(performanceDiagnosticsLabel_);
    diagnosticsLayout->addWidget(performanceCaption);
    diagnosticsLayout->addWidget(performanceDiagnosticsLabel_);
#if OKUFLOW_ENABLE_TEXT_SR
    maxineAttribution_ = new QLabel(QStringLiteral("SuperRes powered by NVIDIA Maxine™"));
    maxineAttribution_->setWordWrap(true);
    maxineAttribution_->setObjectName(QStringLiteral("vendorAttribution"));
    maxineAttribution_->setAccessibleName(QStringLiteral("NVIDIA Maxine attribution"));
    diagnosticsLayout->addWidget(maxineAttribution_);
#endif

    // QScrollArea expands short content to the viewport. Keep the sections
    // packed at the top when most of them are collapsed.
    controlsLayout->addStretch(1);
    advancedScroll_ = makeSettingsScroll(QStringLiteral("imageSettingsScroll"),
                                         controlsContainer_);
    imageTabLayout->addWidget(advancedScroll_, 1);

    // ---- Settings tab: pinned header -----------------------------------
    settingsTabPage_ = new QWidget();
    settingsTabPage_->setObjectName(QStringLiteral("settingsTabPage"));
    auto* settingsTabLayout = new QVBoxLayout(settingsTabPage_);
    settingsTabLayout->setContentsMargins(10, 8, 10, 0);
    settingsTabLayout->setSpacing(6);
    sharedSearchEdit_ = makeSearchEdit(QStringLiteral("sharedSettingsSearch"),
                                       QStringLiteral("Search settings…"),
                                       QStringLiteral("Search shared settings"));
    sharedSearchStatusLabel_ = makeSearchStatus(QStringLiteral("sharedSettingsSearchStatus"));
    settingsTabLayout->addWidget(sharedSearchEdit_);
    settingsTabLayout->addWidget(sharedSearchStatusLabel_);
    settingsTabLayout->addWidget(makeScopeSubtitle(
        QStringLiteral("Shared by all modes. Not saved into quick modes.")));

    sharedSettingsContent_ = new QWidget();
    sharedSettingsContent_->setObjectName(QStringLiteral("sharedSettingsContent"));
    auto* sharedLayout = new QVBoxLayout(sharedSettingsContent_);
    sharedLayout->setContentsMargins(0, 4, 0, 12);
    sharedLayout->setSpacing(8);

    // ---- 1. Camera -----------------------------------------------------
    deviceSection_ = makeSection(QStringLiteral("Camera"), QStringLiteral("device"), true);
    auto* deviceLayout = sectionLayout(deviceSection_);
    sharedLayout->addWidget(deviceSection_);
    cameraCombo_ = new WheelSafeComboBox();
    SetComboItemsAreData(cameraCombo_);
    cameraCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    cameraCombo_->setMinimumContentsLength(20);
    auto* cameraLabel = new QLabel("Camera");
    cameraLabel->setBuddy(cameraCombo_);
    deviceLayout->addWidget(cameraLabel);
    deviceLayout->addWidget(cameraCombo_);

    rotationCombo_ = new WheelSafeComboBox();
    rotationCombo_->addItem("0°");
    rotationCombo_->addItem("90°");
    rotationCombo_->addItem("180°");
    rotationCombo_->addItem("270°");
    rotationCombo_->setCurrentIndex(0);
    rotationCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    rotationCombo_->setEditable(false);
    rotationCombo_->setToolTip("Rotate input/output clockwise");
    auto* rotationLabel = new QLabel("Orientation");
    rotationLabel->setBuddy(rotationCombo_);
    deviceLayout->addWidget(rotationLabel);
    deviceLayout->addWidget(rotationCombo_);

    cameraFormatCombo_ = new WheelSafeComboBox();
    SetComboItemsAreData(cameraFormatCombo_);
    cameraFormatCombo_->addItem(QStringLiteral("Automatic (driver's choice)"),
                                QString());
    cameraFormatCombo_->setToolTip(
        QStringLiteral("Controls the live camera plus original photos and "
                       "original video. Processed video uses the resolution above."));
    auto* cameraFormatLabel =
        new QLabel(QStringLiteral("Camera resolution && frame rate"));
    cameraFormatLabel->setBuddy(cameraFormatCombo_);
    cameraFormatNoticeLabel_ = new QLabel();
    cameraFormatNoticeLabel_->setObjectName(QStringLiteral("formatWarningLabel"));
    cameraFormatNoticeLabel_->setWordWrap(true);
    cameraFormatNoticeLabel_->setVisible(false);
    deviceLayout->addWidget(cameraFormatLabel);
    deviceLayout->addWidget(cameraFormatCombo_);
    deviceLayout->addWidget(cameraFormatNoticeLabel_);

    // ---- 2. View and navigation ----------------------------------------
    deviceMoreSection_ = makeSection(QStringLiteral("View and navigation"),
                                     QStringLiteral("deviceMore"), false);
    auto* deviceMoreLayout = sectionLayout(deviceMoreSection_);
    sharedLayout->addWidget(deviceMoreSection_);
    viewportFitCombo_ = new WheelSafeComboBox();
    viewportFitCombo_->addItem("Fill (crop)", 0);
    viewportFitCombo_->addItem("Fit (show all)", 1);
    viewportFitCombo_->setToolTip(
        "Fill uses the full viewport without stretching; Fit preserves the entire image with bars");
    auto* viewportFitLabel = new QLabel("Viewport framing");
    viewportFitLabel->setBuddy(viewportFitCombo_);
    deviceMoreLayout->addWidget(viewportFitLabel);
    deviceMoreLayout->addWidget(viewportFitCombo_);
    viewportRateCombo_ = new WheelSafeComboBox();
    viewportRateCombo_->addItem("Auto (up to 120 FPS)", 0);
    viewportRateCombo_->addItem("60 FPS", 1);
    viewportRateCombo_->addItem("90 FPS", 2);
    viewportRateCombo_->addItem("120 FPS", 3);
    viewportRateCombo_->addItem("Match display", 4);
    viewportRateCombo_->setToolTip(
        "Controls smooth pan and zoom presentation; camera frame rate is unchanged");
    auto* viewportRateLabel = new QLabel("Viewport motion rate");
    viewportRateLabel->setBuddy(viewportRateCombo_);
    deviceMoreLayout->addWidget(viewportRateLabel);
    deviceMoreLayout->addWidget(viewportRateCombo_);
    joystickCheckbox_ = new QCheckBox("Virtual Joystick");
    joystickCheckbox_->setToolTip(
        QStringLiteral("Show an on-screen control for moving the zoomed view"));
    deviceMoreLayout->addWidget(joystickCheckbox_);
    zoomWheelAccelerationCheckbox_ =
        new QCheckBox(QStringLiteral("Zoom wheel acceleration"));
    zoomWheelAccelerationCheckbox_->setChecked(true);
    zoomWheelAccelerationCheckbox_->setToolTip(
        QStringLiteral("Accelerate fast Ctrl+scroll zoom gestures; keyboard zoom remains exact"));
    deviceMoreLayout->addWidget(zoomWheelAccelerationCheckbox_);

    // ---- 3. Recording --------------------------------------------------
    recordingSection_ = makeSection(QStringLiteral("Recording"),
                                    QStringLiteral("recording"), false);
    auto* recordingLayout = sectionLayout(recordingSection_);
    sharedLayout->addWidget(recordingSection_);
    recordingCanvasCombo_ = new WheelSafeComboBox();
    recordingCanvasCombo_->addItem(
        "Match camera (recommended)",
        static_cast<int>(RecordingCanvasMode::Source));
    recordingCanvasCombo_->addItem(
        "360p (640 x 360)",
        static_cast<int>(RecordingCanvasMode::Nhd360));
    recordingCanvasCombo_->addItem(
        "480p (854 x 480)",
        static_cast<int>(RecordingCanvasMode::Sd480));
    recordingCanvasCombo_->addItem(
        "HD (1280 x 720)",
        static_cast<int>(RecordingCanvasMode::Hd720));
    recordingCanvasCombo_->addItem(
        "Full HD (1920 x 1080)",
        static_cast<int>(RecordingCanvasMode::FullHd1080));
    recordingCanvasCombo_->addItem(
        "Quad HD (2560 x 1440)",
        static_cast<int>(RecordingCanvasMode::QuadHd1440));
    recordingCanvasCombo_->addItem(
        "Ultra HD (3840 x 2160)",
        static_cast<int>(RecordingCanvasMode::UltraHd2160));
    recordingCanvasCombo_->setToolTip(
        "Controls the processed MP4 resolution. The original MP4 always uses "
        "the selected camera mode resolution.");
    auto* recordingCanvasLabel =
        new QLabel(QStringLiteral("Processed recording resolution"));
    recordingCanvasLabel->setBuddy(recordingCanvasCombo_);
    recordingLayout->addWidget(recordingCanvasLabel);
    recordingLayout->addWidget(recordingCanvasCombo_);

    microphoneCombo_ = new WheelSafeComboBox();
    SetComboItemsAreData(microphoneCombo_);
    microphoneCombo_->addItem(
        QStringLiteral("No microphone (video only)"),
        QStringLiteral("__none__"));
    microphoneCombo_->setToolTip(
        QStringLiteral(
            "Select sound for both original and processed video recordings. "
            "The microphone is used only while recording."));
    auto* microphoneLabel = new QLabel(QStringLiteral("Microphone"));
    microphoneLabel->setBuddy(microphoneCombo_);
    recordingLayout->addWidget(microphoneLabel);
    recordingLayout->addWidget(microphoneCombo_);

    transcribeMicrophoneCheckbox_ =
        new QCheckBox(QStringLiteral("Transcribe microphone while recording"));
    transcribeMicrophoneCheckbox_->setToolTip(QStringLiteral(
        "Sends microphone audio to Codex Voice for live transcription, only "
        "while recording. Requires Codex signed in with ChatGPT."));
    recordingLayout->addWidget(transcribeMicrophoneCheckbox_);
    transcriptToNotesCheckbox_ =
        new QCheckBox(QStringLiteral("Add finalized transcript to lecture notes"));
    transcriptToNotesCheckbox_->setToolTip(QStringLiteral(
        "Appends each finalized phrase to the HTML lecture notes. Effective "
        "only while lecture notes are enabled."));
    transcriptToNotesCheckbox_->setEnabled(false);
    recordingLayout->addWidget(transcriptToNotesCheckbox_);
    connect(transcribeMicrophoneCheckbox_, &QCheckBox::toggled,
            transcriptToNotesCheckbox_, &QWidget::setEnabled);
    transcriptionStatusLabel_ = new QLabel();
    transcriptionStatusLabel_->setObjectName(QStringLiteral("transcriptionStatusLabel"));
    transcriptionStatusLabel_->setWordWrap(true);
    transcriptionStatusLabel_->setVisible(false);
    recordingLayout->addWidget(transcriptionStatusLabel_);
    transcriptionQuotaLabel_ = new QLabel();
    transcriptionQuotaLabel_->setObjectName(QStringLiteral("transcriptionQuotaLabel"));
    transcriptionQuotaLabel_->setWordWrap(true);
    transcriptionQuotaLabel_->setVisible(false);
    recordingLayout->addWidget(transcriptionQuotaLabel_);

    // ---- 4. Notes and files --------------------------------------------
    notesFilesSection_ = makeSection(QStringLiteral("Notes and files"),
                                     QStringLiteral("notesFiles"), false);
    auto* notesFilesLayout = sectionLayout(notesFilesSection_);
    sharedLayout->addWidget(notesFilesSection_);
    openNotesButton_ = new QPushButton("Open Notes");
    annotationCaptureOnExitCheckbox_ =
        new QCheckBox(QStringLiteral("Save drawing to notes when leaving Draw"));
    annotationCaptureOnExitCheckbox_->setChecked(true);
    openUserDataFolderButton_ =
        new QPushButton(QStringLiteral("Open my OkuFlow folder"));
    changeUserDataFolderButton_ =
        new QPushButton(QStringLiteral("Change OkuFlow folder..."));
    notesFilesLayout->addWidget(openNotesButton_);
    notesFilesLayout->addWidget(annotationCaptureOnExitCheckbox_);
    notesFilesLayout->addWidget(openUserDataFolderButton_);
    notesFilesLayout->addWidget(changeUserDataFolderButton_);

    // ---- 5. Language ---------------------------------------------------
    // Expanded on first run so someone stuck in an unfamiliar language finds
    // the flag list without opening anything.
    applicationSection_ = makeSection(QStringLiteral("Language"),
                                      QStringLiteral("application"), true);
    auto* applicationLayout = sectionLayout(applicationSection_);
    sharedLayout->addWidget(applicationSection_);
    applicationLanguageCombo_ = new WheelSafeComboBox();
    applicationLanguageCombo_->setIconSize(QSize(30, 20));
    for (AppLanguage language : SupportedAppLanguages()) {
        applicationLanguageCombo_->addItem(
            QIcon(AppLanguageFlagResource(language)),
            AppLanguageNativeName(language),
            AppLanguageCode(language));
    }
    applicationLanguageCombo_->setAccessibleName(
        QStringLiteral("Application language"));
    applicationLanguageCombo_->setAccessibleDescription(
        QStringLiteral(
            "Changes OkuFlow's interface language immediately. "
            "An assistant request already in progress keeps its original "
            "response language."));
    applicationLanguageCombo_->setToolTip(
        QStringLiteral(
            "Changes the interface immediately. In-progress assistant "
            "requests use the language selected when they started."));
    SetComboItemsAreData(applicationLanguageCombo_);
    auto* languageLabel = new QLabel(QStringLiteral("Application language"));
    languageLabel->setBuddy(applicationLanguageCombo_);
    applicationLayout->addWidget(languageLabel);
    applicationLayout->addWidget(applicationLanguageCombo_);
    connect(applicationLanguageCombo_,
            &QComboBox::currentIndexChanged,
            this,
            [this](int index) {
                if (index >= 0) {
                    emit applicationLanguageRequested(
                        applicationLanguageCombo_->itemData(index).toString());
                }
            });

    // ---- 6. AI and downloads -------------------------------------------
    aiDownloadsSection_ = makeSection(QStringLiteral("AI and downloads"),
                                      QStringLiteral("aiDownloads"), false);
    auto* aiDownloadsLayout = sectionLayout(aiDownloadsSection_);
    sharedLayout->addWidget(aiDownloadsSection_);
    aiSettingsButton_ = new QPushButton(QStringLiteral("AI Settings"));
    aiSettingsButton_->setObjectName(QStringLiteral("advancedNavButton"));
    aiSettingsButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/open-settings.svg")));
    aiSettingsButton_->setIconSize(QSize(26, 26));
    aiSettingsButton_->setToolTip(QStringLiteral("Open AI Settings dialog"));
    aiSettingsButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setupAssistantButton_ = new QPushButton(QStringLiteral("Setup && Downloads..."));
    setupAssistantButton_->setToolTip(
        QStringLiteral("Set up Codex CLI and NVIDIA Video Effects"));
    aiDownloadsLayout->addWidget(aiSettingsButton_);
    aiDownloadsLayout->addWidget(setupAssistantButton_);

    // ---- 7. Troubleshooting --------------------------------------------
    troubleshootingSection_ = makeSection(QStringLiteral("Troubleshooting"),
                                          QStringLiteral("troubleshooting"), false);
    auto* troubleshootingLayout = sectionLayout(troubleshootingSection_);
    sharedLayout->addWidget(troubleshootingSection_);
    cameraAccelerationCombo_ = new WheelSafeComboBox();
    cameraAccelerationCombo_->addItem(
        QStringLiteral("Automatic (recommended)"), 0);
    cameraAccelerationCombo_->addItem(
        QStringLiteral("Always use GPU acceleration"), 1);
    cameraAccelerationCombo_->addItem(
        QStringLiteral("Compatibility mode"), 2);
    cameraAccelerationCombo_->setToolTip(
        QStringLiteral(
            "Automatic uses hardware acceleration when the camera and driver "
            "pass startup validation, then falls back safely if needed."));
    cameraAccelerationCombo_->setAccessibleName(
        QStringLiteral("Camera acceleration mode"));
    auto* cameraAccelerationLabel = new QLabel(QStringLiteral("Camera acceleration"));
    cameraAccelerationLabel->setBuddy(cameraAccelerationCombo_);
    troubleshootingLayout->addWidget(cameraAccelerationLabel);
    troubleshootingLayout->addWidget(cameraAccelerationCombo_);
    cameraAccelerationStatusLabel_ = new QLabel(
        QStringLiteral("Camera acceleration has not started yet."));
    cameraAccelerationStatusLabel_->setObjectName(QStringLiteral("scopeSubtitle"));
    cameraAccelerationStatusLabel_->setWordWrap(true);
    cameraAccelerationStatusLabel_->setAccessibleName(
        QStringLiteral("Camera acceleration status"));
    troubleshootingLayout->addWidget(cameraAccelerationStatusLabel_);
    testCameraAccelerationButton_ =
        new QPushButton(QStringLiteral("Test this camera"));
    testCameraAccelerationButton_->setToolTip(
        QStringLiteral(
            "Tests GPU and compatibility capture in a separate process. "
            "The test cannot freeze OkuFlow if a camera driver hangs."));
    testCameraAccelerationButton_->setAccessibleName(
        QStringLiteral("Test camera acceleration"));
    testCameraAccelerationButton_->setAccessibleDescription(
        QStringLiteral(
            "Runs an isolated camera test and updates this camera's "
            "automatic acceleration decision."));
    troubleshootingLayout->addWidget(testCameraAccelerationButton_);

    sharedLayout->addStretch(1);
    sharedSettingsScroll_ = makeSettingsScroll(QStringLiteral("sharedSettingsScroll"),
                                               sharedSettingsContent_);
    settingsTabLayout->addWidget(sharedSettingsScroll_, 1);

    // ---- Search wiring (both tabs) -------------------------------------
    connect(imageSearchEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        FilterSettingsTab(SettingsScope::kImage, text);
    });
    connect(sharedSearchEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        FilterSettingsTab(SettingsScope::kShared, text);
    });
    connect(imageSearchEdit_, &QLineEdit::returnPressed, this, [this]() {
        ActivateSearchResult(SettingsScope::kImage);
    });
    connect(sharedSearchEdit_, &QLineEdit::returnPressed, this, [this]() {
        ActivateSearchResult(SettingsScope::kShared);
    });

    auto* assistantPage = new QWidget();
    auto* assistantLayout = new QVBoxLayout(assistantPage);
    assistantLayout->setContentsMargins(10, 8, 10, 10);
    assistantLayout->setSpacing(8);

    auto* assistantAiSettingsButton = new QPushButton(QStringLiteral("AI Settings"));
    assistantAiSettingsButton->setObjectName(QStringLiteral("advancedNavButton"));
    assistantAiSettingsButton->setIcon(QIcon(QStringLiteral(":/okuflow/icons/open-settings.svg")));
    assistantAiSettingsButton->setIconSize(QSize(26, 26));
    assistantAiSettingsButton->setToolTip(QStringLiteral("Open AI Settings dialog"));
    assistantAiSettingsButton->setAccessibleName(QStringLiteral("AI Settings"));
    assistantAiSettingsButton->setAccessibleDescription(
        QStringLiteral("Configure the vision assistant and speech output"));
    assistantAiSettingsButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(assistantAiSettingsButton, &QPushButton::clicked,
            aiSettingsButton_, &QPushButton::click);
    assistantLayout->addWidget(assistantAiSettingsButton);

    assistantConnectionLabel_ = new QLabel("Starting Codex...");
    assistantConnectionLabel_->setWordWrap(true);
    assistantUsageLabel_ = new QLabel();
    assistantUsageLabel_->setWordWrap(true);
    assistantConnectButton_ = new QPushButton("Connect ChatGPT");
    assistantConnectButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    assistantLayout->addWidget(assistantConnectionLabel_);
    assistantLayout->addWidget(assistantUsageLabel_);
    assistantLayout->addWidget(assistantConnectButton_);

    auto* assistantTabs = new QTabWidget();
    auto* chatPage = new QWidget();
    auto* chatLayout = new QVBoxLayout(chatPage);
    chatLayout->setContentsMargins(0, 6, 0, 0);
    chatLayout->setSpacing(8);
    assistantTranscript_ = new QTextBrowser();
    assistantTranscript_->setOpenExternalLinks(false);
    assistantTranscript_->setPlaceholderText("Ask about the current camera view.");
    assistantTranscript_->setMinimumHeight(220);
    assistantPromptEdit_ = new QPlainTextEdit();
    assistantPromptEdit_->setPlaceholderText("Ask about what is visible...");
    assistantPromptEdit_->setMaximumHeight(105);
    assistantAttachFrameCheckbox_ = new QCheckBox("Attach current view");
    assistantAttachFrameCheckbox_->setChecked(true);
    assistantNewButton_ = new QPushButton("New Conversation");
    assistantSendButton_ = new QPushButton("Send");
    assistantStopButton_ = new QPushButton("Stop");
    assistantStopButton_->setEnabled(false);
    auto* chatActions = new QHBoxLayout();
    chatActions->setSpacing(6);
    chatActions->addWidget(assistantNewButton_);
    chatActions->addStretch(1);
    chatActions->addWidget(assistantStopButton_);
    chatActions->addWidget(assistantSendButton_);
    chatLayout->addWidget(assistantTranscript_, 1);
    chatLayout->addWidget(assistantPromptEdit_);
    chatLayout->addWidget(assistantAttachFrameCheckbox_);
    chatLayout->addLayout(chatActions);

    auto* historyPage = new QWidget();
    auto* historyLayout = new QVBoxLayout(historyPage);
    historyLayout->setContentsMargins(0, 6, 0, 0);
    historyLayout->setSpacing(8);
    assistantHistoryList_ = new QListWidget();
    assistantHistoryList_->setSelectionMode(QAbstractItemView::SingleSelection);
    assistantHistoryList_->setWordWrap(true);
    assistantRenameButton_ = new QPushButton("Rename");
    assistantExportButton_ = new QPushButton("Export");
    assistantDeleteButton_ = new QPushButton("Delete");
    auto* historyActions = new QHBoxLayout();
    historyActions->setSpacing(6);
    historyActions->addWidget(assistantRenameButton_);
    historyActions->addWidget(assistantExportButton_);
    historyActions->addWidget(assistantDeleteButton_);
    historyLayout->addWidget(assistantHistoryList_, 1);
    historyLayout->addLayout(historyActions);

    assistantTabs->addTab(chatPage, "Chat");
    assistantTabs->addTab(historyPage, "History");
    assistantLayout->addWidget(assistantTabs, 1);

    // Read-only live transcript view (plan 36): no send button and no
    // implied Assistant conversation. Finalized text stays selectable.
    auto* transcriptPage = new QWidget();
    auto* transcriptLayout = new QVBoxLayout(transcriptPage);
    transcriptLayout->setContentsMargins(10, 10, 10, 10);
    transcriptLayout->setSpacing(6);
    auto* transcriptPartialCaption = new QLabel(QStringLiteral("Current phrase"));
    transcriptPartialCaption->setObjectName(QStringLiteral("scopeSubtitle"));
    transcriptPartialLabel_ = new QLabel();
    transcriptPartialLabel_->setWordWrap(true);
    transcriptPartialLabel_->setMinimumHeight(40);
    transcriptPartialCaption->setBuddy(transcriptPartialLabel_);
    auto* transcriptFinalsCaption = new QLabel(QStringLiteral("Finalized transcript"));
    transcriptFinalsCaption->setObjectName(QStringLiteral("scopeSubtitle"));
    transcriptFinalsView_ = new QPlainTextEdit();
    transcriptFinalsView_->setReadOnly(true);
    transcriptFinalsView_->setTabChangesFocus(true);
    transcriptFinalsCaption->setBuddy(transcriptFinalsView_);
    transcriptLayout->addWidget(transcriptPartialCaption);
    transcriptLayout->addWidget(transcriptPartialLabel_);
    transcriptLayout->addWidget(transcriptFinalsCaption);
    transcriptLayout->addWidget(transcriptFinalsView_, 1);

    assistantPage->setObjectName(QStringLiteral("assistantTabPage"));
    transcriptPage->setObjectName(QStringLiteral("transcriptTabPage"));
    auto* advancedTabs = new QTabWidget();
    advancedTabs->setObjectName(QStringLiteral("advancedPage"));
    advancedTabs->addTab(imageTabPage_, "Image");
    advancedTabs->addTab(assistantPage, "Assistant");
    advancedTabs->addTab(transcriptPage, "Transcript");
    advancedTabs->addTab(settingsTabPage_, "Settings");
    // Four tabs must fit a 360 px inspector in every language: tabs share the
    // strip and elide, with the full text kept in the tooltip and in the
    // accessible name (QTabBar exposes the unelided text).
    advancedTabs->tabBar()->setObjectName(QStringLiteral("advancedTabBar"));
    advancedTabs->tabBar()->setExpanding(true);
    advancedTabs->setElideMode(Qt::ElideRight);
    advancedTabs->setUsesScrollButtons(false);
    advancedTabs_ = advancedTabs;
    UpdateAdvancedTabToolTips();
    previousAdvancedTabButton_ = new QToolButton();
    previousAdvancedTabButton_->setObjectName(QStringLiteral("advancedTabArrow"));
    previousAdvancedTabButton_->setIconSize(QSize(26, 26));
    previousAdvancedTabButton_->setToolTip(QStringLiteral("Previous Advanced section"));
    previousAdvancedTabButton_->setAccessibleName(QStringLiteral("Previous Advanced section"));
    nextAdvancedTabButton_ = new QToolButton();
    nextAdvancedTabButton_->setObjectName(QStringLiteral("advancedTabArrow"));
    nextAdvancedTabButton_->setIconSize(QSize(26, 26));
    nextAdvancedTabButton_->setToolTip(QStringLiteral("Next Advanced section"));
    nextAdvancedTabButton_->setAccessibleName(QStringLiteral("Next Advanced section"));
    auto* leftTabCorner = new QWidget();
    auto* leftTabCornerLayout = new QHBoxLayout(leftTabCorner);
    leftTabCornerLayout->setContentsMargins(0, 0, 4, 0);
    leftTabCornerLayout->addWidget(previousAdvancedTabButton_);
    auto* rightTabCorner = new QWidget();
    auto* rightTabCornerLayout = new QHBoxLayout(rightTabCorner);
    rightTabCornerLayout->setContentsMargins(4, 0, 0, 0);
    helpButton_ = new QToolButton();
    helpButton_->setObjectName(QStringLiteral("advancedTabArrow"));
    helpButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/help.svg")));
    helpButton_->setIconSize(QSize(26, 26));
    helpButton_->setToolTip(QStringLiteral("Open controls and features help"));
    helpButton_->setAccessibleName(QStringLiteral("Open help"));
    helpButton_->setAccessibleDescription(
        QStringLiteral("Show a guide to OkuFlow controls and features"));
    rightTabCornerLayout->addWidget(helpButton_);
    rightTabCornerLayout->addWidget(nextAdvancedTabButton_);
    advancedTabs->setCornerWidget(leftTabCorner, Qt::TopLeftCorner);
    advancedTabs->setCornerWidget(rightTabCorner, Qt::TopRightCorner);
    connect(previousAdvancedTabButton_, &QToolButton::clicked, this, [advancedTabs]() {
        const int count = advancedTabs->count();
        if (count > 0) advancedTabs->setCurrentIndex((advancedTabs->currentIndex() + count - 1) % count);
    });
    connect(nextAdvancedTabButton_, &QToolButton::clicked, this, [advancedTabs]() {
        const int count = advancedTabs->count();
        if (count > 0) advancedTabs->setCurrentIndex((advancedTabs->currentIndex() + 1) % count);
    });
    connect(helpButton_, &QToolButton::clicked, this, &MainWindow::ShowHelpDialog);
    // With only a few sections every tab is already a large visible target
    // (and Ctrl+Tab cycles them); the wrap-around arrows earn their space
    // only once sections outgrow the strip.
    constexpr int kTabArrowMinimumSections = 5;
    previousAdvancedTabButton_->setVisible(advancedTabs->count() >= kTabArrowMinimumSections);
    nextAdvancedTabButton_->setVisible(advancedTabs->count() >= kTabArrowMinimumSections);
    UpdateDirectionalUi();
    advancedTabs->setMinimumWidth(kAdvancedPanelMinimumWidth);
    advancedTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    advancedPanel_ = advancedTabs;

    renderWidget_ = new RenderWidget();
    renderWidget_->installEventFilter(this);
    renderWidget_->setMouseTracking(true);
    renderWidget_->setMinimumSize(320, 240);
    contentSplitter_ = new QSplitter(Qt::Horizontal);
    contentSplitter_->setObjectName(QStringLiteral("advancedContentSplitter"));
    contentSplitter_->setChildrenCollapsible(false);
    contentSplitter_->setHandleWidth(10);
    contentSplitter_->addWidget(renderWidget_);
    contentSplitter_->addWidget(advancedPanel_);
    contentSplitter_->setStretchFactor(0, 1);
    contentSplitter_->setStretchFactor(1, 0);
    contentSplitter_->setSizes({width() - kAdvancedPanelDefaultWidth,
                                kAdvancedPanelDefaultWidth});
    if (QSplitterHandle* handle = contentSplitter_->handle(1)) {
        handle->setToolTip(QStringLiteral("Drag to resize Advanced settings"));
        handle->setAccessibleName(QStringLiteral("Resize Advanced settings"));
    }
    connect(contentSplitter_, &QSplitter::splitterMoved, this, [this](int, int) {
        if (advancedPanel_ && advancedPanel_->isVisible() && advancedPanel_->width() > 0) {
            advancedPanelPreferredWidth_ = advancedPanel_->width();
        }
    });
    rootLayout->addWidget(contentSplitter_, 1);
    annotationOverlay_ = new AnnotationOverlay(renderWidget_, this);

    // D3D presents directly into a native window, so frameless owned tool
    // windows provide predictable stacking and opacity above the swap chain.
    const Qt::WindowFlags chromeFlags = Qt::Tool |
                                        Qt::FramelessWindowHint |
                                        Qt::NoDropShadowWindowHint;
    topLeftPanel_ = new QWidget(this, chromeFlags);
    topLeftPanel_->setObjectName("topLeftPanel");
    uiVisibilityPanel_ = new QWidget(this, chromeFlags);
    uiVisibilityPanel_->setObjectName(QStringLiteral("uiVisibilityPanel"));
    auto* visibilityLayout = new QHBoxLayout(uiVisibilityPanel_);
    visibilityLayout->setContentsMargins(4, 4, 4, 4);
    uiVisibilityButton_ = new QToolButton(uiVisibilityPanel_);
    uiVisibilityButton_->setObjectName(QStringLiteral("uiVisibilityButton"));
    uiVisibilityButton_->setMinimumHeight(40);
    visibilityLayout->addWidget(uiVisibilityButton_);
    uiVisibilityPanel_->setAttribute(Qt::WA_ShowWithoutActivating);
    uiVisibilityPanel_->hide();
    connect(uiVisibilityButton_, &QToolButton::clicked, this,
            [this]() { setUiHidden(!uiHidden_); });
    auto* topLeftLayout = new QGridLayout(topLeftPanel_);
    topLeftLayout->setContentsMargins(10, 8, 8, 8);
    topLeftLayout->setSpacing(8);
    topLeftLayout->addWidget(simpleModeButton_, 0, 0);
    topLeftLayout->addWidget(advancedModeButton_, 0, 1);

    bottomLeftPanel_ = new QWidget(this, chromeFlags);
    bottomLeftPanel_->setObjectName("bottomLeftPanel");
    auto* bottomLeftLayout = new QGridLayout(bottomLeftPanel_);
    bottomLeftLayout->setContentsMargins(8, 8, 10, 10);
    bottomLeftLayout->setSpacing(6);
    modeGridButton_ = new QToolButton();
    modeGridButton_->setObjectName("modeGridButton");
    modeGridButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));
    modeGridButton_->setIconSize(QSize(30, 30));
    modeGridButton_->setToolTip("Show all quick modes");
    previousModeButton_ = new QPushButton();
    previousModeButton_->setObjectName("previousModeButton");
    previousModeButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    previousModeButton_->setIconSize(QSize(30, 30));
    previousModeButton_->setToolTip("Previous quick mode");
    currentModeButton_ = new ModeCarouselButton();
    currentModeButton_->setObjectName("currentModeButton");
    currentModeButton_->setToolTip("Show all quick modes");
    nextModeButton_ = new QPushButton();
    nextModeButton_->setObjectName("nextModeButton");
    nextModeButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    nextModeButton_->setIconSize(QSize(30, 30));
    nextModeButton_->setToolTip("Next quick mode");
    bottomLeftLayout->addWidget(modeGridButton_, 0, 0);
    bottomLeftLayout->addWidget(previousModeButton_, 0, 1);
    bottomLeftLayout->addWidget(currentModeButton_, 0, 2);
    bottomLeftLayout->addWidget(nextModeButton_, 0, 3);
    simpleTextClarityCheckbox_ = new QCheckBox("Text Clarity");
    simpleTextClarityCheckbox_->setObjectName(QStringLiteral("simpleTextClarityToggle"));
    simpleTextClarityCheckbox_->setToolTip("Automatically clarify text in the camera view");
    topLeftLayout->addWidget(simpleTextClarityCheckbox_, 0, 2);

    keystoneTrackingPanel_ = new QWidget(this, chromeFlags);
    keystoneTrackingPanel_->setObjectName("keystoneTrackingPanel");
    auto* simpleTrackingLayout = new QHBoxLayout(keystoneTrackingPanel_);
    simpleTrackingLayout->setContentsMargins(8, 8, 8, 10);
    simpleTrackingLayout->setSpacing(6);
    simpleKeystoneBackButton_ = new QPushButton();
    simpleKeystonePauseButton_ = new QPushButton();
    simpleKeystoneNextButton_ = new QPushButton();
    simpleKeystoneBackButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/step-back.svg")));
    simpleKeystonePauseButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/pause.svg")));
    simpleKeystoneNextButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/step-forward.svg")));
    for (QPushButton* button : {simpleKeystoneBackButton_, simpleKeystonePauseButton_,
                                simpleKeystoneNextButton_}) {
        button->setIconSize(QSize(30, 30));
        simpleTrackingLayout->addWidget(button);
    }
    keystoneTrackingPanel_->hide();

    bottomRightPanel_ = new QWidget(this, chromeFlags);
    bottomRightPanel_->setObjectName("bottomRightPanel");
    auto* bottomRightLayout = new QGridLayout(bottomRightPanel_);
    bottomRightLayout->setContentsMargins(10, 8, 8, 10);
    bottomRightLayout->setSpacing(6);
    capturePhotoButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/camera.svg")));
    recordButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/record.svg")));
    explainNowButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/explain.svg")));
    readTextButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/read.svg")));
    annotationButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/draw.svg")));
    annotationButton_->setIconSize(QSize(28, 28));
    for (QPushButton* button : {capturePhotoButton_, recordButton_, explainNowButton_, readTextButton_}) {
        button->setIconSize(QSize(28, 28));
    }
    annotationButton_->setMinimumSize(88, 58);
    connect(openUserDataFolderButton_, &QPushButton::clicked, this,
            &MainWindow::openUserDataFolderRequested);
    connect(changeUserDataFolderButton_, &QPushButton::clicked, this,
            &MainWindow::changeUserDataFolderRequested);
    auto* openFolderShortcut =
        new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")), this);
    connect(openFolderShortcut, &QShortcut::activated, this,
            &MainWindow::openUserDataFolderRequested);
    bottomRightLayout->addWidget(capturePhotoButton_, 0, 0);
    bottomRightLayout->addWidget(recordButton_, 0, 1);
    bottomRightLayout->addWidget(explainNowButton_, 0, 2);
    bottomRightLayout->addWidget(readTextButton_, 0, 3);
    bottomRightLayout->addWidget(annotationButton_, 0, 4);

    // Simple-mode live transcript overlay (plan 36): never activates, never
    // takes input or focus, and never announces per-delta — screen-reader
    // users get state changes through the live status path instead.
    simpleTranscriptPanel_ = new QWidget(
        this, chromeFlags | Qt::WindowTransparentForInput |
                  Qt::WindowDoesNotAcceptFocus);
    simpleTranscriptPanel_->setObjectName(QStringLiteral("simpleTranscriptPanel"));
    auto* simpleTranscriptLayout = new QVBoxLayout(simpleTranscriptPanel_);
    simpleTranscriptLayout->setContentsMargins(12, 8, 12, 8);
    simpleTranscriptLabel_ = new QLabel();
    simpleTranscriptLabel_->setObjectName(QStringLiteral("simpleTranscriptLabel"));
    simpleTranscriptLabel_->setWordWrap(true);
    simpleTranscriptLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
    simpleTranscriptLayout->addWidget(simpleTranscriptLabel_);
    simpleTranscriptPanel_->hide();

    connect(annotationButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (changingAnnotationMode_) {
            return;
        }
        if (checked) {
            setAnnotationMode(true);
            return;
        }
        if (annotationOverlay_) {
            annotationOverlay_->CommitPendingText();
        }
        if (annotationOverlay_ && annotationOverlay_->HasInk() &&
            annotationOverlay_->CaptureOnExit()) {
            emit annotationSnapshotRequested(2);
        }
        if (annotationOverlay_) {
            annotationOverlay_->ClearInk();
        }
        setAnnotationMode(false);
    });
    connect(annotationOverlay_, &AnnotationOverlay::SnapshotRequested,
            this, [this]() {
                annotationOverlay_->CommitPendingText();
                if (annotationOverlay_->HasInk()) {
                    emit annotationSnapshotRequested(0);
                }
            });
    connect(annotationOverlay_, &AnnotationOverlay::ClearRequested,
            this, [this]() {
                annotationOverlay_->CommitPendingText();
                if (annotationOverlay_->HasInk()) {
                    emit annotationSnapshotRequested(1);
                    annotationOverlay_->ClearInk();
                }
            });
    connect(annotationOverlay_, &AnnotationOverlay::ExitRequested,
            this, [this]() {
                annotationOverlay_->CommitPendingText();
                if (annotationOverlay_->HasInk() &&
                    annotationOverlay_->CaptureOnExit()) {
                    emit annotationSnapshotRequested(2);
                }
                annotationOverlay_->ClearInk();
                setAnnotationMode(false);
            });
    connect(annotationOverlay_, &AnnotationOverlay::CleanPhotoRequested,
            capturePhotoButton_, &QPushButton::click);
    connect(annotationOverlay_, &AnnotationOverlay::PreferencesChanged,
            this, &MainWindow::annotationPreferencesChanged);
    connect(annotationCaptureOnExitCheckbox_, &QCheckBox::toggled,
            this, [this](bool checked) {
                if (!annotationOverlay_) {
                    return;
                }
                annotationOverlay_->SetPreferences(
                    annotationOverlay_->InkColor(),
                    annotationOverlay_->InkWidthPixels(),
                    checked,
                    annotationOverlay_->Dashed(),
                    annotationOverlay_->ShapeKind(),
                    annotationOverlay_->TextSizePixels());
                emit annotationPreferencesChanged(
                    annotationOverlay_->InkColor().name(QColor::HexRgb),
                    annotationOverlay_->InkWidthPixels(),
                    checked,
                    annotationOverlay_->Dashed(),
                    annotationOverlay_->ShapeKind(),
                    annotationOverlay_->TextSizePixels());
            });

    modeGridPopup_ = new QWidget(this, chromeFlags);
    modeGridPopup_->setObjectName("modeGridPopup");
    auto* modeGridLayout = new QVBoxLayout(modeGridPopup_);
    modeGridLayout->setContentsMargins(8, 8, 8, 8);
    modeGridLayout->addWidget(presetList_);
    modeGridPopup_->hide();

    modeToast_ = new QWidget(this, Qt::ToolTip |
                                   Qt::FramelessWindowHint |
                                   Qt::NoDropShadowWindowHint);
    modeToast_->setObjectName("modeToast");
    auto* modeToastLayout = new QVBoxLayout(modeToast_);
    modeToastLayout->setContentsMargins(28, 18, 28, 18);
    modeToastLayout->setSpacing(4);
    modeToastTitle_ = new QLabel("READ A PAGE");
    modeToastTitle_->setObjectName("modeToastTitle");
    modeToastTitle_->setAlignment(Qt::AlignCenter);
    modeToastSubtitle_ = new QLabel("Reading profile");
    modeToastSubtitle_->setObjectName("modeToastSubtitle");
    modeToastSubtitle_->setAlignment(Qt::AlignCenter);
    modeToastLayout->addWidget(modeToastTitle_);
    modeToastLayout->addWidget(modeToastSubtitle_);
    modeToast_->setAccessibleName("Quick mode changed");
    modeToast_->setAttribute(Qt::WA_ShowWithoutActivating);
    modeToast_->setAttribute(Qt::WA_TransparentForMouseEvents);
    modeToast_->hide();

    cameraPlaceholder_ = new QWidget(
        this, chromeFlags | Qt::WindowTransparentForInput |
                  Qt::WindowDoesNotAcceptFocus);
    cameraPlaceholder_->setObjectName(QStringLiteral("cameraPlaceholder"));
    auto* cameraPlaceholderLayout = new QVBoxLayout(cameraPlaceholder_);
    cameraPlaceholderLayout->setContentsMargins(32, 22, 32, 22);
    cameraPlaceholderLayout->setSpacing(8);
    cameraPlaceholderTitle_ = new QLabel();
    cameraPlaceholderTitle_->setObjectName(QStringLiteral("cameraPlaceholderTitle"));
    cameraPlaceholderTitle_->setAlignment(Qt::AlignCenter);
    cameraPlaceholderDetail_ = new QLabel();
    cameraPlaceholderDetail_->setObjectName(QStringLiteral("cameraPlaceholderDetail"));
    cameraPlaceholderDetail_->setAlignment(Qt::AlignCenter);
    cameraPlaceholderDetail_->setWordWrap(true);
    cameraPlaceholderLayout->addWidget(cameraPlaceholderTitle_);
    cameraPlaceholderLayout->addWidget(cameraPlaceholderDetail_);
    cameraPlaceholder_->setAttribute(Qt::WA_ShowWithoutActivating);
    cameraPlaceholder_->setAttribute(Qt::WA_TransparentForMouseEvents);
    cameraPlaceholder_->hide();
    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
        panel->setAttribute(Qt::WA_ShowWithoutActivating);
    }

    simpleChromeIdleTimer_ = new QTimer(this);
    simpleChromeIdleTimer_->setObjectName(QStringLiteral("simpleChromeIdleTimer"));
    simpleChromeIdleTimer_->setSingleShot(true);
    simpleChromeIdleTimer_->setInterval(kSimpleChromeIdleMs);
    connect(simpleChromeIdleTimer_, &QTimer::timeout, this, &MainWindow::FadeSimpleChrome);
    modeToastTimer_ = new QTimer(this);
    modeToastTimer_->setSingleShot(true);
    modeToastTimer_->setInterval(kModeToastMs);
    connect(modeToastTimer_, &QTimer::timeout, this, [this]() {
        modeToast_->hide();
    });

    connect(modeGridButton_, &QToolButton::clicked, this, &MainWindow::ToggleModeGrid);
    connect(currentModeButton_, &QPushButton::clicked, this, &MainWindow::ToggleModeGrid);
    connect(previousModeButton_, &QPushButton::clicked, this, [this]() { ActivateRelativePreset(-1); });
    connect(nextModeButton_, &QPushButton::clicked, this, [this]() { ActivateRelativePreset(1); });
    for (QPushButton* button : {simpleKeystoneBackButton_, advancedKeystoneBackButton_}) {
        connect(button, &QPushButton::clicked, this, &MainWindow::keystoneStepBackRequested);
    }
    for (QPushButton* button : {simpleKeystonePauseButton_, advancedKeystonePauseButton_}) {
        connect(button, &QPushButton::clicked, this, &MainWindow::keystonePauseResumeRequested);
    }
    for (QPushButton* button : {simpleKeystoneNextButton_, advancedKeystoneNextButton_}) {
        connect(button, &QPushButton::clicked, this, &MainWindow::keystoneStepForwardRequested);
    }
    connect(presetList_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem* previous) {
                if (!gridBrowsing_) UpdateCurrentPresetUi(current, previous);
            });
    connect(presetList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item) ActivatePresetRow(presetList_->row(item));
    });
    connect(presetList_->model(), &QAbstractItemModel::rowsInserted,
            this, [this](const QModelIndex&, int first, int last) {
                for (int row = first; row <= last; ++row) {
                    QListWidgetItem* item = presetList_->item(row);
                    if (!item) continue;
                    const QString original = item->text();
                    const QString plain = PlainPresetLabel(original);
                    item->setData(Qt::AccessibleTextRole, plain);
                    item->setData(Qt::StatusTipRole, original);
                    item->setText(row < kSimpleShortcutCount
                                      ? QStringLiteral("%1\n%2").arg(row + 1).arg(plain)
                                      : plain);
                    item->setTextAlignment(Qt::AlignCenter);
                }
            });
    connect(recordButton_, &QPushButton::toggled, this, [this](bool checked) {
        recordButton_->setIcon(QIcon(checked ? QStringLiteral(":/okuflow/icons/record-stop.svg")
                                             : QStringLiteral(":/okuflow/icons/record.svg")));
    });

    qApp->installEventFilter(this);
    qApp->installNativeEventFilter(this);
    UpdateDirectionalUi();
    QTimer::singleShot(0, this, [this]() {
        UpdateSimpleChromeGeometry();
        RevealSimpleChrome();
    });

    connect(simpleModeButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            setSimpleMode(true);
        }
    });
    connect(advancedModeButton_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            setSimpleMode(false);
        }
    });
    connect(bumpHoldCheckbox_, &QCheckBox::toggled, this,
            [this](bool checked) {
                QAccessibleAnnouncementEvent announcement(
                    this, checked
                              ? QStringLiteral("Extra Stable on")
                              : QStringLiteral("Extra Stable off"));
                announcement.setPoliteness(
                    QAccessible::AnnouncementPoliteness::Polite);
                QAccessible::updateAccessibility(&announcement);
            });
    // Screen-reader metadata for every interactive control. Static accessible
    // names belong only on controls whose label never changes; dynamic text is
    // initialized and updated through SetLiveText so UIA receives its value.
    // Controls on the two settings tabs also state who owns their value, so a
    // screen-reader user hears the same scope a sighted user reads in the tab.
    auto setA11y = [this](QWidget* widget, const QString& name,
                          const QString& description) {
        widget->setAccessibleName(name);
        QString scope;
        if (!qobject_cast<QLabel*>(widget)) {
            if (sharedSettingsContent_ && sharedSettingsContent_->isAncestorOf(widget)) {
                scope = QStringLiteral("Shared setting, used by all modes.");
            } else if (controlsContainer_ && controlsContainer_->isAncestorOf(widget)) {
                scope = QStringLiteral("Saved with the current quick mode.");
            }
        }
        if (scope.isEmpty()) {
            widget->setAccessibleDescription(description);
            return;
        }
        scopedDescriptions_.push_back({widget, scope, description});
        widget->setAccessibleDescription(ScopedDescriptionText(scope, description));
    };
    setA11y(controlsToggleButton_, "Advanced Tuning",
            "Show or hide the advanced tuning controls");
    setA11y(promotePresetButton_, "Save as Quick Mode",
            "Save the current advanced settings as a reusable quick mode");
    setA11y(resetProfileButton_, "Reset current mode to defaults",
            "Resets this mode's image and assistant settings. Settings on the "
            "Settings tab are not changed.");
    setA11y(capturePhotoButton_, "Capture Photo",
            "Save synchronized original and processed camera photos");
    setA11y(recordButton_, "Record Video",
            "Start or stop synchronized original and processed MP4 recordings");
    setA11y(annotationButton_, "Draw on camera view",
            "Open annotation mode to mark the lecture view with scene-anchored ink.");
    setA11y(joystickCheckbox_, "Virtual Joystick",
            "Show an on-screen joystick overlay for panning the zoom focus.");
    setA11y(rotationCombo_, "Rotation",
            "Rotate the camera image clockwise in 90 degree steps.");
    setA11y(viewportRateCombo_, "Viewport motion rate",
            "Choose how smoothly pan and zoom move without changing the camera frame rate.");
    setA11y(viewportFitCombo_, "Viewport framing",
            "Choose Fill to crop without stretching or Fit to show the entire camera image.");
    setA11y(recordingCanvasCombo_, "Processed recording resolution",
            "Choose Match camera or a fixed resolution from 360p through Ultra HD "
            "for the processed MP4. The original MP4 uses the selected camera "
            "mode resolution.");
    setA11y(cameraCombo_, "Camera",
            "Select the active camera device.");
    setA11y(microphoneCombo_, "Recording microphone",
            "Select microphone audio for both original and processed MP4 "
            "recordings, or choose no microphone.");
    setA11y(transcribeMicrophoneCheckbox_, "Transcribe microphone while recording",
            "Sends microphone audio to Codex Voice for live transcription only "
            "while recording. Requires Codex signed in with ChatGPT. Recording "
            "works without it.");
    setA11y(transcriptToNotesCheckbox_, "Add finalized transcript to lecture notes",
            "Appends each finalized phrase to the HTML lecture notes. "
            "Effective only while lecture notes are enabled.");
    setA11y(transcriptionStatusLabel_, "Transcription status",
            "Reports the live transcription state. Recording is never "
            "affected by transcription problems.");
    setA11y(transcriptionQuotaLabel_, "Codex usage",
            "Shows the general Codex quota window. Voice-specific remaining "
            "time is not exposed by this Codex app-server.");
    setA11y(transcriptPartialLabel_, "Current phrase",
            "The phrase being transcribed right now. Partial text is "
            "presentation-only until it is finalized.");
    setA11y(transcriptFinalsView_, "Finalized transcript",
            "Read-only list of finalized transcript phrases from the "
            "current recording. Text is selectable.");
    setA11y(cameraFormatCombo_, "Resolution and frame rate",
            "Select the camera capture mode used by original photos and original video.");
    setA11y(cameraFormatNoticeLabel_, "Camera format notice",
            "Reports when the camera driver selected a different mode.");
    setA11y(zoomWheelAccelerationCheckbox_, "Zoom wheel acceleration",
            "Accelerate fast Control scroll gestures.");
    setA11y(presetList_, "Quick Modes",
            "Choose a task-oriented preset such as reading or high contrast");
    setA11y(bwCheckbox_, "Black and White",
            "Convert the image to thresholded black and white.");
    setA11y(bwSlider_, "Black and White Threshold",
            "Brightness threshold for the black and white conversion");
    setA11y(zoomCheckbox_, "Zoom",
            "Enable the magnifier.");
    setA11y(zoomSlider_, "Zoom Amount",
            "Magnification level");
    setA11y(blurCheckbox_, "Soften image (blur)",
            "Smooth the image with a Gaussian blur");
    setA11y(blurSigmaSlider_, "Blur Sigma",
            "Strength of the Gaussian blur");
    setA11y(blurRadiusSlider_, "Blur Radius",
            "Radius of the Gaussian blur in pixels");
    setA11y(temporalSmoothCheckbox_, "Temporal Smooth",
            "Reduce flicker by averaging consecutive frames");
    setA11y(temporalSmoothSlider_, "Temporal Smooth Blend",
            "How strongly new frames blend into the running average");
    setA11y(vlmAssistCheckbox_, "Scene Explain",
            "Describe the magnified scene using an AI vision model");
    setA11y(assistiveOverlayCheckbox_, "Assistive Overlay",
            "Show assistant results in an on-screen panel");
    setA11y(annotationCaptureOnExitCheckbox_, "Save drawing to notes when leaving Draw",
            "Save an annotated snapshot to lecture notes when Draw mode closes.");
    setA11y(spatialSharpenCheckbox_, "Spatial Sharpen",
            "Sharpen and upscale the image on the GPU");
    setA11y(spatialBackendCombo_, "Sharpen Backend",
            "Choose the GPU sharpening algorithm");
    setA11y(spatialSharpnessSlider_, "Sharpness",
            "Strength of the spatial sharpening");
    setA11y(zoomCenterXSlider_, "Horizontal position",
            "Horizontal position of the zoom focus");
    setA11y(zoomCenterYSlider_, "Vertical position",
            "Vertical position of the zoom focus");
    setA11y(debugButton_, "Debug View",
            "Show the intermediate processing stages in a grid");
    setA11y(focusMarkerCheckbox_, "Show Focus Point",
            "Overlay a marker at the current zoom focus");
    setA11y(processingStatusLabel_, "Processing Status",
            "Whether frames are processed on the CPU or the GPU");
    setA11y(simpleModeButton_, "Simple Mode",
            "Show the simple view with quick modes and large controls");
    setA11y(advancedModeButton_, "Advanced Mode",
            "Show the advanced view with every tuning control");
    setA11y(readTextButton_, "Read Text",
            "Read the current camera text with the vision assistant");
    setA11y(stabilizationCheckbox_, "Stabilize Image",
            "Lock the mounted camera view with full-strength CUDA stabilization");
    setA11y(bumpHoldCheckbox_, "Extra Stable (hold on shake)",
            "During a mounted-camera bump, temporarily show the last sharp stabilized frame and crossfade back when tracking recovers; moving people or content can be hidden while the frame is held");
    setA11y(keystoneCheckbox_, "Straighten Screen (Keystone)",
            "Automatically straighten a projected screen viewed at an angle");
    for (QPushButton* button : {simpleKeystoneBackButton_, advancedKeystoneBackButton_}) {
        setA11y(button, "Previous Screen Correction",
                "Freeze automatic screen correction and return to the previous accepted correction");
    }
    for (QPushButton* button : {simpleKeystonePauseButton_, advancedKeystonePauseButton_}) {
        setA11y(button, "Stop Automatic Screen Correction",
                "Freeze the current screen correction");
    }
    for (QPushButton* button : {simpleKeystoneNextButton_, advancedKeystoneNextButton_}) {
        setA11y(button, "Next Screen Correction",
                "Use the next saved correction or find one new correction while stopped");
    }
    setA11y(autoContrastCheckbox_, "Auto Contrast",
            "Automatically stretch washed-out colors for better readability");
    setA11y(autoContrastStrengthSlider_, "Auto Contrast Strength",
            "How strongly the automatic contrast correction is applied");
    setA11y(simpleTextClarityCheckbox_, "Text Clarity",
            "Automatically select the text clarity processing stack");
    setA11y(textClarityCheckbox_, "Text Clarity",
            "Automatically clarify text using local document analysis");
    setA11y(backgroundFlattenCheckbox_, "Flatten Background",
            "Remove shadows and uneven page lighting");
    setA11y(adaptiveBinarizationCheckbox_, "Adaptive Text",
            "Separate text from its local background using a Sauvola threshold");
    setA11y(textPolarityCombo_, "Text Polarity",
            "Choose automatic, dark on light, or light on dark text");
    setA11y(strokeWeightSlider_, "Stroke Weight",
            "Make text strokes thinner or bolder");
    setA11y(smartSharpenCheckbox_, "Smart Sharpen",
            "Denoise and sharpen text without bright edge halos");
    setA11y(claheCheckbox_, "Local Contrast",
            "Equalize contrast in separate image regions");
    setA11y(twoColorTextCheckbox_, "Two Color Reading",
            "Map detected ink and paper to the selected display colors");
    setA11y(textHysteresisCheckbox_, "Steady Text Edges",
            "Keep thresholded letter edges from flickering between frames");
    setA11y(selectiveSharpenCheckbox_, "Sharpen Text Only",
            "Apply sharpening near detected text strokes and preserve pictures");
    setA11y(focusDetectionCheckbox_, "Warn When Out of Focus",
            "Warn before reading when the camera image is too blurry");
    setA11y(glareSuppressionCheckbox_, "Suppress Glare",
            "Reduce small blown highlights on glossy pages and boards");
    setA11y(mlTextSuperResolutionCheckbox_, "ML Text Super Resolution",
            "Use NVIDIA Video Effects SuperRes when zoom is one point three three times or greater");
    setA11y(mlTextSuperResolutionStrengthSlider_, "Super Resolution Strength",
            "Set NVIDIA SuperRes enhancement strength for this preset");
    setA11y(mlTextSuperResolutionUltra1440pCheckbox_, "Ultra 1440p Super Resolution",
            "Use the full camera frame to build a separate high-resolution scene up to 1440p");
    setA11y(mlTextSuperResolutionPrefer2xCheckbox_, "Faster 2x Mode",
            "Optional speed mode with a narrower view. Leave off for maximum source detail");
    setA11y(displayColorPicker_, "Display Colors",
            "Choose a high contrast color scheme such as white on black");
    setA11y(contrastSlider_, "Contrast",
            "Contrast of the displayed image");
    setA11y(brightnessSlider_, "Brightness",
            "Brightness of the displayed image");
    setA11y(aiSettingsButton_, "AI Settings",
            "Configure the vision assistant and speech output");
    setA11y(openNotesButton_, "Open Notes",
            "Open the lecture notes file written by the assistive features");
    setA11y(openUserDataFolderButton_, "Open my OkuFlow folder",
            "Open the folder containing OkuFlow photos, recordings, notes, and analysis files");
    setA11y(changeUserDataFolderButton_, "Change OkuFlow folder",
            "Choose where OkuFlow saves new photos, recordings, notes, and analysis files");
    setA11y(setupAssistantButton_, "Setup and Downloads",
            "Set up Codex CLI and NVIDIA Video Effects");
    setA11y(assistantConnectionLabel_, "Codex Connection Status",
            "Current Codex app-server and ChatGPT account status");
    setA11y(assistantUsageLabel_, "Codex Usage",
            "Percentage remaining in the current Codex subscription usage window");
    setA11y(assistantConnectButton_, "Connect ChatGPT",
            "Sign in to Codex with a ChatGPT subscription");
    setA11y(assistantTranscript_, "Assistant Conversation",
            "Messages in the current OkuFlow assistant conversation");
    setA11y(assistantPromptEdit_, "Assistant Question",
            "Question for the OkuFlow vision assistant");
    setA11y(assistantAttachFrameCheckbox_, "Attach Current View",
            "Include the current processed camera frame with the question");
    setA11y(assistantSendButton_, "Send Question",
            "Send the question to the OkuFlow vision assistant");
    setA11y(assistantStopButton_, "Stop Assistant",
            "Stop the current assistant response");
    setA11y(assistantNewButton_, "New Conversation",
            "Start a new persistent OkuFlow assistant conversation");
    setA11y(assistantHistoryList_, "Assistant History",
            "OkuFlow assistant conversations saved by Codex");
    setA11y(assistantRenameButton_, "Rename Conversation",
            "Rename the selected assistant conversation");
    setA11y(assistantExportButton_, "Export Conversation",
            "Export the current assistant transcript to a text file");
    setA11y(assistantDeleteButton_, "Delete Conversation",
            "Permanently delete the selected assistant conversation");
    setA11y(modeGridButton_, "Show Quick Modes",
            "Open the grid of premade quick modes");
    setA11y(previousModeButton_, "Previous Quick Mode",
            "Apply the previous premade image setting");
    setA11y(currentModeButton_, "Current Quick Mode",
            "Show all premade quick modes");
    setA11y(nextModeButton_, "Next Quick Mode",
            "Apply the next premade image setting");
    // Explain's name, tooltip, and icon follow the explicit busy state.
    ApplyExplainBusyUi();
    SetLiveText(recordButton_, recordButton_->text(),
                LivePoliteness::kSilent, QStringLiteral("Record"));
    SetLiveText(cameraFormatNoticeLabel_, cameraFormatNoticeLabel_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("Camera format notice"));
    SetLiveText(processingStatusLabel_, processingStatusLabel_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("Pipeline status"));
    SetLiveText(assistantConnectionLabel_, assistantConnectionLabel_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("Codex connection status"));
    SetLiveText(assistantUsageLabel_, assistantUsageLabel_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("Codex usage"));
    SetLiveText(assistantConnectButton_, assistantConnectButton_->text(),
                LivePoliteness::kSilent,
                QStringLiteral("ChatGPT connection"));

    // Tab order is local to each frameless overlay window. The application
    // event filter below bridges those groups while Simple mode is active.
    QWidget::setTabOrder(simpleModeButton_, advancedModeButton_);
    QWidget::setTabOrder(advancedModeButton_, simpleTextClarityCheckbox_);
    QWidget::setTabOrder(modeGridButton_, previousModeButton_);
    QWidget::setTabOrder(previousModeButton_, currentModeButton_);
    QWidget::setTabOrder(currentModeButton_, nextModeButton_);
    QWidget::setTabOrder(simpleKeystoneBackButton_, simpleKeystonePauseButton_);
    QWidget::setTabOrder(simpleKeystonePauseButton_, simpleKeystoneNextButton_);
    QWidget::setTabOrder(capturePhotoButton_, recordButton_);
    QWidget::setTabOrder(recordButton_, explainNowButton_);
    QWidget::setTabOrder(explainNowButton_, readTextButton_);
    QWidget::setTabOrder(readTextButton_, annotationButton_);

    // Inside the inspector, keyboard order is each tab's visual order: its
    // search field, header controls, then section headers and contents
    // top to bottom (nested "Fine-tune text" right after Auto Text Clarity).
    for (QWidget* page : {imageTabPage_, settingsTabPage_}) {
        const std::vector<QWidget*> order = InspectorFocusOrder(page);
        for (std::size_t i = 1; i < order.size(); ++i) {
            QWidget::setTabOrder(order[i - 1], order[i]);
        }
    }

    // Start from the list's real selection. Restoring a configuration that
    // matches no preset leaves the current item null, which emits no change
    // signal, so the carousel must not depend on one to leave its placeholder.
    UpdateCurrentPresetUi(presetList_->currentItem(), nullptr);
    UpdateProfileButtonLayout();

    auto* searchShortcut = new QShortcut(QKeySequence::Find, this);
    connect(searchShortcut, &QShortcut::activated, this, [this]() {
        setUiHidden(false);
        setSimpleMode(false);
        if (advancedTabs_->currentWidget() != settingsTabPage_) advancedTabs_->setCurrentWidget(imageTabPage_);
        auto* edit = advancedTabs_->currentWidget() == settingsTabPage_ ? sharedSearchEdit_ : imageSearchEdit_;
        activateWindow();
        edit->setFocus(Qt::ShortcutFocusReason);
        edit->selectAll();
    });
    setCentralWidget(central);
    setSimpleMode(true);
}

MainWindow::~MainWindow()
{
    if (qApp) {
        qApp->removeNativeEventFilter(this);
        qApp->removeEventFilter(this);
    }
}

QMap<QString, bool> MainWindow::sectionStates() const
{
    QMap<QString, bool> states;
    const std::array<CollapsibleSection*, 16> sections{
        applicationSection_, deviceSection_, deviceMoreSection_, recordingSection_, magnificationSection_,
        readabilitySection_, stabilitySection_, screenFixSection_,
        textClaritySection_, assistantSection_, sharpeningSection_,
        diagnosticsSection_, textClarityFineSection_, notesFilesSection_,
        aiDownloadsSection_, troubleshootingSection_};
    for (const CollapsibleSection* section : sections) {
        if (section && !section->persistKey().isEmpty()) {
            states.insert(section->persistKey(), section->persistedExpanded());
        }
    }
    return states;
}

void MainWindow::setSectionStates(const QMap<QString, bool>& states)
{
    const std::array<CollapsibleSection*, 16> sections{
        applicationSection_, deviceSection_, deviceMoreSection_, recordingSection_, magnificationSection_,
        readabilitySection_, stabilitySection_, screenFixSection_,
        textClaritySection_, assistantSection_, sharpeningSection_,
        diagnosticsSection_, textClarityFineSection_, notesFilesSection_,
        aiDownloadsSection_, troubleshootingSection_};
    for (CollapsibleSection* section : sections) {
        if (section && states.contains(section->persistKey())) {
            const QSignalBlocker blocker(section);
            section->setExpanded(states.value(section->persistKey()));
        }
    }
}

void MainWindow::updateSectionChangedCounts(
    const settings::AdvancedConfig& current,
    const settings::AdvancedConfig& defaults)
{
    auto different = [](auto lhs, auto rhs) {
        if constexpr (std::is_floating_point_v<decltype(lhs)>) {
            return std::abs(lhs - rhs) > 0.0001f;
        }
        return lhs != rhs;
    };

    const int magnificationChanges =
        different(current.zoomEnabled, defaults.zoomEnabled) +
        different(current.zoomAmount, defaults.zoomAmount) +
        different(current.zoomCenterX, defaults.zoomCenterX) +
        different(current.zoomCenterY, defaults.zoomCenterY) +
        different(current.focusMarker, defaults.focusMarker);
    magnificationSection_->setChangedCount(magnificationChanges);

    const int readabilityChanges =
        different(current.blackWhiteEnabled, defaults.blackWhiteEnabled) +
        different(current.blackWhiteThreshold, defaults.blackWhiteThreshold) +
        different(current.autoContrastEnabled, defaults.autoContrastEnabled) +
        different(current.autoContrastStrength, defaults.autoContrastStrength) +
        different(current.displayColorMode, defaults.displayColorMode) +
        different(current.contrast, defaults.contrast) +
        different(current.brightness, defaults.brightness);
    readabilitySection_->setChangedCount(readabilityChanges);

    const int stabilityChanges =
        different(current.stabilizationEnabled, defaults.stabilizationEnabled) +
        different(current.temporalSmoothEnabled, defaults.temporalSmoothEnabled) +
        different(current.temporalSmoothAlpha, defaults.temporalSmoothAlpha);
    stabilitySection_->setChangedCount(stabilityChanges);

    screenFixSection_->setChangedCount(
        different(current.keystoneEnabled, defaults.keystoneEnabled));

    const int textChanges =
        different(current.autoTextClarityEnabled, defaults.autoTextClarityEnabled) +
        different(current.backgroundFlattenEnabled, defaults.backgroundFlattenEnabled) +
        different(current.backgroundFlattenStrength, defaults.backgroundFlattenStrength) +
        different(current.adaptiveBinarizationEnabled, defaults.adaptiveBinarizationEnabled) +
        different(current.sauvolaStrength, defaults.sauvolaStrength) +
        different(current.binarizationSoftness, defaults.binarizationSoftness) +
        different(current.textPolarityMode, defaults.textPolarityMode) +
        different(current.strokeWeight, defaults.strokeWeight) +
        different(current.smartSharpenEnabled, defaults.smartSharpenEnabled) +
        different(current.smartSharpenStrength, defaults.smartSharpenStrength) +
        different(current.claheEnabled, defaults.claheEnabled) +
        different(current.claheClipLimit, defaults.claheClipLimit) +
        different(current.twoColorTextEnabled, defaults.twoColorTextEnabled) +
        different(current.textHysteresisEnabled, defaults.textHysteresisEnabled) +
        different(current.textHysteresisStrength, defaults.textHysteresisStrength) +
        different(current.selectiveSharpenEnabled, defaults.selectiveSharpenEnabled) +
        different(current.focusDetectionEnabled, defaults.focusDetectionEnabled) +
        different(current.focusThreshold, defaults.focusThreshold) +
        different(current.glareSuppressionEnabled, defaults.glareSuppressionEnabled) +
        different(current.glareSuppressionStrength,
                  defaults.glareSuppressionStrength);
    textClaritySection_->setChangedCount(textChanges);
    textClarityFineSection_->setChangedCount(textChanges -
        different(current.autoTextClarityEnabled, defaults.autoTextClarityEnabled));

    const int assistantChanges =
        different(current.vlmAssistEnabled, defaults.vlmAssistEnabled) +
        different(current.assistiveOverlayEnabled,
                  defaults.assistiveOverlayEnabled);
    assistantSection_->setChangedCount(assistantChanges);

    const int sharpeningChanges =
        different(current.blurEnabled, defaults.blurEnabled) +
        different(current.blurSigma, defaults.blurSigma) +
        different(current.blurRadius, defaults.blurRadius) +
        different(current.spatialSharpenEnabled, defaults.spatialSharpenEnabled) +
        different(current.spatialUpscaler, defaults.spatialUpscaler) +
        different(current.spatialSharpness, defaults.spatialSharpness) +
        different(current.mlSuperResEnabled, defaults.mlSuperResEnabled) +
        different(current.mlSuperResStrength, defaults.mlSuperResStrength) +
        different(current.mlSuperResPrefer2x, defaults.mlSuperResPrefer2x) +
        different(current.mlSuperResUltra1440p, defaults.mlSuperResUltra1440p);
    sharpeningSection_->setChangedCount(sharpeningChanges);

    const int diagnosticChanges =
        different(current.debugView, defaults.debugView);
    diagnosticsSection_->setChangedCount(diagnosticChanges);

    int deviceMoreChanges = 0;
    if (viewportRateCombo_ && viewportRateCombo_->currentIndex() != 0) {
        ++deviceMoreChanges;
    }
    if (viewportFitCombo_ && viewportFitCombo_->currentIndex() != 0) {
        ++deviceMoreChanges;
    }
    if (joystickCheckbox_ && joystickCheckbox_->isChecked()) {
        ++deviceMoreChanges;
    }
    if (zoomWheelAccelerationCheckbox_ &&
        !zoomWheelAccelerationCheckbox_->isChecked()) {
        ++deviceMoreChanges;
    }
    deviceMoreSection_->setChangedCount(deviceMoreChanges);

    refreshSliderReadouts();

}

std::vector<CollapsibleSection*> MainWindow::SettingsSections(SettingsScope scope) const
{
    if (scope == SettingsScope::kImage) {
        return {magnificationSection_, readabilitySection_, textClaritySection_,
                stabilitySection_, screenFixSection_, sharpeningSection_,
                assistantSection_, diagnosticsSection_};
    }
    return {deviceSection_, deviceMoreSection_, recordingSection_, notesFilesSection_,
            applicationSection_, aiDownloadsSection_, troubleshootingSection_};
}

std::vector<QWidget*> MainWindow::InspectorFocusOrder(QWidget* page) const
{
    std::vector<QWidget*> order;
    const std::function<void(QWidget*)> visit = [&](QWidget* widget) {
        if (!widget) return;
        if ((widget->focusPolicy() & Qt::TabFocus) &&
            std::find(order.begin(), order.end(), widget) == order.end()) {
            order.push_back(widget);
        }
        if (auto* tabs = qobject_cast<QTabWidget*>(widget)) {
            visit(tabs->tabBar());
            visit(tabs->currentWidget());
            return;
        }
        if (auto* scroll = qobject_cast<QScrollArea*>(widget)) {
            visit(scroll->widget());
            return;
        }
        const std::function<void(QLayout*)> walk = [&](QLayout* layout) {
            if (!layout) return;
            for (int i = 0; i < layout->count(); ++i) {
                auto* item = layout->itemAt(i);
                if (item->widget()) visit(item->widget());
                else if (item->layout()) walk(item->layout());
            }
        };
        walk(widget->layout());
    };
    visit(page);
    return order;
}

void MainWindow::FilterSettingsTab(SettingsScope scope, const QString& query)
{
    const QString needle = query.trimmed().toCaseFolded();
    const bool searching = !needle.isEmpty();
    QWidget*& firstMatch = scope == SettingsScope::kImage
                              ? firstImageSearchMatch_ : firstSharedSearchMatch_;
    firstMatch = nullptr;
    int matches = 0;
    const auto widgetText = [](QWidget* widget) {
        QString text = widget->accessibleName() + QLatin1Char(' ') + widget->toolTip();
        if (auto* label = qobject_cast<QLabel*>(widget)) text += QLatin1Char(' ') + label->text();
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) text += QLatin1Char(' ') + button->text();
        if (auto* combo = qobject_cast<QComboBox*>(widget)) text += QLatin1Char(' ') + combo->currentText();
        return text.toCaseFolded();
    };
    const auto eligible = [](QWidget* widget) {
        return widget && widget->isEnabled() && !widget->isHidden() &&
               (widget->focusPolicy() & Qt::TabFocus);
    };
    for (auto* section : SettingsSections(scope)) {
        if (!section) continue;
        const bool headingMatch = searching && widgetText(section->headerWidget()).contains(needle);
        bool sectionMatch = !searching || headingMatch;
        QWidget* target = headingMatch ? section->headerWidget() : nullptr;
        bool fineMatch = false;
        if (headingMatch) ++matches;
        const auto widgets = section->contentWidget()->findChildren<QWidget*>();
        for (auto* widget : widgets) {
            if (widget->objectName() == QStringLiteral("sliderValueLabel")) continue;
            if (!searching || !widgetText(widget).contains(needle)) continue;
            sectionMatch = true;
            ++matches;
            if (textClarityFineSection_ && textClarityFineSection_->isAncestorOf(widget)) fineMatch = true;
            if (!target && eligible(widget)) target = widget;
            if (!target) {
                if (auto* label = qobject_cast<QLabel*>(widget); label && eligible(label->buddy())) {
                    target = label->buddy();
                }
            }
        }
        section->setVisible(sectionMatch);
        section->setSearchExpanded(searching && sectionMatch);
        if (section == textClaritySection_) {
            textClarityFineSection_->setSearchExpanded(searching && fineMatch);
        }
        if (searching && sectionMatch && !firstMatch) {
            firstMatch = target ? target : section->headerWidget();
        }
    }
    auto* edit = scope == SettingsScope::kImage ? imageSearchEdit_ : sharedSearchEdit_;
    auto* status = scope == SettingsScope::kImage ? imageSearchStatusLabel_ : sharedSearchStatusLabel_;
    QString result = matches > 0 ? QStringLiteral("%1 settings match").arg(matches)
                                 : QStringLiteral("No matching settings");
    if (searching && matches == 0) {
        // Inspect the other scope without changing its query, disclosure, or
        // visibility. Only Enter performs the cross-tab handoff.
        const auto otherScope = scope == SettingsScope::kImage
                                    ? SettingsScope::kShared : SettingsScope::kImage;
        int otherMatches = 0;
        for (auto* section : SettingsSections(otherScope)) {
            if (!section) continue;
            if (widgetText(section->headerWidget()).contains(needle)) ++otherMatches;
            for (auto* widget : section->contentWidget()->findChildren<QWidget*>()) {
                if (widget->objectName() != QStringLiteral("sliderValueLabel") &&
                    widgetText(widget).contains(needle)) ++otherMatches;
            }
        }
        if (otherMatches > 0) {
            result = (scope == SettingsScope::kImage
                ? QStringLiteral("No matches here. %1 in Settings. Press Enter to show them.")
                : QStringLiteral("No matches here. %1 in Image. Press Enter to show them."))
                .arg(otherMatches);
        }
    }
    if (edit) {
        SetLiveAccessibleDescription(edit, searching ? result
            : QStringLiteral("Type part of a setting name to reveal and focus matching controls."));
        QAccessibleEvent event(edit, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
    if (status) {
        status->setVisible(searching);
        if (searching) SetLiveText(status, result, LivePoliteness::kSilent,
                                  QStringLiteral("Settings search"));
    }
}

void MainWindow::ActivateSearchResult(SettingsScope scope)
{
    QWidget* target = scope == SettingsScope::kImage ? firstImageSearchMatch_ : firstSharedSearchMatch_;
    if (!target) {
        const auto other = scope == SettingsScope::kImage ? SettingsScope::kShared : SettingsScope::kImage;
        auto* source = scope == SettingsScope::kImage ? imageSearchEdit_ : sharedSearchEdit_;
        auto* destination = other == SettingsScope::kImage ? imageSearchEdit_ : sharedSearchEdit_;
        if (!source || source->text().trimmed().isEmpty()) return;
        destination->setText(source->text());
        FilterSettingsTab(other, source->text());
        target = other == SettingsScope::kImage ? firstImageSearchMatch_ : firstSharedSearchMatch_;
        if (!target) return;
        scope = other;
    }
    advancedTabs_->setCurrentWidget(scope == SettingsScope::kImage ? imageTabPage_ : settingsTabPage_);
    if (target->isVisible() && target->isEnabled() && (target->focusPolicy() & Qt::TabFocus)) {
        target->window()->activateWindow();
        target->setFocus(Qt::ShortcutFocusReason);
        auto* scroll = scope == SettingsScope::kImage ? advancedScroll_ : sharedSettingsScroll_;
        scroll->ensureWidgetVisible(target, 8, 8);
    }
}

QString MainWindow::ScopedDescriptionText(const QString& scope, const QString& description) const
{
    return TranslateUi(scope) + QLatin1Char(' ') + TranslateUi(description);
}

void MainWindow::UpdateAdvancedTabToolTips()
{
    if (!advancedTabs_) return;
    for (int index = 0; index < advancedTabs_->count(); ++index) {
        advancedTabs_->setTabToolTip(index, advancedTabs_->tabText(index));
    }
}

void MainWindow::UpdateProfileButtonLayout()
{
    if (!profileButtonsLayout_ || !imageTabPage_) return;
    const int required = promotePresetButton_->sizeHint().width() +
                         resetProfileButton_->sizeHint().width() + 32;
    profileButtonsLayout_->setDirection(imageTabPage_->width() < required
        ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}

void MainWindow::refreshTextClarityUi()
{
    if (!textClarityCheckbox_ || !textClarityFineSection_) return;
    const QString explanation = TranslateUi(QStringLiteral(
        "Text Clarity applies automatic enhancement. Fine-tune options apply only while Text Clarity is on."));
    if (textClarityHelp_) textClarityHelp_->setText(explanation);
    textClarityCheckbox_->setToolTip(explanation);
    textClarityFineSection_->headerWidget()->setToolTip(explanation);
    textClarityFineSection_->contentWidget()->setAccessibleDescription(explanation);
    textClarityFineSection_->contentWidget()->setEnabled(textClarityCheckbox_->isChecked());
}

void MainWindow::refreshSliderReadouts()
{
    for (const auto& refresh : sliderReadoutRefreshers_) refresh();
}

void MainWindow::setExplainBusy(bool busy)
{
    if (explainBusy_ == busy) return;
    explainBusy_ = busy;
    ApplyExplainBusyUi();
    UpdateSimpleChromeGeometry();
}

void MainWindow::ApplyExplainBusyUi()
{
    if (!explainNowButton_) return;
    const QString title = explainBusy_ ? QStringLiteral("Stop") : QStringLiteral("Explain");
    explainNowButton_->setIcon(QIcon(explainBusy_ ? QStringLiteral(":/okuflow/icons/record-stop.svg")
                                                : QStringLiteral(":/okuflow/icons/explain.svg")));
    SetLiveText(explainNowButton_, title, LivePoliteness::kSilent,
                explainBusy_ ? QStringLiteral("Stop scene explanation") : QStringLiteral("Explain scene"));
    explainNowButton_->setToolTip(TranslateUi(explainBusy_
        ? QStringLiteral("Stop the current scene explanation")
        : QStringLiteral("Describe the current camera view")));
}

void MainWindow::CloseModeGrid(bool restoreSelection)
{
    if (!modeGridPopup_) return;
    const bool hadVisibleGrid = modeGridPopup_->isVisible();
    if (restoreSelection && presetList_) {
        const QSignalBlocker blocker(presetList_);
        presetList_->setCurrentRow(gridOriginalRow_);
    }
    gridBrowsing_ = false;
    if (hadVisibleGrid && !uiHidden_) {
        // Keep an owned window active while the native grid disappears.
        DWORD foregroundProcess = 0;
        const HWND foreground = GetForegroundWindow();
        if (foreground && GetWindowThreadProcessId(foreground, &foregroundProcess) &&
            foregroundProcess == GetCurrentProcessId()) {
            auto* owner = currentModeButton_->window();
            SetForegroundWindow(reinterpret_cast<HWND>(owner->winId()));
            owner->activateWindow();
            QApplication::setActiveWindow(owner);
            currentModeButton_->setFocus(Qt::PopupFocusReason);
        }
    }
    modeGridPopup_->hide();
    RevealSimpleChrome();
    if (!hadVisibleGrid) return;
    const auto restoreFocus = [this]() {
        if (uiHidden_ || !isVisible() || !currentModeButton_ ||
            modeGridPopup_->isVisible() || QApplication::activeModalWidget() ||
            QApplication::activePopupWidget()) return;
        // A tool-window hide may temporarily leave Qt without an active
        // window. Only restore when the foreground still belongs to OkuFlow.
        DWORD foregroundProcess = 0;
        const HWND foreground = GetForegroundWindow();
        if (!foreground || !GetWindowThreadProcessId(foreground, &foregroundProcess) ||
            foregroundProcess != GetCurrentProcessId()) return;
        QWidget* active = QApplication::activeWindow();
        if (active && active != this && active != topLeftPanel_ &&
            active != bottomLeftPanel_ && active != bottomRightPanel_ &&
            active != keystoneTrackingPanel_ && active != modeGridPopup_ &&
            active != uiVisibilityPanel_) return;
        if (!bottomLeftPanel_->isVisible()) {
            bottomLeftPanel_->setWindowOpacity(1.0);
            bottomLeftPanel_->show();
        }
        currentModeButton_->window()->activateWindow();
        currentModeButton_->setFocus(Qt::PopupFocusReason);
    };
    // Native Tool-window hiding may post an activation event after hide()
    // returns. Restore after that transition and again after activation settles.
    QTimer::singleShot(0, this, [this, restoreFocus]() {
        restoreFocus();
        QTimer::singleShot(0, this, restoreFocus);
    });
}

void MainWindow::FocusRegion(bool backwards)
{
    // A narrow carousel hides the grid button; its region then starts at
    // the current-mode button, which opens the same grid.
    QWidget* carouselRegion = modeGridButton_->isVisibleTo(bottomLeftPanel_)
        ? static_cast<QWidget*>(modeGridButton_) : currentModeButton_;
    std::vector<QWidget*> regions{renderWidget_, simpleModeButton_, carouselRegion, capturePhotoButton_};
    if (!isSimpleMode() && advancedTabs_) regions.push_back(advancedTabs_->tabBar());
    if (annotationOverlay_ && annotationOverlay_->IsActive()) {
        const auto targets = annotationOverlay_->FocusTargets();
        if (!targets.empty()) regions.push_back(targets.front());
    }
    if (auto* overlay = findChild<AssistiveOverlay*>(); overlay && overlay->isVisible()) {
        const auto targets = overlay->FocusTargets();
        if (!targets.empty()) regions.push_back(targets.front());
    }
    regions.push_back(uiVisibilityButton_);
    QWidget* focus = QApplication::focusWidget();
    int current = -1;
    for (int i = 0; i < static_cast<int>(regions.size()); ++i) {
        if (focus && regions[i] && (focus->window() == regions[i]->window())) current = i;
    }
    if (!isSimpleMode() && focus && advancedPanel_->isAncestorOf(focus)) current = 4;
    else if (focus == renderWidget_) current = 0;
    const int count = static_cast<int>(regions.size());
    for (int step = 1; step <= count; ++step) {
        auto* target = regions[(current + (backwards ? -step : step) + count * 2) % count];
        if (!target || !target->isVisible() || !target->isEnabled()) continue;
        target->window()->activateWindow();
        target->setFocus(Qt::ShortcutFocusReason);
        return;
    }
}

void MainWindow::setApp(OkuFlowApp* app)
{
    app_ = app;
}

RenderWidget* MainWindow::renderWidget() const
{
    return renderWidget_;
}

QComboBox* MainWindow::cameraCombo() const { return cameraCombo_; }
QComboBox* MainWindow::microphoneCombo() const { return microphoneCombo_; }
QListWidget* MainWindow::presetList() const { return presetList_; }
QLabel* MainWindow::presetDescriptionLabel() const { return presetDescriptionLabel_; }
QPushButton* MainWindow::promotePresetButton() const { return promotePresetButton_; }
QCheckBox* MainWindow::blackWhiteCheckbox() const { return bwCheckbox_; }
QSlider* MainWindow::blackWhiteSlider() const { return bwSlider_; }
QCheckBox* MainWindow::zoomCheckbox() const { return zoomCheckbox_; }
QSlider* MainWindow::zoomSlider() const { return zoomSlider_; }
QPushButton* MainWindow::debugButton() const { return debugButton_; }
QComboBox* MainWindow::rotationCombo() const { return rotationCombo_; }
QComboBox* MainWindow::viewportRateCombo() const { return viewportRateCombo_; }
QComboBox* MainWindow::viewportFitCombo() const { return viewportFitCombo_; }
QComboBox* MainWindow::cameraAccelerationCombo() const {
    return cameraAccelerationCombo_;
}
QLabel* MainWindow::cameraAccelerationStatusLabel() const {
    return cameraAccelerationStatusLabel_;
}
QPushButton* MainWindow::testCameraAccelerationButton() const {
    return testCameraAccelerationButton_;
}
QComboBox* MainWindow::recordingCanvasCombo() const {
    return recordingCanvasCombo_;
}
QComboBox* MainWindow::applicationLanguageCombo() const {
    return applicationLanguageCombo_;
}
QComboBox* MainWindow::cameraFormatCombo() const { return cameraFormatCombo_; }
QLabel* MainWindow::cameraFormatNoticeLabel() const { return cameraFormatNoticeLabel_; }
QCheckBox* MainWindow::zoomWheelAccelerationCheckbox() const {
    return zoomWheelAccelerationCheckbox_;
}
QCheckBox* MainWindow::focusMarkerCheckbox() const { return focusMarkerCheckbox_; }
QSlider* MainWindow::zoomCenterXSlider() const { return zoomCenterXSlider_; }
QSlider* MainWindow::zoomCenterYSlider() const { return zoomCenterYSlider_; }
QCheckBox* MainWindow::joystickCheckbox() const { return joystickCheckbox_; }
QToolButton* MainWindow::controlsToggleButton() const { return controlsToggleButton_; }
QWidget* MainWindow::controlsContainer() const { return controlsContainer_; }
QCheckBox* MainWindow::blurCheckbox() const { return blurCheckbox_; }
QSlider* MainWindow::blurSigmaSlider() const { return blurSigmaSlider_; }
QSlider* MainWindow::blurRadiusSlider() const { return blurRadiusSlider_; }
QLabel* MainWindow::blurSigmaValueLabel() const { return blurSigmaValueLabel_; }
QLabel* MainWindow::blurRadiusValueLabel() const { return blurRadiusValueLabel_; }
QPushButton* MainWindow::capturePhotoButton() const { return capturePhotoButton_; }
QPushButton* MainWindow::recordButton() const { return recordButton_; }
QPushButton* MainWindow::annotationButton() const { return annotationButton_; }
AnnotationOverlay* MainWindow::annotationOverlay() const { return annotationOverlay_; }
QCheckBox* MainWindow::temporalSmoothCheckbox() const { return temporalSmoothCheckbox_; }
QSlider* MainWindow::temporalSmoothSlider() const { return temporalSmoothSlider_; }
QLabel* MainWindow::temporalSmoothValueLabel() const { return temporalSmoothValueLabel_; }
QCheckBox* MainWindow::vlmAssistCheckbox() const { return vlmAssistCheckbox_; }
QCheckBox* MainWindow::assistiveOverlayCheckbox() const { return assistiveOverlayCheckbox_; }
QCheckBox* MainWindow::spatialSharpenCheckbox() const { return spatialSharpenCheckbox_; }
QComboBox* MainWindow::spatialBackendCombo() const { return spatialBackendCombo_; }
QSlider* MainWindow::spatialSharpnessSlider() const { return spatialSharpnessSlider_; }
QLabel* MainWindow::spatialSharpnessValueLabel() const { return spatialSharpnessValueLabel_; }
QLabel* MainWindow::processingStatusLabel() const { return processingStatusLabel_; }
QLabel* MainWindow::performanceDiagnosticsLabel() const
{
    return performanceDiagnosticsLabel_;
}
QAbstractButton* MainWindow::simpleModeButton() const { return simpleModeButton_; }
QAbstractButton* MainWindow::advancedModeButton() const { return advancedModeButton_; }
QPushButton* MainWindow::explainNowButton() const { return explainNowButton_; }
QPushButton* MainWindow::readTextButton() const { return readTextButton_; }
QCheckBox* MainWindow::transcribeMicrophoneCheckbox() const
{
    return transcribeMicrophoneCheckbox_;
}
QCheckBox* MainWindow::transcriptToNotesCheckbox() const
{
    return transcriptToNotesCheckbox_;
}
QLabel* MainWindow::transcriptionStatusLabel() const { return transcriptionStatusLabel_; }
QLabel* MainWindow::transcriptionQuotaLabel() const { return transcriptionQuotaLabel_; }
QLabel* MainWindow::transcriptPartialLabel() const { return transcriptPartialLabel_; }
QPlainTextEdit* MainWindow::transcriptFinalsView() const { return transcriptFinalsView_; }

void MainWindow::SetSimpleTranscriptActive(bool active)
{
    if (simpleTranscriptActive_ == active && simpleTranscriptPanel_ &&
        simpleTranscriptPanel_->isVisible() == (active && isSimpleMode() && !uiHidden_)) {
        return;
    }
    simpleTranscriptActive_ = active;
    if (!active) {
        simpleTranscriptPartial_.clear();
        simpleTranscriptFinals_.clear();
        RefreshSimpleTranscriptText();
    }
    if (simpleTranscriptPanel_) {
        simpleTranscriptPanel_->setVisible(active && isSimpleMode() && !uiHidden_);
        if (simpleTranscriptPanel_->isVisible()) {
            UpdateSimpleChromeGeometry();
        }
    }
}

void MainWindow::SetSimpleTranscriptPartial(const QString& text)
{
    if (simpleTranscriptPartial_ == text) {
        return;
    }
    simpleTranscriptPartial_ = text;
    RefreshSimpleTranscriptText();
}

void MainWindow::AppendSimpleTranscriptFinal(const QString& text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }
    simpleTranscriptFinals_.append(text);
    while (simpleTranscriptFinals_.size() > 3) {
        simpleTranscriptFinals_.removeFirst();
    }
    RefreshSimpleTranscriptText();
}

void MainWindow::RefreshSimpleTranscriptText()
{
    if (!simpleTranscriptLabel_) {
        return;
    }
    QStringList lines = simpleTranscriptFinals_;
    if (!simpleTranscriptPartial_.trimmed().isEmpty()) {
        lines.append(simpleTranscriptPartial_);
    }
    // Silent update: plain text only, no accessible announcement per delta.
    simpleTranscriptLabel_->setText(lines.join(QStringLiteral("\n")));
    if (simpleTranscriptPanel_ && simpleTranscriptPanel_->isVisible()) {
        UpdateSimpleChromeGeometry();
    }
}

void MainWindow::setAnnotationPreferences(const QColor& color,
                                          int widthPixels,
                                          bool captureOnExit,
                                          bool dashed,
                                          const QString& shapeKind,
                                          int textSizePixels)
{
    if (annotationCaptureOnExitCheckbox_) {
        const QSignalBlocker blocker(annotationCaptureOnExitCheckbox_);
        annotationCaptureOnExitCheckbox_->setChecked(captureOnExit);
    }
    if (annotationOverlay_) {
        annotationOverlay_->SetPreferences(color,
                                           widthPixels,
                                           captureOnExit,
                                           dashed,
                                           shapeKind,
                                           textSizePixels);
    }
}

void MainWindow::setAnnotationViewTransform(const ViewTransform& transform)
{
    if (annotationOverlay_) {
        annotationOverlay_->SetViewTransform(transform);
    }
}

void MainWindow::setAnnotationMode(bool enabled)
{
    if (!annotationOverlay_ || !annotationButton_) {
        return;
    }
    if (!enabled) {
        annotationOverlay_->CommitPendingText();
    }
    changingAnnotationMode_ = true;
    annotationButton_->setChecked(enabled);
    changingAnnotationMode_ = false;
    annotationOverlay_->SetActive(enabled);
    if (uiHidden_) {
        annotationOverlay_->hide();
        return;
    }
    if (enabled) {
        chromePinned_ = true;
        RevealSimpleChrome();
        // The annotation canvas is a transparent top-level tool window. Keep
        // the persistent action bar above it so visible Photo, Record/Stop,
        // Explain, Read, and Draw controls remain clickable in both modes.
        if (bottomRightPanel_) {
            bottomRightPanel_->show();
            bottomRightPanel_->raise();
        }
        RaiseChromeAboveCanvas();
    } else {
        chromePinned_ = false;
        annotationButton_->setFocus(Qt::OtherFocusReason);
        if (isSimpleMode()) {
            simpleChromeIdleTimer_->start();
        }
    }
}
QCheckBox* MainWindow::stabilizationCheckbox() const { return stabilizationCheckbox_; }
QCheckBox* MainWindow::bumpHoldCheckbox() const {
    return bumpHoldCheckbox_;
}
QCheckBox* MainWindow::keystoneCheckbox() const { return keystoneCheckbox_; }
QCheckBox* MainWindow::autoContrastCheckbox() const { return autoContrastCheckbox_; }
QSlider* MainWindow::autoContrastStrengthSlider() const { return autoContrastStrengthSlider_; }
QCheckBox* MainWindow::simpleTextClarityCheckbox() const { return simpleTextClarityCheckbox_; }
QCheckBox* MainWindow::textClarityCheckbox() const { return textClarityCheckbox_; }
QCheckBox* MainWindow::backgroundFlattenCheckbox() const { return backgroundFlattenCheckbox_; }
QSlider* MainWindow::backgroundFlattenStrengthSlider() const { return backgroundFlattenStrengthSlider_; }
QCheckBox* MainWindow::adaptiveBinarizationCheckbox() const { return adaptiveBinarizationCheckbox_; }
QSlider* MainWindow::sauvolaStrengthSlider() const { return sauvolaStrengthSlider_; }
QSlider* MainWindow::binarizationSoftnessSlider() const { return binarizationSoftnessSlider_; }
QComboBox* MainWindow::textPolarityCombo() const { return textPolarityCombo_; }
QSlider* MainWindow::strokeWeightSlider() const { return strokeWeightSlider_; }
QCheckBox* MainWindow::smartSharpenCheckbox() const { return smartSharpenCheckbox_; }
QSlider* MainWindow::smartSharpenStrengthSlider() const { return smartSharpenStrengthSlider_; }
QCheckBox* MainWindow::claheCheckbox() const { return claheCheckbox_; }
QSlider* MainWindow::claheClipLimitSlider() const { return claheClipLimitSlider_; }
QCheckBox* MainWindow::twoColorTextCheckbox() const { return twoColorTextCheckbox_; }
QCheckBox* MainWindow::textHysteresisCheckbox() const { return textHysteresisCheckbox_; }
QSlider* MainWindow::textHysteresisStrengthSlider() const { return textHysteresisStrengthSlider_; }
QCheckBox* MainWindow::selectiveSharpenCheckbox() const { return selectiveSharpenCheckbox_; }
QCheckBox* MainWindow::focusDetectionCheckbox() const { return focusDetectionCheckbox_; }
QSlider* MainWindow::focusThresholdSlider() const { return focusThresholdSlider_; }
QCheckBox* MainWindow::glareSuppressionCheckbox() const { return glareSuppressionCheckbox_; }
QSlider* MainWindow::glareSuppressionStrengthSlider() const { return glareSuppressionStrengthSlider_; }
QCheckBox* MainWindow::mlTextSuperResolutionCheckbox() const { return mlTextSuperResolutionCheckbox_; }
QSlider* MainWindow::mlTextSuperResolutionStrengthSlider() const { return mlTextSuperResolutionStrengthSlider_; }
QCheckBox* MainWindow::mlTextSuperResolutionPrefer2xCheckbox() const {
    return mlTextSuperResolutionPrefer2xCheckbox_;
}
QCheckBox* MainWindow::mlTextSuperResolutionUltra1440pCheckbox() const {
    return mlTextSuperResolutionUltra1440pCheckbox_;
}
ColorSchemePicker* MainWindow::displayColorPicker() const { return displayColorPicker_; }
QSlider* MainWindow::contrastSlider() const { return contrastSlider_; }
QSlider* MainWindow::brightnessSlider() const { return brightnessSlider_; }
QPushButton* MainWindow::aiSettingsButton() const { return aiSettingsButton_; }
QPushButton* MainWindow::openNotesButton() const { return openNotesButton_; }
QPushButton* MainWindow::setupAssistantButton() const { return setupAssistantButton_; }

void MainWindow::setMaxineRuntimeInstalled(bool installed)
{
    maxineRuntimeInstalled_ = installed;
#if OKUFLOW_ENABLE_TEXT_SR
    if (mlTextSuperResolutionCheckbox_) {
        mlTextSuperResolutionCheckbox_->setEnabled(installed);
        mlTextSuperResolutionCheckbox_->setToolTip(
            installed
                ? QStringLiteral("Use NVIDIA Video Effects SuperRes at 1.33x zoom and above; "
                                 "falls back to NIS automatically")
                : QStringLiteral("NVIDIA Video Effects runtime is not installed; "
                                 "open Setup & Downloads"));
    }
    if (mlTextSuperResolutionStrengthSlider_) {
        mlTextSuperResolutionStrengthSlider_->setEnabled(
            installed && mlTextSuperResolutionCheckbox_ &&
            mlTextSuperResolutionCheckbox_->isChecked());
    }
    if (mlTextSuperResolutionPrefer2xCheckbox_) {
        mlTextSuperResolutionPrefer2xCheckbox_->setEnabled(
            installed && mlTextSuperResolutionCheckbox_ &&
            mlTextSuperResolutionCheckbox_->isChecked());
    }
    if (mlTextSuperResolutionUltra1440pCheckbox_) {
        mlTextSuperResolutionUltra1440pCheckbox_->setEnabled(
            installed && mlTextSuperResolutionCheckbox_ &&
            mlTextSuperResolutionCheckbox_->isChecked());
    }
    if (!installed && mlTextSuperResolutionStatusLabel_) {
        setSuperResStatus(QStringLiteral("Runtime not installed; use Setup & Downloads"),
                          false);
    }
#else
    Q_UNUSED(installed);
    if (mlTextSuperResolutionCheckbox_) {
        mlTextSuperResolutionCheckbox_->setEnabled(false);
        mlTextSuperResolutionCheckbox_->setToolTip(
            QStringLiteral("Unavailable in this build; NVIDIA Super Resolution "
                           "support was not compiled"));
    }
    if (mlTextSuperResolutionStrengthSlider_) {
        mlTextSuperResolutionStrengthSlider_->setEnabled(false);
    }
    if (mlTextSuperResolutionPrefer2xCheckbox_) {
        mlTextSuperResolutionPrefer2xCheckbox_->setEnabled(false);
    }
    if (mlTextSuperResolutionUltra1440pCheckbox_) {
        mlTextSuperResolutionUltra1440pCheckbox_->setEnabled(false);
    }
    if (mlTextSuperResolutionStatusLabel_) {
        setSuperResStatus(QStringLiteral("Unavailable in this build"), false);
    }
#endif

    if (!maxineAttribution_) {
        return;
    }
    const QString status = installed
                               ? QStringLiteral("NVIDIA Video Effects runtime installed")
                               : QStringLiteral("NVIDIA Video Effects runtime not installed; use Setup & Downloads");
    maxineAttribution_->setToolTip(status);
    maxineAttribution_->setAccessibleDescription(status);
}

bool MainWindow::isMaxineRuntimeInstalled() const
{
    return maxineRuntimeInstalled_;
}

void MainWindow::ShowHelpDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("OkuFlow Help"));
    dialog.setWindowIcon(windowIcon());
    dialog.setModal(true);
    dialog.setMinimumSize(520, 420);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto* guide = new QTextBrowser(&dialog);
    guide->setAccessibleName(QStringLiteral("OkuFlow controls and features guide"));
    guide->setOpenExternalLinks(false);
    guide->setHtml(QStringLiteral(
        "<h2>Controls</h2>"
        "<p><b>Simple</b> maximizes the camera view. Use the corner carousel "
        "or number keys 1 through 9 to change quick modes.</p>"
        "<p><b>Hide UI</b> or <b>Ctrl+H</b> clears controls and the assistant from "
        "the camera view. <b>Show UI</b> restores them and the existing answer.</p>"
        "<p><b>Advanced</b> opens detailed image and Assistant settings. Drag "
        "the divider at the panel edge to resize it.</p>"
        "<p>Drag the camera view to pan. Use Ctrl plus the mouse wheel to zoom. "
        "Enable <b>Virtual Joystick</b> under <b>Settings &gt; View and navigation</b> "
        "for an on-screen movement control.</p>"
        "<p>Photo and Record save both original and processed versions. Explain "
        "describes the scene; Read recognizes text. Press <b>Ctrl+Shift+O</b> "
        "to open the folder containing your OkuFlow files.</p>"
        "<h2>Features</h2>"
        "<p><b>Text Clarity</b> combines document cleanup, local contrast, "
        "sharpening, and stable text edges. <b>Display Colors</b> provides "
        "reading palettes and custom color schemes.</p>"
        "<p><b>NVIDIA Super Resolution</b> improves zoomed detail when the "
        "optional NVIDIA Video Effects runtime is installed.</p>"
        "<p><b>Text reading and Assistant</b> can read text, explain the view, answer "
        "follow-up questions, and add results to lecture notes.</p>"));
    layout->addWidget(guide, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::setSuperResStatus(const QString& status,
                                   bool active,
                                   bool performanceLimited)
{
    if (!mlTextSuperResolutionStatusLabel_) {
        return;
    }
    SetLiveText(mlTextSuperResolutionStatusLabel_, status,
                LivePoliteness::kSilent,
                QStringLiteral("NVIDIA Super Resolution status"));
    mlTextSuperResolutionStatusLabel_->setToolTip(status);
    mlTextSuperResolutionStatusLabel_->setAccessibleDescription(status);
    const QString color = status == QStringLiteral("Off")
                              ? QStringLiteral("#cfcfcf")
                              : active ? QStringLiteral("#33d17a")
                                       : QStringLiteral("#f6c85f");
    mlTextSuperResolutionStatusLabel_->setStyleSheet(
        QStringLiteral("color: %1;").arg(color));
    if (mlTextSuperResolutionOverrideCheckbox_) {
        mlTextSuperResolutionOverrideCheckbox_->setVisible(
            maxineRuntimeInstalled_ &&
            (performanceLimited || mlTextSuperResolutionOverrideCheckbox_->isChecked()));
    }
}

void MainWindow::setSuperResPerformanceOverrideChecked(bool checked)
{
    if (!mlTextSuperResolutionOverrideCheckbox_) {
        return;
    }
    const QSignalBlocker blocker(mlTextSuperResolutionOverrideCheckbox_);
    mlTextSuperResolutionOverrideCheckbox_->setChecked(checked);
    if (!checked) {
        mlTextSuperResolutionOverrideCheckbox_->setVisible(false);
    }
}
QLabel* MainWindow::assistantConnectionLabel() const { return assistantConnectionLabel_; }
QLabel* MainWindow::assistantUsageLabel() const { return assistantUsageLabel_; }
QPushButton* MainWindow::assistantConnectButton() const { return assistantConnectButton_; }
QTextBrowser* MainWindow::assistantTranscript() const { return assistantTranscript_; }
QPlainTextEdit* MainWindow::assistantPromptEdit() const { return assistantPromptEdit_; }
QCheckBox* MainWindow::assistantAttachFrameCheckbox() const { return assistantAttachFrameCheckbox_; }
QPushButton* MainWindow::assistantSendButton() const { return assistantSendButton_; }
QPushButton* MainWindow::assistantStopButton() const { return assistantStopButton_; }
QPushButton* MainWindow::assistantNewButton() const { return assistantNewButton_; }
QListWidget* MainWindow::assistantHistoryList() const { return assistantHistoryList_; }
QPushButton* MainWindow::assistantRenameButton() const { return assistantRenameButton_; }
QPushButton* MainWindow::assistantExportButton() const { return assistantExportButton_; }
QPushButton* MainWindow::assistantDeleteButton() const { return assistantDeleteButton_; }

void MainWindow::setKeystoneTrackingControls(bool active,
                                              bool available,
                                              bool paused,
                                              bool canStepBack,
                                              bool canStepForward,
                                              bool stepPending,
                                              int position,
                                              int count)
{
    const bool wasActive = keystoneTrackingActive_;
    const QString pauseText = paused ? QStringLiteral("Continue") : QStringLiteral("Stop");
    const bool geometryChanged = wasActive != active ||
                                 advancedKeystonePauseButton_->text() != pauseText;
    keystoneTrackingActive_ = active;

    const QString positionText = count > 0
                                     ? QStringLiteral(" Correction %1 of %2.")
                                           .arg(std::clamp(position, 1, count))
                                           .arg(count)
                                     : QString();
    const QString unavailable = QStringLiteral("GPU screen correction is unavailable.");
    const QString backTip = available
                                ? QStringLiteral("Previous screen correction.%1").arg(positionText)
                                : unavailable;
    const QString pauseTip = available
                                 ? (paused ? QStringLiteral("Continue automatic screen correction.%1")
                                           : QStringLiteral("Stop and hold the current screen correction.%1"))
                                       .arg(positionText)
                                 : unavailable;
    const QString nextTip = !available
                                ? unavailable
                                : stepPending
                                      ? QStringLiteral("Finding one new screen correction...")
                                      : QStringLiteral("Next screen correction, or find one new correction.%1")
                                            .arg(positionText);

    SetLiveText(advancedKeystonePauseButton_, pauseText,
                LivePoliteness::kSilent,
                QStringLiteral("Automatic screen correction"));
    const QIcon pauseIcon(paused ? QStringLiteral(":/okuflow/icons/play.svg")
                                 : QStringLiteral(":/okuflow/icons/pause.svg"));
    advancedKeystonePauseButton_->setIcon(pauseIcon);
    simpleKeystonePauseButton_->setIcon(pauseIcon);

    const bool controlsEnabled = active && available;
    advancedKeystoneTrackingRow_->setEnabled(controlsEnabled);
    for (QPushButton* button : {simpleKeystoneBackButton_, advancedKeystoneBackButton_}) {
        button->setEnabled(controlsEnabled && canStepBack);
        button->setToolTip(backTip);
    }
    for (QPushButton* button : {simpleKeystonePauseButton_, advancedKeystonePauseButton_}) {
        button->setEnabled(controlsEnabled);
        button->setToolTip(pauseTip);
        button->setAccessibleName(paused ? QStringLiteral("Continue Automatic Screen Correction")
                                         : QStringLiteral("Stop Automatic Screen Correction"));
    }
    for (QPushButton* button : {simpleKeystoneNextButton_, advancedKeystoneNextButton_}) {
        button->setEnabled(controlsEnabled && paused && canStepForward && !stepPending);
        button->setToolTip(nextTip);
    }

    if (!active || !isSimpleMode()) {
        keystoneTrackingPanel_->hide();
    } else if (simpleChromeVisible_ && !uiHidden_) {
        keystoneTrackingPanel_->show();
        keystoneTrackingPanel_->raise();
        RaiseDialogsAboveChrome();
    }
    if (geometryChanged) {
        UpdateSimpleChromeGeometry();
    }
}

void MainWindow::ActivatePresetRow(int row)
{
    if (!presetList_ || row < 0 || row >= presetList_->count()) {
        return;
    }

    QListWidgetItem* item = presetList_->item(row);
    gridBrowsing_ = false;
    presetList_->setCurrentRow(row);
    UpdateCurrentPresetUi(item, nullptr);
    emit quickModeActivated(row);
    if (isSimpleMode()) {
        ShowModeAnnouncement(item->data(Qt::AccessibleTextRole).toString(),
                             item->data(Qt::StatusTipRole).toString());
    }
    presetList_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    CloseModeGrid(false);
}

void MainWindow::ActivateRelativePreset(int offset)
{
    if (!presetList_ || presetList_->count() == 0) {
        return;
    }
    const int count = presetList_->count();
    // Arrow browsing is only a candidate. Carousel navigation starts from
    // the mode still displayed/applied when the grid was opened.
    int row = gridBrowsing_ ? gridOriginalRow_ : presetList_->currentRow();
    if (row < 0) {
        row = 0;
    } else {
        row = (row + offset + count) % count;
    }
    ActivatePresetRow(row);
}

void MainWindow::ToggleModeGrid()
{
    if (!modeGridPopup_ || uiHidden_) {
        return;
    }
    const bool show = !modeGridPopup_->isVisible();
    if (show) {
        gridOriginalRow_ = presetList_->currentRow();
        gridBrowsing_ = true;
        if (isSimpleMode()) {
            RevealSimpleChrome();
        } else if (bottomLeftPanel_) {
            bottomLeftPanel_->setWindowOpacity(1.0);
            bottomLeftPanel_->show();
            bottomLeftPanel_->raise();
        }
        simpleChromeIdleTimer_->stop();
        UpdateSimpleChromeGeometry();
        modeGridPopup_->show();
        modeGridPopup_->raise();
        modeGridPopup_->activateWindow();
        presetList_->setFocus(Qt::ShortcutFocusReason);
        if (QListWidgetItem* current = presetList_->currentItem()) {
            presetList_->scrollToItem(current, QAbstractItemView::PositionAtCenter);
        }
    } else {
        CloseModeGrid(true);
    }
}

void MainWindow::UpdateCurrentPresetUi(QListWidgetItem* current, QListWidgetItem* previous)
{
    if (!currentModeButton_) {
        return;
    }

    auto* carousel = static_cast<ModeCarouselButton*>(currentModeButton_);
    if (!current) {
        if (currentModeSummaryLabel_) SetLiveText(currentModeSummaryLabel_,
            QStringLiteral("Custom Setup"), LivePoliteness::kSilent,
            QStringLiteral("Current quick mode"));
        carousel->setShortcutNumber(0);
        SetLiveAccessibleDescription(currentModeButton_, QString());
        SetLiveText(currentModeButton_, QStringLiteral("Custom Setup"),
                    LivePoliteness::kSilent,
                    QStringLiteral("Current quick mode"));
        return;
    }

    QString label = current->data(Qt::AccessibleTextRole).toString();
    if (label.isEmpty()) {
        label = PlainPresetLabel(current->data(Qt::StatusTipRole).toString());
    }
    if (currentModeSummaryLabel_) {
        SetLiveText(currentModeSummaryLabel_, label, LivePoliteness::kSilent,
                    QStringLiteral("Current quick mode"));
    }
    const int row = presetList_->row(current);
    const int shortcut = row >= 0 && row < kSimpleShortcutCount ? row + 1 : 0;
    carousel->setShortcutNumber(shortcut);
    SetLiveAccessibleDescription(
        currentModeButton_,
        shortcut > 0 ? QStringLiteral("Number key %1").arg(shortcut) : QString());
    SetLiveText(currentModeButton_, label,
                LivePoliteness::kSilent,
                QStringLiteral("Current quick mode"));
    // The carousel lives in a top-level tool window that does not track its
    // contents; refit it so a longer label is not elided until the next reveal.
    UpdateSimpleChromeGeometry();

    if (previous && previous != current && isSimpleMode()) {
        ShowModeAnnouncement(label, current->data(Qt::StatusTipRole).toString());
    }
}

void MainWindow::ShowModeAnnouncement(const QString& label, const QString& profileName)
{
    if (uiHidden_ || label.trimmed().isEmpty() || !modeToast_) {
        return;
    }

    modeToastTitle_->setText(label);
    Q_UNUSED(profileName);
    modeToastSubtitle_->setText(TranslateUi(QStringLiteral("Quick mode")));
    modeToast_->setAccessibleName(
        TranslateUi(QStringLiteral("Quick mode changed to %1")).arg(label));
    UpdateSimpleChromeGeometry();
    modeToast_->show();
    modeToast_->raise();
    modeToastTimer_->start();

    QAccessibleAnnouncementEvent accessibleAnnouncement(modeToast_, label);
    accessibleAnnouncement.setPoliteness(QAccessible::AnnouncementPoliteness::Assertive);
    QAccessible::updateAccessibility(&accessibleAnnouncement);
}

void MainWindow::UpdateSimpleChromeGeometry()
{
    if (updatingChromeGeometry_) return;
    QScopedValueRollback<bool> geometryGuard(updatingChromeGeometry_, true);
    if (!renderWidget_ || !topLeftPanel_ || renderWidget_->width() <= 0 || renderWidget_->height() <= 0) {
        return;
    }

    const int viewWidth = renderWidget_->width();
    const int viewHeight = renderWidget_->height();
    const QPoint viewOrigin = renderWidget_->mapToGlobal(QPoint(0, 0));
    UpdateUiVisibilityControl();
    currentModeButton_->setMinimumWidth(220);
    currentModeButton_->setMaximumWidth(QWIDGETSIZE_MAX);
    const std::array<std::pair<QPushButton*, QString>, 5> actionButtons{{
        {capturePhotoButton_, QStringLiteral("Photo")},
        {recordButton_, !recordButton_->isEnabled()
                            ? QStringLiteral("Finishing")
                            : recordButton_->isChecked()
                                  ? QStringLiteral("Stop")
                                  : QStringLiteral("Record")},
        {explainNowButton_, explainBusy_
                                ? QStringLiteral("Stop")
                                : QStringLiteral("Explain")},
        {readTextButton_, QStringLiteral("Read")},
        {annotationButton_, QStringLiteral("Draw")},
    }};
    for (const auto& [button, label] : actionButtons) {
        button->setText(TranslateUi(label));
        button->setMinimumWidth(88);
        button->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    for (QWidget* panel :
         {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_,
          bottomRightPanel_}) {
        panel->adjustSize();
    }
    const int trackingWidth =
        keystoneTrackingActive_ ? keystoneTrackingPanel_->width() : 0;
    const auto refit = [](QWidget* panel) {
        panel->layout()->invalidate();
        panel->layout()->activate();
        panel->adjustSize();
        return std::max(panel->sizeHint().width(), panel->minimumSizeHint().width());
    };
    if (auto* actionsLayout = qobject_cast<QGridLayout*>(bottomRightPanel_->layout())) {
        // Test the real translated button sizes, including stylesheet padding
        // and icons. Retain readable labels even at the minimum camera width.
        for (int columns : {5, 3, 2, 1}) {
            for (int index = 0; index < static_cast<int>(actionButtons.size()); ++index) {
                auto* button = actionButtons[index].first;
                actionsLayout->removeWidget(button);
                actionsLayout->addWidget(button, index / columns, index % columns);
            }
            const int requiredWidth = refit(bottomRightPanel_);
            if (requiredWidth <= viewWidth &&
                (columns != 5 || bottomLeftPanel_->width() + trackingWidth + requiredWidth <= viewWidth)) {
                break;
            }
        }
    }
    if (auto* topLayout = qobject_cast<QGridLayout*>(topLeftPanel_->layout())) {
        for (QWidget* widget : std::array<QWidget*, 3>{simpleModeButton_,
                                advancedModeButton_, simpleTextClarityCheckbox_}) {
            topLayout->removeWidget(widget);
        }
        topLayout->addWidget(simpleModeButton_, 0, 0);
        topLayout->addWidget(advancedModeButton_, 0, 1);
        topLayout->addWidget(simpleTextClarityCheckbox_, 0, 2);
        if (refit(topLeftPanel_) > viewWidth) {
            topLayout->removeWidget(simpleTextClarityCheckbox_);
            topLayout->addWidget(simpleTextClarityCheckbox_, 1, 0, 1, 2);
            if (refit(topLeftPanel_) > viewWidth) {
                topLayout->removeWidget(advancedModeButton_);
                topLayout->removeWidget(simpleTextClarityCheckbox_);
                topLayout->addWidget(advancedModeButton_, 1, 0);
                topLayout->addWidget(simpleTextClarityCheckbox_, 2, 0);
                refit(topLeftPanel_);
            }
        }
    }
    if (auto* carouselLayout = qobject_cast<QGridLayout*>(bottomLeftPanel_->layout())) {
        for (QWidget* widget : std::array<QWidget*, 4>{modeGridButton_, previousModeButton_,
                                currentModeButton_, nextModeButton_}) {
            carouselLayout->removeWidget(widget);
        }
        modeGridButton_->setVisible(true);
        carouselLayout->addWidget(modeGridButton_, 0, 0);
        carouselLayout->addWidget(previousModeButton_, 0, 1);
        carouselLayout->addWidget(currentModeButton_, 0, 2);
        carouselLayout->addWidget(nextModeButton_, 0, 3);
        if (refit(bottomLeftPanel_) > viewWidth) {
            // The current-mode button opens the same grid, so a narrow view
            // drops the separate grid button instead of wrapping it onto a
            // second chrome row over the camera.
            const bool gridHadFocus = modeGridButton_->hasFocus();
            carouselLayout->removeWidget(modeGridButton_);
            carouselLayout->removeWidget(previousModeButton_);
            carouselLayout->removeWidget(currentModeButton_);
            carouselLayout->removeWidget(nextModeButton_);
            modeGridButton_->setVisible(false);
            if (gridHadFocus) currentModeButton_->setFocus(Qt::OtherFocusReason);
            carouselLayout->addWidget(previousModeButton_, 0, 0);
            carouselLayout->addWidget(currentModeButton_, 0, 1);
            carouselLayout->addWidget(nextModeButton_, 0, 2);
            const int overhead = std::max(0, refit(bottomLeftPanel_) - currentModeButton_->width());
            currentModeButton_->setMinimumWidth(120);
            currentModeButton_->setMaximumWidth(std::max(120, viewWidth - overhead));
            refit(bottomLeftPanel_);
        }
    }
    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
        panel->adjustSize();
    }

    const bool rightToLeft = layoutDirection() == Qt::RightToLeft;
    const auto leadingX = [&](int itemWidth) {
        return rightToLeft
                   ? viewOrigin.x() + std::max(0, viewWidth - itemWidth)
                   : viewOrigin.x();
    };
    const auto trailingX = [&](int itemWidth) {
        return rightToLeft
                   ? viewOrigin.x()
                   : viewOrigin.x() + std::max(0, viewWidth - itemWidth);
    };

    const int topChromeOffset = uiVisibilityPanel_ &&
        topLeftPanel_->width() + uiVisibilityPanel_->width() + 8 > viewWidth
            ? uiVisibilityPanel_->height() + 8 : 0;
    topLeftPanel_->move(leadingX(topLeftPanel_->width()), viewOrigin.y() + topChromeOffset);
    bottomLeftPanel_->move(leadingX(bottomLeftPanel_->width()),
                           viewOrigin.y() + std::max(0, viewHeight - bottomLeftPanel_->height()));
    const bool trackingInline = keystoneTrackingActive_ &&
                                bottomLeftPanel_->width() + keystoneTrackingPanel_->width() +
                                        bottomRightPanel_->width() <= viewWidth;
    const int leftChromeHeight = bottomLeftPanel_->height() +
                                 (keystoneTrackingActive_ && !trackingInline
                                      ? keystoneTrackingPanel_->height()
                                      : 0);
    const int leftChromeWidth = trackingInline
                                    ? bottomLeftPanel_->width() + keystoneTrackingPanel_->width()
                                    : std::max(bottomLeftPanel_->width(),
                                               keystoneTrackingActive_ ? keystoneTrackingPanel_->width() : 0);
    if (keystoneTrackingActive_) {
        const int trackingX =
            trackingInline
                ? (rightToLeft
                       ? viewWidth - bottomLeftPanel_->width() -
                             keystoneTrackingPanel_->width()
                       : bottomLeftPanel_->width())
                : (rightToLeft
                       ? viewWidth - keystoneTrackingPanel_->width()
                       : 0);
        const int trackingY = trackingInline
                                  ? viewHeight - keystoneTrackingPanel_->height()
                                  : viewHeight - bottomLeftPanel_->height() - keystoneTrackingPanel_->height();
        keystoneTrackingPanel_->move(viewOrigin.x() + trackingX,
                                     viewOrigin.y() + std::max(0, trackingY));
    }
    const bool bottomPanelsOverlap = leftChromeWidth + bottomRightPanel_->width() > viewWidth;
    const int bottomRightY = bottomPanelsOverlap
                                 ? viewHeight - leftChromeHeight - bottomRightPanel_->height()
                                 : viewHeight - bottomRightPanel_->height();
    bottomRightPanel_->move(trailingX(bottomRightPanel_->width()),
                            viewOrigin.y() + std::max(0, bottomRightY));

    if (simpleTranscriptPanel_ && simpleTranscriptPanel_->isVisible()) {
        const int overlayWidth = std::min(viewWidth - 24, 900);
        simpleTranscriptPanel_->setFixedWidth(std::max(240, overlayWidth));
        simpleTranscriptPanel_->adjustSize();
        const int chromeTop =
            std::min(viewHeight - leftChromeHeight,
                     std::min(bottomRightY, viewHeight - bottomRightPanel_->height()));
        const int overlayY =
            std::max(0, chromeTop - simpleTranscriptPanel_->height() - 8);
        simpleTranscriptPanel_->move(
            viewOrigin.x() +
                std::max(0, (viewWidth - simpleTranscriptPanel_->width()) / 2),
            viewOrigin.y() + overlayY);
    }

    if (modeGridPopup_) {
        const int popupWidth = std::min(viewWidth, std::max(420, std::min(860, viewWidth * 3 / 4)));
        const int columns = std::max(2, (popupWidth - 28) / 220);
        const int rows = std::max(1, (presetList_->count() + columns - 1) / columns);
        const int popupHeight = std::min(std::max(126, rows * 106 + 16),
                                        std::max(126, viewHeight - leftChromeHeight));
        const QSize gridSize(std::max(150, (popupWidth - 32) / columns), 102);
        presetList_->setGridSize(gridSize);
        for (int row = 0; row < presetList_->count(); ++row) {
            if (QListWidgetItem* item = presetList_->item(row)) {
                item->setSizeHint(gridSize - QSize(8, 8));
            }
        }
        modeGridPopup_->setGeometry(leadingX(popupWidth),
                                    viewOrigin.y() + std::max(0, viewHeight - leftChromeHeight - popupHeight),
                                    popupWidth,
                                    popupHeight);
    }

    if (modeToast_) {
        const int toastWidth = std::min(760, std::max(300, viewWidth - 32));
        const int toastHeight = std::min(170, std::max(112, viewHeight / 4));
        modeToast_->setGeometry(viewOrigin.x() + std::max(0, (viewWidth - toastWidth) / 2),
                                viewOrigin.y() + std::max(0, (viewHeight - toastHeight) / 2),
                                toastWidth,
                                toastHeight);
    }

    if (cameraPlaceholder_) {
        const int placeholderWidth = std::min(640, std::max(280, viewWidth - 48));
        cameraPlaceholder_->setFixedWidth(placeholderWidth);
        cameraPlaceholder_->adjustSize();
        cameraPlaceholder_->move(
            viewOrigin.x() + std::max(0, (viewWidth - placeholderWidth) / 2),
            viewOrigin.y() + std::max(0, (viewHeight - cameraPlaceholder_->height()) / 2));
    }
    // Reserve the actual visible native chrome, in render-relative coordinates.
    // SetSafeArea changes only untouched default assistant placement; saved or
    // user-adjusted geometry is retained by the overlay.
    if (auto* overlay = findChild<AssistiveOverlay*>()) {
        int safeTop = 8;
        int safeBottom = viewHeight - 8;
        for (QWidget* panel : {topLeftPanel_, uiVisibilityPanel_}) {
            if (panel && panel->isVisible()) {
                safeTop = std::max(safeTop, panel->geometry().bottom() - viewOrigin.y() + 9);
            }
        }
        for (QWidget* panel : {bottomLeftPanel_, bottomRightPanel_, keystoneTrackingPanel_,
                              simpleTranscriptPanel_}) {
            if (panel && panel->isVisible()) {
                safeBottom = std::min(safeBottom, panel->geometry().top() - viewOrigin.y() - 8);
            }
        }
        const int sideMargin = std::min(20, std::max(0, viewWidth / 30));
        overlay->SetSafeArea(QRect(sideMargin, safeTop,
            std::max(1, viewWidth - sideMargin * 2), std::max(1, safeBottom - safeTop)));
    }
}

void MainWindow::setCameraPlaceholder(const QString& title, const QString& detail)
{
    if (!cameraPlaceholder_) {
        return;
    }
    const QString translatedTitle = TranslateUi(title);
    const QString translatedDetail = TranslateUi(detail);
    if (cameraPlaceholderTitle_->text() == translatedTitle &&
        cameraPlaceholderDetail_->text() == translatedDetail) {
        UpdateCameraPlaceholderVisibility();
        return;
    }
    // Live sources keep the text correct across language switches; silent
    // because the pipeline status label already announces camera changes.
    SetLiveTranslationSource(cameraPlaceholderTitle_, title);
    SetLiveTranslationSource(cameraPlaceholderDetail_, detail);
    cameraPlaceholderTitle_->setText(translatedTitle);
    cameraPlaceholderDetail_->setText(translatedDetail);
    cameraPlaceholderDetail_->setVisible(!detail.isEmpty());
    UpdateSimpleChromeGeometry();
    UpdateCameraPlaceholderVisibility();
}

void MainWindow::UpdateCameraPlaceholderVisibility()
{
    if (!cameraPlaceholder_) {
        return;
    }
    // Like the corner chrome, this tool window would float above other
    // applications, so it only shows while OkuFlow is the active application.
    const bool show = !uiHidden_ && !cameraPlaceholderTitle_->text().isEmpty() &&
                      isVisible() && !isMinimized() &&
                      QGuiApplication::applicationState() == Qt::ApplicationActive;
    if (show == cameraPlaceholder_->isVisible()) {
        return;
    }
    if (show) {
        cameraPlaceholder_->show();
        cameraPlaceholder_->raise();
        RaiseDialogsAboveChrome();
    } else {
        cameraPlaceholder_->hide();
    }
}

void MainWindow::UpdateUiVisibilityControl()
{
    if (!uiVisibilityPanel_ || !uiVisibilityButton_ || !renderWidget_) return;
    SetLiveText(uiVisibilityButton_, uiHidden_ ? QStringLiteral("Show UI") : QStringLiteral("Hide UI"),
                LivePoliteness::kSilent,
                uiHidden_ ? QStringLiteral("Show controls and assistant")
                          : QStringLiteral("Hide controls and assistant"));
    uiVisibilityButton_->setToolTip(TranslateUi(QStringLiteral("Show or hide controls and assistant (Ctrl+H)")));
    uiVisibilityPanel_->adjustSize();
    const QPoint origin = renderWidget_->mapToGlobal(QPoint(0, 0));
    const int offset = layoutDirection() == Qt::RightToLeft ? 4
        : std::max(4, renderWidget_->width() - uiVisibilityPanel_->width() - 4);
    uiVisibilityPanel_->move(origin.x() + offset, origin.y() + 4);
    const bool visible = isVisible() && !isMinimized() &&
        QApplication::applicationState() == Qt::ApplicationActive;
    uiVisibilityPanel_->setVisible(visible);
    if (visible) {
        RaiseChromeAboveCanvas();
    }
}

void MainWindow::setUiHidden(bool hidden)
{
    if (uiHidden_ == hidden) return;
    if (hidden) {
        modeBeforeUiHiddenSimple_ = isSimpleMode();
        if (!modeBeforeUiHiddenSimple_) advancedPanelPreferredWidth_ = advancedPanelWidth();
        if (modeGridPopup_->isVisible()) CloseModeGrid(true);
        uiHidden_ = true;
        simpleChromeIdleTimer_->stop();
        modeToastTimer_->stop();
        if (chromeAnimation_) chromeAnimation_->stop();
        simpleChromeVisible_ = false;
        advancedPanel_->hide();
        for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_,
                              bottomRightPanel_, modeGridPopup_, modeToast_, simpleTranscriptPanel_,
                              cameraPlaceholder_}) {
            if (panel) panel->hide();
        }
        if (annotationOverlay_ && annotationOverlay_->IsActive()) annotationOverlay_->hide();
    } else {
        uiHidden_ = false;
        setSimpleMode(modeBeforeUiHiddenSimple_);
        if (annotationOverlay_ && annotationOverlay_->IsActive()) {
            annotationOverlay_->show();
            annotationOverlay_->raise();
        }
        UpdateCameraPlaceholderVisibility();
    }
    if (auto* overlay = findChild<AssistiveOverlay*>()) overlay->SetUiSuppressed(hidden);
    UpdateSimpleChromeGeometry();
    UpdateUiVisibilityControl();
    if (hidden && uiVisibilityButton_->isVisible()) {
        uiVisibilityPanel_->activateWindow();
        uiVisibilityButton_->setFocus(Qt::ShortcutFocusReason);
    }
}

void MainWindow::SetChromeOpacity(qreal opacity, int durationMs, bool hideWhenFinished)
{
    if (uiHidden_) return;
    if (chromeAnimation_) {
        chromeAnimation_->stop();
        chromeAnimation_->deleteLater();
        chromeAnimation_ = nullptr;
    }

    chromeAnimation_ = new QParallelAnimationGroup(this);
    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
        if (!panel) {
            continue;
        }
        if (panel == keystoneTrackingPanel_ && !keystoneTrackingActive_) {
            panel->hide();
            continue;
        }
        if (opacity > 0.0) {
            panel->show();
            panel->raise();
        }
        auto* animation = new QPropertyAnimation(panel, "windowOpacity", chromeAnimation_);
        animation->setDuration(durationMs);
        animation->setStartValue(panel->windowOpacity());
        animation->setEndValue(opacity);
        chromeAnimation_->addAnimation(animation);
    }
    RaiseDialogsAboveChrome();

    QParallelAnimationGroup* animationGroup = chromeAnimation_;
    connect(animationGroup, &QParallelAnimationGroup::finished, this,
            [this, animationGroup, hideWhenFinished]() {
                if (hideWhenFinished && !simpleChromeVisible_ && isSimpleMode()) {
                    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
                        if (panel) {
                            panel->hide();
                        }
                    }
                }
                if (chromeAnimation_ == animationGroup) {
                    chromeAnimation_ = nullptr;
                }
                animationGroup->deleteLater();
            });
    animationGroup->start();
}

// Chrome panels and dialogs are sibling windows owned by this window, so the
// most recently raised one wins. Raising chrome on reveal/activation must not
// bury an open dialog (Setup Assistant, AI Settings) beneath it.
void MainWindow::RaiseChromeAboveCanvas()
{
    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_,
                          bottomRightPanel_, uiVisibilityPanel_, modeGridPopup_,
                          modeToast_, simpleTranscriptPanel_}) {
        if (panel && panel->isVisible()) panel->raise();
    }
    RaiseDialogsAboveChrome();
}

void MainWindow::RaiseDialogsAboveChrome()
{
    const QList<QDialog*> dialogs =
        findChildren<QDialog*>(QString(), Qt::FindDirectChildrenOnly);
    for (QDialog* dialog : dialogs) {
        if (dialog->isVisible()) {
            dialog->raise();
        }
    }
}

bool MainWindow::SimpleChromeHasFocus() const
{
    QWidget* focus = QApplication::focusWidget();
    if (!focus) {
        return false;
    }
    for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_, modeGridPopup_}) {
        if (panel && (focus == panel || panel->isAncestorOf(focus))) {
            return true;
        }
    }
    return false;
}

void MainWindow::RevealSimpleChrome()
{
    if (uiHidden_ || !isSimpleMode()) {
        return;
    }
    const bool alreadyVisible =
        simpleChromeVisible_ &&
        topLeftPanel_ && topLeftPanel_->isVisible() &&
        bottomLeftPanel_ && bottomLeftPanel_->isVisible() &&
        bottomRightPanel_ && bottomRightPanel_->isVisible();
    if (alreadyVisible) {
        // Mouse movement reaches both Qt's event filter and the native event
        // filter. While the chrome is already visible, activity should only
        // extend its idle deadline; moving/raising four native tool windows on
        // every pointer sample starves the high-refresh viewport clock.
        if (!chromePinned_ && modeGridPopup_ && !modeGridPopup_->isVisible()) {
            const int remainingMs = simpleChromeIdleTimer_->remainingTime();
            if (remainingMs < 0 ||
                remainingMs <= kSimpleChromeIdleMs - 100) {
                simpleChromeIdleTimer_->start();
            }
        }
        return;
    }
    const bool needsAnimation = !simpleChromeVisible_ ||
                                (topLeftPanel_ && !topLeftPanel_->isVisible());
    simpleChromeVisible_ = true;
    UpdateSimpleChromeGeometry();
    if (needsAnimation) {
        SetChromeOpacity(1.0, 120, false);
    } else {
        for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
            if (panel) {
                if (panel == keystoneTrackingPanel_ && !keystoneTrackingActive_) {
                    panel->hide();
                    continue;
                }
                panel->show();
                panel->raise();
            }
        }
        RaiseDialogsAboveChrome();
    }
    if (!chromePinned_ && !modeGridPopup_->isVisible()) {
        simpleChromeIdleTimer_->start();
    }
}

void MainWindow::FadeSimpleChrome()
{
    if (uiHidden_ || !isSimpleMode() || chromePinned_) {
        return;
    }
    if (modeGridPopup_->isVisible() || QApplication::activeModalWidget() ||
        QApplication::activePopupWidget() ||
        (chromeKeyboardFocus_ && SimpleChromeHasFocus())) {
        simpleChromeIdleTimer_->start();
        return;
    }
    simpleChromeVisible_ = false;
    SetChromeOpacity(0.0, kSimpleChromeFadeMs, true);
}

void MainWindow::setSimpleMode(bool simple)
{
    if (uiHidden_) {
        modeBeforeUiHiddenSimple_ = simple;
        return;
    }
    if (advancedPanel_) {
        advancedPanel_->setVisible(!simple);
    }
    // Checking one button in the exclusive group unchecks the other; the
    // toggled handlers re-enter here but converge because setChecked() and
    // setCurrentIndex() are no-ops once the state matches.
    if (simple && simpleModeButton_) {
        simpleModeButton_->setChecked(true);
        bottomLeftPanel_->show();
        if (keystoneTrackingActive_) {
            keystoneTrackingPanel_->show();
        }
        bottomRightPanel_->show();
        if (simpleTranscriptPanel_ && simpleTranscriptActive_) {
            simpleTranscriptPanel_->show();
        }
        UpdateSimpleChromeGeometry();
        RevealSimpleChrome();
    } else if (!simple && advancedModeButton_) {
        advancedModeButton_->setChecked(true);
        QTimer::singleShot(0, this, [this]() { ApplyAdvancedPanelWidth(); });
        simpleChromeIdleTimer_->stop();
        modeToastTimer_->stop();
        modeGridPopup_->hide();
        modeToast_->hide();
        simpleChromeVisible_ = true;
        if (chromeAnimation_) {
            chromeAnimation_->stop();
        }
        topLeftPanel_->setWindowOpacity(1.0);
        topLeftPanel_->show();
        topLeftPanel_->raise();
        // Keep the quick-mode carousel available in Advanced as a compact
        // camera-corner control instead of duplicating the preset model.
        bottomLeftPanel_->setWindowOpacity(1.0);
        bottomLeftPanel_->show();
        bottomLeftPanel_->raise();
        keystoneTrackingPanel_->hide();
        bottomRightPanel_->setWindowOpacity(1.0);
        bottomRightPanel_->show();
        bottomRightPanel_->raise();
        RaiseDialogsAboveChrome();
        if (simpleTranscriptPanel_) {
            // The Advanced Transcript tab shows the same content instead.
            simpleTranscriptPanel_->hide();
        }
        UpdateSimpleChromeGeometry();
    }
}

bool MainWindow::isSimpleMode() const
{
    return uiHidden_ ? modeBeforeUiHiddenSimple_
                     : !advancedPanel_ || !advancedPanel_->isVisible();
}

int MainWindow::advancedPanelWidth() const
{
    if (advancedPanel_ && advancedPanel_->isVisible() && advancedPanel_->width() > 0) {
        return advancedPanel_->width();
    }
    return advancedPanelPreferredWidth_;
}

void MainWindow::setAdvancedPanelWidth(int width)
{
    advancedPanelPreferredWidth_ = std::clamp(width, kAdvancedPanelMinimumWidth, 1200);
    QTimer::singleShot(0, this, [this]() { ApplyAdvancedPanelWidth(); });
}

void MainWindow::ApplyAdvancedPanelWidth()
{
    if (!contentSplitter_ || !advancedPanel_ || !advancedPanel_->isVisible()) {
        return;
    }
    const int totalWidth = contentSplitter_->width();
    if (totalWidth <= 0) {
        return;
    }
    const int maximumPanelWidth = std::max(kAdvancedPanelMinimumWidth,
                                           totalWidth - renderWidget_->minimumWidth());
    const int panelWidth = std::clamp(advancedPanelPreferredWidth_,
                                      kAdvancedPanelMinimumWidth,
                                      maximumPanelWidth);
    contentSplitter_->setSizes({std::max(renderWidget_->minimumWidth(), totalWidth - panelWidth),
                                panelWidth});
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    const bool controlZoom =
        app_ && (event->modifiers() & Qt::ControlModifier) &&
        !HasEditableTextFocus();
    if (controlZoom &&
        (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)) {
        app_->HandleKeyboardZoom(1.0f);
        event->accept();
        return;
    }
    if (controlZoom && event->key() == Qt::Key_Minus) {
        app_->HandleKeyboardZoom(-1.0f);
        event->accept();
        return;
    }
    if (app_ && app_->HandlePanKey(event->key(), true)) {
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (app_ && app_->HandlePanKey(event->key(), false)) {
        event->accept();
        return;
    }
    QMainWindow::keyReleaseEvent(event);
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event && event->type() == QEvent::LanguageChange) {
        // The language manager switches the default locale with the
        // translator, so decimal marks and percent placement change too.
        refreshSliderReadouts();
        refreshTextClarityUi();
        UpdateUiVisibilityControl();
        ApplyExplainBusyUi();
        UpdateAdvancedTabToolTips();
        UpdateProfileButtonLayout();
        for (const auto& item : scopedDescriptions_) {
            item.widget->setAccessibleDescription(ScopedDescriptionText(item.scope, item.description));
        }
        // LanguageManager retranslates the whole tree after installing the
        // translator. Re-match after that pass, without moving keyboard focus
        // or replacing either tab's query or saved disclosure state.
        QTimer::singleShot(0, this, [this]() {
            if (imageSearchEdit_ && !imageSearchEdit_->text().trimmed().isEmpty()) {
                FilterSettingsTab(SettingsScope::kImage, imageSearchEdit_->text());
            }
            if (sharedSearchEdit_ && !sharedSearchEdit_->text().trimmed().isEmpty()) {
                FilterSettingsTab(SettingsScope::kShared, sharedSearchEdit_->text());
            }
        });
    }
    if (event && event->type() == QEvent::LayoutDirectionChange) {
        UpdateDirectionalUi();
        UpdateSimpleChromeGeometry();
    }
}

void MainWindow::UpdateDirectionalUi()
{
    const bool rightToLeft = layoutDirection() == Qt::RightToLeft;
    const QString previousIcon =
        rightToLeft ? QStringLiteral(":/okuflow/icons/next.svg")
                    : QStringLiteral(":/okuflow/icons/previous.svg");
    const QString nextIcon =
        rightToLeft ? QStringLiteral(":/okuflow/icons/previous.svg")
                    : QStringLiteral(":/okuflow/icons/next.svg");
    const QString backIcon =
        rightToLeft ? QStringLiteral(":/okuflow/icons/step-forward.svg")
                    : QStringLiteral(":/okuflow/icons/step-back.svg");
    const QString forwardIcon =
        rightToLeft ? QStringLiteral(":/okuflow/icons/step-back.svg")
                    : QStringLiteral(":/okuflow/icons/step-forward.svg");

    if (previousAdvancedTabButton_) {
        previousAdvancedTabButton_->setIcon(QIcon(previousIcon));
    }
    if (nextAdvancedTabButton_) {
        nextAdvancedTabButton_->setIcon(QIcon(nextIcon));
    }
    if (advancedKeystoneBackButton_) {
        advancedKeystoneBackButton_->setIcon(QIcon(backIcon));
    }
    if (advancedKeystoneNextButton_) {
        advancedKeystoneNextButton_->setIcon(QIcon(forwardIcon));
    }
    if (simpleKeystoneBackButton_) {
        simpleKeystoneBackButton_->setIcon(QIcon(backIcon));
    }
    if (simpleKeystoneNextButton_) {
        simpleKeystoneNextButton_->setIcon(QIcon(forwardIcon));
    }
    if (previousModeButton_) {
        previousModeButton_->setIcon(
            style()->standardIcon(QStyle::SP_ArrowBack));
    }
    if (nextModeButton_) {
        nextModeButton_->setIcon(
            style()->standardIcon(QStyle::SP_ArrowForward));
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::MouseButtonPress) {
        chromeKeyboardFocus_ = false;
    } else if (type == QEvent::FocusIn && SimpleChromeHasFocus()) {
        const auto reason = static_cast<QFocusEvent*>(event)->reason();
        // Native activation and grid restoration preserve the input modality.
        // Only an actual mouse press above clears the keyboard-focus latch.
        if (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason ||
            reason == Qt::ShortcutFocusReason) {
            chromeKeyboardFocus_ = true;
        }
    } else if (type == QEvent::KeyPress && SimpleChromeHasFocus()) {
        chromeKeyboardFocus_ = true;
    }
    const bool keyboardFocusActivity = type == QEvent::FocusIn &&
        (static_cast<QFocusEvent*>(event)->reason() == Qt::TabFocusReason ||
         static_cast<QFocusEvent*>(event)->reason() == Qt::BacktabFocusReason ||
         static_cast<QFocusEvent*>(event)->reason() == Qt::ShortcutFocusReason);
    const bool userActivity = type == QEvent::MouseMove ||
                              type == QEvent::MouseButtonPress ||
                              type == QEvent::Wheel ||
                              type == QEvent::KeyPress ||
                              keyboardFocusActivity ||
                              type == QEvent::Enter ||
                              type == QEvent::TouchBegin ||
                              type == QEvent::ApplicationActivate;
    if (isSimpleMode() && userActivity) {
        RevealSimpleChrome();
    }
    if (!uiHidden_ && !isSimpleMode() && type == QEvent::ApplicationActivate) {
        simpleChromeVisible_ = true;
        topLeftPanel_->setWindowOpacity(1.0);
        bottomLeftPanel_->setWindowOpacity(1.0);
        bottomRightPanel_->setWindowOpacity(1.0);
        UpdateSimpleChromeGeometry();
        topLeftPanel_->show();
        topLeftPanel_->raise();
        bottomLeftPanel_->show();
        bottomLeftPanel_->raise();
        bottomRightPanel_->show();
        bottomRightPanel_->raise();
        RaiseDialogsAboveChrome();
    }
    if (type == QEvent::ApplicationActivate) {
        UpdateUiVisibilityControl();
        UpdateCameraPlaceholderVisibility();
    }

    if (type == QEvent::ApplicationDeactivate) {
        // The chrome panels, grid popup, and toast are top-level tool windows,
        // which stay above OTHER applications' windows. Hide them when the user
        // switches away (e.g. to take notes); reactivation reveals them again
        // via the ApplicationActivate branch above.
        simpleChromeIdleTimer_->stop();
        if (uiVisibilityPanel_) uiVisibilityPanel_->hide();
        if (modeToastTimer_) {
            modeToastTimer_->stop();
        }
        if (modeToast_) {
            modeToast_->hide();
        }
        if (modeGridPopup_) {
            if (gridBrowsing_) {
                const QSignalBlocker blocker(presetList_);
                presetList_->setCurrentRow(gridOriginalRow_);
                gridBrowsing_ = false;
            }
            modeGridPopup_->hide();
        }
        if (cameraPlaceholder_) {
            cameraPlaceholder_->hide();
        }
        for (QWidget* panel : {topLeftPanel_, bottomLeftPanel_, keystoneTrackingPanel_, bottomRightPanel_}) {
            if (panel) {
                panel->hide();
            }
        }
        simpleChromeVisible_ = false;
    }

    // Tool windows share the application's filter, but dialogs and transient
    // editors own their keyboard handling even when parented to this window.
    bool ownKeyboardSurface = false;
    if (type == QEvent::KeyPress) {
    auto* eventWidget = qobject_cast<QWidget*>(watched);
    auto* assistiveOverlay = findChild<AssistiveOverlay*>();
    const auto onSurface = [eventWidget](QWidget* surface) {
        return eventWidget && surface && eventWidget->window() == surface->window();
    };
    ownKeyboardSurface =
        !QApplication::activeModalWidget() && !QApplication::activePopupWidget() &&
        (onSurface(this) || onSurface(topLeftPanel_) || onSurface(bottomLeftPanel_) ||
         onSurface(bottomRightPanel_) || onSurface(keystoneTrackingPanel_) ||
         onSurface(annotationOverlay_) || onSurface(assistiveOverlay) ||
         onSurface(modeGridPopup_) || onSurface(uiVisibilityPanel_));
    }

    if (ownKeyboardSurface && type == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && modeGridPopup_->isVisible()) {
            CloseModeGrid(true);
            event->accept();
            return true;
        }
    }

    if (ownKeyboardSurface && type == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (modeGridPopup_->isVisible() &&
            (watched == presetList_ || presetList_->isAncestorOf(qobject_cast<QWidget*>(watched))) &&
            (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Space)) {
            ActivatePresetRow(presetList_->currentRow());
            event->accept();
            return true;
        }
        if (key->key() == Qt::Key_F && key->modifiers() == Qt::ControlModifier) {
            setUiHidden(false);
            setSimpleMode(false);
            if (advancedTabs_->currentWidget() != settingsTabPage_) advancedTabs_->setCurrentWidget(imageTabPage_);
            auto* edit = advancedTabs_->currentWidget() == settingsTabPage_ ? sharedSearchEdit_ : imageSearchEdit_;
            activateWindow();
            edit->setFocus(Qt::ShortcutFocusReason);
            edit->selectAll();
            event->accept();
            return true;
        }
        if (key->key() == Qt::Key_F6 && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            setUiHidden(false);
            FocusRegion(key->modifiers() & Qt::ShiftModifier);
            event->accept();
            return true;
        }
        if (key->key() == Qt::Key_H && key->modifiers() == Qt::ControlModifier) {
            setUiHidden(!uiHidden_);
            event->accept();
            return true;
        }
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) &&
            !modeGridPopup_->isVisible()) {
            std::vector<QWidget*> focusOrder{
                simpleModeButton_, advancedModeButton_, simpleTextClarityCheckbox_, modeGridButton_,
                previousModeButton_, currentModeButton_, nextModeButton_,
                simpleKeystoneBackButton_, simpleKeystonePauseButton_, simpleKeystoneNextButton_,
                capturePhotoButton_, recordButton_, explainNowButton_, readTextButton_,
                annotationButton_};
            focusOrder.push_back(uiVisibilityButton_);
            if (annotationOverlay_ && annotationOverlay_->IsActive()) {
                const auto annotationTargets = annotationOverlay_->FocusTargets();
                focusOrder.insert(focusOrder.end(),
                                  annotationTargets.begin(),
                                  annotationTargets.end());
            }
            if (auto* overlay = findChild<AssistiveOverlay*>();
                overlay && overlay->isVisible()) {
                const auto overlayTargets = overlay->FocusTargets();
                focusOrder.insert(focusOrder.end(), overlayTargets.begin(), overlayTargets.end());
            }
            if (!isSimpleMode() && advancedTabs_) {
                focusOrder.push_back(advancedTabs_->tabBar());
                focusOrder.push_back(helpButton_);
                const auto inspector = InspectorFocusOrder(advancedTabs_->currentWidget());
                focusOrder.insert(focusOrder.end(), inspector.begin(), inspector.end());
            }
            QWidget* current = QApplication::focusWidget();
            int currentIndex = -1;
            for (int i = 0; i < static_cast<int>(focusOrder.size()); ++i) {
                if (focusOrder[i] == current) {
                    currentIndex = i;
                    break;
                }
            }
            const int direction = key->key() == Qt::Key_Backtab ||
                                  (key->modifiers() & Qt::ShiftModifier) ? -1 : 1;
            if (currentIndex < 0) {
                currentIndex = direction > 0 ? -1 : 0;
            }
            for (int step = 1; step <= static_cast<int>(focusOrder.size()); ++step) {
                const int candidate = (currentIndex + direction * step +
                                       static_cast<int>(focusOrder.size())) %
                                      static_cast<int>(focusOrder.size());
                QWidget* target = focusOrder[candidate];
                if (target && target->isVisible() && target->isEnabled() &&
                    (target->focusPolicy() & Qt::TabFocus)) {
                    // Preserve native traversal inside the inspector. Only
                    // bridge its boundaries to the separate chrome windows.
                    if (!isSimpleMode() && current &&
                        advancedPanel_->isAncestorOf(current) &&
                        advancedPanel_->isAncestorOf(target)) {
                        break;
                    }
                    target->window()->activateWindow();
                    target->setFocus(direction > 0 ? Qt::TabFocusReason
                                                   : Qt::BacktabFocusReason);
                    event->accept();
                    return true;
                }
            }
        }
        const bool plainKey = key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::KeypadModifier;
        if (isSimpleMode() && plainKey && !HasEditableTextFocus() &&
            key->key() >= Qt::Key_1 && key->key() <= Qt::Key_9) {
            ActivatePresetRow(key->key() - Qt::Key_1);
            event->accept();
            return true;
        }
    }

    if (watched == this && (type == QEvent::Show || type == QEvent::Hide ||
                            type == QEvent::WindowStateChange)) {
        QTimer::singleShot(0, this, &MainWindow::UpdateUiVisibilityControl);
    }
    if (type == QEvent::Resize && (watched == imageTabPage_ || watched == advancedPanel_)) {
        UpdateProfileButtonLayout();
    }
    if ((watched == this || watched == centralWidget() || watched == contentSplitter_) &&
                           (type == QEvent::Move ||
                            type == QEvent::Resize ||
                            type == QEvent::WindowStateChange)) {
        QTimer::singleShot(0, this, &MainWindow::UpdateSimpleChromeGeometry);
    }

    if (watched == renderWidget_) {
        switch (type) {
        case QEvent::Resize:
            UpdateSimpleChromeGeometry();
            break;
        case QEvent::Wheel: {
            auto* wheel = static_cast<QWheelEvent*>(event);
            if (wheel->modifiers() & Qt::ControlModifier) {
                if (app_) {
                    app_->HandleZoomWheel(wheel);
                }
                event->accept();
                return true;
            } else if (app_ && app_->HandlePanScroll(wheel)) {
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonPress: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (modeGridPopup_ && modeGridPopup_->isVisible()) {
                CloseModeGrid(true);
            }
            if (mouse->button() == Qt::MiddleButton) {
                if (app_) {
                    app_->BeginMousePan(mouse->position(), renderWidget_->size());
                }
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (app_ && app_->UpdateMousePan(mouse->position())) {
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton) {
                if (app_) {
                    app_->EndMousePan();
                }
                event->accept();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::nativeEventFilter(const QByteArray&, void* message, qintptr*)
{
    const auto* nativeMessage = static_cast<const MSG*>(message);
    if (!nativeMessage || !isSimpleMode()) {
        return false;
    }

    switch (nativeMessage->message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        RevealSimpleChrome();
        break;
    default:
        break;
    }
    return false;
}

} // namespace okuflow

#endif // _WIN32
