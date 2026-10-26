#include "okuflow/common/file_document.hpp"
#include "okuflow/common/media_frame_layout.hpp"
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QFontDatabase>
#include <QPainter>
#include <QPdfWriter>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QTest>
#include <windows.h>

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
bool MakeCroppedRotatedPdf(const QString& path) {
    QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>"};
    for (const int rotate : {0,90,270}) objects.append(QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [10 20 410 620] /CropBox [50 80 350 560] /Rotate ")+
        QByteArray::number(rotate)+" /Resources << /Font << /F1 6 0 R >> >> /Contents 7 0 R >>");
    objects.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    const QByteArray content="BT /F1 12 Tf 80 400 Td (Native cropped page) Tj ET\n";
    objects.append("<< /Length "+QByteArray::number(content.size())+" >>\nstream\n"+content+"endstream");
    QByteArray bytes="%PDF-1.4\n"; QList<int> offsets{0};
    for(int index=0;index<objects.size();++index) {
        offsets.append(bytes.size()); bytes+=QByteArray::number(index+1)+" 0 obj\n"+objects[index]+"\nendobj\n";
    }
    const auto xref=bytes.size(); bytes+="xref\n0 "+QByteArray::number(objects.size()+1)+"\n0000000000 65535 f \n";
    for(int index=1;index<offsets.size();++index) bytes+=QByteArray::number(offsets[index]).rightJustified(10,'0')+" 00000 n \n";
    bytes+="trailer\n<< /Size "+QByteArray::number(objects.size()+1)+" /Root 1 0 R >>\nstartxref\n"+QByteArray::number(xref)+"\n%%EOF\n";
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size();
}
QByteArray Bytes(const QString& path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
QRect RedTextBounds(const QImage& input) {
    const auto image=input.convertToFormat(QImage::Format_ARGB32);
    QRect result;
    for(int y=0;y<image.height()/2;++y) {
        const auto* row=reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for(int x=0;x<image.width();++x) if(qRed(row[x])>200 && qGreen(row[x])<80 && qBlue(row[x])<80)
            result=result.isNull()?QRect(x,y,1,1):result.united(QRect(x,y,1,1));
    }
    return result;
}
int RedLineWidth(const QImage& image) {
    int count=0;
    for(int y=image.height()/2;y<image.height();++y) {
        const auto color=image.pixelColor(image.width()/2,y);
        if(color.red()>200 && color.green()<80 && color.blue()<80) ++count;
    }
    return count;
}
}
class FileDocumentTests final : public QObject {
    Q_OBJECT
private slots:
    void highResolutionExportsPreserveRelativeTextAndStrokeSizes() {
        QTemporaryDir dir;
        AnnotationModel model; model.AddText({0.1,0.1},"M",Qt::red,0.1);
        model.BeginStroke({0.2,0.75},Qt::red,0.05); model.AppendStrokePoint({0.8,0.75},0.001); model.EndStroke();
        const auto savePicture=[&](int size) {
            const auto source=dir.filePath(QString::number(size)+".png");
            QImage image(size,size,QImage::Format_RGB32); image.fill(Qt::white);
            if(!image.save(source))return FileAnnotationCopy{};
            return SaveFileAnnotations(source,dir.filePath("copies"),{{0,model.strokes()}});
        };
        const auto small=savePicture(1000),large=savePicture(4000);
        QVERIFY2(small.committed,qPrintable(small.error)); QVERIFY2(large.committed,qPrintable(large.error));
        const QImage reference(small.path),full(large.path);
        const auto smallText=RedTextBounds(reference),largeText=RedTextBounds(full);
        QVERIFY(smallText.height()>40);
        QVERIFY(std::abs(largeText.height()/4.0-smallText.height())<3.0);
        QVERIFY(std::abs(largeText.width()/4.0-smallText.width())<3.0);
        QVERIFY(std::abs(RedLineWidth(reference)-50)<=2);
        QVERIFY(std::abs(RedLineWidth(full)-200)<=2);
        const auto source=dir.filePath("large-page.pdf");
        { QPdfWriter writer(source); writer.setResolution(144);
          writer.setPageSize(QPageSize(QSizeF(2000,2000),QPageSize::Point));
          writer.setPageMargins(QMarginsF(0,0,0,0));
          QPainter painter(&writer); painter.fillRect(QRect(0,0,writer.width(),writer.height()),Qt::white); }
        const auto pdfCopy=SaveFileAnnotations(source,dir.filePath("copies"),{{0,model.strokes()}});
        QVERIFY2(pdfCopy.committed,qPrintable(pdfCopy.error));
        QPdfDocument pdf; QCOMPARE(pdf.load(pdfCopy.path),QPdfDocument::Error::None);
        const auto page=pdf.render(0,QSize(1000,1000),{},QImage::Format_ARGB32,Qt::white);
        QVERIFY(std::abs(RedTextBounds(page).height()-smallText.height())<3);
        QVERIFY(std::abs(RedLineWidth(page)-50)<=2);
    }
    void failedSidecarCommitRecoversTheSameWorkingCopy() {
        QTemporaryDir dir; const auto source=dir.filePath("original.png"),directory=dir.filePath("copies");
        QImage image(128,128,QImage::Format_RGB32); image.fill(Qt::white); QVERIFY(image.save(source));
        AnnotationModel model; model.BeginStroke({0.1,0.3},Qt::red,0.05);
        model.AppendStrokePoint({0.4,0.3},0.001); model.EndStroke();
        const auto first=SaveFileAnnotations(source,directory,{{0,model.strokes()}}); QVERIFY(first.committed);
        const auto oldOutput=Bytes(first.path),oldState=Bytes(first.path+".ink.json");
        model.BeginStroke({0.6,0.7},Qt::blue,0.05); model.AppendStrokePoint({0.9,0.7},0.001); model.EndStroke();
        HANDLE held=INVALID_HANDLE_VALUE;
        const auto release=qScopeGuard([&] {if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);});
        QByteArray pendingBytes;
        FileAnnotationSaveHooks hooks;
        hooks.documentPublished=[&] {
            pendingBytes=Bytes(first.path+".ink.pending.json");
            held=CreateFileW(reinterpret_cast<LPCWSTR>((first.path+".ink.json").utf16()),
                GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        };
        const auto failed=SaveFileAnnotations(source,directory,{{0,model.strokes()}},{},hooks);
        QVERIFY(held!=INVALID_HANDLE_VALUE); QVERIFY(!failed.committed); QVERIFY(failed.recoveryPending);
        QVERIFY(!pendingBytes.isEmpty()); QVERIFY(Bytes(first.path)!=oldOutput); QCOMPARE(Bytes(first.path+".ink.json"),oldState);
        const auto readBlocked=LoadFileAnnotations(source,directory);
        QVERIFY2(readBlocked.error.isEmpty(),qPrintable(readBlocked.error)); QVERIFY(readBlocked.recoveryPending);
        QCOMPARE(readBlocked.pages[0].size(),2);
        const auto retryBlocked=SaveFileAnnotations(source,directory,{{0,model.strokes()}});
        QVERIFY(!retryBlocked.committed); QVERIFY(retryBlocked.recoveryPending);
        CloseHandle(held); held=INVALID_HANDLE_VALUE;
        const auto recovered=LoadFileAnnotations(source,directory);
        QVERIFY2(recovered.error.isEmpty(),qPrintable(recovered.error)); QVERIFY(!recovered.recoveryPending);
        QCOMPARE(recovered.path,first.path); QCOMPARE(recovered.pages[0].size(),2);
        QVERIFY(!QFileInfo::exists(first.path+".ink.pending.json"));
        // Replay a crash after both commits but before journal cleanup.
        { QFile pending(first.path+".ink.pending.json"); QVERIFY(pending.open(QIODevice::WriteOnly|QIODevice::NewOnly));
          QCOMPARE(pending.write(pendingBytes),pendingBytes.size()); }
        QVERIFY(LoadFileAnnotations(source,directory).error.isEmpty());
        QVERIFY(!QFileInfo::exists(first.path+".ink.pending.json"));
        const auto next=SaveFileAnnotations(source,directory,{{0,model.strokes()}});
        QVERIFY2(next.committed,qPrintable(next.error)); QCOMPARE(next.path,first.path);
        QCOMPARE(QDir(directory).entryList({"*.png"},QDir::Files).size(),1);
        QCOMPARE(QImage(source),image);
    }
    void unpublishedSaveKeepsThePreviousPairAndExternalChangesArePreserved() {
        QTemporaryDir dir; const auto source=dir.filePath("original.png"),directory=dir.filePath("copies");
        QImage image(128,128,QImage::Format_RGB32); image.fill(Qt::white); QVERIFY(image.save(source));
        AnnotationModel model; model.BeginStroke({0.1,0.3},Qt::red,0.05);
        model.AppendStrokePoint({0.4,0.3},0.001); model.EndStroke();
        const auto first=SaveFileAnnotations(source,directory,{{0,model.strokes()}}); QVERIFY(first.committed);
        const auto oldOutput=Bytes(first.path),oldState=Bytes(first.path+".ink.json");
        HANDLE held=CreateFileW(reinterpret_cast<LPCWSTR>(first.path.utf16()),GENERIC_READ,
            FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        QVERIFY(held!=INVALID_HANDLE_VALUE);
        const auto release=qScopeGuard([&] {if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);});
        model.Clear();
        const auto failed=SaveFileAnnotations(source,directory,{}); QVERIFY(!failed.committed);
        QCOMPARE(Bytes(first.path),oldOutput); QCOMPARE(Bytes(first.path+".ink.json"),oldState);
        CloseHandle(held); held=INVALID_HANDLE_VALUE;
        const auto restored=LoadFileAnnotations(source,directory);
        QVERIFY2(restored.error.isEmpty(),qPrintable(restored.error)); QCOMPARE(restored.pages[0].size(),1);
        QVERIFY(!QFileInfo::exists(first.path+".ink.pending.json"));
        FileAnnotationSaveHooks hooks;
        hooks.documentPublished=[&] {
            held=CreateFileW(reinterpret_cast<LPCWSTR>((first.path+".ink.json").utf16()),
                GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        };
        const auto interrupted=SaveFileAnnotations(source,directory,{}, {},hooks);
        QVERIFY(!interrupted.committed); QVERIFY(interrupted.recoveryPending);
        CloseHandle(held); held=INVALID_HANDLE_VALUE;
        { QFile external(first.path); QVERIFY(external.open(QIODevice::Append)); external.write("external change"); }
        const auto changed=Bytes(first.path);
        QVERIFY(!LoadFileAnnotations(source,directory).error.isEmpty());
        QVERIFY(!SaveFileAnnotations(source,directory,{}).committed);
        QCOMPARE(Bytes(first.path),changed); QCOMPARE(Bytes(first.path+".ink.json"),oldState);
        QVERIFY(QFileInfo::exists(first.path+".ink.pending.json"));
    }
    void pdfInkTracksCropBoxesAndPageRotation() {
        QTemporaryDir dir; const auto path=dir.filePath("rotated.pdf"); QVERIFY(MakeCroppedRotatedPdf(path));
        AnnotationModel ink; ink.BeginStroke({0.2,0.3},Qt::red,0.03);
        ink.AppendStrokePoint({0.4,0.3},0.001); ink.EndStroke();
        const auto saved=SaveFileAnnotations(path,dir.filePath("copies"),{{0,ink.strokes()},{1,ink.strokes()},{2,ink.strokes()}});
        QVERIFY2(saved.committed,qPrintable(saved.error));
        QPdfDocument pdf; QCOMPARE(pdf.load(saved.path),QPdfDocument::Error::None);
        for(int page=0;page<3;++page) {
            const auto size=pdf.pagePointSize(page).toSize()*2;
            const auto image=pdf.render(page,size); QVERIFY(!image.isNull());
            const auto color=image.pixelColor(int(size.width()*0.3),int(size.height()*0.3));
            QVERIFY2(color.red()>200 && color.green()<80,qPrintable(QString::number(page)+":"+color.name()));
            QVERIFY(pdf.getAllText(page).text().contains("Native cropped page"));
        }
    }
    void saveUpdatesOnePdfAcrossPagesAndRetainsNativeText() {
        QTemporaryDir dir; const auto source = dir.filePath("original.pdf"); QVERIFY(MakePdf(source));
        QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly)); const auto originalBytes = original.readAll(); original.close();
        AnnotationModel heart; heart.BeginStroke({0.2,0.3},Qt::red,0.02);
        heart.AppendStrokePoint({0.4,0.3},0.001); heart.EndStroke();
        FilePageInk pages{{0,heart.strokes()}};
        const auto directory = dir.filePath("OkuFlow/Documents");
        const auto first = SaveFileAnnotations(source,directory,pages);
        QVERIFY2(first.committed,qPrintable(first.error));
        AnnotationModel star; star.BeginStroke({0.6,0.7},Qt::blue,0.02);
        star.AppendStrokePoint({0.8,0.7},0.001); star.EndStroke(); pages[2]=star.strokes();
        const auto second = SaveFileAnnotations(source,directory,pages);
        QVERIFY2(second.committed,qPrintable(second.error)); QCOMPARE(second.path,first.path);
        QCOMPARE(QDir(directory).entryList({"*.pdf"},QDir::Files).size(),1);
        const auto restored = LoadFileAnnotations(source,directory);
        QVERIFY2(restored.error.isEmpty(),qPrintable(restored.error)); QCOMPARE(restored.pages.size(),2);
        QCOMPARE(restored.pages[0][0].points,heart.strokes()[0].points);
        QCOMPARE(restored.pages[2][0].points,star.strokes()[0].points);
        { QPdfDocument pdf; QCOMPARE(pdf.load(second.path),QPdfDocument::Error::None); QCOMPARE(pdf.pageCount(),3);
        QVERIFY(pdf.getAllText(0).text().contains("Embedded document text"));
        QVERIFY(pdf.getAllText(2).text().contains("Last page"));
        for (const auto pair : {std::pair{0,QPointF(0.3,0.3)},std::pair{2,QPointF(0.7,0.7)}}) {
            const auto image = pdf.render(pair.first,QSize(600,800)); QVERIFY(!image.isNull());
            const auto color = image.pixelColor(int(pair.second.x()*600),int(pair.second.y()*800));
            if (pair.first==0) QVERIFY2(color.red()>200 && color.blue()<80,qPrintable(color.name()));
            else QVERIFY2(color.blue()>200 && color.red()<80,qPrintable(color.name()));
        }
        }
        pages.remove(0);
        const auto cleared = SaveFileAnnotations(source,directory,pages); QVERIFY2(cleared.committed,qPrintable(cleared.error));
        QCOMPARE(cleared.path,first.path); QCOMPARE(LoadFileAnnotations(source,directory).pages.size(),1);
        { QPdfDocument pdf; QCOMPARE(pdf.load(cleared.path),QPdfDocument::Error::None);
          const auto image=pdf.render(0,QSize(600,800),{},QImage::Format_ARGB32,Qt::white);
          QCOMPARE(image.pixelColor(180,240),QColor(Qt::white)); }
        QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(),originalBytes);
        QFile foreign(cleared.path); QVERIFY(foreign.open(QIODevice::Append)); foreign.write("external edit"); foreign.close();
        const auto refused = SaveFileAnnotations(source,directory,pages); QVERIFY(!refused.committed); QVERIFY(!refused.error.isEmpty());
        QVERIFY(foreign.open(QIODevice::ReadOnly)); QVERIFY(foreign.readAll().endsWith("external edit"));
        original.close();
        QFile changed(source); QVERIFY(changed.open(QIODevice::Append)); changed.write("\n%changed original\n"); changed.close();
        const auto stale=SaveFileAnnotations(source,directory,pages,first.sourceSha256);
        QVERIFY(!stale.committed); QVERIFY(!stale.error.isEmpty());
        QCOMPARE(QDir(directory).entryList({"*.pdf"},QDir::Files).size(),1);
    }
    void picturesSaveAtSourceCoordinatesAndRestoreEditableInk() {
        QTemporaryDir dir; QImage image(400,200,QImage::Format_RGB32); image.fill(Qt::white);
        const auto source=dir.filePath("original.png"); QVERIFY(image.save(source));
        AnnotationModel model; model.BeginStroke({0.2,0.5},Qt::red,0.04);
        model.AppendStrokePoint({0.8,0.5},0.001); model.EndStroke();
        const auto saved=SaveFileAnnotations(source,dir.filePath("copies"),{{0,model.strokes()}});
        QVERIFY2(saved.committed,qPrintable(saved.error));
        const QImage output(saved.path); QCOMPARE(output.size(),image.size());
        QCOMPARE(output.pixelColor(200,100),QColor(Qt::red)); QCOMPARE(output.pixelColor(200,10),QColor(Qt::white));
        QCOMPARE(QImage(source),image);
        FileDocument doc; doc.Open(source,512,dir.filePath("copies"));
        QTRY_COMPARE_WITH_TIMEOUT(doc.Snapshot().state,FileDocumentSnapshot::State::Ready,5000);
        QVERIFY(doc.Snapshot().annotations); QCOMPARE(doc.Snapshot().annotations->pages.size(),1);
        AnnotationModel restored; restored.RestoreStrokes(doc.Snapshot().annotations->pages[0]);
        const auto points=restored.strokes()[0].points;
        restored.RotateClockwise(1); restored.RotateClockwise(3); QCOMPARE(restored.strokes()[0].points,points);
    }
    void longPdfRendersOnlySelectedPages() {
        QTemporaryDir dir; const auto path=dir.filePath("long.pdf");
        { QPdfWriter writer(path); QPainter painter(&writer);
          for(int page=0;page<600;++page) { if(page)QVERIFY(writer.newPage()); painter.drawText(100,100,QStringLiteral("Page %1").arg(page+1)); } }
        FileDocument doc; doc.Open(path,512);
        QTRY_COMPARE_WITH_TIMEOUT(doc.Snapshot().state,FileDocumentSnapshot::State::Ready,5000);
        QCOMPARE(doc.Snapshot().pageCount,600); const auto generation=doc.Snapshot().documentGeneration;
        doc.TakeSnapshot(); QVERIFY(doc.Snapshot().image.isNull());
        doc.SelectPage(599,512);
        QTRY_COMPARE_WITH_TIMEOUT(doc.Snapshot().state,FileDocumentSnapshot::State::Ready,5000);
        QCOMPARE(doc.Snapshot().documentGeneration,generation); QVERIFY(doc.Snapshot().text.contains("Page 600"));
        doc.SelectPage(299,512);
        QTRY_COMPARE_WITH_TIMEOUT(doc.Snapshot().state,FileDocumentSnapshot::State::Ready,5000);
        QVERIFY(doc.Snapshot().text.contains("Page 300"));
        doc.Close();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path+".closed") || QFile::rename(path,path+".closed"),1000);
    }
    void singleFrameLayoutHasItsOwnBound() {
        QVERIFY(MediaFrameLayoutFits(2894,4096,2894*4));
        QVERIFY(MediaFrameLayoutFits(4096,4096,4096*4));
        QVERIFY(!MediaFrameLayoutFits(4096,4097,4096*4));
        QVERIFY(!MediaFrameLayoutFits(1,1,INT64_MAX));
        QVERIFY(!MediaFrameLayoutFits(1,1,-4));
        QVERIFY(!MediaFrameLayoutFits(640,480,640*4-1));
        QVERIFY(!MediaFrameLayoutFits(0,480,2560));
    }
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
