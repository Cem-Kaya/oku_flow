#include "okuflow/ui/uct_screen_text_accessible.hpp"

#if defined(_WIN32)

#include "okuflow/ui/ui_translation.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"

#include <QAccessibleWidget>
#include <QCoreApplication>
#include <QHash>
#include <QLocale>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace okuflow {
namespace {

// Appends the AccessibleVirtualChildren elements of a marked host after its
// ordinary widget children. Everything else is QAccessibleWidget behaviour.
class VirtualChildHostAccessible final : public QAccessibleWidget {
public:
    explicit VirtualChildHostAccessible(QWidget* widget) : QAccessibleWidget(widget, QAccessible::Client) {}

    int childCount() const override
    {
        const AccessibleVirtualChildren* provider = Provider();
        return QAccessibleWidget::childCount() + (provider ? provider->VirtualChildCount() : 0);
    }
    QAccessibleInterface* child(int index) const override
    {
        const int widgets = QAccessibleWidget::childCount();
        if (index < widgets) return QAccessibleWidget::child(index);
        const AccessibleVirtualChildren* provider = Provider();
        return provider ? provider->VirtualChild(index - widgets) : nullptr;
    }
    int indexOfChild(const QAccessibleInterface* child) const override
    {
        const int widgetIndex = QAccessibleWidget::indexOfChild(child);
        if (widgetIndex >= 0) return widgetIndex;
        const AccessibleVirtualChildren* provider = Provider();
        const int virtualIndex = provider ? provider->IndexOfVirtualChild(child) : -1;
        return virtualIndex < 0 ? -1 : QAccessibleWidget::childCount() + virtualIndex;
    }
    QAccessibleInterface* childAt(int x, int y) const override
    {
        // Widget children float above the viewport; they win hit testing.
        const int widgets = QAccessibleWidget::childCount();
        for (int i = 0; i < widgets; ++i) {
            QAccessibleInterface* candidate = QAccessibleWidget::child(i);
            if (candidate && candidate->isValid() && candidate->rect().contains(x, y)) return candidate;
        }
        const AccessibleVirtualChildren* provider = Provider();
        return provider ? provider->VirtualChildAt(x, y) : nullptr;
    }

private:
    AccessibleVirtualChildren* Provider() const
    {
        QWidget* host = widget();
        return host ? host->findChild<AccessibleVirtualChildren*>(QString(), Qt::FindDirectChildrenOnly)
                    : nullptr;
    }
};

QAccessibleInterface* VirtualChildHostFactory(const QString& className, QObject* object)
{
    if (!object || !object->isWidgetType() ||
        className != QLatin1StringView(object->metaObject()->className()) ||
        !object->property(kAccessibleVirtualChildHostProperty).toBool()) {
        return nullptr; // only the most derived class of a marked widget
    }
    return new VirtualChildHostAccessible(static_cast<QWidget*>(object));
}

bool EnsureVirtualChildHostFactory()
{
    static const bool installed = [] {
        QAccessible::installFactory(&VirtualChildHostFactory);
        return true;
    }();
    return installed;
}

// Installed before any window exists, so a RenderWidget queried before the
// UCT controller is constructed still gets the host interface.
[[maybe_unused]] const bool kHostFactoryInstalled = EnsureVirtualChildHostFactory();

double Iou(const QRectF& a, const QRectF& b)
{
    const QRectF overlap = a.intersected(b);
    if (overlap.isEmpty()) return 0.0;
    const double shared = overlap.width() * overlap.height();
    const double total = a.width() * a.height() + b.width() * b.height() - shared;
    return total > 0.0 ? shared / total : 0.0;
}

bool Finite(const QRectF& r)
{
    return std::isfinite(r.x()) && std::isfinite(r.y()) && std::isfinite(r.width()) &&
           std::isfinite(r.height());
}

bool Positioned(const QRectF& r)
{
    return Finite(r) && (r.width() > 0.0 || r.height() > 0.0);
}

QWindow* HostWindow(QWidget* host)
{
    QAccessibleInterface* iface = host ? QAccessible::queryAccessibleInterface(host) : nullptr;
    return iface ? iface->window() : nullptr;
}

} // namespace

