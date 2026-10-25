#include "okuflow/app/agent/camera_source_intent.hpp"

namespace okuflow::agent {
void CameraSourceIntent::LoadSaved(SavedCameraIntent saved) { saved_ = std::move(saved); }
void CameraSourceIntent::SelectManual(const QString& link, int index) {
    saved_.symbolicLink = link; saved_.legacyIndex = index; saved_.explicitSelection = true;
    temporaryOwner_.clear(); temporaryRequestedLink_.clear(); beforeTemporary_.reset();
    Request({link,saved_.formatStableId,false},CameraSourceOrigin::Manual);
}
void CameraSourceIntent::SetSavedFormat(const QString& format) {
    saved_.formatStableId = format;
    const auto link = active_ ? active_->symbolicLink : requested_.identity.symbolicLink;
    temporaryOwner_.clear(); temporaryRequestedLink_.clear(); beforeTemporary_.reset();
    Request({link,format,false},CameraSourceOrigin::Manual);
}
bool CameraSourceIntent::RememberAdoptedFormat(quint64 generation, const QString& format) {
    if(generation!=requested_.generation || !active_ || !temporaryOwner_.isEmpty())return false;
    saved_.formatStableId=format;return true;
}
bool CameraSourceIntent::FillStartupIdentity(const QString& link, int index) {
    if (!saved_.symbolicLink.isEmpty() || !temporaryOwner_.isEmpty() || link.isEmpty() ||
        (requested_.origin != CameraSourceOrigin::Startup && requested_.origin != CameraSourceOrigin::Manual)) return false;
    saved_.symbolicLink = link; saved_.legacyIndex = index; return true;
}
void CameraSourceIntent::ReindexSaved(const QStringList& enumeration) {
    if (saved_.symbolicLink.isEmpty()) return;
    const auto index = enumeration.indexOf(saved_.symbolicLink);
    if (index >= 0) saved_.legacyIndex = index;
}
quint64 CameraSourceIntent::Request(CameraSourceIdentity identity, CameraSourceOrigin origin, const QString& operationId) {
    identity.firstFrame = false;
    const auto generation = requested_.generation+1;
    requested_ = {std::move(identity),origin,generation,operationId}; return generation;
}
bool CameraSourceIntent::RequestTemporary(const QString& owner, CameraSourceIdentity identity, const QString& operationId) {
    if (owner.isEmpty() || identity.symbolicLink.isEmpty() || (!temporaryOwner_.isEmpty() && temporaryOwner_ != owner)) return false;
    if (temporaryOwner_.isEmpty()) { temporaryOwner_ = owner; beforeTemporary_ = active_; }
    temporaryRequestedLink_ = identity.symbolicLink;
    Request(std::move(identity),CameraSourceOrigin::AgentTemporary,operationId); return true;
}
bool CameraSourceIntent::Activate(quint64 generation, CameraSourceIdentity identity, bool fallback) {
    if (generation != requested_.generation || (!fallback && identity.symbolicLink != requested_.identity.symbolicLink)) return false;
    active_ = std::move(identity); fallbackActive_ = fallback; return true;
}
bool CameraSourceIntent::MarkFirstFrame(quint64 generation) {
    if (generation != requested_.generation || !active_) return false;
    active_->firstFrame = true; return true;
}
void CameraSourceIntent::Deactivate() { active_.reset(); fallbackActive_ = false; }
QString CameraSourceIntent::TemporaryPhoneIntent() const {
    return temporaryRequestedLink_.startsWith(QStringLiteral("okuflow-phone:")) ? temporaryRequestedLink_ : QString();
}
std::optional<CameraRestoreChoice> CameraSourceIntent::RestoreChoice(const QStringList& enumeration) const {
    if (temporaryOwner_.isEmpty()) return {};
    if (beforeTemporary_ && enumeration.contains(beforeTemporary_->symbolicLink)) return CameraRestoreChoice{*beforeTemporary_,false};
    if (!saved_.symbolicLink.isEmpty() && enumeration.contains(saved_.symbolicLink))
        return CameraRestoreChoice{{saved_.symbolicLink,saved_.formatStableId,false},true};
    return {};
}
bool CameraSourceIntent::ReleaseTemporary(const QString& owner) {
    if (temporaryOwner_ != owner || owner.isEmpty()) return false;
    const auto restore = beforeTemporary_.value_or(CameraSourceIdentity{saved_.symbolicLink,saved_.formatStableId,false});
    Request(restore,CameraSourceOrigin::Reconnect);
    temporaryOwner_.clear(); temporaryRequestedLink_.clear(); beforeTemporary_.reset(); return true;
}
} // namespace okuflow::agent
