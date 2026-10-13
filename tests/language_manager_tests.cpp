#include "gui_test_support.hpp"
#include "okuflow/app/language_manager.hpp"
#include "okuflow/common/response_language.hpp"
#include "okuflow/ui/live_status_text.hpp"
#include "okuflow/ui/ui_translation.hpp"

#include <QAccessible>
#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QtTest>

#include <algorithm>

namespace okuflow {
namespace {

QObject* g_accessibilityTarget{};
QVector<QAccessible::Event>* g_accessibilityEvents{};

void CaptureAccessibilityEvent(QAccessibleEvent* event)
{
    if (event && g_accessibilityEvents &&
        event->object() == g_accessibilityTarget) {
        g_accessibilityEvents->push_back(event->type());
    }
}

class ShortcutTranslator final : public QTranslator {
public:
    bool isEmpty() const override { return false; }
    QString translate(const char* context,
                      const char* source,
                      const char*, int) const override
    {
        if (QByteArray(context) == "OkuFlow" &&
            QByteArray(source) == "Number key %1") {
            return QStringLiteral("Shortcut %1");
        }
        return {};
    }
};

} // namespace

class LanguageManagerTests final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void catalogsLoadAndTranslateSentinels();
    void liveStatusRetranslatesAccessibly();
    void codeChangedTextSurvivesRetranslation();
    void polishTranslatesOnlyEventWidget();
    void newlyShownDescendantsAndToolWindowsTranslate();
    void formattedSourceUsesCurrentCatalog();
    void responseLanguageDirectivePreservesPrompt();
    void rtlDirectionPlumbing();

private:
    QAccessible::UpdateHandler previousHandler_{};
    QVector<QAccessible::Event> accessibilityEvents_;
};

void LanguageManagerTests::init()
{
    accessibilityEvents_.clear();
    g_accessibilityTarget = nullptr;
    g_accessibilityEvents = &accessibilityEvents_;
    previousHandler_ =
        QAccessible::installUpdateHandler(CaptureAccessibilityEvent);
}

void LanguageManagerTests::cleanup()
{
    QAccessible::installUpdateHandler(previousHandler_);
    g_accessibilityTarget = nullptr;
    g_accessibilityEvents = nullptr;
}

void LanguageManagerTests::catalogsLoadAndTranslateSentinels()
{
    auto* application =
        qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager manager(*application);

    QVERIFY(manager.SetLanguage(AppLanguage::Turkish, false));
    QCOMPARE(manager.languageCode(), QStringLiteral("tr"));
    QCOMPARE(TranslateUi(QStringLiteral("Application language")),
             QStringLiteral("Uygulama dili"));
    QCOMPARE(TranslateUi(QStringLiteral("Recording")),
             QStringLiteral("Kayıt"));
    QCOMPARE(TranslateUi(QStringLiteral("Zoom %L1 times"))
                 .arg(2.5, 0, 'f', 1),
             QStringLiteral("2,5 kat yakınlaştırma"));

    QVERIFY(manager.SetLanguage(AppLanguage::German, false));
    QCOMPARE(manager.languageCode(), QStringLiteral("de"));
    QCOMPARE(TranslateUi(QStringLiteral("Application language")),
             QStringLiteral("Anwendungssprache"));
    QCOMPARE(TranslateUi(QStringLiteral("Recording")),
             QStringLiteral("Aufnahme"));
    QCOMPARE(TranslateUi(QStringLiteral("Zoom %L1 times"))
                 .arg(2.5, 0, 'f', 1),
             QStringLiteral("Zoom: 2,5\u00d7"));

    QVERIFY(manager.SetLanguage(AppLanguage::English, false));
    QCOMPARE(TranslateUi(QStringLiteral("Application language")),
             QStringLiteral("Application language"));
}

void LanguageManagerTests::liveStatusRetranslatesAccessibly()
{
    auto* application =
        qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager manager(*application);
    QLabel label;
    g_accessibilityTarget = &label;

    SetLiveText(&label,
                QStringLiteral("Recording"),
                LivePoliteness::kSilent,
                QStringLiteral("Pipeline status"));
    QCOMPARE(label.text(), QStringLiteral("Recording"));
    QCOMPARE(label.accessibleName(),
             QStringLiteral("Pipeline status: Recording"));

    accessibilityEvents_.clear();
    QVERIFY(manager.SetLanguage(AppLanguage::Turkish, false));
    QCOMPARE(label.text(), QStringLiteral("Kayıt"));
    QCOMPARE(label.accessibleName(),
             QStringLiteral("İşlem hattı durumu: Kayıt"));
    QVERIFY(std::find(accessibilityEvents_.cbegin(),
                      accessibilityEvents_.cend(),
                      QAccessible::NameChanged) !=
            accessibilityEvents_.cend());
    QVERIFY(std::find(accessibilityEvents_.cbegin(),
                      accessibilityEvents_.cend(),
                      QAccessible::TextUpdated) !=
            accessibilityEvents_.cend());

    QVERIFY(manager.SetLanguage(AppLanguage::English, false));
}

