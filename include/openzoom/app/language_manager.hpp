#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QObject>
#include <QList>
#include <QLocale>
#include <QString>
#include <QTranslator>

QT_BEGIN_NAMESPACE
class QApplication;
class QEvent;
QT_END_NAMESPACE

namespace openzoom {

enum class AppLanguage {
    English,
    Turkish,
    German,
};

QString AppLanguageCode(AppLanguage language);
QString AppLanguageNativeName(AppLanguage language);
QString AppLanguageFlagResource(AppLanguage language);
QLocale AppLanguageLocale(AppLanguage language);
Qt::LayoutDirection AppLanguageLayoutDirection(AppLanguage language);
QList<AppLanguage> SupportedAppLanguages();
AppLanguage AppLanguageFromCode(const QString& code);
AppLanguage AppLanguageFromSystemLocale();

// Owns the installed Qt translator and applies live language changes to
// OpenZoom's hand-built widget tree. The manager is intentionally app-owned:
// it never reads or writes settings itself.
class LanguageManager final : public QObject {
    Q_OBJECT
public:
    explicit LanguageManager(QApplication& application,
                             QObject* parent = nullptr);

    AppLanguage language() const noexcept { return language_; }
    QString languageCode() const;
    bool rtlTestMode() const noexcept { return forceRightToLeft_; }
    bool SetLanguage(AppLanguage language, bool announce = true);

signals:
    void languageChanged(const QString& code);
    void languageChangeFailed(const QString& code);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void ApplyLocale(AppLanguage language);

    QApplication* application_{};
    QTranslator translator_;
    AppLanguage language_{AppLanguage::English};
    bool translatorInstalled_{};
    bool applyingLanguage_{};
    bool forceRightToLeft_{};
};

} // namespace openzoom

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
