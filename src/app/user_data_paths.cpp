#include "openzoom/app/user_data_paths.hpp"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QVector>

#include <array>

namespace openzoom {

namespace {

QString NormalizeAbsolutePath(const QString& path)
{
    if (path.trimmed().isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QFileInfo(path.trimmed()).absoluteFilePath());
}

bool IsPathInside(const QString& candidate, const QString& parent)
{
    if (candidate.isEmpty() || parent.isEmpty()) {
        return false;
    }
    const QString child = QDir::toNativeSeparators(NormalizeAbsolutePath(candidate));
    QString root = QDir::toNativeSeparators(NormalizeAbsolutePath(parent));
    if (!root.endsWith(QDir::separator())) {
        root += QDir::separator();
    }
    return child.compare(root.left(root.size() - 1), Qt::CaseInsensitive) == 0 ||
           child.startsWith(root, Qt::CaseInsensitive);
}

bool DirectoryHasFiles(const QString& path)
{
    if (!QFileInfo::exists(path)) {
        return false;
    }
    QDirIterator iterator(path, QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    return iterator.hasNext();
}

qint64 DirectoryBytes(const QString& path)
{
    qint64 total = 0;
    QDirIterator iterator(path, QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        total += iterator.fileInfo().size();
    }
    return total;
}

bool CopyFileWithProgress(const QString& sourcePath,
                          const QString& destinationPath,
                          qint64 totalBytes,
                          qint64& copiedBytes,
                          const UserDataPaths::MigrationProgress& progress,
                          bool& cancelled,
                          QString& error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Could not read legacy file: %1").arg(sourcePath);
        return false;
    }
    if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
        error = QStringLiteral("Could not create migration folder: %1")
                    .arg(QFileInfo(destinationPath).absolutePath());
        return false;
    }
    if (QFileInfo::exists(destinationPath)) {
        copiedBytes += QFileInfo(sourcePath).size();
        if (progress && !progress(copiedBytes, totalBytes, sourcePath)) {
            cancelled = true;
            return false;
        }
        return true;
    }

    QSaveFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Could not create migrated file: %1")
                    .arg(destinationPath);
        return false;
    }

    constexpr qint64 kCopyChunkBytes = 1024 * 1024;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(kCopyChunkBytes);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            error = QStringLiteral("Could not finish reading: %1").arg(sourcePath);
            destination.cancelWriting();
            return false;
        }
        if (destination.write(chunk) != chunk.size()) {
            error = QStringLiteral("Could not finish writing: %1")
                        .arg(destinationPath);
            destination.cancelWriting();
            return false;
        }
        copiedBytes += chunk.size();
        if (progress && !progress(copiedBytes, totalBytes, sourcePath)) {
            cancelled = true;
            destination.cancelWriting();
            return false;
        }
    }
    if (!destination.commit()) {
        error = QStringLiteral("Could not finalize migrated file: %1")
                    .arg(destinationPath);
        return false;
    }
    return true;
}

bool CopyDirectoryContents(const QString& sourceRoot,
                           const QString& destinationRoot,
                           qint64 totalBytes,
                           qint64& copiedBytes,
                           const UserDataPaths::MigrationProgress& progress,
                           bool& cancelled,
                           QString& error)
{
    QDir sourceDirectory(sourceRoot);
    QDirIterator iterator(sourceRoot, QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString sourcePath = iterator.next();
        const QString relativePath = sourceDirectory.relativeFilePath(sourcePath);
        const QString destinationPath =
            QDir(destinationRoot).filePath(relativePath);
        if (!CopyFileWithProgress(sourcePath, destinationPath, totalBytes,
                                  copiedBytes, progress, cancelled, error)) {
            return false;
        }
    }
    return true;
}

} // namespace

