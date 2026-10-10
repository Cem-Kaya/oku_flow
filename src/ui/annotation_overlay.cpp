#ifdef _WIN32

#include "okuflow/ui/annotation_overlay.hpp"
#include "okuflow/ui/live_status_text.hpp"

#include <QAccessible>
#include <QAccessibleAnnouncementEvent>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace okuflow {
namespace {

struct InkPreset {
    const char* name;
    const char* color;
};

constexpr std::array<InkPreset, 6> kInkPresets{{
    {"Magenta", "#ff4bd8"},
    {"Yellow", "#fff000"},
    {"Green", "#55ff55"},
    {"Cyan", "#00e5ff"},
    {"Blue", "#2979ff"},
    {"White", "#ffffff"},
}};

void SetAccessible(QWidget* widget,
                   const QString& name,
                   const QString& description)
{
    widget->setAccessibleName(name);
    widget->setAccessibleDescription(description);
    widget->setToolTip(description);
}

QIcon ColorIcon(const QColor& color, bool selected)
{
    QPixmap pixmap(46, 36);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(selected ? Qt::white : QColor(QStringLiteral("#686868")),
                        selected ? 4 : 2));
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(5, 4, 36, 28), 4, 4);
    if (selected) {
        painter.setPen(QPen(Qt::black, 5, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(15, 18), QPointF(20, 24));
        painter.drawLine(QPointF(20, 24), QPointF(31, 12));
        painter.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(15, 18), QPointF(20, 24));
        painter.drawLine(QPointF(20, 24), QPointF(31, 12));
    }
    return QIcon(pixmap);
}

std::array<QPointF, 8> HandlePoints(const QRectF& bounds)
{
    return {
        bounds.topLeft(),
        QPointF(bounds.center().x(), bounds.top()),
        bounds.topRight(),
        QPointF(bounds.right(), bounds.center().y()),
        bounds.bottomRight(),
        QPointF(bounds.center().x(), bounds.bottom()),
        bounds.bottomLeft(),
        QPointF(bounds.left(), bounds.center().y())};
}

QString ToolName(AnnotationTool tool)
{
    switch (tool) {
    case AnnotationTool::kPointer:
        return QStringLiteral("Move tool");
    case AnnotationTool::kPen:
        return QStringLiteral("Pen tool");
    case AnnotationTool::kLine:
        return QStringLiteral("Line tool");
    case AnnotationTool::kShape:
        return QStringLiteral("Shape tool");
    case AnnotationTool::kText:
        return QStringLiteral("Text tool");
    case AnnotationTool::kEraser:
        return QStringLiteral("Erase tool");
    }
    return QStringLiteral("Annotation tool");
}

bool NativeWindowContainsPoint(QWidget* widget, const POINT& point)
{
    if (!widget || !widget->isVisible()) {
        return false;
    }

    const HWND window = reinterpret_cast<HWND>(widget->winId());
    RECT rect{};
    return window && IsWindowVisible(window) && GetWindowRect(window, &rect) &&
           point.x >= rect.left && point.x < rect.right &&
           point.y >= rect.top && point.y < rect.bottom;
}

} // namespace

