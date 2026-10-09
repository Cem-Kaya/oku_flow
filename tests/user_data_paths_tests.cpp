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
    void removesUnrecoverablePhotoOrphan();
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

    const PhotoPairRecoveryResult result =
        paths.RecoverInterruptedPhotoPairs(
            QDateTime::currentDateTime().addSecs(60));
    QCOMPARE(result.completedPairs, 1);
    QVERIFY(result.unresolvedPaths.isEmpty());
    QVERIFY(QFileInfo::exists(processed));
    QVERIFY(QFileInfo::exists(original));
    QVERIFY(!QFileInfo::exists(originalTemp));
}

void UserDataPathsTests::removesUnrecoverablePhotoOrphan()
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
    QCOMPARE(result.removedFiles, 1);
    QVERIFY(result.unresolvedPaths.isEmpty());
    QVERIFY(!QFileInfo::exists(orphan));
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

} // namespace okuflow

QTEST_GUILESS_MAIN(okuflow::UserDataPathsTests)

#include "user_data_paths_tests.moc"
