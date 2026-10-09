#ifdef _WIN32

#include "okuflow/app/language_manager.hpp"

#include "okuflow/ui/ui_translation.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QLocale>
#include <QWidget>

#include <array>

namespace okuflow {
namespace {

struct LanguageDescriptor {
    AppLanguage language;
    const char* code;
    const char* nativeName;
    QLocale::Language localeLanguage;
    QLocale::Territory localeTerritory;
    const char* flagResource;
};

constexpr std::array<LanguageDescriptor, 3> kLanguages{{
    {AppLanguage::English, "en", "English",
     QLocale::English, QLocale::UnitedStates, ":/flags/us.svg"},
    {AppLanguage::Turkish, "tr", "Türkçe",
     QLocale::Turkish, QLocale::Turkey, ":/flags/tr.svg"},
    {AppLanguage::German, "de", "Deutsch",
     QLocale::German, QLocale::Germany, ":/flags/de.svg"},
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

bool ForceRtlRequested(const QApplication& application)
{
    if (application.arguments().contains(QStringLiteral("--rtl-test"))) {
        return true;
    }
    const QString value =
        qEnvironmentVariable("OKUFLOW_FORCE_RTL").trimmed().toLower();
    return value == QStringLiteral("1") ||
           value == QStringLiteral("true") ||
           value == QStringLiteral("yes") ||
           value == QStringLiteral("on");
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

QString AppLanguageFlagResource(AppLanguage language)
{
    return QString::fromLatin1(Descriptor(language).flagResource);
}

QLocale AppLanguageLocale(AppLanguage language)
{
    const LanguageDescriptor& descriptor = Descriptor(language);
    return QLocale(descriptor.localeLanguage, descriptor.localeTerritory);
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

AppLanguage AppLanguageFromCode(const QString& code)
{
    const QString normalized = code.trimmed().toLower();
    for (const LanguageDescriptor& descriptor : kLanguages) {
        if (normalized == QLatin1String(descriptor.code)) {
            return descriptor.language;
        }
    }
    return AppLanguage::English;
}

AppLanguage AppLanguageFromSystemLocale()
{
    const QLocale::Language systemLanguage = QLocale::system().language();
    for (const LanguageDescriptor& descriptor : kLanguages) {
        if (descriptor.localeLanguage == systemLanguage) {
            return descriptor.language;
        }
    }
    return AppLanguage::English;
}

LanguageManager::LanguageManager(QApplication& application, QObject* parent)
    : QObject(parent),
      application_(&application),
      forceRightToLeft_(ForceRtlRequested(application))
{
    application_->installEventFilter(this);
}

QString LanguageManager::languageCode() const
{
    return AppLanguageCode(language_);
}

bool LanguageManager::SetLanguage(AppLanguage language, bool announce)
{
    if (!application_ || applyingLanguage_) {
        return false;
    }
    if (language == language_ &&
        (language == AppLanguage::English || translatorInstalled_)) {
        ApplyLocale(language);
        return true;
    }

    applyingLanguage_ = true;
    if (translatorInstalled_) {
        application_->removeTranslator(&translator_);
        translatorInstalled_ = false;
    }

    if (language != AppLanguage::English) {
        const QString resourcePath =
            QStringLiteral(":/i18n/okuflow_%1.qm")
                .arg(AppLanguageCode(language));
        if (!translator_.load(resourcePath) ||
            !application_->installTranslator(&translator_)) {
            applyingLanguage_ = false;
            emit languageChangeFailed(AppLanguageCode(language));
            return false;
        }
        translatorInstalled_ = true;
    }

    language_ = language;
    ApplyLocale(language);
    applyingLanguage_ = false;

    // Qt posts LanguageChange to top-level widgets when a translator is
    // installed or removed. Reapply now as well so English restoration is
    // immediate and deterministic.
    for (QWidget* widget : application_->topLevelWidgets()) {
        RetranslateWidgetTree(widget);
    }
    if (announce) {
        emit languageChanged(AppLanguageCode(language));
    }
    return true;
}

bool LanguageManager::eventFilter(QObject* watched, QEvent* event)
{
    if (!event || applyingLanguage_) {
        return QObject::eventFilter(watched, event);
    }
    if (event->type() == QEvent::LanguageChange ||
        event->type() == QEvent::Show ||
        event->type() == QEvent::Polish) {
        if (auto* widget = qobject_cast<QWidget*>(watched)) {
            RetranslateWidgetTree(widget);
        }
    }
    return QObject::eventFilter(watched, event);
}

void LanguageManager::ApplyLocale(AppLanguage language)
{
    const QLocale locale = AppLanguageLocale(language);
    QLocale::setDefault(locale);
    application_->setLayoutDirection(
        forceRightToLeft_ ? Qt::RightToLeft : locale.textDirection());
}

} // namespace okuflow

#endif
