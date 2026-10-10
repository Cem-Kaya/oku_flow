#ifdef _WIN32

#include "okuflow/ui/color_scheme_picker.hpp"

#include "okuflow/ui/wheel_safe_combo_box.hpp"
#include "okuflow/ui/ui_translation.hpp"

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QResizeEvent>
#include <QTimer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <array>

namespace okuflow {

namespace {

constexpr int kGridColumns = 6;

class ColorSchemeTrigger final : public QPushButton {
public:
    using QPushButton::QPushButton;

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPushButton::paintEvent(event);
        QStyleOption arrow;
        arrow.initFrom(this);
        arrow.rect = QRect(width() - 24, (height() - 14) / 2, 14, 14);
        QPainter painter(this);
        style()->drawPrimitive(QStyle::PE_IndicatorArrowDown, &arrow, &painter, this);
    }
};

// Corner radius shared by swatches, tiles, and the preview strip — the
// "squircle" look; roughly 30% of the tile edge.
constexpr int kSwatchRadius = 8;

QPixmap SchemePixmap(const color_schemes::ColorScheme& scheme,
                     const QSize& size = QSize(64, 28))
{
    QPixmap image(size);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect area = image.rect().adjusted(1, 1, -1, -1);
    QPainterPath clip;
    clip.addRoundedRect(area, kSwatchRadius, kSwatchRadius);
    painter.setClipPath(clip);
    if (scheme.id == QStringLiteral("normal") ||
        scheme.id == QStringLiteral("inverted")) {
        static const std::array<QColor, 6> colors{
            QColor("#eb3947"), QColor("#ffb30b"), QColor("#2b9b50"),
            QColor("#11b7c8"), QColor("#2c63dc"), QColor("#8b40cf")};
        const int stripeWidth = std::max(1, area.width() / static_cast<int>(colors.size()));
        for (int index = 0; index < static_cast<int>(colors.size()); ++index) {
            QColor color = colors[static_cast<std::size_t>(index)];
            if (scheme.id == QStringLiteral("inverted")) {
                color.setRgb(255 - color.red(), 255 - color.green(), 255 - color.blue());
            }
            const int left = area.left() + index * stripeWidth;
            const int right = index + 1 == static_cast<int>(colors.size())
                                  ? area.right() + 1
                                  : left + stripeWidth;
            painter.fillRect(QRect(left, area.top(), right - left, area.height()), color);
        }
    } else if (scheme.effect) {
        const auto lut = color_schemes::BuildColorLut(scheme);
        for (int x = 0; x < area.width(); ++x) {
            const int lutIndex = area.width() <= 1 ? 0 : x * 255 / (area.width() - 1);
            painter.setPen(color_schemes::UnpackBgra(lut[static_cast<std::size_t>(lutIndex)]));
            painter.drawLine(area.left() + x, area.top(), area.left() + x, area.bottom());
        }
    } else {
        painter.fillRect(area, color_schemes::UnpackBgra(
                                   color_schemes::TextBackgroundBgra(scheme)));
        painter.setPen(color_schemes::UnpackBgra(
            color_schemes::TextForegroundBgra(scheme)));
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(std::max(12, area.height() - 8));
        painter.setFont(font);
        painter.drawText(area, Qt::AlignCenter, QStringLiteral("A"));
    }
    painter.setPen(QPen(QColor(255, 255, 255, 70), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(area.adjusted(0, 0, -1, -1), kSwatchRadius, kSwatchRadius);
    return image;
}

class QuickSchemeButton final : public QPushButton {
public:
    explicit QuickSchemeButton(const color_schemes::ColorScheme& scheme, QWidget* parent)
        : QPushButton(scheme.name, parent), scheme_(scheme)
    {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(100, 64);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(scheme.accessibleName);
        setToolTip(scheme.name);
        setProperty("schemeId", scheme.id);
        setStyleSheet(QStringLiteral("QPushButton:checked { border:3px solid #c052d8; }"));
    }
    QSize sizeHint() const override { return QSize(148, 64); }
    QSize minimumSizeHint() const override { return QSize(100, 64); }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        option.text.clear();
        option.icon = {};
        QPainter painter(this);
        style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        const bool rtl = layoutDirection() == Qt::RightToLeft;
        const QRect swatch(rtl ? width() - 52 : 8, (height() - 28) / 2, 44, 28);
        painter.drawPixmap(swatch, SchemePixmap(scheme_, swatch.size()));
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                      QPalette::ButtonText));
        const QRect label = rtl ? QRect(8, 6, width() - 66, height() - 12)
                                : QRect(60, 6, width() - 68, height() - 12);
        painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeading | Qt::TextWordWrap, text());
        if (isChecked()) {
            painter.setPen(QColor(QStringLiteral("#c052d8")));
            painter.drawText(rect().adjusted(4, 0, -4, -2),
                             Qt::AlignBottom | Qt::AlignTrailing, QStringLiteral("✓"));
        }
    }
