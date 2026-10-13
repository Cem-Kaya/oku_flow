#include "okuflow/app/app_language.hpp"
#include "okuflow/app/language_manager.hpp"
#include "../src/launcher/launcher_text.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QLocale>
#include <iostream>

// Exercise the real compiled catalogs and manager without creating a window
// or acquiring foreground/mouse input on the owner's desktop.
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("OKUFLOW_FORCE_RTL");
    QApplication application(argc, argv);
    bool passed = true;
    auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; passed = false; }
    };
    using namespace okuflow;
    check(SupportedAppLanguages().size() == 25, "expected 25 UI languages");
    LanguageManager manager(application);
    for (const auto language : SupportedAppLanguages()) {
        check(manager.SetLanguage(language, false), "compiled language catalog did not load");
        check(manager.languageCode() == AppLanguageCode(language), "manager reports the wrong language");
        check(application.layoutDirection() == AppLanguageLayoutDirection(language), "layout direction disagrees with locale");
        const auto label = QCoreApplication::translate("OkuFlow", "Application language");
        check(!label.isEmpty(), "empty translated language selector");
        if (language != AppLanguage::English)
            check(label != QStringLiteral("Application language"), "catalog sentinel stayed in English");
    }
    check(ResolveLanguagePreference(QStringLiteral("system"), {QStringLiteral("nl-NL")}) == AppLanguage::Dutch,
          "system preference did not select Dutch");
    check(ResolveLanguagePreference(QStringLiteral("de"), {QStringLiteral("tr-TR")}) == AppLanguage::German,
          "explicit saved preference must win");
    check(ResolveLanguagePreference(QStringLiteral("unknown"), {QStringLiteral("fr-CA")}) == AppLanguage::French,
          "unknown preference must follow supported system language");
    check(NormalizeLanguagePreference(QStringLiteral("PT_br")) == QStringLiteral("pt-BR"), "regional preference normalization failed");
    check(AppLanguageFromUiLanguages({QStringLiteral("zh-Hant"), QStringLiteral("nl-BE")}) == AppLanguage::Dutch,
          "Traditional Chinese must not silently select Simplified");
    check(AppLanguageFromUiLanguages({QStringLiteral("zh-CN")}) == AppLanguage::ChineseSimplified, "Simplified Chinese system mapping failed");
    check(AppLanguageFromUiLanguages({QStringLiteral("zh-Hans-TW")}) == AppLanguage::ChineseSimplified,
          "explicit Simplified script must win over territory");
    check(AppLanguageFromUiLanguages({QStringLiteral("zh-TW"), QStringLiteral("nl-BE")}) == AppLanguage::Dutch,
          "Traditional territory must preserve the next supported preference");
    check(AppLanguageFromUiLanguages({QStringLiteral("iw-IL")}) == AppLanguage::Hebrew, "legacy Hebrew locale failed");
    check(AppLanguageFromUiLanguages({QStringLiteral("in-ID")}) == AppLanguage::Indonesian, "legacy Indonesian locale failed");
    check(manager.SetLanguage(AppLanguage::English, false), "could not release first manager translator");
    LanguageManager failureManager(application, nullptr, [](AppLanguage language) {
        return language == AppLanguage::French ? QStringLiteral(":/missing-catalog.qm") : AppLanguageCatalogResource(language);
    });
    check(failureManager.SetLanguage(AppLanguage::Dutch, false), "Dutch setup for failed switch failed");
    const auto previous = QCoreApplication::translate("OkuFlow", "Application language");
    const auto locale = QLocale().name();
    const auto direction = application.layoutDirection();
    check(!failureManager.SetLanguage(AppLanguage::French, false), "missing catalog unexpectedly loaded");
    check(failureManager.language() == AppLanguage::Dutch &&
          QCoreApplication::translate("OkuFlow", "Application language") == previous &&
          QLocale().name() == locale && application.layoutDirection() == direction,
          "failed switch discarded the active translator, locale or direction");
    check(failureManager.SetLanguage(AppLanguage::German, false), "switch after failed catalog did not recover");
    using namespace okuflow::launcher;
    check(LanguageFromWindows(MAKELANGID(LANG_DUTCH, SUBLANG_DEFAULT)) == Language::Dutch, "launcher Dutch mapping failed");
    check(LanguageFromWindows(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)) == Language::English,
          "launcher silently selected Simplified for Traditional Chinese");
    for (std::size_t row = 0; row < kText.size(); ++row) {
        for (std::size_t column = 0; column < kMessageCount; ++column)
            check(Translate(static_cast<Language>(row), static_cast<Message>(column))[0] != L'\0', "launcher text missing");
    }
    return passed ? 0 : 1;
}
