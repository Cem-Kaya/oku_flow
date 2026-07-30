#pragma once

#include "openzoom/common/view_transform.hpp"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>

QT_BEGIN_NAMESPACE
class QPainter;
class QPainterPath;
QT_END_NAMESPACE

namespace openzoom {

enum class AnnotationTool : std::uint8_t {
    kPen,
    kLine,
    kText,
    kShape,
    kPointer,
    kEraser,
};

enum class AnnotationItemKind : std::uint8_t {
    kFreehand,
    kLine,
    kText,
    kRectangle,
    kEllipse,
};

struct AnnotationStroke {
    std::uint64_t id{};
    AnnotationItemKind kind{AnnotationItemKind::kFreehand};
    QVector<QPointF> points;
    QColor color{Qt::yellow};
    qreal sceneWidth{0.004};
    QString text;
    bool dashed{};

    bool operator==(const AnnotationStroke&) const = default;
};

// Session-owned vector ink model. Coordinates and stroke widths are normalized
// to the processed camera scene, so edits remain registered while the viewport
// pans and zooms.
class AnnotationModel {
public:
    const QVector<AnnotationStroke>& strokes() const noexcept;
    bool hasInk() const noexcept;
    std::optional<std::uint64_t> selectedStrokeId() const noexcept;
    const QVector<std::uint64_t>& selectedStrokeIds() const noexcept;
    const AnnotationStroke* selectedStroke() const noexcept;

    void BeginStroke(const QPointF& scenePoint,
                     const QColor& color,
                     qreal sceneWidth,
                     AnnotationItemKind kind = AnnotationItemKind::kFreehand,
                     bool dashed = false);
    std::uint64_t AddText(const QPointF& scenePoint,
                          const QString& text,
                          const QColor& color,
                          qreal sceneHeight);
    bool AppendStrokePoint(const QPointF& scenePoint, qreal minimumDistance);
    bool SetStraightStrokeEnd(const QPointF& scenePoint);
    void EndStroke();

    std::optional<std::uint64_t> HitTest(const QPointF& scenePoint,
                                         qreal sceneTolerance) const;
    bool SelectAt(const QPointF& scenePoint,
                  qreal sceneTolerance,
                  bool extendSelection = false);
    int SelectInRect(const QRectF& sceneRect, bool extendSelection = false);
    void ClearSelection();
    bool EraseAt(const QPointF& scenePoint, qreal sceneTolerance);
    bool DeleteSelection();
    bool BeginMoveSelection();
    bool MoveSelection(const QPointF& sceneDelta);
    void EndMoveSelection();
    bool NudgeSelection(const QPointF& sceneDelta);
    bool BeginScaleSelection();
    bool ScaleSelection(const QPointF& sceneAnchor,
                        qreal scaleX,
                        qreal scaleY);
    void EndScaleSelection();
    bool ScaleSelectionAboutCenter(qreal scaleFactor);

    bool Clear();
    void ResetSession();
    bool Undo();
    bool Redo();
    bool canUndo() const noexcept;
    bool canRedo() const noexcept;

private:
    struct Snapshot {
        QVector<AnnotationStroke> strokes;
        QVector<std::uint64_t> selection;
    };

    void PushUndo();
    bool IsSelected(std::uint64_t id) const noexcept;
    AnnotationStroke* FindStroke(std::uint64_t id);
    const AnnotationStroke* FindStroke(std::uint64_t id) const;

    QVector<AnnotationStroke> strokes_;
    QVector<Snapshot> undo_;
    QVector<Snapshot> redo_;
    QVector<std::uint64_t> selection_;
    std::optional<std::uint64_t> activeStroke_;
    std::uint64_t nextId_{1};
    bool movingSelection_{};
    bool scalingSelection_{};
    QVector<AnnotationStroke> moveStartStrokes_;
    std::optional<AnnotationStroke> scaleStartStroke_;
};

// Builds the exact painter path used by live rendering, snapshots, selection
// bounds, and handle hit testing.
QPainterPath BuildStrokePath(const AnnotationStroke& stroke,
                             const ViewTransform& transform,
                             const QSizeF& destinationSize);

// Shared overlay/snapshot renderer. The destination size is the viewport in
// logical pixels for the live overlay or image pixels for a saved snapshot.
void RenderAnnotationStrokes(QPainter& painter,
                             const QVector<AnnotationStroke>& strokes,
                             const ViewTransform& transform,
                             const QSizeF& destinationSize,
                             std::optional<std::uint64_t> selectedStroke = {});
void RenderAnnotationStrokes(QPainter& painter,
                             const QVector<AnnotationStroke>& strokes,
                             const ViewTransform& transform,
                             const QSizeF& destinationSize,
                             const QVector<std::uint64_t>& selectedStrokes);

qreal AnnotationSceneTolerance(const ViewTransform& transform,
                               const QSizeF& destinationSize,
                               qreal pixelTolerance);

} // namespace openzoom
