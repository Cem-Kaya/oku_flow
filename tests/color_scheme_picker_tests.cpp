#include "okuflow/ui/color_scheme_picker.hpp"
#include "okuflow/ui/ui_translation.hpp"
#include "okuflow/app/language_manager.hpp"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QTranslator>

namespace okuflow {
namespace {
QPushButton* Quick(ColorSchemePicker& picker, const char* id)
{
    return picker.findChild<QPushButton*>(QStringLiteral("quickColor_%1").arg(QString::fromLatin1(id)));
}
class PickerTranslator final : public QTranslator {
public:
    bool isEmpty() const override { return false; }
    QString translate(const char* context, const char* source, const char*, int) const override
    {
        if (QByteArray(context) != "OkuFlow") return {};
        if (QByteArray(source) == "More colors\n%1") return QStringLiteral("Weitere Farben\n%1");
        if (QByteArray(source) == "Normal colors") return QStringLiteral("Normale Farben");
        return {};
    }
};
}

class ColorSchemePickerTests final : public QObject {
    Q_OBJECT
private slots:
    void favoriteSelectionEmitsOnce();
    void programmaticSelectionRefreshesChecks();
    void quickChoicesReflow();
    void moreColorsRetainsFullCatalog();
    void moreColorsRetranslates();
    void realLanguageRoundtripRetainsTranslatedNames();
};

void ColorSchemePickerTests::favoriteSelectionEmitsOnce()
{
    ColorSchemePicker picker;
    QSignalSpy changes(&picker, &ColorSchemePicker::schemeChanged);
    auto* posterize = Quick(picker, "posterize-6");
    QVERIFY(posterize);
    QVERIFY(Quick(picker, "normal")->isChecked());
    posterize->click();
    QCOMPARE(picker.currentScheme().id, QStringLiteral("posterize-6"));
    QCOMPARE(changes.count(), 1);
    QVERIFY(posterize->isChecked());
    QVERIFY(!Quick(picker, "normal")->isChecked());
    posterize->click();
    QCOMPARE(changes.count(), 1);
    QVERIFY(posterize->isChecked());
    Quick(picker, "yellow-black")->click();
    QCOMPARE(picker.currentScheme().id, QStringLiteral("yellow-black"));
    QCOMPARE(changes.count(), 2);
    QVERIFY(!posterize->isChecked());
    QVERIFY(Quick(picker, "yellow-black")->isChecked());
}

void ColorSchemePickerTests::programmaticSelectionRefreshesChecks()
{
    ColorSchemePicker picker;
    QSignalSpy changes(&picker, &ColorSchemePicker::schemeChanged);
    picker.setCurrentScheme(*color_schemes::FindBuiltInColorScheme(QStringLiteral("black-yellow")));
    QVERIFY(Quick(picker, "black-yellow")->isChecked());
    QVERIFY(!Quick(picker, "normal")->isChecked());
    auto* more = picker.findChild<QPushButton*>(QStringLiteral("moreColorsButton"));
    QVERIFY(more);
    QVERIFY(more->text().startsWith(QStringLiteral("More colors\n")));
    QVERIFY(more->text().contains(picker.currentScheme().name));
    picker.setCurrentScheme(*color_schemes::FindBuiltInColorScheme(QStringLiteral("inverted")));
    for (const char* id : {"normal", "posterize-6", "yellow-black", "black-yellow"})
        QVERIFY(!Quick(picker, id)->isChecked());
    QCOMPARE(changes.count(), 0);
}

void ColorSchemePickerTests::quickChoicesReflow()
{
    ColorSchemePicker picker;
    picker.resize(360, 240);
    picker.show();
    auto* first = Quick(picker, "posterize-6");
    auto* second = Quick(picker, "yellow-black");
    auto* third = Quick(picker, "normal");
    QTRY_COMPARE(first->y(), second->y());
    QTRY_VERIFY(third->y() > first->y());
    picker.resize(760, 240);
    QTRY_COMPARE(third->y(), first->y());
    QVERIFY(third->x() > second->x());
    picker.resize(360, 240);
    QTRY_VERIFY(third->y() > first->y());
    QVERIFY(first->geometry().right() < picker.width());
}

void ColorSchemePickerTests::moreColorsRetainsFullCatalog()
{
    ColorSchemePicker picker;
    picker.resize(360, 240);
    picker.show();
    QSignalSpy changes(&picker, &ColorSchemePicker::schemeChanged);
    picker.findChild<QPushButton*>(QStringLiteral("moreColorsButton"))->click();
    QWidget* popup = nullptr;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("colorSchemePopup")) popup = widget;
    }
    QVERIFY(popup);
    QTRY_VERIFY(popup->isVisible());
    QSet<QString> ids;
    for (auto* button : popup->findChildren<QAbstractButton*>()) {
        const QString id = button->property("schemeId").toString();
        if (id.isEmpty()) continue;
        QVERIFY2(!ids.contains(id), qPrintable(id));
        ids.insert(id);
    }
    QCOMPARE(ids.size(), static_cast<qsizetype>(color_schemes::BuiltInColorSchemes().size()));
    for (const auto& scheme : color_schemes::BuiltInColorSchemes()) QVERIFY(ids.contains(scheme.id));
    QVERIFY(popup->findChild<QComboBox*>());
    QCOMPARE(picker.currentScheme().id, QStringLiteral("normal"));
    QCOMPARE(changes.count(), 0);
}

