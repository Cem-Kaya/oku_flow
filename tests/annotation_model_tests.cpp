#include "openzoom/common/annotation_model.hpp"
#include "openzoom/common/view_transform.hpp"

#include <QImage>
#include <QPainter>
#include <QtTest>

#include <cmath>

namespace openzoom {

class AnnotationModelTests final : public QObject {
    Q_OBJECT

private slots:
    void editsSupportUndoRedoAndPermanentSessionReset();
    void selectionMoveEraseAndNudgeUseSceneCoordinates();
    void lineAndTextItemsSelectMoveAndRender();
    void shapesAndDashedStyleRenderAndHitTest();
    void selectionScaleIsOneUndoableGesture();
    void marqueeSelectionMovesAndDeletesAGroupAsOneEdit();
    void selectionVisualsNeverLeakIntoOtherStrokes();
    void renderingTracksCanonicalViewportTransform();
    void recordingInkRetainsSceneCoordinatesWithSuperResCrop();
    void sceneToleranceTightensAsViewportZooms();
    void renderingHonorsTheCallersExclusionRegion();
};

void AnnotationModelTests::renderingHonorsTheCallersExclusionRegion()
{
    AnnotationModel model;
    model.BeginStroke({0.1, 0.5}, Qt::yellow, 0.02);
    model.AppendStrokePoint({0.9, 0.5}, 0.001);
    model.EndStroke();
    QImage image(400, 200, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setClipRegion(QRegion(image.rect()) - QRect(150, 0, 100, 200));
    RenderAnnotationStrokes(painter, model.strokes(),
                            ComputeViewTransform(400, 200, 400, 200,
                                                 1.0f, 0.5f, 0.5f, ViewportFitMode::kFill),
                            image.size());
    painter.end();
    QVERIFY(image.pixelColor(100, 100).alpha() > 0);
    QCOMPARE(image.pixelColor(200, 100).alpha(), 0);
    QVERIFY(image.pixelColor(300, 100).alpha() > 0);
}

void AnnotationModelTests::editsSupportUndoRedoAndPermanentSessionReset()
{
    AnnotationModel model;
    model.BeginStroke({0.2, 0.3}, QColor(QStringLiteral("#fff000")), 0.01);
    QVERIFY(model.AppendStrokePoint({0.8, 0.7}, 0.001));
    model.EndStroke();
    QVERIFY(model.hasInk());
    QCOMPARE(model.strokes().size(), 1);

    QVERIFY(model.Undo());
    QVERIFY(!model.hasInk());
    QVERIFY(model.Redo());
    QCOMPARE(model.strokes().size(), 1);

    model.ResetSession();
    QVERIFY(!model.hasInk());
    QVERIFY(!model.canUndo());
    QVERIFY(!model.canRedo());
}

void AnnotationModelTests::selectionMoveEraseAndNudgeUseSceneCoordinates()
{
    AnnotationModel model;
    model.BeginStroke({0.2, 0.2}, Qt::cyan, 0.01);
    model.AppendStrokePoint({0.4, 0.4}, 0.001);
    model.EndStroke();
    const auto id = model.HitTest({0.3, 0.3}, 0.02);
    QVERIFY(id.has_value());
    QVERIFY(model.SelectAt({0.3, 0.3}, 0.02));
    QVERIFY(model.NudgeSelection({0.1, 0.05}));
    QVERIFY(!model.HitTest({0.2, 0.2}, 0.01).has_value());
    QVERIFY(model.HitTest({0.4, 0.35}, 0.02).has_value());
    QVERIFY(model.DeleteSelection());
    QVERIFY(!model.hasInk());
    QVERIFY(model.Undo());
    QVERIFY(model.hasInk());
    QVERIFY(model.EraseAt({0.4, 0.35}, 0.02));
    QVERIFY(!model.hasInk());
}

void AnnotationModelTests::lineAndTextItemsSelectMoveAndRender()
{
    AnnotationModel model;
    model.BeginStroke({0.15, 0.25},
                      Qt::yellow,
                      0.008,
                      AnnotationItemKind::kLine);
    QVERIFY(model.SetStraightStrokeEnd({0.55, 0.25}));
    model.EndStroke();
    QCOMPARE(model.strokes().front().kind, AnnotationItemKind::kLine);

    const std::uint64_t textId =
        model.AddText({0.25, 0.45}, QStringLiteral("Lecture note"),
                      Qt::cyan, 0.06);
    QVERIFY(textId != 0);
    QCOMPARE(model.strokes().back().kind, AnnotationItemKind::kText);
    QCOMPARE(model.selectedStrokeId(), std::optional<std::uint64_t>(textId));
    QVERIFY(model.SelectAt({0.35, 0.47}, 0.01));
    QVERIFY(model.BeginMoveSelection());
    QVERIFY(model.MoveSelection({0.05, 0.02}));
    model.EndMoveSelection();
    QVERIFY(model.HitTest({0.40, 0.49}, 0.015).has_value());

    const ViewTransform transform =
        ComputeViewTransform(1280, 720, 640, 360, 1.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill);
    QImage image(640, 360, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    RenderAnnotationStrokes(painter,
                            model.strokes(),
                            transform,
                            image.size(),
                            model.selectedStrokeId());
    painter.end();

    int visiblePixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            visiblePixels += image.pixelColor(x, y).alpha() > 0 ? 1 : 0;
        }
    }
    QVERIFY(visiblePixels > 500);
}

void AnnotationModelTests::shapesAndDashedStyleRenderAndHitTest()
{
    AnnotationModel model;
    model.BeginStroke({0.2, 0.2},
                      QColor(QStringLiteral("#fff000")),
                      0.008,
                      AnnotationItemKind::kRectangle,
                      true);
    QVERIFY(model.SetStraightStrokeEnd({0.6, 0.6}));
    model.EndStroke();
    QCOMPARE(model.strokes().front().kind, AnnotationItemKind::kRectangle);
    QVERIFY(model.strokes().front().dashed);
    QVERIFY(model.HitTest({0.4, 0.2}, 0.015).has_value());
    QVERIFY(!model.HitTest({0.4, 0.4}, 0.015).has_value());

    model.BeginStroke({0.65, 0.2},
                      QColor(QStringLiteral("#00e5ff")),
                      0.008,
                      AnnotationItemKind::kEllipse);
    QVERIFY(model.SetStraightStrokeEnd({0.9, 0.6}));
    model.EndStroke();
    QCOMPARE(model.strokes().back().kind, AnnotationItemKind::kEllipse);
    QVERIFY(model.HitTest({0.775, 0.2}, 0.02).has_value());

    const ViewTransform transform =
        ComputeViewTransform(1280, 720, 640, 360, 1.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill);
    QImage image(640, 360, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    RenderAnnotationStrokes(painter, model.strokes(), transform, image.size());
    painter.end();
    QVERIFY(image.pixelColor(128, 72).alpha() > 0);
}

void AnnotationModelTests::selectionScaleIsOneUndoableGesture()
{
    AnnotationModel model;
    model.BeginStroke({0.2, 0.2}, Qt::yellow, 0.01,
                      AnnotationItemKind::kRectangle);
    QVERIFY(model.SetStraightStrokeEnd({0.4, 0.4}));
    model.EndStroke();
    QVERIFY(model.SelectAt({0.3, 0.2}, 0.02));
    const AnnotationStroke before = model.strokes().front();

    QVERIFY(model.BeginScaleSelection());
    QVERIFY(model.ScaleSelection({0.2, 0.2}, 1.5, 2.0));
    QVERIFY(model.ScaleSelection({0.2, 0.2}, 2.0, 2.0));
    model.EndScaleSelection();
    const AnnotationStroke after = model.strokes().front();
    QVERIFY(after.points.back().x() > before.points.back().x());
    QVERIFY(after.points.back().y() > before.points.back().y());

    QVERIFY(model.Undo());
    QCOMPARE(model.strokes().front(), before);
    QVERIFY(model.Redo());
    QCOMPARE(model.strokes().front(), after);
}

void AnnotationModelTests::marqueeSelectionMovesAndDeletesAGroupAsOneEdit()
{
    AnnotationModel model;
    model.BeginStroke({0.15, 0.20}, Qt::yellow, 0.01);
    QVERIFY(model.AppendStrokePoint({0.25, 0.25}, 0.001));
    model.EndStroke();
    model.BeginStroke({0.35, 0.30}, Qt::cyan, 0.01);
    QVERIFY(model.AppendStrokePoint({0.45, 0.35}, 0.001));
    model.EndStroke();
    model.BeginStroke({0.75, 0.75}, Qt::green, 0.01);
    QVERIFY(model.AppendStrokePoint({0.85, 0.80}, 0.001));
    model.EndStroke();

    QCOMPARE(model.SelectInRect(QRectF(0.10, 0.15, 0.40, 0.25)), 2);
    QCOMPARE(model.selectedStrokeIds().size(), 2);
    const QVector<AnnotationStroke> beforeMove = model.strokes();

    QVERIFY(model.BeginMoveSelection());
    QVERIFY(model.MoveSelection({0.10, 0.05}));
    model.EndMoveSelection();
    QCOMPARE(model.strokes().at(0).points.front(),
             beforeMove.at(0).points.front() + QPointF(0.10, 0.05));
    QCOMPARE(model.strokes().at(1).points.front(),
             beforeMove.at(1).points.front() + QPointF(0.10, 0.05));
    QCOMPARE(model.strokes().at(2), beforeMove.at(2));

    QVERIFY(model.Undo());
    QCOMPARE(model.strokes(), beforeMove);
    QCOMPARE(model.selectedStrokeIds().size(), 2);
    QVERIFY(model.DeleteSelection());
    QCOMPARE(model.strokes().size(), 1);
    QCOMPARE(model.strokes().front(), beforeMove.at(2));
    QVERIFY(model.Undo());
    QCOMPARE(model.strokes(), beforeMove);
}

// Regression: selecting one stroke must never alter how OTHER strokes render.
// The selection handles are painted with a solid cyan brush; a leaked brush
// previously made every stroke drawn after the selected one render FILLED
// (drawPath fills with the active brush), flooding closed-ish strokes cyan.
void AnnotationModelTests::selectionVisualsNeverLeakIntoOtherStrokes()
{
    AnnotationModel model;
    // Stroke A: drawn FIRST, then selected — the leak only affected strokes
    // that come after the selected one in draw order.
    model.BeginStroke({0.15, 0.15}, Qt::yellow, 0.008);
    QVERIFY(model.AppendStrokePoint({0.25, 0.15}, 0.001));
    model.EndStroke();
    // Stroke B: drawn SECOND — an open arc whose implicit closing encloses a
    // large empty area (like a drawn mouth). Its interior must stay empty.
    model.BeginStroke({0.30, 0.70}, Qt::yellow, 0.008);
    QVERIFY(model.AppendStrokePoint({0.50, 0.85}, 0.001));
    QVERIFY(model.AppendStrokePoint({0.70, 0.70}, 0.001));
    model.EndStroke();

    QVERIFY(model.SelectAt({0.20, 0.15}, 0.02));
    QCOMPARE(model.selectedStrokeId(),
             std::optional<std::uint64_t>(model.strokes().front().id));

    const ViewTransform transform =
        ComputeViewTransform(1280, 720, 640, 360, 1.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill);
    QImage image(640, 360, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    RenderAnnotationStrokes(painter,
                            model.strokes(),
                            transform,
                            image.size(),
                            model.selectedStrokeId());
    painter.end();

    // Scene (0.5, 0.75) sits inside B's implicitly-closed area, well clear of
    // the stroke itself (~18+ px from every edge at this geometry) and far
    // from A's selection box. It must remain fully transparent.
    const QColor interior = image.pixelColor(320, 270);
    QCOMPARE(interior.alpha(), 0);

    // The selected stroke must still show its handles (sanity check that the
    // selection visuals were actually exercised): the box around A carries
    // solid cyan handle pixels near its corner.
    bool foundHandlePixel = false;
    for (int y = 30; y < 90 && !foundHandlePixel; ++y) {
        for (int x = 70; x < 180 && !foundHandlePixel; ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 200 && color.blue() > 200 &&
                color.green() > 180 && color.red() < 80) {
                foundHandlePixel = true;
            }
        }
    }
    QVERIFY(foundHandlePixel);
}

void AnnotationModelTests::renderingTracksCanonicalViewportTransform()
{
    AnnotationStroke stroke;
    stroke.id = 1;
    stroke.points = {{0.25, 0.5}, {0.75, 0.5}};
    stroke.color = QColor(QStringLiteral("#00e5ff"));
    stroke.sceneWidth = 0.01;

    const ViewTransform transform =
        ComputeViewTransform(1280,
                             720,
                             400,
                             200,
                             2.0f,
                             0.5f,
                             0.5f,
                             ViewportFitMode::kFill);
    QVERIFY(transform.valid);

    QImage image(400, 200, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    RenderAnnotationStrokes(painter, {stroke}, transform, image.size());
    painter.end();

    const QColor center = image.pixelColor(200, 100);
    QVERIFY(center.alpha() > 0);
    QVERIFY(center.cyan() > 180);

    QImage panned(400, 200, QImage::Format_ARGB32);
    panned.fill(Qt::transparent);
    const ViewTransform right =
        ComputeViewTransform(1280,
                             720,
                             400,
                             200,
                             2.0f,
                             0.75f,
                             0.5f,
                             ViewportFitMode::kFill);
    QPainter pannedPainter(&panned);
    RenderAnnotationStrokes(pannedPainter, {stroke}, right, panned.size());
    pannedPainter.end();
    QVERIFY(panned.pixelColor(100, 100).alpha() > 0);
}

void AnnotationModelTests::recordingInkRetainsSceneCoordinatesWithSuperResCrop()
{
    AnnotationStroke stroke;
    stroke.id = 1;
    stroke.points = {{0.60, 0.60}, {0.70, 0.60}};
    stroke.color = Qt::cyan;
    stroke.sceneWidth = 0.01;

    // A panned 2x view is backed by a non-central SuperRes crop. Annotation
    // points stay normalized to the original scene, not that cache texture.
    const ViewTransform annotationTransform = ComputeViewTransform(
        1280, 720, 640, 360, 2.0f, 0.65f, 0.60f, ViewportFitMode::kFill);
    QVERIFY(annotationTransform.valid);
    ViewTransform croppedTextureTransform;
    QVERIFY(RemapViewTransformToSourceRect(
        annotationTransform, {0.40f, 0.35f, 0.50f, 0.50f},
        croppedTextureTransform));
    ViewTransform fullFrameTextureTransform;
    QVERIFY(RemapViewTransformToSourceRect(
        annotationTransform, {0.0f, 0.0f, 1.0f, 1.0f},
        fullFrameTextureTransform));

    // Recording canvases may differ from the live viewport dimensions.
    for (const QSize canvas : {QSize(640, 360), QSize(1280, 720)}) {
        auto render = [&](const ViewTransform& inkTransform) {
            QImage layer(canvas, QImage::Format_ARGB32);
            layer.fill(Qt::transparent);
            QPainter painter(&layer);
            RenderAnnotationStrokes(painter, {stroke}, inkTransform, canvas);
            painter.end();
            return layer;
        };
        const QImage recordedInk = render(annotationTransform);
        const QPoint expectedCenter(canvas.width() / 2, canvas.height() / 2);
        QVERIFY(recordedInk.pixelColor(expectedCenter).alpha() > 200);
        QVERIFY(recordedInk.pixelColor(canvas.width() / 4,
                                       canvas.height() / 2).alpha() == 0);
        QCOMPARE(recordedInk, render(fullFrameTextureTransform));

        // This is the previously incorrect call: texture-space geometry
        // moves scene-space ink away from the content at the view center.
        const QImage misplacedInk = render(croppedTextureTransform);
        QCOMPARE(misplacedInk.pixelColor(expectedCenter).alpha(), 0);
        QVERIFY(misplacedInk != recordedInk);
    }
}

void AnnotationModelTests::sceneToleranceTightensAsViewportZooms()
{
    const QSizeF viewport(1280, 720);
    const ViewTransform oneX =
        ComputeViewTransform(1280, 720, 1280, 720, 1.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill);
    const ViewTransform fourX =
        ComputeViewTransform(1280, 720, 1280, 720, 4.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill);
    const qreal oneXTolerance =
        AnnotationSceneTolerance(oneX, viewport, 12.0);
    const qreal fourXTolerance =
        AnnotationSceneTolerance(fourX, viewport, 12.0);
    QVERIFY(fourXTolerance < oneXTolerance);
    QVERIFY(std::abs(oneXTolerance / fourXTolerance - 4.0) < 0.01);
}

} // namespace openzoom

QTEST_MAIN(openzoom::AnnotationModelTests)

#include "annotation_model_tests.moc"