private:
    color_schemes::ColorScheme scheme_;
};

class SchemeTile final : public QToolButton {
public:
    explicit SchemeTile(const color_schemes::ColorScheme& scheme,
                        QWidget* parent = nullptr)
        : QToolButton(parent), scheme_(scheme)
    {
        setFixedSize(36, 36);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(scheme_.name);
        setAccessibleName(scheme_.accessibleName);
        setAccessibleDescription(QStringLiteral("Display color scheme"));
    }

    const color_schemes::ColorScheme& scheme() const { return scheme_; }
    void setScheme(const color_schemes::ColorScheme& scheme)
    {
        scheme_ = scheme;
        setToolTip(scheme_.name);
        setAccessibleName(scheme_.accessibleName);
        update();
    }
    void setSelected(bool selected)
    {
        selected_ = selected;
        setChecked(selected);
        update();
    }
    void setPencilBadge(bool enabled) { pencilBadge_ = enabled; update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRect swatch = rect().adjusted(3, 3, -3, -3);
        painter.drawPixmap(swatch, SchemePixmap(scheme_, swatch.size()));
        if (pencilBadge_) {
            painter.setPen(QPen(Qt::white, 2));
            painter.drawLine(swatch.right() - 8, swatch.bottom() - 3,
                             swatch.right() - 2, swatch.bottom() - 9);
        }
        if (selected_) {
            painter.setPen(QPen(QColor("#c052d8"), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2),
                                    kSwatchRadius + 2, kSwatchRadius + 2);
            painter.setBrush(QColor("#c052d8"));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QRect(width() - 15, 1, 13, 13));
            painter.setPen(Qt::white);
            painter.drawText(QRect(width() - 15, 0, 13, 13), Qt::AlignCenter,
                             QStringLiteral("✓"));
        } else if (hasFocus()) {
            painter.setPen(QPen(Qt::white, 2, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2),
                                    kSwatchRadius + 2, kSwatchRadius + 2);
        }
    }

private:
    color_schemes::ColorScheme scheme_;
    bool selected_{};
    bool pencilBadge_{};
};

class SchemePreview final : public QWidget {
public:
    explicit SchemePreview(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(34);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(QStringLiteral("Custom color preview"));
    }
    void setScheme(const color_schemes::ColorScheme& scheme)
    {
        scheme_ = scheme;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.drawPixmap(rect(), SchemePixmap(scheme_, size()));
    }
private:
    color_schemes::ColorScheme scheme_ = color_schemes::LegacyColorScheme(2);
};

class WheelSafeSpinBox final : public QSpinBox {
public:
    using QSpinBox::QSpinBox;
protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

color_schemes::ColorScheme EditorScheme(const std::vector<QColor>& stops,
                                        color_schemes::SchemeMode mode,
                                        bool stepped)
{
    color_schemes::ColorScheme scheme;
    scheme.id = QStringLiteral("custom");
    scheme.name = QStringLiteral("Custom colors");
    scheme.accessibleName = QStringLiteral("Custom display colors");
    scheme.mode = mode;
    scheme.stops = stops;
    scheme.stepped = stepped || mode == color_schemes::SchemeMode::Posterize;
    scheme.textColorAtHighLuma = true;
    return color_schemes::NormalizeColorScheme(scheme);
}

} // namespace

