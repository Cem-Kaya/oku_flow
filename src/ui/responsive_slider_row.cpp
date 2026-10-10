#ifdef _WIN32

#include "okuflow/ui/responsive_slider_row.hpp"

#include <QGridLayout>
#include <QResizeEvent>
#include <QSlider>
#include <algorithm>

namespace okuflow {

ResponsiveSliderRow::ResponsiveSliderRow(QWidget* leadingWidget,
                                         QSlider* slider,
                                         QWidget* trailingWidget,
                                         QWidget* parent)
    : QWidget(parent),
      leadingWidget_(leadingWidget),
      slider_(slider),
      trailingWidget_(trailingWidget)
{
    layout_ = new QGridLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setHorizontalSpacing(8);
    layout_->setVerticalSpacing(4);
    // Inline layout is a preference, not a hard minimum. Otherwise the parent
    // scroll area widens its content before resizeEvent can select stacking.
    layout_->setSizeConstraint(QLayout::SetNoConstraint);
    slider_->setMinimumWidth(120);
    ApplyLayout(false);
}

QSize ResponsiveSliderRow::minimumSizeHint() const
{
    const QSize leading = leadingWidget_->minimumSizeHint().expandedTo(leadingWidget_->minimumSize());
    const QSize slider = slider_->minimumSizeHint().expandedTo(slider_->minimumSize());
    const QSize trailing = trailingWidget_
        ? trailingWidget_->minimumSizeHint().expandedTo(trailingWidget_->minimumSize())
        : QSize(0, 0);
    const int controlsWidth = slider.width() +
        (trailingWidget_ ? trailing.width() + layout_->horizontalSpacing() : 0);
    return QSize(std::max(leading.width(), controlsWidth),
                 layout_->minimumSize().height());
}

void ResponsiveSliderRow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const int trailingWidth = trailingWidget_
        ? std::max(trailingWidget_->sizeHint().width(), trailingWidget_->minimumWidth()) + 8
        : 0;
    const int inlineWidth = leadingWidget_->sizeHint().width() +
                            std::max(slider_->minimumSizeHint().width(), slider_->minimumWidth()) +
                            trailingWidth + 8;
    ApplyLayout(event->size().width() < inlineWidth);
}

void ResponsiveSliderRow::ApplyLayout(bool stacked)
{
    if (stacked_ == stacked && layout_->indexOf(leadingWidget_) >= 0) {
        return;
    }
    stacked_ = stacked;
    layout_->removeWidget(leadingWidget_);
    layout_->removeWidget(slider_);
    if (trailingWidget_) {
        layout_->removeWidget(trailingWidget_);
    }

    if (stacked_) {
        layout_->addWidget(leadingWidget_, 0, 0, 1, 3);
        layout_->addWidget(slider_, 1, 0, 1, trailingWidget_ ? 2 : 3);
        if (trailingWidget_) {
            layout_->addWidget(trailingWidget_, 1, 2);
        }
    } else {
        layout_->addWidget(leadingWidget_, 0, 0);
        layout_->addWidget(slider_, 0, 1);
        if (trailingWidget_) {
            layout_->addWidget(trailingWidget_, 0, 2);
        }
    }
    layout_->setColumnStretch(1, 1);
    updateGeometry();
}

} // namespace okuflow

#endif // _WIN32
