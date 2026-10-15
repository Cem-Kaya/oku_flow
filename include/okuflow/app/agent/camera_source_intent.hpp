#pragma once
#include <QString>
#include <QStringList>
#include <optional>

namespace okuflow::agent {
enum class CameraSourceOrigin { Startup, Manual, AgentTemporary, Reconnect, Fallback };
struct CameraSourceIdentity {
    QString symbolicLink, formatStableId;
    bool firstFrame{};
};
struct SavedCameraIntent {
    QString symbolicLink, formatStableId;
    int legacyIndex{-1};
    bool explicitSelection{};
};
struct RequestedCameraSource {
    CameraSourceIdentity identity;
    CameraSourceOrigin origin{CameraSourceOrigin::Startup};
    quint64 generation{};
    QString operationId;
};
struct CameraRestoreChoice { CameraSourceIdentity identity; bool usesSavedStartup{}; };

// Internal physical identities never enter a provider DTO. Saved intent,
// requested transition and actual first-frame identity have distinct owners.
class CameraSourceIntent final {
public:
    void LoadSaved(SavedCameraIntent saved);
    const SavedCameraIntent& Saved() const { return saved_; }
    const RequestedCameraSource& Requested() const { return requested_; }
    const std::optional<CameraSourceIdentity>& Active() const { return active_; }
    void SelectManual(const QString& link, int index);
    void SetSavedFormat(const QString& format);
    bool FillStartupIdentity(const QString& link, int index);
    void ReindexSaved(const QStringList& enumeration);
    quint64 Request(CameraSourceIdentity identity, CameraSourceOrigin origin, const QString& operationId = {});
    bool RequestTemporary(const QString& owner, CameraSourceIdentity identity, const QString& operationId);
    bool Activate(quint64 generation, CameraSourceIdentity identity, bool fallback = false);
    bool MarkFirstFrame(quint64 generation);
    void Deactivate();
    bool FallbackActive() const { return fallbackActive_; }
    QString TemporaryOwner() const { return temporaryOwner_; }
    QString TemporaryPhoneIntent() const;
    std::optional<CameraRestoreChoice> RestoreChoice(const QStringList& enumeration) const;
    bool ReleaseTemporary(const QString& owner);
private:
    SavedCameraIntent saved_;
    RequestedCameraSource requested_;
    std::optional<CameraSourceIdentity> active_, beforeTemporary_;
    QString temporaryOwner_, temporaryRequestedLink_;
    bool fallbackActive_{};
};
} // namespace okuflow::agent