ColorSchemePicker::ColorSchemePicker(QWidget* parent)
    : QWidget(parent), currentScheme_(color_schemes::LegacyColorScheme(0))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    quickChoicesLayout_ = new QGridLayout();
    quickChoicesLayout_->setContentsMargins(0, 0, 0, 0);
    quickChoicesLayout_->setSpacing(6);
    for (const QString& id : {QStringLiteral("posterize-6"), QStringLiteral("yellow-black"),
                              QStringLiteral("normal"), QStringLiteral("black-yellow")}) {
        const auto* scheme = color_schemes::FindBuiltInColorScheme(id);
        if (!scheme) continue;
        quickSchemes_.push_back(*scheme);
        auto* button = new QuickSchemeButton(*scheme, this);
        button->setObjectName(QStringLiteral("quickColor_%1").arg(id));
        quickChoices_.push_back(button);
        connect(button, &QPushButton::clicked, this, [this, scheme = *scheme]() {
            SelectScheme(scheme);
        });
    }
    layout->addLayout(quickChoicesLayout_);
    ArrangeQuickChoices();
    trigger_ = new ColorSchemeTrigger(currentScheme_.name, this);
    trigger_->setObjectName(QStringLiteral("moreColorsButton"));
    trigger_->setIcon(QIcon(SchemePixmap(currentScheme_)));
    trigger_->setIconSize(QSize(64, 28));
    trigger_->setMinimumHeight(64);
    trigger_->setStyleSheet(QStringLiteral("QPushButton { padding-right: 30px; }"));
    trigger_->setAccessibleName(QStringLiteral("Display colors, Normal colors"));
    trigger_->setToolTip(QStringLiteral("Choose display colors"));
    layout->addWidget(trigger_);
    connect(trigger_, &QPushButton::clicked, this, &ColorSchemePicker::ShowPopup);

    editorStops_ = {QColor("#000000"), QColor("#ffffff")};
    BuildPopup();
    setCurrentScheme(currentScheme_);
    if (qApp) {
        qApp->installEventFilter(this);
    }
}

void ColorSchemePicker::ArrangeQuickChoices()
{
    const int columns = width() >= 720 ? 4 : 2;
    if (!quickChoicesLayout_ || columns == quickColumns_) return;
    quickColumns_ = columns;
    for (auto* button : quickChoices_) quickChoicesLayout_->removeWidget(button);
    for (int column = 0; column < 4; ++column) {
        quickChoicesLayout_->setColumnStretch(column, column < columns ? 1 : 0);
    }
    for (size_t index = 0; index < quickChoices_.size(); ++index) {
        quickChoicesLayout_->addWidget(quickChoices_[index], static_cast<int>(index) / columns,
                                       static_cast<int>(index) % columns);
    }
}

void ColorSchemePicker::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    ArrangeQuickChoices();
}

ColorSchemePicker::~ColorSchemePicker()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
    delete popup_;
}

