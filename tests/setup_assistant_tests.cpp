#include "okuflow/app/setup_assistant.hpp"
#include "okuflow/common/maxine_superres.hpp"
#include "okuflow/ui/ui_translation.hpp"
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QTranslator>
#include <QPixmap>
#include <QtTest>

namespace okuflow {
bool MaxineSuperRes::IsRuntimeInstalled(const std::wstring&) { return false; }
}

class SetupAssistantTests : public QObject {
    Q_OBJECT
private slots:
    void runtimeStatus_data() {
        QTest::addColumn<bool>("installed");
        QTest::addColumn<QString>("language");
        for (const auto& language : {QString("en"), QString("tr"), QString("de")}) {
            QTest::newRow(qPrintable(language + " supported runtime")) << true << language;
            QTest::newRow(qPrintable(language + " missing runtime")) << false << language;
        }
    }
    void runtimeStatus() {
        QFETCH(bool, installed);
        QFETCH(QString, language);
        QTranslator translator;
        if (language != "en") {
            QVERIFY(translator.load(":/i18n/okuflow_" + language + ".qm"));
            QCoreApplication::installTranslator(&translator);
        }
        okuflow::SetupAssistantDialog dialog({}, false, nullptr,
            [installed] { return okuflow::WindowsRuntimeStatus{installed}; });
        dialog.setAttribute(Qt::WA_DeleteOnClose, false);
        dialog.resize(560, 420);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        okuflow::RetranslateWidgetTree(&dialog);
        QCoreApplication::processEvents();
        auto* row = dialog.findChild<QWidget*>("windowsRuntimeRow");
        auto* status = dialog.findChild<QLabel*>("windowsRuntimeStatus");
        auto* install = dialog.findChild<QPushButton*>("windowsRuntimeInstall");
        QVERIFY(row && status && install);
        QVERIFY(row->isVisible());
        QCOMPARE(status->text(), okuflow::TranslateUi(installed ? QString("Installed") : QString("Not installed")));
        QCOMPARE(install->text(), okuflow::TranslateUi(installed ? QString("Installed") : QString("Install for Windows")));
        const bool bundled = QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath("vc_redist.x64.exe"));
        QCOMPARE(install->isEnabled(), !installed && bundled);
        QVERIFY(!install->accessibleName().isEmpty());
        QTRY_VERIFY(install->width() >= install->sizeHint().width());
        if (!installed && !bundled) QVERIFY(!install->toolTip().isEmpty());
        QVERIFY(row->rect().contains(install->geometry().center()));
        if (const auto evidence = qEnvironmentVariable("OKUFLOW_UI_TEST_SCREENSHOTS"); !evidence.isEmpty()) {
            QVERIFY(QDir().mkpath(evidence));
            QVERIFY(dialog.grab().save(QDir(evidence).filePath("setup-" + language + (installed ? "-installed.png" : "-missing.png"))));
        }
        // Inspect status only: the fixture never starts a vendor installer,
        // opens a browser, changes settings or needs a foreground click.
        dialog.close();
        QCoreApplication::removeTranslator(&translator);
    }
};
QTEST_MAIN(SetupAssistantTests)
#include "setup_assistant_tests.moc"
