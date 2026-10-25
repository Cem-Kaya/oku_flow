#include "okuflow/common/file_document.hpp"
#ifdef _WIN32
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPdfDocument>
#include <QPdfSelection>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace okuflow {
namespace {
constexpr qint64 kFileBytes = 256ll * 1024 * 1024;
constexpr qint64 kPagePixels = 16ll * 1024 * 1024;
QSize RenderSize(QSizeF source, int edge) {
    if (!std::isfinite(source.width()) || !std::isfinite(source.height()) ||
        source.width() <= 0 || source.height() <= 0) return {};
    double scale = std::clamp(edge, 1, 8192) / std::max(source.width(), source.height());
    scale = std::min(scale, std::sqrt(double(kPagePixels) / (source.width() * source.height())));
    return {std::max(1, int(std::floor(source.width() * scale))),
            std::max(1, int(std::floor(source.height() * scale)))};
}
}
struct FileDocument::Shared {
    std::mutex mutex;
    std::condition_variable changed;
    FileDocumentSnapshot requested, result;
    std::uint64_t generation{}, documentGeneration{};
    bool pending{}, stop{};
    QString annotationDirectory;
};
bool FileDocument::SupportsPath(const QString& path) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    return suffix == "pdf" || suffix == "png" || suffix == "jpg" || suffix == "jpeg" ||
        suffix == "bmp" || suffix == "webp" || suffix == "tif" || suffix == "tiff";
}
FileDocument::FileDocument() : shared_(std::make_shared<Shared>()) {
    std::thread([s = shared_] {
        QFile file;
        QPdfDocument pdf;
        std::uint64_t opened{};
        for (;;) {
            FileDocumentSnapshot request;
            QString annotationDirectory;
            {
                std::unique_lock lock(s->mutex);
                s->changed.wait(lock, [&] { return s->stop || s->pending; });
                if (s->stop) return;
                request = s->requested; s->pending = false;
                annotationDirectory = s->annotationDirectory;
            }
            if (request.path.isEmpty()) { pdf.close(); file.close(); opened = 0; continue; }
            const auto current = [&] {
                std::lock_guard lock(s->mutex);
                return !s->stop && s->generation == request.generation;
            };
            try {
                if (opened != request.documentGeneration) {
                    pdf.close(); file.close();
                    file.setFileName(request.path);
                    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > kFileBytes)
                        throw QStringLiteral("The local file is unavailable or exceeds 256 MiB.");
                    if (request.pdf) {
                        pdf.load(&file);
                        if (pdf.status() != QPdfDocument::Status::Ready || pdf.pageCount() <= 0)
                            throw QStringLiteral("The PDF is invalid, encrypted, or unsupported.");
                    }
                    opened = request.documentGeneration;
                    if (!annotationDirectory.isEmpty())
                        request.annotations = std::make_shared<FileAnnotationCopy>(LoadFileAnnotations(request.path, annotationDirectory));
                }
                request.pageCount = request.pdf ? pdf.pageCount() : 1;
                if (request.page < 0 || request.page >= request.pageCount)
                    throw QStringLiteral("The requested page is unavailable.");
                if (!current()) continue;
                if (request.pdf) {
                    const auto size = RenderSize(pdf.pagePointSize(request.page), request.longEdge);
                    if (size.isEmpty()) throw QStringLiteral("The PDF page has invalid dimensions.");
                    request.image = pdf.render(request.page, size, {}, QImage::Format_ARGB32, Qt::white);
                    if (!current()) continue;
                    request.text = pdf.getSelectionAtIndex(request.page, 0, 512 * 1024).text();
                } else {
                    file.seek(0);
                    QImageReader reader(&file);
                    reader.setAutoTransform(true);
                    const auto source = reader.size();
                    if (source.isEmpty() || qint64(source.width()) * source.height() > 128ll * 1024 * 1024)
                        throw QStringLiteral("The picture has invalid or excessive dimensions.");
                    const auto size = RenderSize(source, std::min(request.longEdge, std::max(source.width(), source.height())));
                    reader.setScaledSize(size);
                    request.image = reader.read();
                }
                if (request.image.isNull()) throw QStringLiteral("The page or picture could not be decoded.");
                request.image = request.image.convertToFormat(QImage::Format_ARGB32);
                if (request.image.isNull()) throw QStringLiteral("The page image could not be allocated.");
                if (!current()) continue;
                auto pixels = std::make_shared<std::vector<std::uint8_t>>(size_t(request.image.width()) * request.image.height() * 4);
                for (int y = 0; y < request.image.height(); ++y) {
                    if (!current()) break;
                    std::memcpy(pixels->data() + size_t(y) * request.image.width() * 4,
                        request.image.constScanLine(y), size_t(request.image.width()) * 4);
                }
                request.pixels = std::move(pixels);
                request.state = FileDocumentSnapshot::State::Ready;
            } catch (const QString& error) {
                request.state = FileDocumentSnapshot::State::Failed; request.error = error;
                // An unsuccessful load never authorizes reuse of a previous parser.
                if (opened != request.documentGeneration) { pdf.close(); file.close(); opened = 0; }
            } catch (...) {
                request.state = FileDocumentSnapshot::State::Failed;
                request.error = QStringLiteral("The document decoder failed.");
            }
            {
                std::lock_guard lock(s->mutex);
                if (!s->stop && s->generation == request.generation) s->result = std::move(request);
            }
        }
    }).detach();
}
FileDocument::~FileDocument() {
    std::lock_guard lock(shared_->mutex);
    shared_->stop = true; ++shared_->generation; shared_->changed.notify_one();
}
std::uint64_t FileDocument::Open(QString path, int longEdge, QString annotationDirectory) {
    std::lock_guard lock(shared_->mutex);
    auto& request = shared_->requested;
    request = {}; request.generation = ++shared_->generation;
    request.documentGeneration = ++shared_->documentGeneration;
    request.path = std::move(path); request.pdf = QFileInfo(request.path).suffix().compare("pdf", Qt::CaseInsensitive) == 0;
    request.longEdge = std::clamp(longEdge, 512, 8192); request.state = FileDocumentSnapshot::State::Opening;
    shared_->result = request; shared_->pending = true; shared_->changed.notify_one();
    shared_->annotationDirectory = std::move(annotationDirectory);
    return request.generation;
}
std::uint64_t FileDocument::SelectPage(int page, int longEdge) {
    std::lock_guard lock(shared_->mutex);
    if (shared_->requested.path.isEmpty() || page < 0 ||
        (shared_->result.pageCount && page >= shared_->result.pageCount)) return 0;
    auto& request = shared_->requested;
    request.image = {}; request.pixels.reset(); request.text.clear(); request.error.clear();
    request.page = page; request.longEdge = std::clamp(longEdge, 512, 8192);
    request.generation = ++shared_->generation; request.state = FileDocumentSnapshot::State::Opening;
    shared_->result = request; shared_->pending = true; shared_->changed.notify_one();
    return request.generation;
}
void FileDocument::Close() {
    std::lock_guard lock(shared_->mutex);
    ++shared_->generation; shared_->requested = {}; shared_->result = {}; shared_->pending = true;
    shared_->changed.notify_one();
}
FileDocumentSnapshot FileDocument::Snapshot() const {
    std::lock_guard lock(shared_->mutex); return shared_->result;
}
FileDocumentSnapshot FileDocument::TakeSnapshot() {
    std::lock_guard lock(shared_->mutex);
    auto result = shared_->result;
    shared_->result.image = {};
    shared_->result.pixels.reset();
    shared_->result.annotations.reset();
    return result;
}
}
#endif
