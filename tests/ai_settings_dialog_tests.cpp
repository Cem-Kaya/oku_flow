#include "okuflow/ui/ai_settings_dialog.hpp"
#include "okuflow/common/codex_app_server_client.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGroupBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>

namespace okuflow {

// The dialog only displays this unrelated static text. Keep the service at
// that seam instead of linking account, process, or network functionality.
QString CodexAppServerClient::BuiltInAssistantInstructions()
{
    return QStringLiteral("Read-only built-in instruction fixture.");
}

namespace {

template <typename T>
T* Accessible(AiSettingsDialog& dialog, const char* name)
{
    for (T* child : dialog.findChildren<T*>()) {
        if (child->accessibleName() == QString::fromLatin1(name)) {
            return child;
        }
    }
    return nullptr;
}

bool ShowDialog(AiSettingsDialog& dialog)
{
    dialog.show();
    if (!QTest::qWaitForWindowExposed(&dialog)) {
        return false;
    }
    dialog.activateWindow();
    return QTest::qWaitForWindowActive(&dialog);
}

struct SaveAttempt {
    int warnings{0};
    QMessageBox::Icon icon{QMessageBox::NoIcon};
    QString text;
    bool timedOut{false};
};

SaveAttempt ClickSave(AiSettingsDialog& dialog, QPushButton& save)
{
    SaveAttempt attempt;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer dismissWarning;
    QObject::connect(&dismissWarning, &QTimer::timeout, &dialog, [&]() {
        // Validation uses a synchronous QMessageBox. Close only warnings owned
        // by this fixture, including unexpected warnings in successful cases.
        for (QMessageBox* warning : dialog.findChildren<QMessageBox*>()) {
            if (warning->isVisible()) {
                ++attempt.warnings;
                attempt.icon = warning->icon();
                attempt.text = warning->text();
                warning->done(QMessageBox::Ok);
            }
        }
        if (elapsed.elapsed() > 2000) {
            attempt.timedOut = true;
            dialog.reject();
        }
    });
    dismissWarning.start(10);
    save.click();
    dismissWarning.stop();
    QCoreApplication::processEvents();
    return attempt;
}

} // namespace

class AiSettingsDialogTests final : public QObject {
    Q_OBJECT
private slots:
    void clearedApiKeyRetainsProtectedCredentialId();
    void codexExplainPromptIsEditableAndReturned();
    void serverProviderPreservesHiddenCodexPreferences();
    void codingWorkspaceRejectsInvalidPathThenAcceptsCorrection_data();
    void codingWorkspaceRejectsInvalidPathThenAcceptsCorrection();
    void textEditorsTraverseWithTab_data();
    void textEditorsTraverseWithTab();
    void narrowGroupsKeepFieldsReachable_data();
    void narrowGroupsKeepFieldsReachable();
};

void AiSettingsDialogTests::clearedApiKeyRetainsProtectedCredentialId()
{
    settings::AssistiveSettings initial;
    initial.aiProvider = QStringLiteral("openai-compatible");
    initial.vlmCredentialId = QStringLiteral("fixture-credential-id");
    initial.vlmApiKey = QStringLiteral("fixture-placeholder-not-a-secret");
    AiSettingsDialog dialog(initial);
    auto* key = Accessible<QLineEdit>(dialog, "API Key");
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(key);
    QVERIFY(buttons);
    auto* save = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(save);
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    QVERIFY(ShowDialog(dialog));
    key->setFocus();
    QTRY_VERIFY(key->hasFocus());
    QTest::keyClick(key, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(key, Qt::Key_Backspace);
    QVERIFY(key->text().isEmpty());
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    const SaveAttempt attempt = ClickSave(dialog, *save);
    QVERIFY(!attempt.timedOut);
    QCOMPARE(attempt.warnings, 0);
    QCOMPARE(accepted.count(), 1);
    const settings::AssistiveSettings result = dialog.result();
    QVERIFY(result.vlmApiKey.isEmpty());
    QCOMPARE(result.vlmCredentialId, initial.vlmCredentialId);
}

void AiSettingsDialogTests::codexExplainPromptIsEditableAndReturned()
{
    settings::AssistiveSettings initial;
    initial.vlmPrompt = QStringLiteral("Original scene preference");
    AiSettingsDialog dialog(initial);
    auto* prompt = Accessible<QPlainTextEdit>(dialog, "Explain Prompt");
    auto* scroll = dialog.findChild<QScrollArea*>();
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(prompt);
    QVERIFY(scroll);
    QVERIFY(buttons);
    auto* save = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(save);
    QVERIFY(ShowDialog(dialog));
    QVERIFY(prompt->isVisible());
    QVERIFY(prompt->isEnabled());
    QVERIFY(!prompt->isReadOnly());
    scroll->ensureWidgetVisible(prompt);
    prompt->setFocus();
    QTRY_VERIFY(prompt->hasFocus());
    QTest::keyClick(prompt, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(prompt, "  Describe the slide briefly.");
    QTest::keyClick(prompt, Qt::Key_Return);
    QTest::keyClicks(prompt, "Preserve technical terms.  ");
    const QString edited = QStringLiteral(
        "  Describe the slide briefly.\nPreserve technical terms.  ");
    QCOMPARE(prompt->toPlainText(), edited);
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    const SaveAttempt attempt = ClickSave(dialog, *save);
    QVERIFY(!attempt.timedOut);
    QCOMPARE(attempt.warnings, 0);
    QCOMPARE(accepted.count(), 1);
    QCOMPARE(dialog.result().vlmPrompt, edited);
}

void AiSettingsDialogTests::serverProviderPreservesHiddenCodexPreferences()
{
    settings::AssistiveSettings initial;
    initial.codexCodingEnabled = true;
    initial.codexInternetEnabled = true;
    initial.codexWorkspaceDirectory = QStringLiteral("invalid-relative-workspace");
    initial.codexExecutablePath = QStringLiteral("C:\\fixture\\codex.exe");
    initial.codexModel = QStringLiteral("saved-model-fixture");
    initial.codexReasoningEffort = QStringLiteral("high");
    AiSettingsDialog dialog(initial);
    auto* provider = Accessible<QComboBox>(dialog, "AI Provider");
    auto* coding = Accessible<QCheckBox>(dialog, "Allow Assistant Coding");
    auto* workspace = Accessible<QLineEdit>(dialog, "Assistant Coding Workspace");
    auto* server = Accessible<QLineEdit>(dialog, "VLM Server URL");
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(provider);
    QVERIFY(coding);
    QVERIFY(workspace);
    QVERIFY(server);
    QVERIFY(buttons);
    auto* save = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(save);
    QVERIFY(ShowDialog(dialog));
    const int serverIndex = provider->findData(QStringLiteral("openai-compatible"));
    QVERIFY(serverIndex >= 0);
    provider->setCurrentIndex(serverIndex);
    QVERIFY(!coding->isVisible());
    QVERIFY(!workspace->isVisible());
    QVERIFY(server->isVisible());
    server->setText(QStringLiteral("http://localhost:11434/v1/chat/completions"));
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    const SaveAttempt attempt = ClickSave(dialog, *save);
    QVERIFY(!attempt.timedOut);
    QCOMPARE(attempt.warnings, 0);
    QCOMPARE(accepted.count(), 1);
    const settings::AssistiveSettings result = dialog.result();
    QCOMPARE(result.aiProvider, QStringLiteral("openai-compatible"));
    QCOMPARE(result.vlmApiUrl, server->text());
    QCOMPARE(result.codexCodingEnabled, initial.codexCodingEnabled);
    QCOMPARE(result.codexInternetEnabled, initial.codexInternetEnabled);
    QCOMPARE(result.codexWorkspaceDirectory, initial.codexWorkspaceDirectory);
    QCOMPARE(result.codexExecutablePath, initial.codexExecutablePath);
    QCOMPARE(result.codexModel, initial.codexModel);
    QCOMPARE(result.codexReasoningEffort, initial.codexReasoningEffort);
}

void AiSettingsDialogTests::codingWorkspaceRejectsInvalidPathThenAcceptsCorrection_data()
{
    QTest::addColumn<bool>("relative");
    QTest::newRow("existing-relative-folder") << true;
    QTest::newRow("nonexistent-absolute-folder") << false;
}

void AiSettingsDialogTests::codingWorkspaceRejectsInvalidPathThenAcceptsCorrection()
{
    QFETCH(bool, relative);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    settings::AssistiveSettings initial;
    initial.codexCodingEnabled = true;
    initial.codexWorkspaceDirectory = relative
        ? QStringLiteral(".")
        : QDir(temporary.path()).filePath(QStringLiteral("missing-workspace"));
    if (relative) {
        QVERIFY(QFileInfo(initial.codexWorkspaceDirectory).isDir());
        QVERIFY(!QFileInfo(initial.codexWorkspaceDirectory).isAbsolute());
    } else {
        QVERIFY(QFileInfo(initial.codexWorkspaceDirectory).isAbsolute());
        QVERIFY(!QFileInfo::exists(initial.codexWorkspaceDirectory));
    }
    AiSettingsDialog dialog(initial);
    auto* workspace = Accessible<QLineEdit>(dialog, "Assistant Coding Workspace");
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(workspace);
    QVERIFY(buttons);
    auto* save = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(save);
    QVERIFY(ShowDialog(dialog));
    QSignalSpy accepted(&dialog, &QDialog::accepted);
    const SaveAttempt rejected = ClickSave(dialog, *save);
    QVERIFY(!rejected.timedOut);
    QCOMPARE(rejected.warnings, 1);
    QCOMPARE(rejected.icon, QMessageBox::Warning);
    QVERIFY(rejected.text.contains(QStringLiteral("workspace"), Qt::CaseInsensitive));
    QCOMPARE(accepted.count(), 0);
    QVERIFY(dialog.isVisible());
    QCOMPARE(workspace->text(), initial.codexWorkspaceDirectory);
    QTRY_VERIFY(workspace->hasFocus());
    workspace->setText(temporary.path());
    const SaveAttempt corrected = ClickSave(dialog, *save);
    QVERIFY(!corrected.timedOut);
    QCOMPARE(corrected.warnings, 0);
    QCOMPARE(accepted.count(), 1);
    QVERIFY(dialog.result().codexCodingEnabled);
    QCOMPARE(QDir::fromNativeSeparators(dialog.result().codexWorkspaceDirectory),
             temporary.path());
}

void AiSettingsDialogTests::textEditorsTraverseWithTab_data()
{
    QTest::addColumn<QString>("provider");
    QTest::newRow("codex") << QStringLiteral("codex");
    QTest::newRow("server") << QStringLiteral("openai-compatible");
}

void AiSettingsDialogTests::textEditorsTraverseWithTab()
{
    QFETCH(QString, provider);
    settings::AssistiveSettings initial;
    initial.aiProvider = provider;
    initial.assistantInstructions = QStringLiteral("Answer concisely.");
    initial.vlmPrompt = QStringLiteral("Describe the diagram.");
    AiSettingsDialog dialog(initial);
    auto* instructions = Accessible<QPlainTextEdit>(dialog, "Assistant Instructions");
    auto* prompt = Accessible<QPlainTextEdit>(dialog, "Explain Prompt");
    auto* scroll = dialog.findChild<QScrollArea*>();
    QVERIFY(instructions);
    QVERIFY(prompt);
    QVERIFY(scroll);
    QVERIFY(ShowDialog(dialog));
    if (provider == QStringLiteral("codex")) {
        auto* disclosure = Accessible<QToolButton>(dialog, "Built-in Codex Prompt");
        auto* reference = Accessible<QPlainTextEdit>(dialog, "Built-in Codex Prompt");
        QVERIFY(disclosure);
        QVERIFY(reference);
        QVERIFY(!reference->isVisible());
        disclosure->click();
        QVERIFY(reference->isVisible());
        QVERIFY(reference->isReadOnly());
        const QString original = reference->toPlainText();
        scroll->ensureWidgetVisible(reference);
        reference->setFocus();
        QTRY_VERIFY(reference->hasFocus());
        QTest::keyClick(reference, Qt::Key_Tab);
        QTRY_VERIFY(instructions->hasFocus());
        QCOMPARE(reference->toPlainText(), original);
    }
    scroll->ensureWidgetVisible(instructions);
    instructions->setFocus();
    QTRY_VERIFY(instructions->hasFocus());
    QTest::keyClick(instructions, Qt::Key_Tab);
    QTRY_VERIFY(prompt->hasFocus());
    QCOMPARE(instructions->toPlainText(), initial.assistantInstructions);
    QTest::keyClick(prompt, Qt::Key_Tab);
    QTRY_VERIFY(!prompt->hasFocus());
    QCOMPARE(prompt->toPlainText(), initial.vlmPrompt);
    prompt->setFocus();
    QTRY_VERIFY(prompt->hasFocus());
    QTest::keyClick(prompt, Qt::Key_Backtab);
    QTRY_VERIFY(instructions->hasFocus());
}

void AiSettingsDialogTests::narrowGroupsKeepFieldsReachable_data()
{
    textEditorsTraverseWithTab_data();
}

void AiSettingsDialogTests::narrowGroupsKeepFieldsReachable()
{
    QFETCH(QString, provider);
    settings::AssistiveSettings initial;
    initial.aiProvider = provider;
    AiSettingsDialog dialog(initial);
    dialog.resize(480, 420);
    auto* scroll = dialog.findChild<QScrollArea*>();
    QVERIFY(scroll);
    QVERIFY(ShowDialog(dialog));
    QCOMPARE(dialog.width(), 480);
    QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
    QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    const QRect contentBounds = scroll->widget()->rect();
    for (QGroupBox* group : dialog.findChildren<QGroupBox*>()) {
        if (group->isVisible()) {
            QVERIFY2(contentBounds.contains(group->geometry()),
                     qPrintable(group->title()));
        }
    }
    const QStringList fields{
        QStringLiteral("AI Provider"),
        provider == QStringLiteral("codex") ? QStringLiteral("Codex Model")
                                            : QStringLiteral("VLM Server URL"),
        QStringLiteral("Assistant Instructions"),
        QStringLiteral("Explain Prompt")};
    for (const QString& name : fields) {
        QWidget* field = Accessible<QWidget>(dialog, name.toLatin1().constData());
        QVERIFY2(field, qPrintable(name));
        QVERIFY2(field->width() >= 200, qPrintable(name));
        scroll->ensureWidgetVisible(field, 0, 0);
        QCoreApplication::processEvents();
        const QRect caretScrolledBounds(
            field->mapTo(scroll->viewport(), QPoint()), field->size());
        const QRect contentFieldBounds(
            field->mapTo(scroll->widget(), QPoint()), field->size());
        const QRect viewportBounds = scroll->viewport()->rect();
        QString geometry;
        QDebug(&geometry).nospace()
            << name << " caretScrolledField=" << caretScrolledBounds
            << " viewport=" << viewportBounds << " fieldSize=" << field->size()
            << " contentSize=" << scroll->widget()->size()
            << " contentPosition=" << scroll->widget()->pos()
            << " fieldInContent=" << contentFieldBounds
            << " caret=" << field->inputMethodQuery(Qt::ImCursorRectangle).toRect()
            << " scroll=" << scroll->verticalScrollBar()->value()
            << "/" << scroll->verticalScrollBar()->maximum();
        if (!viewportBounds.contains(caretScrolledBounds)) {
            qInfo().noquote() << "Caret scrolling does not expose the whole field:" << geometry;
        }
        QVERIFY2(field->width() <= viewportBounds.width(), qPrintable(geometry));
        QVERIFY2(field->height() <= viewportBounds.height(), qPrintable(geometry));
        // Qt's ensureWidgetVisible uses the input-method caret rectangle for
        // text controls. Test whole-field reachability through the real scroll
        // bar instead, without changing the production widget's geometry.
        scroll->verticalScrollBar()->setValue(
            contentFieldBounds.center().y() - viewportBounds.height() / 2);
        QCoreApplication::processEvents();
        const QRect fieldBounds(field->mapTo(scroll->viewport(), QPoint()), field->size());
        QString reachedGeometry;
        QDebug(&reachedGeometry).nospace()
            << geometry << " reachedField=" << fieldBounds
            << " reachedViewport=" << scroll->viewport()->rect()
            << " reachedContentPosition=" << scroll->widget()->pos()
            << " reachedScroll=" << scroll->verticalScrollBar()->value();
        QVERIFY2(scroll->viewport()->rect().contains(fieldBounds),
                 qPrintable(reachedGeometry));
    }
}

} // namespace okuflow

QTEST_MAIN(okuflow::AiSettingsDialogTests)
#include "ai_settings_dialog_tests.moc"
