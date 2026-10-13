#include "gui_test_support.hpp"
#include "okuflow/ui/color_scheme_picker.hpp"
#include "okuflow/ui/theme_contrast.hpp"
#include "okuflow/ui/ui_translation.hpp"
#include "okuflow/app/language_manager.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QMap>
#include <QPalette>
#include <QPushButton>
#include <QScopeGuard>
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
    void popupContrastAppearance();
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
    picker.setAttribute(Qt::WA_DontShowOnScreen);
    picker.setStyleSheet(QStringLiteral("QWidget { font-size: 12pt; }"));
    picker.resize(320, 240);
    picker.show();
    const auto saveChoices = [&picker](const QString& code) {
        const QString root = qEnvironmentVariable("OKUFLOW_UI_TEST_SCREENSHOTS");
        if (root.isEmpty()) return;
        QDir directory(root);
        if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) return;
        QCoreApplication::processEvents();
        if (!picker.grab().save(directory.filePath(code + QStringLiteral("-color-shortcuts.png")))) {
            qWarning().noquote() << "Cannot save color shortcut screenshot:" << code;
        }
    };
    QSignalSpy changes(&picker, &ColorSchemePicker::schemeChanged);
    auto* more = picker.findChild<QPushButton*>(QStringLiteral("moreColorsButton"));
    QVERIFY(languages.SetLanguage(AppLanguage::German));
    QTRY_COMPARE(Quick(picker, "yellow-black")->text(), QStringLiteral("Gelb auf Schwarz"));
    QTRY_COMPARE(Quick(picker, "black-yellow")->text(), QStringLiteral("Schwarz auf Gelb"));
    QTRY_COMPARE(Quick(picker, "posterize-6")->text(), QStringLiteral("Auf 6 Farben vereinfachen"));
    QTRY_COMPARE(more->text(), QStringLiteral("Weitere Farben\nNormale Farben"));
    saveChoices(QStringLiteral("de"));
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
    saveChoices(QStringLiteral("tr"));
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
    saveChoices(QStringLiteral("en"));
    QCOMPARE(changes.count(), 0);
}

