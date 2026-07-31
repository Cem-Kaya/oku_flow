#pragma once

#include <QString>

namespace openzoom {

// Appends the model-facing response-language instruction for a supported
// OpenZoom language. Built-in prompts stay in English; user-authored
// assistant instructions remain separate and unmodified.
QString AppendResponseLanguageDirective(const QString& prompt,
                                        const QString& languageCode);

} // namespace openzoom