AnnotationOverlay::AnnotationOverlay(QWidget* renderTarget, QWidget* owner)
    : QWidget(owner,
              Qt::Tool |
                  Qt::FramelessWindowHint |
                  Qt::NoDropShadowWindowHint),
      renderTarget_(renderTarget)
{
    setObjectName(QStringLiteral("annotationOverlay"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Lecture annotation canvas"));
    setAccessibleDescription(
        QStringLiteral("Draw, select, resize, save, or clear annotations anchored to the camera scene."));
    if (renderTarget_) {
        for (QWidget* widget = renderTarget_; widget; widget = widget->parentWidget()) {
            widget->installEventFilter(this);
        }
        SyncGeometryToRenderTarget();
    }
    if (owner) {
        owner->installEventFilter(this);
    }
    BuildToolbar();
    UpdateDirectionalUi();
    hide();
}

void AnnotationOverlay::BuildToolbar()
{
    const QString chromeStyle = QStringLiteral(R"(
        QFrame#annotationToolbar, QFrame#annotationOptions,
        QFrame#annotationActions {
            background: #111111;
            border: 3px solid #f2f2f2;
            border-radius: 6px;
        }
        QToolButton {
            color: #ffffff;
            background: #3d3d3d;
            border: 2px solid #737373;
            border-radius: 5px;
            padding: 4px;
        }
        QToolButton:hover { background: #505050; }
        QToolButton:checked {
            color: #ffffff;
            border: 3px solid #ffffff;
            background: #a83cbe;
        }
        QToolButton:focus { border: 3px solid #d79ae6; }
        QToolButton:disabled { color: #8f8f8f; }
        QToolButton#annotationDone {
            color: #ffffff;
            background: #a83cbe;
            border: 3px solid #ffffff;
        }
        QToolButton#annotationDone:hover { background: #bd52d3; }
        QLineEdit {
            color: #ffffff;
            background: #303030;
            border: 2px solid #737373;
            min-height: 42px;
            padding: 2px 6px;
        }
        QLineEdit:focus { border: 3px solid #d79ae6; }
        QLabel { color: #ffffff; border: none; }
        QLabel#annotationOptionsTitle {
            color: #d79ae6;
            font-weight: 700;
        }
        QSlider { min-width: 42px; min-height: 132px; }
        QSlider::groove:vertical {
            width: 10px; border-radius: 5px; background: #555555;
        }
        QSlider::sub-page:vertical {
            background: #555555; border-radius: 5px;
        }
        QSlider::add-page:vertical {
            background: #a83cbe; border-radius: 5px;
        }
        QSlider::handle:vertical {
            height: 24px; margin: 0 -8px; border-radius: 12px;
            background: #f2f2f2; border: 2px solid #222222;
        }
    )");

    toolbar_ = new QFrame(this);
    toolbar_->setObjectName(QStringLiteral("annotationToolbar"));
    toolbar_->setAttribute(Qt::WA_StyledBackground, true);
    toolbar_->setStyleSheet(chromeStyle);
    auto* toolLayout = new QVBoxLayout(toolbar_);
    toolLayout->setContentsMargins(7, 7, 7, 7);
    toolLayout->setSpacing(5);

    auto makeTool = [toolLayout](const QString& text,
                                 const QString& iconName) {
        auto* button = new QToolButton();
        button->setText(text);
        button->setIcon(QIcon(QStringLiteral(":/okuflow/icons/") + iconName));
        button->setIconSize(QSize(25, 25));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setFixedSize(84, 72);
        button->setCheckable(true);
        toolLayout->addWidget(button);
        return button;
    };

    pointerButton_ = makeTool(QStringLiteral("Move"), QStringLiteral("pointer.svg"));
    penButton_ = makeTool(QStringLiteral("Pen"), QStringLiteral("draw.svg"));
    lineButton_ = makeTool(QStringLiteral("Line"), QStringLiteral("line.svg"));
    shapeButton_ = makeTool(QStringLiteral("Shape"), QStringLiteral("shape.svg"));
    textButton_ = makeTool(QStringLiteral("Text"), QStringLiteral("text.svg"));
    eraserButton_ = makeTool(QStringLiteral("Erase"), QStringLiteral("eraser.svg"));

    auto* toolGroup = new QButtonGroup(this);
    toolGroup->setExclusive(true);
    for (QToolButton* button :
         {pointerButton_, penButton_, lineButton_, shapeButton_,
          textButton_, eraserButton_}) {
        toolGroup->addButton(button);
    }
    penButton_->setChecked(true);

    optionsPanel_ = new QFrame(this);
    optionsPanel_->setObjectName(QStringLiteral("annotationOptions"));
    optionsPanel_->setAttribute(Qt::WA_StyledBackground, true);
    optionsPanel_->setStyleSheet(chromeStyle);
    auto* optionsLayout = new QVBoxLayout(optionsPanel_);
    optionsLayout->setContentsMargins(12, 10, 12, 10);
    optionsLayout->setSpacing(8);

    optionsTitleLabel_ = new QLabel(QStringLiteral("Pen options"));
    optionsTitleLabel_->setObjectName(QStringLiteral("annotationOptionsTitle"));
    optionsLayout->addWidget(optionsTitleLabel_);

    auto* colorGrid = new QGridLayout();
    colorGrid->setContentsMargins(0, 0, 0, 0);
    colorGrid->setSpacing(5);
    auto* colorGroup = new QButtonGroup(this);
    colorGroup->setExclusive(true);
    for (int i = 0; i < static_cast<int>(kInkPresets.size()); ++i) {
        const InkPreset& preset = kInkPresets[static_cast<std::size_t>(i)];
        auto* button = new QToolButton(optionsPanel_);
        const QColor color(QString::fromLatin1(preset.color));
        const QString colorName = QString::fromLatin1(preset.name);
        button->setCheckable(true);
        button->setProperty("inkColor", color.name(QColor::HexRgb));
        button->setIcon(ColorIcon(color, false));
        button->setIconSize(QSize(46, 36));
        button->setFixedSize(58, 48);
        SetAccessible(button,
                      QString::fromLatin1(preset.name) +
                          QStringLiteral(" annotation color"),
                      QStringLiteral("Use %1 ink. The check mark indicates the selected color.")
                          .arg(QString::fromLatin1(preset.name)));
        colorGroup->addButton(button);
        colorButtons_.push_back(button);
        colorGrid->addWidget(button, i / 3, i % 3);
        connect(button, &QToolButton::toggled, this,
                [this, button, color, colorName](bool checked) {
                    button->setIcon(ColorIcon(color, checked));
                    if (checked) {
                        inkColor_ = color;
                        EmitPreferences();
                        Announce(colorName);
                    }
                });
    }
    optionsLayout->addLayout(colorGrid);

    auto* thicknessLayout = new QHBoxLayout();
    thicknessLayout->setContentsMargins(0, 0, 0, 0);
    widthTitleLabel_ = new QLabel(QStringLiteral("Thickness"));
    widthTitleLabel_->setAlignment(Qt::AlignTop);
    widthSlider_ = new QSlider(Qt::Vertical);
    widthSlider_->setRange(2, 24);
    widthSlider_->setValue(inkWidthPixels_);
    widthSlider_->installEventFilter(this);
    widthValueLabel_ = new QLabel(QString::number(inkWidthPixels_));
    widthValueLabel_->setMinimumWidth(34);
    widthValueLabel_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    thicknessLayout->addWidget(widthTitleLabel_);
    thicknessLayout->addWidget(widthSlider_, 0, Qt::AlignHCenter);
    thicknessLayout->addWidget(widthValueLabel_);
    optionsLayout->addLayout(thicknessLayout);

    auto* styleLayout = new QHBoxLayout();
    styleLayout->setContentsMargins(0, 0, 0, 0);
    solidButton_ = new QToolButton(optionsPanel_);
    solidButton_->setText(QStringLiteral("Solid"));
    solidButton_->setCheckable(true);
    dashedButton_ = new QToolButton(optionsPanel_);
    dashedButton_->setText(QStringLiteral("Dashed"));
    dashedButton_->setCheckable(true);
    auto* styleGroup = new QButtonGroup(this);
    styleGroup->setExclusive(true);
    styleGroup->addButton(solidButton_);
    styleGroup->addButton(dashedButton_);
    solidButton_->setChecked(true);
    styleLayout->addWidget(solidButton_);
    styleLayout->addWidget(dashedButton_);
    optionsLayout->addLayout(styleLayout);

    auto* shapeLayout = new QHBoxLayout();
    shapeLayout->setContentsMargins(0, 0, 0, 0);
    rectangleButton_ = new QToolButton(optionsPanel_);
    rectangleButton_->setText(QStringLiteral("Rectangle"));
    rectangleButton_->setCheckable(true);
    ellipseButton_ = new QToolButton(optionsPanel_);
    ellipseButton_->setText(QStringLiteral("Ellipse"));
    ellipseButton_->setCheckable(true);
    auto* shapeGroup = new QButtonGroup(this);
    shapeGroup->setExclusive(true);
    shapeGroup->addButton(rectangleButton_);
    shapeGroup->addButton(ellipseButton_);
    rectangleButton_->setChecked(true);
    shapeLayout->addWidget(rectangleButton_);
    shapeLayout->addWidget(ellipseButton_);
    optionsLayout->addLayout(shapeLayout);

    textInput_ = new QLineEdit(this);
    textInput_->setObjectName(QStringLiteral("annotationInlineTextEditor"));
    textInput_->setPlaceholderText(QStringLiteral("Type text, then press Enter"));
    textInput_->setClearButtonEnabled(true);
    textInput_->setStyleSheet(chromeStyle);
    textInput_->setMinimumHeight(48);
    textInput_->hide();

    actionToolbar_ = new QFrame(this);
    actionToolbar_->setObjectName(QStringLiteral("annotationActions"));
    actionToolbar_->setAttribute(Qt::WA_StyledBackground, true);
    actionToolbar_->setStyleSheet(chromeStyle);
    auto* actionLayout = new QVBoxLayout(actionToolbar_);
    actionLayout->setContentsMargins(7, 7, 7, 7);
    actionLayout->setSpacing(5);
    auto makeAction = [actionLayout](const QString& text,
                                     const QString& iconName) {
        auto* button = new QToolButton();
        button->setText(text);
        button->setIcon(QIcon(QStringLiteral(":/okuflow/icons/") + iconName));
        button->setIconSize(QSize(25, 25));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setFixedSize(84, 72);
        actionLayout->addWidget(button);
        return button;
    };
    undoButton_ = makeAction(QStringLiteral("Undo"), QStringLiteral("undo.svg"));
    redoButton_ = makeAction(QStringLiteral("Redo"), QStringLiteral("redo.svg"));
    snapshotButton_ = makeAction(QStringLiteral("Save"), QStringLiteral("save.svg"));
    clearButton_ = makeAction(QStringLiteral("Clear"), QStringLiteral("clear.svg"));
    exitButton_ = makeAction(QStringLiteral("Done"), QStringLiteral("done.svg"));
    exitButton_->setObjectName(QStringLiteral("annotationDone"));

    SetAccessible(pointerButton_, QStringLiteral("Move annotation tool"),
                  QStringLiteral("Select, move, or resize one annotation. Shortcut V."));
    SetAccessible(penButton_, QStringLiteral("Pen tool"),
                  QStringLiteral("Draw freehand on the camera view. Shortcut P."));
    SetAccessible(lineButton_, QStringLiteral("Line tool"),
                  QStringLiteral("Draw a straight line. Shortcut L."));
    SetAccessible(shapeButton_, QStringLiteral("Shape tool"),
                  QStringLiteral("Draw rectangles or ellipses. Shortcut S."));
    SetAccessible(textButton_, QStringLiteral("Text tool"),
                  QStringLiteral("Click the camera view, then type a label. Shortcut T."));
    SetAccessible(eraserButton_, QStringLiteral("Erase annotation tool"),
                  QStringLiteral("Remove a complete annotation. Shortcut E."));
    SetAccessible(widthSlider_, QStringLiteral("Annotation thickness"),
                  QStringLiteral("Adjust stroke thickness or text size. Mouse wheel scroll does not change it."));
    SetAccessible(textInput_, QStringLiteral("Annotation text"),
                  QStringLiteral("Type text at the selected camera position. Press Enter to place it or Escape to cancel."));
    SetAccessible(solidButton_, QStringLiteral("Solid stroke"),
                  QStringLiteral("Draw an uninterrupted stroke."));
    SetAccessible(dashedButton_, QStringLiteral("Dashed stroke"),
                  QStringLiteral("Draw a dashed stroke with transparent gaps."));
    SetAccessible(rectangleButton_, QStringLiteral("Rectangle shape"),
                  QStringLiteral("Draw a rectangle."));
    SetAccessible(ellipseButton_, QStringLiteral("Ellipse shape"),
                  QStringLiteral("Draw an ellipse."));
    SetAccessible(undoButton_, QStringLiteral("Undo annotation"),
                  QStringLiteral("Undo the last edit. Shortcut Control Z."));
    SetAccessible(redoButton_, QStringLiteral("Redo annotation"),
                  QStringLiteral("Redo the last edit. Shortcut Control Shift Z or Control Y."));
    SetAccessible(snapshotButton_, QStringLiteral("Save annotated view"),
                  QStringLiteral("Save the marked view to lecture notes. Shortcut Control S."));
    SetAccessible(clearButton_, QStringLiteral("Clear annotations"),
                  QStringLiteral("Save a marked snapshot and clear all annotations."));
    SetAccessible(exitButton_, QStringLiteral("Done drawing"),
                  QStringLiteral("Leave Draw mode."));

    auto activateTool = [this](AnnotationTool tool) {
        if (tool_ == tool &&
            tool != AnnotationTool::kPointer &&
            tool != AnnotationTool::kEraser) {
            ShowToolOptions(!optionsPanel_->isVisible());
            return;
        }
        SetTool(tool);
    };
    connect(pointerButton_, &QToolButton::clicked, this,
            [activateTool]() { activateTool(AnnotationTool::kPointer); });
    connect(penButton_, &QToolButton::clicked, this,
            [activateTool]() { activateTool(AnnotationTool::kPen); });
    connect(lineButton_, &QToolButton::clicked, this,
            [activateTool]() { activateTool(AnnotationTool::kLine); });
    connect(shapeButton_, &QToolButton::clicked, this,
            [activateTool]() { activateTool(AnnotationTool::kShape); });
    connect(textButton_, &QToolButton::clicked, this,
            [activateTool]() { activateTool(AnnotationTool::kText); });
    connect(eraserButton_, &QToolButton::clicked, this,
            [activateTool]() { activateTool(AnnotationTool::kEraser); });
    connect(undoButton_, &QToolButton::clicked, this, [this]() { Undo(); });
    connect(redoButton_, &QToolButton::clicked, this, [this]() { Redo(); });
    connect(snapshotButton_, &QToolButton::clicked, this, [this]() {
        CommitPendingText();
        emit SnapshotRequested();
    });
    connect(clearButton_, &QToolButton::clicked, this, [this]() {
        CommitPendingText();
        emit ClearRequested();
    });
    connect(exitButton_, &QToolButton::clicked, this, [this]() {
        CommitPendingText();
        emit ExitRequested();
    });
    connect(textInput_, &QLineEdit::returnPressed,
            this, &AnnotationOverlay::CommitPendingText);
    connect(textInput_, &QLineEdit::editingFinished, this, [this]() {
        if (!committingText_ && pendingTextScenePoint_ &&
            !textInput_->hasFocus()) {
            CommitPendingText();
        }
    });
    connect(widthSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (tool_ == AnnotationTool::kText) {
            textSizePixels_ = value;
        } else {
            inkWidthPixels_ = value;
        }
        SetLiveText(widthValueLabel_,
                    QStringLiteral("%1 px").arg(value),
                    LivePoliteness::kSilent,
                    tool_ == AnnotationTool::kText
                        ? QStringLiteral("Text size")
                        : QStringLiteral("Drawing thickness"));
        EmitPreferences();
    });
    connect(solidButton_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) {
            dashed_ = false;
            EmitPreferences();
            Announce(QStringLiteral("Solid line style"));
        }
    });
    connect(dashedButton_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) {
            dashed_ = true;
            EmitPreferences();
            Announce(QStringLiteral("Dashed line style"));
        }
    });
    connect(rectangleButton_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) {
            shapeKind_ = AnnotationItemKind::kRectangle;
            EmitPreferences();
            Announce(QStringLiteral("Rectangle shape"));
        }
    });
    connect(ellipseButton_, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) {
            shapeKind_ = AnnotationItemKind::kEllipse;
            EmitPreferences();
            Announce(QStringLiteral("Ellipse shape"));
        }
    });

    for (QWidget* target : FocusTargets()) {
        target->installEventFilter(this);
    }
    ShowToolOptions(false);
    UpdateToolOptions();
    UpdateUndoButtons();
}

