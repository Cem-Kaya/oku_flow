#ifdef _WIN32

#include "openzoom/ui/assistive_overlay.hpp"
#include "openzoom/ui/live_status_text.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <QTextBrowser>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <array>

namespace openzoom {

namespace {
QString SpokenAssistiveText(QString body)
{
    const std::array<QString, 2> sectionLabels{
        QStringLiteral("OCR"),
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
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName(QStringLiteral("assistiveOverlay"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setMinimumSize(360, 260);
    setAccessibleName(QStringLiteral("Assistive results"));
    setStyleSheet(QStringLiteral(R"(
        QWidget#assistiveOverlay {
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
            font-size: 13pt;
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

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(8);

    headerWidget_ = new QWidget(this);
    headerWidget_->setObjectName(QStringLiteral("assistiveHeader"));
    headerWidget_->setCursor(Qt::SizeAllCursor);
    headerWidget_->setAccessibleName(QStringLiteral("Move assistive panel"));
    auto* header = new QHBoxLayout(headerWidget_);
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    titleLabel_ = new QLabel();
    titleLabel_->setObjectName(QStringLiteral("assistiveTitle"));
    titleLabel_->setAccessibleName(QStringLiteral("Assistive result title"));
    header->addWidget(titleLabel_, 1);

    closeButton_ = new QToolButton();
    closeButton_->setObjectName(QStringLiteral("assistiveCloseButton"));
    closeButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/close.svg")));
    closeButton_->setIconSize(QSize(28, 28));
    closeButton_->setToolTip(QStringLiteral("Close result"));
    closeButton_->setAccessibleName(QStringLiteral("Close assistive result"));
    closeButton_->setAccessibleDescription(
        QStringLiteral("Hide the current OCR or scene explanation result"));
    closeButton_->setFixedSize(52, 52);
    header->addWidget(closeButton_);
    layout->addWidget(headerWidget_);

    bodyView_ = new QTextBrowser();
    bodyView_->setObjectName(QStringLiteral("assistiveBody"));
    bodyView_->setReadOnly(true);
    bodyView_->setOpenExternalLinks(false);
    bodyView_->setFocusPolicy(Qt::StrongFocus);
    bodyView_->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    bodyView_->setAccessibleName(QStringLiteral("Assistive result text"));
    bodyView_->setAccessibleDescription(
        QStringLiteral("Streaming OCR and scene explanation result. Use arrow keys to read the text."));
    layout->addWidget(bodyView_, 1);

    auto* questionRow = new QHBoxLayout();
    questionRow->setSpacing(8);
    questionEdit_ = new QLineEdit();
    questionEdit_->setObjectName(QStringLiteral("assistiveQuestion"));
    questionEdit_->setPlaceholderText(QStringLiteral("Ask about this view..."));
    questionEdit_->setAccessibleName(QStringLiteral("Question about the current view"));
    questionEdit_->setAccessibleDescription(
        QStringLiteral("Type a follow-up question at any time. Sending becomes available when the current answer finishes."));
    askButton_ = new QPushButton(QStringLiteral("Ask"));
    askButton_->setObjectName(QStringLiteral("assistiveAskButton"));
    askButton_->setEnabled(false);
    askButton_->setAccessibleName(QStringLiteral("Ask Assistant"));
    askButton_->setAccessibleDescription(
        QStringLiteral("Send the question with the current camera view to OpenZoom Assistant"));
    questionRow->addWidget(questionEdit_, 1);
    questionRow->addWidget(askButton_);
    layout->addLayout(questionRow);

    auto* footer = new QHBoxLayout();
    footer->addStretch(1);
    readAloudButton_ = new QPushButton(QStringLiteral("Read Aloud"));
    readAloudButton_->setObjectName(QStringLiteral("assistiveReadButton"));
    readAloudButton_->setIcon(QIcon(QStringLiteral(":/openzoom/icons/read.svg")));
    readAloudButton_->setIconSize(QSize(24, 24));
    readAloudButton_->setAccessibleName(QStringLiteral("Read assistive result aloud"));
    readAloudButton_->setAccessibleDescription(
        QStringLiteral("Speak the current OCR or scene explanation result"));
    footer->addWidget(readAloudButton_);
    layout->addLayout(footer);

    connect(closeButton_, &QToolButton::clicked, this, [this]() {
        hide();
        emit Dismissed();
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
    connect(closeShortcut, &QShortcut::activated, closeButton_, &QToolButton::click);

    headerWidget_->installEventFilter(this);
    titleLabel_->installEventFilter(this);
    headerWidget_->setMouseTracking(true);
    titleLabel_->setMouseTracking(true);

    setVisible(false);
    if (parent) {
        parent->installEventFilter(this);
        if (parent->window() != parent) {
            parent->window()->installEventFilter(this);
        }
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
    placementInitialized_ = false;
    if (isVisible()) {
        UpdatePlacement();
    }
}

QRect AssistiveOverlay::RelativeGeometry() const
{
    if (!placementInitialized_) {
        return restoredRelativeGeometry_;
    }
    if (!parentWidget() || !geometry().isValid()) {
        return {};
    }
    QRect relative = geometry();
    relative.translate(-parentWidget()->mapToGlobal(QPoint(0, 0)));
    return relative;
}

void AssistiveOverlay::SetContent(const QString& title, const QString& body, bool visible)
{
    if (title_ != title) {
        title_ = title;
        SetLiveText(titleLabel_, title_, LivePoliteness::kSilent,
                    QStringLiteral("Assistive result"));
    }
    if (body_ != body) {
        if (!body_.isEmpty() && body.startsWith(body_)) {
            QTextCursor cursor = bodyView_->textCursor();
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(body.mid(body_.size()));
            bodyView_->setTextCursor(cursor);
        } else {
            bodyView_->setPlainText(body);
        }
        body_ = body;
        bodyView_->ensureCursorVisible();
    }
    const bool shouldShow = visible && (!title_.isEmpty() || !body_.isEmpty());
    const bool wasVisible = isVisible();
    if (shouldShow != wasVisible) {
        setVisible(shouldShow);
    }
    if (shouldShow && !wasVisible) {
        raise();
    }
}

bool AssistiveOverlay::eventFilter(QObject* watched, QEvent* event)
{
    const bool parentGeometryChanged = parentWidget() &&
                                       (watched == parentWidget() || watched == parentWidget()->window()) &&
                                       (event->type() == QEvent::Move ||
                                        event->type() == QEvent::Resize ||
                                        event->type() == QEvent::WindowStateChange);
    if (parentGeometryChanged) {
        UpdatePlacement();
    } else if ((watched == headerWidget_ || watched == titleLabel_) &&
               event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            BeginDrag(mouse->globalPosition().toPoint());
            return true;
        }
    } else if ((watched == headerWidget_ || watched == titleLabel_) &&
               event->type() == QEvent::MouseMove && dragging_) {
        ContinueDrag(static_cast<QMouseEvent*>(event)->globalPosition().toPoint());
        return true;
    } else if ((watched == headerWidget_ || watched == titleLabel_) &&
               event->type() == QEvent::MouseButtonRelease && dragging_) {
        dragging_ = false;
        releaseMouse();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void AssistiveOverlay::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    UpdatePlacement();
}

void AssistiveOverlay::UpdatePlacement()
{
    if (!parentWidget()) {
        return;
    }
    const int parentWidth = parentWidget()->width();
    const int parentHeight = parentWidget()->height();
    const QPoint parentOrigin = parentWidget()->mapToGlobal(QPoint(0, 0));
    if (!placementInitialized_) {
        QRect requested;
        if (restoredRelativeGeometry_.isValid()) {
            requested = restoredRelativeGeometry_;
            requested.translate(parentOrigin);
        } else {
            const int sideMargin = std::min(20, std::max(0, parentWidth / 30));
            const int topClearance = std::clamp(parentHeight / 9, 96, 132);
            const int overlayWidth = std::clamp(parentWidth * 3 / 5, 360, 760);
            const int overlayHeight = std::clamp(parentHeight * 2 / 5, 260, 520);
            requested = QRect(parentOrigin + QPoint(sideMargin, topClearance),
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
    QWidget::mousePressEvent(event);
}

void AssistiveOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_) {
        ContinueDrag(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    if (resizing_) {
        ContinueResize(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    UpdateResizeCursor(event->position().toPoint());
    QWidget::mouseMoveEvent(event);
}

void AssistiveOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        releaseMouse();
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
    QWidget::mouseReleaseEvent(event);
}

void AssistiveOverlay::leaveEvent(QEvent* event)
{
    if (!resizing_) {
        unsetCursor();
    }
    QWidget::leaveEvent(event);
}

void AssistiveOverlay::BeginDrag(const QPoint& globalPosition)
{
    resizing_ = false;
    dragging_ = false;
    raise();
    if (QWindow* nativeWindow = windowHandle(); nativeWindow && nativeWindow->startSystemMove()) {
        return;
    }
    dragging_ = true;
    pointerStartGlobal_ = globalPosition;
    pointerStartGeometry_ = geometry();
    grabMouse();
}

void AssistiveOverlay::ContinueDrag(const QPoint& globalPosition)
{
    if (!dragging_) {
        return;
    }
    QRect requested = pointerStartGeometry_;
    requested.moveTopLeft(pointerStartGeometry_.topLeft() + globalPosition - pointerStartGlobal_);
    setGeometry(ConstrainedGeometry(requested));
}

void AssistiveOverlay::BeginResize(const QPoint& localPosition, const QPoint& globalPosition)
{
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
    if (!parentWidget()) {
        return requested;
    }
    const QRect available(parentWidget()->mapToGlobal(QPoint(0, 0)), parentWidget()->size());
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

std::array<QWidget*, 5> AssistiveOverlay::FocusTargets() const
{
    return {bodyView_, questionEdit_, askButton_, readAloudButton_, closeButton_};
}


} // namespace openzoom

#endif // _WIN32
