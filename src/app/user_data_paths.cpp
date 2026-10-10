#include "okuflow/app/user_data_paths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>

#include <filesystem>
#include <memory>
#include <system_error>

namespace okuflow {

namespace {

QString NormalizeAbsolutePath(const QString& path)
{
    if (path.trimmed().isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QFileInfo(path.trimmed()).absoluteFilePath());
}

// Resolves Windows junctions/symlinks before a containment comparison. A
// purely lexical check would accept a root that is a link pointing back
// inside the install directory. The candidate may not exist yet, so the
// deepest existing ancestor is canonicalized and the remainder reattached.
QString CanonicalizeForContainment(const QString& path)
{
    const QString absolute = NormalizeAbsolutePath(path);
    if (absolute.isEmpty()) {
        return absolute;
    }
    QString existing = absolute;
    QStringList tail;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const QFileInfo info(existing);
        const QString parent = info.absolutePath();
        if (parent == existing) {
            break;
        }
        tail.prepend(info.fileName());
        existing = parent;
    }
    // QFileInfo::canonicalFilePath does NOT resolve NTFS junctions on
    // Windows (verified by the junction regression test), so resolution
    // goes through std::filesystem::canonical, which uses
    // GetFinalPathNameByHandle and resolves junctions and symlinks alike.
    QString canonical;
    if (QFileInfo::exists(existing)) {
        std::error_code errorCode;
        const std::filesystem::path resolved = std::filesystem::canonical(
            std::filesystem::path(existing.toStdWString()), errorCode);
        if (!errorCode) {
            QString text = QString::fromStdWString(resolved.native());
            if (text.startsWith(QStringLiteral("\\\\?\\"))) {
                text = text.mid(4);
            }
            canonical = NormalizeAbsolutePath(text);
        }
    }
    if (canonical.isEmpty()) {
        return absolute;
    }
    QString resolved = canonical;
    for (const QString& part : tail) {
        resolved = QDir(resolved).filePath(part);
    }
    return QDir::cleanPath(resolved);
}

bool IsPathInside(const QString& candidate, const QString& parent)
{
    if (candidate.isEmpty() || parent.isEmpty()) {
        return false;
    }
    const QString child =
        QDir::toNativeSeparators(CanonicalizeForContainment(candidate));
    QString root = QDir::toNativeSeparators(CanonicalizeForContainment(parent));
    if (!root.endsWith(QDir::separator())) {
        root += QDir::separator();
    }
    return child.compare(root.left(root.size() - 1), Qt::CaseInsensitive) == 0 ||
           child.startsWith(root, Qt::CaseInsensitive);
}

struct PhotoPairFiles {
    QString originalFinal;
    QString processedFinal;
    QString originalTemp;
    QString processedTemp;
    QString transactionLock;
    QString ownershipMarker;
    QDateTime newestModification;
};

void TrackNewestModification(PhotoPairFiles& pair, const QFileInfo& file)
{
    if (!pair.newestModification.isValid() ||
        file.lastModified() > pair.newestModification) {
        pair.newestModification = file.lastModified();
    }
}

QString PhotoPairKey(const QString& fileName,
                     bool* original,
                     bool* temporary)
{
    QString finalName = fileName;
    *temporary = finalName.endsWith(QStringLiteral(".writing"));
    if (*temporary) {
        finalName.chop(QStringLiteral(".writing").size());
    }
    const QString originalSuffix = QStringLiteral("_original.jpg");
    const QString processedSuffix = QStringLiteral("_processed.jpg");
    if (!finalName.startsWith(QStringLiteral("IMG_"))) {
        return {};
    }
    if (finalName.endsWith(originalSuffix)) {
        *original = true;
        return finalName.left(finalName.size() - originalSuffix.size());
    }
    if (finalName.endsWith(processedSuffix)) {
        *original = false;
        return finalName.left(finalName.size() - processedSuffix.size());
    }
    return {};
}

} // namespace