void ColorSchemePicker::BuildPopup()
{
    popup_ = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    popup_->setObjectName(QStringLiteral("colorSchemePopup"));
    popup_->setAttribute(Qt::WA_ShowWithoutActivating, false);
    // A translucent native tool window can lose its painted backing surface
    // above the D3D viewport. Use an opaque native surface so camera content
    // and inspector controls can never show through the picker.
    popup_->setAttribute(Qt::WA_TranslucentBackground, false);
    popup_->setAttribute(Qt::WA_OpaquePaintEvent, true);
    popup_->setAttribute(Qt::WA_StyledBackground, true);
    popup_->setAutoFillBackground(true);
    QPalette popupPalette = popup_->palette();
    popupPalette.setColor(QPalette::Window, QColor(QStringLiteral("#161618")));
    popup_->setPalette(popupPalette);
    popup_->setStyleSheet(QStringLiteral(
        "QWidget#colorSchemePopup { background-color:#161618; "
        "border:2px solid #77777d; border-radius:6px; }"
        "QLabel#pickerSection { color:rgba(255,255,255,150); font-weight:600; "
        "font-size:11px; letter-spacing:1px; text-transform:uppercase; "
        "margin-top:4px; }"
        "QWidget#colorSchemePopup QPushButton, QWidget#colorSchemePopup QToolButton, "
        "QWidget#colorSchemePopup QComboBox, QWidget#colorSchemePopup QSpinBox { "
        "min-height:32px; background:#232326; border:1px solid rgba(255,255,255,45); "
        "border-radius:9px; padding:2px 10px; }"
        "QWidget#colorSchemePopup QPushButton:hover, "
        "QWidget#colorSchemePopup QToolButton:hover { background:#2e2e33; }"
        "QWidget#colorSchemePopup QPushButton:focus, "
        "QWidget#colorSchemePopup QToolButton:focus, "
        "QWidget#colorSchemePopup QComboBox:focus, "
        "QWidget#colorSchemePopup QSpinBox:focus { "
        "border:2px solid palette(highlight); }"
        "QWidget#colorSchemePopup QCheckBox { spacing:8px; }"));
    popup_->installEventFilter(this);

    auto* root = new QVBoxLayout(popup_);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* reset = new QPushButton(QStringLiteral("Reset to Normal colors"), popup_);
    reset->setProperty("schemeId", QStringLiteral("normal"));
    reset->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    reset->setIconSize(QSize(24, 24));
    reset->setAccessibleName(QStringLiteral("Reset display colors to Normal colors"));
    connect(reset, &QPushButton::clicked, this, [this]() {
        SelectScheme(color_schemes::LegacyColorScheme(0));
    });
    root->addWidget(reset);

    auto addSection = [root, this](const QString& title,
                                   const std::vector<color_schemes::ColorScheme>& schemes) {
        auto* label = new QLabel(title, popup_);
        label->setObjectName(QStringLiteral("pickerSection"));
        root->addWidget(label);
        auto* grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(7);
        int index = 0;
        for (const auto& scheme : schemes) {
            auto* tile = new SchemeTile(scheme, popup_);
            tile->setProperty("schemeId", scheme.id);
            schemeTiles_.push_back(tile);
            tile->installEventFilter(this);
            connect(tile, &QToolButton::clicked, this, [this, scheme]() {
                SelectScheme(scheme);
            });
            grid->addWidget(tile, index / kGridColumns, index % kGridColumns);
            ++index;
        }
        grid->setColumnStretch(kGridColumns, 1);
        root->addLayout(grid);
    };

    std::vector<color_schemes::ColorScheme> pairs;
    std::vector<color_schemes::ColorScheme> effects;
    for (const auto& scheme : color_schemes::BuiltInColorSchemes()) {
        if (scheme.id == QStringLiteral("normal")) {
            continue;
        }
        (scheme.effect ? effects : pairs).push_back(scheme);
    }
    const auto prioritize = [](std::vector<color_schemes::ColorScheme>& schemes,
                               const QString& id) {
        const auto found = std::find_if(schemes.begin(), schemes.end(),
            [&id](const auto& scheme) { return scheme.id == id; });
        if (found != schemes.end()) std::rotate(schemes.begin(), found, found + 1);
    };
    prioritize(pairs, QStringLiteral("black-yellow"));
    prioritize(pairs, QStringLiteral("yellow-black"));
    prioritize(effects, QStringLiteral("posterize-6"));
    addSection(QStringLiteral("Reading colors"), pairs);
    addSection(QStringLiteral("Effects"), effects);

    auto* customLabel = new QLabel(QStringLiteral("Custom"), popup_);
    customLabel->setObjectName(QStringLiteral("pickerSection"));
    root->addWidget(customLabel);

    auto* customRow = new QHBoxLayout();
    customTile_ = new SchemeTile(color_schemes::LegacyColorScheme(2), popup_);
    static_cast<SchemeTile*>(customTile_)->setPencilBadge(true);
    customTile_->setVisible(false);
    customTile_->installEventFilter(this);
    connect(customTile_, &QToolButton::clicked, this, [this]() {
        if (hasCustomScheme()) SelectScheme(customScheme_);
    });
    customRow->addWidget(customTile_);
    customModeCombo_ = new WheelSafeComboBox(popup_);
    customModeCombo_->addItem(QStringLiteral("Two colors"),
                              static_cast<int>(color_schemes::SchemeMode::Duotone));
    customModeCombo_->addItem(QStringLiteral("Posterize"),
                              static_cast<int>(color_schemes::SchemeMode::Posterize));
    customModeCombo_->addItem(QStringLiteral("Gradient"),
                              static_cast<int>(color_schemes::SchemeMode::Gradient));
    customModeCombo_->setAccessibleName(QStringLiteral("Custom color mode"));
    customRow->addWidget(customModeCombo_, 1);
    stopCountSpin_ = new WheelSafeSpinBox(popup_);
    stopCountSpin_->setRange(2, 8);
    stopCountSpin_->setValue(2);
    stopCountSpin_->setPrefix(QStringLiteral("Stops: "));
    stopCountSpin_->setAccessibleName(QStringLiteral("Number of color stops"));
    customRow->addWidget(stopCountSpin_);
    root->addLayout(customRow);

    auto* wells = new QHBoxLayout();
    static const std::array<QColor, 8> defaults{
        QColor("#000000"), QColor("#ffffff"), QColor("#e63946"), QColor("#ffb703"),
        QColor("#2a9d4b"), QColor("#00bcd4"), QColor("#2463eb"), QColor("#8b3fd1")};
    for (int index = 0; index < 8; ++index) {
        auto* well = new QToolButton(popup_);
        well->setFixedSize(36, 36);
        well->setProperty("stopIndex", index);
        well->setAccessibleName(QStringLiteral("Color stop %1 of 8").arg(index + 1));
        well->setToolTip(QStringLiteral("Choose color stop %1").arg(index + 1));
        colorWells_.push_back(well);
        connect(well, &QToolButton::clicked, this, [this, index]() {
            while (editorStops_.size() <= static_cast<std::size_t>(index)) {
                editorStops_.push_back(defaults[editorStops_.size()]);
            }
            QColorDialog dialog(editorStops_[static_cast<std::size_t>(index)], popup_);
            dialog.setWindowTitle(QStringLiteral("Choose color stop %1").arg(index + 1));
            dialog.setOption(QColorDialog::DontUseNativeDialog, true);
            // Match the picker popover's look; also enlarge the tiny stock
            // controls — this dialog serves low-vision users.
            dialog.setStyleSheet(QStringLiteral(
                "QColorDialog { background:#161618; font-size:12pt; }"
                "QColorDialog QPushButton { min-height:36px; min-width:96px; "
                "background:#232326; border:1px solid rgba(255,255,255,45); "
                "border-radius:9px; padding:4px 14px; }"
                "QColorDialog QPushButton:hover { background:#2e2e33; }"
                "QColorDialog QPushButton:focus { border:2px solid palette(highlight); }"
                "QColorDialog QSpinBox, QColorDialog QLineEdit { min-height:32px; "
                "background:#232326; border:1px solid rgba(255,255,255,45); "
                "border-radius:9px; padding:2px 8px; }"
                "QColorDialog QSpinBox:focus, QColorDialog QLineEdit:focus { "
                "border:2px solid palette(highlight); }"
                "QColorDialog QLabel { color:rgba(255,255,255,200); }"));
            if (dialog.exec() == QDialog::Accepted && dialog.selectedColor().isValid()) {
                editorStops_[static_cast<std::size_t>(index)] = dialog.selectedColor();
                RefreshCustomEditor();
            }
        });
        wells->addWidget(well);
    }
    root->addLayout(wells);

    auto* customOptions = new QHBoxLayout();
    steppedCheck_ = new QCheckBox(QStringLiteral("Stepped colors"), popup_);
    steppedCheck_->setAccessibleName(QStringLiteral("Use hard color bands"));
    customOptions->addWidget(steppedCheck_);
    customPreview_ = new SchemePreview(popup_);
    customOptions->addWidget(customPreview_, 1);
    applyCustomButton_ = new QPushButton(QStringLiteral("Apply custom"), popup_);
    customOptions->addWidget(applyCustomButton_);
    root->addLayout(customOptions);

    connect(customModeCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        const auto mode = static_cast<color_schemes::SchemeMode>(customModeCombo_->currentData().toInt());
        if (mode == color_schemes::SchemeMode::Duotone) {
            stopCountSpin_->setValue(2);
            steppedCheck_->setChecked(false);
        } else if (mode == color_schemes::SchemeMode::Posterize) {
            if (stopCountSpin_->value() < 3) stopCountSpin_->setValue(6);
            steppedCheck_->setChecked(true);
        } else {
            steppedCheck_->setChecked(false);
        }
        RefreshCustomEditor();
    });
    connect(stopCountSpin_, &QSpinBox::valueChanged, this,
            [this]() { RefreshCustomEditor(); });
    connect(steppedCheck_, &QCheckBox::toggled, this,
            [this]() { RefreshCustomEditor(); });
    connect(applyCustomButton_, &QPushButton::clicked,
            this, &ColorSchemePicker::ApplyCustomScheme);

    RefreshCustomEditor();
    popup_->adjustSize();
}