void AnnotationOverlay::SetActive(bool active)
{
    if (active_ == active) {
        return;
    }
    active_ = active;
    if (active_) {
        SyncGeometryToRenderTarget();
        show();
        raise();
        toolbar_->show();
        actionToolbar_->show();
        toolbar_->raise();
        actionToolbar_->raise();
        SetTool(AnnotationTool::kPen);
        setFocus(Qt::OtherFocusReason);
        Announce(QStringLiteral("Annotation mode on. Pen tool."));
    } else {
        CancelPendingText();
        forwardingMiddleDrag_ = false;
        pointerDragging_ = false;
        scalingSelection_ = false;
        marqueeSelecting_ = false;
        marqueeRectView_ = {};
        model_.EndStroke();
        model_.EndMoveSelection();
        model_.EndScaleSelection();
        model_.ResetSession();
        ShowToolOptions(false);
        UpdateUndoButtons();
        hide();
        Announce(QStringLiteral("Draw mode off."));
    }
}

bool AnnotationOverlay::IsActive() const noexcept { return active_; }
bool AnnotationOverlay::HasInk() const noexcept { return model_.hasInk(); }
const QVector<AnnotationStroke>& AnnotationOverlay::Strokes() const noexcept
{
    return model_.strokes();
}

void AnnotationOverlay::SetViewTransform(const ViewTransform& transform)
{
    if (transform_.sourceX == transform.sourceX &&
        transform_.sourceY == transform.sourceY &&
        transform_.sourceWidth == transform.sourceWidth &&
        transform_.sourceHeight == transform.sourceHeight &&
        transform_.destinationX == transform.destinationX &&
        transform_.destinationY == transform.destinationY &&
        transform_.destinationWidth == transform.destinationWidth &&
        transform_.destinationHeight == transform.destinationHeight &&
        transform_.valid == transform.valid) {
        return;
    }
    transform_ = transform;
    PositionTextEditor();
    update();
}

ViewTransform AnnotationOverlay::CurrentViewTransform() const noexcept
{
    return transform_;
}

void AnnotationOverlay::SetPreferences(const QColor& color,
                                       int widthPixels,
                                       bool captureOnExit,
                                       bool dashed,
                                       const QString& shapeKind,
                                       int textSizePixels)
{
    inkColor_ = color.isValid() ? color : QColor(QStringLiteral("#fff000"));
    inkWidthPixels_ = std::clamp(widthPixels, 2, 24);
    captureOnExit_ = captureOnExit;
    dashed_ = dashed;
    shapeKind_ = shapeKind.compare(QStringLiteral("ellipse"),
                                   Qt::CaseInsensitive) == 0
                     ? AnnotationItemKind::kEllipse
                     : AnnotationItemKind::kRectangle;
    textSizePixels_ = std::clamp(textSizePixels, 12, 72);
    for (QToolButton* button : colorButtons_) {
        const QColor candidate(button->property("inkColor").toString());
        const QSignalBlocker blocker(button);
        const bool selected = candidate == inkColor_;
        button->setChecked(selected);
        button->setIcon(ColorIcon(candidate, selected));
    }
    {
        const QSignalBlocker solidBlocker(solidButton_);
        const QSignalBlocker dashedBlocker(dashedButton_);
        solidButton_->setChecked(!dashed_);
        dashedButton_->setChecked(dashed_);
    }
    {
        const QSignalBlocker rectangleBlocker(rectangleButton_);
        const QSignalBlocker ellipseBlocker(ellipseButton_);
        rectangleButton_->setChecked(shapeKind_ == AnnotationItemKind::kRectangle);
        ellipseButton_->setChecked(shapeKind_ == AnnotationItemKind::kEllipse);
    }
    UpdateToolOptions();
}

