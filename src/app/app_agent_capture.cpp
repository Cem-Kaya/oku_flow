#ifdef _WIN32
#include "app_internal.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include <QUuid>

namespace okuflow {
QString OkuFlowApp::RequestPhotoCapture(const QString& owner)
{
    if (!controlTarget_ || !cameraActive_ || MediaViewSelected() || !photoOperationId_.isEmpty()) return {};
    photoOperationId_ = controlTarget_->CreatePhoto(owner);
    if (photoOperationId_.isEmpty()) return {};
    photoCapturePending_ = true;
    pipelineOrchestrator_->MarkViewportDirty();
    ShowStatusMessage(QStringLiteral("Capturing original and processed photos..."),2500);
    return photoOperationId_;
}
std::optional<RecordingSessionInfo> OkuFlowApp::RequestRecording(RecordingAudioRequest audio,
    RecordingOwnerKind ownerKind, const QString& ownerId)
{
    if (!recordingManager_ || recordingManager_->IsWorkerAbandoned() || !cameraActive_ || !BeforeRequestRecording()) return {};
    if (recordingManager_->IsActive()) return recordingManager_->CurrentSessionInfo();
    RecordingSessionInfo reservation;
    reservation.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    reservation.ownerKind = ownerKind; reservation.ownerId = ownerId; reservation.requestedAudio = audio;
    // Reserve/admit the exact session BEFORE acquiring the microphone. A
    // denied preflight never steals microphone resources or saved preferences.
    recordingManager_->SetRequested(true,reservation);
    if (!recordingManager_->IsActive()) return {};
    // Selection changes only after the worker admits its first Original frame.
    // Encoder/path failure after preflight must preserve a selected file.
    const auto microphone=audio==RecordingAudioRequest::SavedMicrophone?StartSelectedMicrophone():MicrophoneStartResult::Disabled;
    if(audio==RecordingAudioRequest::None)StopMicrophoneCapture();
    if(microphone!=MicrophoneStartResult::Pending)recordingManager_->SetSessionAudioResult(reservation.id,false);
    return recordingManager_->CurrentSessionInfo();
}
bool OkuFlowApp::StopRecording(const QString& expectedSessionId)
{
    if (!recordingManager_ || expectedSessionId.isEmpty()) return false;
    const auto current = recordingManager_->CurrentSessionInfo();
    if (!current || current->id != expectedSessionId) return false;
    OnVideoRecordingStopping(*current);
    StopMicrophoneCapture(true);
    return recordingManager_->StopSession(expectedSessionId);
}
} // namespace okuflow
#endif
