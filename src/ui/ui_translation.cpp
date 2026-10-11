#ifdef _WIN32

#include "okuflow/ui/ui_translation.hpp"

#include <QAbstractButton>
#include <QAccessible>
#include <QAccessibleEvent>
#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSet>
#include <QTextEdit>
#include <QTabWidget>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <functional>

namespace okuflow {

const char* const* TranslationCatalogSources(std::size_t* count);

namespace {

constexpr auto kSourceText = "_okuflowTranslationSourceText";
constexpr auto kSourceTitle = "_okuflowTranslationSourceTitle";
constexpr auto kSourcePlaceholder = "_okuflowTranslationSourcePlaceholder";
constexpr auto kSourceToolTip = "_okuflowTranslationSourceToolTip";
constexpr auto kSourceAccessibleName = "_okuflowTranslationSourceAccessibleName";
constexpr auto kSourceAccessibleDescription =
    "_okuflowTranslationSourceAccessibleDescription";
constexpr auto kSourceWindowTitle = "_okuflowTranslationSourceWindowTitle";
constexpr auto kSourceComboItems = "_okuflowTranslationSourceComboItems";
constexpr auto kSourceTabItems = "_okuflowTranslationSourceTabItems";
constexpr auto kComboItemsAreData = "_okuflowComboItemsAreData";
constexpr auto kLiveSourceText = "_okuflowLiveTranslationSourceText";
constexpr auto kLiveRolePrefix = "_okuflowLiveTranslationRolePrefix";
constexpr auto kLiveDescriptionSource =
    "_okuflowLiveTranslationDescriptionSource";

struct TranslationTemplate {
    QString source;
    QByteArray sourceUtf8;
    QRegularExpression expression;
    QVector<int> placeholderIndexes;
    int literalCharacters{};
};

QVector<TranslationTemplate> BuildTranslationTemplates()
{
    std::size_t sourceCount = 0;
    const char* const* sources =
        TranslationCatalogSources(&sourceCount);
    QVector<TranslationTemplate> templates;
    templates.reserve(static_cast<qsizetype>(sourceCount));

    for (std::size_t sourceIndex = 0;
         sourceIndex < sourceCount;
         ++sourceIndex) {
        const QString source =
            QString::fromUtf8(sources[sourceIndex]);
        QString expression = QStringLiteral("^");
        QVector<int> placeholderIndexes;
        QString literal;
        int literalCharacters = 0;
        for (qsizetype index = 0; index < source.size();) {
            if (source.at(index) == QLatin1Char('%')) {
                qsizetype digitIndex = index + 1;
                if (digitIndex < source.size() &&
                    source.at(digitIndex) == QLatin1Char('L')) {
                    ++digitIndex;
                }
                if (digitIndex < source.size() &&
                    source.at(digitIndex).isDigit() &&
                    source.at(digitIndex) != QLatin1Char('0')) {
                    expression +=
                        QRegularExpression::escape(literal);
                    literalCharacters += literal.size();
                    literal.clear();
                    expression += QStringLiteral("(.+?)");
                    placeholderIndexes.push_back(
                        source.at(digitIndex).digitValue());
                    index = digitIndex + 1;
                    continue;
                }
            }
            literal += source.at(index);
            ++index;
        }
        expression += QRegularExpression::escape(literal);
        literalCharacters += literal.size();
        expression += QStringLiteral("$");

        // A placeholder-only key is too broad to identify safely. It is also
        // not useful to translate because it has no reader-facing wording.
        if (placeholderIndexes.isEmpty() ||
            literalCharacters < 3) {
            continue;
        }
        templates.push_back(
            {source,
             source.toUtf8(),
             QRegularExpression(
                 expression,
                 QRegularExpression::DotMatchesEverythingOption),
             placeholderIndexes,
             literalCharacters});
    }
    std::sort(templates.begin(),
              templates.end(),
              [](const TranslationTemplate& left,
                 const TranslationTemplate& right) {
                  return left.literalCharacters >
                         right.literalCharacters;
              });
    return templates;
}

QString TranslateFormattedUi(const QString& formattedSource)
{
    static const QVector<TranslationTemplate> templates =
        BuildTranslationTemplates();
    // In the English source UI every template is untranslated, so no regular
    // expression can produce a different result. Inspect the actual catalog
    // rather than the locale: callers may install independent translators.
    const bool hasTranslatedTemplate = std::any_of(
        templates.cbegin(), templates.cend(),
        [](const TranslationTemplate& candidate) {
            return QCoreApplication::translate(
                       "OkuFlow", candidate.sourceUtf8.constData()) !=
                   candidate.source;
        });
    if (!hasTranslatedTemplate) {
        return formattedSource;
    }
    for (const TranslationTemplate& candidate : templates) {
        const QRegularExpressionMatch match =
            candidate.expression.match(formattedSource);
        if (!match.hasMatch()) {
            continue;
        }

        QString translated =
            QCoreApplication::translate(
                "OkuFlow", candidate.sourceUtf8.constData());
        if (translated == candidate.source) {
            return formattedSource;
        }

        QMap<int, QString> arguments;
        for (int captureIndex = 0;
             captureIndex < candidate.placeholderIndexes.size();
             ++captureIndex) {
            arguments.insert(
                candidate.placeholderIndexes.at(captureIndex),
                match.captured(captureIndex + 1));
        }
        QList<int> placeholderIndexes = arguments.keys();
        std::sort(placeholderIndexes.begin(),
                  placeholderIndexes.end(),
                  std::greater<int>());
        for (int placeholderIndex : placeholderIndexes) {
            const QString argument =
                arguments.value(placeholderIndex);
            translated.replace(
                QStringLiteral("%L") +
                    QString::number(placeholderIndex),
                argument);
            translated.replace(
                QStringLiteral("%") +
                    QString::number(placeholderIndex),
                argument);
        }
        return translated;
    }
    return formattedSource;
}

QString VisibleText(QWidget* widget)
{
    if (auto* label = qobject_cast<QLabel*>(widget)) {
        return label->text();
    }
    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        return button->text();
    }
    return {};
}

void ApplyVisibleText(QWidget* widget, const QString& text)
{
    if (auto* label = qobject_cast<QLabel*>(widget)) {
        label->setText(text);
    } else if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        button->setText(text);
    }
}

