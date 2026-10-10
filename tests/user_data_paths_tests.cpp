#include "okuflow/app/user_data_paths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

namespace okuflow {

class UserDataPathsTests : public QObject {
    Q_OBJECT

private slots:
    void createsStableCategoryAndDateFolders();
    void rejectsApplicationDirectory();
    void rejectsJunctionIntoApplicationDirectory();
    void constructorRejectsPersistedRootInsideInstall();
    void completesInterruptedPhotoPairCommit();
    void preservesUnmarkedPhotoFinals();
    void avoidsExistingPhotoNames();
    void rollsBackOnlyWriterOwnedFiles();
    void preservesMarkedInterruptedPhotoOrphan();
    void removesFailedEncodeTemps();
    void reportsInvalidPhotoMarker();
    void leavesActivePhotoPairTransactionAlone();
};

void UserDataPathsTests::createsStableCategoryAndDateFolders()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString install = temporary.filePath(QStringLiteral("install"));
    const QString root = temporary.filePath(QStringLiteral("documents/OkuFlow"));
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
    const QString install = temporary.filePath(QStringLiteral("OkuFlow"));
    QVERIFY(QDir().mkpath(install));

    const UserDataValidationResult result =
        UserDataPaths::ValidateRoot(
            QDir(install).filePath(QStringLiteral("captures")), install);
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("outside")));
}

void UserDataPathsTests::rejectsJunctionIntoApplicationDirectory()
{
    // A root that is lexically outside the install directory but is a
    // Windows junction pointing back inside it must be rejected — the
    // containment check resolves links before comparing.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString install = temporary.filePath(QStringLiteral("install"));
    const QString target =
        QDir(install).filePath(QStringLiteral("inside"));
    QVERIFY(QDir().mkpath(target));
    const QString junction =
        temporary.filePath(QStringLiteral("outside-link"));

    // Junctions require no privilege; mklink is a cmd.exe builtin.
    QProcess mklink;
    mklink.start(QStringLiteral("cmd.exe"),
                 {QStringLiteral("/c"), QStringLiteral("mklink"),
                  QStringLiteral("/J"),
                  QDir::toNativeSeparators(junction),
                  QDir::toNativeSeparators(target)});
    if (!mklink.waitForFinished(10000) || mklink.exitCode() != 0 ||
        !QFileInfo::exists(junction)) {
        QSKIP("mklink /J unavailable; junction containment not testable");
    }

    const UserDataValidationResult result =
        UserDataPaths::ValidateRoot(junction, install);
    QVERIFY(!result.ok);
}

void UserDataPathsTests::constructorRejectsPersistedRootInsideInstall()
{
    // Persisted settings bypass ValidateRoot, so the constructor itself must
    // refuse a root inside the install directory and fall back to the
    // default (empty configured root). Checked via ConfiguredRoot() so the
    // test never touches the real Documents folder.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString install = temporary.filePath(QStringLiteral("OkuFlow"));
    QVERIFY(QDir().mkpath(install));

    const UserDataPaths rejected(
        QDir(install).filePath(QStringLiteral("captures")), install);
    QCOMPARE(rejected.ConfiguredRoot(), QString());

    const QString outside = temporary.filePath(QStringLiteral("elsewhere"));
    QVERIFY(QDir().mkpath(outside));
    const UserDataPaths accepted(outside, install);
    QCOMPARE(QDir::cleanPath(accepted.ConfiguredRoot()),
             QDir::cleanPath(outside));
}

void UserDataPathsTests::completesInterruptedPhotoPairCommit()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    UserDataPaths paths(temporary.filePath(QStringLiteral("OkuFlow")),
                        temporary.filePath(QStringLiteral("install")));
    const QString day = paths.PhotosForDate(QDate::currentDate());
    const QString stem = QStringLiteral("IMG_20260801_120000_000");
    const QString processed =
        QDir(day).filePath(stem + QStringLiteral("_processed.jpg"));
    const QString original =
        QDir(day).filePath(stem + QStringLiteral("_original.jpg"));
    const QString originalTemp = original + QStringLiteral(".writing");

    QFile processedFile(processed);
    QVERIFY(processedFile.open(QIODevice::WriteOnly));
    QCOMPARE(processedFile.write("processed"), qint64(9));
    processedFile.close();
    QFile originalFile(originalTemp);
    QVERIFY(originalFile.open(QIODevice::WriteOnly));
    QCOMPARE(originalFile.write("original"), qint64(8));
    originalFile.close();

    QFile marker(QDir(day).filePath(stem + QStringLiteral(".pair.pending")));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    QCOMPARE(marker.write("OkuFlow photo pair v1\n"), qint64(22));
    marker.close();
    const PhotoPairRecoveryResult result =
        paths.RecoverInterruptedPhotoPairs(
            QDateTime::currentDateTime().addSecs(60));
    QCOMPARE(result.completedPairs, 1);
    QVERIFY(result.unresolvedPaths.isEmpty());
    QVERIFY(QFileInfo::exists(processed));
    QVERIFY(QFileInfo::exists(original));
    QVERIFY(!QFileInfo::exists(originalTemp));
    originalFile.setFileName(original);
    QVERIFY(originalFile.open(QIODevice::ReadOnly));
    QCOMPARE(originalFile.readAll(), QByteArray("original"));
    QVERIFY(processedFile.open(QIODevice::ReadOnly));
    QCOMPARE(processedFile.readAll(), QByteArray("processed"));
}