QColor AnnotationOverlay::InkColor() const { return inkColor_; }
int AnnotationOverlay::InkWidthPixels() const noexcept { return inkWidthPixels_; }
bool AnnotationOverlay::CaptureOnExit() const noexcept { return captureOnExit_; }
bool AnnotationOverlay::Dashed() const noexcept { return dashed_; }
QString AnnotationOverlay::ShapeKind() const
{
    return shapeKind_ == AnnotationItemKind::kEllipse
               ? QStringLiteral("ellipse")
               : QStringLiteral("rectangle");
}
int AnnotationOverlay::TextSizePixels() const noexcept { return textSizePixels_; }
AnnotationTool AnnotationOverlay::CurrentTool() const noexcept { return tool_; }

QVector<QWidget*> AnnotationOverlay::FocusTargets() const
{
    QVector<QWidget*> targets{
        pointerButton_, penButton_, lineButton_, shapeButton_, textButton_,
        eraserButton_};
    for (QToolButton* color : colorButtons_) {
        targets.push_back(color);
    }
    targets.append({
        widthSlider_, solidButton_, dashedButton_, rectangleButton_,
        ellipseButton_, textInput_, undoButton_, redoButton_,
        snapshotButton_, clearButton_, exitButton_});
    return targets;
}

void AnnotationOverlay::ClearInk()
{
    CancelPendingText();
    model_.Clear();
    UpdateUndoButtons();
    update();
}

bool AnnotationOverlay::Undo()
{
    const bool changed = model_.Undo();
    UpdateUndoButtons();
    if (changed) {
        update();
        Announce(QStringLiteral("Undo"));
    }
    return changed;
}

bool AnnotationOverlay::Redo()
{
    const bool changed = model_.Redo();
    UpdateUndoButtons();
    if (changed) {
        update();
        Announce(QStringLiteral("Redo"));
    }
    return changed;
}

bool AnnotationOverlay::CommitPendingText()
{
    if (!pendingTextScenePoint_ || committingText_) {
        return false;
    }

    committingText_ = true;
    const QPointF scenePoint = *pendingTextScenePoint_;
    const QString text = textInput_->text().trimmed();
    pendingTextScenePoint_.reset();
    textInput_->hide();
    textInput_->clear();

    const std::uint64_t id =
        text.isEmpty()
            ? 0
            : model_.AddText(scenePoint,
                             text,
                             inkColor_,
                             CurrentTextHeight());
    committingText_ = false;
    if (id == 0) {
        Announce(QStringLiteral("Text entry canceled"));
        return false;
    }

    UpdateUndoButtons();
    update();
    Announce(QStringLiteral("Text placed"));
    return true;
}

bool AnnotationOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == excludedWidget_) {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
            event->type() == QEvent::Show || event->type() == QEvent::Hide ||
            event->type() == QEvent::ParentChange ||
            event->type() == QEvent::WindowStateChange) {
            UpdateInputRegion();
        }
        // The chat owns its keyboard input, including Draw shortcuts.
        return QWidget::eventFilter(watched, event);
    }
    if (watched == widthSlider_ && event->type() == QEvent::Wheel) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (watched == textInput_ && keyEvent->key() != Qt::Key_Escape) {
            return QWidget::eventFilter(watched, event);
        }
        if (qobject_cast<QWidget*>(watched)) {
            keyPressEvent(keyEvent);
            if (keyEvent->isAccepted()) {
                return true;
            }
        }
    }
    auto* geometryWidget = qobject_cast<QWidget*>(watched);
    if (geometryWidget && renderTarget_ &&
        (geometryWidget == renderTarget_ || geometryWidget->isAncestorOf(renderTarget_)) &&
        (event->type() == QEvent::Resize ||
         event->type() == QEvent::Move ||
         event->type() == QEvent::Show ||
         event->type() == QEvent::WindowStateChange)) {
        SyncGeometryToRenderTarget();
    }
    return QWidget::eventFilter(watched, event);
}

bool AnnotationOverlay::nativeEvent(const QByteArray& eventType,
                                    void* message,
                                    qintptr* result)
{
    auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage && nativeMessage->message == WM_NCHITTEST) {
        const POINT nativeScreenPoint{
            GET_X_LPARAM(nativeMessage->lParam),
            GET_Y_LPARAM(nativeMessage->lParam)};
        if (excludedWidget_ && excludedWidget_->isVisible() &&
            excludedWidget_->isWindow() &&
            NativeWindowContainsPoint(excludedWidget_, nativeScreenPoint)) {
            *result = HTTRANSPARENT;
            return true;
        }
        if (QWidget* owner = parentWidget()) {
            for (const QString& name :
                 {QStringLiteral("topLeftPanel"),
                  QStringLiteral("bottomLeftPanel"),
                  QStringLiteral("keystoneTrackingPanel"),
                  QStringLiteral("bottomRightPanel"),
                  QStringLiteral("uiVisibilityPanel")}) {
                QWidget* panel = owner->findChild<QWidget*>(name);
                if (!panel || !panel->isVisible()) {
                    continue;
                }
                // WM_NCHITTEST supplies physical screen pixels. Qt widget
                // geometry uses device-independent pixels, so comparing the
                // two directly misses the visible controls at non-100% DPI.
                if (NativeWindowContainsPoint(panel, nativeScreenPoint)) {
                    *result = HTTRANSPARENT;
                    return true;
                }
            }
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

void AnnotationOverlay::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setClipRegion(drawingRegion_);
    painter.fillRect(rect(), QColor(0, 0, 0, 1));
    RenderAnnotationStrokes(painter,
                            model_.strokes(),
                            transform_,
                            size(),
                            model_.selectedStrokeIds());
    if (marqueeSelecting_ && !marqueeRectView_.isEmpty()) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(QColor(0, 229, 255, 30));
        painter.setPen(QPen(QColor(QStringLiteral("#00e5ff")),
                            3.0,
                            Qt::DashLine));
        painter.drawRect(marqueeRectView_.normalized());
    }
}

void AnnotationOverlay::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    PositionToolbar();
    PositionTextEditor();
    UpdateInputRegion();
}

void AnnotationOverlay::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event && event->type() == QEvent::LayoutDirectionChange) {
        UpdateDirectionalUi();
        PositionToolbar();
    }
}

void AnnotationOverlay::SyncGeometryToRenderTarget()
{
    if (!renderTarget_) {
        return;
    }
    const QRect targetGeometry(renderTarget_->mapToGlobal(QPoint(0, 0)),
                               renderTarget_->size());
    if (geometry() != targetGeometry) {
        setGeometry(targetGeometry);
    }
    PositionToolbar();
    UpdateInputRegion();
}

void AnnotationOverlay::SetExcludedWidget(QWidget* widget)
{
    if (excludedWidget_) {
        excludedWidget_->removeEventFilter(this);
        disconnect(excludedWidget_, nullptr, this, nullptr);
    }
    excludedWidget_ = widget;
    if (excludedWidget_) {
        excludedWidget_->installEventFilter(this);
        connect(excludedWidget_, &QObject::destroyed, this, [this]() {
            excludedWidget_ = nullptr;
            UpdateInputRegion();
        });
    }
    UpdateInputRegion();
}

void AnnotationOverlay::UpdateInputRegion()
{
    QRegion region(rect());
    if (excludedWidget_ && excludedWidget_->isVisible()) {
        const QRect excluded(mapFromGlobal(excludedWidget_->mapToGlobal(QPoint(0, 0))),
                             excludedWidget_->size());
        region -= excluded;
    }
    if (region == drawingRegion_) {
        return;
    }
    drawingRegion_ = region;
    PositionToolbar();
    // A native window region removes both ink and input, including toolbar
    // children. Raising Draw later cannot put it back over the floating chat.
    // QWidget treats an empty mask as "no mask", so use an off-window pixel
    // when the panel covers the entire viewport.
    setMask(region.isEmpty() ? QRegion(QRect(-1, -1, 1, 1)) : region);
    update();
}