void LanguageManagerTests::codeChangedTextSurvivesRetranslation()
{
    auto* application =
        qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager manager(*application);

    // Polish/Show retranslation records the construction-time label, then
    // status code replaces it (Setup Assistant: Install -> Update). The next
    // pass must keep the replacement rather than restoring the first label.
    QPushButton button(QStringLiteral("Install"));
    RetranslateWidgetTree(&button);
    SetLiveAccessibleDescription(&button, QStringLiteral("Recording"));
    button.setText(QStringLiteral("Recording"));
    button.setToolTip(QStringLiteral("Recording"));
    RetranslateWidgetTree(&button);
    QCOMPARE(button.text(), QStringLiteral("Recording"));
    QCOMPARE(button.toolTip(), QStringLiteral("Recording"));

    QVERIFY(manager.SetLanguage(AppLanguage::Turkish, false));
    QCOMPARE(button.text(), QStringLiteral("Kayıt"));
    QCOMPARE(button.toolTip(), QStringLiteral("Kayıt"));
    QCOMPARE(button.accessibleDescription(), QStringLiteral("Kayıt"));

    // Returning to Custom Setup removes the previous shortcut description;
    // retranslation must not resurrect it on the next language change.
    SetLiveAccessibleDescription(&button, QString());

    QVERIFY(manager.SetLanguage(AppLanguage::English, false));
    QCOMPARE(button.text(), QStringLiteral("Recording"));
    QCOMPARE(button.toolTip(), QStringLiteral("Recording"));
    QVERIFY(button.accessibleDescription().isEmpty());
}

void LanguageManagerTests::polishTranslatesOnlyEventWidget()
{
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager manager(*application);
    QVERIFY(manager.SetLanguage(AppLanguage::Turkish, false));

    QLabel parent(QStringLiteral("Recording"));
    QLabel child(QStringLiteral("Recording"), &parent);
    QEvent polish(QEvent::Polish);
    QCoreApplication::sendEvent(&parent, &polish);
    QCOMPARE(parent.text(), QStringLiteral("Kayıt"));
    QCOMPARE(child.text(), QStringLiteral("Recording"));
    QVERIFY(!child.property("_okuflowTranslationSourceText").isValid());

    QShowEvent show;
    QCoreApplication::sendEvent(&parent, &show);
    QCOMPARE(child.text(), QStringLiteral("Recording"));
    QVERIFY(!child.property("_okuflowTranslationSourceText").isValid());

    child.ensurePolished();
    QCOMPARE(child.text(), QStringLiteral("Kayıt"));
    QVERIFY(manager.SetLanguage(AppLanguage::German, false));
    QCOMPARE(parent.text(), QStringLiteral("Aufnahme"));
    QCOMPARE(child.text(), QStringLiteral("Aufnahme"));
    QVERIFY(manager.SetLanguage(AppLanguage::English, false));
    QCOMPARE(parent.text(), QStringLiteral("Recording"));
    QCOMPARE(child.text(), QStringLiteral("Recording"));
}

