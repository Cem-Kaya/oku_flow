#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/common/uct_timing_trace.hpp"
#include "okuflow/ui/uct_overlay_snapshot.hpp"

#include <QFont>
#include <QDeadlineTimer>
#include <QHash>
#include <QPointer>
#include <QRegion>
#include <QScreen>
#include <QTransform>
#include <QWidget>

#include <array>
#include <utility>

QT_BEGIN_NAMESPACE
class QPaintEvent;
QT_END_NAMESPACE

namespace okuflow {

// Click-through, non-activating window over the native camera viewport that
// draws Ultra Clear Text glyph plates. It holds only accepted, scene-registered
// words; every paint projects them through the latest presentation transform,
// so pan/zoom/resize redraw at presentation rate without recognition work.
// Hides while the application is inactive or its owner is modal-blocked or
// disabled. Accepted words remain available for restoration when it resumes.
class UltraClearTextOverlay final : public QWidget {
    Q_OBJECT
public:
    UltraClearTextOverlay(QWidget* renderTarget, QWidget* owner);

    // Called on every presentation; repaints only when geometry changed.
    void SetViewTransform(const ViewTransform& transform);
    const ViewTransform& CurrentViewTransform() const noexcept { return transform_; }

    // Scene-space words safe to show. An empty set hides the window.
    struct SceneWord {
        QString text;
        QRectF sceneRect; // offset already applied
        uct::LineColors colors;
        std::uint64_t lineId{}; // with wordIndex, the stable reading/accessibility key
        QVector<QRectF> protectedSceneRects; // source ink that neither pass may cover
        int wordIndex{-1}; // uct::SceneWord::wordIndex; with lineId the accessibility key
        // Rotated word geometry (fitted ink). Glyph and plate follow its
        // baseline; sceneRect remains its bound. Invalid for upright words.
        uct::TextQuad sceneQuad;
        std::uint64_t layoutGroupId{}; // shared type size/baseline; 0 uses lineId
        QVector<QRectF> carvedSceneRects; // protected subtraction remainders; original otherwise
    };
    void SetWords(QVector<SceneWord> words);
    // Optional diagnostic provenance of this content assignment, never a view
    // receipt or a replacement for the model's independent word proof.
    void SetTimingTicket(uct::timing::PublicationTicket ticket) { timingTicket_ = ticket; }
    // UI-thread diagnostic copy, independent of optional trace record loss.
    uct::timing::PublicationTicket TimingTicket() const noexcept { return timingTicket_; }
    // Cached revision that actually reached paint, rather than assignment.
    std::uint64_t PaintedTimingRevision() const noexcept { return timingPaintedRevision_; }
    void ClearWords();
    int WordCount() const noexcept { return words_.size(); }
    const QVector<SceneWord>& Words() const noexcept { return words_; }

    // User style: -3..3 maps onto font weight, reusing Text Clarity's
    // existing stroke-weight preference.
    void SetStrokeWeight(int weight);
    void SetExcludedWidget(QWidget* widget);

    // Words as drawn for the current transform (tests and diagnostics).
    QVector<QRectF> ProjectedWordRects() const;
    struct GlyphGeometry {
        QRectF inkRect;
        qreal baselineY{};
    };
    QVector<GlyphGeometry> ProjectedGlyphs() const;
    // Freeze only eligible glyphs/plates in the requested capture transform.
    // Export excludes UI chrome, preserves protected source ink and leaves
    // the last-painted accessibility geometry untouched. UI thread only.
    UctOverlaySnapshot CaptureSnapshot(const ViewTransform& transform, const QSize& viewportSize);
    std::uint64_t PaintCount() const noexcept { return paintCount_; }
    QWidget* RenderTarget() const noexcept { return renderTarget_; }

    // --- Last-painted geometry (UCT screen text accessibility, UI thread) ---
    // Every query answers from the plan the most recent paintEvent actually
    // drew, never from words or a transform not yet painted. Coordinates are
    // overlay-local logical pixels, equal to render-target-local pixels while
    // PaintedGeometryCurrent(). Nothing here runs for frames nobody asks about.

