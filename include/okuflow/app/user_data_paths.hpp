#pragma once

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QStringList>

namespace okuflow {

struct UserDataValidationResult {
    bool ok{false};
    QString normalizedRoot;
    QString error;
};

struct PhotoPairRecoveryResult {
    int completedPairs{};
    int removedFiles{};
    // Paths that could neither be committed nor removed. The caller should
    // surface these because an incomplete capture may remain visible.
    QStringList unresolvedPaths;
};

// Owns every user-created OkuFlow artifact path. Configuration and downloaded
// tools deliberately remain under the application-data directory.
class UserDataPaths final {
public:
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

    // Reconciles stale IMG_* original/processed transactions after an
    // interrupted process. If one final rename succeeded and the other
    // fully encoded .writing file remains, the pair is completed; otherwise
    // incomplete finals and temps are rolled back together.
    PhotoPairRecoveryResult RecoverInterruptedPhotoPairs(
        const QDateTime& staleBefore) const;

private:
    QString EnsureRelative(const QString& relativePath, QString* error) const;

    QString configuredRoot_;
    QString installDirectory_;
};

} // namespace okuflow
