#pragma once

#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/view_overlay_snapshot.hpp"
#include "okuflow/common/annotation_model.hpp"
#include "ultra_clear_text_test_support.hpp"

#include <QPainter>
#include <QDir>
#include <QUuid>
#include <QtTest>
#include <future>

namespace uct_capture_test {

inline QImage Composite(const okuflow::UctOverlaySnapshot& text, QSize size)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(QColor(82, 94, 108));
    QPainter painter(&image);
    text.Paint(painter, size);
    return image;
}

inline void FrozenExportMatchesGlyphsAndSurvivesLiveChanges()
{
    uct_test::RegisterFixtureFonts();
    QWidget owner;
    owner.resize(640, 360);
    QWidget viewport(&owner);
    viewport.setGeometry(owner.rect());
    okuflow::UltraClearTextOverlay overlay(&viewport, &owner);
    okuflow::ViewTransform transform;
    transform.valid = true;
    overlay.SetViewTransform(transform);
    const okuflow::UltraClearTextOverlay::SceneWord word{
        QStringLiteral("Clear"), QRectF(.15, .25, .35, .20),
        {QColor(Qt::white), QColor(Qt::black)}, 1, {}, 0};
    owner.show();
    viewport.show();
    overlay.SetWords({word});
    QCoreApplication::processEvents();
    QVERIFY(overlay.isVisible());

    QImage onScreen(owner.size(), QImage::Format_ARGB32);
    onScreen.fill(QColor(82, 94, 108));
    overlay.render(&onScreen, QPoint(), QRegion(), QWidget::DrawChildren);
    const auto generation = overlay.PaintedGeneration();
    const auto paints = overlay.PaintCount();
    const auto snapshot = overlay.CaptureSnapshot(transform, owner.size());
    QVERIFY(!snapshot.IsEmpty());
    const auto exported = Composite(snapshot, owner.size());
    // QWidget::render composites a premultiplied backing layer; direct vector
    // replay can round an antialiased edge by one channel unit. Geometry and
    // alpha must agree, with no larger colour difference anywhere.
    int maximumChannelError=0, maximumAlphaError=0;
    for(int y=0;y<exported.height();++y)for(int x=0;x<exported.width();++x) {
        const auto a=exported.pixel(x,y),b=onScreen.pixel(x,y);
        maximumChannelError=std::max({maximumChannelError,std::abs(qRed(a)-qRed(b)),
            std::abs(qGreen(a)-qGreen(b)),std::abs(qBlue(a)-qBlue(b))});
        maximumAlphaError=std::max(maximumAlphaError,std::abs(qAlpha(a)-qAlpha(b)));
    }
    if (maximumChannelError>1 || maximumAlphaError) {
        const auto dir=QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(
            QStringLiteral("OkuFlow/test-evidence/uct-export-")+QUuid::createUuid().toString(QUuid::Id128));
        QVERIFY(QDir().mkpath(dir));
        exported.save(dir+QStringLiteral("/frozen.png"));
        onScreen.save(dir+QStringLiteral("/screen.png"));
        qInfo().noquote()<<"Export mismatch evidence:"<<dir;
        qInfo()<<"DPI image/picture"<<exported.logicalDpiX()<<snapshot.picture.logicalDpiX();
        bool found=false;
        for(int y=0;y<exported.height()&&!found;++y)for(int x=0;x<exported.width();++x)
            if(exported.pixel(x,y)!=onScreen.pixel(x,y)) {
                qInfo()<<"First mismatched pixel"<<x<<y<<exported.pixelColor(x,y)<<onScreen.pixelColor(x,y);
                found=true;break;
            }
    }
    QVERIFY(maximumChannelError<=1);
    QCOMPARE(maximumAlphaError,0);
    int brightInk=0;
    for(int y=85;y<170;++y)for(int x=90;x<330;++x) {
        const auto pixel=exported.pixelColor(x,y);
        if(pixel.red()>230 && pixel.green()>230 && pixel.blue()>230)++brightInk;
    }
    QVERIFY(brightInk>100); // a plate without sharp letters is not an export
    QCOMPARE(overlay.PaintedGeneration(), generation);
    QCOMPARE(overlay.PaintCount(), paints);

    // A delayed writer owns vector commands, not a QWidget or a live word
    // list. This exercises painting on a worker after the original changed.
    overlay.SetStrokeWeight(3);
    transform.sourceX = .25f;
    transform.sourceWidth = .5f;
    overlay.SetViewTransform(transform);
    overlay.ClearWords();
    auto worker = std::async(std::launch::async,
        [snapshot] { return Composite(snapshot, QSize(640, 360)); });
    QCOMPARE(worker.get(), exported);
    QVERIFY(overlay.CaptureSnapshot(transform, owner.size()).IsEmpty());

    // High-DPI export remains vector, with the same logical placement.
    const auto doubled = Composite(snapshot, QSize(1280, 720));
    QCOMPARE(doubled.pixelColor(10, 10), QColor(82, 94, 108));
    QCOMPARE(doubled.pixelColor(180, 200), exported.pixelColor(90, 100));
    QVERIFY(doubled != exported.scaled(doubled.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation));
}

inline void ExportProtectsOmissionsAndKeepsInkAboveText()
{
    uct_test::RegisterFixtureFonts();
    QWidget owner;
    owner.resize(640, 360);
    QWidget viewport(&owner);
    viewport.setGeometry(owner.rect());
    okuflow::UltraClearTextOverlay overlay(&viewport, &owner);
    okuflow::ViewTransform transform;
    transform.valid = true;
    overlay.SetViewTransform(transform);
    auto word = okuflow::UltraClearTextOverlay::SceneWord{
        QStringLiteral("Clear"), QRectF(.15, .25, .35, .20),
        {QColor(Qt::white), QColor(Qt::black)}, 1, {}, 0};
    owner.show(); viewport.show();
    overlay.SetWords({word});
    QCoreApplication::processEvents();

    // UI chrome is absent from a photo: it must not leave a hole in UCT.
    QWidget chrome(&owner);
    chrome.setObjectName(QStringLiteral("topLeftPanel"));
    chrome.setGeometry(90, 85, 240, 90);
    chrome.show();
    QCoreApplication::processEvents();
    const auto text = overlay.CaptureSnapshot(transform, owner.size());
    QVERIFY(!text.IsEmpty());
    auto image = Composite(text, owner.size());
    QCOMPARE(image.pixelColor(90, 100), QColor(Qt::black));
    const auto clean = image.copy();

    okuflow::AnnotationStroke ink;
    ink.kind = okuflow::AnnotationItemKind::kFreehand;
    ink.color = Qt::yellow;
    ink.sceneWidth = .025;
    ink.points = {QPointF(.15, .35), QPointF(.5, .35)};
    {
        QPainter painter(&image);
        okuflow::ViewOverlaySnapshot overlays{text, {ink}, transform};
        overlays.Paint(painter, image.size());
    }
    QCOMPARE(image.pixelColor(200, 126), QColor(Qt::yellow));
    QVERIFY(image != clean);
    QCOMPARE(clean, Composite(text, owner.size()));

    // An uncertain neighboring token protects its source pixels from both
    // plates and glyphs, including a plate's padding.
    word.protectedSceneRects = {QRectF(.10, .20, .45, .30)};
    overlay.SetWords({word});
    QVERIFY(overlay.CaptureSnapshot(transform, owner.size()).IsEmpty());
    overlay.SetWords({});
    QVERIFY(overlay.CaptureSnapshot(transform, owner.size()).IsEmpty());
}

} // namespace uct_capture_test