void ColorSchemePicker::ShowPopup()
{
    popup_->adjustSize();
    const QScreen* screen = trigger_->screen();
    const QRect available = screen ? screen->availableGeometry() : QRect();
    QPoint position = trigger_->mapToGlobal(QPoint(0, trigger_->height() + 4));
    if (position.x() + popup_->width() > available.right()) {
        position.setX(available.right() - popup_->width() + 1);
    }
    if (position.y() + popup_->height() > available.bottom()) {
        position.setY(trigger_->mapToGlobal(QPoint(0, 0)).y() - popup_->height() - 4);
    }
    position.setX(std::max(position.x(), available.left()));
    position.setY(std::max(position.y(), available.top()));
    popup_->move(position);
    RefreshSelection();
    popup_->show();
    popup_->raise();
    popup_->activateWindow();
    if (!schemeTiles_.empty()) schemeTiles_.front()->setFocus(Qt::PopupFocusReason);
}

void ColorSchemePicker::HidePopup(bool restoreFocus)
{
    if (!popup_->isVisible()) return;
    popup_->hide();
    if (restoreFocus) trigger_->setFocus(Qt::PopupFocusReason);
}

void ColorSchemePicker::SelectScheme(const color_schemes::ColorScheme& scheme)
{
    const auto normalized = color_schemes::NormalizeColorScheme(scheme);
    const bool changed = !color_schemes::SchemesEquivalent(currentScheme_, normalized) ||
                         currentScheme_.id != normalized.id;
    setCurrentScheme(normalized);
    HidePopup();
    if (changed) {
        emit schemeChanged();
        Announce(QStringLiteral("Display colors changed to %1").arg(currentScheme_.name));
    }
}

