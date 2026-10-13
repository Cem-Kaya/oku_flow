#pragma once

#include <QList>
#include <QLocale>
#include <QString>
#include <QStringList>

namespace okuflow {

// OkuFlow's 25 UI languages: the okuflow.com website set, Dutch, and ten
// further widely used Windows/Android display languages. The declaration
// order is the language-picker order after "System default".
enum class AppLanguage {
    English,
    Turkish,
    German,
    French,
    Spanish,
    Italian,
    PortugueseBrazil,
    Russian,
    Ukrainian,
    Arabic,
    Hindi,
    Japanese,
    Korean,
    ChineseSimplified,
    Dutch,
    Polish,
    Czech,
    Swedish,
    Indonesian,
    Vietnamese,
    Thai,
    Bengali,
    Persian,
    Hebrew,
    Urdu,
};

// Persisted preference token that follows the Windows display language.
inline constexpr char kSystemLanguagePreference[] = "system";

// Stable BCP 47 code used in settings, AI response directives, and notes
// documents: "en", "pt-BR", "zh-Hans", ...
QString AppLanguageCode(AppLanguage language);
QString AppLanguageNativeName(AppLanguage language);
// English language name for model-facing response-language directives.
QString AppLanguageEnglishName(AppLanguage language);
QString AppLanguageFlagResource(AppLanguage language);
// Embedded Qt catalog path; empty for English, which needs no translator.
QString AppLanguageCatalogResource(AppLanguage language);
QLocale AppLanguageLocale(AppLanguage language);
Qt::LayoutDirection AppLanguageLayoutDirection(AppLanguage language);
QList<AppLanguage> SupportedAppLanguages();

// Exact supported-code lookup. Matching ignores case and accepts '_' for '-'
// ("pt_br", "zh-hans"); returns false for anything else.
bool AppLanguageFromCode(const QString& code, AppLanguage* language);
// Lenient lookup that falls back to English for unknown codes.
AppLanguage AppLanguageFromCode(const QString& code);

// Maps one BCP 47 UI-language tag ("de-AT", "pt-PT", "zh-CN") to a supported
// language. Every Portuguese tag maps to Portuguese (Brazil); Traditional
// Chinese tags are not mapped to Simplified Chinese.
bool AppLanguageFromLocaleTag(const QString& tag, AppLanguage* language);
// First supported entry of an ordered UI-language preference list, else
// English.
AppLanguage AppLanguageFromUiLanguages(const QStringList& uiLanguages);
// The supported language matching the Windows display-language preferences.
AppLanguage AppLanguageFromSystemLocale();

// Returns a canonical persisted preference: a supported language code or
// kSystemLanguagePreference. Unknown values return an empty string, which
// callers treat as "System default".
QString NormalizeLanguagePreference(const QString& preference);
bool IsSystemLanguagePreference(const QString& preference);
// An explicit supported code wins; "system", empty, or unknown preferences
// follow the supplied ordered system UI-language list.
AppLanguage ResolveLanguagePreference(const QString& preference,
                                      const QStringList& systemUiLanguages);

} // namespace okuflow
