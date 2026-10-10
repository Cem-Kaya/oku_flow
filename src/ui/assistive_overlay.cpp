#ifdef _WIN32

#include "okuflow/ui/assistive_overlay.hpp"
#include "okuflow/ui/live_status_text.hpp"
#include "okuflow/ui/ui_translation.hpp"

#include <QEvent>
#include <QApplication>
#include <QActionGroup>
#include <QMenu>
#include <QTextBlockFormat>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMainWindow>
#include <QPushButton>
#include <QPainter>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QTextCursor>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <array>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace okuflow {

namespace {
class DockPreviewLabel final : public QLabel {
public:
    using QLabel::QLabel;

protected:
    void paintEvent(QPaintEvent*) override
    {
        // Translucent native tool windows cannot rely on stylesheet background
        // propagation. Paint the target, border, and instruction explicitly.
        QPainter painter(this);
        painter.setBrush(QColor(74, 20, 91, 180));
        painter.setPen(QPen(QColor(QStringLiteral("#e7a7ff")), 6));
        painter.drawRect(QRectF(rect()).adjusted(3, 3, -3, -3));
        painter.setPen(Qt::white);
        painter.drawText(rect().adjusted(18, 18, -18, -18),
                          Qt::AlignCenter | Qt::TextWordWrap, text());
    }
};

QString SpokenAssistiveText(QString body)
{
    const std::array<QString, 2> sectionLabels{
        QStringLiteral("Read Text"),
        QStringLiteral("Scene Explain")};
    for (const QString& label : sectionLabels) {
        const QString leadingHeader = label + QLatin1Char('\n');
        if (body.startsWith(leadingHeader)) {
            body.remove(0, leadingHeader.size());
        }
        body.replace(QStringLiteral("\n\n%1\n").arg(label), QStringLiteral("\n\n"));
    }
    return body.trimmed();
}

} // namespace

