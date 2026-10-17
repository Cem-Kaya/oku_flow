#include "okuflow/ui/ultra_clear_text_overlay.hpp"

#if defined(_WIN32)

#include <QEvent>
#include <QFontMetricsF>
#include <QPainter>
#include <QPaintEvent>
#include <QRegion>

#include <algorithm>
#include <cmath>
#include <limits>

namespace okuflow {
namespace {

constexpr qreal kReferencePixelSize = 100.0;
// Tiny glyphs gain nothing from replacement; leave the original camera text.
constexpr qreal kMinProjectedHeight = 6.0;
constexpr qreal kMinProjectedWidth = 3.0;

// The one eligibility decision shared by the plate pass and the glyph pass.
// The plate covers the original word, so it may only be painted when the
// replacement glyph will be painted over it in the same frame, and the
// glyph may only be painted on its plate. Both the original box and the
// fitted ink must meet the minimum projected size, the fitted glyph must be
// valid, and some of both the plate and the ink must survive clipping.
bool ReplacementIsDrawable(bool glyphValid, const QRectF& referenceInk,
                           const QRectF& originalView, const QRectF& inkView,
                           const QRectF& plateView, const QRegion& clip)
{
    if (!glyphValid || referenceInk.isEmpty() || originalView.isEmpty() ||
        inkView.isEmpty() || plateView.isEmpty()) {
        return false;
    }
    if (!std::isfinite(originalView.width()) || !std::isfinite(originalView.height()) ||
        !std::isfinite(inkView.width()) || !std::isfinite(inkView.height())) {
        return false;
    }
    if (originalView.height() < kMinProjectedHeight ||
        originalView.width() < kMinProjectedWidth ||
        inkView.height() < kMinProjectedHeight || inkView.width() < kMinProjectedWidth) {
        return false;
    }
    return clip.intersects(plateView.toAlignedRect()) &&
           clip.intersects(inkView.toAlignedRect());
}

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
    referenceFont_ = GlyphFont(strokeWeight_, kReferencePixelSize);
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
    if (inkCache_.size() > 4096) {
        inkCache_.clear();
    }
    RebuildGlyphLayout();
    UpdateVisibility();
    update();
}

void UltraClearTextOverlay::ClearWords()
{
    if (words_.isEmpty() && !isVisible()) {
        return;
    }
    words_.clear();
    glyphs_.clear();
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
    referenceFont_ = GlyphFont(strokeWeight_, kReferencePixelSize);
    inkCache_.clear();
    RebuildGlyphLayout();
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

QVector<UltraClearTextOverlay::GlyphGeometry> UltraClearTextOverlay::ProjectedGlyphs() const
{
    QVector<GlyphGeometry> result;
    result.reserve(glyphs_.size());
    for (const SceneGlyph& glyph : glyphs_) {
        const QPointF origin = uct::SceneToViewPixel(transform_, width(), height(), glyph.origin);
        result.push_back({uct::SceneRectToView(transform_, width(), height(), glyph.inkRect), origin.y()});
    }
    return result;
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

QRectF UltraClearTextOverlay::InkAtReference(const QString& text)
{
    const auto it = inkCache_.constFind(text);
    if (it != inkCache_.constEnd()) {
        return it.value();
    }
    const QFontMetricsF metrics(referenceFont_);
    const QRectF ink = metrics.tightBoundingRect(text);
    inkCache_.insert(text, ink);
    return ink;
}

void UltraClearTextOverlay::RebuildGlyphLayout()
{
    // OCR boxes bound ink, not the font em square: e.g. "of" has neither
    // capitals nor descenders, while "Changing" has both. Fit a robust shared
    // type size/baseline per recognized line instead of stretching each word
    // to an unrelated height. This work is cached until content/style changes;
    // presentation only reprojects it.
    const auto median = [](QVector<qreal> values) {
        const qsizetype middle = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + middle, values.end());
        return values[middle];
    };
    QHash<std::uint64_t, QVector<int>> lines;
    for (int i = 0; i < words_.size(); ++i) {
        // Unlabelled legacy/test words are independent, never one giant line.
        const std::uint64_t key = words_[i].lineId ? words_[i].lineId :
                                 std::numeric_limits<std::uint64_t>::max() - i;
        lines[key].push_back(i);
    }
    glyphs_.clear();
    glyphs_.resize(words_.size());
    for (const QVector<int>& indices : lines) {
        QVector<qreal> sizes;
        for (int i : indices) {
            const QRectF ink = InkAtReference(words_[i].text);
            if (ink.height() > 0.0 && !words_[i].sceneRect.isEmpty()) {
                sizes.push_back(words_[i].sceneRect.height() / ink.height());
            }
        }
        if (sizes.isEmpty()) {
            continue;
        }
        const qreal scaleY = median(std::move(sizes));
        QVector<qreal> baselines;
        for (int i : indices) {
            const QRectF ink = InkAtReference(words_[i].text);
            if (!ink.isEmpty()) {
                baselines.push_back(words_[i].sceneRect.top() - ink.top() * scaleY);
            }
        }
        if (baselines.isEmpty()) {
            continue;
        }
        const qreal baseline = median(std::move(baselines));
        for (int i : indices) {
            const SceneWord& word = words_[i];
            const QRectF ink = InkAtReference(word.text);
            if (ink.isEmpty() || word.sceneRect.isEmpty()) {
                glyphs_[i] = {};
                continue;
            }
            const qreal scaleX = word.sceneRect.width() / ink.width();
            const QRectF fitted(word.sceneRect.left(), baseline + ink.top() * scaleY,
                                word.sceneRect.width(), ink.height() * scaleY);
            if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || fitted.isEmpty()) {
                glyphs_[i] = {};
                continue;
            }
            // Cache the measured reference ink and the readable-color
            // decision with the layout. Paint only reprojects these, so
            // pan/zoom at presentation rate repeats neither font
            // measurement nor color work.
            glyphs_[i] = {fitted,
                          QPointF(word.sceneRect.left() - ink.left() * scaleX, baseline),
                          ink, ReadableColors(word.colors), true};
        }
    }
}

void UltraClearTextOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    ++paintCount_;
    if (words_.isEmpty() || glyphs_.size() != words_.size() || !transform_.valid) {
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

    // Decide eligibility once per word for this projection, then let both
    // passes consume that single decision: a plate is painted if and only
    // if its glyph is painted over it.
    const QVector<GlyphGeometry> projected = ProjectedGlyphs();
    QVector<QRectF> plateRects(words_.size());
    QVector<bool> drawable(words_.size(), false);
    for (int i = 0; i < words_.size(); ++i) {
        const QRectF view = uct::SceneRectToView(transform_, width(), height(), words_[i].sceneRect);
        const qreal pad = view.height() * 0.12;
        plateRects[i] = view.united(projected[i].inkRect).adjusted(-pad, -pad * 0.5, pad, pad * 0.5);
        drawable[i] = ReplacementIsDrawable(glyphs_[i].valid, glyphs_[i].referenceInk, view,
                                            projected[i].inkRect, plateRects[i], clip);
    }
    // Paint all plates before any glyphs so a neighbouring word's plate
    // cannot erase an overhang or descender already drawn.
    for (int i = 0; i < words_.size(); ++i) {
        if (drawable[i]) {
            painter.fillRect(plateRects[i], glyphs_[i].colors.background);
        }
    }
    for (int i = 0; i < words_.size(); ++i) {
        if (!drawable[i]) {
            continue;
        }
        const SceneWord& word = words_[i];
        const SceneGlyph& glyph = glyphs_[i];
        const QRectF view = projected[i].inkRect;
        const QPointF origin = uct::SceneToViewPixel(transform_, width(), height(), glyph.origin);
        const qreal scaleX = view.width() / glyph.referenceInk.width();
        const qreal scaleY = view.height() / glyph.referenceInk.height();
        painter.save();
        painter.translate(origin);
        painter.scale(scaleX, scaleY);
        painter.setFont(referenceFont_);
        painter.setPen(glyph.colors.foreground);
        painter.drawText(QPointF(0.0, 0.0), word.text);
        painter.restore();
    }
}

} // namespace okuflow

#endif // _WIN32
