#include "okuflow/ui/ultra_clear_text_overlay.hpp"

#if defined(_WIN32)

#include <QApplication>
#include <QBackingStore>
#include <QEvent>
#include <QFontMetricsF>
#include <QLineF>
#include <QPainter>
#include <QPolygon>
#include <QPaintEvent>
#include <QRegion>
#include <QScreen>
#include <QScopeGuard>
#include <QSet>
#include <QThread>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace okuflow {
namespace {

constexpr qreal kReferencePixelSize = 100.0;
// Tiny glyphs gain nothing from replacement; leave the original camera text.
constexpr qreal kMinProjectedHeight = 6.0;
constexpr qreal kMinProjectedWidth = 3.0;
// Words whose shapes share at least this fraction of the smaller one are
// claiming the same ink. Correct neighbours (same or adjacent lines) touch
// at most at kerning/descender edges.
constexpr double kOverlapFraction = 0.2;

QRectF PointBounds(const std::array<QPointF, 4>& points)
{
    qreal left = points[0].x(), right = left, top = points[0].y(), bottom = top;
    for (const QPointF& point : points) {
        left = std::min(left, point.x()); right = std::max(right, point.x());
        top = std::min(top, point.y()); bottom = std::max(bottom, point.y());
    }
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

QRect PixelCoverage(const QRectF& rect, const QRect& bounds)
{
    const QRectF clipped = rect.intersected(QRectF(bounds));
    if (clipped.isEmpty()) return {};
    // Half-open coverage by pixel centres. Outward rounding BOTH sides of
    // a fractional carve gives disjoint source spans a shared pixel row.
    const int left = static_cast<int>(std::ceil(clipped.left() - .5));
    const int top = static_cast<int>(std::ceil(clipped.top() - .5));
    const int right = static_cast<int>(std::ceil(clipped.right() - .5));
    const int bottom = static_cast<int>(std::ceil(clipped.bottom() - .5));
    return QRect(left, top, std::max(0, right - left), std::max(0, bottom - top));
}

QRegion ProtectedRegion(const QRectF& rect, const QRect& bounds, bool carved = false)
{
    const QRect covered = PixelCoverage(rect, bounds);
    // Original thin marks need conservative coverage if either axis has no
    // centres. Only subtraction remainders use the narrower strip rule:
    // outward rounding there would invent a pixel shared with proved ink.
    const bool emptyX = covered.width() == 0, emptyY = covered.height() == 0;
    return (carved ? emptyX && emptyY : emptyX || emptyY)
        ? QRegion(rect.intersected(QRectF(bounds)).toAlignedRect()) : QRegion(covered);
}

QRegion PolygonRegion(const std::array<QPointF, 4>& points, const QRect& bounds)
{
    QPolygon polygon;
    polygon.reserve(4);
    for (const QPointF& point : points)
        polygon << QPoint(static_cast<int>(std::ceil(point.x() - .5)),
                          static_cast<int>(std::ceil(point.y() - .5)));
    return QRegion(polygon).intersected(PixelCoverage(PointBounds(points), bounds));
}

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
    const auto finite = [](const QRectF& r) {
        return std::isfinite(r.x()) && std::isfinite(r.y()) &&
               std::isfinite(r.width()) && std::isfinite(r.height());
    };
    if (!finite(originalView) || !finite(inkView) || !finite(plateView)) {
        return false;
    }
    if (originalView.height() < kMinProjectedHeight ||
        originalView.width() < kMinProjectedWidth ||
        inkView.height() < kMinProjectedHeight || inkView.width() < kMinProjectedWidth) {
        return false;
    }
    const QRectF surface(clip.boundingRect());
    return clip.intersects(plateView.intersected(surface).toAlignedRect()) &&
           clip.intersects(inkView.intersected(surface).toAlignedRect());
}

qreal RelativeLuminance(const QColor& color)
{
    const auto channel = [](qreal c) {
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
           0.0722 * channel(color.blueF());
}

// Preserve the source board/paper color. Improve ink first; only if even pure
// ink cannot reach 7:1 do we move neutral paper toward its polarity endpoint.
// Clearly colored surfaces retain their hue/paper at a 3:1 solid-ink floor.
// This is cached with the layout, never performed on the pan/zoom paint path.
uct::LineColors ReadableColors(const uct::LineColors &measured) {
    const auto contrast = [](const QColor &a, const QColor &b) {
        const qreal x = RelativeLuminance(a), y = RelativeLuminance(b);
        return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
    };
    const int spread = std::max({measured.foreground.red(), measured.foreground.green(),
                                 measured.foreground.blue()}) -
                       std::min({measured.foreground.red(), measured.foreground.green(),
                                 measured.foreground.blue()});
    // A small chroma spread is common in nominally neutral compressed video:
    // AA picks up the paper/board tint. Treat it as one solid black/white ink
    // instead of alternating white, cyan and green from word to word. Keep
    // clearly chromatic, already-readable ink (e.g. a yellow UI label).
    if (spread >= 64 && contrast(measured.foreground, measured.background) >= 7.0)
        return measured; // already readable chromatic ink retains its hue
    const bool darkText =
        RelativeLuminance(measured.foreground) <= RelativeLuminance(measured.background);
    uct::LineColors result{darkText ? QColor(Qt::black) : QColor(Qt::white), measured.background};
    const int paperSpread = std::max({measured.background.red(), measured.background.green(),
                                      measured.background.blue()}) -
                            std::min({measured.background.red(), measured.background.green(),
                                      measured.background.blue()});
    const double sourceContrast = contrast(result.foreground, result.background);
    if (sourceContrast >= 7.0 || (paperSpread >= 64 && sourceContrast >= 3.0))
        return result;
    const QColor endpoint = darkText ? QColor(Qt::white) : QColor(Qt::black);
    const auto mix = [&](double amount) {
        return QColor::fromRgbF(
            measured.background.redF() * (1.0 - amount) + endpoint.redF() * amount,
            measured.background.greenF() * (1.0 - amount) + endpoint.greenF() * amount,
            measured.background.blueF() * (1.0 - amount) + endpoint.blueF() * amount);
    };
    double low = 0.0, high = 1.0;
    for (int i = 0; i < 16; ++i) {
        const double middle = (low + high) * 0.5;
        if (contrast(result.foreground, mix(middle)) >= 7.0)
            high = middle;
        else
            low = middle;
    }
    result.background = mix(high);
    return result;
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
    applicationActive_ = QGuiApplication::applicationState() == Qt::ApplicationActive;
    // One application filter sees activation as well as target/ancestor
    // geometry and modality events, without duplicate per-widget filters.
    qApp->installEventFilter(this);
    if (renderTarget_) {
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
    timingPaintedRevision_ = 0;
    if (uct::timing::Enabled() && timingTicket_.admission.producer)
        uct::timing::RecordEvent(uct::timing::Event::OverlayAssigned, timingTicket_.admission,
            {0, static_cast<std::uint32_t>(words_.size()), timingTicket_.flags, 0, timingTicket_.revision});
    if (inkCache_.size() > 4096) {
        inkCache_.clear();
    }
    if (caretCache_.size() > 4096) {
        caretCache_.clear();
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
    caretCache_.clear();
    RebuildGlyphLayout();
    update();
}

void UltraClearTextOverlay::SetExcludedWidget(QWidget* widget)
{
    excludedWidget_ = widget;
    update();
}

void UltraClearTextOverlay::SetDisplayColors(
    DisplayColors mode, const std::array<std::uint32_t, 256>& lut)
{
    if (mode == displayColors_ && (mode != DisplayColors::LumaLut || lut == displayColorLut_))
        return;
    displayColors_ = mode;
    displayColorLut_ = lut; // owned palette; never borrow mutable application settings
    for (int i = 0; i < glyphs_.size(); ++i)
        glyphs_[i].colors = GlyphColors(words_[i].colors);
    update(); // colors only: no shaping, recognition, proof or geometry changes
}

uct::LineColors UltraClearTextOverlay::GlyphColors(const uct::LineColors& measured) const
{
    auto colors = ReadableColors(measured);
    const auto map = [this](const QColor& color) {
        if (displayColors_ == DisplayColors::Inverted)
            return QColor(255 - color.red(), 255 - color.green(), 255 - color.blue());
        if (displayColors_ == DisplayColors::LumaLut) {
            // Same encoded-RGB luma and truncating byte index as the native
            // CUDA display-color pass. Never re-run readability normalization
            // afterwards: it would replace the user's selected palette again.
            const float luma = .299f * (color.red() / 255.f) +
                               .587f * (color.green() / 255.f) +
                               .114f * (color.blue() / 255.f);
            const auto index = static_cast<unsigned>(std::clamp(luma, 0.f, 1.f) * 255.f);
            return QColor::fromRgb(displayColorLut_[index]);
        }
        return color;
    };
    colors.foreground = map(colors.foreground);
    colors.background = map(colors.background);
    return colors;
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
    for (int i = 0; i < glyphs_.size(); ++i) {
        const SceneGlyph& glyph = glyphs_[i];
        if (glyph.oriented) {
            PlannedWord planned;
            if (PlanOriented(planned, glyph, uct::SceneQuadToView(transform_, width(), height(), words_[i].sceneQuad)))
                result.push_back({planned.inkRect, planned.origin.y()});
            else
                result.push_back({});
            continue;
        }
        const QPointF origin = uct::SceneToViewPixel(transform_, width(), height(), glyph.origin);
        result.push_back({uct::SceneRectToView(transform_, width(), height(), glyph.inkRect), origin.y()});
    }
    return result;
}

bool UltraClearTextOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == qApp) {
        switch (event->type()) {
        case QEvent::ApplicationActivate:
            applicationActive_ = true;
            UpdateVisibility();
            break;
        case QEvent::ApplicationDeactivate:
            applicationActive_ = false;
            UpdateVisibility();
            break;
        default:
            break;
        }
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && renderTarget_ &&
        (widget == renderTarget_ || widget->isAncestorOf(renderTarget_))) {
        switch (event->type()) {
        case QEvent::WindowBlocked:
        case QEvent::WindowUnblocked:
            if (widget == renderTarget_->window()) {
                ownerBlocked_ = event->type() == QEvent::WindowBlocked;
                UpdateVisibility();
            }
            break;
        case QEvent::EnabledChange:
            UpdateVisibility();
            break;
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
    // The tool window is a sibling of native dialogs, not part of their
    // client area. A late OCR completion must not show it above a modal file
    // picker or another application. Keep the words for a normal resume.
    const bool wanted = targetShown && applicationActive_ && !ownerBlocked_ &&
                        window->isEnabled() && !QApplication::activeModalWidget() &&
                        !words_.isEmpty();
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

bool UltraClearTextOverlay::RebuildGlyphLayout(const QDeadlineTimer* deadline)
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
    QHash<std::uint64_t, QVector<int>> lines, orientedLines;
    for (int i = 0; i < words_.size(); ++i) {
        if (deadline && deadline->hasExpired()) return false;
        // Unlabelled legacy/test words are independent, never one giant line.
        const std::uint64_t group = words_[i].layoutGroupId ? words_[i].layoutGroupId : words_[i].lineId;
        const std::uint64_t key = group ? group : std::numeric_limits<std::uint64_t>::max() - i;
        (words_[i].sceneQuad.valid ? orientedLines : lines)[key].push_back(i);
    }
    glyphs_.clear();
    glyphs_.resize(words_.size());
    // Rotated lines share one type size measured along their own height
    // axis. Each word keeps its own fitted top: on a curled page the
    // baseline follows the word, never a straight median across the arc.
    // Ratios along one axis survive every later pan/zoom projection.
    for (const QVector<int>& indices : orientedLines) {
        if (deadline && deadline->hasExpired()) return false;
        QVector<qreal> sizes;
        for (int i : indices) {
            if (deadline && deadline->hasExpired()) return false;
            const QRectF ink = InkAtReference(words_[i].text);
            const qreal h = uct::QuadHeight(words_[i].sceneQuad);
            if (ink.height() > 0.0 && h > 0.0 && std::isfinite(h)) sizes.push_back(h / ink.height());
        }
        if (sizes.isEmpty()) continue;
        const qreal scale = median(std::move(sizes));
        for (int i : indices) {
            if (deadline && deadline->hasExpired()) return false;
            const QRectF ink = InkAtReference(words_[i].text);
            const qreal h = uct::QuadHeight(words_[i].sceneQuad);
            SceneGlyph glyph;
            if (!ink.isEmpty() && h > 0.0 && std::isfinite(h)) {
                const auto& q=words_[i].sceneQuad;
                const auto warp=q.points[2]-q.points[1]-q.points[3]+q.points[0];
                // A shared screen-space type size flattens depth. Genuine
                // four-corner taper keeps the word's own source ink height.
                const bool perspective=std::hypot(warp.x(),warp.y())>
                    std::min(h,uct::QuadWidth(q))*.005;
                const qreal localScale=perspective?h/ink.height():scale;
                glyph.inkRect = words_[i].sceneQuad.Bounds();
                glyph.origin = words_[i].sceneQuad.points[3];
                glyph.referenceInk = ink;
                glyph.colors = GlyphColors(words_[i].colors);
                glyph.oriented = true;
                glyph.heightRatio = localScale * ink.height() / h;
                glyph.baselineFraction = -ink.top() * localScale / h;
                glyph.valid = std::isfinite(glyph.heightRatio) && std::isfinite(glyph.baselineFraction) &&
                              glyph.heightRatio > 0.0;
            }
            glyphs_[i] = glyph;
        }
    }
    for (const QVector<int>& indices : lines) {
        if (deadline && deadline->hasExpired()) return false;
        QVector<qreal> sizes;
        for (int i : indices) {
            if (deadline && deadline->hasExpired()) return false;
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
            if (deadline && deadline->hasExpired()) return false;
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
            if (deadline && deadline->hasExpired()) return false;
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
                          ink, GlyphColors(word.colors), true};
        }
    }
    return ResolveOverlaps(deadline);
}

bool UltraClearTextOverlay::ResolveOverlaps(const QDeadlineTimer* deadline)
{
    // Scene areas scale uniformly under every view transform, so the
    // overlap fraction is decided once here rather than on each pan.
    struct Item { int index; QRectF bounds; uct::TextQuad shape; double area; };
    QVector<Item> items;
    items.reserve(words_.size());
    for (int i = 0; i < words_.size(); ++i) {
        if (deadline && deadline->hasExpired()) return false;
        if (!glyphs_[i].valid) continue;
        const uct::TextQuad shape = words_[i].sceneQuad.valid ? words_[i].sceneQuad
                                                              : uct::TextQuad::FromRect(words_[i].sceneRect);
        const double area = uct::QuadArea(shape);
        if (area > 0.0 && std::isfinite(area)) items.push_back({i, shape.Bounds(), shape, area});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.bounds.top() < b.bounds.top() || (a.bounds.top() == b.bounds.top() && a.index < b.index);
    });
    for (qsizetype a = 0; a < items.size(); ++a) {
        for (qsizetype b = a + 1; b < items.size() && items[b].bounds.top() <= items[a].bounds.bottom(); ++b) {
            if (deadline && deadline->hasExpired()) return false;
            if (!items[a].bounds.intersects(items[b].bounds)) continue;
            const double smaller = std::min(items[a].area, items[b].area);
            const double shared = uct::QuadIntersectionArea(items[a].shape, items[b].shape);
            if (shared < kOverlapFraction * smaller) continue;
            const int first = std::min(items[a].index, items[b].index);
            const int second = std::max(items[a].index, items[b].index);
            if (words_[first].text == words_[second].text && shared >= 0.5 * smaller) {
                glyphs_[second].duplicate = true; // one observation; the twin replaces this ink
            } else {
                glyphs_[first].collision = glyphs_[second].collision = true;
            }
        }
    }
    return !deadline || !deadline->hasExpired();
}

bool UltraClearTextOverlay::PlanOriented(PlannedWord& planned, const SceneGlyph& glyph,
                                         const uct::TextQuad& viewQuad) const
{
    planned.oriented = true;
    planned.omission = PaintOmission::kGeometryOrSize;
    planned.originalRect = planned.inkRect = planned.plateRect = {};
    planned.original = planned.ink = planned.plate = {};
    if (viewQuad.valid) {
        planned.original = viewQuad.points;
        planned.originalRect = PointBounds(viewQuad.points);
    }
    if (!viewQuad.valid || glyph.referenceInk.isEmpty()) return false;
    const QPointF topLeft = viewQuad.points[0];
    const QPointF baseline = viewQuad.points[1] - topLeft;
    const qreal w = std::hypot(baseline.x(), baseline.y());
    if (!(w > 0.0) || !std::isfinite(w)) return false;
    planned.along = baseline / w;
    planned.across = QPointF(-planned.along.y(), planned.along.x());
    const qreal h = QPointF::dotProduct(viewQuad.points[3] - topLeft, planned.across);
    if (!(h > 0.0) || !std::isfinite(h)) return false;
    const QRectF& ink = glyph.referenceInk;
    planned.scaleY = glyph.heightRatio * h / ink.height();
    planned.scaleX = w / ink.width();
    const qreal base = glyph.baselineFraction * h;
    planned.origin = topLeft + planned.across * base - planned.along * (ink.left() * planned.scaleX);
    planned.inkTop = ink.top() * planned.scaleY;
    planned.inkBottom = (ink.top() + ink.height()) * planned.scaleY;
    // Map the local word frame to all four corners. Rotation alone loses
    // perspective/skew, including the exact affine projection of a tilted
    // source into a differently shaped viewport.
    QPolygonF unit, target;
    unit << QPointF(0, 0) << QPointF(w, 0) << QPointF(w, h) << QPointF(0, h);
    for (const auto& point : viewQuad.points) target << point;
    QTransform frame;
    if (!QTransform::quadToQuad(unit, target, frame)) return false;
    const auto at = [&](qreal u, qreal v) { return frame.map(QPointF(u, v)); };
    planned.original = viewQuad.points;
    planned.ink = {at(0, base + planned.inkTop), at(w, base + planned.inkTop),
                   at(w, base + planned.inkBottom), at(0, base + planned.inkBottom)};
    const qreal pad = h * 0.12;
    const qreal top = std::min<qreal>(0.0, base + planned.inkTop) - pad * 0.5;
    const qreal bottom = std::max<qreal>(h, base + planned.inkBottom) + pad * 0.5;
    planned.plate = {at(-pad, top), at(w + pad, top), at(w + pad, bottom), at(-pad, bottom)};
    planned.originalRect = PointBounds(planned.original);
    planned.inkRect = PointBounds(planned.ink);
    planned.plateRect = PointBounds(planned.plate);
    QPolygonF reference, inkTarget;
    reference << ink.topLeft() << ink.topRight() << ink.bottomRight() << ink.bottomLeft();
    for (const auto& point : planned.ink) inkTarget << point;
    if (!QTransform::quadToQuad(reference, inkTarget, planned.glyphToView)) return false;
    planned.angleDegrees = std::atan2(planned.along.y(), planned.along.x()) * 180.0 / 3.14159265358979323846;
    const auto finite = [](const QRectF& r) {
        return std::isfinite(r.x()) && std::isfinite(r.y()) && std::isfinite(r.width()) && std::isfinite(r.height());
    };
    // Size gates use the word's own height/width, not its rotated bound.
    if (!finite(planned.originalRect) || !finite(planned.inkRect) || !finite(planned.plateRect) ||
        !std::isfinite(planned.scaleX) || !std::isfinite(planned.scaleY)) return false;
    if (!(h >= kMinProjectedHeight && w >= kMinProjectedWidth &&
          planned.inkBottom - planned.inkTop >= kMinProjectedHeight)) {
        planned.omission = PaintOmission::kProjectedSize;
        return false;
    }
    planned.omission = PaintOmission::kNone;
    return true;
}

bool UltraClearTextOverlay::ComputePaintPlan(PaintPlan& plan, const ViewTransform& transform,
                                              const QSize& viewportSize, bool excludeChrome,
                                              const QDeadlineTimer* deadline)
{
    const QRect bounds(QPoint(0, 0), viewportSize);
    const int count = words_.size();
    plan.words.resize(count);
    plan.texts.resize(count);

    // Never paint over native controls floating above the viewport.
    QRegion clip(bounds);
    const auto exclude = [&](QWidget* widget) {
        if (!widget || !widget->isVisible()) {
            return;
        }
        const QPoint topLeft = mapFromGlobal(widget->mapToGlobal(QPoint(0, 0)));
        clip -= QRegion(QRect(topLeft, widget->size()));
    };
    if (excludeChrome) exclude(excludedWidget_);
    if (QWidget* owner = excludeChrome ? parentWidget() : nullptr) {
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
    // Rejected spans retain their original source ink. Exclusion applies to
    // both passes, including plate padding beyond an accepted word's box.
    QRegion& protectedInk = protectedInk_;
    protectedInk = QRegion();
    QSet<std::pair<const QRectF*, const QRectF*>> seenProtection;
    for (const SceneWord& word : words_) {
        if (deadline && deadline->hasExpired()) return false;
        const auto identity = std::pair{word.protectedSceneRects.constData(), word.carvedSceneRects.constData()};
        if (word.protectedSceneRects.isEmpty() || seenProtection.contains(identity))
            continue;
        seenProtection.insert(identity);
        const auto key = [](const QRectF& rect) {
            return std::array<double, 4>{rect.x(), rect.y(), rect.width(), rect.height()};
        };
        std::set<std::array<double, 4>> carved;
        for (const auto& rect : word.carvedSceneRects) {
            if (deadline && deadline->hasExpired()) return false;
            if (std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
                std::isfinite(rect.width()) && std::isfinite(rect.height())) carved.insert(key(rect));
        }
        for (const QRectF& scene : word.protectedSceneRects) {
            if (deadline && deadline->hasExpired()) return false;
            const QRectF view = uct::SceneRectToView(transform, viewportSize.width(), viewportSize.height(), scene);
            if (!view.isEmpty() && std::isfinite(view.x()) && std::isfinite(view.y()) &&
                std::isfinite(view.width()) && std::isfinite(view.height()))
                protectedInk += ProtectedRegion(view, bounds, carved.contains(key(scene)));
        }
    }

    // Decide eligibility once per word for this projection, then let both
    // passes consume that single decision: a plate is painted if and only
    // if its glyph is painted over it.
    for (int i = 0; i < count; ++i) {
        if (deadline && deadline->hasExpired()) return false;
        PlannedWord& planned = plan.words[i];
        const SceneGlyph& glyph = glyphs_[i];
        planned.lineId = words_[i].lineId;
        planned.wordIndex = words_[i].wordIndex;
        plan.texts[i] = words_[i].text;
        if (glyph.oriented) {
            // Rotated plate and glyph follow the projected word quad. The
            // plate is painted only together with its glyph, as upright.
            const bool shaped = PlanOriented(planned, glyph, uct::SceneQuadToView(
                transform, viewportSize.width(), viewportSize.height(), words_[i].sceneQuad));
            const QRectF surface(clip.boundingRect());
            planned.drawn = shaped && glyph.valid && !glyph.duplicate && !glyph.collision &&
                            clip.intersects(planned.plateRect.intersected(surface).toAlignedRect()) &&
                            clip.intersects(planned.inkRect.intersected(surface).toAlignedRect());
            planned.omission = planned.drawn ? PaintOmission::kNone :
                !glyph.valid ? PaintOmission::kInvalidGlyph :
                glyph.duplicate ? PaintOmission::kDuplicate :
                glyph.collision ? PaintOmission::kCollision :
                !shaped ? planned.omission : PaintOmission::kClipped;
            if (!planned.drawn && !glyph.duplicate && !planned.originalRect.isEmpty())
                protectedInk += PolygonRegion(planned.original, bounds);
            continue;
        }
        planned.oriented = false;
        const QRectF view = uct::SceneRectToView(transform, viewportSize.width(), viewportSize.height(), words_[i].sceneRect);
        const QRectF ink = uct::SceneRectToView(transform, viewportSize.width(), viewportSize.height(), glyph.inkRect);
        planned.originalRect = view;
        planned.inkRect = ink;
        planned.origin = uct::SceneToViewPixel(transform, viewportSize.width(), viewportSize.height(), glyph.origin);
        planned.scaleX = glyph.referenceInk.width() > 0.0 ? ink.width() / glyph.referenceInk.width() : 0.0;
        planned.scaleY = glyph.referenceInk.height() > 0.0 ? ink.height() / glyph.referenceInk.height() : 0.0;
        const qreal pad = view.height() * 0.12;
        planned.plateRect = view.united(ink).adjusted(-pad, -pad * 0.5, pad, pad * 0.5);
        planned.drawn = ReplacementIsDrawable(glyph.valid, glyph.referenceInk, view, ink,
                                              planned.plateRect, clip) &&
                        !glyph.duplicate && !glyph.collision;
        planned.omission = planned.drawn ? PaintOmission::kNone :
            !glyph.valid ? PaintOmission::kInvalidGlyph :
            glyph.duplicate ? PaintOmission::kDuplicate :
            glyph.collision ? PaintOmission::kCollision :
            (!clip.intersects(planned.plateRect.toAlignedRect()) ||
             !clip.intersects(ink.toAlignedRect())) ? PaintOmission::kClipped :
                PaintOmission::kGeometryOrSize;
        if (!planned.drawn && !glyph.duplicate && !view.isEmpty() && std::isfinite(view.x()) &&
            std::isfinite(view.y()) && std::isfinite(view.width()) && std::isfinite(view.height()))
            protectedInk += ProtectedRegion(view, bounds);
    }
    // A rotated word's bounding box covers its neighbours' corners. Test its
    // exact polygons, after a cheap bounding-box rejection.
    const auto touchesProtection = [&](const PlannedWord& planned) {
        const QRect original = PixelCoverage(planned.originalRect, bounds);
        const QRect ink = PixelCoverage(planned.inkRect, bounds);
        if (!planned.oriented) return protectedInk.intersects(original) || protectedInk.intersects(ink);
        if (!protectedInk.intersects(original) && !protectedInk.intersects(ink)) return false;
        return protectedInk.intersects(PolygonRegion(planned.original, bounds)) ||
               protectedInk.intersects(PolygonRegion(planned.ink, bounds));
    };
    const auto protectSource = [&](const PlannedWord& planned) {
        protectedInk += planned.oriented ? PolygonRegion(planned.original, bounds)
                                         : ProtectedRegion(planned.originalRect, bounds);
    };
    // Never show a clipped fragment of an accepted word over protected ink.
    // A suppressed replacement's own source box becomes protected too; resolve
    // overlap chains before painting any plate so draw order cannot erase it.
    bool changed;
    int protectionPasses = 0;
    do {
        changed = false;
        for (int i = 0; i < count; ++i) {
            if (deadline && deadline->hasExpired()) return false;
            PlannedWord& planned = plan.words[i];
            if (planned.drawn && touchesProtection(planned)) {
                planned.drawn = false;
                planned.omission = PaintOmission::kProtectedInk;
                protectSource(planned);
                changed = true;
            }
        }
    } while (changed && ++protectionPasses < 8);
    // Pathological overlapping detector boxes cannot make paint quadratic.
    // If the closure is still growing, this frame keeps all original pixels.
    if (changed) {
        for (PlannedWord& planned : plan.words) {
            if (planned.drawn) planned.omission = PaintOmission::kProtectionLimit;
            planned.drawn = false;
        }
    }
    plan.visibleClip = clip;
    plan.paintClip = clip - protectedInk;
    return !deadline || !deadline->hasExpired();
}

bool UltraClearTextOverlay::PreservesDrawnWords(QVector<SceneWord> proposed,
    const QVector<uct::OverlayModel::RegionalWordMatch>& matches, double remainingMs)
{
    if (QThread::currentThread() != thread() || !std::isfinite(remainingMs) || remainingMs <= 0 ||
        !PaintedGeometryCurrent() || plan_.font != referenceFont_ || proposed.size() > 8192 ||
        plan_.words.size() != words_.size() || plan_.texts.size() != words_.size()) return false;
    QDeadlineTimer deadline;
    deadline.setPreciseRemainingTime(0,static_cast<qint64>(std::min(remainingMs,3.0)*1e6),Qt::PreciseTimer);
    const auto mapping = [](const ViewTransform& t) {
        return std::array<float,9>{t.sourceX,t.sourceY,t.sourceWidth,t.sourceHeight,
            t.destinationX,t.destinationY,t.destinationWidth,t.destinationHeight,float(t.valid)};
    };
    if (mapping(plan_.transform) != mapping(transform_)) return false;
    using Key = std::pair<std::uint64_t,int>;
    QHash<Key,Key> replacements;
    for (const auto& match : matches) {
        if (deadline.hasExpired()) return false;
        if (replacements.contains({match.oldLineId,match.oldWordIndex})) return false;
        replacements.insert({match.oldLineId,match.oldWordIndex},{match.proposedLineId,match.proposedWordIndex});
    }
    QHash<Key,QString> required;
    for (int i = 0; i < words_.size(); ++i) {
        if (deadline.hasExpired()) return false;
        const auto& painted = plan_.words[i]; const auto& word = words_[i];
        if (painted.lineId != word.lineId || painted.wordIndex != word.wordIndex || plan_.texts[i] != word.text)
            return false; // a content assignment has not painted yet
        if (painted.oriented != word.sceneQuad.valid ||
            (word.sceneQuad.valid ? uct::SceneQuadToView(transform_,width(),height(),word.sceneQuad).points != painted.original
                                 : uct::SceneRectToView(transform_,width(),height(),word.sceneRect) != painted.originalRect))
            return false;
        if (!painted.drawn) continue;
        const auto match = replacements.constFind({painted.lineId,painted.wordIndex});
        if (match == replacements.cend()) return false;
        if (required.contains(match.value())) return false;
        required.insert(match.value(),word.text);
    }
    // The optional path never calls cold font measurement/shaping. A later
    // ordinary full result can populate missing typography without this guard.
    for (const auto& word : proposed)
        if (deadline.hasExpired() || !inkCache_.contains(word.text)) return false;
    auto savedWords = std::move(words_);
    auto savedGlyphs = std::move(glyphs_);
    auto savedProtection = std::move(protectedInk_);
    const auto restore = qScopeGuard([&] {
        words_ = std::move(savedWords); glyphs_ = std::move(savedGlyphs); protectedInk_ = std::move(savedProtection);
    });
    words_ = std::move(proposed);
    bool reuse = words_.size() == savedWords.size() && savedGlyphs.size() == savedWords.size();
    QHash<std::uint64_t,std::uint64_t> groups, reverseGroups;
    for (int i = 0; reuse && i < words_.size(); ++i) {
        if (deadline.hasExpired()) return false;
        const auto& old = savedWords[i]; const auto& word = words_[i];
        const auto group = word.layoutGroupId ? word.layoutGroupId : word.lineId;
        const auto oldGroup = old.layoutGroupId ? old.layoutGroupId : old.lineId;
        reuse = old.text == word.text && old.sceneRect == word.sceneRect && old.sceneQuad == word.sceneQuad &&
            replacements.value({old.lineId,old.wordIndex},Key{}) == Key(word.lineId,word.wordIndex) &&
            (!groups.contains(oldGroup) || groups.value(oldGroup) == group) &&
            (!reverseGroups.contains(group) || reverseGroups.value(group) == oldGroup);
        groups.insert(oldGroup,group); reverseGroups.insert(group,oldGroup);
    }
    if (reuse) {
        glyphs_ = savedGlyphs;
        for (int i = 0; i < glyphs_.size(); ++i) {
            if (deadline.hasExpired()) return false;
            glyphs_[i].colors = GlyphColors(words_[i].colors);
        }
    } else if (!RebuildGlyphLayout(&deadline)) return false;
    PaintPlan scratch;
    if (!ComputePaintPlan(scratch,transform_,size(),true,&deadline) || scratch.visibleClip != plan_.visibleClip)
        return false;
    for (int i = 0; i < scratch.words.size(); ++i) {
        if (deadline.hasExpired()) return false;
        const auto& word = scratch.words[i];
        const auto expected = required.constFind({word.lineId,word.wordIndex});
        if (expected == required.cend()) continue;
        if (!word.drawn || scratch.texts[i] != expected.value()) return false;
        required.erase(expected);
    }
    return required.isEmpty() && !deadline.hasExpired();
}

void UltraClearTextOverlay::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    ++paintCount_;
    // An offscreen QWidget::render (frame export, grab) repaints into a
    // redirected device other than the window backing store. Those pixels
    // are not on screen, so such a paint must not become the painted-plan
    // authority: draw with a scratch plan and restore the on-screen one.
    const QPaintDevice* device = redirected(nullptr);
    QBackingStore* store = backingStore();
    const bool offscreenPaint = device && store && device != store->paintDevice();
    PaintPlan savedPlan;
    if (offscreenPaint) {
        savedPlan = plan_;
    }
    plan_.valid = false;
    if (words_.isEmpty() || glyphs_.size() != words_.size() || !transform_.valid) {
        if (offscreenPaint) {
            plan_ = std::move(savedPlan);
        }
        return;
    }
    ComputePaintPlan(plan_, transform_, size(), true);
    const QFont paintFont = referenceFont_;
    QPainter painter(this);
    PaintWords(painter, plan_);
    if (offscreenPaint) {
        plan_ = std::move(savedPlan);
        return;
    }
    // The plan now describes exactly these pixels.
    plan_.transform = transform_;
    plan_.font = paintFont;
    plan_.size = size();
    plan_.devicePixelRatio = devicePixelRatioF();
    plan_.screen = screen();
    ++plan_.generation;
    plan_.valid = true;
    // A completed software paint plan, not a scanout/photon receipt. Export
    // paints returned above and cannot satisfy this first-content-paint event.
    if (uct::timing::Enabled() && timingTicket_.admission.producer &&
        timingTicket_.revision != timingPaintedRevision_) {
        timingPaintedRevision_ = timingTicket_.revision;
        uct::timing::RecordEvent(uct::timing::Event::OverlayPainted, timingTicket_.admission,
            {0, static_cast<std::uint32_t>(words_.size()), timingTicket_.flags, 0, timingTicket_.revision});
    }
}


void UltraClearTextOverlay::PaintWords(QPainter& painter, const PaintPlan& plan) const
{
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(plan.paintClip);
    // Paint all plates before any glyphs so a neighbouring word's plate
    // cannot erase an overhang or descender already drawn.
    for (int i = 0; i < words_.size(); ++i) {
        const PlannedWord& planned = plan.words[i];
        if (!planned.drawn) continue;
        if (planned.oriented) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(glyphs_[i].colors.background);
            painter.drawConvexPolygon(planned.plate.data(), static_cast<int>(planned.plate.size()));
        } else {
            painter.fillRect(planned.plateRect, glyphs_[i].colors.background);
        }
    }
    painter.setBrush(Qt::NoBrush);
    for (int i = 0; i < words_.size(); ++i) {
        const PlannedWord& planned = plan.words[i];
        if (!planned.drawn) {
            continue;
        }
        painter.save();
        if (planned.oriented) painter.setWorldTransform(planned.glyphToView, true);
        else {
            painter.translate(planned.origin);
            painter.scale(planned.scaleX, planned.scaleY);
        }
        painter.setFont(referenceFont_);
        painter.setPen(glyphs_[i].colors.foreground);
        painter.drawText(QPointF(0.0, 0.0), words_[i].text);
        painter.restore();
    }
}

UctOverlaySnapshot UltraClearTextOverlay::CaptureSnapshot(const ViewTransform& transform,
                                                          const QSize& viewportSize)
{
    UctOverlaySnapshot snapshot;
    if (!isVisible() || words_.isEmpty() || glyphs_.size() != words_.size() ||
        !transform.valid || viewportSize.isEmpty()) return snapshot;
    PaintPlan capturePlan;
    ComputePaintPlan(capturePlan, transform, viewportSize, false);
    if (std::none_of(capturePlan.words.cbegin(), capturePlan.words.cend(),
                     [](const PlannedWord& word) { return word.drawn; })) return snapshot;
    snapshot.viewportSize = viewportSize;
    QPainter recorder(&snapshot.picture);
    PaintWords(recorder, capturePlan);
    recorder.end();
    return snapshot;
}

void UctOverlaySnapshot::Paint(QPainter& painter, const QSize& imageSize) const
{
    if (IsEmpty() || imageSize.isEmpty()) return;
    painter.save();
    painter.setClipRect(QRect(QPoint(0, 0), imageSize), Qt::IntersectClip);
    painter.scale(qreal(imageSize.width()) / viewportSize.width(),
                  qreal(imageSize.height()) / viewportSize.height());
    painter.drawPicture(QPointF(0.0, 0.0), picture);
    painter.restore();
}

bool UltraClearTextOverlay::PaintedGeometryCurrent() const
{
    if (!plan_.valid || !isVisible() || !renderTarget_ || plan_.size != size() ||
        plan_.screen != screen() || plan_.devicePixelRatio != devicePixelRatioF()) {
        return false;
    }
    // The invariant accessibility mapping needs: overlay-local coordinates
    // are render-target-local coordinates. Compare both client origins in
    // the same global space; a window-with-parent's geometry() may be
    // frame-inclusive or parent-relative depending on the platform, while
    // mapToGlobal() is the mapping the accessibility layer itself applies.
    return size() == renderTarget_->size() &&
           mapToGlobal(QPoint(0, 0)) == renderTarget_->mapToGlobal(QPoint(0, 0));
}

const QVector<qreal>& UltraClearTextOverlay::CaretPositions(const QString& text, const QFont& font) const
{
    // The cache key carries the font: carets shaped for one stroke weight
    // must never answer for pixels painted with another, whichever order
    // queries and SetStrokeWeight calls arrive in.
    const QString key = text + QLatin1Char('\0') + font.key();
    const auto it = caretCache_.constFind(key);
    if (it != caretCache_.constEnd()) {
        return it.value();
    }
    // Same font, shaping and font fallback as drawText(QPointF, text) at the
    // glyph origin: one unwrapped line starting at x = 0.
    QVector<qreal> carets(text.size() + 1, std::numeric_limits<qreal>::quiet_NaN());
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setPosition(QPointF(0.0, 0.0));
    }
    layout.endLayout();
    if (line.isValid()) {
        for (int i = 0; i <= text.size(); ++i) {
            if (i == 0 || i == text.size() || layout.isValidCursorPosition(i)) {
                carets[i] = line.cursorToX(i);
            }
        }
    }
    return caretCache_.insert(key, std::move(carets)).value();
}