AssistiveOverlay::AssistiveOverlay(QWidget* parent)
    : QDockWidget(parent ? parent->window() : nullptr),
      renderTarget_(parent),
      dockHost_(qobject_cast<QMainWindow*>(parent ? parent->window() : nullptr))
{
    setObjectName(QStringLiteral("assistiveOverlay"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setMinimumSize(360, 260);
    setAccessibleName(QStringLiteral("Assistive results"));
    setStyleSheet(QStringLiteral(R"(
        QDockWidget#assistiveOverlay {
            background: #111111;
            border: 3px solid #f2f2f2;
            border-radius: 8px;
        }
        QLabel#assistiveTitle {
            color: #fff7d6;
            font-size: 14pt;
            font-weight: 700;
            border: none;
        }
        QTextBrowser#assistiveBody {
            color: #f5f5f5;
            background: transparent;
            border: none;
            font-size: 20pt;
            selection-background-color: #a84bc1;
            selection-color: #ffffff;
        }
        QPushButton#assistiveReadButton, QPushButton#assistiveAskButton {
            color: #ffffff;
            background: #3d3d3d;
            border: 2px solid #737373;
            border-radius: 6px;
            min-height: 36px;
        }
        QPushButton#assistiveReadButton:focus, QPushButton#assistiveAskButton:focus {
            border: 3px solid #bd52d3;
        }
        QPushButton#assistiveReadButton {
            background: #74328b;
            border-color: #e5b5f2;
            font-size: 16pt;
            font-weight: 600;
            min-height: 48px;
            padding: 2px 12px;
        }
        QToolButton#assistiveDockPosition {
            color: #ffffff;
            background: #303030;
            border: 2px solid #9a9a9a;
            border-radius: 6px;
            font-size: 20pt;
        }
        QToolButton#assistiveDockPosition:focus { border-color: #e5b5f2; }
        QLineEdit#assistiveQuestion {
            color: #ffffff;
            background: #202020;
            border: 2px solid #9a9a9a;
            border-radius: 6px;
            min-height: 38px;
            padding: 2px 8px;
        }
        QLineEdit#assistiveQuestion:focus { border: 3px solid #bd52d3; }
        QToolButton#assistiveCloseButton {
            background: #080808;
            border: 3px solid #ffffff;
            border-radius: 6px;
            padding: 7px;
        }
        QToolButton#assistiveCloseButton:hover,
        QToolButton#assistiveCloseButton:focus {
            background: #a747c5;
            border-color: #ffffff;
        }
        QToolButton#assistiveCloseButton:pressed { background: #733087; }
    )"));

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(8);

    headerWidget_ = new QWidget(this);
    headerWidget_->setObjectName(QStringLiteral("assistiveHeader"));
    headerWidget_->setCursor(Qt::SizeAllCursor);
    headerWidget_->setAccessibleName(QStringLiteral("Move assistive panel"));
    auto* header = new QHBoxLayout(headerWidget_);
    header->setContentsMargins(14, 10, 14, 0);
    header->setSpacing(8);
    newChatButton_ = new QPushButton(QStringLiteral("New Conversation"));
    newChatButton_->setObjectName(QStringLiteral("assistiveNewChatButton"));
    newChatButton_->setCursor(Qt::ArrowCursor);
    newChatButton_->setAccessibleName(QStringLiteral("New Conversation"));
    newChatButton_->setAccessibleDescription(
        QStringLiteral("Start a fresh assistant conversation. The next answer "
                       "begins a new conversation section in the lecture notes."));
    // Reading controls take precedence; New Conversation is secondary below.

    titleLabel_ = new QLabel();
    titleLabel_->setObjectName(QStringLiteral("assistiveTitle"));
    titleLabel_->setWordWrap(true);
    titleLabel_->setAccessibleName(QStringLiteral("Assistive result title"));
    header->addWidget(titleLabel_, 1);

    dockPositionButton_ = new QToolButton();
    dockPositionButton_->setObjectName(QStringLiteral("assistiveDockPosition"));
    dockPositionButton_->setText(QStringLiteral("⋯"));
    dockPositionButton_->setAccessibleName(QStringLiteral("Panel position"));
    dockPositionButton_->setToolTip(QStringLiteral("Panel position"));
    dockPositionButton_->setFixedSize(48, 48);
    dockPositionButton_->setCursor(Qt::ArrowCursor);
    dockPositionButton_->setPopupMode(QToolButton::InstantPopup);
    auto* placementMenu = new QMenu(dockPositionButton_);
    auto* placementGroup = new QActionGroup(placementMenu);
    const std::array<QString, 3> positions{QStringLiteral("floating"),
        QStringLiteral("left"), QStringLiteral("right")};
    const std::array<QString, 3> labels{QStringLiteral("Floating"),
        QStringLiteral("Dock left"), QStringLiteral("Dock right")};
    for (size_t index = 0; index < positions.size(); ++index) {
        QAction* action = placementMenu->addAction(TranslateUi(labels[index]));
        action->setProperty("_okuflowPlacementSource", labels[index]);
        action->setData(positions[index]);
        action->setCheckable(true);
        placementGroup->addAction(action);
        dockPositionActions_[index] = action;
        connect(action, &QAction::triggered, this, [this, position = positions[index]]() {
            SetDockPosition(position);
        });
    }
    dockPositionButton_->setMenu(placementMenu);
    connect(placementMenu, &QMenu::aboutToShow,
            this, &AssistiveOverlay::UpdateDockPositionControl);
    dockPositionButton_->setVisible(dockHost_ != nullptr);
    header->addWidget(dockPositionButton_);

    closeButton_ = new QToolButton();
    closeButton_->setObjectName(QStringLiteral("assistiveCloseButton"));
    closeButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/close.svg")));
    closeButton_->setIconSize(QSize(28, 28));
    closeButton_->setToolTip(QStringLiteral("Close result"));
    closeButton_->setAccessibleName(QStringLiteral("Close assistive result"));
    closeButton_->setAccessibleDescription(
        QStringLiteral("Hide the current assistant result"));
    closeButton_->setFixedSize(52, 52);
    header->addWidget(closeButton_);
    setTitleBarWidget(headerWidget_);

    bodyView_ = new QTextBrowser();
    bodyView_->setObjectName(QStringLiteral("assistiveBody"));
    bodyView_->setReadOnly(true);
    bodyView_->setOpenExternalLinks(false);
    bodyView_->setFocusPolicy(Qt::StrongFocus);
    bodyView_->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    bodyView_->setAccessibleName(QStringLiteral("Assistive result text"));
    bodyView_->setAccessibleDescription(
        QStringLiteral("Streaming assistant result. Use arrow keys to read the text."));
    layout->addWidget(bodyView_, 1);
    QFont answerFont = bodyView_->font();
    answerFont.setPointSize(20);
    bodyView_->setFont(answerFont);

    auto* questionRow = new QHBoxLayout();
    questionRow->setSpacing(8);
    questionEdit_ = new QLineEdit();
    questionEdit_->setObjectName(QStringLiteral("assistiveQuestion"));
    questionEdit_->setPlaceholderText(QStringLiteral("Ask about this view..."));
    questionEdit_->setAccessibleName(QStringLiteral("Question about the current view"));
    questionEdit_->setAccessibleDescription(
        QStringLiteral("Type a follow-up question at any time. Sending becomes available when the current answer finishes."));
    questionEdit_->installEventFilter(this);
    askButton_ = new QPushButton(QStringLiteral("Ask"));
    askButton_->setObjectName(QStringLiteral("assistiveAskButton"));
    askButton_->setEnabled(false);
    askButton_->setAccessibleName(QStringLiteral("Ask Assistant"));
    askButton_->setAccessibleDescription(
        QStringLiteral("Send the question with the current camera view to OkuFlow Assistant"));
    questionRow->addWidget(questionEdit_, 1);
    questionRow->addWidget(askButton_);
    layout->addLayout(questionRow);

    auto* footer = new QHBoxLayout();
    readAloudButton_ = new QPushButton(QStringLiteral("Read Aloud"));
    readAloudButton_->setObjectName(QStringLiteral("assistiveReadButton"));
    readAloudButton_->setIcon(QIcon(QStringLiteral(":/okuflow/icons/read.svg")));
    readAloudButton_->setIconSize(QSize(24, 24));
    readAloudButton_->setAccessibleName(QStringLiteral("Read assistive result aloud"));
    readAloudButton_->setAccessibleDescription(
        QStringLiteral("Speak the current assistant result"));
    footer->addWidget(readAloudButton_);
    footer->addStretch(1);
    footer->addWidget(newChatButton_);
    layout->addLayout(footer);
    setWidget(content);
    const auto focusTargets = FocusTargets();
    for (size_t index = 1; index < focusTargets.size(); ++index) {
        QWidget::setTabOrder(focusTargets[index - 1], focusTargets[index]);
    }
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    // Header gestures are owned below. Qt's automatic dock dragging otherwise
    // competes with native floating moves and can repeatedly plug/unplug the dock.
    setFeatures(QDockWidget::DockWidgetFloatable);
    dockDebounceTimer_ = new QTimer(this);
    dockDebounceTimer_->setSingleShot(true);
    dockDebounceTimer_->setTimerType(Qt::PreciseTimer);
    connect(dockDebounceTimer_, &QTimer::timeout, this, &AssistiveOverlay::UpdateDockPreview);
    undockTimer_ = new QTimer(this);
    undockTimer_->setSingleShot(true);
    undockTimer_->setInterval(350);
    connect(undockTimer_, &QTimer::timeout, this, &AssistiveOverlay::ReleaseDockLatch);
    if (dockHost_) {
        dockHost_->addDockWidget(Qt::LeftDockWidgetArea, this);
    }
    setFloating(true);
    UpdateDockPositionControl();
    connect(this, &QDockWidget::topLevelChanged, this, [this](bool floating) {
        if (floating && !changingDockPosition_) {
            placementInitialized_ = false;
            UpdatePlacement();
        }
        UpdateDockPositionControl();
    });
    connect(this, &QDockWidget::dockLocationChanged,
            this, &AssistiveOverlay::UpdateDockPositionControl);

    connect(closeButton_, &QToolButton::clicked, this, [this]() {
        desiredVisible_ = false;
        hide();
        emit Dismissed();
    });
    connect(newChatButton_, &QPushButton::clicked, this, [this]() {
        questionEdit_->clear();
        questionEdit_->setFocus();
        emit NewChatRequested();
    });
    connect(readAloudButton_, &QPushButton::clicked, this, [this]() {
        const QString speechText = SpokenAssistiveText(body_);
        if (!speechText.isEmpty()) {
            emit ReadAloudRequested(speechText);
        }
    });
    connect(questionEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        askButton_->setEnabled(!busy_ && !text.trimmed().isEmpty());
    });
    connect(questionEdit_, &QLineEdit::returnPressed, this, &AssistiveOverlay::SubmitQuestion);
    connect(askButton_, &QPushButton::clicked, this, &AssistiveOverlay::SubmitQuestion);
    auto* closeShortcut = new QShortcut(QKeySequence::Cancel, this);
    closeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closeShortcut, &QShortcut::activated, this, [this]() {
        if (questionEdit_->hasFocus() && !questionEdit_->text().isEmpty()) {
            questionEdit_->clear();
        } else if (renderTarget_) {
            renderTarget_->window()->activateWindow();
            renderTarget_->setFocus(Qt::ShortcutFocusReason);
        }
    });

    headerWidget_->installEventFilter(this);
    titleLabel_->installEventFilter(this);
    headerWidget_->setMouseTracking(true);
    titleLabel_->setMouseTracking(true);

    dockPreview_ = new DockPreviewLabel(this, Qt::Tool | Qt::FramelessWindowHint |
                                   Qt::NoDropShadowWindowHint |
                                   Qt::WindowDoesNotAcceptFocus |
                                   Qt::WindowTransparentForInput);
    dockPreview_->setObjectName(QStringLiteral("assistiveDockPreview"));
    dockPreview_->setAttribute(Qt::WA_TranslucentBackground);
    dockPreview_->setAttribute(Qt::WA_ShowWithoutActivating);
    dockPreview_->setAttribute(Qt::WA_TransparentForMouseEvents);
    dockPreview_->setAlignment(Qt::AlignCenter);
    dockPreview_->setWordWrap(true);
    dockPreview_->setStyleSheet(QStringLiteral(
        "QLabel#assistiveDockPreview { color: #ffffff; font-size: 18pt; "
        "font-weight: bold; }"));
    dockPreview_->hide();

    setVisible(false);
    for (QWidget* widget = parent; widget; widget = widget->parentWidget()) {
        widget->installEventFilter(this);
    }
}

