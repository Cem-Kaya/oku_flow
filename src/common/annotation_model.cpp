#include "openzoom/common/annotation_model.hpp"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace openzoom {
namespace {

qreal SquaredDistanceToSegment(const QPointF& point,
                               const QPointF& start,
                               const QPointF& end)
{
    const QPointF segment = end - start;
    const qreal lengthSquared =
        segment.x() * segment.x() + segment.y() * segment.y();
    if (lengthSquared <= std::numeric_limits<qreal>::epsilon()) {
        const QPointF delta = point - start;
        return delta.x() * delta.x() + delta.y() * delta.y();
    }
    const QPointF offset = point - start;
    const qreal projection = std::clamp(
        (offset.x() * segment.x() + offset.y() * segment.y()) /
            lengthSquared,
        0.0,
        1.0);
    const QPointF closest = start + projection * segment;
    const QPointF delta = point - closest;
    return delta.x() * delta.x() + delta.y() * delta.y();
}

QPointF SceneToView(const QPointF& point,
                    const ViewTransform& transform,
                    const QSizeF& size)
{
    const qreal localX =
        (point.x() - transform.sourceX) / transform.sourceWidth;
    const qreal localY =
        (point.y() - transform.sourceY) / transform.sourceHeight;
    return {
        (transform.destinationX + localX * transform.destinationWidth) *
            size.width(),
        (transform.destinationY + localY * transform.destinationHeight) *
            size.height()};
}

qreal StrokePixelWidth(const AnnotationStroke& stroke,
                       const ViewTransform& transform,
                       const QSizeF& size)
{
    const qreal xScale =
        transform.destinationWidth * size.width() / transform.sourceWidth;
    const qreal yScale =
        transform.destinationHeight * size.height() / transform.sourceHeight;
    return std::clamp(stroke.sceneWidth * std::sqrt(xScale * yScale),
                      2.0,
                      72.0);
}

QRectF ApproximateTextSceneBounds(const AnnotationStroke& stroke)
{
    if (stroke.points.isEmpty()) {
        return {};
    }
    const qreal height = std::max<qreal>(stroke.sceneWidth, 0.001);
    const qreal width =
        std::max<qreal>(height,
                        height * 0.4 * std::max<qsizetype>(1, stroke.text.size()));
    return QRectF(stroke.points.front(), QSizeF(width, height));
}

QRectF StrokeSceneBounds(const AnnotationStroke& stroke)
{
    if (stroke.kind == AnnotationItemKind::kText) {
        return ApproximateTextSceneBounds(stroke);
    }
    if (stroke.points.isEmpty()) {
        return {};
    }
    qreal left = stroke.points.front().x();
    qreal right = left;
    qreal top = stroke.points.front().y();
    qreal bottom = top;
    for (const QPointF& point : stroke.points) {
        left = std::min(left, point.x());
        right = std::max(right, point.x());
        top = std::min(top, point.y());
        bottom = std::max(bottom, point.y());
    }
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

} // namespace

QPainterPath BuildStrokePath(const AnnotationStroke& stroke,
                             const ViewTransform& transform,
                             const QSizeF& destinationSize)
{
    QPainterPath path;
    if (!transform.valid || destinationSize.isEmpty() ||
        stroke.points.isEmpty()) {
        return path;
    }
    if (stroke.kind == AnnotationItemKind::kText) {
        const QPointF origin =
            SceneToView(stroke.points.front(), transform, destinationSize);
        const qreal yScale = transform.destinationHeight *
                             destinationSize.height() /
                             transform.sourceHeight;
        QFont font;
        font.setBold(true);
        font.setPixelSize(
            std::clamp(static_cast<int>(std::lround(stroke.sceneWidth * yScale)),
                       12,
                       144));
        const QFontMetricsF metrics(font);
        path.addText(origin + QPointF(0.0, metrics.ascent()),
                     font,
                     stroke.text);
        return path;
    }
    if ((stroke.kind == AnnotationItemKind::kRectangle ||
         stroke.kind == AnnotationItemKind::kEllipse) &&
        stroke.points.size() >= 2) {
        const QRectF bounds(
            SceneToView(stroke.points.front(), transform, destinationSize),
            SceneToView(stroke.points[1], transform, destinationSize));
        if (stroke.kind == AnnotationItemKind::kRectangle) {
            path.addRect(bounds.normalized());
        } else {
            path.addEllipse(bounds.normalized());
        }
        return path;
    }
    path.moveTo(SceneToView(stroke.points.front(), transform, destinationSize));
    for (qsizetype i = 1; i < stroke.points.size(); ++i) {
        path.lineTo(SceneToView(stroke.points[i], transform, destinationSize));
    }
    return path;
}

const QVector<AnnotationStroke>& AnnotationModel::strokes() const noexcept
{
    return strokes_;
}

bool AnnotationModel::hasInk() const noexcept
{
    return !strokes_.isEmpty();
}

std::optional<std::uint64_t> AnnotationModel::selectedStrokeId() const noexcept
{
    return selection_.isEmpty()
               ? std::nullopt
               : std::optional<std::uint64_t>(selection_.back());
}

const QVector<std::uint64_t>&
AnnotationModel::selectedStrokeIds() const noexcept
{
    return selection_;
}

const AnnotationStroke* AnnotationModel::selectedStroke() const noexcept
{
    return selection_.isEmpty() ? nullptr : FindStroke(selection_.back());
}

void AnnotationModel::PushUndo()
{
    undo_.push_back({strokes_, selection_});
    if (undo_.size() > 100) {
        undo_.removeFirst();
    }
    redo_.clear();
}

AnnotationStroke* AnnotationModel::FindStroke(std::uint64_t id)
{
    auto it = std::find_if(strokes_.begin(), strokes_.end(),
                           [id](const AnnotationStroke& stroke) {
                               return stroke.id == id;
                           });
    return it == strokes_.end() ? nullptr : &*it;
}

const AnnotationStroke* AnnotationModel::FindStroke(std::uint64_t id) const
{
    auto it = std::find_if(strokes_.cbegin(), strokes_.cend(),
                           [id](const AnnotationStroke& stroke) {
                               return stroke.id == id;
                           });
    return it == strokes_.cend() ? nullptr : &*it;
}

bool AnnotationModel::IsSelected(std::uint64_t id) const noexcept
{
    return selection_.contains(id);
}

void AnnotationModel::BeginStroke(const QPointF& scenePoint,
                                  const QColor& color,
                                  qreal sceneWidth,
                                  AnnotationItemKind kind,
                                  bool dashed)
{
    PushUndo();
    AnnotationStroke stroke;
    stroke.id = nextId_++;
    stroke.kind = kind;
    stroke.points.push_back(scenePoint);
    stroke.color = color.isValid() ? color : QColor(Qt::yellow);
    stroke.sceneWidth = std::clamp(sceneWidth, 0.0001, 0.2);
    stroke.dashed = dashed;
    strokes_.push_back(std::move(stroke));
    activeStroke_ = strokes_.back().id;
    selection_.clear();
}

std::uint64_t AnnotationModel::AddText(const QPointF& scenePoint,
                                       const QString& text,
                                       const QColor& color,
                                       qreal sceneHeight)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return 0;
    }
    PushUndo();
    AnnotationStroke item;
    item.id = nextId_++;
    item.kind = AnnotationItemKind::kText;
    item.points.push_back(scenePoint);
    item.color = color.isValid() ? color : QColor(Qt::yellow);
    item.sceneWidth = std::clamp(sceneHeight, 0.0005, 0.2);
    item.text = trimmed;
    strokes_.push_back(std::move(item));
    selection_ = {strokes_.back().id};
    activeStroke_.reset();
    return strokes_.back().id;
}

