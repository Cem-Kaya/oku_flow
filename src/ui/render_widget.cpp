#ifdef _WIN32

#include "openzoom/ui/render_widget.hpp"

#include "openzoom/d3d12/presenter.hpp"

#include <Windows.h>

#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>

#include <algorithm>

namespace openzoom {

RenderWidget::RenderWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    presenterResizeTimer_ = new QTimer(this);
    presenterResizeTimer_->setSingleShot(true);
    presenterResizeTimer_->setTimerType(Qt::PreciseTimer);
    presenterResizeTimer_->setInterval(16);
    connect(presenterResizeTimer_,
            &QTimer::timeout,
            this,
            &RenderWidget::ApplyPendingPresenterResize);
}

QPaintEngine* RenderWidget::paintEngine() const {
    return nullptr;
}

void RenderWidget::setPresenter(D3D12Presenter* presenter) {
    presenter_ = presenter;
}

bool RenderWidget::isPresenterReady() const {
    return presenter_ && presenter_->IsInitialized();
}

void RenderWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    EnsurePresenter();
}

void RenderWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Throttle to the next frame; restarting an active single-shot timer would
    // defer every resize indefinitely while a splitter keeps moving.
    if (EnsurePresenter() && !presenterResizeTimer_->isActive()) {
        presenterResizeTimer_->start();
    }
}

bool RenderWidget::EnsurePresenter() {
    if (presenter_ && presenter_->IsFaulted()) return false;
    if (!presenter_ || presenter_->IsInitialized()) {
        return presenter_ != nullptr;
    }

    const HWND hwnd = reinterpret_cast<HWND>(winId());
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const UINT nativeWidth = static_cast<UINT>(
        std::max<LONG>(1, clientRect.right - clientRect.left));
    const UINT nativeHeight = static_cast<UINT>(
        std::max<LONG>(1, clientRect.bottom - clientRect.top));
    presenter_->Initialize(hwnd, nativeWidth, nativeHeight);
    return presenter_->IsInitialized();
}

void RenderWidget::ApplyPendingPresenterResize() {
    if (!presenter_ || !presenter_->IsInitialized()) {
        return;
    }
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    RECT clientRect{};
    if (!GetClientRect(hwnd, &clientRect)) {
        return;
    }
    const UINT nativeWidth = static_cast<UINT>(
        std::max<LONG>(1, clientRect.right - clientRect.left));
    const UINT nativeHeight = static_cast<UINT>(
        std::max<LONG>(1, clientRect.bottom - clientRect.top));
    if (nativeWidth != presenter_->ViewportWidth() ||
        nativeHeight != presenter_->ViewportHeight()) {
        presenter_->Resize(nativeWidth, nativeHeight);
    }
}

} // namespace openzoom

#endif // _WIN32
