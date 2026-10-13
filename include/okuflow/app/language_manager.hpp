#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include "okuflow/app/app_language.hpp"

#include <QObject>
#include <QString>
#include <QTranslator>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QApplication;
class QEvent;
QT_END_NAMESPACE

namespace okuflow {

// Owns the installed Qt translator and applies live language changes to
// OkuFlow's hand-built widget tree. The manager is intentionally app-owned:
// it never reads or writes settings itself.
//
// Changes are load-then-swap: the requested catalog is loaded and installed
// before the previous translator is removed, so a failed load leaves the
// previous language, translator, locale and layout direction fully active.
class LanguageManager final : public QObject {
    Q_OBJECT
public:
    // Maps a language to its embedded catalog path (empty for English).
    using CatalogResolver = std::function<QString(AppLanguage)>;

    // `catalogResolver` defaults to AppLanguageCatalogResource; tests pass a
    // resolver that names a missing catalog to exercise failure handling.
    explicit LanguageManager(QApplication& application,
                             QObject* parent = nullptr,
                             CatalogResolver catalogResolver = {});
    ~LanguageManager() override;

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
    CatalogResolver catalogResolver_;
    std::unique_ptr<QTranslator> translator_;
    AppLanguage language_{AppLanguage::English};
    bool applyingLanguage_{};
    bool forceRightToLeft_{};
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
