#include "okuflow/app/windows_runtime_contract.hpp"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>
#include <windows.h>
#include <shellapi.h>

class WindowsRuntimeTests : public QObject {
    Q_OBJECT
private slots:
    void versions() {
        QVERIFY(!okuflow::SupportsWindowsRuntimeVersion({14, 43, 0, 0}));
        QVERIFY(okuflow::SupportsWindowsRuntimeVersion({14, 99, 0, 0}));
        QVERIFY(!okuflow::SupportsWindowsRuntimeVersion({15, 0, 0, 0}));
    }
    void installerResults() {
        using Result = okuflow::WindowsRuntimeInstallResult;
        for (std::uint32_t code : {1602u, 1223u, 0x80070642u, 0x800704c7u})
            QVERIFY(okuflow::ClassifyWindowsRuntimeInstallResult(code, false) == Result::Canceled);
        QVERIFY(okuflow::ClassifyWindowsRuntimeInstallResult(0, false) == Result::Failed);
        QVERIFY(okuflow::ClassifyWindowsRuntimeInstallResult(0, true) == Result::Success);
        for (std::uint32_t code : {1638u, 0x80070666u}) {
            QVERIFY(okuflow::ClassifyWindowsRuntimeInstallResult(code, false) == Result::Failed);
            QVERIFY(okuflow::ClassifyWindowsRuntimeInstallResult(code, true) == Result::Success);
        }
        QVERIFY(okuflow::ClassifyWindowsRuntimeInstallResult(3010, false) == Result::RestartRequired);
        QVERIFY(okuflow::ClassifyWindowsRuntimeInstallResult(1618, true) == Result::Failed);
    }
    void rejectUnreviewedInstaller() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("vc_redist.x64.exe");
        QVERIFY(!okuflow::VerifyWindowsRuntimeInstallerFile(path.toStdWString()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("unreviewed executable");
        file.close();
        QVERIFY(!okuflow::VerifyWindowsRuntimeInstallerFile(path.toStdWString()));
    }
    void commandLineRoundTrip() {
        const QStringList arguments{"", "plain", "with spaces", "quoted \"text\"", "trailing\\", "\\\\server\\share\\", "Türkçe"};
        std::wstring command = L"fixture.exe";
        for (const auto& argument : arguments) command += L" " + okuflow::QuoteWindowsArgument(argument.toStdWString());
        int count = 0;
        auto parsed = CommandLineToArgvW(command.c_str(), &count);
        QVERIFY(parsed);
        QCOMPARE(count, arguments.size() + 1);
        for (int i = 1; i < count; ++i) QCOMPARE(QString::fromWCharArray(parsed[i]), arguments[i - 1]);
        LocalFree(parsed);
    }
    void payloadDirectory() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto root = directory.path();
        QCOMPARE(QString::fromStdWString(okuflow::WindowsRuntimePayloadDirectory(root.toStdWString())), root);
        QVERIFY(QDir().mkdir(directory.filePath("app")));
        QCOMPARE(QDir::cleanPath(QString::fromStdWString(okuflow::WindowsRuntimePayloadDirectory(root.toStdWString()))),
                 directory.filePath("app"));
    }
    void launcherPreservesChildContract_data() {
        QTest::addColumn<bool>("nested");
        QTest::newRow("legacy-flat") << false;
        QTest::newRow("nested-runtime") << true;
    }
    void launcherPreservesChildContract() {
        QFETCH(bool, nested);
        // Never trigger UAC or change a machine with a missing runtime.
        if (!okuflow::IsSystemWindowsRuntimeReady()) QSKIP("Installed runtime needed for owned child forwarding fixture.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString working = directory.filePath("working directory");
        QVERIFY(QDir().mkdir(working));
        const QString launcher = directory.filePath("oku_flow.exe");
        QVERIFY(QFile::copy(QString::fromUtf8(OKUFLOW_LAUNCHER_TEST_BINARY), launcher));
        const QString payload = nested ? directory.filePath("app") : directory.path();
        QVERIFY(QDir().mkpath(payload));
        QVERIFY(QFile::copy(QString::fromUtf8(OKUFLOW_LAUNCHER_TEST_CHILD), QDir(payload).filePath("OkuFlowApp.exe")));
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("OKUFLOW_LAUNCHER_FIXTURE", "inherited environment");
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(working);
        const QStringList arguments{"--console", "--startup-profile", "file with spaces.json", "", "quoted \"text\"", "trailing\\", "Türkçe"};
        process.start(launcher, arguments);
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitCode(), 42);
        QCOMPARE(process.readAllStandardError(), QByteArray("fixture stderr"));
        const auto output = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QCOMPARE(output["arguments"].toArray(), QJsonArray::fromStringList(arguments));
        QCOMPARE(QDir::cleanPath(output["directory"].toString()), QDir::cleanPath(working));
        QCOMPARE(output["environment"].toString(), QString("inherited environment"));
    }
};
QTEST_GUILESS_MAIN(WindowsRuntimeTests)
#include "windows_runtime_tests.moc"