void MarkAccessibleVirtualChildHost(QWidget* host)
{
    EnsureVirtualChildHostFactory();
    if (host) host->setProperty(kAccessibleVirtualChildHostProperty, true);
}

AccessibleVirtualChildren::AccessibleVirtualChildren(QWidget* host) : QObject(host)
{
    MarkAccessibleVirtualChildHost(host);
}

QWidget* AccessibleVirtualChildren::Host() const
{
    return qobject_cast<QWidget*>(parent());
}

// --- Line element ------------------------------------------------------------

class UctScreenLineAccessible final : public QAccessibleInterface,
                                      public QAccessibleTextInterface,
                                      public QAccessibleAttributesInterface {
public:
    UctScreenLineAccessible(UctScreenTextAccessibility* owner, uct::ScreenLine content)
        : owner_(owner), content_(std::move(content))
    {
    }

    std::uint64_t LineId() const noexcept { return content_.lineId; }
    const QString& Text() const noexcept { return content_.text; }
    const QRectF& SceneRect() const noexcept { return content_.sceneRect; }
    // Same text by contract (identity kept or rebound); geometry may move.
    void SetContent(uct::ScreenLine content)
    {
        content_ = std::move(content);
        geometry_ = {};
    }

    // QAccessibleInterface
    bool isValid() const override { return !owner_.isNull(); }
    QObject* object() const override { return nullptr; }
    QWindow* window() const override { return owner_ ? HostWindow(owner_->Host()) : nullptr; }
    QAccessibleInterface* childAt(int, int) const override { return nullptr; }
    QAccessibleInterface* parent() const override { return owner_ ? owner_->RootInterface() : nullptr; }
    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    QString text(QAccessible::Text type) const override
    {
        Note();
        return type == QAccessible::Name ? content_.text : QString();
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override
    {
        Note();
        const Geometry& geometry = LocalGeometry();
        if (owner_ && !geometry.bounds.isEmpty()) {
            const QRect viewport = owner_->ViewportRect();
            const QRect bounds =
                geometry.bounds.translated(viewport.topLeft()).toAlignedRect().intersected(viewport);
            if (!bounds.isEmpty()) return bounds;
        }
        return Fallback(geometry.lineBox);
    }
    QAccessible::Role role() const override { return QAccessible::StaticText; }
    QAccessible::State state() const override
    {
        QAccessible::State state;
        state.readOnly = true;
        state.offscreen = LocalGeometry().bounds.isEmpty();
        return state;
    }
    void* interface_cast(QAccessible::InterfaceType type) override
    {
        if (type == QAccessible::TextInterface) return static_cast<QAccessibleTextInterface*>(this);
        if (type == QAccessible::AttributesInterface)
            return static_cast<QAccessibleAttributesInterface*>(this);
        return nullptr;
    }

    // QAccessibleTextInterface: read-only, no caret, no selection.
    void selection(int, int* startOffset, int* endOffset) const override
    {
        if (startOffset) *startOffset = 0;
        if (endOffset) *endOffset = 0;
    }
    int selectionCount() const override { return 0; }
    void addSelection(int, int) override {}
    void removeSelection(int) override {}
    void setSelection(int, int, int) override {}
    int cursorPosition() const override { return 0; }
    void setCursorPosition(int) override {}
    QString text(int startOffset, int endOffset) const override
    {
        Note();
        const int size = static_cast<int>(content_.text.size());
        const int start = std::clamp(startOffset, 0, size);
        const int end = std::clamp(endOffset < 0 ? size : endOffset, start, size);
        return content_.text.mid(start, end - start);
    }
    int characterCount() const override
    {
        Note();
        return static_cast<int>(content_.text.size());
    }
    QRect characterRect(int offset) const override
    {
        Note();
        const Geometry& geometry = LocalGeometry();
        if (offset < 0 || offset >= geometry.visible.size()) return Fallback(geometry.lineBox);
        const QRectF& visible = geometry.visible[offset];
        if (owner_ && !visible.isEmpty()) {
            const QRect viewport = owner_->ViewportRect();
            const QRect rect = visible.translated(viewport.topLeft()).toAlignedRect().intersected(viewport);
            if (!rect.isEmpty()) return rect;
        }
        // Hidden: the run-shared anchor, never this unit's own raw centre.
        return Fallback(offset < geometry.fallback.size() ? geometry.fallback[offset]
                                                           : geometry.lineBox);
    }
    int offsetAtPoint(const QPoint& point) const override
    {
        Note();
        const Geometry& geometry = LocalGeometry();
        if (!owner_) return -1;
        const QPointF local = QPointF(point - owner_->ViewportRect().topLeft()) + QPointF(0.5, 0.5);
        for (int i = 0; i < geometry.visible.size(); ++i) {
            if (!geometry.visible[i].isEmpty() && geometry.visible[i].contains(local)) return i;
        }
        return -1;
    }
    // A screen reader must never pan or zoom the camera view.
    void scrollToSubstring(int, int) override {}
    QString attributes(int, int* startOffset, int* endOffset) const override
    {
        if (startOffset) *startOffset = 0;
        if (endOffset) *endOffset = static_cast<int>(content_.text.size());
        return QString();
    }

    // QAccessibleAttributesInterface
    QList<QAccessible::Attribute> attributeKeys() const override
    {
        if (!owner_ || owner_->languageTag_.isEmpty()) return {};
        return {QAccessible::Attribute::Locale};
    }
    QVariant attributeValue(QAccessible::Attribute key) const override
    {
        if (key != QAccessible::Attribute::Locale || !owner_ || owner_->languageTag_.isEmpty()) return {};
        return QVariant::fromValue(QLocale(owner_->languageTag_));
    }

private:
    // Overlay-local geometry of the last paint, rebuilt only when a query
    // follows a new paint plan. Never computed while nobody asks.
    struct Geometry {
        bool built{};
        bool current{};
        std::uint64_t generation{};
        QVector<QRectF> visible; // per UTF-16 unit: visible part, empty if hidden
        QVector<QRectF> raw;     // per UTF-16 unit: unclipped position hint
        // Per UTF-16 unit: fallback hint for hidden units. Every unit of
        // one contiguous hidden run shares the same hint (see Fallback use
        // in characterRect), so Qt's UIA text-range provider, which builds
        // a range rect from the first and last character rects alone and
        // ignores IsOffscreen/empty, yields zero area for a fully hidden
        // line or run instead of spanning distinct raw centres.
        QVector<QRectF> fallback;
        QRectF lineBox;          // projected line box (fallback position hint)
        QRectF bounds;           // union of visible
    };

    void Note() const
    {
        if (owner_) owner_->NoteQuery();
    }

    const Geometry& LocalGeometry() const
    {
        UltraClearTextOverlay* overlay = owner_ ? owner_->overlay_.data() : nullptr;
        const bool current = overlay && overlay->PaintedGeometryCurrent();
        const std::uint64_t generation = overlay ? overlay->PaintedGeneration() : 0;
        if (geometry_.built && geometry_.current == current && geometry_.generation == generation)
            return geometry_;
        Geometry geometry;
        geometry.built = true;
        geometry.current = current;
        geometry.generation = generation;
        const int units = static_cast<int>(content_.text.size());
        geometry.visible.resize(units);
        geometry.raw.resize(units);
        geometry.fallback.resize(units);
        if (overlay && generation != 0) {
            const QRectF lineBox = overlay->PaintedSceneRect(content_.sceneRect);
            if (Finite(lineBox)) geometry.lineBox = lineBox;
        }
        // Not current: the whole line is one hidden run on the line box.
        for (QRectF& hint : geometry.fallback) hint = geometry.lineBox;
        if (!current) {
            geometry_ = std::move(geometry);
            return geometry_;
        }
        const QVector<uct::ScreenToken>& tokens = content_.tokens;
        QVector<QRectF> boxes(tokens.size());
        const auto inside = [units](const uct::ScreenToken& token) {
            return token.start >= 0 && token.length > 0 && token.start + token.length <= units;
        };
        // 1) Verified glyph words, exactly as last painted.
        for (int j = 0; j < tokens.size(); ++j) {
            const uct::ScreenToken& token = tokens[j];
            if (token.wordIndex < 0 || !inside(token)) continue;
            UltraClearTextOverlay::PaintedWordGeometry word;
            if (!overlay->PaintedWord(content_.lineId, token.wordIndex,
                                      content_.text.mid(token.start, token.length), word) ||
                word.characters.size() != token.length || !Finite(word.box)) {
                continue; // not part of the last paint: hidden, never guessed
            }
            for (int u = 0; u < token.length; ++u) {
                geometry.raw[token.start + u] = word.characters[u];
                geometry.visible[token.start + u] = overlay->VisiblePart(word.characters[u], word.drawn);
            }
            boxes[j] = word.box;
        }
        // Reading direction from the painted boxes themselves: on a
        // right-to-left line token order runs visually right to left, so
        // "between neighbours" must not assume the next word is rightward.
        const QRectF* firstBox = nullptr;
        const QRectF* lastBox = nullptr;
        for (const QRectF& box : boxes) {
            if (!Positioned(box)) continue;
            if (!firstBox) firstBox = &box;
            lastBox = &box;
        }
        const bool descending = firstBox && lastBox && firstBox != lastBox &&
                                firstBox->center().x() > lastBox->center().x();
        // Horizontal gap between two boxes, whichever is leftmost.
        const auto gapBetween = [](const QRectF& a, const QRectF& b, qreal& left, qreal& right) {
            left = std::min(a.right(), b.right());
            right = std::max(a.left(), b.left());
        };
        // 2) "…" runs: the original ink between their neighbouring words.
        for (int j = 0; j < tokens.size(); ++j) {
            const uct::ScreenToken& token = tokens[j];
            if (token.wordIndex >= 0 || !inside(token)) continue;
            const QRectF* before = nullptr;
            const QRectF* after = nullptr;
            for (int k = j - 1; k >= 0 && !before; --k)
                if (Positioned(boxes[k])) before = &boxes[k];
            for (int k = j + 1; k < tokens.size() && !after; ++k)
                if (Positioned(boxes[k])) after = &boxes[k];
            const bool haveLine = Positioned(geometry.lineBox);
            if (!haveLine && !before && !after) continue;
            qreal left = 0.0;
            qreal right = 0.0;
            if (before && after) {
                gapBetween(*before, *after, left, right);
            } else if (before) {
                // The run continues from `before` in reading direction to
                // the far line edge (leftward on a descending line).
                if (descending) {
                    left = haveLine ? geometry.lineBox.left() : before->left();
                    right = before->left();
                } else {
                    left = before->right();
                    right = haveLine ? geometry.lineBox.right() : before->right();
                }
            } else if (after) {
                if (descending) {
                    left = after->right();
                    right = haveLine ? geometry.lineBox.right() : after->right();
                } else {
                    left = haveLine ? geometry.lineBox.left() : after->left();
                    right = after->left();
                }
            } else {
                left = geometry.lineBox.left();
                right = geometry.lineBox.right();
            }
            qreal top = haveLine ? geometry.lineBox.top() : std::numeric_limits<qreal>::max();
            qreal bottom = haveLine ? geometry.lineBox.bottom() : std::numeric_limits<qreal>::lowest();
            if (!haveLine) {
                for (const QRectF* neighbour : {before, after}) {
                    if (!neighbour) continue;
                    top = std::min(top, neighbour->top());
                    bottom = std::max(bottom, neighbour->bottom());
                }
            }
            const QRectF run(QPointF(left, top), QPointF(std::max(left, right), bottom));
            boxes[j] = run;
            for (int u = 0; u < token.length; ++u) {
                geometry.raw[token.start + u] = run;
                geometry.visible[token.start + u] = overlay->VisiblePart(run, false);
            }
        }
        // 3) Single separating spaces cover the gap between neighbours.
        for (int j = 0; j + 1 < tokens.size(); ++j) {
            const int space = tokens[j].start + tokens[j].length;
            if (space < 0 || space >= units || !Positioned(boxes[j]) || !Positioned(boxes[j + 1])) continue;
            const QRectF& a = boxes[j];
            const QRectF& b = boxes[j + 1];
            qreal gapLeft = 0.0;
            qreal gapRight = 0.0;
            gapBetween(a, b, gapLeft, gapRight);
            const QRectF gap(QPointF(gapLeft, std::min(a.top(), b.top())),
                             QPointF(std::max(gapLeft, gapRight), std::max(a.bottom(), b.bottom())));
            geometry.raw[space] = gap;
            geometry.visible[space] = overlay->VisiblePart(gap, false);
        }
        for (const QRectF& visible : geometry.visible)
            if (!visible.isEmpty()) geometry.bounds = geometry.bounds.united(visible);
        // 4) One shared anchor per contiguous hidden run: the first
        // positioned raw rect of the run, else the line box. Distinct
        // per-character centres would let Qt's endpoint-min/max UIA range
        // rect claim area for text that is entirely hidden.
        for (int u = 0; u < units;) {
            if (!geometry.visible[u].isEmpty()) {
                ++u;
                continue;
            }
            int end = u;
            QRectF anchor;
            while (end < units && geometry.visible[end].isEmpty()) {
                if (!Positioned(anchor) && Positioned(geometry.raw[end])) anchor = geometry.raw[end];
                ++end;
            }
            if (!Positioned(anchor)) anchor = geometry.lineBox;
            for (int k = u; k < end; ++k) geometry.fallback[k] = anchor;
            u = end;
        }
        geometry_ = std::move(geometry);
        return geometry_;
    }

    // Never QRect() and never outside the viewport: a zero-size rect at the
    // viewport point nearest the text, so union-based highlights ignore it.
    QRect Fallback(const QRectF& localHint) const
    {
        if (!owner_) return QRect(QPoint(0, 0), QSize(0, 0));
        const QRect viewport = owner_->ViewportRect();
        QPointF centre = Positioned(localHint) ? localHint.center() + QPointF(viewport.topLeft())
                                               : QPointF(viewport.center());
        const qreal x = std::clamp<qreal>(centre.x(), viewport.left(), viewport.left() + std::max(0, viewport.width()));
        const qreal y = std::clamp<qreal>(centre.y(), viewport.top(), viewport.top() + std::max(0, viewport.height()));
        return QRect(QPoint(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y))), QSize(0, 0));
    }

    QPointer<UctScreenTextAccessibility> owner_;
    uct::ScreenLine content_;
    mutable Geometry geometry_;
};

