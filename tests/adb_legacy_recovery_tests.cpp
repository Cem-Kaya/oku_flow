// Guiless contract tests for TryReclaimLegacyBundledAdbServer. Every
// process here is a private fixture copy on a private ephemeral loopback
// port inside a QTemporaryDir distribution: no real adb, no port 5037, no
// device, no PhoneAdbTestHooks and no global runtime hook. The helper may
// stop only the exact fixture process these tests created inside a
// distribution whose manifest pin, adb entry hash, launcher ProductName
// (or injected launcher hash) and files root are pinned through the
// injected provenance seam. Production defaults remain the immutable
// reviewed constants and are also exercised against the foreign fixture,
// which they must refuse. The sibling scan is narrowed to each fixture's
// temp root through policy.siblingScanRootForTest, so a real owner
// instance running elsewhere neither blocks these tests nor is touched
// by them; production keeps the broad fail-closed scan.
#include "okuflow/capture/adb_legacy_recovery.hpp"

#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#ifndef OKUFLOW_ADB_LEGACY_FIXTURE
#error "adb_legacy_recovery_tests requires adb_legacy_fixture"
#endif

namespace {

using namespace okuflow;

QString FixturePath() { return QStringLiteral(OKUFLOW_ADB_LEGACY_FIXTURE); }
const wchar_t* FixtureProductName() { return L"OkuFlow Legacy Fixture"; }

QString Sha256HexFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool WriteTextFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

struct ReadyInfo {
    DWORD pid{0};
    quint16 port{0};
};

bool WaitReadyFile(const QString& path, ReadyInfo* ready, int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QList<QByteArray> fields = file.readAll().trimmed().split(' ');
            bool pidOk = false, portOk = false;
            if (fields.size() == 2) {
                const auto pid = fields[0].toULong(&pidOk);
                const auto port = fields[1].toUInt(&portOk);
                if (pidOk && portOk && pid) {
                    ready->pid = static_cast<DWORD>(pid);
                    ready->port = static_cast<quint16>(port);
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// Observation only: tests never terminate a process they found by PID. Own
// QProcess children are cleaned up through QProcess itself.
bool ProcessExited(DWORD pid)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
}

bool WaitProcessExited(DWORD pid, int timeoutMs = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ProcessExited(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ProcessExited(pid);
}

struct RunningFixture {
    std::unique_ptr<QProcess> process;
    ReadyInfo ready;
    ~RunningFixture()
    {
        if (process && process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(3000);
        }
    }
    bool Alive() const { return ready.pid && !ProcessExited(ready.pid); }
};

std::unique_ptr<RunningFixture> StartFixture(const QString& executable,
                                             const QStringList& arguments,
                                             const QString& readyFile)
{
    auto running = std::make_unique<RunningFixture>();
    running->process = std::make_unique<QProcess>();
    running->process->setProgram(executable);
    running->process->setArguments(arguments);
    running->process->setWorkingDirectory(QFileInfo(executable).absolutePath());
    running->process->start();
    if (!running->process->waitForStarted(5000)) return {};
    if (!WaitReadyFile(readyFile, &running->ready)) return {};
    return running;
}

struct Distribution {
    QString bundleRoot;
    QString payloadRoot;
    QString platformTools;
    QString adbPath;
    QString launcherPath;
    std::string manifestSha256;
};

// Builds one private reviewed-shape distribution around fixture copies.
// `adbEntryHash` overrides the manifest's adb.exe hash for mismatch rows;
// the manifest hash is always computed from the written manifest bytes.
Distribution BuildDistribution(const QString& parentDir, bool nested,
                               const QString& adbEntryHash = {})
{
    Distribution distribution;
    distribution.bundleRoot = QDir(parentDir).filePath(QStringLiteral("bundle"));
    distribution.payloadRoot = nested
        ? QDir(distribution.bundleRoot).filePath(QStringLiteral("app"))
        : distribution.bundleRoot;
    distribution.platformTools =
        QDir(distribution.payloadRoot).filePath(QStringLiteral("platform-tools"));
    QDir().mkpath(distribution.platformTools);
    const auto copyFixture = [](const QString& target) {
        QFile::remove(target);
        return QFile::copy(FixturePath(), target);
    };
    distribution.adbPath =
        QDir(distribution.platformTools).filePath(QStringLiteral("adb.exe"));
    copyFixture(distribution.adbPath);
    copyFixture(QDir(distribution.payloadRoot).filePath(QStringLiteral("OkuFlowApp.exe")));
    distribution.launcherPath =
        QDir(distribution.bundleRoot).filePath(QStringLiteral("oku_flow.exe"));
    copyFixture(distribution.launcherPath);
    for (const QString& name : {QStringLiteral("AdbWinApi.dll"),
                                QStringLiteral("AdbWinUsbApi.dll"),
                                QStringLiteral("NOTICE.txt"),
                                QStringLiteral("source.properties")}) {
        WriteTextFile(QDir(distribution.platformTools).filePath(name),
                      QByteArrayLiteral("fixture\n"));
    }
    const QString adbHash = adbEntryHash.isEmpty() ? Sha256HexFile(distribution.adbPath)
                                                   : adbEntryHash;
    QJsonArray files;
    const auto entry = [](const QString& path, const QString& sha256) {
        QJsonObject object;
        object.insert(QStringLiteral("path"), path);
        object.insert(QStringLiteral("sha256"), sha256);
        return object;
    };
    files.append(entry(QStringLiteral("platform-tools/adb.exe"), adbHash));
    for (const QString& name : {QStringLiteral("AdbWinApi.dll"),
                                QStringLiteral("AdbWinUsbApi.dll"),
                                QStringLiteral("NOTICE.txt"),
                                QStringLiteral("source.properties")}) {
        files.append(entry(QStringLiteral("platform-tools/") + name,
                           Sha256HexFile(QDir(distribution.platformTools).filePath(name))));
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), QStringLiteral("37.0.0-okuflow.1"));
    manifest.insert(QStringLiteral("protocol"), 41);
    manifest.insert(QStringLiteral("sourceBuilt"), true);
    manifest.insert(QStringLiteral("files"), files);
    const QString manifestPath =
        QDir(distribution.platformTools).filePath(QStringLiteral("adb-build.json"));
    WriteTextFile(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    distribution.manifestSha256 = Sha256HexFile(manifestPath).toStdString();
    return distribution;
}

std::wstring TestMutexName()
{
    return L"Local\\OkuFlow.LegacyAdbRecovery.Tests." +
           std::to_wstring(GetCurrentProcessId());
}

LegacyAdbRecoveryRequest MakeRequest(quint16 port, const Distribution& distribution,
                                     const QString& requiredRoot,
                                     const std::atomic<bool>* canceled = nullptr)
{
    LegacyAdbRecoveryRequest request;
    request.port = port;
    request.canceled = canceled;
    request.externallyConfirmedUnresponsive = true;
    request.phoneUseIntent = true;
    request.noActiveOrStartingPhoneSessions = true;
    request.expectations.reviewedManifestSha256 = distribution.manifestSha256;
    request.expectations.expectedLauncherProductName = FixtureProductName();
    request.expectations.requiredDistributionRoot =
        QDir::toNativeSeparators(QFileInfo(requiredRoot).canonicalFilePath()).toStdWString();
    // Narrow the sibling scan to this fixture's temp tree so a real owner
    // instance elsewhere can stay running; production scans broadly.
    request.policy.siblingScanRootForTest =
        QDir::toNativeSeparators(QFileInfo(requiredRoot).canonicalFilePath()).toStdWString();
    request.policy.mutexAcquireTimeout = std::chrono::milliseconds(1000);
    request.policy.hostKillConnectTimeout = std::chrono::milliseconds(500);
    request.policy.hostKillExitWait = std::chrono::milliseconds(400);
    request.policy.forcedExitWait = std::chrono::milliseconds(2000);
    request.policy.mutexNameOverride = TestMutexName();
    return request;
}

std::unique_ptr<RunningFixture> StartServer(const Distribution& distribution,
                                            const QString& readyFile,
                                            const QString& killMode,
                                            quint16 port = 0)
{
    return StartFixture(distribution.adbPath,
                        {QStringLiteral("server"), QStringLiteral("--port"),
                         QString::number(port), QStringLiteral("--ready-file"), readyFile,
                         QStringLiteral("--kill-mode"), killMode},
                        readyFile);
}

std::unique_ptr<RunningFixture> StartSibling(const QString& parentDir,
                                             const QString& readyFile)
{
    const QString directory = QDir(parentDir).filePath(QStringLiteral("sibling"));
    QDir().mkpath(directory);
    const QString executable = QDir(directory).filePath(QStringLiteral("oku_flow.exe"));
    QFile::remove(executable);
    if (!QFile::copy(FixturePath(), executable)) return {};
    return StartFixture(executable,
                        {QStringLiteral("sleep"), QStringLiteral("--ready-file"), readyFile},
                        readyFile);
}

} // namespace

class AdbLegacyRecoveryTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // No real-owner skip: every request narrows the sibling scan to
        // its private temp root, so a running owner instance elsewhere
        // is neither required to close nor able to block these rows.
        QVERIFY(QFileInfo::exists(FixturePath()));
        QVERIFY(!Sha256HexFile(FixturePath()).isEmpty());
        QVERIFY(LegacyAdbProvenanceExpectations{}.reviewedLauncherSha256 ==
                std::vector<std::string>{kReviewedLegacyLauncherSha256});
        QVERIFY(LegacyAdbRecoveryPolicy{}.siblingScanRootForTest.empty());
    }

    void recoversViaHostKill_data()
    {
        QTest::addColumn<bool>("nested");
        QTest::addColumn<QString>("killMode");
        QTest::newRow("flat-ok") << false << QStringLiteral("ok");
        QTest::newRow("flat-silent") << false << QStringLiteral("silent");
        QTest::newRow("nested-ok") << true << QStringLiteral("ok");
        QTest::newRow("nested-silent") << true << QStringLiteral("silent");
    }

    void recoversViaHostKill()
    {
        QFETCH(bool, nested);
        QFETCH(QString, killMode);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), nested);
        QVERIFY(!distribution.manifestSha256.empty());
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  killMode);
        QVERIFY(server);
        QVERIFY(server->Alive());
        QVERIFY(server->ready.port != 0);
        QVERIFY(server->ready.port != 5037);

        const auto result = TryReclaimLegacyBundledAdbServer(
            MakeRequest(server->ready.port, distribution, temp.path()));
        if (result.status != LegacyAdbRecoveryStatus::Recovered)
            qWarning("Recovery refused: status=%d detail=%d diagnostic=%s",
                     int(result.status), int(result.detail), result.diagnostic.c_str());
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::Recovered);
        QCOMPARE(result.detail, LegacyAdbRecoveryDetail::None);
        QVERIFY(result.stoppedViaHostKill);
        QVERIFY(!result.usedForcedTermination);
        QVERIFY(result.hostKillDelivered);
        QCOMPARE(result.listenerProcessId, server->ready.pid);
        QVERIFY(WaitProcessExited(server->ready.pid));

        // The old server is gone; a second attempt has nothing to manage.
        const auto second = TryReclaimLegacyBundledAdbServer(
            MakeRequest(server->ready.port, distribution, temp.path()));
        QCOMPARE(second.status, LegacyAdbRecoveryStatus::NotManaged);
        QCOMPARE(second.detail, LegacyAdbRecoveryDetail::NoLoopbackListener);
    }

    void recoversViaForcedTerminationWhenKillIgnored()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), true);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ignore"));
        QVERIFY(server);
        const auto result = TryReclaimLegacyBundledAdbServer(
            MakeRequest(server->ready.port, distribution, temp.path()));
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::Recovered);
        QVERIFY(result.hostKillDelivered);
        QVERIFY(!result.stoppedViaHostKill);
        QVERIFY(result.usedForcedTermination);
        QVERIFY(WaitProcessExited(server->ready.pid));
    }

    void foreignServerIsNotManagedAndUntouched()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString directory = QDir(temp.path()).filePath(QStringLiteral("foreign"));
        QDir().mkpath(directory);
        const QString executable = QDir(directory).filePath(QStringLiteral("adb.exe"));
        QVERIFY(QFile::copy(FixturePath(), executable));
        const QString readyFile = QDir(temp.path()).filePath(QStringLiteral("ready.txt"));
        auto server = StartFixture(executable,
                                   {QStringLiteral("server"), QStringLiteral("--port"),
                                    QStringLiteral("0"), QStringLiteral("--ready-file"),
                                    readyFile, QStringLiteral("--kill-mode"),
                                    QStringLiteral("ok")},
                                   readyFile);
        QVERIFY(server);
        // Production-pinned expectations: a same-basename server outside a
        // platform-tools distribution is foreign even before hashing.
        LegacyAdbRecoveryRequest request;
        request.port = server->ready.port;
        request.externallyConfirmedUnresponsive = true;
        request.phoneUseIntent = true;
        request.noActiveOrStartingPhoneSessions = true;
        request.policy.mutexNameOverride = TestMutexName();
        // Provenance expectations stay at production defaults above; only
        // the private sibling scan is narrowed to this temp tree.
        request.policy.siblingScanRootForTest =
            QDir::toNativeSeparators(QFileInfo(temp.path()).canonicalFilePath()).toStdWString();
        const auto result = TryReclaimLegacyBundledAdbServer(request);
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::NotManaged);
        QCOMPARE(result.detail, LegacyAdbRecoveryDetail::ImageNotBundledAdb);
        QVERIFY(server->Alive());
    }

    void refusesOnProvenanceMismatch_data()
    {
        QTest::addColumn<int>("mismatch");
        QTest::addColumn<int>("expectedDetail");
        QTest::newRow("manifest-pin") << 0
            << static_cast<int>(LegacyAdbRecoveryDetail::ManifestHashMismatch);
        QTest::newRow("adb-entry-hash") << 1
            << static_cast<int>(LegacyAdbRecoveryDetail::AdbExecutableHashMismatch);
        QTest::newRow("product-name") << 2
            << static_cast<int>(LegacyAdbRecoveryDetail::LauncherProductMismatch);
        QTest::newRow("files-root") << 3
            << static_cast<int>(LegacyAdbRecoveryDetail::DistributionOutsideExpectedRoot);
    }

    void refusesOnProvenanceMismatch()
    {
        QFETCH(int, mismatch);
        QFETCH(int, expectedDetail);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString wrongAdbHash = mismatch == 1
            ? QString::fromLatin1(QCryptographicHash::hash(
                  QByteArrayLiteral("not the fixture"), QCryptographicHash::Sha256).toHex())
            : QString{};
        const Distribution distribution = BuildDistribution(temp.path(), false, wrongAdbHash);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);
        QString requiredRoot = temp.path();
        if (mismatch == 3) {
            requiredRoot = QDir(temp.path()).filePath(QStringLiteral("elsewhere"));
            QDir().mkpath(requiredRoot);
        }
        auto request = MakeRequest(server->ready.port, distribution, requiredRoot);
        if (mismatch == 0)
            request.expectations.reviewedManifestSha256 = std::string(64, '0');
        if (mismatch == 2)
            request.expectations.expectedLauncherProductName = L"Not The Fixture Product";
        const auto result = TryReclaimLegacyBundledAdbServer(request);
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::NotManaged);
        QCOMPARE(static_cast<int>(result.detail), expectedDetail);
        QVERIFY(server->Alive());
    }

    void launcherHashAllowlistFallback_data()
    {
        QTest::addColumn<bool>("wrongProduct");
        QTest::addColumn<int>("hashMode"); // 0 fixture, 1 wrong, 2 malformed
        QTest::addColumn<int>("expectedStatus");
        QTest::addColumn<int>("expectedDetail");
        QTest::newRow("wrong-product-exact-hash") << true << 0
            << static_cast<int>(LegacyAdbRecoveryStatus::Recovered)
            << static_cast<int>(LegacyAdbRecoveryDetail::None);
        QTest::newRow("wrong-product-wrong-hash") << true << 1
            << static_cast<int>(LegacyAdbRecoveryStatus::NotManaged)
            << static_cast<int>(LegacyAdbRecoveryDetail::LauncherProductMismatch);
        QTest::newRow("exact-product-wrong-hash-still-or") << false << 1
            << static_cast<int>(LegacyAdbRecoveryStatus::Recovered)
            << static_cast<int>(LegacyAdbRecoveryDetail::None);
        QTest::newRow("malformed-allowlist-fails-closed") << false << 2
            << static_cast<int>(LegacyAdbRecoveryStatus::Failed)
            << static_cast<int>(LegacyAdbRecoveryDetail::ManifestInvalid);
    }

    void launcherHashAllowlistFallback()
    {
        QFETCH(bool, wrongProduct);
        QFETCH(int, hashMode);
        QFETCH(int, expectedStatus);
        QFETCH(int, expectedDetail);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), false);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);
        auto request = MakeRequest(server->ready.port, distribution, temp.path());
        if (wrongProduct)
            request.expectations.expectedLauncherProductName = L"Not The Fixture Product";
        const QString fixtureLauncherHash = Sha256HexFile(distribution.launcherPath);
        QVERIFY(!fixtureLauncherHash.isEmpty());
        const QString wrongHash = QString::fromLatin1(QCryptographicHash::hash(
            QByteArrayLiteral("not the launcher"), QCryptographicHash::Sha256).toHex());
        if (hashMode == 0)
            request.expectations.reviewedLauncherSha256 = {fixtureLauncherHash.toStdString()};
        else if (hashMode == 1)
            request.expectations.reviewedLauncherSha256 = {wrongHash.toStdString()};
        else
            request.expectations.reviewedLauncherSha256 = {"not-hex"};
        const auto result = TryReclaimLegacyBundledAdbServer(request);
        QCOMPARE(static_cast<int>(result.status), expectedStatus);
        QCOMPARE(static_cast<int>(result.detail), expectedDetail);
        if (expectedStatus == static_cast<int>(LegacyAdbRecoveryStatus::Recovered)) {
            QVERIFY(result.stoppedViaHostKill);
            QVERIFY(WaitProcessExited(server->ready.pid));
        } else {
            QVERIFY(!result.hostKillDelivered);
            QVERIFY(server->Alive());
        }
    }

    void siblingOutsideScanRootDoesNotBlockRecovery()
    {
        QTemporaryDir temp;
        QTemporaryDir outside;
        QVERIFY(temp.isValid());
        QVERIFY(outside.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), false);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);
        // A same-named fixture outside the scan root stands in for a real
        // owner instance elsewhere: under the test seam it neither blocks
        // recovery nor is touched, while in-root siblings still refuse
        // (see refusesWhileSiblingRunningThenRecovers).
        auto outsideSibling = StartSibling(
            outside.path(), QDir(outside.path()).filePath(QStringLiteral("sibling-ready.txt")));
        QVERIFY(outsideSibling);
        const auto result = TryReclaimLegacyBundledAdbServer(
            MakeRequest(server->ready.port, distribution, temp.path()));
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::Recovered);
        QVERIFY(WaitProcessExited(server->ready.pid));
        QVERIFY(outsideSibling->Alive());
    }

    void refusesWhileSiblingRunningThenRecovers()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), false);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);
        {
            auto sibling = StartSibling(
                temp.path(), QDir(temp.path()).filePath(QStringLiteral("sibling-ready.txt")));
            QVERIFY(sibling);
            const auto refused = TryReclaimLegacyBundledAdbServer(
                MakeRequest(server->ready.port, distribution, temp.path()));
            QCOMPARE(refused.status, LegacyAdbRecoveryStatus::Busy);
            QCOMPARE(refused.detail, LegacyAdbRecoveryDetail::SiblingOkuFlowRunning);
            QVERIFY(server->Alive());
        }
        const auto recovered = TryReclaimLegacyBundledAdbServer(
            MakeRequest(server->ready.port, distribution, temp.path()));
        QCOMPARE(recovered.status, LegacyAdbRecoveryStatus::Recovered);
        QVERIFY(WaitProcessExited(server->ready.pid));
    }

    void refusesOnCancelAndMissingPreconditions()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), false);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);

        std::atomic<bool> canceled{true};
        auto canceledRequest =
            MakeRequest(server->ready.port, distribution, temp.path(), &canceled);
        QCOMPARE(TryReclaimLegacyBundledAdbServer(canceledRequest).status,
                 LegacyAdbRecoveryStatus::Canceled);
        QVERIFY(server->Alive());

        auto unconfirmed = MakeRequest(server->ready.port, distribution, temp.path());
        unconfirmed.externallyConfirmedUnresponsive = false;
        const auto unconfirmedResult = TryReclaimLegacyBundledAdbServer(unconfirmed);
        QCOMPARE(unconfirmedResult.status, LegacyAdbRecoveryStatus::Busy);
        QCOMPARE(unconfirmedResult.detail,
                 LegacyAdbRecoveryDetail::PreconditionsNotConfirmed);
        QVERIFY(server->Alive());

        auto sessionsBusy = MakeRequest(server->ready.port, distribution, temp.path());
        sessionsBusy.hasActiveOrStartingPhoneSessions = [] { return true; };
        const auto sessionsResult = TryReclaimLegacyBundledAdbServer(sessionsBusy);
        QCOMPARE(sessionsResult.status, LegacyAdbRecoveryStatus::Busy);
        QCOMPARE(sessionsResult.detail, LegacyAdbRecoveryDetail::SessionsBusy);
        QVERIFY(server->Alive());
    }

    void identityChangedWhenListenerReplacedAfterClassification()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), false);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);
        const QString foreignDirectory =
            QDir(temp.path()).filePath(QStringLiteral("foreign"));
        QDir().mkpath(foreignDirectory);
        const QString foreignExecutable =
            QDir(foreignDirectory).filePath(QStringLiteral("adb.exe"));
        QVERIFY(QFile::copy(FixturePath(), foreignExecutable));
        const QString foreignReady =
            QDir(temp.path()).filePath(QStringLiteral("foreign-ready.txt"));
        std::unique_ptr<RunningFixture> foreign;
        auto request = MakeRequest(server->ready.port, distribution, temp.path());
        const quint16 port = server->ready.port;
        bool hookRan = false;
        request.policy.afterInitialClassificationForTest = [&] {
            hookRan = true;
            server->process->kill();
            server->process->waitForFinished(3000);
            foreign = StartFixture(foreignExecutable,
                                   {QStringLiteral("server"), QStringLiteral("--port"),
                                    QString::number(port), QStringLiteral("--ready-file"),
                                    foreignReady, QStringLiteral("--kill-mode"),
                                    QStringLiteral("ok")},
                                   foreignReady);
        };
        const auto result = TryReclaimLegacyBundledAdbServer(request);
        QVERIFY(hookRan);
        QVERIFY(foreign);
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::IdentityChanged);
        QVERIFY(foreign->Alive());
    }

    void listenerReplacedAfterHostKillConnectGetsNoKill()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), false);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);
        const QString foreignDirectory =
            QDir(temp.path()).filePath(QStringLiteral("foreign"));
        QDir().mkpath(foreignDirectory);
        const QString foreignExecutable =
            QDir(foreignDirectory).filePath(QStringLiteral("adb.exe"));
        QVERIFY(QFile::copy(FixturePath(), foreignExecutable));
        const QString foreignReady =
            QDir(temp.path()).filePath(QStringLiteral("foreign-ready.txt"));
        std::unique_ptr<RunningFixture> foreign;
        auto request = MakeRequest(server->ready.port, distribution, temp.path());
        const quint16 port = server->ready.port;
        bool hookRan = false;
        // Runs after the helper's TCP connect succeeds but before it may
        // send: swap the listener for a foreign kill-mode-ok server. The
        // post-connect revalidation must abort with no bytes delivered,
        // so the foreign server (which would exit on host:kill) stays up
        // and recovery is never credited from the stale classification.
        request.policy.afterHostKillConnectForTest = [&] {
            hookRan = true;
            server->process->kill();
            server->process->waitForFinished(3000);
            foreign = StartFixture(foreignExecutable,
                                   {QStringLiteral("server"), QStringLiteral("--port"),
                                    QString::number(port), QStringLiteral("--ready-file"),
                                    foreignReady, QStringLiteral("--kill-mode"),
                                    QStringLiteral("ok")},
                                   foreignReady);
        };
        const auto result = TryReclaimLegacyBundledAdbServer(request);
        QVERIFY(hookRan);
        QVERIFY(foreign);
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::IdentityChanged);
        QVERIFY(result.detail == LegacyAdbRecoveryDetail::ProcessChangedOrExited ||
                result.detail == LegacyAdbRecoveryDetail::ListenerChanged);
        QVERIFY(!result.hostKillDelivered);
        QVERIFY(!result.stoppedViaHostKill);
        QVERIFY(!result.usedForcedTermination);
        QVERIFY(foreign->Alive());
    }

    void cancelsAfterClassificationBeforeStop()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), false);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);
        std::atomic<bool> canceled{false};
        auto request = MakeRequest(server->ready.port, distribution, temp.path(), &canceled);
        request.policy.afterInitialClassificationForTest = [&] { canceled.store(true); };
        const auto result = TryReclaimLegacyBundledAdbServer(request);
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::Canceled);
        QVERIFY(server->Alive());
    }

    void siblingAppearingAfterClassificationRefuses()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), false);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);
        std::unique_ptr<RunningFixture> sibling;
        auto request = MakeRequest(server->ready.port, distribution, temp.path());
        request.policy.afterInitialClassificationForTest = [&] {
            sibling = StartSibling(
                temp.path(), QDir(temp.path()).filePath(QStringLiteral("sibling-ready.txt")));
        };
        const auto result = TryReclaimLegacyBundledAdbServer(request);
        QVERIFY(sibling);
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::Busy);
        QCOMPARE(result.detail, LegacyAdbRecoveryDetail::SiblingOkuFlowRunning);
        QVERIFY(server->Alive());
    }

    void refusesWhileRecoveryMutexHeld()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const Distribution distribution = BuildDistribution(temp.path(), false);
        auto server = StartServer(distribution,
                                  QDir(temp.path()).filePath(QStringLiteral("ready.txt")),
                                  QStringLiteral("ok"));
        QVERIFY(server);
        std::atomic<bool> acquired{false};
        std::atomic<bool> release{false};
        std::thread holder([&] {
            HANDLE mutex = CreateMutexW(nullptr, FALSE, TestMutexName().c_str());
            if (!mutex) return;
            if (WaitForSingleObject(mutex, 5000) == WAIT_OBJECT_0) {
                acquired.store(true);
                while (!release.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                ReleaseMutex(mutex);
            }
            CloseHandle(mutex);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!acquired.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        QVERIFY(acquired.load());
        auto request = MakeRequest(server->ready.port, distribution, temp.path());
        request.policy.mutexAcquireTimeout = std::chrono::milliseconds(250);
        const auto result = TryReclaimLegacyBundledAdbServer(request);
        release.store(true);
        holder.join();
        QCOMPARE(result.status, LegacyAdbRecoveryStatus::Busy);
        QCOMPARE(result.detail, LegacyAdbRecoveryDetail::MutexBusy);
        QVERIFY(server->Alive());
    }
};

QTEST_APPLESS_MAIN(AdbLegacyRecoveryTests)
#include "adb_legacy_recovery_tests.moc"
