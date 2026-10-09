#ifdef _WIN32

#include "okuflow/ui/joystick_overlay.hpp"

#include <QColor>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace okuflow {

JoystickOverlay::JoystickOverlay(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setVisible(false);
    if (parent) {
        parent->installEventFilter(this);
        if (QWidget* actionPanel =
                parent->window()->findChild<QWidget*>(QStringLiteral("bottomRightPanel"))) {
            actionPanel->installEventFilter(this);
        }
    }
    setFixedSize(160, 160);
    UpdateMask();
    ResetKnob();
}

void JoystickOverlay::ResetKnob()
{
    knobPos_ = QPointF(width() / 2.0, height() / 2.0);
    update();
}

bool JoystickOverlay::eventFilter(QObject* watched, QEvent* event)
{
    const bool parentChanged =
        watched == parentWidget() && event->type() == QEvent::Resize;
    const bool actionPanelChanged =
        watched->objectName() == QStringLiteral("bottomRightPanel") &&
        (event->type() == QEvent::Move ||
         event->type() == QEvent::Resize ||
         event->type() == QEvent::Show ||
         event->type() == QEvent::Hide);
    if (parentChanged || actionPanelChanged) {
        QTimer::singleShot(0, this, &JoystickOverlay::UpdatePlacement);
    }
    return QWidget::eventFilter(watched, event);
}

void JoystickOverlay::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    UpdatePlacement();
}

void JoystickOverlay::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    painter.setBrush(QColor(60, 60, 60, 180));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(rect());

    painter.setBrush(QColor(230, 230, 230, 230));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(KnobRect());
}

void JoystickOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        UpdateFromPosition(event->position());
    }
}

void JoystickOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_) {
        UpdateFromPosition(event->position());
    }
}

void JoystickOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (dragging_ && event->button() == Qt::LeftButton) {
        dragging_ = false;
        ResetKnob();
        emit JoystickChanged(0.0f, 0.0f);
        update();
    }
}

void JoystickOverlay::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    UpdateMask();
}

QRectF JoystickOverlay::KnobRect() const
{
    constexpr qreal knobRadius = 24.0;
    return QRectF(knobPos_.x() - knobRadius,
                  knobPos_.y() - knobRadius,
                  knobRadius * 2.0,
                  knobRadius * 2.0);
}

void JoystickOverlay::UpdatePlacement()
{
    if (!parentWidget()) {
        return;
    }
    const int margin = 20;
    const int x = parentWidget()->width() - width() - margin;
    int y = parentWidget()->height() - height() - margin;
    QWidget* actionPanel =
        parentWidget()->window()->findChild<QWidget*>(QStringLiteral("bottomRightPanel"));
    if (actionPanel && actionPanel->isVisible()) {
        const QPoint actionTopLeft =
            parentWidget()->mapFromGlobal(actionPanel->mapToGlobal(QPoint(0, 0)));
        const QRect actionRect(actionTopLeft, actionPanel->size());
        const QRect joystickRect(x, y, width(), height());
        if (joystickRect.adjusted(-margin, -margin, margin, margin)
                .intersects(actionRect)) {
            y = actionRect.top() - height() - margin;
        }
    }
    move(std::max(0, x), std::max(0, y));
}

void JoystickOverlay::UpdateFromPosition(const QPointF& pos)
{
    const QPointF center(width() / 2.0, height() / 2.0);
    QPointF delta = pos - center;
    const qreal maxRadius = width() / 2.0;
    if (delta.manhattanLength() < 0.0001) {
        delta = QPointF(0, 0);
    }
    const qreal distance = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
    if (distance > maxRadius) {
        delta *= maxRadius / distance;
    }
    knobPos_ = center + delta;
    update();

    float normX = static_cast<float>(delta.x() / maxRadius);
    float normY = static_cast<float>(delta.y() / maxRadius);
    normX = std::clamp(normX, -1.0f, 1.0f);
    normY = std::clamp(normY, -1.0f, 1.0f);

    emit JoystickChanged(normX, -normY);
}

void JoystickOverlay::UpdateMask()
{
    if (width() <= 0 || height() <= 0) {
        clearMask();
        return;
    }
    QRegion region(rect(), QRegion::Ellipse);
    setMask(region);
}


} // namespace okuflow

#endif // _WIN32