void ColorSchemePicker::setCurrentScheme(const color_schemes::ColorScheme& scheme)
{
    currentScheme_ = color_schemes::NormalizeColorScheme(scheme);
    const QString displayName = currentScheme_.id == QStringLiteral("custom")
        ? currentScheme_.name : TranslateUi(currentScheme_.name);
    // The generic tree translator preserves formatted arguments verbatim.
    // Store the translated built-in name so later Show/Polish passes cannot
    // replace it with English. User-defined names are never translated.
    SetLiveTranslationSource(trigger_, QStringLiteral("More colors\n%1").arg(displayName));
    trigger_->setText(TranslateUi(QStringLiteral("More colors\n%1"))
                         .arg(displayName));
    trigger_->setIcon(QIcon(SchemePixmap(currentScheme_)));
    trigger_->setAccessibleName(QStringLiteral("Display colors, %1")
                                    .arg(currentScheme_.accessibleName));
    trigger_->setToolTip(QStringLiteral("Display colors: %1").arg(currentScheme_.name));
    RefreshSelection();
}

void ColorSchemePicker::setCustomScheme(const color_schemes::ColorScheme& scheme)
{
    if (scheme.stops.size() < 2) return;
    customScheme_ = color_schemes::NormalizeColorScheme(scheme);
    customScheme_.id = QStringLiteral("custom");
    customScheme_.legacyMode = -1;
    editorStops_ = customScheme_.stops;
    if (customModeCombo_ && stopCountSpin_ && steppedCheck_) {
        const QSignalBlocker modeBlock(customModeCombo_);
        const QSignalBlocker countBlock(stopCountSpin_);
        const QSignalBlocker steppedBlock(steppedCheck_);
        const int modeIndex = customModeCombo_->findData(static_cast<int>(customScheme_.mode));
        if (modeIndex >= 0) customModeCombo_->setCurrentIndex(modeIndex);
        stopCountSpin_->setValue(std::clamp(static_cast<int>(customScheme_.stops.size()), 2, 8));
        steppedCheck_->setChecked(customScheme_.stepped);
    }
    RefreshCustomTile();
    RefreshCustomEditor();
}