void UserDataPathsTests::preservesUnmarkedPhotoFinals()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    UserDataPaths paths(temporary.filePath(QStringLiteral("OkuFlow")),
                        temporary.filePath(QStringLiteral("install")));
    const QString day = paths.PhotosForDate(QDate::currentDate());
    const QString orphan = QDir(day).filePath(QStringLiteral(
        "IMG_20260801_120001_000_processed.jpg"));
    QFile orphanFile(orphan);
    QVERIFY(orphanFile.open(QIODevice::WriteOnly));
    QCOMPARE(orphanFile.write("processed"), qint64(9));
    orphanFile.close();

    const PhotoPairRecoveryResult result =
        paths.RecoverInterruptedPhotoPairs(
            QDateTime::currentDateTime().addSecs(60));
    QCOMPARE(result.completedPairs, 0);
    QCOMPARE(result.removedFiles, 0);
    QVERIFY(result.unresolvedPaths.isEmpty());
    QVERIFY(QFileInfo::exists(orphan));
    const QString original = QDir(day).filePath(QStringLiteral(
        "IMG_user_retained_original.jpg"));
    QFile originalFile(original);
    QVERIFY(originalFile.open(QIODevice::WriteOnly));
    QCOMPARE(originalFile.write("keep original"), qint64(13));
    originalFile.close();
    QFile legacyTemp(orphan + QStringLiteral(".writing"));
    QVERIFY(legacyTemp.open(QIODevice::WriteOnly));
    legacyTemp.write("unfinished");
    legacyTemp.close();
    const auto cleanup = paths.RecoverInterruptedPhotoPairs(
        QDateTime::currentDateTime().addSecs(60));
    QCOMPARE(cleanup.removedFiles, 1);
    QVERIFY(originalFile.open(QIODevice::ReadOnly));
    QCOMPARE(originalFile.readAll(), QByteArray("keep original"));
    QVERIFY(orphanFile.open(QIODevice::ReadOnly));
    QCOMPARE(orphanFile.readAll(), QByteArray("processed"));
}

void UserDataPathsTests::leavesActivePhotoPairTransactionAlone()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    UserDataPaths paths(temporary.filePath(QStringLiteral("OkuFlow")),
                        temporary.filePath(QStringLiteral("install")));
    const QString day = paths.PhotosForDate(QDate::currentDate());
    const QString stem = QStringLiteral("IMG_20260801_120002_000");
    const QString processed =
        QDir(day).filePath(stem + QStringLiteral("_processed.jpg"));
    const QString original =
        QDir(day).filePath(stem + QStringLiteral("_original.jpg"));
    const QString originalTemp = original + QStringLiteral(".writing");
    const QString lockPath =
        QDir(day).filePath(stem + QStringLiteral(".pair.lock"));

    QFile processedFile(processed);
    QVERIFY(processedFile.open(QIODevice::WriteOnly));
    QCOMPARE(processedFile.write("processed"), qint64(9));
    processedFile.close();
    QFile originalFile(originalTemp);
    QVERIFY(originalFile.open(QIODevice::WriteOnly));
    QCOMPARE(originalFile.write("original"), qint64(8));
    originalFile.close();

    QLockFile activeTransaction(lockPath);
    activeTransaction.setStaleLockTime(0);
    QVERIFY(activeTransaction.tryLock(0));
    QFile marker(QDir(day).filePath(stem + QStringLiteral(".pair.pending")));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.write("OkuFlow photo pair v1\n");
    marker.close();
    const PhotoPairRecoveryResult result =
        paths.RecoverInterruptedPhotoPairs(
            QDateTime::currentDateTime().addSecs(60));
    QCOMPARE(result.completedPairs, 0);
    QCOMPARE(result.removedFiles, 0);
    QVERIFY(QFileInfo::exists(processed));
    QVERIFY(QFileInfo::exists(originalTemp));
    QVERIFY(!QFileInfo::exists(original));
    activeTransaction.unlock();
}

