#pragma once

#include <QString>

namespace okuflow {

// Appends the model-facing response-language instruction for a supported
// OkuFlow language. Built-in prompts stay in English; user-authored
// assistant instructions remain separate and unmodified.
QString AppendResponseLanguageDirective(const QString& prompt,
                                        const QString& languageCode);

} // namespace okuflow