void LanguageManagerTests::newlyShownDescendantsAndToolWindowsTranslate()
{
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager manager(*application);
    QVERIFY(manager.SetLanguage(AppLanguage::Turkish, false));

    QWidget window;
    window.setWindowTitle(QStringLiteral("Application language"));
    auto* windowLayout = new QVBoxLayout(&window);
    auto* nested = new QWidget(&window);
    auto* nestedLayout = new QVBoxLayout(nested);
    auto* button = new QPushButton(QStringLiteral("Recording"), nested);
    button->setToolTip(QStringLiteral("Recording"));
    button->setAccessibleName(QStringLiteral("Recording"));
    SetLiveAccessibleDescription(button, QStringLiteral("Number key 3"));
    nestedLayout->addWidget(button);
    windowLayout->addWidget(nested);

    // OkuFlow's floating chrome is an owned native top-level tool window;
    // it receives its own Polish/Show events rather than its owner's Show.
    QWidget tool(&window, Qt::Tool);
    tool.setAttribute(Qt::WA_NativeWindow);
    tool.setWindowTitle(QStringLiteral("Application language"));
    auto* toolLayout = new QVBoxLayout(&tool);
    auto* toolLabel = new QLabel(QStringLiteral("Recording"), &tool);
    toolLayout->addWidget(toolLabel);
    window.show();
    tool.show();
    QTRY_COMPARE(button->text(), QStringLiteral("Kayıt"));
    QTRY_COMPARE(toolLabel->text(), QStringLiteral("Kayıt"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Uygulama dili"));
    QCOMPARE(tool.windowTitle(), QStringLiteral("Uygulama dili"));
    QCOMPARE(button->accessibleDescription(), QStringLiteral("Sayı tuşu 3"));

    // Also cover a child constructed after its ancestors are already shown.
    auto* lateLabel = new QLabel(QStringLiteral("Recording"), nested);
    nestedLayout->addWidget(lateLabel);
    lateLabel->show();
    QTRY_COMPARE(lateLabel->text(), QStringLiteral("Kayıt"));

    QVERIFY(manager.SetLanguage(AppLanguage::German, false));
    QCOMPARE(button->text(), QStringLiteral("Aufnahme"));
    QCOMPARE(button->toolTip(), QStringLiteral("Aufnahme"));
    QCOMPARE(button->accessibleName(), QStringLiteral("Aufnahme"));
    QCOMPARE(button->accessibleDescription(), QStringLiteral("Zifferntaste 3"));
    QCOMPARE(toolLabel->text(), QStringLiteral("Aufnahme"));
    QCOMPARE(lateLabel->text(), QStringLiteral("Aufnahme"));

    QVERIFY(manager.SetLanguage(AppLanguage::English, false));
    QCOMPARE(button->text(), QStringLiteral("Recording"));
    QCOMPARE(button->toolTip(), QStringLiteral("Recording"));
    QCOMPARE(button->accessibleName(), QStringLiteral("Recording"));
    QCOMPARE(button->accessibleDescription(), QStringLiteral("Number key 3"));
    QCOMPARE(toolLabel->text(), QStringLiteral("Recording"));
    QCOMPARE(lateLabel->text(), QStringLiteral("Recording"));
}

void LanguageManagerTests::formattedSourceUsesCurrentCatalog()
{
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    LanguageManager manager(*application);
    const QString source = QStringLiteral("Number key 3");
    QCOMPARE(TranslateUi(source), source);
    QVERIFY(manager.SetLanguage(AppLanguage::Turkish, false));
    QCOMPARE(TranslateUi(source), QStringLiteral("Sayı tuşu 3"));
    QVERIFY(manager.SetLanguage(AppLanguage::German, false));
    QCOMPARE(TranslateUi(source), QStringLiteral("Zifferntaste 3"));
    QVERIFY(manager.SetLanguage(AppLanguage::English, false));
    QCOMPARE(TranslateUi(source), source);

    // The fast path follows actual translator contents, including a partial
    // independent translator installed while the application locale is EN.
    ShortcutTranslator translator;
    QVERIFY(application->installTranslator(&translator));
    QCOMPARE(TranslateUi(source), QStringLiteral("Shortcut 3"));
    application->removeTranslator(&translator);
    QCOMPARE(TranslateUi(source), source);
}

void LanguageManagerTests::responseLanguageDirectivePreservesPrompt()
{
    const QString prompt = QStringLiteral("Describe this view.");
    QCOMPARE(AppendResponseLanguageDirective(prompt, QStringLiteral("en")),
             prompt);
    QCOMPARE(AppendResponseLanguageDirective(prompt, QStringLiteral("tr")),
             QStringLiteral("Describe this view.\n\nRespond in Turkish."));
    QCOMPARE(AppendResponseLanguageDirective(prompt, QStringLiteral("de")),
             QStringLiteral("Describe this view.\n\nRespond in German."));
    QCOMPARE(AppendResponseLanguageDirective(QString(), QStringLiteral("tr")),
             QStringLiteral("Respond in Turkish."));
}

void LanguageManagerTests::rtlDirectionPlumbing()
{
    QCOMPARE(AppLanguageLayoutDirection(AppLanguage::English),
             Qt::LeftToRight);
    QCOMPARE(AppLanguageLayoutDirection(AppLanguage::Turkish),
             Qt::LeftToRight);
    QCOMPARE(AppLanguageLayoutDirection(AppLanguage::German),
             Qt::LeftToRight);
    QCOMPARE(QLocale(QLocale::Arabic).textDirection(),
             Qt::RightToLeft);
    QCOMPARE(QLocale(QLocale::Hebrew).textDirection(),
             Qt::RightToLeft);
    QCOMPARE(QLocale(QLocale::Persian).textDirection(),
             Qt::RightToLeft);
    QCOMPARE(QLocale(QLocale::Urdu).textDirection(),
             Qt::RightToLeft);

    auto* application =
        qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(application);
    qputenv("OKUFLOW_FORCE_RTL", "1");
    {
        LanguageManager manager(*application);
        QVERIFY(manager.rtlTestMode());
        QVERIFY(manager.SetLanguage(AppLanguage::English, false));
        QCOMPARE(application->layoutDirection(), Qt::RightToLeft);

        QWidget row;
        auto* layout = new QHBoxLayout(&row);
        auto* first = new QPushButton(QStringLiteral("First"));
        auto* second = new QPushButton(QStringLiteral("Second"));
        layout->addWidget(first);
        layout->addWidget(second);
        row.resize(320, 80);
        row.show();
        QCoreApplication::processEvents();
        QVERIFY(first->x() > second->x());
    }
    qunsetenv("OKUFLOW_FORCE_RTL");
    application->setLayoutDirection(Qt::LeftToRight);
}

} // namespace okuflow

OKUFLOW_GUI_TEST_MAIN(okuflow::LanguageManagerTests)
#include "language_manager_tests.moc"