void AssistiveOverlay::SetBusy(bool busy)
{
    busy_ = busy;
    questionEdit_->setEnabled(true);
    askButton_->setEnabled(!busy_ && !questionEdit_->text().trimmed().isEmpty());
}

void AssistiveOverlay::RestoreRelativeGeometry(const QRect& geometry)
{
    restoredRelativeGeometry_ = geometry;
    usingDefaultPlacement_ = !geometry.isValid();
    placementInitialized_ = false;
    if (isVisible()) {
        UpdatePlacement();
    }
}

QRect AssistiveOverlay::RelativeGeometry() const
{
    if (!isFloating() || !placementInitialized_) {
        return restoredRelativeGeometry_;
    }
    if (!renderTarget_ || !geometry().isValid()) {
        return {};
    }
    QRect relative = geometry();
    relative.translate(-renderTarget_->mapToGlobal(QPoint(0, 0)));
    return relative;
}

void AssistiveOverlay::SetDockPosition(const QString& position)
{
    ++dragSerial_;
    CancelDockedDrag();
    FinishDrag(false);
    if (!dockHost_ || position == DockPosition()) {
        return;
    }
    const bool floating = position != QStringLiteral("left") &&
                          position != QStringLiteral("right");
    if (isFloating()) {
        restoredRelativeGeometry_ = RelativeGeometry();
    }
    changingDockPosition_ = true;
    if (floating) {
        setFloating(true);
    } else {
        dockHost_->addDockWidget(position == QStringLiteral("left")
                                     ? Qt::LeftDockWidgetArea
                                     : Qt::RightDockWidgetArea, this);
        setFloating(false);
        dockHost_->resizeDocks({this}, {std::clamp(dockHost_->width() / 3, 360, 600)},
                               Qt::Horizontal);
    }
    changingDockPosition_ = false;
    placementInitialized_ = false;
    UpdatePlacement();
    UpdateDockPositionControl();
    if (uiSuppressed_) {
        hide();
    }
}