bool AnnotationModel::AppendStrokePoint(const QPointF& scenePoint,
                                        qreal minimumDistance)
{
    if (!activeStroke_) {
        return false;
    }
    AnnotationStroke* stroke = FindStroke(*activeStroke_);
    if (!stroke || stroke->points.isEmpty()) {
        return false;
    }
    const QPointF delta = scenePoint - stroke->points.back();
    if (delta.x() * delta.x() + delta.y() * delta.y() <
        minimumDistance * minimumDistance) {
        return false;
    }
    stroke->points.push_back(scenePoint);
    return true;
}

bool AnnotationModel::SetStraightStrokeEnd(const QPointF& scenePoint)
{
    if (!activeStroke_) {
        return false;
    }
    AnnotationStroke* stroke = FindStroke(*activeStroke_);
    if (!stroke || stroke->points.isEmpty()) {
        return false;
    }
    if (stroke->points.size() == 1) {
        stroke->points.push_back(scenePoint);
    } else {
        stroke->points.resize(2);
        stroke->points[1] = scenePoint;
    }
    return true;
}

void AnnotationModel::EndStroke()
{
    if (activeStroke_) {
        AnnotationStroke* stroke = FindStroke(*activeStroke_);
        if (stroke && stroke->points.size() == 1) {
            stroke->points.push_back(stroke->points.front());
        }
    }
    activeStroke_.reset();
}

