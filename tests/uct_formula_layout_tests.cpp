#include "okuflow/common/uct_formula_artifact.hpp"
#include <QtTest>
#include <QPainter>
#include <QFontDatabase>

class FormulaLayoutTests final:public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {QVERIFY(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"))>=0);}
    void fractionsAndSuperscriptsAreVectorPaths() {
        const auto layout=okuflow::uct::formula::Typeset(QStringLiteral("K = \\frac{1}{2} m v^{2}"));
        QVERIFY2(layout.path,qPrintable(layout.error));
        QVERIFY(layout.path->elementCount()>100);
        const auto b=layout.path->boundingRect();
        QVERIFY(qAbs(b.x())<0.001 && qAbs(b.y())<0.001);
        QVERIFY(qAbs(b.width()-1)<0.001 && qAbs(b.height()-1)<0.001);
        QImage image(1200,400,QImage::Format_ARGB32);image.fill(Qt::black);
        QPainter painter(&image);painter.scale(1200,400);painter.fillPath(*layout.path,Qt::yellow);painter.end();
        int pixels=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)pixels+=image.pixelColor(x,y).red()>128;
        QVERIFY(pixels>1000);
    }
    void unsafeOrUnboundedLayoutKeepsOriginal() {
        for(const auto& text:{QStringLiteral("\\input{private}"),QStringLiteral("\\newcommand{\\x}{x}\\x"),QString(9000,'x'),QStringLiteral("\\begin{document}x\\end{document}")})
            QVERIFY(!okuflow::uct::formula::Typeset(text).path);
    }
};
QTEST_MAIN(FormulaLayoutTests)
#include "uct_formula_layout_tests.moc"
