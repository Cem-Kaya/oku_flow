#ifdef _WIN32

#include "openzoom/app/interaction_controller.hpp"

#include "openzoom/app/app.hpp"
#include "openzoom/app/constants.hpp"
#include "openzoom/ui/main_window.hpp"

#include <QCursor>
#include <QAccessible>
#include <QTimer>
#include <QtGlobal>
#include <QWheelEvent>
#include <QCheckBox>
#include <QLabel>
#include <QSlider>

#include <algorithm>
#include <cmath>

namespace openzoom {

InteractionController::InteractionController(OpenZoomApp& app)
    : app_(app) {}

bool InteractionController::HandlePanKey(int key, bool pressed)
{
    bool* keyState = nullptr;
    switch (key) {
    case Qt::Key_Left:
        keyState = &panLeftPressed_;
        break;
    case Qt::Key_Right:
        keyState = &panRightPressed_;
        break;
    case Qt::Key_Up:
        keyState = &panUpPressed_;
        break;
    case Qt::Key_Down:
        keyState = &panDownPressed_;
        break;
    default:
        return false;
    }
    const bool changed = *keyState != pressed;
    *keyState = pressed;
    if (changed && !pressed && !HasContinuousMotion()) {
        app_.SetZoomCenter(
            app_.zoomCenterX_, app_.zoomCenterY_, true, true, true);
    }
    return true;
}

bool InteractionController::HandlePanScroll(const QWheelEvent* wheelEvent)
{
    if (!wheelEvent || !app_.uiState_->renderWidget_) {
        return false;
    }

    if (app_.debugViewEnabled_) {
        return false;
    }

    if (!app_.zoomEnabled_ || app_.zoomAmount_ <= 1.0f) {
        return false;
    }

    QPointF pixelDelta = wheelEvent->pixelDelta();
    float deltaX = 0.0f;
    float deltaY = 0.0f;
    bool hasPixelPrecision = false;

    if (!pixelDelta.isNull()) {
        deltaX = pixelDelta.x();
        deltaY = pixelDelta.y();
        hasPixelPrecision = true;
    } else {
        QPoint angleDelta = wheelEvent->angleDelta();
        if (angleDelta.isNull()) {
            return false;
        }
        deltaX = static_cast<float>(angleDelta.x()) / 120.0f;
        deltaY = static_cast<float>(angleDelta.y()) / 120.0f;
    }

    const float zoomFactor = std::max(app_.zoomAmount_, 1.0f);
    float moveX = 0.0f;
    float moveY = 0.0f;

    if (hasPixelPrecision) {
        const float widgetWidth = static_cast<float>(std::max(1, app_.uiState_->renderWidget_->width()));
        const float widgetHeight = static_cast<float>(std::max(1, app_.uiState_->renderWidget_->height()));
        moveX = -deltaX / widgetWidth / zoomFactor;
        moveY = -deltaY / widgetHeight / zoomFactor;
    } else {
        constexpr float wheelStepScale = 1.2f;
        moveX = -deltaX * app_constants::kPanKeyboardStep * wheelStepScale / zoomFactor;
        moveY = -deltaY * app_constants::kPanKeyboardStep * wheelStepScale / zoomFactor;
    }

    if (std::abs(moveX) < 1e-6f && std::abs(moveY) < 1e-6f) {
        return false;
    }

    app_.SetZoomCenter(app_.zoomCenterX_ + moveX,
                       app_.zoomCenterY_ + moveY,
                       true,
                       true);
    return true;
}

float InteractionController::ScaledZoom(float current,
                                        float notches,
                                        float accel) const {
    constexpr float kZoomStepRatio = 1.10f;
    return current * std::pow(kZoomStepRatio, notches * accel);
}

void InteractionController::ApplyZoom(float target, const QPointF* localPos) {
    if (!app_.uiState_->zoomSlider_) {
        return;
    }

    float focusU = 0.0f;
    float focusV = 0.0f;
    bool hasFocus =
        localPos && app_.MapViewToSource(*localPos, focusU, focusV);
    if (app_.debugViewEnabled_) {
        hasFocus = false;
    }

    if (!app_.zoomEnabled_) {
        if (app_.uiState_->zoomCheckbox_) {
            auto blocker = app_.uiState_->BlockSignals(app_.uiState_->zoomCheckbox_);
            app_.uiState_->zoomCheckbox_->setChecked(true);
        }
        app_.zoomEnabled_ = true;
        app_.uiState_->zoomSlider_->setEnabled(true);
    }

    const float prevZoom = app_.zoomAmount_;
    const float minimumZoom =
        static_cast<float>(app_.uiState_->zoomSlider_->minimum()) /
        static_cast<float>(app_constants::kZoomSliderScale);
    const float maximumZoom =
        static_cast<float>(app_.uiState_->zoomSlider_->maximum()) /
        static_cast<float>(app_constants::kZoomSliderScale);
    target = std::clamp(target, minimumZoom, maximumZoom);
    const int newValue = std::clamp(
        static_cast<int>(std::lround(
            target * static_cast<float>(app_constants::kZoomSliderScale))),
        app_.uiState_->zoomSlider_->minimum(),
        app_.uiState_->zoomSlider_->maximum());

    if (newValue == app_.uiState_->zoomSlider_->value()) {
        return;
    }

    auto blockSlider = app_.uiState_->BlockSignals(app_.uiState_->zoomSlider_);
    app_.uiState_->zoomSlider_->setValue(newValue);
    blockSlider.unblock();
    app_.zoomAmount_ = std::max(
        1.0f,
        static_cast<float>(newValue) / static_cast<float>(app_constants::kZoomSliderScale));
    app_.UpdateProcessingStatusLabel();
    const float newZoom = app_.zoomAmount_;

    if (hasFocus) {
        const float factor = (prevZoom <= 0.0f || newZoom <= 0.0f) ? 1.0f : (prevZoom / newZoom);
        const float newCenterX = focusU - (focusU - app_.zoomCenterX_) * factor;
        const float newCenterY = focusV - (focusV - app_.zoomCenterY_) * factor;
        app_.SetZoomCenter(newCenterX, newCenterY, true, true);
    } else {
        app_.SyncCurrentConfigToPersistence(true);
    }
    ScheduleZoomAnnouncement();
}

void InteractionController::HandleZoomWheel(const QWheelEvent* wheelEvent)
{
    if (!wheelEvent) {
        return;
    }
    float notches = 0.0f;
    if (!wheelEvent->pixelDelta().isNull()) {
        notches = static_cast<float>(wheelEvent->pixelDelta().y()) / 50.0f;
    } else if (!wheelEvent->angleDelta().isNull()) {
        notches = static_cast<float>(wheelEvent->angleDelta().y()) / 120.0f;
    }
    if (qFuzzyIsNull(notches)) {
        return;
    }

    constexpr qint64 kAccelWindowMs = 120;
    constexpr float kAccelGrowth = 1.35f;
    constexpr float kAccelMax = 3.0f;
    const int direction = notches > 0.0f ? 1 : -1;
    qint64 gap = kAccelWindowMs + 1;
    if (wheelTimer_.isValid()) {
        gap = wheelTimer_.restart();
    } else {
        wheelTimer_.start();
    }
    if (direction != wheelDirection_) {
        wheelAccel_ = 1.0f;
    } else if (app_.settingsController_->MutableSettings()
                   .zoomWheelAcceleration &&
               gap < kAccelWindowMs) {
        wheelAccel_ = std::min(wheelAccel_ * kAccelGrowth, kAccelMax);
    } else {
        wheelAccel_ = 1.0f;
    }
    wheelDirection_ = direction;

    const float target = ScaledZoom(app_.zoomAmount_, notches, wheelAccel_);
    const QPointF localPos = wheelEvent->position();
    ApplyZoom(target, &localPos);
}

void InteractionController::HandleKeyboardZoom(float notches) {
    if (qFuzzyIsNull(notches)) {
        return;
    }
    wheelAccel_ = 1.0f;
    wheelDirection_ = 0;
    ApplyZoom(ScaledZoom(app_.zoomAmount_, notches, 1.0f), nullptr);
}

void InteractionController::ScheduleZoomAnnouncement() {
    const quint64 generation = ++zoomAnnouncementGeneration_;
    QTimer::singleShot(250, &app_, [this, generation]() {
        if (generation != zoomAnnouncementGeneration_) {
            return;
        }
        const QString message =
            QStringLiteral("Zoom %1 times")
                .arg(QString::number(app_.zoomAmount_, 'f', 1));
        app_.ShowStatusMessage(message, 2500);
    });
}

bool InteractionController::ApplyInputForces(double elapsedSeconds)
{
    float moveX = 0.0f;
    if (panLeftPressed_) {
        moveX -= 1.0f;
    }
    if (panRightPressed_) {
        moveX += 1.0f;
    }
    moveX += joystickPanX_;

    float moveY = 0.0f;
    if (panUpPressed_) {
        moveY -= 1.0f;
    }
    if (panDownPressed_) {
        moveY += 1.0f;
    }
    moveY += -joystickPanY_;

    if (std::abs(moveX) < 1e-5f && std::abs(moveY) < 1e-5f) {
        return false;
    }

    const float length = std::sqrt(moveX * moveX + moveY * moveY);
    float normalizedX = moveX;
    float normalizedY = moveY;
    if (length > 1e-5f) {
        normalizedX /= length;
        normalizedY /= length;
    }

    const bool keyboardActive = panLeftPressed_ || panRightPressed_ || panUpPressed_ || panDownPressed_;
    const float analogStrength = std::sqrt(joystickPanX_ * joystickPanX_ + joystickPanY_ * joystickPanY_);

    float unitsPerSecond = 0.0f;
    if (keyboardActive) {
        unitsPerSecond = app_constants::kPanKeyboardUnitsPerSecond;
    }
    if (analogStrength > 0.001f) {
        unitsPerSecond = std::max(
            unitsPerSecond,
            app_constants::kPanJoystickUnitsPerSecond *
                std::clamp(analogStrength, 0.1f, 1.0f));
    }
    if (unitsPerSecond <= 0.0f) {
        unitsPerSecond = app_constants::kPanJoystickUnitsPerSecond;
    }

    const float zoomFactor = std::max(1.0f, app_.zoomAmount_);
    const float step =
        unitsPerSecond *
        static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.05)) /
        zoomFactor;

    app_.SetZoomCenter(app_.zoomCenterX_ + normalizedX * step,
                       app_.zoomCenterY_ + normalizedY * step,
                       false,
                       true,
                       false);
    return true;
}