void AssistiveOverlay::SetSafeArea(const QRect& relativeSafeArea)
{
    if (safeArea_ == relativeSafeArea) {
        return;
    }
    safeArea_ = relativeSafeArea;
    // A restored or user-moved panel must not be repositioned by chrome.
    if (usingDefaultPlacement_ && !restoredRelativeGeometry_.isValid()) {
        placementInitialized_ = false;
    }
    if (!placementInitialized_ && isVisible()) {
        UpdatePlacement();
    }
}

QString AssistiveOverlay::DockPosition() const
{
    if (isFloating() || !dockHost_) {
        return QStringLiteral("floating");
    }
    return dockHost_->dockWidgetArea(const_cast<AssistiveOverlay*>(this)) ==
                   Qt::RightDockWidgetArea
               ? QStringLiteral("right") : QStringLiteral("left");
}

void AssistiveOverlay::UpdateDockPositionControl()
{
    for (QAction* action : dockPositionActions_) {
        if (!action) {
            continue;
        }
        const QSignalBlocker blocker(action);
        action->setText(TranslateUi(action->property("_okuflowPlacementSource").toString()));
        action->setChecked(action->data().toString() == DockPosition());
    }
}

void AssistiveOverlay::SetContent(const QString& title, const QString& body, bool visible)
{
    if (title_ != title) {
        title_ = title;
        SetLiveText(titleLabel_, title_, LivePoliteness::kSilent,
                    QStringLiteral("Assistive result"));
    }
    if (body_ != body) {
        const int formattingStart = !body_.isEmpty() && body.startsWith(body_)
            ? static_cast<int>(body_.size()) : 0;
        if (!body_.isEmpty() && body.startsWith(body_)) {
            QTextCursor cursor = bodyView_->textCursor();
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(body.mid(body_.size()));
            bodyView_->setTextCursor(cursor);
        } else {
            bodyView_->setPlainText(body);
        }
        body_ = body;
        QTextCursor formatCursor(bodyView_->document());
        formatCursor.setPosition(formattingStart);
        formatCursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        QTextBlockFormat format;
        format.setLineHeight(135, QTextBlockFormat::ProportionalHeight);
        formatCursor.mergeBlockFormat(format);
        bodyView_->ensureCursorVisible();
    }
    desiredVisible_ = visible && (!title_.isEmpty() || !body_.isEmpty());
    const bool shouldShow = desiredVisible_ && !uiSuppressed_;
    const bool wasVisible = isVisible();
    if (shouldShow != wasVisible) {
        setVisible(shouldShow);
    }
    if (shouldShow && !wasVisible) {
        raise();
    }
}

void AssistiveOverlay::SetUiSuppressed(bool suppressed)
{
    if (uiSuppressed_ == suppressed) {
        return;
    }
    uiSuppressed_ = suppressed;
    if (suppressed) {
        if (!isFloating() && isVisible()) {
            suppressedDockWidth_ = width();
        }
        // Hiding a dock releases its reserved camera area; its docking mode
        // and the floating placement remain intact for the next show.
        hide();
        return;
    }
    if (!desiredVisible_) {
        return;
    }
    show();
    if (!isFloating() && dockHost_ && suppressedDockWidth_ > 0) {
        dockHost_->resizeDocks({this}, {suppressedDockWidth_}, Qt::Horizontal);
    }
    raise();
}

