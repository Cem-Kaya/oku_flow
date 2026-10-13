#include "okuflow/app/app_language.hpp"

#include <array>

namespace okuflow {
namespace {

struct LanguageDescriptor {
    AppLanguage language;
    const char* code;
    const char* nativeName;
    const char* englishName;
    QLocale::Language localeLanguage;
    QLocale::Script localeScript;
    QLocale::Territory localeTerritory;
    const char* flag;
};

// Flags are decorative picker icons; the accessible name is the native name.
constexpr std::array<LanguageDescriptor, 25> kLanguages{{
    {AppLanguage::English, "en", "English", "English",
     QLocale::English, QLocale::LatinScript, QLocale::UnitedStates, "us"},
    {AppLanguage::Turkish, "tr", "Türkçe", "Turkish",
     QLocale::Turkish, QLocale::LatinScript, QLocale::Turkey, "tr"},
    {AppLanguage::German, "de", "Deutsch", "German",
     QLocale::German, QLocale::LatinScript, QLocale::Germany, "de"},
    {AppLanguage::French, "fr", "Français", "French",
     QLocale::French, QLocale::LatinScript, QLocale::France, "fr"},
    {AppLanguage::Spanish, "es", "Español", "Spanish",
     QLocale::Spanish, QLocale::LatinScript, QLocale::Spain, "es"},
    {AppLanguage::Italian, "it", "Italiano", "Italian",
     QLocale::Italian, QLocale::LatinScript, QLocale::Italy, "it"},
    {AppLanguage::PortugueseBrazil, "pt-BR", "Português (Brasil)",
     "Brazilian Portuguese",
     QLocale::Portuguese, QLocale::LatinScript, QLocale::Brazil, "br"},
    {AppLanguage::Russian, "ru", "Русский", "Russian",
     QLocale::Russian, QLocale::CyrillicScript, QLocale::Russia, "ru"},
    {AppLanguage::Ukrainian, "uk", "Українська", "Ukrainian",
     QLocale::Ukrainian, QLocale::CyrillicScript, QLocale::Ukraine, "ua"},
    {AppLanguage::Arabic, "ar", "العربية", "Arabic",
     QLocale::Arabic, QLocale::ArabicScript, QLocale::UnitedArabEmirates, "ae"},
    {AppLanguage::Hindi, "hi", "हिन्दी", "Hindi",
     QLocale::Hindi, QLocale::DevanagariScript, QLocale::India, "in"},
    {AppLanguage::Japanese, "ja", "日本語", "Japanese",
     QLocale::Japanese, QLocale::JapaneseScript, QLocale::Japan, "jp"},
    {AppLanguage::Korean, "ko", "한국어", "Korean",
     QLocale::Korean, QLocale::KoreanScript, QLocale::SouthKorea, "kr"},
    {AppLanguage::ChineseSimplified, "zh-Hans", "简体中文",
     "Simplified Chinese",
     QLocale::Chinese, QLocale::SimplifiedHanScript, QLocale::China, "cn"},
    {AppLanguage::Dutch, "nl", "Nederlands", "Dutch",
     QLocale::Dutch, QLocale::LatinScript, QLocale::Netherlands, "nl"},
    {AppLanguage::Polish, "pl", "Polski", "Polish",
     QLocale::Polish, QLocale::LatinScript, QLocale::Poland, "pl"},
    {AppLanguage::Czech, "cs", "Čeština", "Czech",
     QLocale::Czech, QLocale::LatinScript, QLocale::Czechia, "cz"},
    {AppLanguage::Swedish, "sv", "Svenska", "Swedish",
     QLocale::Swedish, QLocale::LatinScript, QLocale::Sweden, "se"},
    {AppLanguage::Indonesian, "id", "Bahasa Indonesia", "Indonesian",
     QLocale::Indonesian, QLocale::LatinScript, QLocale::Indonesia, "id"},
    {AppLanguage::Vietnamese, "vi", "Tiếng Việt", "Vietnamese",
     QLocale::Vietnamese, QLocale::LatinScript, QLocale::Vietnam, "vn"},
    {AppLanguage::Thai, "th", "ไทย", "Thai",
     QLocale::Thai, QLocale::ThaiScript, QLocale::Thailand, "th"},
    {AppLanguage::Bengali, "bn", "বাংলা", "Bengali",
     QLocale::Bangla, QLocale::BanglaScript, QLocale::Bangladesh, "bd"},
    {AppLanguage::Persian, "fa", "فارسی", "Persian",
     QLocale::Persian, QLocale::ArabicScript, QLocale::Iran, "ir"},
    {AppLanguage::Hebrew, "he", "עברית", "Hebrew",
     QLocale::Hebrew, QLocale::HebrewScript, QLocale::Israel, "il"},
    {AppLanguage::Urdu, "ur", "اردو", "Urdu",
     QLocale::Urdu, QLocale::ArabicScript, QLocale::Pakistan, "pk"},
}};

const LanguageDescriptor& Descriptor(AppLanguage language)
{
    for (const LanguageDescriptor& descriptor : kLanguages) {
        if (descriptor.language == language) {
            return descriptor;
        }
    }
    return kLanguages.front();
}

QString CanonicalTag(const QString& value)
{
    return value.trimmed().replace(QLatin1Char('_'), QLatin1Char('-')).toLower();
}

} // namespace

QString AppLanguageCode(AppLanguage language)
{
    return QString::fromLatin1(Descriptor(language).code);
}

QString AppLanguageNativeName(AppLanguage language)
{
    return QString::fromUtf8(Descriptor(language).nativeName);
}

QString AppLanguageEnglishName(AppLanguage language)
{
    return QString::fromLatin1(Descriptor(language).englishName);
}

QString AppLanguageFlagResource(AppLanguage language)
{
    return QStringLiteral(":/flags/%1.svg")
        .arg(QString::fromLatin1(Descriptor(language).flag));
}

QString AppLanguageCatalogResource(AppLanguage language)
{
    if (language == AppLanguage::English) {
        return {};
    }
    return QStringLiteral(":/i18n/okuflow_%1.qm")
        .arg(AppLanguageCode(language).replace(QLatin1Char('-'),
                                               QLatin1Char('_')));
}

QLocale AppLanguageLocale(AppLanguage language)
{
    const LanguageDescriptor& descriptor = Descriptor(language);
    return QLocale(descriptor.localeLanguage,
                   descriptor.localeScript,
                   descriptor.localeTerritory);
}

Qt::LayoutDirection AppLanguageLayoutDirection(AppLanguage language)
{
    return AppLanguageLocale(language).textDirection();
}

QList<AppLanguage> SupportedAppLanguages()
{
    QList<AppLanguage> languages;
    languages.reserve(static_cast<qsizetype>(kLanguages.size()));
    for (const LanguageDescriptor& descriptor : kLanguages) {
        languages.push_back(descriptor.language);
    }
    return languages;
}

bool AppLanguageFromCode(const QString& code, AppLanguage* language)
{
    const QString normalized = CanonicalTag(code);
    for (const LanguageDescriptor& descriptor : kLanguages) {
        if (normalized == QString::fromLatin1(descriptor.code).toLower()) {
            if (language) {
                *language = descriptor.language;
            }
            return true;
        }
    }
    return false;
}

AppLanguage AppLanguageFromCode(const QString& code)
{
    AppLanguage language = AppLanguage::English;
    AppLanguageFromCode(code, &language);
    return language;
}

bool AppLanguageFromLocaleTag(const QString& tag, AppLanguage* language)
{
    const QString normalized = CanonicalTag(tag);
    if (normalized.isEmpty()) {
        return false;
    }
    // Legacy Hebrew/Indonesian tags still appear in some language lists.
    const QString primary = normalized.section(QLatin1Char('-'), 0, 0);
    if (primary == QStringLiteral("zh")) {
        // QLocale can fill omitted subtags from its default territory. Keep
        // the user's explicit script authoritative before that expansion.
        const QStringList subtags = normalized.split(QLatin1Char('-'));
        if (subtags.contains(QStringLiteral("hant")) ||
            (!subtags.contains(QStringLiteral("hans")) &&
             (subtags.contains(QStringLiteral("tw")) ||
              subtags.contains(QStringLiteral("hk")) ||
              subtags.contains(QStringLiteral("mo"))))) {
            return false;
        }
        if (subtags.contains(QStringLiteral("hans"))) {
            if (language) *language = AppLanguage::ChineseSimplified;
            return true;
        }
    }
    QString effective = normalized;
    if (primary == QStringLiteral("iw")) {
        effective = QStringLiteral("he");
    } else if (primary == QStringLiteral("in")) {
        effective = QStringLiteral("id");
    }
    const QLocale locale(effective);
    if (locale.language() == QLocale::C) {
        return false;
    }
    for (const LanguageDescriptor& descriptor : kLanguages) {
        if (descriptor.localeLanguage != locale.language()) {
            continue;
        }
        if (descriptor.localeLanguage == QLocale::Chinese &&
            locale.script() != QLocale::SimplifiedHanScript) {
            return false;
        }
        if (language) {
            *language = descriptor.language;
        }
        return true;
    }
    return false;
}

AppLanguage AppLanguageFromUiLanguages(const QStringList& uiLanguages)
{
    for (const QString& tag : uiLanguages) {
        AppLanguage language = AppLanguage::English;
        if (AppLanguageFromLocaleTag(tag, &language)) {
            return language;
        }
    }
    return AppLanguage::English;
}

AppLanguage AppLanguageFromSystemLocale()
{
    return AppLanguageFromUiLanguages(QLocale::system().uiLanguages());
}

QString NormalizeLanguagePreference(const QString& preference)
{
    const QString normalized = CanonicalTag(preference);
    if (normalized == QLatin1String(kSystemLanguagePreference)) {
        return QString::fromLatin1(kSystemLanguagePreference);
    }
    AppLanguage language = AppLanguage::English;
    return AppLanguageFromCode(normalized, &language)
               ? AppLanguageCode(language)
               : QString();
}

bool IsSystemLanguagePreference(const QString& preference)
{
    const QString normalized = NormalizeLanguagePreference(preference);
    return normalized.isEmpty() ||
           normalized == QLatin1String(kSystemLanguagePreference);
}

AppLanguage ResolveLanguagePreference(const QString& preference,
                                      const QStringList& systemUiLanguages)
{
    AppLanguage language = AppLanguage::English;
    if (!IsSystemLanguagePreference(preference) &&
        AppLanguageFromCode(preference, &language)) {
        return language;
    }
    return AppLanguageFromUiLanguages(systemUiLanguages);
}

} // namespace okuflow