QString AccessibleText(const QString& text, const QString& rolePrefix)
{
    const QString prefix = rolePrefix.trimmed();
    if (prefix.isEmpty()) {
        return text;
    }
    if (text.isEmpty()) {
        return prefix;
    }
    return QStringLiteral("%1: %2").arg(prefix, text);
}

// Returns the current-language text for a source-language widget property.
// The first value seen becomes the source. If the property later no longer
// holds the translation applied here, application code replaced it (for
// example a status-dependent button label), so the new value becomes the
// source instead of being reverted on the next Show/Polish/LanguageChange pass.
QString TranslateTrackedProperty(QObject* object,
                                 const char* propertyName,
                                 const QString& currentValue)
{
    const QByteArray appliedName = QByteArray(propertyName) + "Applied";
    const QVariant source = object->property(propertyName);
    const QVariant applied = object->property(appliedName.constData());
    QString sourceText = currentValue;
    if (source.isValid() && applied.isValid() &&
        applied.toString() == currentValue) {
        sourceText = source.toString();
    } else {
        object->setProperty(propertyName, currentValue);
    }
    const QString translated = TranslateUi(sourceText);
    object->setProperty(appliedName.constData(), translated);
    return translated;
}

void TranslateWidget(QWidget* widget)
{
    if (!widget) {
        return;
    }

    if (!widget->windowTitle().isEmpty()) {
        widget->setWindowTitle(TranslateTrackedProperty(
            widget, kSourceWindowTitle, widget->windowTitle()));
    }
    if (!widget->toolTip().isEmpty()) {
        widget->setToolTip(TranslateTrackedProperty(
            widget, kSourceToolTip, widget->toolTip()));
    }
    const QVariant liveSource = widget->property(kLiveSourceText);
    if (liveSource.isValid()) {
        const QString previousText = VisibleText(widget);
        const QString translatedText = TranslateUi(liveSource.toString());
        const QString translatedAccessible = AccessibleText(
            translatedText,
            TranslateUi(widget->property(kLiveRolePrefix).toString()));
        const QString previousAccessible = widget->accessibleName();
        ApplyVisibleText(widget, translatedText);
        widget->setAccessibleName(translatedAccessible);
        if (previousText != translatedText &&
            qobject_cast<QLabel*>(widget)) {
            QAccessibleTextUpdateEvent textEvent(
                widget, 0, previousText, translatedText);
            QAccessible::updateAccessibility(&textEvent);
        }
        if (previousAccessible != translatedAccessible) {
            QAccessibleEvent nameEvent(widget, QAccessible::NameChanged);
            QAccessible::updateAccessibility(&nameEvent);
        }
    } else if (!widget->accessibleName().isEmpty()) {
        widget->setAccessibleName(TranslateTrackedProperty(
            widget, kSourceAccessibleName, widget->accessibleName()));
    }
    const QVariant liveDescription = widget->property(kLiveDescriptionSource);
    if (liveDescription.isValid()) {
        widget->setAccessibleDescription(
            TranslateUi(liveDescription.toString()));
    } else if (!widget->accessibleDescription().isEmpty()) {
        widget->setAccessibleDescription(TranslateTrackedProperty(
            widget,
            kSourceAccessibleDescription,
            widget->accessibleDescription()));
    }

    if (!liveSource.isValid()) {
        if (auto* label = qobject_cast<QLabel*>(widget)) {
            if (!label->text().isEmpty()) {
                label->setText(TranslateTrackedProperty(
                    label, kSourceText, label->text()));
            }
        } else if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
            if (!button->text().isEmpty()) {
                button->setText(TranslateTrackedProperty(
                    button, kSourceText, button->text()));
            }
        }
    }

    if (auto* group = qobject_cast<QGroupBox*>(widget)) {
        if (!group->title().isEmpty()) {
            group->setTitle(TranslateTrackedProperty(
                group, kSourceTitle, group->title()));
        }
    }

    if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
        if (!edit->placeholderText().isEmpty()) {
            edit->setPlaceholderText(TranslateTrackedProperty(
                edit, kSourcePlaceholder, edit->placeholderText()));
        }
    } else if (auto* plainEdit = qobject_cast<QPlainTextEdit*>(widget)) {
        if (!plainEdit->placeholderText().isEmpty()) {
            plainEdit->setPlaceholderText(TranslateTrackedProperty(
                plainEdit, kSourcePlaceholder, plainEdit->placeholderText()));
        }
    } else if (auto* textEdit = qobject_cast<QTextEdit*>(widget)) {
        if (!textEdit->placeholderText().isEmpty()) {
            textEdit->setPlaceholderText(TranslateTrackedProperty(
                textEdit, kSourcePlaceholder, textEdit->placeholderText()));
        }
    }

    auto* combo = qobject_cast<QComboBox*>(widget);
    if (combo && !combo->property(kComboItemsAreData).toBool()) {
        QStringList sources =
            combo->property(kSourceComboItems).toStringList();
        if (sources.size() != combo->count()) {
            sources.clear();
            sources.reserve(combo->count());
            for (int i = 0; i < combo->count(); ++i) {
                sources.push_back(combo->itemText(i));
            }
            combo->setProperty(kSourceComboItems, sources);
        }
        for (int i = 0; i < combo->count(); ++i) {
            combo->setItemText(i, TranslateUi(sources.at(i)));
        }
    }

    if (auto* tabs = qobject_cast<QTabWidget*>(widget)) {
        QStringList sources =
            tabs->property(kSourceTabItems).toStringList();
        if (sources.size() != tabs->count()) {
            sources.clear();
            sources.reserve(tabs->count());
            for (int i = 0; i < tabs->count(); ++i) {
                sources.push_back(tabs->tabText(i));
            }
            tabs->setProperty(kSourceTabItems, sources);
        }
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setTabText(i, TranslateUi(sources.at(i)));
        }
    }
}

} // namespace