bool AnnotationOverlay::IsExcluded(const QPoint& position) const
{
    return excludedWidget_ && excludedWidget_->isVisible() &&
           QRect(excludedWidget_->mapToGlobal(QPoint(0, 0)), excludedWidget_->size())
               .contains(mapToGlobal(position));
}

void AnnotationOverlay::PositionToolbar()
{
    if (!toolbar_ || !optionsPanel_ || !actionToolbar_) {
        return;
    }
    toolbar_->adjustSize();
    actionToolbar_->adjustSize();
    int safeTop = 8;
    int safeBottom = height() - 8;
    if (QWidget* owner = parentWidget()) {
        if (QWidget* topPanel =
                owner->findChild<QWidget*>(QStringLiteral("topLeftPanel"))) {
            safeTop = std::clamp(
                mapFromGlobal(topPanel->mapToGlobal(
                    QPoint(0, topPanel->height()))).y() + 8,
                8,
                height() - 8);
        }
        for (const QString& name :
             {QStringLiteral("bottomLeftPanel"),
              QStringLiteral("bottomRightPanel")}) {
            if (QWidget* panel = owner->findChild<QWidget*>(name);
                panel && panel->isVisible()) {
                safeBottom = std::min(
                    safeBottom,
                    std::clamp(
                        mapFromGlobal(panel->mapToGlobal(QPoint(0, 0))).y() - 8,
                        8,
                        height() - 8));
            }
        }
    }
    const int safeHeight = std::max(1, safeBottom - safeTop);
    const bool rightToLeft = layoutDirection() == Qt::RightToLeft;
    toolbar_->move(
        rightToLeft ? std::max(0, width() - toolbar_->width()) : 0,
        safeTop + std::max(0, (safeHeight - toolbar_->height()) / 2));
    actionToolbar_->move(
        rightToLeft ? 0 : std::max(0, width() - actionToolbar_->width()),
        safeTop + std::max(0, (safeHeight - actionToolbar_->height()) / 2));

    if (excludedWidget_ && excludedWidget_->isVisible()) {
        const QRect excluded(mapFromGlobal(excludedWidget_->mapToGlobal(QPoint())),
                             excludedWidget_->size());
        QFrame* leftRail = rightToLeft ? actionToolbar_ : toolbar_;
        QFrame* rightRail = rightToLeft ? toolbar_ : actionToolbar_;
        if (leftRail->geometry().intersects(excluded)) {
            const int x = excluded.right() + 9;
            if (x + leftRail->width() + 8 <= rightRail->x()) {
                leftRail->move(x, leftRail->y());
            } else if (excluded.bottom() + 9 + leftRail->height() <= safeBottom) {
                leftRail->move(leftRail->x(), excluded.bottom() + 9);
            } else if (excluded.top() - 8 - leftRail->height() >= safeTop) {
                leftRail->move(leftRail->x(), excluded.top() - 8 - leftRail->height());
            }
        }
        if (rightRail->geometry().intersects(excluded)) {
            const int x = excluded.left() - 8 - rightRail->width();
            if (x >= leftRail->geometry().right() + 9) {
                rightRail->move(x, rightRail->y());
            } else if (excluded.bottom() + 9 + rightRail->height() <= safeBottom) {
                rightRail->move(rightRail->x(), excluded.bottom() + 9);
            } else if (excluded.top() - 8 - rightRail->height() >= safeTop) {
                rightRail->move(rightRail->x(), excluded.top() - 8 - rightRail->height());
            }
        }
    }

    const int gapStart =
        rightToLeft ? actionToolbar_->geometry().right() + 9
                    : toolbar_->geometry().right() + 9;
    const int gapEnd =
        rightToLeft ? toolbar_->x() - 8
                    : actionToolbar_->x() - 8;
    const int availableWidth = std::max(1, gapEnd - gapStart);
    optionsPanel_->setMaximumWidth(std::min(360, availableWidth));
    optionsPanel_->adjustSize();
    QToolButton* activeButton = penButton_;
    if (tool_ == AnnotationTool::kPointer) activeButton = pointerButton_;
    if (tool_ == AnnotationTool::kLine) activeButton = lineButton_;
    if (tool_ == AnnotationTool::kShape) activeButton = shapeButton_;
    if (tool_ == AnnotationTool::kText) activeButton = textButton_;
    if (tool_ == AnnotationTool::kEraser) activeButton = eraserButton_;
    const int optionsY = std::clamp(
        toolbar_->y() + activeButton->geometry().top(),
        safeTop,
        std::max(safeTop, safeBottom - optionsPanel_->height()));
    const int optionsX =
        rightToLeft ? gapEnd - optionsPanel_->width() : gapStart;
    optionsPanel_->move(std::max(0, optionsX), optionsY);
}

void AnnotationOverlay::UpdateDirectionalUi()
{
    const bool rightToLeft = layoutDirection() == Qt::RightToLeft;
    if (undoButton_) {
        undoButton_->setIcon(QIcon(
            rightToLeft ? QStringLiteral(":/okuflow/icons/redo.svg")
                        : QStringLiteral(":/okuflow/icons/undo.svg")));
    }
    if (redoButton_) {
        redoButton_->setIcon(QIcon(
            rightToLeft ? QStringLiteral(":/okuflow/icons/undo.svg")
                        : QStringLiteral(":/okuflow/icons/redo.svg")));
    }
}

bool AnnotationOverlay::MapViewPointToScene(const QPointF& viewPoint,
                                            QPointF& scenePoint) const
{
    if (!transform_.valid || width() <= 0 || height() <= 0 ||
        IsExcluded(viewPoint.toPoint())) {
        return false;
    }
    const qreal viewU = viewPoint.x() / static_cast<qreal>(width());
    const qreal viewV = viewPoint.y() / static_cast<qreal>(height());
    if (viewU < transform_.destinationX ||
        viewV < transform_.destinationY ||
        viewU > transform_.destinationX + transform_.destinationWidth ||
        viewV > transform_.destinationY + transform_.destinationHeight) {
        return false;
    }
    const qreal localU =
        (viewU - transform_.destinationX) / transform_.destinationWidth;
    const qreal localV =
        (viewV - transform_.destinationY) / transform_.destinationHeight;
    scenePoint = {
        std::clamp<qreal>(transform_.sourceX +
                              localU * transform_.sourceWidth,
                          0.0,
                          1.0),
        std::clamp<qreal>(transform_.sourceY +
                              localV * transform_.sourceHeight,
                          0.0,
                          1.0)};
    return true;
}

QPointF AnnotationOverlay::MapScenePointToView(
    const QPointF& scenePoint) const
{
    if (!transform_.valid || transform_.sourceWidth <= 0.0f ||
        transform_.sourceHeight <= 0.0f) {
        return {};
    }
    const qreal localU =
        (scenePoint.x() - transform_.sourceX) / transform_.sourceWidth;
    const qreal localV =
        (scenePoint.y() - transform_.sourceY) / transform_.sourceHeight;
    return {
        (transform_.destinationX +
         localU * transform_.destinationWidth) * width(),
        (transform_.destinationY +
         localV * transform_.destinationHeight) * height()};
}

void AnnotationOverlay::BeginTextEntry(const QPointF& scenePoint)
{
    if (pendingTextScenePoint_) {
        CommitPendingText();
    }
    pendingTextScenePoint_ = scenePoint;
    textInput_->clear();
    ShowToolOptions(false);
    PositionTextEditor();
    textInput_->show();
    textInput_->raise();
    textInput_->setFocus(Qt::MouseFocusReason);
    Announce(QStringLiteral("Type annotation text. Press Enter to place it."));
}

