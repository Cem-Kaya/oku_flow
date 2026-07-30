#pragma once

#include <QDate>
#include <QString>

#include <functional>

namespace openzoom {

struct UserDataValidationResult {
    bool ok{false};
    QString normalizedRoot;
    QString error;
};

struct LegacyMigrationResult {
    bool foundLegacyData{false};
    bool copiedData{false};
    bool cancelled{false};
    qint64 bytesCopied{0};
    qint64 totalBytes{0};
    QString error;
};

// Owns every user-created OpenZoom artifact path. Configuration and downloaded
// tools deliberately remain under the application-data directory.
class UserDataPaths final {
public:
    using MigrationProgress =
        std::function<bool(qint64 copiedBytes, qint64 totalBytes,
                           const QString& currentPath)>;

    explicit UserDataPaths(QString configuredRoot = {},
                           QString installDirectory = {});

    static QString DefaultRoot();
    static UserDataValidationResult ValidateRoot(
        const QString& requestedRoot,
        const QString& installDirectory = {});

    bool SetConfiguredRoot(const QString& configuredRoot, QString* error = nullptr);
    QString ConfiguredRoot() const;

    QString Root(QString* error = nullptr) const;
    QString Photos(QString* error = nullptr) const;
    QString PhotosForDate(const QDate& date, QString* error = nullptr) const;
    QString Recordings(QString* error = nullptr) const;
    QString RecordingsForDate(const QDate& date, QString* error = nullptr) const;
    QString Notes(QString* error = nullptr) const;
    QString Analysis(QString* error = nullptr) const;
    QString Debug(QString* error = nullptr) const;

    QString LegacyOutputRoot() const;
    bool HasLegacyData() const;
    LegacyMigrationResult MigrateLegacyOutput(
        const MigrationProgress& progress = {}) const;

private:
    QString EnsureRelative(const QString& relativePath, QString* error) const;

    QString configuredRoot_;
    QString installDirectory_;
};

} // namespace openzoom
