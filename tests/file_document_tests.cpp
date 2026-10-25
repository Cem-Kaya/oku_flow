#include "okuflow/common/file_document.hpp"
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QFontDatabase>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>

using namespace okuflow;
namespace {
bool MakePdf(const QString& path) {
    QPdfWriter writer(path);
    writer.setResolution(144);
    QPainter painter(&writer);
    if (!painter.isActive()) return false;
    painter.setFont(QFont("Arial", 24));
    painter.drawText(QRect(100, 100, 1000, 180), QStringLiteral("Embedded document text"));
    if (!writer.newPage()) return false;
    QImage scan(640, 240, QImage::Format_RGB32); scan.fill(Qt::white);
    { QPainter ink(&scan); ink.setFont(QFont("Arial", 24)); ink.drawText(scan.rect(), Qt::AlignCenter, "Scanned picture"); }
    painter.drawImage(QRect(100, 100, 1000, 375), scan);
    if (!writer.newPage()) return false;
    painter.drawText(QRect(100, 100, 1000, 180), QStringLiteral("Last page"));
    return painter.end();
}
}
class FileDocumentTests final : public QObject {
    Q_OBJECT
private slots:
    void picturesAndInvalidInputs() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QImage image(96, 48, QImage::Format_RGB32); image.fill(QColor(22, 103, 177));
        const auto path = dir.filePath("picture.png"); QVERIFY(image.save(path));
        QVERIFY(FileDocument::SupportsPath(path)); QVERIFY(FileDocument::SupportsPath("BOOK.PDF"));
        QVERIFY(!FileDocument::SupportsPath("movie.mp4"));
        FileDocument source; const auto generation = source.Open(path, 512);
        QTRY_COMPARE_WITH_TIMEOUT(source.Snapshot().state, FileDocumentSnapshot::State::Ready, 5000);
        const auto result = source.TakeSnapshot();
        QCOMPARE(result.generation, generation); QCOMPARE(result.image.size(), image.size());
        QCOMPARE(result.image.pixelColor(30, 20), image.pixelColor(30, 20));
        QVERIFY(result.pixels); QCOMPARE(result.pixels->size(), size_t(96 * 48 * 4));
        QCOMPARE((*result.pixels)[0], std::uint8_t(177));
        QCOMPARE((*result.pixels)[1], std::uint8_t(103));
        QCOMPARE((*result.pixels)[2], std::uint8_t(22));
        QCOMPARE(result.pageCount, 1); QVERIFY(result.text.isEmpty()); QVERIFY(!result.pdf);
        QVERIFY(source.Snapshot().image.isNull());
        QVERIFY(!source.Snapshot().pixels);
        source.Open(dir.filePath("missing.pdf"));
        QTRY_COMPARE_WITH_TIMEOUT(source.Snapshot().state, FileDocumentSnapshot::State::Failed, 5000);
        QVERIFY(!source.Snapshot().error.isEmpty());
        source.Close(); QCOMPARE(source.Snapshot().state, FileDocumentSnapshot::State::Closed);
    }
    void embeddedAndScannedPdfPagesStayDistinct() {
        QTemporaryDir dir; const auto path = dir.filePath("mixed.pdf"); QVERIFY(MakePdf(path));
        FileDocument source; source.Open(path, 512);
        QTRY_COMPARE_WITH_TIMEOUT(source.Snapshot().state, FileDocumentSnapshot::State::Ready, 5000);
        const auto first = source.Snapshot();
        QVERIFY(first.pdf); QCOMPARE(first.pageCount, 3); QVERIFY(!first.image.isNull());
        QVERIFY(first.text.contains("Embedded document text"));
        source.SelectPage(1, 512);
        QTRY_COMPARE_WITH_TIMEOUT(source.Snapshot().state, FileDocumentSnapshot::State::Ready, 5000);
        const auto scanned = source.Snapshot(); QCOMPARE(scanned.page, 1);
        QCOMPARE(scanned.documentGeneration, first.documentGeneration);
        QVERIFY(scanned.text.trimmed().isEmpty()); QVERIFY(!scanned.image.isNull());
        QVERIFY(scanned.image != first.image);
        QCOMPARE(source.SelectPage(3), std::uint64_t(0));
        source.SelectPage(2, 8192);
        QTRY_COMPARE_WITH_TIMEOUT(source.Snapshot().state, FileDocumentSnapshot::State::Ready, 5000);
        const auto last = source.Snapshot(); QVERIFY(last.text.contains("Last page"));
        QVERIFY(qint64(last.image.width()) * last.image.height() <= 16ll * 1024 * 1024);
        QVERIFY(std::max(last.image.width(), last.image.height()) <= 8192);
    }
    void newestOpenAndCloseFencePublication() {
        QTemporaryDir dir; const auto pdf = dir.filePath("book.pdf"); QVERIFY(MakePdf(pdf));
        QImage image(64, 32, QImage::Format_RGB32); image.fill(Qt::green);
        const auto path = dir.filePath("new.png"); QVERIFY(image.save(path));
        FileDocument source;
        source.Open(pdf, 8192); const auto newest = source.Open(path, 512);
        QTRY_COMPARE_WITH_TIMEOUT(source.Snapshot().state, FileDocumentSnapshot::State::Ready, 5000);
        QCOMPARE(source.Snapshot().generation, newest); QCOMPARE(source.Snapshot().path, path);
        source.Open(pdf, 8192); source.Close();
        QTest::qWait(100); QCOMPARE(source.Snapshot().state, FileDocumentSnapshot::State::Closed);
        source.Open(pdf, 512);
        QTRY_COMPARE_WITH_TIMEOUT(source.Snapshot().state, FileDocumentSnapshot::State::Ready, 5000);
        source.SelectPage(1, 512); const auto last = source.SelectPage(2, 512);
        QTRY_COMPARE_WITH_TIMEOUT(source.Snapshot().state, FileDocumentSnapshot::State::Ready, 5000);
        QCOMPARE(source.Snapshot().generation, last); QVERIFY(source.Snapshot().text.contains("Last page"));
    }
};
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf"));
    FileDocumentTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "file_document_tests.moc"