std::optional<std::uint64_t> AnnotationModel::HitTest(
    const QPointF& scenePoint,
    qreal sceneTolerance) const
{
    for (auto stroke = strokes_.crbegin(); stroke != strokes_.crend(); ++stroke) {
        const qreal tolerance =
            sceneTolerance + std::max<qreal>(0.0, stroke->sceneWidth * 0.5);
        const qreal toleranceSquared = tolerance * tolerance;
        if (stroke->kind == AnnotationItemKind::kText) {
            const QRectF textBounds =
                ApproximateTextSceneBounds(*stroke).adjusted(
                    -tolerance, -tolerance, tolerance, tolerance);
            if (textBounds.contains(scenePoint)) {
                return stroke->id;
            }
            continue;
        }
        if ((stroke->kind == AnnotationItemKind::kRectangle ||
             stroke->kind == AnnotationItemKind::kEllipse) &&
            stroke->points.size() >= 2) {
            const QRectF bounds(stroke->points.front(), stroke->points[1]);
            const QRectF rect = bounds.normalized();
            constexpr int kSamples = 48;
            QPointF previous;
            for (int sample = 0; sample <= kSamples; ++sample) {
                QPointF current;
                if (stroke->kind == AnnotationItemKind::kRectangle) {
                    const qreal t = sample / static_cast<qreal>(kSamples) * 4.0;
                    const int edge = std::min(3, static_cast<int>(std::floor(t)));
                    const qreal u = t - edge;
                    if (edge == 0) {
                        current = {rect.left() + u * rect.width(), rect.top()};
                    } else if (edge == 1) {
                        current = {rect.right(), rect.top() + u * rect.height()};
                    } else if (edge == 2) {
                        current = {rect.right() - u * rect.width(), rect.bottom()};
                    } else {
                        current = {rect.left(), rect.bottom() - u * rect.height()};
                    }
                } else {
                    constexpr qreal kTau = 6.2831853071795864769;
                    const qreal angle =
                        kTau * sample / static_cast<qreal>(kSamples);
                    current = {
                        rect.center().x() + std::cos(angle) * rect.width() * 0.5,
                        rect.center().y() + std::sin(angle) * rect.height() * 0.5};
                }
                if (sample > 0 &&
                    SquaredDistanceToSegment(scenePoint, previous, current) <=
                        toleranceSquared) {
                    return stroke->id;
                }
                previous = current;
            }
            continue;
        }
        if (stroke->points.size() == 1) {
            const QPointF delta = scenePoint - stroke->points.front();
            if (delta.x() * delta.x() + delta.y() * delta.y() <=
                toleranceSquared) {
                return stroke->id;
            }
            continue;
        }
        for (qsizetype i = 1; i < stroke->points.size(); ++i) {
            if (SquaredDistanceToSegment(scenePoint,
                                         stroke->points[i - 1],
                                         stroke->points[i]) <=
                toleranceSquared) {
                return stroke->id;
            }
        }
    }
    return std::nullopt;
}

bool AnnotationModel::SelectAt(const QPointF& scenePoint,
                               qreal sceneTolerance,
                               bool extendSelection)
{
    const auto hit = HitTest(scenePoint, sceneTolerance);
    if (!hit) {
        if (!extendSelection) {
            selection_.clear();
        }
        return false;
    }
    if (!extendSelection) {
        selection_ = {*hit};
        return true;
    }
    const qsizetype existing = selection_.indexOf(*hit);
    if (existing >= 0) {
        selection_.removeAt(existing);
        return false;
    }
    selection_.push_back(*hit);
    return true;
}

int AnnotationModel::SelectInRect(const QRectF& sceneRect,
                                  bool extendSelection)
{
    if (!extendSelection) {
        selection_.clear();
    }
    const QRectF normalized = sceneRect.normalized();
    if (normalized.width() <= 0.0 || normalized.height() <= 0.0) {
        return selection_.size();
    }
    for (const AnnotationStroke& stroke : strokes_) {
        const qreal margin = std::max<qreal>(stroke.sceneWidth * 0.5, 0.0005);
        const QRectF bounds =
            StrokeSceneBounds(stroke).adjusted(-margin, -margin, margin, margin);
        if (normalized.intersects(bounds) && !IsSelected(stroke.id)) {
            selection_.push_back(stroke.id);
        }
    }
    return selection_.size();
}

