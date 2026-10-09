#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QPointF>
#include <QRectF>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
QT_END_NAMESPACE

namespace okuflow {

class JoystickOverlay : public QWidget {
    Q_OBJECT
public:
    explicit JoystickOverlay(QWidget* parent = nullptr);

    void ResetKnob();

signals:
    void JoystickChanged(float normX, float normY);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QRectF KnobRect() const;
    void UpdatePlacement();
    void UpdateFromPosition(const QPointF& pos);
    void UpdateMask();

    bool dragging_{};
    QPointF knobPos_{};
};

} // namespace okuflow

#endif // _WIN32