QString TranslateUi(const QString& sourceText)
{
    if (sourceText.isEmpty()) {
        return sourceText;
    }
    const QByteArray utf8 = sourceText.toUtf8();
    const QString translated =
        QCoreApplication::translate("OkuFlow", utf8.constData());
    if (translated != sourceText) {
        return translated;
    }
    // An exact catalog key has already been looked up. Only a formatted
    // source with substituted arguments needs template identification.
    static const QSet<QByteArray> sourceKeys = []() {
        std::size_t count = 0;
        const char* const* sources = TranslationCatalogSources(&count);
        QSet<QByteArray> keys;
        keys.reserve(static_cast<qsizetype>(count));
        for (std::size_t i = 0; i < count; ++i) {
            keys.insert(QByteArray(sources[i]));
        }
        return keys;
    }();
    return sourceKeys.contains(utf8)
               ? sourceText
               : TranslateFormattedUi(sourceText);
}

void SetLiveTranslationSource(QWidget* widget,
                              const QString& sourceText,
                              const QString& rolePrefix)
{
    if (!widget) {
        return;
    }
    const QVariant previousSource = widget->property(kLiveSourceText);
    if (!previousSource.isValid() || previousSource.toString() != sourceText) {
        widget->setProperty(kLiveSourceText, sourceText);
    }
    const QVariant previousPrefix = widget->property(kLiveRolePrefix);
    if (!previousPrefix.isValid() || previousPrefix.toString() != rolePrefix) {
        widget->setProperty(kLiveRolePrefix, rolePrefix);
    }
}

void SetLiveAccessibleDescription(QWidget* widget, const QString& sourceText)
{
    if (!widget) {
        return;
    }
    widget->setProperty(kLiveDescriptionSource, sourceText);
    const QString description = TranslateUi(sourceText);
    if (widget->accessibleDescription() != description) {
        widget->setAccessibleDescription(description);
        QAccessibleEvent event(widget, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
}

void RetranslateWidget(QWidget* widget)
{
    TranslateWidget(widget);
}

void RetranslateWidgetTree(QWidget* root)
{
    if (!root) {
        return;
    }
    TranslateWidget(root);
    const QList<QWidget*> children =
        root->findChildren<QWidget*>(QString(), Qt::FindChildrenRecursively);
    for (QWidget* child : children) {
        TranslateWidget(child);
    }
}

void SetComboItemsAreData(QWidget* combo)
{
    if (combo) {
        combo->setProperty(kComboItemsAreData, true);
    }
}

} // namespace okuflow

#endif
