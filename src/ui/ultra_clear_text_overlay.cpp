#include "okuflow/ui/ultra_clear_text_overlay.hpp"

#if defined(_WIN32)

#include <QEvent>
#include <QFontMetricsF>
#include <QPainter>
#include <QPaintEvent>
#include <QRegion>

#include <algorithm>
#include <cmath>

namespace okuflow {
namespace {

constexpr qreal kReferencePixelSize = 100.0;

qreal RelativeLuminance(const QColor& color)
{
    const auto channel = [](qreal c) {
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
           0.0722 * channel(color.blueF());
}

// Keeps the scene's own colors (and so the user's display color scheme)
// when they already read well; otherwise falls back to pure black/white in
// the scene's polarity.
uct::LineColors ReadableColors(const uct::LineColors& measured)
{
    const qreal fg = RelativeLuminance(measured.foreground);
    const qreal bg = RelativeLuminance(measured.background);
    const qreal ratio = (std::max(fg, bg) + 0.05) / (std::min(fg, bg) + 0.05);
    if (ratio >= 7.0) {
        return measured;
    }
    const bool darkText = fg <= bg;
    return {darkText ? QColor(Qt::black) : QColor(Qt::white),
            darkText ? QColor(Qt::white) : QColor(Qt::black)};
}

QFont GlyphFont(int strokeWeight, qreal pixelSize)
{
    QFont font(QStringLiteral("Segoe UI"));
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(pixelSize))));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(500 + 100 * strokeWeight, 100, 900)));
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

} // namespace

