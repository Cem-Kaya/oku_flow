#pragma once

#include "okuflow/common/uct_model_settings.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QVector>
#include <atomic>
#include <functional>

namespace okuflow::uct::installation {
struct File {
    QString name;
    QUrl url;
    qint64 bytes{};
    QByteArray sha256;
};
struct Model {
    QString id, name, directory, revision;
    bool detection{};
    ModelTier tier{};
    QVector<File> files;
    qint64 Bytes() const;
};
// Immutable application catalog, never downloaded executable instructions.
QVector<Model> Catalog();
QVector<Model> Select(const ModelSettings& settings);
// Compiled native partition package, including source notices. No caller URLs.
Model FormulaPackage(formula::Model model);
QString DefaultCacheRoot();
QString CachedDirectory(const QString& cacheRoot, const Model& model);
QString SourceDirectory(const QString& bundledRoot, const QString& cacheRoot, const Model& model);
bool VerifyDirectory(const QString& directory, const Model& model,
                     const std::atomic<bool>* cancelled = nullptr);
struct Progress {
    enum class Stage { Downloading, Preparing } stage{};
    qint64 completed{}, total{};
};
struct Preparation { bool ok{}; QString detail; };
struct Result {
    bool ok{}, cancelled{};
    QString code, detail;
    QJsonObject receipt;
};
using Notify = std::function<void(Progress)>;
using Prepare = std::function<Preparation(const std::atomic<bool>&)>;
// Blocking worker-only transaction. Publishes complete verified sources into
// revision directories, then calls prepare. It NEVER changes active settings or
// deletes/replaces previous models. Only a successful result permits activation.
Result Run(const QVector<Model>& models, const QString& bundledRoot, const QString& cacheRoot,
           const std::atomic<bool>& cancelled, const Notify& notify, const Prepare& prepare);
} // namespace okuflow::uct::installation