// --- Root element --------------------------------------------------------------

class UctScreenTextRootAccessible final : public QAccessibleInterface {
public:
    explicit UctScreenTextRootAccessible(UctScreenTextAccessibility* owner) : owner_(owner) {}

    bool isValid() const override { return owner_ && owner_->root_ == this; }
    QObject* object() const override { return nullptr; }
    QWindow* window() const override { return owner_ ? HostWindow(owner_->Host()) : nullptr; }
    QAccessibleInterface* childAt(int x, int y) const override
    {
        if (!owner_) return nullptr;
        // Prefer the line whose painted text is under the point.
        for (const auto& line : owner_->lines_) {
            if (line.iface->offsetAtPoint(QPoint(x, y)) >= 0) return line.iface;
        }
        for (const auto& line : owner_->lines_) {
            const QRect rect = line.iface->rect();
            if (!rect.isEmpty() && rect.contains(x, y)) return line.iface;
        }
        return nullptr;
    }
    QAccessibleInterface* parent() const override
    {
        return owner_ && owner_->Host() ? QAccessible::queryAccessibleInterface(owner_->Host()) : nullptr;
    }
    QAccessibleInterface* child(int index) const override
    {
        if (!owner_ || index < 0 || index >= owner_->lines_.size()) return nullptr;
        return owner_->lines_[index].iface;
    }
    int childCount() const override { return owner_ ? static_cast<int>(owner_->lines_.size()) : 0; }
    int indexOfChild(const QAccessibleInterface* child) const override
    {
        if (!owner_ || !child) return -1;
        for (int i = 0; i < owner_->lines_.size(); ++i)
            if (owner_->lines_[i].iface == child) return i;
        return -1;
    }
    QString text(QAccessible::Text type) const override
    {
        if (type == QAccessible::Name) return TranslateUi(UctScreenTextAccessibility::RootNameSource());
        if (type == QAccessible::Description)
            return TranslateUi(UctScreenTextAccessibility::RootDescriptionSource());
        return QString();
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override
    {
        if (!owner_) return QRect(QPoint(0, 0), QSize(0, 0));
        return owner_->ViewportRect();
    }
    QAccessible::Role role() const override { return QAccessible::Grouping; }
    QAccessible::State state() const override
    {
        QAccessible::State state;
        state.readOnly = true;
        QWidget* host = owner_ ? owner_->Host() : nullptr;
        if (!host || !host->isVisible()) {
            state.invisible = true;
            state.offscreen = true;
        }
        return state;
    }

private:
    QPointer<UctScreenTextAccessibility> owner_;
};

// --- Owner ------------------------------------------------------------------------

UctScreenTextAccessibility::UctScreenTextAccessibility(UltraClearTextOverlay* overlay, QWidget* host,
                                                       SnapshotProvider provider, Clock clock,
                                                       UctScreenTextTiming timing)
    : AccessibleVirtualChildren(host),
      overlay_(overlay),
      provider_(std::move(provider)),
      clock_(clock ? std::move(clock) : Clock([] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
              .count();
      })),
      timing_(timing)
{
    setObjectName(QStringLiteral("uctScreenTextAccessibility"));
    cadence_ = new QTimer(this);
    cadence_->setSingleShot(true);
    connect(cadence_, &QTimer::timeout, this, &UctScreenTextAccessibility::ProcessDueWork);
    active_ = QAccessible::isActive();
    QAccessible::installActivationObserver(this);
}