void ColorSchemePickerTests::popupContrastAppearance()
{
    // Light OS palette: black text roles on near-white surfaces. The popover
    // is pinned dark, so every text role must stay light on its surfaces.
    QPalette light = qApp->palette();
    const QPalette original = light;
    const auto restore = qScopeGuard([&original]() { qApp->setPalette(original); });
    light.setColor(QPalette::Window, QColor(QStringLiteral("#f3f3f3")));
    light.setColor(QPalette::WindowText, QColor(QStringLiteral("#000000")));
    light.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
    light.setColor(QPalette::Text, QColor(QStringLiteral("#000000")));
    light.setColor(QPalette::Button, QColor(QStringLiteral("#fbfbfb")));
    light.setColor(QPalette::ButtonText, QColor(QStringLiteral("#000000")));
    light.setColor(QPalette::Highlight, QColor(QStringLiteral("#005da6")));
    light.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    qApp->setPalette(light);

    ColorSchemePicker picker;
    picker.resize(360, 240);
    picker.show();
    picker.findChild<QPushButton*>(QStringLiteral("moreColorsButton"))->click();
    QWidget* popup = nullptr;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("colorSchemePopup")) popup = widget;
    }
    QVERIFY(popup);
    QTRY_VERIFY(popup->isVisible());
    const QPalette popupPalette = popup->palette();
    const auto readableOn = [](const QColor& text, const QColor& surface) {
        return theme_contrast::ContrastRatio(text, surface) >= 4.5;
    };
    QVERIFY2(readableOn(popupPalette.color(QPalette::Active, QPalette::ButtonText),
                        popupPalette.color(QPalette::Active, QPalette::Button)),
             "Popup button text is unreadable under a light OS palette");
    QVERIFY2(readableOn(popupPalette.color(QPalette::Active, QPalette::WindowText),
                        popupPalette.color(QPalette::Active, QPalette::Window)),
             "Popup label text is unreadable under a light OS palette");
    QVERIFY2(readableOn(popupPalette.color(QPalette::Active, QPalette::Text),
                        popupPalette.color(QPalette::Active, QPalette::Base)),
             "Popup field text is unreadable under a light OS palette");

    // The reset button is painted from the stylesheet, not the palette, so
    // check its rendered pixels: most of its area is the dark control
    // surface and its label must stand out against that surface.
    QPushButton* reset = nullptr;
    for (auto* button : popup->findChildren<QPushButton*>()) {
        if (button->property("schemeId").toString() == QStringLiteral("normal")) reset = button;
    }
    QVERIFY(reset);
    QCoreApplication::processEvents();
    const QImage shot = popup->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const qreal ratio = shot.devicePixelRatio();
    const QPoint resetTopLeft = reset->mapTo(popup, QPoint(0, 0));
    QRect region(QPointF(resetTopLeft.x() * ratio, resetTopLeft.y() * ratio).toPoint(),
                 QSizeF(reset->width() * ratio, reset->height() * ratio).toSize());
    // Skip the standard reset icon so only the label text can pass.
    region.adjust(static_cast<int>(44 * ratio), static_cast<int>(4 * ratio),
                  -static_cast<int>(4 * ratio), -static_cast<int>(4 * ratio));
    region = region.intersected(shot.rect());
    QVERIFY(!region.isEmpty());
    QMap<QRgb, int> histogram;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) ++histogram[shot.pixel(x, y)];
    }
    QRgb background = histogram.firstKey();
    for (auto it = histogram.begin(); it != histogram.end(); ++it) {
        if (it.value() > histogram.value(background)) background = it.key();
    }
    int readable = 0;
    for (auto it = histogram.begin(); it != histogram.end(); ++it) {
        if (theme_contrast::ContrastRatio(QColor(it.key()), QColor(background)) >= 4.5) {
            readable += it.value();
        }
    }
    QVERIFY2(readable >= 12, "Popup reset label is unreadable under a light OS palette");

    // A checked quick choice paints its label itself; it must use the
    // highlighted-text role on the highlight surface, not button text.
    auto* checked = Quick(picker, "yellow-black");
    QVERIFY(checked);
    picker.setCurrentScheme(*color_schemes::FindBuiltInColorScheme(QStringLiteral("yellow-black")));
    QVERIFY(checked->isChecked());
    QCoreApplication::processEvents();
    const QImage buttonShot = checked->grab().toImage().convertToFormat(QImage::Format_RGB32);
    QMap<QRgb, int> buttonHistogram;
    const qreal buttonRatio = buttonShot.devicePixelRatio();
    const int labelLeft = static_cast<int>(60 * buttonRatio);
    for (int y = 0; y < buttonShot.height(); ++y) {
        for (int x = labelLeft; x < buttonShot.width(); ++x) {
            ++buttonHistogram[buttonShot.pixel(x, y)];
        }
    }
    QRgb buttonBackground = buttonHistogram.firstKey();
    for (auto it = buttonHistogram.begin(); it != buttonHistogram.end(); ++it) {
        if (it.value() > buttonHistogram.value(buttonBackground)) buttonBackground = it.key();
    }
    int buttonReadable = 0;
    for (auto it = buttonHistogram.begin(); it != buttonHistogram.end(); ++it) {
        if (theme_contrast::ContrastRatio(QColor(it.key()), QColor(buttonBackground)) >= 4.5) {
            buttonReadable += it.value();
        }
    }
    QVERIFY2(buttonReadable >= 12,
             "Checked quick-choice label is unreadable under a light OS palette");
}
}

OKUFLOW_GUI_TEST_MAIN(okuflow::ColorSchemePickerTests)
#include "color_scheme_picker_tests.moc"