void ColorSchemePickerTests::moreColorsRetranslates()
{
    ColorSchemePicker picker;
    PickerTranslator translator;
    QVERIFY(qApp->installTranslator(&translator));
    RetranslateWidgetTree(&picker);
    QEvent languageChange(QEvent::LanguageChange);
    QApplication::sendEvent(&picker, &languageChange);
    auto* more = picker.findChild<QPushButton*>(QStringLiteral("moreColorsButton"));
    QTRY_COMPARE(more->text(), QStringLiteral("Weitere Farben\nNormale Farben"));
    qApp->removeTranslator(&translator);
    RetranslateWidgetTree(&picker);
    QApplication::sendEvent(&picker, &languageChange);
    QTRY_COMPARE(more->text(), QStringLiteral("More colors\nNormal colors"));
}

void ColorSchemePickerTests::realLanguageRoundtripRetainsTranslatedNames()
{
    LanguageManager languages(*qApp);
    ColorSchemePicker picker;
    QSignalSpy changes(&picker, &ColorSchemePicker::schemeChanged);
    auto* more = picker.findChild<QPushButton*>(QStringLiteral("moreColorsButton"));
    QVERIFY(languages.SetLanguage(AppLanguage::German));
    QTRY_COMPARE(Quick(picker, "yellow-black")->text(), QStringLiteral("Gelb auf Schwarz"));
    QTRY_COMPARE(Quick(picker, "black-yellow")->text(), QStringLiteral("Schwarz auf Gelb"));
    QTRY_COMPARE(Quick(picker, "posterize-6")->text(), QStringLiteral("6 Farben posterisieren"));
    QTRY_COMPARE(more->text(), QStringLiteral("Weitere Farben\nNormale Farben"));
    // Reproduce the later generic Show/Polish retranslation that used to
    // overwrite the explicitly translated current-selection subtitle.
    RetranslateWidgetTree(&picker);
    QCOMPARE(more->text(), QStringLiteral("Weitere Farben\nNormale Farben"));
    picker.setCurrentScheme(*color_schemes::FindBuiltInColorScheme(QStringLiteral("yellow-black")));
    RetranslateWidgetTree(&picker);
    QCOMPARE(more->text(), QStringLiteral("Weitere Farben\nGelb auf Schwarz"));
    QVERIFY(languages.SetLanguage(AppLanguage::Turkish));
    QTRY_COMPARE(Quick(picker, "yellow-black")->text(), QStringLiteral("Siyah üzerine sarı"));
    QTRY_COMPARE(Quick(picker, "black-yellow")->text(), QStringLiteral("Sarı üzerine siyah"));
    QTRY_COMPARE(Quick(picker, "posterize-6")->text(), QStringLiteral("6 renk posterleştirme"));
    QTRY_COMPARE(more->text(), QStringLiteral("Diğer renkler\nSiyah üzerine sarı"));
    auto custom = color_schemes::LegacyColorScheme(2);
    custom.id = QStringLiteral("custom");
    custom.name = QStringLiteral("Yellow on black");
    picker.setCurrentScheme(custom);
    RetranslateWidgetTree(&picker);
    QCOMPARE(more->text(), QStringLiteral("Diğer renkler\nYellow on black"));
    QVERIFY(languages.SetLanguage(AppLanguage::English));
    QTRY_COMPARE(Quick(picker, "yellow-black")->text(), QStringLiteral("Yellow on black"));
    QTRY_COMPARE(Quick(picker, "posterize-6")->text(), QStringLiteral("Posterize 6"));
    QTRY_COMPARE(more->text(), QStringLiteral("More colors\nYellow on black"));
    QCOMPARE(changes.count(), 0);
}
}

QTEST_MAIN(okuflow::ColorSchemePickerTests)
#include "color_scheme_picker_tests.moc"