UctScreenTextAccessibility::~UctScreenTextAccessibility()
{
    QAccessible::removeActivationObserver(this);
    DestroyAll();
}

QString UctScreenTextAccessibility::RootNameSource()
{
    return QStringLiteral("Ultra Clear Text on this view");
}

QString UctScreenTextAccessibility::RootDescriptionSource()
{
    return QStringLiteral("\u2026 marks text left as the original image.");
}

void UctScreenTextAccessibility::accessibilityActiveChanged(bool active)
{
    if (active_ == active) return;
    active_ = active;
    if (!active_) {
        DestroyAll(); // nothing is kept while no client listens
        return;
    }
    if (enabled_) {
        EnsureRoot();
        publishImmediately_ = true;
        Sync(false);
    }
}

void UctScreenTextAccessibility::SetEnabled(bool enabled)
{
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) {
        DestroyAll();
        return;
    }
    if (!active_) return;
    EnsureRoot();
    publishImmediately_ = true;
    Sync(false);
}

void UctScreenTextAccessibility::ContentChanged()
{
    if (!active_ || !enabled_) return;
    Sync(false);
}

void UctScreenTextAccessibility::RevokeAll()
{
    publishImmediately_ = true;
    if (lines_.isEmpty() && !pending_) return; // per-frame clear path stays trivial
    RemoveAllLines();
    pending_ = false;
    cadence_->stop();
}