void AnnotationOverlay::CancelPendingText()
{
    if (!pendingTextScenePoint_) {
        return;
    }
    committingText_ = true;
    pendingTextScenePoint_.reset();
    textInput_->hide();
    textInput_->clear();
    committingText_ = false;
}

void AnnotationOverlay::PositionTextEditor()
{
    if (!textInput_ || !pendingTextScenePoint_ || width() <= 0 ||
        height() <= 0) {
        return;
    }
    const QPointF anchor = MapScenePointToView(*pendingTextScenePoint_);
    const int editorWidth = std::clamp(width() / 4, 240, 420);
    const int editorHeight = std::max(48, textInput_->sizeHint().height());
    const int x = std::clamp(
        static_cast<int>(std::lround(anchor.x())),
        8,
        std::max(8, width() - editorWidth - 8));
    const int y = std::clamp(
        static_cast<int>(std::lround(anchor.y() - editorHeight * 0.5)),
        8,
        std::max(8, height() - editorHeight - 8));
    textInput_->setGeometry(x, y, editorWidth, editorHeight);
}

qreal AnnotationOverlay::CurrentSceneWidth() const
{
    return AnnotationSceneTolerance(transform_, size(), inkWidthPixels_);
}

qreal AnnotationOverlay::CurrentTextHeight() const
{
    return AnnotationSceneTolerance(transform_, size(), textSizePixels_);
}

qreal AnnotationOverlay::CurrentHitTolerance() const
{
    return AnnotationSceneTolerance(transform_, size(), 16.0);
}

bool AnnotationOverlay::IsOverChrome(const QPoint& position) const
{
    return IsExcluded(position) ||
           (toolbar_ && toolbar_->isVisible() &&
            toolbar_->geometry().contains(position)) ||
           (optionsPanel_ && optionsPanel_->isVisible() &&
            optionsPanel_->geometry().contains(position)) ||
           (actionToolbar_ && actionToolbar_->isVisible() &&
            actionToolbar_->geometry().contains(position)) ||
           (textInput_ && textInput_->isVisible() &&
            textInput_->geometry().contains(position));
}

QRectF AnnotationOverlay::SelectedBoundsInView() const
{
    QRectF bounds;
    bool haveBounds = false;
    for (const std::uint64_t id : model_.selectedStrokeIds()) {
        const auto it = std::find_if(
            model_.strokes().cbegin(),
            model_.strokes().cend(),
            [id](const AnnotationStroke& stroke) { return stroke.id == id; });
        if (it == model_.strokes().cend()) {
            continue;
        }
        const QRectF itemBounds =
            BuildStrokePath(*it, transform_, size()).boundingRect();
        bounds = haveBounds ? bounds.united(itemBounds) : itemBounds;
        haveBounds = true;
    }
    return haveBounds
               ? bounds.adjusted(-10.0, -10.0, 10.0, 10.0)
               : QRectF();
}

int AnnotationOverlay::ScaleHandleAt(const QPointF& viewPoint) const
{
    if (model_.selectedStrokeIds().size() != 1) {
        return -1;
    }
    const QRectF bounds = SelectedBoundsInView();
    if (bounds.isEmpty()) {
        return -1;
    }
    constexpr qreal hitSize = 22.0;
    const auto handles = HandlePoints(bounds);
    for (int i = 0; i < static_cast<int>(handles.size()); ++i) {
        if (QRectF(handles[static_cast<std::size_t>(i)] -
                       QPointF(hitSize * 0.5, hitSize * 0.5),
                   QSizeF(hitSize, hitSize))
                .contains(viewPoint)) {
            return i;
        }
    }
    return -1;
}

void AnnotationOverlay::UpdatePointerCursor(const QPointF& viewPoint)
{
    if (tool_ != AnnotationTool::kPointer) {
        unsetCursor();
        return;
    }
    switch (ScaleHandleAt(viewPoint)) {
    case 0:
    case 4:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case 2:
    case 6:
        setCursor(Qt::SizeBDiagCursor);
        break;
    case 1:
    case 5:
        setCursor(Qt::SizeVerCursor);
        break;
    case 3:
    case 7:
        setCursor(Qt::SizeHorCursor);
        break;
    default:
        setCursor(Qt::ArrowCursor);
        break;
    }
}

void AnnotationOverlay::mousePressEvent(QMouseEvent* event)
{
    if (!active_) {
        event->ignore();
        return;
    }
    if (event->button() == Qt::MiddleButton &&
        !IsOverChrome(event->position().toPoint())) {
        forwardingMiddleDrag_ = true;
        ForwardMouseToRenderTarget(event) ? event->accept() : event->ignore();
        return;
    }
    if (event->button() != Qt::LeftButton ||
        IsOverChrome(event->position().toPoint())) {
        event->ignore();
        return;
    }
    QPointF scenePoint;
    if (!MapViewPointToScene(event->position(), scenePoint)) {
        event->ignore();
        return;
    }
    ShowToolOptions(false);
    strokeStartView_ = event->position();

    if (tool_ == AnnotationTool::kPen ||
        tool_ == AnnotationTool::kLine ||
        tool_ == AnnotationTool::kShape) {
        AnnotationItemKind kind = AnnotationItemKind::kFreehand;
        if (tool_ == AnnotationTool::kLine) {
            kind = AnnotationItemKind::kLine;
        } else if (tool_ == AnnotationTool::kShape) {
            kind = shapeKind_;
        }
        model_.BeginStroke(scenePoint,
                           inkColor_,
                           CurrentSceneWidth(),
                           kind,
                           dashed_);
    } else if (tool_ == AnnotationTool::kText) {
        BeginTextEntry(scenePoint);
    } else if (tool_ == AnnotationTool::kEraser) {
        model_.EraseAt(scenePoint, CurrentHitTolerance());
    } else {
        const bool extendSelection =
            event->modifiers() &
            (Qt::ShiftModifier | Qt::ControlModifier);
        activeScaleHandle_ =
            extendSelection ? -1 : ScaleHandleAt(event->position());
        if (activeScaleHandle_ >= 0) {
            scaleStartBoundsView_ = SelectedBoundsInView();
            const auto handles = HandlePoints(scaleStartBoundsView_);
            const int opposite = (activeScaleHandle_ + 4) % 8;
            const AnnotationStroke* selected = model_.selectedStroke();
            const QRectF itemBounds =
                selected ? BuildStrokePath(*selected, transform_, size())
                               .boundingRect()
                         : QRectF();
            const auto itemHandles = HandlePoints(itemBounds);
            if (!itemBounds.isEmpty() &&
                MapViewPointToScene(
                    itemHandles[static_cast<std::size_t>(opposite)],
                    scaleAnchorScene_)) {
                scalingSelection_ = model_.BeginScaleSelection();
            }
        } else {
            const auto hit =
                model_.HitTest(scenePoint, CurrentHitTolerance());
            const bool hitWasSelected =
                hit && model_.selectedStrokeIds().contains(*hit);
            bool selected = false;
            if (hit && hitWasSelected && !extendSelection) {
                selected = true;
            } else if (hit) {
                selected = model_.SelectAt(scenePoint,
                                           CurrentHitTolerance(),
                                           extendSelection);
            }
            if (selected) {
                pointerDragging_ = model_.BeginMoveSelection();
                previousPointerScene_ = scenePoint;
                const int count = model_.selectedStrokeIds().size();
                Announce(
                    count == 1
                        ? QStringLiteral(
                              "Annotation selected. Drag to move, or use "
                              "handles to resize.")
                        : QStringLiteral("%1 annotations selected. Drag to "
                                         "move the group.")
                              .arg(count));
            } else if (!hit) {
                marqueeSelecting_ = true;
                marqueeAdditive_ = extendSelection;
                marqueeStartView_ = event->position();
                marqueeRectView_ =
                    QRectF(marqueeStartView_, marqueeStartView_);
                if (!marqueeAdditive_) {
                    model_.ClearSelection();
                }
            }
        }
    }
    UpdateUndoButtons();
    update();
    event->accept();
}

void AnnotationOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!active_) {
        event->ignore();
        return;
    }
    if (forwardingMiddleDrag_ ||
        (event->buttons() & Qt::MiddleButton)) {
        ForwardMouseToRenderTarget(event) ? event->accept() : event->ignore();
        return;
    }
    if (!(event->buttons() & Qt::LeftButton)) {
        UpdatePointerCursor(event->position());
        event->accept();
        return;
    }

    bool changed = false;
    if (marqueeSelecting_) {
        marqueeRectView_ =
            QRectF(marqueeStartView_, event->position()).normalized();
        update();
        event->accept();
        return;
    }
    if (scalingSelection_ && activeScaleHandle_ >= 0) {
        const auto handles = HandlePoints(scaleStartBoundsView_);
        const QPointF start =
            handles[static_cast<std::size_t>(activeScaleHandle_)];
        const QPointF anchor =
            handles[static_cast<std::size_t>((activeScaleHandle_ + 4) % 8)];
        const QPointF original = start - anchor;
        const QPointF current = event->position() - anchor;
        qreal scaleX = std::abs(original.x()) > 0.1
                           ? current.x() / original.x()
                           : 1.0;
        qreal scaleY = std::abs(original.y()) > 0.1
                           ? current.y() / original.y()
                           : 1.0;
        if (activeScaleHandle_ == 1 || activeScaleHandle_ == 5) {
            scaleX = 1.0;
        }
        if (activeScaleHandle_ == 3 || activeScaleHandle_ == 7) {
            scaleY = 1.0;
        }
        const qreal minScaleX =
            24.0 / std::max<qreal>(24.0, scaleStartBoundsView_.width());
        const qreal minScaleY =
            24.0 / std::max<qreal>(24.0, scaleStartBoundsView_.height());
        scaleX = std::max(scaleX, minScaleX);
        scaleY = std::max(scaleY, minScaleY);
        const AnnotationStroke* selected = model_.selectedStroke();
        const bool forceUniform =
            selected && selected->kind == AnnotationItemKind::kText;
        const bool corner = activeScaleHandle_ % 2 == 0;
        if (forceUniform ||
            (corner && !(event->modifiers() & Qt::ShiftModifier))) {
            const qreal uniform = std::max(scaleX, scaleY);
            scaleX = uniform;
            scaleY = uniform;
        }
        changed =
            model_.ScaleSelection(scaleAnchorScene_, scaleX, scaleY);
    } else {
        QPointF viewPoint = event->position();
        if (tool_ == AnnotationTool::kShape &&
            (event->modifiers() & Qt::ShiftModifier)) {
            QPointF delta = viewPoint - strokeStartView_;
            const qreal extent = std::max(std::abs(delta.x()),
                                          std::abs(delta.y()));
            delta.setX(std::copysign(extent, delta.x()));
            delta.setY(std::copysign(extent, delta.y()));
            viewPoint = strokeStartView_ + delta;
        }
        QPointF scenePoint;
        if (!MapViewPointToScene(viewPoint, scenePoint)) {
            event->ignore();
            return;
        }
        if (tool_ == AnnotationTool::kPen) {
            changed = model_.AppendStrokePoint(
                scenePoint,
                AnnotationSceneTolerance(transform_, size(), 2.0));
        } else if (tool_ == AnnotationTool::kLine ||
                   tool_ == AnnotationTool::kShape) {
            changed = model_.SetStraightStrokeEnd(scenePoint);
        } else if (tool_ == AnnotationTool::kEraser) {
            changed = model_.EraseAt(scenePoint, CurrentHitTolerance());
        } else if (pointerDragging_) {
            changed = model_.MoveSelection(scenePoint - previousPointerScene_);
            previousPointerScene_ = scenePoint;
        }
    }
    if (changed) {
        UpdateUndoButtons();
        update();
    }
    event->accept();
}

void AnnotationOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton ||
        forwardingMiddleDrag_) {
        ForwardMouseToRenderTarget(event) ? event->accept() : event->ignore();
        forwardingMiddleDrag_ = false;
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (marqueeSelecting_) {
        const QRectF activeDestination(
            transform_.destinationX * width(),
            transform_.destinationY * height(),
            transform_.destinationWidth * width(),
            transform_.destinationHeight * height());
        const QRectF selectionView =
            marqueeRectView_.normalized().intersected(activeDestination);
        QPointF topLeftScene;
        QPointF bottomRightScene;
        int selectedCount = model_.selectedStrokeIds().size();
        if (!selectionView.isEmpty() &&
            MapViewPointToScene(selectionView.topLeft(), topLeftScene) &&
            MapViewPointToScene(selectionView.bottomRight(),
                                bottomRightScene)) {
            selectedCount = model_.SelectInRect(
                QRectF(topLeftScene, bottomRightScene),
                marqueeAdditive_);
        }
        marqueeSelecting_ = false;
        marqueeAdditive_ = false;
        marqueeRectView_ = {};
        Announce(selectedCount == 0
                     ? QStringLiteral("Nothing selected")
                     : QStringLiteral("%1 annotation%2 selected")
                           .arg(selectedCount)
                           .arg(selectedCount == 1
                                    ? QString()
                                    : QStringLiteral("s")));
    }
    model_.EndStroke();
    model_.EndMoveSelection();
    model_.EndScaleSelection();
    pointerDragging_ = false;
    scalingSelection_ = false;
    activeScaleHandle_ = -1;
    UpdateUndoButtons();
    update();
    event->accept();
}

bool AnnotationOverlay::ForwardMouseToRenderTarget(QMouseEvent* event)
{
    if (!renderTarget_) {
        return false;
    }
    const QPointF global = event->globalPosition();
    const QPointF local = renderTarget_->mapFromGlobal(global.toPoint());
    QMouseEvent forwarded(event->type(),
                          local,
                          local,
                          global,
                          event->button(),
                          event->buttons(),
                          event->modifiers(),
                          event->pointingDevice());
    QCoreApplication::sendEvent(renderTarget_, &forwarded);
    return forwarded.isAccepted();
}

bool AnnotationOverlay::ForwardWheelToRenderTarget(QWheelEvent* event)
{
    if (!renderTarget_ || IsOverChrome(event->position().toPoint())) {
        return false;
    }
    const QPointF global = event->globalPosition();
    const QPointF local = renderTarget_->mapFromGlobal(global.toPoint());
    QWheelEvent forwarded(local,
                          global,
                          event->pixelDelta(),
                          event->angleDelta(),
                          event->buttons(),
                          event->modifiers(),
                          event->phase(),
                          event->inverted(),
                          event->source(),
                          event->pointingDevice());
    QCoreApplication::sendEvent(renderTarget_, &forwarded);
    return forwarded.isAccepted();
}

void AnnotationOverlay::wheelEvent(QWheelEvent* event)
{
    ForwardWheelToRenderTarget(event) ? event->accept() : event->ignore();
}

bool AnnotationOverlay::ForwardKeyToOwner(QKeyEvent* event)
{
    QWidget* owner = parentWidget();
    if (!owner) {
        return false;
    }
    QKeyEvent forwarded(event->type(),
                        event->key(),
                        event->modifiers(),
                        event->text(),
                        event->isAutoRepeat(),
                        event->count());
    QCoreApplication::sendEvent(owner, &forwarded);
    return forwarded.isAccepted();
}

