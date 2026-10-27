#include "okuflow/common/uct_model_installation.hpp"
#include "uct_download_catalog.hpp"
#include "uct_formula_catalog.hpp"
#include "uct_model_installation_internal.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace okuflow::uct::installation {
namespace {
struct Failure : std::runtime_error {
    QString code;
    Failure(const char* c, const char* detail) : std::runtime_error(detail), code(c) {}
};
bool Token(const QString& value) {
    return !value.isEmpty() && value.size() <= 100 && std::all_of(value.begin(), value.end(), [](QChar c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}
void CheckCancelled(const std::atomic<bool>& cancelled) {
    if (cancelled.load()) throw Failure("cancelled", "Model setup cancelled.");
}
bool VerifyFile(const QString& path, const File& entry, const std::atomic<bool>* cancelled) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() != entry.bytes) return false;
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash digest(QCryptographicHash::Sha256);
    qint64 count = 0;
    while (!input.atEnd()) {
        if (cancelled && cancelled->load()) return false;
        const auto block = input.read(256 * 1024);
        if (block.isEmpty()) return false;
        count += block.size();
        if (count > entry.bytes) return false;
        digest.addData(block);
    }
    return input.error() == QFileDevice::NoError && count == entry.bytes && digest.result().toHex() == entry.sha256;
}
void CheckRoot(const QString& root);
QByteArray SourceReceipt(const Model& model) {
    QJsonArray files;
    for(const auto& file:model.files)files.push_back(QJsonObject{{"name",file.name},{"bytes",file.bytes},
        {"sha256",QString::fromLatin1(file.sha256)}});
    return QJsonDocument(QJsonObject{{"schema",1},{"id",model.id},{"revision",model.revision},{"files",files}})
        .toJson(QJsonDocument::Compact);
}
void QuarantineDamaged(const QString& cacheRoot,const QString& directory,const Model& model) {
    // Repair only a directory previously published by this transaction. Never
    // delete it, follow a redirected path, or adopt an unrecognized directory.
    CheckRoot(directory);
    QFile marker(QDir(directory).filePath("source.json"));
    const auto expected=SourceReceipt(model);
    if(!marker.open(QIODevice::ReadOnly) || marker.size()!=expected.size() || marker.readAll()!=expected)
        throw Failure("cache_damaged","An unrecognized model cache was left unchanged.");
    marker.close();
    const auto backup=QDir(cacheRoot).filePath("damaged-"+model.id+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    if(!QDir().rename(directory,backup))throw Failure("cache_damaged","Cannot preserve the damaged model cache for repair.");
}
void CheckRoot(const QString& root) {
    if (!QDir::isAbsolutePath(root) || root.startsWith("//") || root.startsWith("\\\\"))
        throw Failure("cache_unavailable", "Cannot create the local model cache.");
    // Do not follow an existing redirected installation directory for writes.
    for (QString path = QDir::cleanPath(root); !path.isEmpty();) {
        const QFileInfo info(path);
        if (!info.exists()) {
            const auto parent=info.absolutePath();
            if(parent==path)throw Failure("cache_unavailable","The model cache drive is unavailable.");
            path=parent;continue;
        }
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw Failure("cache_unavailable", "The model cache must be an ordinary local directory.");
#else
        if (info.isSymLink()) throw Failure("cache_unavailable", "The model cache must be an ordinary local directory.");
#endif
        const auto parent = info.absolutePath();
        if (parent == path) break;
        path = parent;
    }
    if (!QDir().mkpath(root)) throw Failure("cache_unavailable", "Cannot create the local model cache.");
}
void Download(QNetworkAccessManager& network, const File& file, const QString& target,
              const std::atomic<bool>& cancelled, qint64 completed, qint64 total, const Notify& notify) {
    // URLs come only from Catalog(). The transaction rechecks catalog equality.
    QNetworkRequest request(file.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    auto* reply = network.get(request);
    QSaveFile output(target); output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) { reply->abort(); delete reply; throw Failure("disk_write", "Cannot write the downloaded model."); }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 received = 0;
    QString failure;
    QEventLoop loop;
    QElapsedTimer deadline; deadline.start();
    const auto read = [&] {
        while (reply->bytesAvailable() > 0 && failure.isEmpty()) {
            const auto bytes = reply->read(256 * 1024);
            if (bytes.isEmpty()) break;
            if (received + bytes.size() > file.bytes) { failure = "download_size"; reply->abort(); break; }
            if (output.write(bytes) != bytes.size()) { failure = "disk_write"; reply->abort(); break; }
            hash.addData(bytes); received += bytes.size();
            if (notify) notify({Progress::Stage::Downloading, completed + received, total});
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, read);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (cancelled.load() || deadline.elapsed() > 900000) {
            failure = cancelled.load() ? "cancelled" : "download_timeout";
            reply->abort();
        }
    });
    timer.start(50);
    if (!reply->isFinished()) loop.exec();
    read();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error();
    delete reply;
    CheckCancelled(cancelled);
    if (!failure.isEmpty()) throw Failure(failure.toLatin1().constData(), "Model download was not completed.");
    if (networkError != QNetworkReply::NoError || status != 200)
        throw Failure("download_failed", "The model server did not complete the download.");
    if (received != file.bytes || hash.result().toHex() != file.sha256)
        throw Failure("download_verification", "The downloaded model does not match its published checksum.");
    if (!output.commit()) throw Failure("disk_write", "Cannot commit the verified model file.");
}
}
qint64 Model::Bytes() const { qint64 bytes = 0; for (const auto& file : files) bytes += file.bytes; return bytes; }
QVector<Model> Catalog() {
    static const auto catalog = [] {
        const auto document = QJsonDocument::fromJson(kDownloadCatalog);
        if (document.object()["schema"].toInt() != 1) throw std::logic_error("Invalid compiled model catalog.");
        QVector<Model> result;
        for (const auto& row : document.object()["models"].toArray()) {
            const auto object = row.toObject();
            const auto tier = ParseModelTier(object["tier"].toString());
            Model model{object["id"].toString(), object["name"].toString(), object["directory"].toString(),
                        object["revision"].toString(), object["kind"] == "det", tier.value_or(ModelTier::Medium), {}};
            if (!tier || !Token(model.id) || !Token(model.revision) || model.revision.size() != 40 ||
                (object["kind"] != "det" && object["kind"] != "rec") || object["format"] != "onnx")
                throw std::logic_error("Invalid compiled model identity.");
            for (const auto& value : object["files"].toArray()) {
                const auto f = value.toObject();
                File file{f["name"].toString(), QUrl(f["url"].toString()), qint64(f["bytes"].toDouble()), f["sha256"].toString().toLatin1()};
                if ((file.name != "inference.onnx" && file.name != "inference.yml" && file.name != "README.md") ||
                    file.bytes < 1 || file.bytes > 256ll*1024*1024 || file.sha256.size() != 64 ||
                    QByteArray::fromHex(file.sha256).toHex() != file.sha256 || file.url.scheme() != "https" ||
                    file.url.host() != "huggingface.co" || !file.url.path().contains("/resolve/"+model.revision+"/"))
                    throw std::logic_error("Invalid compiled model download.");
                model.files.push_back(file);
            }
            if (model.files.size() != 3) throw std::logic_error("Incomplete compiled model package.");
            result.push_back(model);
        }
        if (result.size() != 6) throw std::logic_error("Incomplete compiled OCR model catalog.");
        return result;
    }();
    return catalog;
}
QVector<Model> Select(const ModelSettings& settings) {
    QVector<Model> result;
    for (const auto& model : Catalog())
        if (model.detection ? (model.tier == settings.gpuDetector || model.tier == settings.cpuDetector)
                            : (model.tier == settings.gpuRecognizer || model.tier == settings.cpuRecognizer)) result.push_back(model);
    if(settings.installMath)result.push_back(FormulaPackage(settings.mathModel));
    return result;
}
Model FormulaPackage(formula::Model selected) {
    const auto document=QJsonDocument::fromJson(formula::kCatalog).object();
    const auto rows=document["models"].toArray();
    const int index=static_cast<int>(selected);
    if(index<0 || index>=rows.size())throw std::logic_error("Unknown Math model.");
    const auto row=rows[index].toObject();
    Model result;result.id="math-"+formula::ModelToken(selected);result.name=row["name"].toString();
    result.directory=result.name;result.revision=row["revision"].toString();
    if(!Token(result.id) || result.revision.size()!=40 || !row["downloadAvailable"].toBool())
        throw std::logic_error("Math download is not available.");
    auto files=row["files"].toArray();
    for(const auto& notice:document["notices"].toArray())files.append(notice);
    QSet<QString> names;
    for(const auto& value:files) {
        const auto pin=value.toObject();
        File f{pin["name"].toString(),QUrl(pin["url"].toString()),pin["bytes"].toInteger(),pin["sha256"].toString().toLatin1()};
        const bool graph=f.name=="encoder.onnx" || f.name=="decoder.onnx";
        const bool notice=f.name=="LICENSE.txt" || f.name=="NOTICE.txt";
        const bool source=f.name=="README.md" || f.name=="config.json";
        const bool github=f.url.host()=="github.com" && f.url.path().startsWith("/Cem-Kaya/oku_flow/releases/download/uct-formula-tf32-v1/");
        const bool upstream=f.url.host()=="huggingface.co" && f.url.path().contains("/resolve/"+result.revision+"/");
        if(names.contains(f.name) || !(graph || notice || source) || f.bytes<1 || f.bytes>512ll*1024*1024 ||
            f.sha256.size()!=64 || QByteArray::fromHex(f.sha256).toHex()!=f.sha256 ||
            f.url.scheme()!="https" || ((graph||notice)?!github:!upstream))
            throw std::logic_error("Invalid compiled Math package.");
        names.insert(f.name);result.files.push_back(std::move(f));
    }
    if(names.size()!=6)throw std::logic_error("Incomplete Math package.");
    return result;
}
QString DefaultCacheRoot() { return QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/model-cache/sources"); }
QString CachedDirectory(const QString& cacheRoot, const Model& model) {
    return QDir(cacheRoot).filePath(model.id+"-"+model.revision);
}
bool VerifyDirectory(const QString& directory, const Model& model, const std::atomic<bool>* cancelled) {
    const QFileInfo info(directory);
    if (!info.isDir() || info.isSymLink()) return false;
    for (const auto& file : model.files) if (!VerifyFile(QDir(directory).filePath(file.name), file, cancelled)) return false;
    return true;
}
QString SourceDirectory(const QString& bundledRoot, const QString& cacheRoot, const Model& model) {
    const auto bundled = QDir(bundledRoot).filePath(model.directory);
    if (VerifyDirectory(bundled, model)) return bundled;
    const auto cached = CachedDirectory(cacheRoot, model);
    return VerifyDirectory(cached, model) ? cached : QString{};
}
Result Run(const QVector<Model>& models, const QString& bundledRoot, const QString& cacheRoot,
           const std::atomic<bool>& cancelled, const Notify& notify, const Prepare& prepare) {
    try {
        if (models.isEmpty() || models.size() > 7 || !prepare) throw Failure("invalid_request", "No supported model was selected.");
        auto catalog = Catalog();
        for(const auto model:{formula::Model::PlusS,formula::Model::PlusM,formula::Model::L,formula::Model::PlusL})
            catalog.push_back(FormulaPackage(model));
        QSet<QString> seen;
        for (const auto& model : models) {
            const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& pin) { return pin.id == model.id; });
            if (found == catalog.end() || seen.contains(model.id) || found->revision != model.revision ||
                found->files.size() != model.files.size() || found->directory != model.directory ||
                found->detection != model.detection || found->tier != model.tier || found->name != model.name)
                throw Failure("invalid_request", "Model request differs from the compiled catalog.");
            for (int i = 0; i < model.files.size(); ++i)
                if (found->files[i].url != model.files[i].url || found->files[i].name != model.files[i].name ||
                    found->files[i].sha256 != model.files[i].sha256 || found->files[i].bytes != model.files[i].bytes)
                    throw Failure("invalid_request", "Model download differs from the compiled catalog.");
            seen.insert(model.id);
        }
        QNetworkAccessManager network;
        return detail::RunPinned(models,bundledRoot,cacheRoot,cancelled,notify,prepare,
            [&](const File& file,const QString& target,const std::atomic<bool>& stop,qint64 completed,qint64 total,const Notify& progress) {
                Download(network,file,target,stop,completed,total,progress);
            });
    } catch(const Failure& error) {
        return {false,error.code=="cancelled",error.code,QString::fromUtf8(error.what()),{}};
    } catch(const std::exception& error) {
        return {false,false,"setup_failed",QString::fromUtf8(error.what()).left(4096),{}};
    }
}
Result detail::RunPinned(const QVector<Model>& models,const QString& bundledRoot,const QString& cacheRoot,
                        const std::atomic<bool>& cancelled,const Notify& notify,const Prepare& prepare,const Fetch& fetch) {
    Result result;
    try {
        CheckCancelled(cancelled); CheckRoot(cacheRoot);
        QLockFile lock(QDir(cacheRoot).filePath("installation.lock"));
        if (!lock.tryLock(0)) throw Failure("setup_busy", "Another model setup is running.");
        qint64 total = 0, completed = 0;
        for (const auto& model : models) total += model.Bytes();
        QJsonArray identities;
        for (const auto& model : models) {
            CheckCancelled(cancelled);
            auto directory = SourceDirectory(bundledRoot, cacheRoot, model);
            if (directory.isEmpty()) {
                const auto destination = CachedDirectory(cacheRoot, model);
                if (QFileInfo::exists(destination))
                    QuarantineDamaged(cacheRoot,destination,model);
                const QStorageInfo storage(cacheRoot);
                if (storage.isValid() && storage.bytesAvailable() < model.Bytes()*2 + 64ll*1024*1024)
                    throw Failure("disk_full", "Not enough disk space for model setup.");
                QTemporaryDir stage(QDir(cacheRoot).filePath("download-XXXXXX"));
                if (!stage.isValid()) throw Failure("disk_write", "Cannot create a temporary model directory.");
                for (const auto& file : model.files) {
                    CheckCancelled(cancelled);
                    fetch(file, QDir(stage.path()).filePath(file.name), cancelled, completed, total, notify);
                    completed += file.bytes;
                }
                if (!VerifyDirectory(stage.path(), model, &cancelled)) {
                    CheckCancelled(cancelled);
                    throw Failure("download_verification", "Downloaded model verification failed.");
                }
                const auto markerBytes=SourceReceipt(model);
                QSaveFile marker(QDir(stage.path()).filePath("source.json"));marker.setDirectWriteFallback(false);
                if(!marker.open(QIODevice::WriteOnly) || marker.write(markerBytes)!=markerBytes.size() || !marker.commit())
                    throw Failure("disk_write","Cannot commit the source model receipt.");
                CheckCancelled(cancelled);
                if (!QDir().rename(stage.path(), destination)) throw Failure("disk_write", "Cannot publish the complete model directory.");
                stage.setAutoRemove(false); directory = destination;
            } else completed += model.Bytes();
            identities.push_back(QJsonObject{{"id",model.id},{"revision",model.revision}});
            if (notify) notify({Progress::Stage::Downloading, completed, total});
        }
        CheckCancelled(cancelled);
        if (notify) notify({Progress::Stage::Preparing, completed, total});
        const auto prepared = prepare(cancelled);
        CheckCancelled(cancelled);
        if (!prepared.ok) { result.code = "preparation_failed"; result.detail = prepared.detail; return result; }
        result.receipt = {{"schema",1},{"models",identities},{"backend",prepared.detail}};
        // A receipt is diagnostic data, not a mutable activation pointer.
        const auto bytes = QJsonDocument(result.receipt).toJson(QJsonDocument::Compact);
        const auto identity = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        QSaveFile receipt(QDir(cacheRoot).filePath("ready-"+QString::fromLatin1(identity)+".json"));
        receipt.setDirectWriteFallback(false);
        if (!receipt.open(QIODevice::WriteOnly) || receipt.write(bytes) != bytes.size() || !receipt.commit())
            throw Failure("disk_write", "Cannot commit the model setup receipt.");
        CheckCancelled(cancelled);
        result.ok = true; result.code = "ready"; result.detail = prepared.detail;
    } catch (const Failure& error) {
        result.cancelled = error.code == "cancelled"; result.code = error.code; result.detail = QString::fromUtf8(error.what());
    } catch (const std::exception& error) {
        result.code = "setup_failed"; result.detail = QString::fromUtf8(error.what()).left(4096);
    }
    return result;
}
} // namespace okuflow::uct::installation