bool InteractionController::HasContinuousMotion() const
{
    return panLeftPressed_ || panRightPressed_ ||
           panUpPressed_ || panDownPressed_ ||
           std::abs(joystickPanX_) > 0.001f ||
           std::abs(joystickPanY_) > 0.001f;
}

void InteractionController::BeginMousePan(const QPointF& pos, const QSize& widgetSize)
{
    Q_UNUSED(widgetSize);
    middlePanActive_ = true;
    middlePanLastPos_ = pos;
    if (app_.uiState_->renderWidget_) {
        app_.uiState_->renderWidget_->setCursor(Qt::ClosedHandCursor);
        app_.uiState_->renderWidget_->grabMouse(Qt::ClosedHandCursor);
    }
}

bool InteractionController::UpdateMousePan(const QPointF& pos)
{
    if (!middlePanActive_) {
        return false;
    }

    float prevU = 0.0f;
    float prevV = 0.0f;
    float currU = 0.0f;
    float currV = 0.0f;

    bool prevValid = app_.MapViewToSource(middlePanLastPos_, prevU, prevV);
    bool currValid = app_.MapViewToSource(pos, currU, currV);

    if (!prevValid || !currValid) {
        middlePanLastPos_ = pos;
        return false;
    }

    const float deltaX = prevU - currU;
    const float deltaY = prevV - currV;
    if (std::abs(deltaX) < 1e-5f && std::abs(deltaY) < 1e-5f) {
        middlePanLastPos_ = pos;
        return false;
    }

    app_.SetZoomCenter(app_.zoomCenterX_ + deltaX,
                       app_.zoomCenterY_ + deltaY,
                       false,
                       true,
                       false);
    middlePanLastPos_ = pos;
    return true;
}