bool AssistiveOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == questionEdit_ &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier &&
            !QApplication::activePopupWidget()) {
            if (event->type() == QEvent::KeyPress) {
                if (dragging_ || dockHeaderPressed_) {
                    CancelDockedDrag();
                    FinishDrag(false);
                } else if (!questionEdit_->text().isEmpty()) {
                    questionEdit_->clear();
                } else if (renderTarget_) {
                    renderTarget_->window()->activateWindow();
                    renderTarget_->setFocus(Qt::ShortcutFocusReason);
                }
            }
            event->accept();
            return true;
        }
    }
    auto* geometryWidget = qobject_cast<QWidget*>(watched);
    const bool parentGeometryChanged = renderTarget_ && geometryWidget &&
                                       (geometryWidget == renderTarget_ ||
                                        geometryWidget->isAncestorOf(renderTarget_)) &&
                                       (event->type() == QEvent::Move ||
                                        event->type() == QEvent::Resize ||
                                        event->type() == QEvent::WindowStateChange);
    if (parentGeometryChanged) {
        UpdatePlacement();
    } else if ((watched == headerWidget_ || watched == titleLabel_) &&
               event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            if (isFloating()) {
                BeginDrag(mouse->globalPosition().toPoint());
            } else {
                BeginDockedDrag(mouse->globalPosition().toPoint());
            }
            return true;
        }
    } else if ((watched == headerWidget_ || watched == titleLabel_) &&
               event->type() == QEvent::MouseButtonDblClick) {
        return true;
    } else if ((watched == headerWidget_ || watched == titleLabel_) && dockHeaderPressed_) {
        if (event->type() == QEvent::MouseMove) {
            ContinueDockedDrag(static_cast<QMouseEvent*>(event)->globalPosition().toPoint());
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            CancelDockedDrag();
            return true;
        }
    } else if ((watched == headerWidget_ || watched == titleLabel_) &&
               event->type() == QEvent::MouseMove && dragging_) {
        if (!nativeDragging_) {
            ContinueDrag(static_cast<QMouseEvent*>(event)->globalPosition().toPoint());
        }
        return true;
    } else if ((watched == headerWidget_ || watched == titleLabel_) &&
               event->type() == QEvent::MouseButtonRelease && dragging_ && !nativeDragging_) {
        FinishDrag(true);
        return true;
    }
    return QDockWidget::eventFilter(watched, event);
}

bool AssistiveOverlay::event(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        UpdateDockPositionControl();
    }
    if ((dragging_ || dockHeaderPressed_) &&
        (event->type() == QEvent::ShortcutOverride ||
                      event->type() == QEvent::KeyPress) &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape &&
        static_cast<QKeyEvent*>(event)->modifiers() == Qt::NoModifier) {
        if (event->type() == QEvent::KeyPress) {
            CancelDockedDrag();
            FinishDrag(false);
        }
        event->accept();
        return true;
    }
    if (dockHeaderPressed_ &&
        (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::UngrabMouse)) {
        CancelDockedDrag();
    }
    return QDockWidget::event(event);
}

bool AssistiveOverlay::nativeEvent(const QByteArray& eventType, void* message,
                                   qintptr* result)
{
    const auto* nativeMessage = static_cast<const MSG*>(message);
    if (nativeDragging_ && nativeMessage) {
        if (nativeMessage->message == WM_EXITSIZEMOVE) {
            FinishDrag((GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0);
        } else if (nativeMessage->message == WM_CANCELMODE) {
            FinishDrag(false);
        } else if (nativeMessage->message == WM_KEYDOWN &&
                   nativeMessage->wParam == VK_ESCAPE) {
            FinishDrag(false);
            *result = 0;
            return true;
        }
    }
    return QDockWidget::nativeEvent(eventType, message, result);
}

void AssistiveOverlay::moveEvent(QMoveEvent* event)
{
    QDockWidget::moveEvent(event);
    UpdateDockPreview();
}

void AssistiveOverlay::hideEvent(QHideEvent* event)
{
    if (!isFloating() && width() > 0) {
        suppressedDockWidth_ = width();
    }
    ++dragSerial_;
    CancelDockedDrag();
    FinishDrag(false);
    QDockWidget::hideEvent(event);
}

void AssistiveOverlay::showEvent(QShowEvent* event)
{
    QDockWidget::showEvent(event);
    UpdatePlacement();
}

void AssistiveOverlay::UpdatePlacement()
{
    if (!renderTarget_ || !isFloating() || changingDockPosition_ || dragging_ ||
        uiSuppressed_) {
        return;
    }
    const int parentWidth = renderTarget_->width();
    const int parentHeight = renderTarget_->height();
    const QPoint parentOrigin = renderTarget_->mapToGlobal(QPoint(0, 0));
    if (!placementInitialized_) {
        QRect requested;
        if (restoredRelativeGeometry_.isValid()) {
            requested = restoredRelativeGeometry_;
            requested.translate(parentOrigin);
        } else {
            const int sideMargin = std::min(20, std::max(0, parentWidth / 30));
            const int topClearance = std::clamp(parentHeight / 9, 96, 132);
            const QRect viewport(QPoint(), renderTarget_->size());
            const QRect fallback(sideMargin, topClearance,
                std::max(0, parentWidth - sideMargin * 2),
                std::max(0, parentHeight - topClearance - 96));
            const QRect safe = safeArea_.isValid() ? safeArea_.intersected(viewport) : fallback;
            // Qt enforces the content layout minimum after setGeometry.
            // Include it before anchoring, especially on the RTL edge.
            const QSize layoutMinimum = minimumSizeHint();
            const int minimumDefaultWidth = std::max(360, layoutMinimum.width());
            const int minimumDefaultHeight = std::max(260, layoutMinimum.height());
            const int overlayWidth = std::min(std::max(minimumDefaultWidth, parentWidth * 43 / 100),
                                               std::max(minimumDefaultWidth, safe.width()));
            const int overlayHeight = std::min(std::max(minimumDefaultHeight, parentHeight * 3 / 4),
                                                std::max(minimumDefaultHeight, safe.height()));
            const int x = layoutDirection() == Qt::RightToLeft
                ? safe.right() - overlayWidth + 1 : safe.left();
            requested = QRect(parentOrigin + QPoint(x, safe.top()),
                              QSize(overlayWidth, overlayHeight));
        }
        setGeometry(ConstrainedGeometry(requested));
        parentOrigin_ = parentOrigin;
        placementInitialized_ = true;
        return;
    }
    QRect requested = geometry();
    requested.translate(parentOrigin - parentOrigin_);
    parentOrigin_ = parentOrigin;
    const QRect constrained = ConstrainedGeometry(requested);
    if (constrained != geometry()) {
        setGeometry(constrained);
    }
}

void AssistiveOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && ResizeEdgesAt(event->position().toPoint()) != Qt::Edges{}) {
        BeginResize(event->position().toPoint(), event->globalPosition().toPoint());
        event->accept();
        return;
    }
    QDockWidget::mousePressEvent(event);
}

void AssistiveOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (dockHeaderPressed_) {
        ContinueDockedDrag(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    if (dragging_) {
        if (!nativeDragging_) {
            ContinueDrag(event->globalPosition().toPoint());
        }
        event->accept();
        return;
    }
    if (resizing_) {
        ContinueResize(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    UpdateResizeCursor(event->position().toPoint());
    QDockWidget::mouseMoveEvent(event);
}

void AssistiveOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && dockHeaderPressed_) {
        CancelDockedDrag();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && dragging_ && !nativeDragging_) {
        FinishDrag(true);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && resizing_) {
        resizing_ = false;
        resizeEdges_ = {};
        releaseMouse();
        UpdateResizeCursor(event->position().toPoint());
        event->accept();
        return;
    }
    QDockWidget::mouseReleaseEvent(event);
}

void AssistiveOverlay::leaveEvent(QEvent* event)
{
    if (!resizing_) {
        unsetCursor();
    }
    QDockWidget::leaveEvent(event);
}

void AssistiveOverlay::BeginDrag(const QPoint& globalPosition)
{
    usingDefaultPlacement_ = false;
    CancelDockedDrag();
    resizing_ = false;
    dragging_ = true;
    nativeDragging_ = true;
    dragMoved_ = false;
    ++dragSerial_;
    dockCandidate_.clear();
    pendingDockCandidate_.clear();
    dockCandidateSince_.invalidate();
    dockDebounceTimer_->stop();
    blockedDockSide_.clear();
    pointerStartGlobal_ = globalPosition;
    pointerStartGeometry_ = geometry();
    raise();
    if (QWindow* nativeWindow = windowHandle(); nativeWindow && nativeWindow->startSystemMove()) {
        return;
    }
    nativeDragging_ = false;
    grabMouse();
}

void AssistiveOverlay::BeginDockedDrag(const QPoint& globalPosition)
{
    CancelDockedDrag();
    dockHeaderPressed_ = true;
    dockPressGlobal_ = globalPosition;
    dockPointerGlobal_ = globalPosition;
    dockGrabOffset_ = globalPosition - mapToGlobal(QPoint());
    dockStartWidth_ = width();
    grabMouse();
}

void AssistiveOverlay::ContinueDockedDrag(const QPoint& globalPosition)
{
    dockPointerGlobal_ = globalPosition;
    // A click or a small hand tremor must not detach the panel. The hold starts
    // after a deliberate pull, and returning to the start resets it.
    if ((globalPosition - dockPressGlobal_).manhattanLength() < 32) {
        undockTimer_->stop();
    } else if (!undockTimer_->isActive()) {
        undockTimer_->start();
    }
}

void AssistiveOverlay::CancelDockedDrag()
{
    dockHeaderPressed_ = false;
    if (undockTimer_) {
        undockTimer_->stop();
    }
    if (!dragging_ && mouseGrabber() == this) {
        releaseMouse();
    }
}

void AssistiveOverlay::ReleaseDockLatch()
{
    if (!dockHeaderPressed_ || isFloating() || !isVisible()) {
        CancelDockedDrag();
        return;
    }
    const QPoint pointer = dockPointerGlobal_;
    const QPoint offset = dockGrabOffset_;
    const int originalWidth = std::max(1, dockStartWidth_);
    const QString side = DockPosition();
    CancelDockedDrag();
    SetDockPosition(QStringLiteral("floating"));
    const QPoint floatingOffset(
        std::clamp(offset.x() * width() / originalWidth, 0, width() - 1),
        std::clamp(offset.y(), 0, std::max(0, headerWidget_->height() - 1)));
    setGeometry(ConstrainedGeometry(QRect(pointer - floatingOffset, size())));
    BeginDrag(pointer);
    // Do not immediately snap back to the side just released. The user must
    // first pull clear of its wider release zone before that side can re-arm.
    blockedDockSide_ = side;
}

void AssistiveOverlay::ContinueDrag(const QPoint& globalPosition)
{
    if (!dragging_) {
        return;
    }
    QRect requested = pointerStartGeometry_;
    requested.moveTopLeft(pointerStartGeometry_.topLeft() + globalPosition - pointerStartGlobal_);
    setGeometry(ConstrainedGeometry(requested));
    UpdateDockPreview();
}

void AssistiveOverlay::UpdateDockPreview()
{
    if (!dragging_ || !dockHost_ || !dockPreview_ || !isFloating()) {
        return;
    }
    if (!dragMoved_) {
        dragMoved_ = (geometry().topLeft() - pointerStartGeometry_.topLeft()).manhattanLength() >=
                     QApplication::startDragDistance();
        if (!dragMoved_) {
            return;
        }
    }
    // QWidget geometry is already in the correct per-monitor logical pixels,
    // including moves delivered during Windows' native modal move loop.
    QRect bounds(dockHost_->mapToGlobal(dockHost_->contentsRect().topLeft()),
                 dockHost_->contentsRect().size());
    if (QWidget* central = dockHost_->centralWidget()) {
        bounds.setTop(central->mapToGlobal(QPoint()).y());
        bounds.setBottom(central->mapToGlobal(QPoint(0, central->height() - 1)).y());
    }
    constexpr int kDockSnapDistance = 36;
    constexpr int kDockReleaseDistance = 96;
    const int leftDistance = std::abs(geometry().left() - bounds.left());
    const int rightDistance = std::abs(geometry().right() - bounds.right());
    const int grabX = geometry().left() + pointerStartGlobal_.x() - pointerStartGeometry_.left();
    // Keep the target lit as the window crosses the edge, until the grabbed
    // title-bar point also leaves the host. Both edge-first and cursor-first
    // drags therefore dock, without a dead gap between them.
    const bool nearLeft = geometry().left() <= bounds.left() + kDockSnapDistance &&
                          grabX >= bounds.left() - kDockSnapDistance;
    const bool nearRight = geometry().right() >= bounds.right() - kDockSnapDistance &&
                           grabX <= bounds.right() + kDockSnapDistance;
    const int headerY = headerWidget_->mapToGlobal(headerWidget_->rect().center()).y();
    const bool insideHeight = headerY >= bounds.top() && headerY <= bounds.bottom();
    const bool retainLeft = insideHeight && geometry().left() <= bounds.left() + kDockReleaseDistance &&
                            grabX >= bounds.left() - kDockReleaseDistance;
    const bool retainRight = insideHeight && geometry().right() >= bounds.right() - kDockReleaseDistance &&
                             grabX <= bounds.right() + kDockReleaseDistance;
    if ((blockedDockSide_ == QStringLiteral("left") && !retainLeft) ||
        (blockedDockSide_ == QStringLiteral("right") && !retainRight)) {
        blockedDockSide_.clear();
    }
    const bool retainCandidate =
        (dockCandidate_ == QStringLiteral("left") && retainLeft) ||
        (dockCandidate_ == QStringLiteral("right") && retainRight);
    QString desiredCandidate = retainCandidate ? dockCandidate_ : QString();
    if (desiredCandidate.isEmpty() && insideHeight) {
        if (nearLeft && (!nearRight || leftDistance <= rightDistance)) {
            desiredCandidate = QStringLiteral("left");
        } else if (nearRight) {
            desiredCandidate = QStringLiteral("right");
        }
    }
    if (desiredCandidate == blockedDockSide_) {
        desiredCandidate.clear();
    }
    // Debounce changes to the latched target, rather than restarting a timer
    // for every move. A brief edge crossing cannot arm a drop; once armed,
    // small excursions cannot flicker the highlight or repeatedly re-arm it.
    if (desiredCandidate == dockCandidate_) {
        dockDebounceTimer_->stop();
        dockCandidateSince_.invalidate();
        pendingDockCandidate_.clear();
    } else {
        const int delay = dockCandidate_.isEmpty() ? 180 : 250;
        if (!dockCandidateSince_.isValid() || pendingDockCandidate_ != desiredCandidate) {
            pendingDockCandidate_ = desiredCandidate;
            dockCandidateSince_.start();
            dockDebounceTimer_->start(delay);
        } else if (dockCandidateSince_.elapsed() >= delay) {
            dockCandidate_ = desiredCandidate;
            pendingDockCandidate_.clear();
            dockCandidateSince_.invalidate();
            dockDebounceTimer_->stop();
        } else if (!dockDebounceTimer_->isActive()) {
            dockDebounceTimer_->start(std::max(1, delay - static_cast<int>(dockCandidateSince_.elapsed())));
        }
    }
    if (dockCandidate_.isEmpty()) {
        dockPreview_->hide();
        return;
    }
    const bool left = dockCandidate_ == QStringLiteral("left");
    const int previewWidth = std::min(bounds.width(), std::clamp(dockHost_->width() / 3, 360, 600));
    const QRect previewBounds(left ? bounds.left() : bounds.right() - previewWidth + 1,
                              bounds.top(), previewWidth, bounds.height());
    SetLiveText(dockPreview_, left ? QStringLiteral("Release to dock left")
                                  : QStringLiteral("Release to dock right"),
                LivePoliteness::kSilent, QStringLiteral("Panel position"));
    dockPreview_->setGeometry(previewBounds);
    dockPreview_->show();
    dockPreview_->raise();
}

void AssistiveOverlay::FinishDrag(bool commit)
{
    if (!dragging_) {
        return;
    }
    const QString target = commit ? dockCandidate_ : QString();
    dragging_ = false;
    nativeDragging_ = false;
    dockCandidate_.clear();
    pendingDockCandidate_.clear();
    dockCandidateSince_.invalidate();
    dockDebounceTimer_->stop();
    if (dockPreview_) {
        dockPreview_->hide();
    }
    if (mouseGrabber() == this) {
        releaseMouse();
    }
    if (!commit) {
        setGeometry(pointerStartGeometry_);
    } else if (!target.isEmpty()) {
        const quint64 serial = dragSerial_;
        // Changing the native window while handling WM_EXITSIZEMOVE is unsafe.
        // Dock after the move loop returns, unless another action supersedes it.
        QTimer::singleShot(0, this, [this, serial, target]() {
            if (serial == dragSerial_ && !dragging_ && isFloating() && isVisible()) {
                SetDockPosition(target);
            }
        });
    }
}

void AssistiveOverlay::BeginResize(const QPoint& localPosition, const QPoint& globalPosition)
{
    usingDefaultPlacement_ = false;
    ++dragSerial_;
    FinishDrag(false);
    resizeEdges_ = ResizeEdgesAt(localPosition);
    if (resizeEdges_ == Qt::Edges{}) {
        return;
    }
    dragging_ = false;
    resizing_ = false;
    raise();
    if (QWindow* nativeWindow = windowHandle();
        nativeWindow && nativeWindow->startSystemResize(resizeEdges_)) {
        return;
    }
    resizing_ = true;
    pointerStartGlobal_ = globalPosition;
    pointerStartGeometry_ = geometry();
    grabMouse();
}

void AssistiveOverlay::ContinueResize(const QPoint& globalPosition)
{
    if (!resizing_) {
        return;
    }
    const QPoint delta = globalPosition - pointerStartGlobal_;
    QRect requested = pointerStartGeometry_;
    if (resizeEdges_.testFlag(Qt::LeftEdge)) requested.setLeft(requested.left() + delta.x());
    if (resizeEdges_.testFlag(Qt::RightEdge)) requested.setRight(requested.right() + delta.x());
    if (resizeEdges_.testFlag(Qt::TopEdge)) requested.setTop(requested.top() + delta.y());
    if (resizeEdges_.testFlag(Qt::BottomEdge)) requested.setBottom(requested.bottom() + delta.y());
    setGeometry(ConstrainedGeometry(requested.normalized()));
}

Qt::Edges AssistiveOverlay::ResizeEdgesAt(const QPoint& localPosition) const
{
    if (!isFloating()) {
        return {};
    }
    constexpr int kResizeMargin = 9;
    Qt::Edges edges;
    if (localPosition.x() <= kResizeMargin) edges |= Qt::LeftEdge;
    if (localPosition.x() >= width() - kResizeMargin) edges |= Qt::RightEdge;
    if (localPosition.y() <= kResizeMargin) edges |= Qt::TopEdge;
    if (localPosition.y() >= height() - kResizeMargin) edges |= Qt::BottomEdge;
    return edges;
}

void AssistiveOverlay::UpdateResizeCursor(const QPoint& localPosition)
{
    const Qt::Edges edges = ResizeEdgesAt(localPosition);
    const bool horizontal = edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
    const bool vertical = edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
    if (horizontal && vertical) {
        const bool forward = (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge)) ||
                             (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge));
        setCursor(forward ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    } else if (horizontal) {
        setCursor(Qt::SizeHorCursor);
    } else if (vertical) {
        setCursor(Qt::SizeVerCursor);
    } else {
        unsetCursor();
    }
}