void UserDataPathsTests::avoidsExistingPhotoNames()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stem = QStringLiteral("IMG_same_timestamp");
    const QStringList suffixes{QStringLiteral("_original.jpg"),
        QStringLiteral("_processed.jpg"), QStringLiteral("_original.jpg.writing"),
        QStringLiteral("_processed.jpg.writing"), QStringLiteral(".pair.pending"),
        QStringLiteral(".pair.lock")};
    for (const QString& suffix : suffixes) {
        const QString path = temporary.filePath(stem + suffix);
        QFile existing(path);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("user bytes"), qint64(10));
        existing.close();
        const auto encoder = [](QIODevice* device) {
            return device->write("new bytes") == 9;
        };
        const PhotoPairWriteResult saved = UserDataPaths::WritePhotoPair(
            temporary.path(), stem, encoder, encoder);
        QVERIFY(saved.committed);
        QVERIFY(saved.leftoverPaths.isEmpty());
        QVERIFY(saved.originalPath != temporary.filePath(stem + QStringLiteral("_original.jpg")));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("user bytes"));
        existing.close();
        QVERIFY(QFile::remove(path));
    }
}

void UserDataPathsTests::rollsBackOnlyWriterOwnedFiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString stem = QStringLiteral("IMG_failed_commit");
    const QString original = temporary.filePath(stem + QStringLiteral("_original.jpg"));
    // Simulate another filesystem actor creating a final after reservation.
    const auto encodeOriginal = [&original](QIODevice* device) {
        QFile external(original);
        if (!external.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            external.write("retain me") != 9) {
            return false;
        }
        return device->write("original") == 8;
    };
    const PhotoPairWriteResult saved = UserDataPaths::WritePhotoPair(
        temporary.path(), stem, encodeOriginal,
        [](QIODevice* device) { return device->write("processed") == 9; });
    QVERIFY(!saved.committed);
    QVERIFY(saved.leftoverPaths.isEmpty());
    QFile external(original);
    QVERIFY(external.open(QIODevice::ReadOnly));
    QCOMPARE(external.readAll(), QByteArray("retain me"));
    QCOMPARE(QDir(temporary.path()).entryList(QDir::Files).size(), 1);
}

void UserDataPathsTests::preservesMarkedInterruptedPhotoOrphan()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    UserDataPaths paths(temporary.path());
    const QDir day(paths.PhotosForDate(QDate::currentDate()));
    QFile orphan(day.filePath(QStringLiteral("IMG_crashed_processed.jpg")));
    QVERIFY(orphan.open(QIODevice::WriteOnly));
    orphan.write("processed");
    orphan.close();
    QFile marker(day.filePath(QStringLiteral("IMG_crashed.pair.pending")));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.write("OkuFlow photo pair v1\n");
    marker.close();
    const auto recovered = paths.RecoverInterruptedPhotoPairs(QDateTime());
    QCOMPARE(recovered.removedFiles, 0);
    QCOMPARE(recovered.unresolvedPaths, QStringList{orphan.fileName()});
    QVERIFY(orphan.exists());
    QVERIFY(marker.exists());
}

void UserDataPathsTests::removesFailedEncodeTemps()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    bool markerVisibleDuringEncode = false;
    const auto saved = UserDataPaths::WritePhotoPair(
        temporary.path(), QStringLiteral("IMG_encode_failure"),
        [&temporary, &markerVisibleDuringEncode](QIODevice* device) {
            markerVisibleDuringEncode = QFileInfo::exists(temporary.filePath(
                QStringLiteral("IMG_encode_failure.pair.pending")));
            device->write("partial");
            return false;
        },
        [](QIODevice* device) { return device->write("processed") == 9; });
    QVERIFY(!saved.committed);
    QVERIFY(!markerVisibleDuringEncode);
    QVERIFY(saved.leftoverPaths.isEmpty());
    QVERIFY(QDir(temporary.path()).entryList(QDir::Files).isEmpty());
}

void UserDataPathsTests::reportsInvalidPhotoMarker()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    UserDataPaths paths(temporary.path());
    const QDir day(paths.PhotosForDate(QDate::currentDate()));
    QFile marker(day.filePath(QStringLiteral("IMG_invalid.pair.pending")));
    QVERIFY(marker.open(QIODevice::WriteOnly));
    marker.write("OkuFlow photo pair v1\nextra");
    marker.close();
    QFile original(day.filePath(QStringLiteral("IMG_invalid_original.jpg.writing")));
    QVERIFY(original.open(QIODevice::WriteOnly));
    original.write("partial");
    original.close();
    QFile processed(day.filePath(QStringLiteral("IMG_invalid_processed.jpg")));
    QVERIFY(processed.open(QIODevice::WriteOnly));
    processed.write("retain");
    processed.close();
    const auto recovered = paths.RecoverInterruptedPhotoPairs(QDateTime());
    QCOMPARE(recovered.completedPairs, 0);
    QCOMPARE(recovered.removedFiles, 1);
    QCOMPARE(recovered.unresolvedPaths, QStringList{marker.fileName()});
    QVERIFY(processed.exists());
    QVERIFY(marker.exists());
}

} // namespace okuflow

QTEST_GUILESS_MAIN(okuflow::UserDataPathsTests)

#include "user_data_paths_tests.moc"
