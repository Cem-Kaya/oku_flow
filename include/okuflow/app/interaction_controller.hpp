#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QPointF>
#include <QElapsedTimer>
#include <QSize>

class QWheelEvent;

namespace okuflow {

class OkuFlowApp;

class InteractionController {
public:
    explicit InteractionController(OkuFlowApp& app);

    bool HandlePanKey(int key, bool pressed);
    bool HandlePanScroll(const QWheelEvent* wheelEvent);
    void HandleZoomWheel(const QWheelEvent* wheelEvent);
    void HandleKeyboardZoom(float notches);

    bool ApplyInputForces(double elapsedSeconds);
    bool HasContinuousMotion() const;

    void BeginMousePan(const QPointF& pos, const QSize& widgetSize);
    bool UpdateMousePan(const QPointF& pos);
    void EndMousePan();
    bool IsMousePanActive() const { return middlePanActive_; }

    void ResetJoystick();
    void SetJoystickAxes(float x, float y);

private:
    float ScaledZoom(float current, float notches, float accel) const;
    void ApplyZoom(float target, const QPointF* localPos);
    void ScheduleZoomAnnouncement();

    OkuFlowApp& app_;
    bool panLeftPressed_{false};
    bool panRightPressed_{false};
    bool panUpPressed_{false};
    bool panDownPressed_{false};
    float joystickPanX_{0.0f};
    float joystickPanY_{0.0f};
    bool middlePanActive_{false};
    QPointF middlePanLastPos_{};
    QElapsedTimer wheelTimer_;
    float wheelAccel_{1.0f};
    int wheelDirection_{};
    quint64 zoomAnnouncementGeneration_{};
};

} // namespace okuflow

#endif // _WIN32