void UctScreenTextAccessibility::ProcessDueWork()
{
    if (!active_ || !enabled_ || !pending_) return;
    if (clock_() < DueMs()) {
        cadence_->stop();
        ScheduleTick();
        return;
    }
    Sync(true);
}

QAccessibleInterface* UctScreenTextAccessibility::RootInterface() const
{
    return root_;
}

int UctScreenTextAccessibility::VirtualChildCount() const
{
    return root_ ? 1 : 0;
}

QAccessibleInterface* UctScreenTextAccessibility::VirtualChild(int index) const
{
    return index == 0 ? root_ : nullptr;
}

int UctScreenTextAccessibility::IndexOfVirtualChild(const QAccessibleInterface* child) const
{
    return root_ && child == root_ ? 0 : -1;
}

QAccessibleInterface* UctScreenTextAccessibility::VirtualChildAt(int x, int y) const
{
    // Only claim the point when painted text is there; otherwise the viewport
    // itself stays the element under the pointer.
    return root_ && root_->childAt(x, y) ? root_ : nullptr;
}

void UctScreenTextAccessibility::Sync(bool publishAdditions)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!active_ || !enabled_ || !root_) return;
    ++snapshots_;
    UctScreenTextSnapshot snapshot = provider_ ? provider_() : UctScreenTextSnapshot{};
    if (!snapshot.enabled) {
        RevokeAll();
        return;
    }
    // Defensive bounds; the provider already applies the same limits.
    const uct::ScreenTextLimits limits;
    int keep = 0;
    int total = 0;
    for (const uct::ScreenLine& line : snapshot.lines) {
        if (keep >= limits.maxLines || line.text.size() > limits.maxLineUtf16 ||
            total + line.text.size() > limits.maxTotalUtf16) {
            break;
        }
        total += static_cast<int>(line.text.size());
        ++keep;
    }
    snapshot.lines.resize(keep);
    snapshot.lines.erase(std::remove_if(snapshot.lines.begin(), snapshot.lines.end(),
                                        [](const uct::ScreenLine& line) {
                                            return line.text.isEmpty() || line.text.contains(QLatin1Char('\n'));
                                        }),
                         snapshot.lines.end());
    languageTag_ = snapshot.languageTag;

    const int count = static_cast<int>(snapshot.lines.size());
    QHash<std::uint64_t, int> byId;
    byId.reserve(count);
    for (int i = 0; i < count; ++i) byId.insert(snapshot.lines[i].lineId, i);
    QVector<int> claimedBy(count, -1);
    QVector<int> unresolved;
    QVector<ExposedLine> revoked;
    // 1) Same line id, same text: identity and reader position are kept.
    for (int k = 0; k < lines_.size(); ++k) {
        const UctScreenLineAccessible* iface = lines_[k].iface;
        const int index = byId.value(iface->LineId(), -1);
        if (index >= 0 && claimedBy[index] < 0 && snapshot.lines[index].text == iface->Text()) {
            claimedBy[index] = k;
        } else {
            unresolved.push_back(k);
        }
    }
    // 2) Identical text re-recognized under a new id at the same place:
    //    rebind silently. 3) Anything else is revoked now.
    for (int k : unresolved) {
        const UctScreenLineAccessible* iface = lines_[k].iface;
        int best = -1;
        double bestIou = 0.0;
        for (int j = 0; j < count; ++j) {
            if (claimedBy[j] >= 0 || snapshot.lines[j].text != iface->Text()) continue;
            const double iou = Iou(snapshot.lines[j].sceneRect, iface->SceneRect());
            if (iou >= 0.5 && iou > bestIou) {
                best = j;
                bestIou = iou;
            }
        }
        if (best >= 0) {
            claimedBy[best] = k;
        } else {
            revoked.push_back(lines_[k]);
        }
    }
    // Rebuild in reading order. Additions wait for the cadence.
    QVector<ExposedLine> next;
    next.reserve(count);
    QVector<UctScreenLineAccessible*> created;
    bool missing = false;
    const bool publish = publishAdditions || publishImmediately_;
    for (int j = 0; j < count; ++j) {
        if (claimedBy[j] >= 0) {
            ExposedLine line = lines_[claimedBy[j]];
            line.iface->SetContent(snapshot.lines[j]);
            next.push_back(line);
        } else if (publish) {
            auto* iface = new UctScreenLineAccessible(this, snapshot.lines[j]);
            next.push_back({QAccessible::registerAccessibleInterface(iface), iface});
            created.push_back(iface);
        } else {
            missing = true;
        }
    }
    // The tree no longer references revoked elements before they are
    // deleted: the cache's ObjectDestroyed may make a client walk the tree.
    lines_ = std::move(next);
    for (const ExposedLine& line : revoked) DeleteLine(line);
    if (!created.isEmpty()) {
        ++publications_;
        publishImmediately_ = false;
        // Tree bookkeeping only: Qt's UIA adapter maps no event for this,
        // and nothing is announced or focused.
        for (UctScreenLineAccessible* iface : created) {
            QAccessibleEvent event(iface, QAccessible::ObjectCreated);
            QAccessible::updateAccessibility(&event);
        }
    }
    if (missing) {
        if (!pending_) {
            pending_ = true;
            pendingSinceMs_ = clock_();
        }
        ScheduleTick();
    } else {
        pending_ = false;
        cadence_->stop();
    }
}