UserDataPaths::UserDataPaths(QString configuredRoot, QString installDirectory)
    : configuredRoot_(configuredRoot.trimmed()),
      installDirectory_(NormalizeAbsolutePath(installDirectory))
{
    // Defense in depth for persisted roots that bypass ValidateRoot: a saved
    // directory later replaced with a junction into the install directory
    // must never be honored. Falls back to the default Documents root; the
    // app-level startup validation is responsible for telling the user.
    if (!configuredRoot_.isEmpty() &&
        IsPathInside(configuredRoot_, installDirectory_)) {
        configuredRoot_.clear();
    }
}

QString UserDataPaths::DefaultRoot()
{
    QString documents =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty()) {
        documents = QDir(
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
                        .filePath(QStringLiteral("Documents"));
    }
    return QDir::cleanPath(QDir(documents).filePath(QStringLiteral("OkuFlow")));
}

UserDataValidationResult UserDataPaths::ValidateRoot(
    const QString& requestedRoot,
    const QString& installDirectory)
{
    UserDataValidationResult result;
    result.normalizedRoot = requestedRoot.trimmed().isEmpty()
                                ? DefaultRoot()
                                : NormalizeAbsolutePath(requestedRoot);
    if (result.normalizedRoot.isEmpty()) {
        result.error = QStringLiteral("OkuFlow could not determine a folder.");
        return result;
    }
    if (IsPathInside(result.normalizedRoot, installDirectory)) {
        result.error = QStringLiteral(
            "Choose a folder outside the OkuFlow application folder. "
            "Application updates replace files in that folder.");
        return result;
    }
    const QFileInfo existing(result.normalizedRoot);
    if (existing.exists() && !existing.isDir()) {
        result.error =
            QStringLiteral("The selected location is a file, not a folder.");
        return result;
    }
    if (!QDir().mkpath(result.normalizedRoot)) {
        result.error =
            QStringLiteral("OkuFlow could not create the selected folder.");
        return result;
    }

    const QString probePath = QDir(result.normalizedRoot)
                                  .filePath(QStringLiteral(".okuflow-write-test-%1.tmp")
                                                .arg(QUuid::createUuid().toString(
                                                    QUuid::WithoutBraces)));
    QFile probe(probePath);
    if (!probe.open(QIODevice::WriteOnly) ||
        probe.write("OkuFlow", 7) != 7) {
        result.error =
            QStringLiteral("OkuFlow cannot write to the selected folder.");
        probe.close();
        QFile::remove(probePath);
        return result;
    }
    probe.close();
    if (!QFile::remove(probePath)) {
        result.error = QStringLiteral(
            "OkuFlow wrote to the selected folder but could not remove its "
            "temporary test file.");
        return result;
    }
    result.ok = true;
    return result;
}

bool UserDataPaths::SetConfiguredRoot(const QString& configuredRoot, QString* error)
{
    const UserDataValidationResult validation =
        ValidateRoot(configuredRoot, installDirectory_);
    if (!validation.ok) {
        if (error) {
            *error = validation.error;
        }
        return false;
    }
    configuredRoot_ = configuredRoot.trimmed().isEmpty()
                          ? QString()
                          : validation.normalizedRoot;
    if (error) {
        error->clear();
    }
    return true;
}

QString UserDataPaths::ConfiguredRoot() const
{
    return configuredRoot_;
}

QString UserDataPaths::EnsureRelative(const QString& relativePath,
                                      QString* error) const
{
    const QString root =
        configuredRoot_.isEmpty() ? DefaultRoot() : NormalizeAbsolutePath(configuredRoot_);
    const QString path = relativePath.isEmpty()
                             ? root
                             : QDir(root).filePath(relativePath);
    if (!QDir().mkpath(path)) {
        if (error) {
            *error =
                QStringLiteral("OkuFlow could not create folder: %1").arg(path);
        }
        return {};
    }
    if (error) {
        error->clear();
    }
    return QDir(path).absolutePath();
}

