#include "okuflow/common/uct_ink_outline.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QtTest>

using namespace okuflow::uct;
class InkOutlineTests : public QObject
{
    Q_OBJECT
    static ink::Region Region(int width = 160, int height = 96)
    {
        return {0, 0, width, height, {220, 220, 220}, {40, 40, 40}};
    }
    static QImage Image(int width = 160, int height = 96)
    {
        QImage image(width, height, QImage::Format_ARGB32);
        image.fill(QColor(40, 40, 40));
        return image;
    }
  private slots:
    void outlinesKeepStacksExponentsAndHoles()
    {
        auto image = Image();
        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(220, 220, 220));
        painter.drawRect(QRect(20, 16, 5, 16));
        painter.drawRect(QRect(14, 40, 22, 5));
        painter.drawRect(QRect(20, 54, 14, 6)); // a vertical stack, no reading-order inference
        painter.drawRect(QRect(68, 48, 20, 6));
        painter.drawRect(QRect(102, 18, 8, 7)); // raised island
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(220, 220, 220), 5));
        painter.drawEllipse(QRect(118, 48, 24, 24));
        painter.end();
        const auto outline = ink::Build(image, Region());
        QVERIFY2(outline.geometry, ink::Name(outline.refusal));
        QCOMPARE(outline.components[2], 6);
        QCOMPARE(outline.holes[2], 1);
        QVERIFY(outline.geometry->path.contains(QPointF(22, 20)));
        QVERIFY(outline.geometry->path.contains(QPointF(22, 42)));
        QVERIFY(outline.geometry->path.contains(QPointF(22, 56)));
        QVERIFY(outline.geometry->path.contains(QPointF(105, 20)));
        QVERIFY(!outline.geometry->path.contains(QPointF(130, 60)));
        QVERIFY(outline.vertices <= ink::MaximumVertices);
    }
    void borderAndContrastRefuse()
    {
        auto image = Image();
        QPainter painter(&image);
        painter.fillRect(QRect(0, 20, 30, 5), QColor(220, 220, 220));
        painter.fillRect(QRect(70, 50, 60, 8), QColor(220, 220, 220));
        painter.end();
        QCOMPARE(ink::Build(image, Region()).refusal, ink::Refusal::BorderInk);
        auto region = Region();
        region.foreground[0] = region.foreground[1] = region.foreground[2] = 45;
        QCOMPARE(ink::Build(image, region).refusal, ink::Refusal::LowContrast);
        QCOMPARE(ink::Build(Image(513, 96), Region(513, 96)).refusal, ink::Refusal::Bounds);
        auto outside = Region();
        outside.x = std::numeric_limits<int>::max();
        QCOMPARE(ink::Build(image, outside).refusal, ink::Refusal::Bounds);
    }
    void faintResolvedStrokeRefuses()
    {
        auto image = Image();
        QPainter painter(&image);
        painter.fillRect(QRect(20, 20, 60, 8), QColor(220, 220, 220));
        painter.fillRect(QRect(20, 50, 60, 5), QColor(105, 105, 105));
        painter.end();
        QCOMPARE(ink::Build(image, Region()).refusal, ink::Refusal::FaintInk);
    }
    void partlyFaintStrokeDoesNotBecomeADot()
    {
        auto image = Image();
        QPainter p(&image);
        p.fillRect(QRect(20, 20, 90, 8), QColor(220, 220, 220));
        p.fillRect(QRect(20, 55, 90, 5), QColor(105, 105, 105));
        p.fillRect(QRect(63, 57, 1, 1), QColor(220, 220, 220));
        p.end();
        QCOMPARE(ink::Build(image, Region()).refusal, ink::Refusal::FaintInk);
    }
    void weakTinyIslandKeepsOriginalInsteadOfAmplifyingTexture()
    {
        auto image = Image();
        QPainter p(&image);
        p.fillRect(QRect(20, 20, 100, 8), QColor(220, 220, 220));
        p.end();
        image.setPixelColor(60, 60, QColor(140, 140, 140));
        image.setPixelColor(100, 60, QColor(220, 220, 220));
        const auto outline = ink::Build(image, Region());
        QVERIFY2(outline.geometry, ink::Name(outline.refusal));
        QVERIFY(!outline.geometry->path.contains(QPointF(60.5, 60.5)));
        QVERIFY(outline.geometry->path.contains(QPointF(100.5, 60.5)));
        QCOMPARE(image.pixelColor(60, 60), QColor(140, 140, 140)); // omission leaves source pixels intact
    }
    void nestedRingsAndInnerMarksKeepTheirOwners()
    {
        auto image = Image();
        QPainter p(&image);
        p.setPen(QPen(QColor(220, 220, 220), 5));
        p.drawEllipse(QRectF(43, 8, 76, 80));
        p.drawEllipse(QRectF(69, 23, 24, 20));
        p.drawEllipse(QRectF(69, 54, 24, 20));
        p.fillRect(QRect(104, 46, 4, 4), QColor(220, 220, 220));
        p.end();
        const auto outline = ink::Build(image, Region());
        QVERIFY2(outline.geometry, ink::Name(outline.refusal));
        QCOMPARE(outline.components[2], 4);
        QCOMPARE(outline.holes[2], 3);
        QVERIFY(outline.geometry->path.contains(QPointF(105, 47)));
        QVERIFY(!outline.geometry->path.contains(QPointF(81, 33)));
        QVERIFY(!outline.geometry->path.contains(QPointF(81, 64)));
    }
    void thinResolvedStrokesRefuse()
    {
        auto image = Image();
        QPainter painter(&image);
        for (int y = 12; y < 90; y += 8)
            painter.fillRect(QRect(20, y, 120, 1), QColor(220, 220, 220));
        painter.end();
        QCOMPARE(ink::Build(image, Region()).refusal, ink::Refusal::ThinInk);
    }
    void fractionalEdgesAndTinyPinholesRemain()
    {
        auto image = Image();
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(220, 220, 220), 5));
        painter.drawLine(QPointF(20.25, 20.25), QPointF(100.25, 65.25));
        painter.end();
        const auto outline = ink::Build(image, Region());
        QVERIFY2(outline.geometry, ink::Name(outline.refusal));
        bool fractional = false;
        for (const auto &loop : outline.geometry->loops)
            for (auto p : loop)
                fractional = fractional || p.x() != std::floor(p.x()) || p.y() != std::floor(p.y());
        QVERIFY(fractional);
        image = Image();
        QPainter fill(&image);
        fill.fillRect(QRect(20, 20, 100, 40), QColor(220, 220, 220));
        fill.end();
        image.setPixelColor(70, 40, QColor(40, 40, 40));
        const auto hole = ink::Build(image, Region());
        QVERIFY2(hole.geometry, ink::Name(hole.refusal));
        QVERIFY(!hole.geometry->path.contains(QPointF(70.5, 40.5))); // do not fill the small hole
    }
    void noisySaddlesRemainClosed()
    {
        for (unsigned seed = 1; seed <= 12; ++seed)
        {
            auto image = Image();
            unsigned value = seed;
            for (int y = 20; y < 76; ++y)
                for (int x = 20; x < 140; ++x)
                {
                    value = value * 1664525u + 1013904223u;
                    const int shade = 65 + int(value >> 24) * 155 / 255;
                    image.setPixelColor(x, y, QColor(shade, shade, shade));
                }
            const auto result = ink::Build(image, Region());
            QVERIFY2(result.refusal != ink::Refusal::OpenContour,
                     "Closed source ink produced an open directed contour.");
        }
    }
    void optionalFrozenImage()
    {
        const auto manifest = qEnvironmentVariable("OKUFLOW_UCT_INK_OUTLINE_MANIFEST");
        if (manifest.isEmpty())
            QSKIP("Explicit private frozen-image manifest required.");
        QFile file(manifest);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto rows = QJsonDocument::fromJson(file.readAll()).array();
        QVERIFY(!rows.isEmpty());
        for (const auto &row : rows)
        {
            const auto object = row.toObject();
            QImage image(object["image"].toString());
            QVERIFY(!image.isNull());
            const auto bounds = object["region"].toArray();
            const QColor fg(object["foreground"].toString()), bg(object["background"].toString());
            ink::Region region{bounds[0].toInt(),
                               bounds[1].toInt(),
                               bounds[2].toInt(),
                               bounds[3].toInt(),
                               {fg.red(), fg.green(), fg.blue()},
                               {bg.red(), bg.green(), bg.blue()}};
            QElapsedTimer timer;
            timer.start();
            const auto outline = ink::Build(image, region);
            const auto ms = timer.nsecsElapsed() / 1e6;
            QJsonObject report{{"image", object["image"]},
                               {"decision", ink::Name(outline.refusal)},
                               {"milliseconds", ms},
                               {"vertices", outline.vertices},
                               {"components", outline.components[2]},
                               {"holes", outline.holes[2]}};
            qInfo().noquote() << QJsonDocument(report).toJson(QJsonDocument::Compact);
            if (outline.geometry)
            {
                const int scale = 4;
                QImage result(region.w * scale, region.h * scale, QImage::Format_ARGB32);
                result.fill(outline.background);
                QPainter p(&result);
                p.setRenderHint(QPainter::Antialiasing);
                p.scale(scale, scale);
                p.translate(-region.x, -region.y);
                p.fillPath(outline.geometry->path, Qt::white);
                p.end();
                QVERIFY(result.save(object["output"].toString()));
            }
        }
    }
};
QTEST_GUILESS_MAIN(InkOutlineTests)
#include "uct_ink_outline_tests.moc"