    // Increments with every completed paint plan; 0 before the first one.
    std::uint64_t PaintedGeneration() const noexcept { return plan_.valid ? plan_.generation : 0; }
    // True while the last paint still describes the pixels on screen: it
    // drew a plan, the overlay is visible and exactly covers the render
    // target, and neither size, screen nor device pixel ratio changed since.
    bool PaintedGeometryCurrent() const;
    struct PaintedWordGeometry {
        bool drawn{};  // replacement plate and glyph were painted
        QRectF box;    // fitted ink when drawn, otherwise the projected source box
        // One rect per UTF-16 code unit (code units of one grapheme cluster
        // share a rect), unclipped. Drawn words use the QTextLayout caret
        // positions of the font stored with the paint plan (a later
        // SetStrokeWeight does not re-shape old pixels), so shaping and
        // fallback fonts match drawText(); source boxes split
        // proportionally, normalized against the visual caret extent so
        // right-to-left words keep their first character on the right.
        QVector<QRectF> characters;
    };
    // Word (lineId, wordIndex) of the last paint. False when the word was not
    // part of it or its text differs from `text`.
    bool PaintedWord(std::uint64_t lineId, int wordIndex, const QString& text,
                     PaintedWordGeometry& out) const;
    // Optional UI-thread transaction guard. Uses the actual current paint as
    // old authority and one cache-only proposed plan; no paint/publication,
    // event pumping, visibility change or cold text shaping. Refuses absent
    // paint authority, changed context, cold typography or budget exhaustion.
    bool PreservesDrawnWords(QVector<SceneWord> proposed,
                            const QVector<uct::OverlayModel::RegionalWordMatch>& matches,
                            double remainingMs);
    // Projects a scene rect through the transform of the last paint.
    QRectF PaintedSceneRect(const QRectF& sceneRect) const;
    // Bounding box of the part of `local` the last paint left visible.
    // Replacement pixels clip to the painted region; original camera pixels
    // clip to the viewport minus floating chrome. Empty when nothing shows.
    QRectF VisiblePart(const QRectF& local, bool replacement) const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    uct::timing::PublicationTicket timingTicket_{};
    std::uint64_t timingPaintedRevision_{};
    void SyncGeometryToRenderTarget();
    void UpdateVisibility();
    bool RebuildGlyphLayout(const QDeadlineTimer* deadline = nullptr);
    QRectF InkAtReference(const QString& text);
    struct PaintPlan;
    bool ComputePaintPlan(PaintPlan& plan, const ViewTransform& transform,
                          const QSize& viewportSize, bool excludeChrome,
                          const QDeadlineTimer* deadline = nullptr);
    void PaintWords(QPainter& painter, const PaintPlan& plan) const;
    // Caret x of every UTF-16 position shaped with `font` (NaN inside a
    // grapheme cluster). Computed lazily on the first accessibility query.
    // Painted geometry passes the font stored with the paint plan, never
    // the live reference font: a stroke-weight change replaces the live
    // font and clears this cache before the next paint, while the pixels
    // (and the plan's origin/scale) still belong to the old font.
    const QVector<qreal>& CaretPositions(const QString& text, const QFont& font) const;

    QPointer<QWidget> renderTarget_;
    QPointer<QWidget> excludedWidget_;
    bool applicationActive_{};
    bool ownerBlocked_{};
    ViewTransform transform_;
    QVector<SceneWord> words_;
    // Fitted glyph cached when words or style change; paint only reprojects
    // it. A word whose glyph is not valid is never drawn in either pass, so
    // an opaque plate can never erase the original without its replacement.
    struct SceneGlyph {
        QRectF inkRect;
        QPointF origin;
        QRectF referenceInk;      // measured ink at the reference pixel size
        uct::LineColors colors{}; // readable-color decision, made once
        bool valid{};
        // Rotated words: shared line type size and the word's own baseline
        // as fractions of its local height, valid under any pan/zoom.
        bool oriented{};
        qreal heightRatio{};       // glyph ink height / word height
        qreal baselineFraction{};  // baseline distance from the top edge / word height
        // Overlap decisions made once per content change, never per paint:
        // a second observation of the same word is not painted (and does not
        // protect ink its twin replaces); different text claiming the same
        // ink keeps every original pixel there.
        bool duplicate{};
        bool collision{};
    };
    QVector<SceneGlyph> glyphs_;
    QHash<QString, QRectF> inkCache_;
    mutable QHash<QString, QVector<qreal>> caretCache_;
    // The single per-frame eligibility decision, shared by both paint passes
    // and kept afterwards as the authority for accessibility geometry.
    // Vectors are reused, so recomputing it per paint does not allocate.
    struct PlannedWord {
        std::uint64_t lineId{};
        int wordIndex{-1};
        QRectF originalRect; // projected source box (bound of the quad when rotated)
        QRectF inkRect;      // projected fitted ink (bound when rotated)
        QRectF plateRect;    // plate bound
        QPointF origin;      // glyph baseline origin
        qreal scaleX{};
        qreal scaleY{};
        bool drawn{};
        // Rotated words: exact view polygons and baseline frame. Fixed-size
        // arrays keep per-paint planning allocation-free.
        bool oriented{};
        qreal angleDegrees{};
        QPointF along, across;               // unit baseline and downward normal
        qreal inkTop{}, inkBottom{};         // ink extent from the baseline along `across`
        std::array<QPointF, 4> original{}, ink{}, plate{};
        QTransform glyphToView; // complete projective glyph/quad mapping
    };
    struct PaintPlan {
        bool valid{};
        std::uint64_t generation{};
        QVector<PlannedWord> words; // parallel to words_ at paint time
        QVector<QString> texts;     // word text at paint time
        QRegion visibleClip;        // viewport minus floating chrome
        QRegion paintClip;          // visibleClip minus protected source ink
        ViewTransform transform;
        QFont font; // reference font this paint actually shaped with
        QSize size;
        qreal devicePixelRatio{};
        QPointer<QScreen> screen;
    };
    PaintPlan plan_;
    // Projects one rotated word; false when its view quad is degenerate.
    bool PlanOriented(PlannedWord& planned, const SceneGlyph& glyph, const uct::TextQuad& viewQuad) const;
    bool ResolveOverlaps(const QDeadlineTimer* deadline = nullptr);
    QRegion protectedInk_; // reused scratch
    mutable QHash<std::pair<std::uint64_t, int>, int> planIndex_;
    mutable std::uint64_t planIndexGeneration_{};
    QFont referenceFont_;
    int strokeWeight_{};
    std::uint64_t paintCount_{};
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