QString UserDataPaths::Root(QString* error) const
{
    return EnsureRelative({}, error);
}

QString UserDataPaths::Photos(QString* error) const
{
    return EnsureRelative(QStringLiteral("Photos"), error);
}

QString UserDataPaths::PhotosForDate(const QDate& date, QString* error) const
{
    return EnsureRelative(
        QStringLiteral("Photos/%1").arg(date.toString(Qt::ISODate)), error);
}

QString UserDataPaths::Recordings(QString* error) const
{
    return EnsureRelative(QStringLiteral("Recordings"), error);
}

QString UserDataPaths::RecordingsForDate(const QDate& date, QString* error) const
{
    return EnsureRelative(
        QStringLiteral("Recordings/%1").arg(date.toString(Qt::ISODate)), error);
}

QString UserDataPaths::Notes(QString* error) const
{
    return EnsureRelative(QStringLiteral("Notes"), error);
}

QString UserDataPaths::Analysis(QString* error) const
{
    return EnsureRelative(QStringLiteral("Analysis"), error);
}

QString UserDataPaths::Debug(QString* error) const
{
    return EnsureRelative(QStringLiteral("Debug"), error);
}

PhotoPairWriteResult UserDataPaths::WritePhotoPair(
    const QString& directory, const QString& preferredStem,
    const std::function<bool(QIODevice*)>& encodeOriginal,
    const std::function<bool(QIODevice*)>& encodeProcessed)
{
    PhotoPairWriteResult result;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const QString stem = attempt == 0 ? preferredStem
            : preferredStem + QStringLiteral("_") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QDir dir(directory);
        const QString original = dir.filePath(stem + QStringLiteral("_original.jpg"));
        const QString processed = dir.filePath(stem + QStringLiteral("_processed.jpg"));
        const QString originalTemp = original + QStringLiteral(".writing");
        const QString processedTemp = processed + QStringLiteral(".writing");
        const QString markerPath = dir.filePath(stem + QStringLiteral(".pair.pending"));
        const QString lockPath = dir.filePath(stem + QStringLiteral(".pair.lock"));
        // Existing lock metadata belongs to recovery, even if its owner died.
        if (QFileInfo::exists(lockPath)) {
            continue;
        }
        QLockFile lock(lockPath);
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) {
            continue;
        }
        bool occupied = false;
        for (const QString& path : {original, processed, originalTemp,
                                    processedTemp, markerPath}) {
            occupied = occupied || QFileInfo::exists(path);
        }
        if (occupied) {
            continue;
        }
        result.originalPath = original;
        result.processedPath = processed;
        QStringList owned;
        const auto encode = [&owned](const QString& path,
                                     const std::function<bool(QIODevice*)>& writer) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                return false;
            }
            owned.append(path);
            const bool saved = writer(&file) && file.flush();
            file.close();
            return saved;
        };
        bool markerCreated = false;
        const auto publishMarker = [&markerCreated, &markerPath]() {
            QFile marker(markerPath);
            if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                return false;
            }
            markerCreated = true;
            const QByteArray signature("OkuFlow photo pair v1\n");
            return marker.write(signature) == signature.size() && marker.flush();
        };
        // A valid marker certifies that both encodes have finished, never
        // merely that this writer reserved the stem.
        if (encode(processedTemp, encodeProcessed) &&
            encode(originalTemp, encodeOriginal) && publishMarker() &&
            QFile::rename(processedTemp, processed)) {
            owned.removeAll(processedTemp);
            owned.append(processed);
            if (QFile::rename(originalTemp, original)) {
                owned.removeAll(originalTemp);
                owned.append(original);
                result.committed = true;
            }
        }
        if (!result.committed) {
            for (const QString& path : owned) {
                if (QFileInfo::exists(path) && !QFile::remove(path)) {
                    result.leftoverPaths.append(path);
                }
            }
        }
        // Retain the completed-encode record when rollback could not finish.
        if (markerCreated && (result.committed || result.leftoverPaths.isEmpty()) &&
            !QFile::remove(markerPath)) {
            result.leftoverPaths.append(markerPath);
        }
        return result;
    }
    return result;
}

