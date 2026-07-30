#include "openzoom/app/user_data_paths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

namespace openzoom {

namespace {

bool WriteFile(const QString& path, const QByteArray& contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(contents) == contents.size();
}

} // namespace

class UserDataPathsTests : public QObject {
    Q_OBJECT

private slots:
    void createsStableCategoryAndDateFolders();
    void rejectsApplicationDirectory();
    void migratesLegacyFilesWithoutDeletingSource();
    void cancellationDoesNotWriteBreadcrumb();
};

void UserDataPathsTests::createsStableCategoryAndDateFolders()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString install = temporary.filePath(QStringLiteral("install"));
    const QString root = temporary.filePath(QStringLiteral("documents/OpenZoom"));
    QVERIFY(QDir().mkpath(install));

    UserDataPaths paths(root, install);
    QString error;
    QCOMPARE(paths.PhotosForDate(QDate(2026, 7, 28), &error),
             QDir(root).filePath(QStringLiteral("Photos/2026-07-28")));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(paths.RecordingsForDate(QDate(2026, 7, 28), &error),
             QDir(root).filePath(QStringLiteral("Recordings/2026-07-28")));
    QCOMPARE(paths.Notes(&error),
             QDir(root).filePath(QStringLiteral("Notes")));
    QCOMPARE(paths.Analysis(&error),
             QDir(root).filePath(QStringLiteral("Analysis")));
    QCOMPARE(paths.Debug(&error),
             QDir(root).filePath(QStringLiteral("Debug")));
    QVERIFY(QFileInfo::exists(paths.Root()));
}

void UserDataPathsTests::rejectsApplicationDirectory()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString install = temporary.filePath(QStringLiteral("OpenZoom"));
    QVERIFY(QDir().mkpath(install));

    const UserDataValidationResult result =
        UserDataPaths::ValidateRoot(
            QDir(install).filePath(QStringLiteral("captures")), install);
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("outside")));
}

void UserDataPathsTests::migratesLegacyFilesWithoutDeletingSource()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString install = temporary.filePath(QStringLiteral("install"));
    const QString root = temporary.filePath(QStringLiteral("data"));
    const QString legacyPhoto =
        QDir(install).filePath(QStringLiteral("output/img/photo.jpg"));
    const QString legacyVideo =
        QDir(install).filePath(QStringLiteral("output/vid/recording.mp4"));
    const QString legacyNote =
        QDir(install).filePath(QStringLiteral("output/notes/session.html"));
    QVERIFY(WriteFile(legacyPhoto, QByteArray("photo")));
    QVERIFY(WriteFile(legacyVideo, QByteArray("video")));
    QVERIFY(WriteFile(legacyNote, QByteArray("notes")));

    UserDataPaths paths(root, install);
    QVERIFY(paths.HasLegacyData());
    const LegacyMigrationResult result = paths.MigrateLegacyOutput();
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(result.foundLegacyData);
    QVERIFY(result.copiedData);
    QVERIFY(QFileInfo::exists(legacyPhoto));
    QVERIFY(QFileInfo::exists(legacyVideo));
    QVERIFY(QFileInfo::exists(legacyNote));
    QVERIFY(QFileInfo::exists(
        QDir(root).filePath(QStringLiteral("Photos/photo.jpg"))));
    QVERIFY(QFileInfo::exists(
        QDir(root).filePath(QStringLiteral("Recordings/recording.mp4"))));
    QVERIFY(QFileInfo::exists(
        QDir(root).filePath(QStringLiteral("Notes/session.html"))));
    QVERIFY(QFileInfo::exists(
        QDir(root).filePath(QStringLiteral(".legacy-output-migrated"))));
    QVERIFY(QFileInfo::exists(
        QDir(install).filePath(QStringLiteral("output/MIGRATED.txt"))));
    QVERIFY(!paths.HasLegacyData());
}

void UserDataPathsTests::cancellationDoesNotWriteBreadcrumb()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString install = temporary.filePath(QStringLiteral("install"));
    const QString root = temporary.filePath(QStringLiteral("data"));
    QVERIFY(WriteFile(
        QDir(install).filePath(QStringLiteral("output/img/photo.jpg")),
        QByteArray(1024, 'x')));

    UserDataPaths paths(root, install);
    const LegacyMigrationResult result = paths.MigrateLegacyOutput(
        [](qint64, qint64, const QString&) { return false; });
    QVERIFY(result.cancelled);
    QVERIFY(!QFileInfo::exists(
        QDir(root).filePath(QStringLiteral(".legacy-output-migrated"))));
    QVERIFY(!QFileInfo::exists(
        QDir(install).filePath(QStringLiteral("output/MIGRATED.txt"))));
    QVERIFY(paths.HasLegacyData());
}

} // namespace openzoom

QTEST_GUILESS_MAIN(openzoom::UserDataPathsTests)

#include "user_data_paths_tests.moc"
