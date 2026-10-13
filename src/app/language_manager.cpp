#ifdef _WIN32

#include "okuflow/app/language_manager.hpp"

#include "okuflow/ui/ui_translation.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QLocale>
#include <QWidget>

namespace okuflow {
namespace {

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

LanguageManager::LanguageManager(QApplication& application,
                                 QObject* parent,
                                 CatalogResolver catalogResolver)
    : QObject(parent),
      application_(&application),
      catalogResolver_(catalogResolver ? std::move(catalogResolver)
                                       : CatalogResolver(
                                             &AppLanguageCatalogResource)),
      forceRightToLeft_(ForceRtlRequested(application))
{
    application_->installEventFilter(this);
}

LanguageManager::~LanguageManager()
{
    if (application_ && translator_) {
        application_->removeTranslator(translator_.get());
    }
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
        (language == AppLanguage::English || translator_)) {
        ApplyLocale(language);
        return true;
    }

    // Load the candidate completely before touching the active translator.
    std::unique_ptr<QTranslator> candidate;
    if (language != AppLanguage::English) {
        const QString resourcePath = catalogResolver_(language);
        candidate = std::make_unique<QTranslator>();
        if (resourcePath.isEmpty() || !candidate->load(resourcePath)) {
            emit languageChangeFailed(AppLanguageCode(language));
            return false;
        }
    }

    applyingLanguage_ = true;
    if (candidate && !application_->installTranslator(candidate.get())) {
        applyingLanguage_ = false;
        emit languageChangeFailed(AppLanguageCode(language));
        return false;
    }
    if (translator_) {
        application_->removeTranslator(translator_.get());
    }
    translator_ = std::move(candidate);

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
    if (event->type() != QEvent::LanguageChange &&
        event->type() != QEvent::Show &&
        event->type() != QEvent::Polish) {
        return QObject::eventFilter(watched, event);
    }
    if (auto* widget = qobject_cast<QWidget*>(watched)) {
        if (event->type() == QEvent::LanguageChange) {
            RetranslateWidgetTree(widget);
        } else if (event->type() == QEvent::Show ||
                   event->type() == QEvent::Polish) {
            // Qt polishes/shows descendants individually. Walking their
            // subtrees here repeats translation for every ancestor event.
            RetranslateWidget(widget);
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