void AnnotationModel::ClearSelection()
{
    selection_.clear();
}

bool AnnotationModel::EraseAt(const QPointF& scenePoint, qreal sceneTolerance)
{
    const auto hit = HitTest(scenePoint, sceneTolerance);
    if (!hit) {
        return false;
    }
    PushUndo();
    strokes_.erase(std::remove_if(strokes_.begin(), strokes_.end(),
                                  [hit](const AnnotationStroke& stroke) {
                                      return stroke.id == *hit;
                                  }),
                   strokes_.end());
    if (hit) {
        selection_.removeAll(*hit);
    }
    return true;
}

bool AnnotationModel::DeleteSelection()
{
    if (selection_.isEmpty()) {
        return false;
    }
    const QVector<std::uint64_t> ids = selection_;
    PushUndo();
    strokes_.erase(std::remove_if(strokes_.begin(), strokes_.end(),
                                  [&ids](const AnnotationStroke& stroke) {
                                      return ids.contains(stroke.id);
                                  }),
                   strokes_.end());
    selection_.clear();
    return true;
}

bool AnnotationModel::BeginMoveSelection()
{
    if (selection_.isEmpty()) {
        return false;
    }
    moveStartStrokes_.clear();
    for (const std::uint64_t id : selection_) {
        if (const AnnotationStroke* stroke = FindStroke(id)) {
            moveStartStrokes_.push_back(*stroke);
        }
    }
    if (moveStartStrokes_.isEmpty()) {
        return false;
    }
    PushUndo();
    movingSelection_ = true;
    return true;
}

bool AnnotationModel::MoveSelection(const QPointF& sceneDelta)
{
    if (!movingSelection_ || moveStartStrokes_.isEmpty()) {
        return false;
    }
    QRectF groupBounds;
    bool haveBounds = false;
    for (const std::uint64_t id : selection_) {
        const AnnotationStroke* stroke = FindStroke(id);
        if (!stroke) {
            continue;
        }
        const QRectF bounds = StrokeSceneBounds(*stroke);
        groupBounds = haveBounds ? groupBounds.united(bounds) : bounds;
        haveBounds = true;
    }
    if (!haveBounds) {
        return false;
    }
    const QPointF clampedDelta(
        std::clamp(sceneDelta.x(), -groupBounds.left(), 1.0 - groupBounds.right()),
        std::clamp(sceneDelta.y(), -groupBounds.top(), 1.0 - groupBounds.bottom()));
    for (const std::uint64_t id : selection_) {
        AnnotationStroke* stroke = FindStroke(id);
        if (!stroke) {
            continue;
        }
        for (QPointF& point : stroke->points) {
            point += clampedDelta;
        }
    }
    return true;
}

void AnnotationModel::EndMoveSelection()
{
    movingSelection_ = false;
    moveStartStrokes_.clear();
}

bool AnnotationModel::NudgeSelection(const QPointF& sceneDelta)
{
    if (selection_.isEmpty()) {
        return false;
    }
    if (!BeginMoveSelection()) {
        return false;
    }
    const bool moved = MoveSelection(sceneDelta);
    EndMoveSelection();
    return moved;
}

bool AnnotationModel::BeginScaleSelection()
{
    if (selection_.size() != 1) {
        return false;
    }
    const AnnotationStroke* stroke = FindStroke(selection_.front());
    if (!stroke) {
        return false;
    }
    PushUndo();
    scaleStartStroke_ = *stroke;
    scalingSelection_ = true;
    return true;
}

bool AnnotationModel::ScaleSelection(const QPointF& sceneAnchor,
                                     qreal scaleX,
                                     qreal scaleY)
{
    if (!scalingSelection_ || selection_.size() != 1 || !scaleStartStroke_) {
        return false;
    }
    AnnotationStroke* stroke = FindStroke(selection_.front());
    if (!stroke) {
        return false;
    }
    *stroke = *scaleStartStroke_;
    scaleX = std::clamp(scaleX, 0.01, 100.0);
    scaleY = std::clamp(scaleY, 0.01, 100.0);
    for (QPointF& point : stroke->points) {
        point.setX(std::clamp(sceneAnchor.x() +
                                  (point.x() - sceneAnchor.x()) * scaleX,
                              0.0,
                              1.0));
        point.setY(std::clamp(sceneAnchor.y() +
                                  (point.y() - sceneAnchor.y()) * scaleY,
                              0.0,
                              1.0));
    }
    const qreal geometricScale = std::sqrt(scaleX * scaleY);
    stroke->sceneWidth =
        std::clamp(scaleStartStroke_->sceneWidth * geometricScale,
                   0.0001,
                   0.2);
    return true;
}