UserDataPaths::UserDataPaths(QString configuredRoot, QString installDirectory)
    : configuredRoot_(configuredRoot.trimmed()),
      installDirectory_(NormalizeAbsolutePath(installDirectory))
{
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
    return QDir::cleanPath(QDir(documents).filePath(QStringLiteral("OpenZoom")));
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
        result.error = QStringLiteral("OpenZoom could not determine a folder.");
        return result;
    }
    if (IsPathInside(result.normalizedRoot, installDirectory)) {
        result.error = QStringLiteral(
            "Choose a folder outside the OpenZoom application folder. "
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
            QStringLiteral("OpenZoom could not create the selected folder.");
        return result;
    }

    const QString probePath = QDir(result.normalizedRoot)
                                  .filePath(QStringLiteral(".openzoom-write-test-%1.tmp")
                                                .arg(QUuid::createUuid().toString(
                                                    QUuid::WithoutBraces)));
    QFile probe(probePath);
    if (!probe.open(QIODevice::WriteOnly) ||
        probe.write("OpenZoom", 8) != 8) {
        result.error =
            QStringLiteral("OpenZoom cannot write to the selected folder.");
        probe.close();
        QFile::remove(probePath);
        return result;
    }
    probe.close();
    if (!QFile::remove(probePath)) {
        result.error = QStringLiteral(
            "OpenZoom wrote to the selected folder but could not remove its "
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
                QStringLiteral("OpenZoom could not create folder: %1").arg(path);
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

QString UserDataPaths::LegacyOutputRoot() const
{
    return installDirectory_.isEmpty()
               ? QString()
               : QDir(installDirectory_).filePath(QStringLiteral("output"));
}

bool UserDataPaths::HasLegacyData() const
{
    const QString legacyRoot = LegacyOutputRoot();
    if (legacyRoot.isEmpty() || !DirectoryHasFiles(legacyRoot)) {
        return false;
    }
    const QString root =
        configuredRoot_.isEmpty() ? DefaultRoot() : NormalizeAbsolutePath(configuredRoot_);
    return !QFileInfo::exists(
        QDir(root).filePath(QStringLiteral(".legacy-output-migrated")));
}

LegacyMigrationResult UserDataPaths::MigrateLegacyOutput(
    const MigrationProgress& progress) const
{
    LegacyMigrationResult result;
    const QString legacyRoot = LegacyOutputRoot();
    result.foundLegacyData = !legacyRoot.isEmpty() && DirectoryHasFiles(legacyRoot);
    if (!result.foundLegacyData) {
        return result;
    }

    QString rootError;
    const QString newRoot = Root(&rootError);
    if (newRoot.isEmpty()) {
        result.error = rootError;
        return result;
    }

    struct MigrationGroup {
        const char* source;
        const char* destination;
    };
    constexpr std::array groups{
        MigrationGroup{"img", "Photos"},
        MigrationGroup{"photos", "Photos"},
        MigrationGroup{"vid", "Recordings"},
        MigrationGroup{"recordings", "Recordings"},
        MigrationGroup{"notes", "Notes"},
        MigrationGroup{"assistant", "Analysis/Assistant"},
        MigrationGroup{"analysis", "Analysis"},
    };

    struct EligibleGroup {
        QString source;
        QString destination;
    };
    QVector<EligibleGroup> eligible;
    for (const MigrationGroup& group : groups) {
        const QString source = QDir(legacyRoot).filePath(
            QString::fromLatin1(group.source));
        const QString destination = QDir(newRoot).filePath(
            QString::fromLatin1(group.destination));
        if (!DirectoryHasFiles(source) || DirectoryHasFiles(destination)) {
            continue;
        }
        eligible.push_back({source, destination});
        result.totalBytes += DirectoryBytes(source);
    }

    for (const EligibleGroup& group : eligible) {
        if (!CopyDirectoryContents(group.source, group.destination,
                                   result.totalBytes, result.bytesCopied,
                                   progress, result.cancelled, result.error)) {
            return result;
        }
        result.copiedData = true;
    }

    const QByteArray contents =
        QStringLiteral(
            "OpenZoom copied compatible user files from:\n%1\n\n"
            "Copy completed: %2\n"
            "The original files were not deleted. You may review and remove "
            "them manually.\n")
            .arg(QDir::toNativeSeparators(legacyRoot),
                 QDateTime::currentDateTime().toString(Qt::ISODate))
            .toUtf8();

    QSaveFile migrationMarker(
        QDir(newRoot).filePath(QStringLiteral(".legacy-output-migrated")));
    if (!migrationMarker.open(QIODevice::WriteOnly | QIODevice::Text)) {
        result.error =
            QStringLiteral("Legacy files were copied, but OpenZoom could not "
                           "write the migration record.");
        return result;
    }
    if (migrationMarker.write(contents) != contents.size() ||
        !migrationMarker.commit()) {
        result.error =
            QStringLiteral("OpenZoom could not finalize the migration record.");
        return result;
    }

    // Best effort: this human-readable breadcrumb lives beside the original
    // files. A Program Files installation may be read-only, so the marker in
    // the user-data root remains the authority for suppressing repeat prompts.
    QSaveFile breadcrumb(
        QDir(legacyRoot).filePath(QStringLiteral("MIGRATED.txt")));
    if (breadcrumb.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (breadcrumb.write(contents) != contents.size()) {
            breadcrumb.cancelWriting();
        } else {
            breadcrumb.commit();
        }
    }
    return result;
}

} // namespace openzoom