void ColorSchemePicker::RefreshSelection()
{
    for (size_t index = 0; index < quickChoices_.size(); ++index) {
        const QSignalBlocker blocker(quickChoices_[index]);
        quickChoices_[index]->setChecked(quickSchemes_[index].id == currentScheme_.id &&
            color_schemes::SchemesEquivalent(quickSchemes_[index], currentScheme_));
    }
    for (QToolButton* button : schemeTiles_) {
        auto* tile = static_cast<SchemeTile*>(button);
        tile->setSelected(tile->scheme().id == currentScheme_.id &&
                          color_schemes::SchemesEquivalent(tile->scheme(), currentScheme_));
    }
    if (customTile_) {
        static_cast<SchemeTile*>(customTile_)->setSelected(
            currentScheme_.id == QStringLiteral("custom") && hasCustomScheme() &&
            color_schemes::SchemesEquivalent(currentScheme_, customScheme_));
    }
}

void ColorSchemePicker::RefreshCustomEditor()
{
    if (!customModeCombo_ || !stopCountSpin_) return;
    const int count = stopCountSpin_->value();
    static const std::array<QColor, 8> defaults{
        QColor("#000000"), QColor("#ffffff"), QColor("#e63946"), QColor("#ffb703"),
        QColor("#2a9d4b"), QColor("#00bcd4"), QColor("#2463eb"), QColor("#8b3fd1")};
    while (editorStops_.size() < static_cast<std::size_t>(count)) {
        editorStops_.push_back(defaults[editorStops_.size()]);
    }
    for (int index = 0; index < static_cast<int>(colorWells_.size()); ++index) {
        QToolButton* well = colorWells_[static_cast<std::size_t>(index)];
        well->setVisible(index < count);
        if (index < count) {
            const QColor color = editorStops_[static_cast<std::size_t>(index)];
            well->setStyleSheet(QStringLiteral(
                "QToolButton { background:%1; border:2px solid rgba(255,255,255,110); "
                "border-radius:18px; }"
                "QToolButton:hover { border-color:#ffffff; }"
                "QToolButton:focus { border:3px solid palette(highlight); "
                "border-radius:18px; }")
                                    .arg(color.name()));
            const bool twoColors = static_cast<color_schemes::SchemeMode>(
                                       customModeCombo_->currentData().toInt()) ==
                                   color_schemes::SchemeMode::Duotone;
            const QString role = twoColors && index == 0
                                     ? QStringLiteral("Background color")
                                     : twoColors && index == 1
                                           ? QStringLiteral("Text color")
                                           : QStringLiteral("Color stop %1 of %2")
                                                 .arg(index + 1).arg(count);
            well->setAccessibleName(role);
            well->setToolTip(QStringLiteral("%1, %2").arg(role, color.name()));
        }
    }
    const auto mode = static_cast<color_schemes::SchemeMode>(
        customModeCombo_->currentData().toInt());
    steppedCheck_->setEnabled(mode != color_schemes::SchemeMode::Duotone);
    std::vector<QColor> visibleStops(editorStops_.begin(), editorStops_.begin() + count);
    static_cast<SchemePreview*>(customPreview_)->setScheme(
        EditorScheme(visibleStops, mode, steppedCheck_->isChecked()));
}

