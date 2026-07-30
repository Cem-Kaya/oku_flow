#ifdef _WIN32

#include "openzoom/app/protected_secret_store.hpp"

#include <QByteArray>

#include <windows.h>
#include <wincred.h>

#include <algorithm>

namespace openzoom {

namespace {

QString WindowsErrorMessage(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    QString message =
        size > 0 && buffer ? QString::fromWCharArray(buffer, static_cast<int>(size)).trimmed()
                           : QStringLiteral("Windows error %1").arg(code);
    if (buffer) {
        LocalFree(buffer);
    }
    return message;
}

std::wstring NativeCredentialId(const QString& credentialId)
{
    return credentialId.toStdWString();
}

} // namespace

QString ProtectedSecretStore::DefaultVlmCredentialId()
{
    return QStringLiteral("OpenZoom/VLM API Key");
}

ProtectedSecretResult ProtectedSecretStore::Read(const QString& credentialId)
{
    ProtectedSecretResult result;
    if (credentialId.trimmed().isEmpty()) {
        return result;
    }

    const std::wstring target = NativeCredentialId(credentialId.trimmed());
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_FOUND) {
            return result;
        }
        result.status = ProtectedSecretResult::Status::Error;
        result.error =
            QStringLiteral("Could not read the protected credential: %1")
                .arg(WindowsErrorMessage(error));
        return result;
    }

    const QByteArray secret(
        reinterpret_cast<const char*>(credential->CredentialBlob),
        static_cast<qsizetype>(credential->CredentialBlobSize));
    result.status = ProtectedSecretResult::Status::Found;
    result.value = QString::fromUtf8(secret);
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        SecureZeroMemory(credential->CredentialBlob,
                         credential->CredentialBlobSize);
    }
    CredFree(credential);
    return result;
}

bool ProtectedSecretStore::Write(const QString& credentialId,
                                 const QString& secret,
                                 QString* error)
{
    const QString cleanId = credentialId.trimmed();
    if (cleanId.isEmpty()) {
        if (error) {
            *error = QStringLiteral("The protected credential id is empty.");
        }
        return false;
    }

    QByteArray utf8 = secret.toUtf8();
    if (utf8.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        if (error) {
            *error = QStringLiteral("The API key is too large for Windows Credential Manager.");
        }
        SecureZeroMemory(utf8.data(), static_cast<SIZE_T>(utf8.size()));
        return false;
    }

    const std::wstring target = NativeCredentialId(cleanId);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(utf8.size());
    credential.CredentialBlob =
        reinterpret_cast<LPBYTE>(utf8.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"OpenZoom");

    const BOOL written = CredWriteW(&credential, 0);
    const DWORD windowsError = written ? ERROR_SUCCESS : GetLastError();
    if (!utf8.isEmpty()) {
        SecureZeroMemory(utf8.data(), static_cast<SIZE_T>(utf8.size()));
    }
    if (!written) {
        if (error) {
            *error = QStringLiteral("Could not protect the API key: %1")
                         .arg(WindowsErrorMessage(windowsError));
        }
        return false;
    }
    return true;
}

bool ProtectedSecretStore::Remove(const QString& credentialId, QString* error)
{
    const QString cleanId = credentialId.trimmed();
    if (cleanId.isEmpty()) {
        return true;
    }

    const std::wstring target = NativeCredentialId(cleanId);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        return true;
    }
    const DWORD windowsError = GetLastError();
    if (windowsError == ERROR_NOT_FOUND) {
        return true;
    }
    if (error) {
        *error = QStringLiteral("Could not remove the protected API key: %1")
                     .arg(WindowsErrorMessage(windowsError));
    }
    return false;
}

} // namespace openzoom

#endif