void AnnotationModel::EndScaleSelection()
{
    scalingSelection_ = false;
    scaleStartStroke_.reset();
}

bool AnnotationModel::ScaleSelectionAboutCenter(qreal scaleFactor)
{
    if (selection_.size() != 1) {
        return false;
    }
    AnnotationStroke* stroke = FindStroke(selection_.front());
    if (!stroke) {
        return false;
    }
    const QRectF bounds = StrokeSceneBounds(*stroke);
    const QPointF center = bounds.center();
    if (!BeginScaleSelection()) {
        return false;
    }
    const bool changed =
        ScaleSelection(center, scaleFactor, scaleFactor);
    EndScaleSelection();
    return changed;
}

bool AnnotationModel::Clear()
{
    if (strokes_.isEmpty()) {
        return false;
    }
    PushUndo();
    strokes_.clear();
    selection_.clear();
    activeStroke_.reset();
    movingSelection_ = false;
    moveStartStrokes_.clear();
    scalingSelection_ = false;
    scaleStartStroke_.reset();
    return true;
}

void AnnotationModel::ResetSession()
{
    strokes_.clear();
    undo_.clear();
    redo_.clear();
    selection_.clear();
    activeStroke_.reset();
    movingSelection_ = false;
    moveStartStrokes_.clear();
    scalingSelection_ = false;
    scaleStartStroke_.reset();
    nextId_ = 1;
}

bool AnnotationModel::Undo()
{
    if (undo_.isEmpty()) {
        return false;
    }
    redo_.push_back({strokes_, selection_});
    const Snapshot snapshot = undo_.takeLast();
    strokes_ = snapshot.strokes;
    selection_ = snapshot.selection;
    activeStroke_.reset();
    movingSelection_ = false;
    moveStartStrokes_.clear();
    scalingSelection_ = false;
    scaleStartStroke_.reset();
    return true;
}

bool AnnotationModel::Redo()
{
    if (redo_.isEmpty()) {
        return false;
    }
    undo_.push_back({strokes_, selection_});
    const Snapshot snapshot = redo_.takeLast();
    strokes_ = snapshot.strokes;
    selection_ = snapshot.selection;
    activeStroke_.reset();
    movingSelection_ = false;
    moveStartStrokes_.clear();
    scalingSelection_ = false;
    scaleStartStroke_.reset();
    return true;
}

bool AnnotationModel::canUndo() const noexcept
{
    return !undo_.isEmpty();
}

bool AnnotationModel::canRedo() const noexcept
{
    return !redo_.isEmpty();
}

qreal AnnotationSceneTolerance(const ViewTransform& transform,
                               const QSizeF& destinationSize,
                               qreal pixelTolerance)
{
    if (!transform.valid || destinationSize.width() <= 0.0 ||
        destinationSize.height() <= 0.0) {
        return 0.02;
    }
    const qreal sourcePerPixelX =
        transform.sourceWidth /
        (transform.destinationWidth * destinationSize.width());
    const qreal sourcePerPixelY =
        transform.sourceHeight /
        (transform.destinationHeight * destinationSize.height());
    return pixelTolerance * std::max(sourcePerPixelX, sourcePerPixelY);
}

void RenderAnnotationStrokes(QPainter& painter,
                             const QVector<AnnotationStroke>& strokes,
                             const ViewTransform& transform,
                             const QSizeF& destinationSize,
                             std::optional<std::uint64_t> selectedStroke)
{
    QVector<std::uint64_t> selection;
    if (selectedStroke) {
        selection.push_back(*selectedStroke);
    }
    RenderAnnotationStrokes(painter,
                            strokes,
                            transform,
                            destinationSize,
                            selection);
}