void AnnotationOverlay::keyPressEvent(QKeyEvent* event)
{
    const bool control = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    if (control && event->key() == Qt::Key_Z) {
        shift ? Redo() : Undo();
        event->accept();
        return;
    }
    if (control && event->key() == Qt::Key_Y) {
        Redo();
        event->accept();
        return;
    }
    if (control && event->key() == Qt::Key_S) {
        if (model_.hasInk()) {
            emit SnapshotRequested();
        }
        event->accept();
        return;
    }
    if (control &&
        (event->key() == Qt::Key_Plus ||
         event->key() == Qt::Key_Equal ||
         event->key() == Qt::Key_Minus)) {
        if (model_.selectedStrokeId()) {
            const qreal step = shift ? 0.15 : 0.05;
            const qreal factor =
                event->key() == Qt::Key_Minus ? 1.0 - step : 1.0 + step;
            if (model_.ScaleSelectionAboutCenter(factor)) {
                UpdateUndoButtons();
                update();
                Announce(QStringLiteral("Selected annotation scaled"));
            }
            event->accept();
            return;
        }
        if (ForwardKeyToOwner(event)) {
            event->accept();
            return;
        }
    }
    if (!control) {
        if (event->key() == Qt::Key_P) penButton_->click();
        else if (event->key() == Qt::Key_L) lineButton_->click();
        else if (event->key() == Qt::Key_S) shapeButton_->click();
        else if (event->key() == Qt::Key_T) textButton_->click();
        else if (event->key() == Qt::Key_V) pointerButton_->click();
        else if (event->key() == Qt::Key_E) eraserButton_->click();
        else goto not_a_tool_shortcut;
        event->accept();
        return;
    }

not_a_tool_shortcut:
    if (event->key() == Qt::Key_Delete && model_.DeleteSelection()) {
        UpdateUndoButtons();
        update();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (pendingTextScenePoint_) {
            CancelPendingText();
            setFocus(Qt::OtherFocusReason);
            Announce(QStringLiteral("Text entry canceled"));
        } else if (optionsPanel_->isVisible()) {
            ShowToolOptions(false);
        } else if (model_.selectedStrokeId()) {
            model_.ClearSelection();
            update();
            Announce(QStringLiteral("Selection cleared"));
        } else {
            emit ExitRequested();
        }
        event->accept();
        return;
    }
    QPointF nudge;
    if (event->key() == Qt::Key_Left) nudge.setX(-1.0);
    if (event->key() == Qt::Key_Right) nudge.setX(1.0);
    if (event->key() == Qt::Key_Up) nudge.setY(-1.0);
    if (event->key() == Qt::Key_Down) nudge.setY(1.0);
    if (!nudge.isNull()) {
        if (model_.selectedStrokeId()) {
            const qreal amount =
                AnnotationSceneTolerance(transform_, size(), shift ? 10.0 : 2.0);
            if (model_.NudgeSelection(nudge * amount)) {
                UpdateUndoButtons();
                update();
            }
            event->accept();
            return;
        }
        if (ForwardKeyToOwner(event)) {
            event->accept();
            return;
        }
    }
    if (!model_.selectedStrokeId() &&
        (event->key() == Qt::Key_PageUp ||
         event->key() == Qt::Key_PageDown ||
         event->key() == Qt::Key_Plus ||
         event->key() == Qt::Key_Equal ||
         event->key() == Qt::Key_Minus)) {
        if (ForwardKeyToOwner(event)) {
            event->accept();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void AnnotationOverlay::SetTool(AnnotationTool tool)
{
    if (tool_ == AnnotationTool::kText &&
        tool != AnnotationTool::kText &&
        pendingTextScenePoint_) {
        CommitPendingText();
    }
    tool_ = tool;
    pointerButton_->setChecked(tool == AnnotationTool::kPointer);
    penButton_->setChecked(tool == AnnotationTool::kPen);
    lineButton_->setChecked(tool == AnnotationTool::kLine);
    shapeButton_->setChecked(tool == AnnotationTool::kShape);
    textButton_->setChecked(tool == AnnotationTool::kText);
    eraserButton_->setChecked(tool == AnnotationTool::kEraser);
    if (tool == AnnotationTool::kPointer ||
        tool == AnnotationTool::kEraser) {
        ShowToolOptions(false);
    } else {
        ShowToolOptions(true);
    }
    UpdateToolOptions();
    const QString name = ToolName(tool);
    emit ToolChanged(name);
    Announce(name);
}

void AnnotationOverlay::ShowToolOptions(bool visible)
{
    const bool supportsOptions =
        tool_ == AnnotationTool::kPen ||
        tool_ == AnnotationTool::kLine ||
        tool_ == AnnotationTool::kShape ||
        tool_ == AnnotationTool::kText;
    optionsPanel_->setVisible(visible && supportsOptions && active_);
    if (optionsPanel_->isVisible()) {
        PositionToolbar();
        optionsPanel_->raise();
    } else {
        QToolButton* activeButton = penButton_;
        if (tool_ == AnnotationTool::kLine) activeButton = lineButton_;
        if (tool_ == AnnotationTool::kShape) activeButton = shapeButton_;
        if (tool_ == AnnotationTool::kText) activeButton = textButton_;
        if (tool_ == AnnotationTool::kPointer) activeButton = pointerButton_;
        if (tool_ == AnnotationTool::kEraser) activeButton = eraserButton_;
        if (active_ && activeButton) {
            activeButton->setFocus(Qt::OtherFocusReason);
        }
    }
}

void AnnotationOverlay::UpdateToolOptions()
{
    const bool text = tool_ == AnnotationTool::kText;
    const bool shape = tool_ == AnnotationTool::kShape;
    const bool styled = tool_ == AnnotationTool::kPen ||
                        tool_ == AnnotationTool::kLine ||
                        shape;
    solidButton_->setVisible(styled);
    dashedButton_->setVisible(styled);
    rectangleButton_->setVisible(shape);
    ellipseButton_->setVisible(shape);
    SetLiveText(widthTitleLabel_,
                text ? QStringLiteral("Text size")
                     : QStringLiteral("Thickness"),
                LivePoliteness::kSilent);
    {
        const QSignalBlocker blocker(widthSlider_);
        widthSlider_->setRange(text ? 12 : 2, text ? 72 : 24);
        widthSlider_->setValue(text ? textSizePixels_ : inkWidthPixels_);
        SetLiveText(
            widthValueLabel_,
            QStringLiteral("%1 px").arg(
                text ? textSizePixels_ : inkWidthPixels_),
            LivePoliteness::kSilent,
            text ? QStringLiteral("Text size")
                 : QStringLiteral("Drawing thickness"));
    }
    if (text) {
        SetLiveText(optionsTitleLabel_, QStringLiteral("Text"),
                    LivePoliteness::kSilent,
                    QStringLiteral("Annotation options"));
    } else if (shape) {
        SetLiveText(optionsTitleLabel_, QStringLiteral("Shape"),
                    LivePoliteness::kSilent,
                    QStringLiteral("Annotation options"));
    } else if (tool_ == AnnotationTool::kLine) {
        SetLiveText(optionsTitleLabel_, QStringLiteral("Line"),
                    LivePoliteness::kSilent,
                    QStringLiteral("Annotation options"));
    } else {
        SetLiveText(optionsTitleLabel_, QStringLiteral("Pen"),
                    LivePoliteness::kSilent,
                    QStringLiteral("Annotation options"));
    }
    PositionToolbar();
}

void AnnotationOverlay::UpdateUndoButtons()
{
    undoButton_->setEnabled(model_.canUndo());
    redoButton_->setEnabled(model_.canRedo());
    snapshotButton_->setEnabled(model_.hasInk());
    clearButton_->setEnabled(model_.hasInk());
}

void AnnotationOverlay::EmitPreferences()
{
    emit PreferencesChanged(inkColor_.name(QColor::HexRgb),
                            inkWidthPixels_,
                            captureOnExit_,
                            dashed_,
                            ShapeKind(),
                            textSizePixels_);
}

void AnnotationOverlay::Announce(const QString& message)
{
    if (message.isEmpty()) {
        return;
    }
    QAccessibleAnnouncementEvent event(this, message);
    event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&event);
}

} // namespace okuflow

#endif // _WIN32
