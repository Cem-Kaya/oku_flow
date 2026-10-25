#include "okuflow/common/file_annotations.hpp"
#ifdef _WIN32
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <windows.h>

namespace okuflow {
namespace {
constexpr qint64 kMaxSourceBytes = 256ll * 1024 * 1024;
constexpr qint64 kMaxInkBytes = 32ll * 1024 * 1024;
QString HashFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    return h.addData(&f) ? QString::fromLatin1(h.result().toHex()) : QString();
}
FileAnnotationCopy Identify(const QString& source, const QString& directory) {
    FileAnnotationCopy r;
    const QFileInfo input(source);
    if (!input.isFile() || input.size() <= 0 || input.size() > kMaxSourceBytes) {
        r.error = QStringLiteral("The original document is unavailable or exceeds 256 MiB."); return r;
    }
    r.sourceSha256 = HashFile(source);
    if (r.sourceSha256.isEmpty()) { r.error = QStringLiteral("The original document could not be read."); return r; }
    const auto key = QCryptographicHash::hash((input.canonicalFilePath().toCaseFolded() +
        '\n' + r.sourceSha256).toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    const auto extension = input.suffix().compare("pdf", Qt::CaseInsensitive) == 0 ? ".pdf" : ".png";
    r.path = QDir(directory).filePath(input.completeBaseName().left(100) + "-OkuFlow-" + key + extension);
    if (QFileInfo(r.path).absoluteFilePath().compare(input.absoluteFilePath(), Qt::CaseInsensitive) == 0)
        r.error = QStringLiteral("The original document cannot be replaced.");
    return r;
}
QJsonArray EncodeInk(const FilePageInk& pages) {
    QJsonArray result;
    for (auto page = pages.cbegin(); page != pages.cend(); ++page) {
        if (page.value().empty()) continue;
        QJsonArray strokes;
        for (const auto& s : page.value()) {
            QJsonArray points;
            for (const auto& p : s.points) points.append(QJsonArray{p.x(), p.y()});
            strokes.append(QJsonObject{{"kind", int(s.kind)}, {"points", points},
                {"color", s.color.name(QColor::HexArgb)}, {"width", s.sceneWidth},
                {"text", s.text}, {"dashed", s.dashed}});
        }
        result.append(QJsonObject{{"page", page.key()}, {"strokes", strokes}});
    }
    return result;
}
bool DecodeInk(const QJsonArray& input, FilePageInk& pages) {
    int pointCount = 0;
    for (const auto& value : input) {
        const auto page = value.toObject(); const int index = page["page"].toInt(-1);
        if (index < 0 || pages.contains(index)) return false;
        QVector<AnnotationStroke> strokes;
        for (const auto& item : page["strokes"].toArray()) {
            const auto object = item.toObject(); AnnotationStroke stroke;
            const int kind = object["kind"].toInt(-1);
            if (kind < 0 || kind > int(AnnotationItemKind::kEllipse)) return false;
            stroke.id = strokes.size() + 1; stroke.kind = AnnotationItemKind(kind);
            stroke.color = QColor(object["color"].toString());
            stroke.sceneWidth = object["width"].toDouble();
            stroke.text = object["text"].toString(); stroke.dashed = object["dashed"].toBool();
            if (!stroke.color.isValid() || !std::isfinite(stroke.sceneWidth) ||
                stroke.sceneWidth <= 0 || stroke.sceneWidth > 1 || stroke.text.size() > 65536) return false;
            for (const auto& point : object["points"].toArray()) {
                const auto pair = point.toArray();
                if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble() || ++pointCount > 2000000) return false;
                const double x = pair[0].toDouble(), y = pair[1].toDouble();
                if (!std::isfinite(x) || !std::isfinite(y) || x < -1 || x > 2 || y < -1 || y > 2) return false;
                stroke.points.append(QPointF(x,y));
            }
            if (stroke.points.empty()) return false;
            strokes.append(std::move(stroke));
        }
        pages.insert(index, std::move(strokes));
    }
    return true;
}
bool ReadState(FileAnnotationCopy& r) {
    const QString sidecar = r.path + ".ink.json";
    if (!QFileInfo::exists(r.path) && !QFileInfo::exists(sidecar)) return true;
    QFile file(sidecar);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxInkBytes) return false;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    return object["schema"].toInt() == 1 && object["sourceSha256"].toString() == r.sourceSha256 &&
        object["outputSha256"].toString() == HashFile(r.path) &&
        !object["outputSha256"].toString().isEmpty() && DecodeInk(object["pages"].toArray(), r.pages);
}
bool RunPdfTool(const QStringList& args) {
    const QString tool = FileAnnotationPdfTool();
    if (tool.isEmpty()) return false;
    QProcess process;
    process.setProgram(tool); process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
        a->flags |= CREATE_NO_WINDOW;
    });
    process.start();
    if (!process.waitForStarted(5000)) return false;
    if (!process.waitForFinished(60000)) { process.kill(); process.waitForFinished(5000); return false; }
    // qpdf's warning exit is not silently treated as a clean export.
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
bool PublishFile(const QString& source, const QString& target) {
    QFile input(source); QSaveFile output(target); output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return false;
    while (!input.atEnd()) {
        const auto bytes = input.read(1024 * 1024);
        if (bytes.isEmpty() || output.write(bytes) != bytes.size()) return false;
    }
    return output.commit();
}
}
QString FileAnnotationPdfTool() {
    const QString relative = QStringLiteral("qpdf/bin/qpdf.exe");
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(relative);
    const auto verified = [](const QString& path) {
        return HashFile(path) == QStringLiteral("1bdaf878f37eaffea1ea3c30591214c77636a0f2fc7432427516b6e22e36c0ff") &&
            HashFile(QFileInfo(path).dir().filePath("qpdf30.dll")) == QStringLiteral("8812952678de93e0750308a113aa650e9161af466f9770679ea9673adeb61041");
    };
    if (QFileInfo::exists(bundled)) return verified(bundled) ? bundled : QString();
    const QString root = qEnvironmentVariable("OKUFLOW_QPDF_ROOT",
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("OkuFlow/qpdf/12.4.2"));
    const QString cached = QDir(root).filePath("bin/qpdf.exe");
    return verified(cached) ? cached : QString();
}
FileAnnotationCopy LoadFileAnnotations(const QString& source, const QString& directory) {
    auto r = Identify(source,directory);
    if (r.error.isEmpty() && !ReadState(r))
        r.error = QStringLiteral("The saved working copy was changed outside OkuFlow; it has been preserved.");
    return r;
}
FileAnnotationCopy SaveFileAnnotations(const QString& source, const QString& directory, const FilePageInk& pages,
                                      const QString& expectedSourceSha256) {
    auto r = Identify(source,directory);
    if (!r.error.isEmpty()) return r;
    if (!expectedSourceSha256.isEmpty() && r.sourceSha256 != expectedSourceSha256) {
        r.error = QStringLiteral("The original document changed after it was opened."); return r;
    }
    if (!QDir().mkpath(directory)) { r.error = QStringLiteral("The OkuFlow Documents folder is unavailable."); return r; }
    QLockFile lock(r.path + ".lock");
    if (!lock.tryLock(0)) { r.error = QStringLiteral("This working copy is already being saved."); return r; }
    if (!ReadState(r)) { r.error = QStringLiteral("The saved working copy was changed outside OkuFlow; it has been preserved."); return r; }
    const auto previousOutputHash = HashFile(r.path);
    r.pages = pages;
    // Apply the same validation to authored and restored ink, before creating a PDF.
    FilePageInk validated;
    const auto encoded = EncodeInk(pages);
    if (QJsonDocument(encoded).toJson().size() > kMaxInkBytes || !DecodeInk(encoded,validated)) {
        r.error = QStringLiteral("The document drawing is too large or invalid."); return r;
    }
    QTemporaryDir staging(QDir(directory).filePath(".okuflow-save-XXXXXX"));
    if (!staging.isValid()) { r.error = QStringLiteral("The document save could not be staged."); return r; }
    const QString original = staging.filePath("original");
    if (!QFile::copy(source,original) || HashFile(original) != r.sourceSha256) {
        r.error = QStringLiteral("The original document changed while saving."); return r;
    }
    QString ready = original;
    if (QFileInfo(source).suffix().compare("pdf", Qt::CaseInsensitive) == 0) {
        QPdfDocument pdf;
        if (pdf.load(original) != QPdfDocument::Error::None || pdf.pageCount() <= 0) {
            r.error = QStringLiteral("The original PDF is unavailable."); return r;
        }
        QStringList destinations;
        const QString overlay = staging.filePath("ink.pdf");
        {
            QPdfWriter writer(overlay); writer.setResolution(144);
            QPainter painter;
            for (auto page = validated.cbegin(); page != validated.cend(); ++page) {
                if (page.key() >= pdf.pageCount()) { r.error = QStringLiteral("A drawing refers to a missing PDF page."); return r; }
                const auto points = pdf.pagePointSize(page.key());
                writer.setPageSize(QPageSize(points,QPageSize::Point,QString(),QPageSize::ExactMatch));
                writer.setPageMargins(QMarginsF(0,0,0,0));
                if (!painter.isActive()) {
                    if (!painter.begin(&writer)) { r.error = QStringLiteral("The PDF drawing could not be encoded."); return r; }
                } else if (!writer.newPage()) { r.error = QStringLiteral("The PDF drawing page could not be encoded."); return r; }
                RenderAnnotationStrokes(painter,page.value(),ViewTransform{0,0,1,1,0,0,1,1,true},QSizeF(writer.width(),writer.height()));
                destinations.append(QString::number(page.key()+1));
            }
            if (painter.isActive() && !painter.end()) { r.error = QStringLiteral("The PDF drawing could not be finalized."); return r; }
        }
        if (!destinations.empty()) {
            ready = staging.filePath("annotated.pdf");
            if (!RunPdfTool({original,"--overlay",overlay,"--to="+destinations.join(','),"--",ready})) {
                r.error = QStringLiteral("The pinned PDF export runtime is unavailable or the PDF overlay failed."); return r;
            }
            QPdfDocument check;
            if (check.load(ready) != QPdfDocument::Error::None || check.pageCount() != pdf.pageCount()) {
                r.error = QStringLiteral("The saved PDF failed page validation."); return r;
            }
        }
    } else {
        QImageReader reader(original); reader.setAutoTransform(true);
        const auto size = reader.size();
        if (size.isEmpty() || qint64(size.width())*size.height() > 16ll*1024*1024 ||
            (validated.size() && (validated.size()!=1 || validated.firstKey()!=0))) {
            r.error = QStringLiteral("The picture is invalid or exceeds the 16-Mpixel save limit."); return r;
        }
        auto image = reader.read().convertToFormat(QImage::Format_ARGB32);
        if (image.isNull()) { r.error = QStringLiteral("The original picture could not be read."); return r; }
        { QPainter painter(&image); RenderAnnotationStrokes(painter,validated.value(0),ViewTransform{0,0,1,1,0,0,1,1,true},image.size()); }
        ready = staging.filePath("annotated.png");
        if (!image.save(ready,"PNG")) { r.error = QStringLiteral("The picture copy could not be encoded."); return r; }
    }
    const auto outputHash = HashFile(ready);
    const QJsonObject metadata{{"schema",1},{"sourceSha256",r.sourceSha256},
        {"outputSha256",outputHash},{"pages",encoded}};
    const auto bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    QSaveFile state(r.path + ".ink.json"); state.setDirectWriteFallback(false);
    if (HashFile(r.path) != previousOutputHash || outputHash.isEmpty() || !state.open(QIODevice::WriteOnly) || state.write(bytes) != bytes.size() ||
        !PublishFile(ready,r.path) || !state.commit()) {
        r.error = QStringLiteral("The document copy could not be committed."); return r;
    }
    r.committed = true; return r;
}
}
#endif
