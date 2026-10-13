#include "okuflow/common/response_language.hpp"

#include "okuflow/app/app_language.hpp"

namespace okuflow {

QString AppendResponseLanguageDirective(const QString& prompt,
                                        const QString& languageCode)
{
    AppLanguage language = AppLanguage::English;
    const QString directive =
        AppLanguageFromCode(languageCode, &language) &&
                language != AppLanguage::English
            ? QStringLiteral("Respond in %1.")
                  .arg(AppLanguageEnglishName(language))
            : QString();
    if (directive.isEmpty()) {
        return prompt;
    }

    const QString trimmed = prompt.trimmed();
    return trimmed.isEmpty()
               ? directive
               : QStringLiteral("%1\n\n%2").arg(trimmed, directive);
}

} // namespace okuflow