QRect AssistiveOverlay::ConstrainedGeometry(const QRect& requested) const
{
    if (!renderTarget_) {
        return requested;
    }
    const QRect available(renderTarget_->mapToGlobal(QPoint(0, 0)), renderTarget_->size());
    const int minWidth = std::min(minimumWidth(), available.width());
    const int minHeight = std::min(minimumHeight(), available.height());
    const int width = std::clamp(requested.width(), minWidth, available.width());
    const int height = std::clamp(requested.height(), minHeight, available.height());
    const int x = std::clamp(requested.x(), available.left(),
                             std::max(available.left(), available.right() - width + 1));
    const int y = std::clamp(requested.y(), available.top(),
                             std::max(available.top(), available.bottom() - height + 1));
    return QRect(x, y, width, height);
}

void AssistiveOverlay::SubmitQuestion()
{
    const QString question = questionEdit_->text().trimmed();
    if (question.isEmpty() || busy_) {
        questionEdit_->setFocus();
        return;
    }
    questionEdit_->clear();
    emit QuestionSubmitted(question);
}

std::array<QWidget*, 7> AssistiveOverlay::FocusTargets() const
{
    return {bodyView_, readAloudButton_, questionEdit_, askButton_,
            newChatButton_, dockPositionButton_, closeButton_};
}


} // namespace okuflow

#endif // _WIN32
