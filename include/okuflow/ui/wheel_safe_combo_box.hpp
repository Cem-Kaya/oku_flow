#pragma once

#include <QComboBox>
#include <QHelpEvent>
#include <QSlider>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QToolTip>
#include <QWheelEvent>

namespace okuflow {

// A wheel over a settings selector belongs to the surrounding scroll panel.
// Selection remains available by click and keyboard without accidental edits.
// Long entries (device names) elide in the middle so both the product and the
// distinguishing suffix stay visible; hovering an elided entry shows it whole.
class WheelSafeComboBox final : public QComboBox {
public:
    explicit WheelSafeComboBox(QWidget* parent = nullptr) : QComboBox(parent) {}

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        event->ignore();
    }

    void paintEvent(QPaintEvent* event) override
    {
        if (isEditable()) {
            QComboBox::paintEvent(event);
            return;
        }
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        option.currentText = fontMetrics().elidedText(
            option.currentText, Qt::ElideMiddle, TextRect(option).width());
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::ToolTip && !isEditable()) {
            QStyleOptionComboBox option;
            initStyleOption(&option);
            if (fontMetrics().horizontalAdvance(option.currentText) >
                TextRect(option).width()) {
                QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(),
                                   option.currentText, this);
                return true;
            }
        }
        return QComboBox::event(event);
    }

private:
    QRect TextRect(const QStyleOptionComboBox& option) const
    {
        return style()->subControlRect(QStyle::CC_ComboBox, &option,
                                       QStyle::SC_ComboBoxEditField, this);
    }
};

// A wheel over a settings slider also belongs to the surrounding scroll panel.
// Values remain editable by dragging, clicking, and keyboard input.
class WheelSafeSlider final : public QSlider {
public:
    explicit WheelSafeSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSlider(orientation, parent)
    {
    }

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        event->ignore();
    }
};

} // namespace okuflow