void InteractionController::EndMousePan()
{
    if (!middlePanActive_) {
        return;
    }
    middlePanActive_ = false;
    app_.SetZoomCenter(
        app_.zoomCenterX_, app_.zoomCenterY_, true, true, true);
    if (app_.uiState_->renderWidget_) {
        app_.uiState_->renderWidget_->releaseMouse();
        app_.uiState_->renderWidget_->unsetCursor();
    }
}

void InteractionController::ResetJoystick()
{
    joystickPanX_ = 0.0f;
    joystickPanY_ = 0.0f;
}

void InteractionController::SetJoystickAxes(float x, float y)
{
    const bool wasActive =
        std::abs(joystickPanX_) > 0.001f ||
        std::abs(joystickPanY_) > 0.001f;
    joystickPanX_ = std::clamp(x, -1.0f, 1.0f);
    joystickPanY_ = std::clamp(y, -1.0f, 1.0f);
    const bool isActive =
        std::abs(joystickPanX_) > 0.001f ||
        std::abs(joystickPanY_) > 0.001f;
    if (wasActive != isActive || isActive) {
        app_.pipelineOrchestrator_->NotifyViewportMotion();
        app_.pipelineOrchestrator_->UpdateTimerPolicy();
    }
    if (wasActive && !isActive) {
        app_.SetZoomCenter(
            app_.zoomCenterX_, app_.zoomCenterY_, true, true, true);
    }
}

} // namespace openzoom

#endif // _WIN32