void RenderAnnotationStrokes(
    QPainter& painter,
    const QVector<AnnotationStroke>& strokes,
    const ViewTransform& transform,
    const QSizeF& destinationSize,
    const QVector<std::uint64_t>& selectedStrokes)
{
    if (!transform.valid || destinationSize.isEmpty()) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF activeDestination(
        transform.destinationX * destinationSize.width(),
        transform.destinationY * destinationSize.height(),
        transform.destinationWidth * destinationSize.width(),
        transform.destinationHeight * destinationSize.height());
    painter.setClipRect(activeDestination, Qt::IntersectClip);
    QRectF groupSelectionBounds;
    bool haveGroupSelectionBounds = false;

    // Pass 1: ink only. Selection visuals are deferred to pass 2 so that
    // (a) they always render on top of every stroke, and (b) their pen/brush
    // state can never leak into stroke drawing. A leaked solid cyan handle
    // brush previously FILLED every stroke drawn after the selected one
    // (drawPath fills with the active brush in addition to stroking), which
    // painted later closed-ish strokes solid cyan.
    QVector<QRectF> selectedBounds;
    for (const AnnotationStroke& stroke : strokes) {
        if (stroke.points.isEmpty()) {
            continue;
        }
        const QPainterPath path =
            BuildStrokePath(stroke, transform, destinationSize);
        const qreal coreWidth =
            stroke.kind == AnnotationItemKind::kText
                ? 2.0
                : StrokePixelWidth(stroke, transform, destinationSize);
        QPen halo(QColor(8, 8, 8, 235),
                  coreWidth + 6.0,
                  Qt::SolidLine,
                  Qt::RoundCap,
                  Qt::RoundJoin);
        if (stroke.dashed && stroke.kind != AnnotationItemKind::kText) {
            const qreal haloScale = coreWidth / (coreWidth + 6.0);
            halo.setDashPattern({4.0 * haloScale, 3.0 * haloScale});
            halo.setDashOffset(0.0);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(halo);
        painter.drawPath(path);

        QPen core(stroke.color,
                  coreWidth,
                  Qt::SolidLine,
                  Qt::RoundCap,
                  Qt::RoundJoin);
        if (stroke.dashed && stroke.kind != AnnotationItemKind::kText) {
            core.setDashPattern({4.0, 3.0});
            core.setDashOffset(0.0);
        }
        painter.setPen(core);
        if (stroke.kind == AnnotationItemKind::kText) {
            painter.fillPath(path, stroke.color);
        } else {
            painter.drawPath(path);
        }

        if (selectedStrokes.contains(stroke.id)) {
            const QRectF selectionBounds =
                path.boundingRect().adjusted(-10.0, -10.0, 10.0, 10.0);
            selectedBounds.push_back(selectionBounds);
            groupSelectionBounds =
                haveGroupSelectionBounds
                    ? groupSelectionBounds.united(selectionBounds)
                    : selectionBounds;
            haveGroupSelectionBounds = true;
        }
    }

    // Pass 2: selection visuals over all ink.
    for (const QRectF& selectionBounds : selectedBounds) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(QStringLiteral("#00e5ff")),
                            3.0,
                            Qt::DashLine));
        painter.drawRect(selectionBounds);
        if (selectedStrokes.size() != 1) {
            continue;
        }
        painter.setPen(QPen(QColor(8, 8, 8), 2.0));
        painter.setBrush(QColor(QStringLiteral("#00e5ff")));
        constexpr qreal handleSize = 14.0;
        const std::array<QPointF, 8> handles{
            selectionBounds.topLeft(),
            QPointF(selectionBounds.center().x(), selectionBounds.top()),
            selectionBounds.topRight(),
            QPointF(selectionBounds.right(), selectionBounds.center().y()),
            selectionBounds.bottomRight(),
            QPointF(selectionBounds.center().x(), selectionBounds.bottom()),
            selectionBounds.bottomLeft(),
            QPointF(selectionBounds.left(), selectionBounds.center().y())};
        for (const QPointF& handle : handles) {
            painter.drawRect(QRectF(handle.x() - handleSize * 0.5,
                                    handle.y() - handleSize * 0.5,
                                    handleSize,
                                    handleSize));
        }
    }
    if (selectedStrokes.size() > 1 && haveGroupSelectionBounds) {
        painter.setBrush(QColor(0, 229, 255, 20));
        painter.setPen(QPen(QColor(QStringLiteral("#ffffff")),
                            2.0,
                            Qt::DashLine));
        painter.drawRect(groupSelectionBounds.adjusted(
            -4.0, -4.0, 4.0, 4.0));
    }
    painter.restore();
}

} // namespace openzoom