PhotoPairRecoveryResult UserDataPaths::RecoverInterruptedPhotoPairs(
    const QDateTime& staleBefore) const
{
    PhotoPairRecoveryResult result;
    QString error;
    const QString root = Photos(&error);
    if (root.isEmpty()) {
        if (!error.isEmpty()) {
            result.unresolvedPaths.append(error);
        }
        return result;
    }

    QStringList directories =
        QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                             QDir::Name | QDir::Reversed);
    // Scan every dated directory: an app may not be reopened promptly after
    // a crash, so limiting recovery to the newest dates would leave old
    // temporary files and incomplete transactions unexamined.
    directories.prepend(QString()); // Also cover the legacy Photos root.

    for (const QString& child : directories) {
        const QDir directory(
            child.isEmpty() ? root : QDir(root).filePath(child));
        const QFileInfoList files = directory.entryInfoList(
            {QStringLiteral("IMG_*_original.jpg"),
             QStringLiteral("IMG_*_processed.jpg"),
             QStringLiteral("IMG_*_original.jpg.writing"),
             QStringLiteral("IMG_*_processed.jpg.writing"),
             QStringLiteral("IMG_*.pair.lock"),
             QStringLiteral("IMG_*.pair.pending")},
            QDir::Files);
        QHash<QString, PhotoPairFiles> pairs;
        for (const QFileInfo& file : files) {
            const QString fileName = file.fileName();
            if (fileName.startsWith(QStringLiteral("IMG_")) &&
                (fileName.endsWith(QStringLiteral(".pair.lock")) ||
                 fileName.endsWith(QStringLiteral(".pair.pending")))) {
                const QString suffix = fileName.endsWith(QStringLiteral(".pair.lock"))
                    ? QStringLiteral(".pair.lock") : QStringLiteral(".pair.pending");
                const QString key = fileName.left(
                    fileName.size() - suffix.size());
                PhotoPairFiles& pair = pairs[key];
                pair.originalFinal = directory.filePath(
                    key + QStringLiteral("_original.jpg"));
                pair.processedFinal = directory.filePath(
                    key + QStringLiteral("_processed.jpg"));
                pair.originalTemp = pair.originalFinal +
                                    QStringLiteral(".writing");
                pair.processedTemp = pair.processedFinal +
                                     QStringLiteral(".writing");
                pair.transactionLock = directory.filePath(key + QStringLiteral(".pair.lock"));
                pair.ownershipMarker = directory.filePath(key + QStringLiteral(".pair.pending"));
                TrackNewestModification(pair, file);
                continue;
            }
            bool original = false;
            bool temporary = false;
            const QString key =
                PhotoPairKey(fileName, &original, &temporary);
            if (key.isEmpty()) {
                continue;
            }
            PhotoPairFiles& pair = pairs[key];
            // Keep all four expected paths even when only one member exists;
            // recovery may need to rename a temp into its absent final name.
            pair.originalFinal = directory.filePath(
                key + QStringLiteral("_original.jpg"));
            pair.processedFinal = directory.filePath(
                key + QStringLiteral("_processed.jpg"));
            pair.originalTemp = pair.originalFinal +
                                QStringLiteral(".writing");
            pair.processedTemp = pair.processedFinal +
                                 QStringLiteral(".writing");
            pair.transactionLock = directory.filePath(
                key + QStringLiteral(".pair.lock"));
            pair.ownershipMarker = directory.filePath(
                key + QStringLiteral(".pair.pending"));
            QString& slot = original
                                ? (temporary ? pair.originalTemp
                                             : pair.originalFinal)
                                : (temporary ? pair.processedTemp
                                             : pair.processedFinal);
            slot = file.absoluteFilePath();
            TrackNewestModification(pair, file);
        }

        for (auto pairIt = pairs.begin(); pairIt != pairs.end(); ++pairIt) {
            PhotoPairFiles& pair = pairIt.value();
            const auto exists = [](const QString& path) {
                return !path.isEmpty() && QFileInfo::exists(path);
            };
            std::unique_ptr<QLockFile> recoveryLock;
            if (exists(pair.transactionLock) || exists(pair.ownershipMarker)) {
                recoveryLock =
                    std::make_unique<QLockFile>(pair.transactionLock);
                // A live owner, regardless of duration, must never have its
                // transaction stolen. A dead owner is detected from the lock
                // metadata and can be recovered immediately after a crash.
                recoveryLock->setStaleLockTime(0);
                if (!recoveryLock->tryLock(0)) {
                    continue;
                }
            } else if (staleBefore.isValid() &&
                       pair.newestModification >= staleBefore) {
                // Compatibility for temp/final leftovers written before the
                // transaction lock existed.
                continue;
            }
            const auto removeFile = [&result, &exists](QString& path) {
                if (!exists(path)) {
                    path.clear();
                    return;
                }
                if (QFile::remove(path)) {
                    ++result.removedFiles;
                    path.clear();
                } else if (!result.unresolvedPaths.contains(path)) {
                    result.unresolvedPaths.append(path);
                }
            };

            QFile marker(pair.ownershipMarker);
            const QByteArray signature("OkuFlow photo pair v1\n");
            const bool encodedPair = marker.open(QIODevice::ReadOnly) &&
                marker.read(signature.size() + 1) == signature;
            marker.close();
            if (!encodedPair) {
                // A user may deliberately keep just one half of a capture.
                // Legacy temp names are disposable; finals are not evidence
                // that an interrupted transaction owns those files.
                removeFile(pair.originalTemp);
                removeFile(pair.processedTemp);
                if (exists(pair.ownershipMarker)) {
                    result.unresolvedPaths.append(pair.ownershipMarker);
                }
                continue;
            }

            bool originalFinal = exists(pair.originalFinal);
            bool processedFinal = exists(pair.processedFinal);
            if (originalFinal && processedFinal) {
                removeFile(pair.originalTemp);
                removeFile(pair.processedTemp);
                removeFile(pair.ownershipMarker);
                continue;
            }

            // The runtime renames processed first. A processed final plus an
            // original temp therefore proves both JPEG encodes completed;
            // finish the second rename. The symmetric case is handled too so
            // recovery remains correct if commit order changes later.
            bool completed = false;
            if (processedFinal && !originalFinal &&
                exists(pair.originalTemp) &&
                !pair.originalFinal.isEmpty()) {
                completed = QFile::rename(pair.originalTemp,
                                          pair.originalFinal);
                if (completed) {
                    ++result.completedPairs;
                    pair.originalTemp.clear();
                }
            } else if (originalFinal && !processedFinal &&
                       exists(pair.processedTemp) &&
                       !pair.processedFinal.isEmpty()) {
                completed = QFile::rename(pair.processedTemp,
                                          pair.processedFinal);
                if (completed) {
                    ++result.completedPairs;
                    pair.processedTemp.clear();
                }
            }
            if (completed) {
                removeFile(pair.originalTemp);
                removeFile(pair.processedTemp);
                removeFile(pair.ownershipMarker);
                continue;
            }

            // Existing finals may have been retained or replaced by the user
            // after a marker cleanup failure. Recovery never deletes them.
            removeFile(pair.originalTemp);
            removeFile(pair.processedTemp);
            for (const QString& path : {pair.originalFinal, pair.processedFinal}) {
                if (exists(path) && !result.unresolvedPaths.contains(path)) {
                    result.unresolvedPaths.append(path);
                }
            }
            if (!exists(pair.originalTemp) && !exists(pair.processedTemp) &&
                !exists(pair.originalFinal) && !exists(pair.processedFinal)) {
                removeFile(pair.ownershipMarker);
            }
        }
    }
    return result;
}

} // namespace okuflow
