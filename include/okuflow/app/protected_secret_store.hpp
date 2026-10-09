#pragma once

#include <QString>

namespace okuflow {

struct ProtectedSecretResult {
    enum class Status {
        Found,
        NotFound,
        Error,
    };

    Status status{Status::NotFound};
    QString value;
    QString error;
};

// Stores user secrets in Windows Credential Manager. The settings file keeps
// only the opaque credential id; the secret never belongs in settings JSON.
class ProtectedSecretStore {
public:
    static QString DefaultVlmCredentialId();

    static ProtectedSecretResult Read(const QString& credentialId);
    static bool Write(const QString& credentialId,
                      const QString& secret,
                      QString* error = nullptr);
    static bool Remove(const QString& credentialId,
                       QString* error = nullptr);
};

} // namespace okuflow
