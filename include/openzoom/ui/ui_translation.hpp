#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QString>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace openzoom {

// Translates one source-language UI string using the shared catalog context.
// Serialization values, device names, diagnostics, and file names must not be
// passed through this function.
QString TranslateUi(const QString& sourceText);

// Records source-language dynamic text and its semantic role. Retranslation
// uses these values rather than the currently displayed translation, so
// Turkish/German can switch back to English without reconstructing widgets.
void SetLiveTranslationSource(QWidget* widget,
                              const QString& sourceText,
                              const QString& rolePrefix = {});

// Captures source-language properties the first time a widget is seen, then
// reapplies their translations. This lets the existing hand-built Qt UI react
// to QEvent::LanguageChange without recreating widgets or losing selection.
void RetranslateWidgetTree(QWidget* root);

// Excludes data-driven combo entries (camera names, model ids, voices) from
// the automatic item translation pass while still translating the combo's
// label, tooltip, and accessibility metadata.
void SetComboItemsAreData(QWidget* combo);

} // namespace openzoom

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