UltraClearTextOverlay::UltraClearTextOverlay(QWidget* renderTarget, QWidget* owner)
    : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint |
                         Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus),
      renderTarget_(renderTarget)
{
    setObjectName(QStringLiteral("ultraClearTextOverlay"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    // The recognized text is exposed through the native reading pane; this
    // surface is a visual aid only.
    setAccessibleName(QStringLiteral("Ultra Clear Text overlay"));
    setAccessibleDescription(QStringLiteral(
        "Visual glyph overlay. The recognized text is available in the Ultra Clear Text reading pane."));
    if (renderTarget_) {
        for (QWidget* widget = renderTarget_; widget; widget = widget->parentWidget()) {
            widget->installEventFilter(this);
        }
        SyncGeometryToRenderTarget();
    }
    hide();
}

void UltraClearTextOverlay::SetViewTransform(const ViewTransform& transform)
{
    const ViewTransform& t = transform_;
    if (t.valid == transform.valid && t.sourceX == transform.sourceX &&
        t.sourceY == transform.sourceY && t.sourceWidth == transform.sourceWidth &&
        t.sourceHeight == transform.sourceHeight && t.destinationX == transform.destinationX &&
        t.destinationY == transform.destinationY &&
        t.destinationWidth == transform.destinationWidth &&
        t.destinationHeight == transform.destinationHeight) {
        return;
    }
    transform_ = transform;
    if (!words_.isEmpty()) {
        update();
    }
}

void UltraClearTextOverlay::SetWords(QVector<SceneWord> words)
{
    words_ = std::move(words);
    if (advanceCache_.size() > 4096) {
        advanceCache_.clear();
    }
    UpdateVisibility();
    update();
}

void UltraClearTextOverlay::ClearWords()
{
    if (words_.isEmpty() && !isVisible()) {
        return;
    }
    words_.clear();
    UpdateVisibility();
    update();
}

void UltraClearTextOverlay::SetStrokeWeight(int weight)
{
    weight = std::clamp(weight, -3, 3);
    if (weight == strokeWeight_) {
        return;
    }
    strokeWeight_ = weight;
    advanceCache_.clear();
    update();
}

void UltraClearTextOverlay::SetExcludedWidget(QWidget* widget)
{
    excludedWidget_ = widget;
    update();
}

QVector<QRectF> UltraClearTextOverlay::ProjectedWordRects() const
{
    QVector<QRectF> rects;
    for (const SceneWord& word : words_) {
        const QRectF view = uct::SceneRectToView(transform_, width(), height(), word.sceneRect);
        if (!view.isEmpty()) {
            rects.push_back(view);
        }
    }
    return rects;
}

bool UltraClearTextOverlay::eventFilter(QObject* watched, QEvent* event)
{
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && renderTarget_ &&
        (widget == renderTarget_ || widget->isAncestorOf(renderTarget_))) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::WindowStateChange:
            SyncGeometryToRenderTarget();
            UpdateVisibility();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void UltraClearTextOverlay::SyncGeometryToRenderTarget()
{
    if (!renderTarget_) {
        return;
    }
    const QRect target(renderTarget_->mapToGlobal(QPoint(0, 0)), renderTarget_->size());
    if (geometry() != target) {
        setGeometry(target);
    }
}

void UltraClearTextOverlay::UpdateVisibility()
{
    QWidget* window = renderTarget_ ? renderTarget_->window() : nullptr;
    const bool targetShown = renderTarget_ && renderTarget_->isVisible() && window &&
                             !(window->windowState() & Qt::WindowMinimized);
    const bool wanted = targetShown && !words_.isEmpty();
    if (wanted && !isVisible()) {
        SyncGeometryToRenderTarget();
        show();
    } else if (!wanted && isVisible()) {
        hide();
    }
}

qreal UltraClearTextOverlay::AdvanceAtReference(const QString& text)
{
    const auto it = advanceCache_.constFind(text);
    if (it != advanceCache_.constEnd()) {
        return it.value();
    }
    const QFontMetricsF metrics(GlyphFont(strokeWeight_, kReferencePixelSize));
    const qreal advance = std::max<qreal>(1.0, metrics.horizontalAdvance(text));
    advanceCache_.insert(text, advance);
    return advance;
}

void UltraClearTextOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    ++paintCount_;
    if (words_.isEmpty() || !transform_.valid) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Never paint over native controls floating above the viewport.
    QRegion clip(rect());
    const auto exclude = [&](QWidget* widget) {
        if (!widget || !widget->isVisible()) {
            return;
        }
        const QPoint topLeft = mapFromGlobal(widget->mapToGlobal(QPoint(0, 0)));
        clip -= QRegion(QRect(topLeft, widget->size()));
    };
    exclude(excludedWidget_);
    if (QWidget* owner = parentWidget()) {
        for (const QString& name :
             {QStringLiteral("topLeftPanel"), QStringLiteral("bottomLeftPanel"),
              QStringLiteral("keystoneTrackingPanel"), QStringLiteral("bottomRightPanel"),
              QStringLiteral("uiVisibilityPanel"), QStringLiteral("annotationOverlay")}) {
            QWidget* panel = owner->findChild<QWidget*>(name);
            // The annotation canvas is transparent; only exclude its chrome.
            if (panel && name == QStringLiteral("annotationOverlay")) {
                for (QWidget* chrome : panel->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
                    exclude(chrome);
                }
                continue;
            }
            exclude(panel);
        }
    }
    painter.setClipRegion(clip);

    for (const SceneWord& word : words_) {
        const QRectF view = uct::SceneRectToView(transform_, width(), height(), word.sceneRect);
        // Tiny glyphs gain nothing from replacement; leave the original.
        if (view.height() < 6.0 || view.width() < 3.0 || !rect().intersects(view.toAlignedRect())) {
            continue;
        }
        const uct::LineColors colors = ReadableColors(word.colors);
        const qreal pad = view.height() * 0.12;
        painter.fillRect(view.adjusted(-pad, -pad * 0.5, pad, pad * 0.5), colors.background);
        const qreal pixelSize = view.height() * 0.82;
        const qreal advance = AdvanceAtReference(word.text) * pixelSize / kReferencePixelSize;
        const qreal scaleX = std::clamp(view.width() / std::max<qreal>(1.0, advance), 0.55, 1.8);
        painter.save();
        painter.translate(view.left(), view.top());
        painter.scale(scaleX, 1.0);
        painter.setFont(GlyphFont(strokeWeight_, pixelSize));
        painter.setPen(colors.foreground);
        painter.drawText(QRectF(0.0, 0.0, view.width() / scaleX, view.height()),
                         Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, word.text);
        painter.restore();
    }
}

} // namespace okuflow

#endif // _WIN32