void UctScreenTextAccessibility::EnsureRoot()
{
    if (root_) return;
    root_ = new UctScreenTextRootAccessible(this);
    rootId_ = QAccessible::registerAccessibleInterface(root_);
    QAccessibleEvent event(root_, QAccessible::ObjectCreated);
    QAccessible::updateAccessibility(&event);
}

void UctScreenTextAccessibility::DeleteLine(const ExposedLine& line)
{
    ++revocations_;
    // After application teardown Qt's cache has already deleted everything.
    if (QCoreApplication::instance()) QAccessible::deleteAccessibleInterface(line.id);
}

void UctScreenTextAccessibility::RemoveAllLines()
{
    const QVector<ExposedLine> lines = std::exchange(lines_, {});
    for (const ExposedLine& line : lines) DeleteLine(line);
}

void UctScreenTextAccessibility::DestroyAll()
{
    RemoveAllLines();
    pending_ = false;
    publishImmediately_ = true;
    if (cadence_) cadence_->stop();
    if (!root_) return;
    const QAccessible::Id id = std::exchange(rootId_, 0);
    root_ = nullptr;
    if (QCoreApplication::instance()) QAccessible::deleteAccessibleInterface(id);
}

std::int64_t UctScreenTextAccessibility::DueMs() const
{
    std::int64_t due = pendingSinceMs_ + timing_.cadenceMs;
    if (clock_() - lastQueryMs_ < timing_.readingHoldMs) {
        // Hold while a client reads, never longer than the bound measured
        // from the first pending change.
        due = std::max(due, std::min(lastQueryMs_ + timing_.readingHoldMs,
                                     pendingSinceMs_ + timing_.readingHoldMs));
    }
    return due;
}

void UctScreenTextAccessibility::ScheduleTick()
{
    if (!pending_ || cadence_->isActive()) return; // never restarted by churn
    const std::int64_t wait = std::clamp<std::int64_t>(DueMs() - clock_(), 0,
                                                       timing_.cadenceMs + timing_.readingHoldMs);
    cadence_->start(static_cast<int>(wait));
}

void UctScreenTextAccessibility::NoteQuery() const
{
    lastQueryMs_ = clock_();
}

QRect UctScreenTextAccessibility::ViewportRect() const
{
    QWidget* host = Host();
    if (!host) return QRect(QPoint(0, 0), QSize(0, 0));
    // RenderWidget's own native window: the same window Qt's UIA adapter
    // scales with, so DPI is applied exactly once.
    return QRect(host->mapToGlobal(QPoint(0, 0)), host->size());
}

} // namespace okuflow

#endif // _WIN32
