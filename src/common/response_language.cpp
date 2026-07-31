#include "openzoom/common/response_language.hpp"

namespace openzoom {

QString AppendResponseLanguageDirective(const QString& prompt,
                                        const QString& languageCode)
{
    QString directive;
    const QString normalized = languageCode.trimmed().toLower();
    if (normalized == QStringLiteral("tr")) {
        directive = QStringLiteral("Respond in Turkish.");
    } else if (normalized == QStringLiteral("de")) {
        directive = QStringLiteral("Respond in German.");
    }
    if (directive.isEmpty()) {
        return prompt;
    }

    const QString trimmed = prompt.trimmed();
    return trimmed.isEmpty()
               ? directive
               : QStringLiteral("%1\n\n%2").arg(trimmed, directive);
}

} // namespace openzoom
