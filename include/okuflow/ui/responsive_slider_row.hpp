#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QWidget>

QT_BEGIN_NAMESPACE
class QGridLayout;
class QResizeEvent;
class QSlider;
QT_END_NAMESPACE

namespace okuflow {

class ResponsiveSliderRow final : public QWidget {
public:
    ResponsiveSliderRow(QWidget* leadingWidget,
                        QSlider* slider,
                        QWidget* trailingWidget = nullptr,
                        QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void ApplyLayout(bool stacked);

    QGridLayout* layout_{};
    QWidget* leadingWidget_{};
    QSlider* slider_{};
    QWidget* trailingWidget_{};
    bool stacked_{true};
};

} // namespace okuflow

#endif // _WIN32