void ColorSchemePicker::RefreshCustomTile()
{
    if (!customTile_) return;
    customTile_->setVisible(hasCustomScheme());
    if (hasCustomScheme()) {
        static_cast<SchemeTile*>(customTile_)->setScheme(customScheme_);
        static_cast<SchemeTile*>(customTile_)->setPencilBadge(true);
    }
    popup_->adjustSize();
}

void ColorSchemePicker::ApplyCustomScheme()
{
    const int count = stopCountSpin_->value();
    const auto mode = static_cast<color_schemes::SchemeMode>(
        customModeCombo_->currentData().toInt());
    std::vector<QColor> stops(editorStops_.begin(), editorStops_.begin() + count);
    customScheme_ = EditorScheme(stops, mode, steppedCheck_->isChecked());
    RefreshCustomTile();
    SelectScheme(customScheme_);
}

void ColorSchemePicker::Announce(const QString& message)
{
    QAccessibleAnnouncementEvent announcement(this, message);
    announcement.setPoliteness(QAccessible::AnnouncementPoliteness::Assertive);
    QAccessible::updateAccessibility(&announcement);
}

bool ColorSchemePicker::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == this && event->type() == QEvent::LanguageChange) {
        QTimer::singleShot(0, this, [this]() { setCurrentScheme(currentScheme_); });
    }
    // While the color-stop dialog (a modal child of the popup) is open, the
    // popup loses activation but must stay visible, and Escape belongs to the
    // dialog — otherwise picking a custom color dismisses the whole popover.
    QWidget* activeModal = QApplication::activeModalWidget();
    const bool popupOwnsModal =
        activeModal != nullptr && popup_ != nullptr && popup_->isAncestorOf(activeModal);
    if (popup_->isVisible() && !popupOwnsModal &&
        (event->type() == QEvent::ApplicationDeactivate ||
         (watched == popup_ && event->type() == QEvent::WindowDeactivate))) {
        HidePopup(false);
        return false;
    }
    if (popup_->isVisible() && !popupOwnsModal && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            HidePopup();
            return true;
        }
        auto it = std::find(schemeTiles_.begin(), schemeTiles_.end(), watched);
        if (it != schemeTiles_.end()) {
            const int index = static_cast<int>(std::distance(schemeTiles_.begin(), it));
            int next = index;
            switch (key->key()) {
            case Qt::Key_Left: next = std::max(0, index - 1); break;
            case Qt::Key_Right: next = std::min(static_cast<int>(schemeTiles_.size()) - 1, index + 1); break;
            case Qt::Key_Up: next = std::max(0, index - kGridColumns); break;
            case Qt::Key_Down: next = std::min(static_cast<int>(schemeTiles_.size()) - 1, index + kGridColumns); break;
            case Qt::Key_Home: next = 0; break;
            case Qt::Key_End: next = static_cast<int>(schemeTiles_.size()) - 1; break;
            default: return QWidget::eventFilter(watched, event);
            }
            schemeTiles_[static_cast<std::size_t>(next)]->setFocus(Qt::ShortcutFocusReason);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace okuflow

#endif // _WIN32
