#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/common/uct_timing_trace.hpp"

#include <QAccessible>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <limits>

QT_BEGIN_NAMESPACE
class QAccessibleInterface;
class QTimer;
class QWidget;
QT_END_NAMESPACE

// Ultra Clear Text on-screen text for Windows UI Automation.
//
// The text the overlay actually replaced on the camera view is exposed as
// virtual accessibility children of the render viewport: one read-only
// Grouping root, and one StaticText element per OCR line, each with its own
// text interface. Geometry is answered at query time from the overlay's last
// actual paint, mapped to global logical pixels (Qt's UIA adapter applies DPI
// once). New lines are published on a 1 s cadence, held up to 3 s
// while a screen reader is reading. Changed existing lines are replaced
// immediately; every loss of validation or authority is
// revoked synchronously. Nothing is announced, focused or spoken unprompted.
// See docs/uct_screen_text_accessibility.md.
namespace okuflow {

class UltraClearTextOverlay;
class UctScreenLineAccessible;
class UctScreenTextRootAccessible;

// Dynamic property. A widget for which it is true at the widget's first
// accessibility query gets an interface (QAccessibleWidget, role Client)
// that appends the elements of its AccessibleVirtualChildren child after
// its ordinary widget children. RenderWidget sets it in its constructor.
inline constexpr char kAccessibleVirtualChildHostProperty[] = "okuflowAccessibleVirtualChildHost";

// Installs the host factory once and sets the property on `host`. Has no
// effect on a widget whose accessibility interface already exists.
void MarkAccessibleVirtualChildHost(QWidget* host);

// Supplier of object-less accessibility elements for one host widget. It
// must be a direct QObject child of the host; the host interface finds it
// on every query, so attaching late or being destroyed first is safe.
class AccessibleVirtualChildren : public QObject {
    Q_OBJECT
public:
    // Becomes a child of `host` and marks it (MarkAccessibleVirtualChildHost).
    explicit AccessibleVirtualChildren(QWidget* host);
    QWidget* Host() const;

    virtual int VirtualChildCount() const = 0;
    virtual QAccessibleInterface* VirtualChild(int index) const = 0;
    virtual int IndexOfVirtualChild(const QAccessibleInterface* child) const = 0; // -1 if absent
    // Deepest useful element at a global logical point, or nullptr.
    virtual QAccessibleInterface* VirtualChildAt(int x, int y) const = 0;
};

// What the controller would expose right now. `lines` must already be the
// drawn, capped projection (uct::OverlayModel::ScreenLines filtered to words
// the overlay holds); an empty list is an empty root.
struct UctScreenTextSnapshot {
    bool enabled{};               // UCT is on: the root element exists
    QVector<uct::ScreenLine> lines; // reading order
    QString languageTag;          // recognizer language (UIA Culture)
};

struct UctScreenTextTiming {
    std::int64_t cadenceMs{1000};     // additions wait for the next semantic tick
    std::int64_t readingHoldMs{3000}; // hold while read; bounded from first pending change
};

// Owns the UCT accessibility elements of one render viewport. UI thread only.
// While QAccessible is inactive it creates nothing and every hook returns at
// once; activation (ActivationObserver) builds the current state. It never
// blocks, never touches the recognition or probe workers, and never forces
// a repaint. Destruction deletes every element it registered.
class UctScreenTextAccessibility final : public AccessibleVirtualChildren,
                                         private QAccessible::ActivationObserver {
    Q_OBJECT
public:
    using SnapshotProvider = std::function<UctScreenTextSnapshot()>;
    using Clock = std::function<std::int64_t()>;

    // `overlay` is the paint authority for geometry; `host` the viewport.
    UctScreenTextAccessibility(UltraClearTextOverlay* overlay, QWidget* host,
                               SnapshotProvider provider, Clock clock = {},
                               UctScreenTextTiming timing = {});
    ~UctScreenTextAccessibility() override;

    // UCT on/off: creates or deletes the root (and all lines).
    void SetEnabled(bool enabled);
    // Drawn content may have changed. Removals, text changes and identity
    // rebinding apply now; additions wait for the cadence (or publish at once
    // when nothing was published since enable, activation or RevokeAll()).
    void ContentChanged();
    // Diagnostic content ticket copied when additions are scheduled. View
    // projection cannot create an admission or change a pending ticket.
    void SetTimingTicket(uct::timing::PublicationTicket ticket) { timingTicket_ = ticket; }
    // Synchronously deletes every line element; the root stays (empty).
    void RevokeAll();
    // Cadence timer body; public for fixtures that drive a fake clock.
    void ProcessDueWork();

    // Diagnostics and fixtures.
    bool IsActive() const noexcept { return active_; }
    QAccessibleInterface* RootInterface() const;
    int ExposedLineCount() const noexcept { return static_cast<int>(lines_.size()); }
    bool HasPendingAdditions() const noexcept { return pending_; }
    std::uint64_t Publications() const noexcept { return publications_; }
    std::uint64_t Revocations() const noexcept { return revocations_; }
    std::uint64_t SnapshotsTaken() const noexcept { return snapshots_; }
    // Translatable source strings of the root element.
    static QString RootNameSource();
    static QString RootDescriptionSource();

    // AccessibleVirtualChildren
    int VirtualChildCount() const override;
    QAccessibleInterface* VirtualChild(int index) const override;
    int IndexOfVirtualChild(const QAccessibleInterface* child) const override;
    QAccessibleInterface* VirtualChildAt(int x, int y) const override;

private:
    friend class UctScreenLineAccessible;
    friend class UctScreenTextRootAccessible;

    struct ExposedLine {
        QAccessible::Id id{};
        UctScreenLineAccessible* iface{};
    };
    uct::timing::PublicationTicket timingTicket_{};
    uct::timing::PublicationTicket scheduledTimingTicket_{};
    bool haveScheduledTimingTicket_{};

    void accessibilityActiveChanged(bool active) override;
    void Sync(bool publishAdditions);
    void EnsureRoot();
    void DestroyAll();
    void RemoveAllLines();
    void DeleteLine(const ExposedLine& line);
    void ScheduleTick();
    std::int64_t DueMs() const;
    void NoteQuery() const;
    QRect ViewportRect() const;

    QPointer<UltraClearTextOverlay> overlay_;
    SnapshotProvider provider_;
    Clock clock_;
    UctScreenTextTiming timing_;
    QTimer* cadence_{};
    bool active_{};
    bool enabled_{};
    bool publishImmediately_{true};
    bool pending_{};
    std::int64_t pendingSinceMs_{};
    mutable std::int64_t lastQueryMs_{std::numeric_limits<std::int64_t>::min() / 2};
    QAccessible::Id rootId_{};
    UctScreenTextRootAccessible* root_{};
    QVector<ExposedLine> lines_;
    QString languageTag_;
    std::uint64_t publications_{};
    std::uint64_t revocations_{};
    std::uint64_t snapshots_{};
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