bool UltraClearTextOverlay::PaintedWord(std::uint64_t lineId, int wordIndex, const QString& text,
                                        PaintedWordGeometry& out) const
{
    out = {};
    if (!plan_.valid || text.isEmpty()) {
        return false;
    }
    if (planIndexGeneration_ != plan_.generation) {
        planIndex_.clear();
        for (int i = 0; i < plan_.words.size(); ++i) {
            planIndex_.insert({plan_.words[i].lineId, plan_.words[i].wordIndex}, i);
        }
        planIndexGeneration_ = plan_.generation;
    }
    const auto found = planIndex_.constFind({lineId, wordIndex});
    if (found == planIndex_.constEnd() || plan_.texts[found.value()] != text) {
        return false;
    }
    const PlannedWord& planned = plan_.words[found.value()];
    out.drawn = planned.drawn;
    out.omission = planned.omission;
    out.box = planned.drawn ? planned.inkRect : planned.originalRect;
    const QVector<qreal>& carets = CaretPositions(text, plan_.font);
    // Visual caret extent over every shaped position. For right-to-left
    // text the first logical caret sits at the visual maximum, so
    // normalizing against the endpoint distance (abs(caret - first))
    // would mirror the word; normalize against the visual minimum.
    qreal visualMin = std::numeric_limits<qreal>::max();
    qreal visualMax = std::numeric_limits<qreal>::lowest();
    for (qreal caret : carets) {
        if (!std::isfinite(caret)) {
            continue;
        }
        visualMin = std::min(visualMin, caret);
        visualMax = std::max(visualMax, caret);
    }
    const qreal extent = visualMax >= visualMin ? visualMax - visualMin : 0.0;
    const int units = static_cast<int>(text.size());
    if (planned.oriented) {
        // Characters follow the rotated baseline; each cell reports its
        // screen bound, as the accessibility interface carries rectangles.
        const auto fraction = [&](int position) {
            return extent <= 0.0 ? qreal(position) / units : (carets[position] - visualMin) / extent;
        };
        const auto edge = [](const std::array<QPointF, 4>& q, qreal f, bool bottom) {
            return bottom ? q[3] + (q[2] - q[3]) * f : q[0] + (q[1] - q[0]) * f;
        };
        const auto cell = [&](int a, int b) {
            if (planned.drawn && extent > 0.0) {
                const auto& ink = glyphs_[found.value()].referenceInk;
                const auto at = [&](qreal u, qreal v) { return planned.glyphToView.map(QPointF(u, v)); };
                return PointBounds({at(carets[a], ink.top()), at(carets[b], ink.top()),
                                    at(carets[b], ink.bottom()), at(carets[a], ink.bottom())});
            }
            const auto& q = planned.drawn ? planned.ink : planned.original;
            return PointBounds({edge(q, fraction(a), false), edge(q, fraction(b), false),
                                edge(q, fraction(b), true), edge(q, fraction(a), true)});
        };
        out.characters.resize(units);
        int clusterStart = 0;
        for (int i = 1; i <= units; ++i) {
            if (i < units && !std::isfinite(carets[i])) continue;
            const QRectF bounds = cell(clusterStart, i);
            for (int unit = clusterStart; unit < i; ++unit) out.characters[unit] = bounds;
            clusterStart = i;
        }
        return true;
    }
    const auto toX = [&](int position) {
        if (extent <= 0.0) {
            // No measurable advance: equal shares of the painted box.
            return out.box.left() + out.box.width() * position / units;
        }
        if (planned.drawn) {
            return planned.origin.x() + planned.scaleX * carets[position];
        }
        // Original ink: split the source box in proportion to the shaped
        // advances, measured from the visual (left) edge.
        return out.box.left() + out.box.width() * ((carets[position] - visualMin) / extent);
    };
    out.characters.resize(units);
    int clusterStart = 0;
    for (int i = 1; i <= units; ++i) {
        if (i < units && !std::isfinite(carets[i])) {
            continue; // inside a cluster: surrogate pair or combining sequence
        }
        const qreal a = toX(clusterStart);
        const qreal b = toX(i);
        const QRectF cell(QPointF(std::min(a, b), out.box.top()), QPointF(std::max(a, b), out.box.bottom()));
        for (int unit = clusterStart; unit < i; ++unit) {
            out.characters[unit] = cell;
        }
        clusterStart = i;
    }
    return true;
}

QRectF UltraClearTextOverlay::PaintedSceneRect(const QRectF& sceneRect) const
{
    if (!plan_.valid) {
        return {};
    }
    return uct::SceneRectToView(plan_.transform, plan_.size.width(), plan_.size.height(), sceneRect);
}

QRectF UltraClearTextOverlay::VisiblePart(const QRectF& local, bool replacement) const
{
    if (!plan_.valid || local.isEmpty() || !std::isfinite(local.x()) || !std::isfinite(local.y()) ||
        !std::isfinite(local.width()) || !std::isfinite(local.height())) {
        return {};
    }
    const QRegion& clip = replacement ? plan_.paintClip : plan_.visibleClip;
    const QRectF bounded = local.intersected(QRectF(QPointF(0.0, 0.0), QSizeF(plan_.size)));
    if (bounded.isEmpty()) {
        return {};
    }
    const QRegion visible = clip.intersected(bounded.toAlignedRect());
    if (visible.isEmpty()) {
        return {};
    }
    // Sub-pixel extents survive wherever the clip does not cut the rect.
    return bounded.intersected(QRectF(visible.boundingRect()));
}

} // namespace okuflow

#endif // _WIN32
