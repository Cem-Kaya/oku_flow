#ifdef _WIN32
#include "app_internal.hpp"
#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/common/video_audio_output.hpp"
#include "okuflow/common/video_playback.hpp"
#include "okuflow/ui/annotation_overlay.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/video_transport.hpp"
#include <QPointer>
#include <QThreadPool>
#include <QUuid>

namespace okuflow {
namespace {
constexpr std::int64_t kTicksPerMs = 10'000;
QString NormalizedVideoPath(const QString& path)
{
    // Lexical only: existence/canonicalization can block on a cloud/network
    // provider. The native worker opens and proves the held file identity.
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}
bool SameVideoPath(const QString& a, const QString& b)
{
    return !a.isEmpty() && !b.isEmpty() &&
        NormalizedVideoPath(a).compare(NormalizedVideoPath(b), Qt::CaseInsensitive) == 0;
}
}

struct OkuFlowApp::VideoDocumentState {
    enum class Kind { Camera, Recording, File };
    struct Part {
        int index{1};
        std::shared_ptr<RecordingArchive> archive;
        QString path;
        std::int64_t firstCaptureClock100ns{-1}, lastCaptureClock100ns{-1};
        std::vector<RecordingFrameAdmission> admissions;
    };
    VideoPlayback player;
    VideoAudioOutput audio;
    Kind kind{Kind::Camera};
    RecordingSessionInfo session;
    std::vector<Part> parts;
    std::size_t selectedPart{}, framePart{}, presentedPart{};
    std::optional<std::size_t> readerPart;
    std::shared_ptr<const VideoPlaybackFrame> frame, neighbor, presented, presentedNeighbor;
    std::uint64_t requestGeneration{}, holdEpoch{};
    std::uint64_t completedRequestGeneration{}, saveRevision{};
    std::uint64_t saveFrameSequence{}, saveSelectionGeneration{};
    std::optional<std::int64_t> savePts100ns;
    QString documentId, recordingDocumentId, saveDocumentId, saveState{QStringLiteral("idle")};
    bool active{}, finishing{}, followingLive{}, playing{}, muted{true};
    bool pendingHold{}, heldLive{}, pausedDisplayHold{}, resumePending{}, waitingNextPart{}, heldUnrecorded{}, heldMappingUnavailable{};
    std::optional<RecordingFrameAdmission> heldAdmission;
    std::uint64_t heldSequence{};
    std::int64_t heldCaptureClock100ns{-1};
    QString filePath, readerFilePath, pendingOpenPath, waitingSession, openingPath, lastError;
    std::int64_t fileDuration100ns{};
    bool fileHasAudio{}, fileAudioAvailable{};
    bool openingFile{}, restoringFile{}, openingArchive{}, afterOpenEnd{}, savePending{}, beforeOpenPlaying{};
    bool controlsInitialized{}, controlsMediaSelected{};
    std::optional<std::int64_t> afterOpenSeek;
    bool afterOperationPlay{};
    int cameraRotation{}, fileRotation{};
    bool cameraRotationSaved{};
    VideoPlaybackSnapshot playback;

    Part& UpdatePart(int index, const std::shared_ptr<RecordingArchive>& archive,
                     const QString& path = {})
    {
        auto found = std::find_if(parts.begin(), parts.end(),
            [index](const Part& part) { return part.index == index; });
        if (found == parts.end()) {
            parts.push_back({index, archive, path});
            std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.index < b.index; });
            found = std::find_if(parts.begin(), parts.end(), [index](const Part& p) { return p.index == index; });
        }
        if (archive) found->archive = archive;
        if (!path.isEmpty()) found->path = path;
        return *found;
    }
    std::int64_t Offset(std::size_t index) const
    {
        std::int64_t offset{};
        for (std::size_t i = 0; i < index && i < parts.size(); ++i) {
            if (!parts[i].archive) break;
            const auto snapshot = parts[i].archive->Snapshot();
            // Only a finalized preceding part establishes the next origin.
            if (!snapshot.finalized) break;
            offset += std::max<std::int64_t>(0, snapshot.duration100ns);
        }
        return offset;
    }
    std::int64_t End() const
    {
        std::int64_t end{-1};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (!parts[i].archive) continue;
            const auto snapshot = parts[i].archive->Snapshot();
            if (snapshot.seekableEnd100ns >= 0) end = Offset(i) + snapshot.seekableEnd100ns;
        }
        return end;
    }
    bool OpenPart(std::size_t index, std::int64_t localPts, bool play, bool end = false)
    {
        if (index >= parts.size() || !parts[index].archive ||
            parts[index].archive->Snapshot().seekableEnd100ns < 0) return false;
        selectedPart = index;
        afterOperationPlay = play;
        if (!readerPart || *readerPart != index || openingFile) {
            requestGeneration = player.OpenArchive(parts[index].archive);
            readerPart = index;
            openingArchive = true;
            openingFile = false;
            readerFilePath.clear();
            afterOpenSeek = localPts;
            afterOpenEnd = end;
        } else {
            requestGeneration = end ? player.JumpToEnd() : player.Seek100ns(localPts);
            openingArchive = false;
        }
        audio.Reset(requestGeneration);
        return true;
    }
    void RecoverFile(std::int64_t target, bool play, bool end = false)
    {
        openingPath = filePath; readerFilePath = filePath;
        openingFile = restoringFile = true; openingArchive = false; readerPart.reset();
        afterOpenSeek = target; afterOpenEnd = end; afterOperationPlay = play;
        requestGeneration = player.OpenFile(filePath.toStdWString());
        audio.Reset(requestGeneration);
    }
};

void OkuFlowApp::InitializeVideoPlayback()
{
    if (videoDocument_ || !mainWindow_) return;
    videoDocument_ = std::make_shared<VideoDocumentState>();
    videoPlaybackTimer_ = new QTimer(this);
    videoPlaybackTimer_->setInterval(16);
    connect(videoPlaybackTimer_, &QTimer::timeout, this, &OkuFlowApp::PollVideoPlayback);
    connect(mainWindow_.get(), &MainWindow::openVideoRequested, this, [this] {
        const auto path = QFileDialog::getOpenFileName(mainWindow_.get(),
            QCoreApplication::translate("OkuFlow", "Open video"), {},
            QCoreApplication::translate("OkuFlow", "Video files (*.mp4 *.m4v *.mov *.avi *.wmv *.mkv);;All files (*)"));
        if (!path.isEmpty()) OpenVideoFile(path);
    });
    connect(mainWindow_.get(), &MainWindow::videoFileDropped, this, &OkuFlowApp::OpenVideoFile);
    auto* transport = mainWindow_->videoTransport();
    connect(transport, &VideoTransport::SeekRequested, this, &OkuFlowApp::SeekSelectedVideo);
    connect(transport, &VideoTransport::PlayPauseRequested, this, &OkuFlowApp::ToggleSelectedVideoPlayback);
    connect(transport, &VideoTransport::JumpToEndRequested, this, &OkuFlowApp::JumpSelectedVideoToEnd);
    connect(transport, &VideoTransport::ReturnToCameraRequested, this, &OkuFlowApp::ReturnVideoToCamera);
    connect(transport, &VideoTransport::SaveFrameRequested, this, &OkuFlowApp::SaveSelectedVideoFrame);
    connect(transport, &VideoTransport::MutedChanged, this, [this](bool muted) {
        if (!videoDocument_) return;
        // Reviewing a recording is always silent: speaker output must not
        // feed the microphone and playback mute never changes capture mute.
        videoDocument_->muted = videoDocument_->kind == VideoDocumentState::Kind::Recording || muted;
        videoDocument_->player.SetMuted(videoDocument_->muted);
        PollVideoPlayback();
    });
    PollVideoPlayback();
}

void OkuFlowApp::ShutdownVideoPlayback()
{
    if (videoPlaybackTimer_) videoPlaybackTimer_->stop();
    if (videoDocument_) {
        videoDocument_->player.Close();
        videoDocument_->audio.Close();
        videoDocument_.reset(); // native owners cancel without joining
    }
}

void OkuFlowApp::InvalidateMediaSelection(bool preservePendingReadback)
{
    ++mediaDocumentEpoch_;
    mediaSceneCacheValid_ = false;
    mediaScenePixels_.reset();
    mediaUctNeighborSeq_ = 0;
    if (!preservePendingReadback) {
        RetirePendingMediaHoldReadback();
    } else if (videoDocument_ && videoDocument_->pendingHold) {
        videoDocument_->holdEpoch = mediaDocumentEpoch_;
    }
    CancelAnnotationCaptures();
    CancelPendingAssistantCaptures();
    if (controlTarget_) { controlTarget_->CancelObservations(); controlTarget_->CancelPhotoReadback(); }
    if (controlService_) controlService_->SourceChanged();
    if (ultraClearText_) ultraClearText_->ClearImmutableScene();
    if (pipelineOrchestrator_) pipelineOrchestrator_->MarkViewportDirty();
}

bool OkuFlowApp::BeforeRequestRecording() const
{
    return !videoDocument_ ||
        (videoDocument_->waitingSession.isEmpty() && !videoDocument_->finishing);
}

void OkuFlowApp::OnVideoRecordingStarted(const RecordingSessionInfo& session)
{
    if (!videoDocument_) InitializeVideoPlayback();
    if (!videoDocument_) return;
    // This callback is issued only after the first successful Original admission.
    ReturnVideoToCamera();
    auto& doc = *videoDocument_;
    doc.kind = VideoDocumentState::Kind::Recording;
    doc.session = session;
    doc.documentId = doc.recordingDocumentId = QStringLiteral("media-") + QUuid::createUuid().toString(QUuid::Id128);
    doc.parts.clear(); doc.readerPart.reset(); doc.selectedPart = 0;
    doc.active = doc.followingLive = true;
    cameraFramePresented_ = false;
    doc.finishing = doc.playing = doc.pendingHold = doc.heldLive = doc.pausedDisplayHold = doc.resumePending = doc.heldUnrecorded = false;
    doc.waitingNextPart = false;
    doc.heldMappingUnavailable = false;
    doc.muted = true;
    doc.heldAdmission.reset();
    doc.cameraRotation = rotationQuarterTurns_;
    doc.cameraRotationSaved = true;
    doc.lastError.clear();
    videoPlaybackTimer_->start();
    PollVideoPlayback();
}

void OkuFlowApp::OnVideoRecordingStopping(const RecordingSessionInfo& session)
{
    // A preflight reservation is not the selected recording document. Stopping
    // before first admission (or a stale stop) must leave file review intact.
    if (!videoDocument_ || !videoDocument_->active || videoDocument_->session.id != session.id) return;
    auto& doc = *videoDocument_;
    doc.finishing = true; doc.active = false;
    if (doc.kind == VideoDocumentState::Kind::Recording && doc.followingLive) {
        doc.cameraRotation = rotationQuarterTurns_; doc.cameraRotationSaved = true;
        const auto livePresentation = lastAnnotationPresentation_;
        const auto liveGpuPresentation = lastAnnotationGpuPresentation_;
        InvalidateMediaSelection();
        // Capture needs the last successful camera receipt, while cancelled
        // annotation/observation requests must retain no authority.
        lastAnnotationPresentation_ = livePresentation;
        lastAnnotationGpuPresentation_ = liveGpuPresentation;
        doc.pendingHold = true;
        doc.holdEpoch = mediaDocumentEpoch_;
        doc.playing = false;
        const bool retained = CaptureLiveForMediaHold();
        lastAnnotationPresentation_.reset(); lastAnnotationGpuPresentation_.reset();
        doc.followingLive = false;
        if (!retained) {
            doc.pendingHold = false;
            doc.heldUnrecorded = true;
            ShowStatusMessage(QStringLiteral("The displayed frame could not be retained. Recorded video remains available."));
        }
    }
    PollVideoPlayback();
}

void OkuFlowApp::OnVideoSegmentSaved(const SavedRecordingSegment& segment)
{
    if (!videoDocument_ || videoDocument_->session.id != segment.session.id) return;
    auto& doc = *videoDocument_;
    auto& part = doc.UpdatePart(segment.segmentIndex, segment.archive, segment.originalPath);
    part.firstCaptureClock100ns = segment.firstCaptureClock100ns;
    part.lastCaptureClock100ns = segment.lastCaptureClock100ns;
    // Keep archive identity and cursor in place. Reopening the saved path here
    // would invalidate a newer seek and could replace an unrelated document.
    PollVideoPlayback();
}

void OkuFlowApp::OnVideoRecordingEnded(const RecordingSessionInfo& session)
{
    if (!videoDocument_) return;
    auto& doc = *videoDocument_;
    if (doc.session.id == session.id) {
        doc.active = doc.finishing = false;
        doc.session = session;
    }
    if (doc.waitingSession == session.id) {
        doc.waitingSession.clear();
        OpenPendingVideoFile();
    }
    PollVideoPlayback();
}

void OkuFlowApp::OpenVideoFile(const QString& path)
{
    if (!videoDocument_) InitializeVideoPlayback();
    if (!videoDocument_ || path.isEmpty()) return;
    if (!QDir::isAbsolutePath(path)) {
        ShowStatusMessage(QStringLiteral("Video could not be opened: select one existing local file."));
        return;
    }
    auto& doc = *videoDocument_;
    doc.pendingOpenPath = NormalizedVideoPath(path); // newest request owns completion
    if (recordingManager_) {
        const auto current = recordingManager_->CurrentSessionInfo();
        if (current) {
            doc.waitingSession = current->id;
            if (recordingManager_->IsActive()) {
                OnVideoRecordingStopping(*current);
                if (!StopRecording(current->id)) {
                    doc.waitingSession.clear(); doc.pendingOpenPath.clear();
                    ShowStatusMessage(QStringLiteral("Video was not opened because the recording could not be stopped."));
                    return;
                }
            }
            ShowStatusMessage(QStringLiteral("Finishing recording before opening video..."), 2500);
            videoPlaybackTimer_->start();
            return;
        }
    }
    OpenPendingVideoFile();
}

void OkuFlowApp::OpenPendingVideoFile()
{
    if (!videoDocument_) return;
    auto& doc = *videoDocument_;
    if (!doc.waitingSession.isEmpty() || doc.pendingOpenPath.isEmpty()) return;
    doc.openingPath = std::exchange(doc.pendingOpenPath, {});
    // Even a matching label is probed by the native worker. A path can have
    // been replaced; only a held Windows file identity proves own output.
    doc.beforeOpenPlaying = doc.playing;
    doc.player.SetPlaying(false);
    doc.playing = false;
    doc.openingFile = true; doc.openingArchive = false;
    doc.restoringFile = false; doc.readerPart.reset(); doc.readerFilePath = doc.openingPath;
    doc.afterOpenSeek.reset(); doc.afterOpenEnd = doc.afterOperationPlay = false;
    doc.requestGeneration = doc.player.OpenFile(doc.openingPath.toStdWString());
    doc.audio.Reset(doc.requestGeneration);
    doc.lastError.clear();
    videoPlaybackTimer_->start();
    PollVideoPlayback();
}

void OkuFlowApp::ReturnVideoToCamera()
{
    if (!videoDocument_) return;
    auto& doc = *videoDocument_;
    if (doc.followingLive) doc.cameraRotation = rotationQuarterTurns_;
    InvalidateMediaSelection();
    doc.player.Close(); doc.audio.Close();
    doc.requestGeneration = doc.player.Snapshot().requestGeneration;
    doc.completedRequestGeneration = 0;
    doc.frame.reset(); doc.neighbor.reset(); doc.presented.reset(); doc.presentedNeighbor.reset();
    doc.filePath.clear(); doc.openingPath.clear(); doc.pendingOpenPath.clear(); doc.waitingSession.clear();
    doc.openingFile = doc.openingArchive = doc.pendingHold = doc.heldLive = doc.pausedDisplayHold = doc.resumePending = false;
    doc.heldAdmission.reset(); doc.heldUnrecorded = false; doc.playing = false;
    doc.waitingNextPart = false;
    doc.heldMappingUnavailable = false;
    if (doc.cameraRotationSaved) {
        rotationQuarterTurns_ = doc.cameraRotation; UpdateRotationUi();
    }
    doc.cameraRotationSaved = doc.active;
    doc.kind = doc.active ? VideoDocumentState::Kind::Recording : VideoDocumentState::Kind::Camera;
    if (!doc.active) doc.documentId.clear();
    doc.followingLive = doc.active;
    cudaSceneReady_ = cpuSceneReady_ = false; // never present cached file pixels as camera
    uctProvenance_.Reset();
    if (pipelineOrchestrator_) pipelineOrchestrator_->MarkViewportDirty();
    if (!doc.active && videoPlaybackTimer_) videoPlaybackTimer_->stop();
    PollVideoPlayback();
}

bool OkuFlowApp::MediaViewSelected() const
{
    return videoDocument_ && videoDocument_->kind != VideoDocumentState::Kind::Camera &&
        !videoDocument_->followingLive;
}

bool OkuFlowApp::MediaPlaybackPlaying() const
{
    return MediaViewSelected() && (videoDocument_->playing ||
        videoDocument_->afterOperationPlay || videoDocument_->waitingNextPart);
}

int OkuFlowApp::CameraOriginalRotationQuarterTurns() const
{
    return videoDocument_ && videoDocument_->cameraRotationSaved && MediaViewSelected()
        ? videoDocument_->cameraRotation : rotationQuarterTurns_;
}

bool OkuFlowApp::PresentMediaView()
{
    if (!MediaViewSelected() || !videoDocument_->frame) return false;
    auto& doc = *videoDocument_;
    const auto& frame = *doc.frame;
    const bool presented = PresentSelectedMediaPixels(frame.bgra.data(), frame.width, frame.height,
        frame.strideBytes, mediaDocumentEpoch_, frame.sampleId, doc.neighbor.get(), frame.pts100ns,
        MediaPlaybackPlaying());
    if (presented) {
        doc.presented = doc.frame;
        doc.presentedNeighbor = doc.neighbor;
        doc.presentedPart = doc.framePart;
        if (!doc.heldLive && !doc.pendingHold && doc.frame->requestGeneration == doc.requestGeneration)
            doc.completedRequestGeneration = doc.requestGeneration;
    }
    return presented;
}

void OkuFlowApp::AcceptLiveMediaHold(std::vector<std::uint8_t> pixels, UINT width, UINT height,
                                   std::uint64_t sequence, std::int64_t captureClock100ns)
{
    if (!videoDocument_ || !videoDocument_->pendingHold ||
        videoDocument_->holdEpoch != mediaDocumentEpoch_) return;
    auto& doc = *videoDocument_;
    doc.pendingHold = false;
    if (!VideoPlaybackLayoutFits(width, height, static_cast<std::int64_t>(width) * 4) ||
        pixels.size() != static_cast<std::size_t>(width) * height * 4) {
        doc.resumePending = false;
        ShowStatusMessage(QStringLiteral("The displayed frame could not be retained. Recorded video remains available."));
        return;
    }
    const auto& shownView = mediaHoldViewTransform_;
    const bool fitted = shownView.destinationWidth < 0.99999f || shownView.destinationHeight < 0.99999f ||
        shownView.destinationX > 0.00001f || shownView.destinationY > 0.00001f;
    const auto fitMode = fitted ? settings::ViewportFitModeSetting::Fit : settings::ViewportFitModeSetting::Fill;
    const float heldZoom = fitted ? 1.0f : 1.0f / std::max(shownView.sourceWidth, shownView.sourceHeight);
    const float heldCenterX = shownView.sourceX + shownView.sourceWidth * 0.5f;
    const float heldCenterY = shownView.sourceY + shownView.sourceHeight * 0.5f;
    bool exactFraming = shownView.valid && std::isfinite(heldZoom) && heldZoom >= 1.0f &&
        heldZoom <= static_cast<float>(kZoomSliderMaxMultiplier) &&
        shownView.sourceWidth > 0.0f && shownView.sourceHeight > 0.0f;
    if (exactFraming && presenter_ && presenter_->ViewportWidth() && presenter_->ViewportHeight()) {
        const auto reproduced = ComputeViewTransform(width, height, presenter_->ViewportWidth(), presenter_->ViewportHeight(),
            heldZoom, heldCenterX, heldCenterY, fitted ? ViewportFitMode::kFit : ViewportFitMode::kFill);
        const auto same = [](float a, float b) { return std::isfinite(a) && std::isfinite(b) && std::abs(a-b) < 0.00001f; };
        exactFraming = reproduced.valid && same(reproduced.sourceX, shownView.sourceX) &&
            same(reproduced.sourceY, shownView.sourceY) && same(reproduced.sourceWidth, shownView.sourceWidth) &&
            same(reproduced.sourceHeight, shownView.sourceHeight) && same(reproduced.destinationX, shownView.destinationX) &&
            same(reproduced.destinationY, shownView.destinationY) && same(reproduced.destinationWidth, shownView.destinationWidth) &&
            same(reproduced.destinationHeight, shownView.destinationHeight);
    }
    if (!exactFraming) {
        doc.resumePending = false;
        ShowStatusMessage(QStringLiteral("Pause unavailable: the displayed framing could not be retained exactly."));
        return;
    }
    auto frame = std::make_shared<VideoPlaybackFrame>();
    frame->bgra = std::move(pixels); frame->width = width; frame->height = height;
    frame->strideBytes = width * 4; frame->sampleId = sequence;
    frame->requestGeneration = doc.requestGeneration;
    doc.frame = frame; doc.neighbor.reset(); doc.heldLive = true;
    doc.heldSequence = sequence; doc.heldCaptureClock100ns = captureClock100ns;
    doc.heldAdmission.reset(); doc.heldUnrecorded = false;
    doc.heldMappingUnavailable = false;
    rotationQuarterTurns_ = 0; UpdateRotationUi(); // source orientation is baked into Original
    zoomAmount_ = heldZoom; zoomEnabled_ = heldZoom > 1.00001f;
    if (uiState_->zoomCheckbox_) { const QSignalBlocker block(uiState_->zoomCheckbox_); uiState_->zoomCheckbox_->setChecked(zoomEnabled_); }
    if (uiState_->zoomSlider_) { const QSignalBlocker block(uiState_->zoomSlider_); uiState_->zoomSlider_->setValue(static_cast<int>(std::lround(heldZoom*kZoomSliderScale))); }
    SetZoomCenter(heldCenterX, heldCenterY, true, true, false);
    if (pipelineOrchestrator_->ViewportFitMode() != fitMode) SetViewportFitMode(fitMode, false);
    PollVideoPlayback();
}

void OkuFlowApp::SeekSelectedVideo(qint64 positionMs)
{
    if (!videoDocument_) return;
    auto& doc = *videoDocument_;
    if (doc.kind == VideoDocumentState::Kind::Camera || doc.openingFile) return;
    if (doc.followingLive) { doc.cameraRotation = rotationQuarterTurns_; doc.cameraRotationSaved = true; }
    InvalidateMediaSelection();
    doc.followingLive = doc.pendingHold = doc.heldLive = doc.pausedDisplayHold = doc.resumePending = doc.heldUnrecorded = false;
    doc.heldAdmission.reset(); doc.playing = false;
    doc.waitingNextPart = false;
    doc.heldMappingUnavailable = false;
    const auto target = std::clamp<std::int64_t>(positionMs, 0,
        std::numeric_limits<std::int64_t>::max() / kTicksPerMs) * kTicksPerMs;
    if (doc.kind == VideoDocumentState::Kind::File) {
        if (!SameVideoPath(doc.readerFilePath, doc.filePath)) doc.RecoverFile(target, false);
        else { doc.requestGeneration = doc.player.Seek100ns(target); doc.audio.Reset(doc.requestGeneration); }
    } else {
        rotationQuarterTurns_ = 0; UpdateRotationUi();
        bool sought{};
        for (std::size_t i = doc.parts.size(); i-- > 0;) {
            if (!doc.parts[i].archive) continue;
            const auto end = doc.parts[i].archive->Snapshot().seekableEnd100ns;
            const auto offset = doc.Offset(i);
            if (end >= 0 && target >= offset) {
                sought = doc.OpenPart(i, std::min(end, target - offset), false);
                break;
            }
        }
        if (!sought) ShowStatusMessage(QStringLiteral("Waiting for recorded video to become seekable."), 2500);
    }
    PollVideoPlayback();
}

void OkuFlowApp::ToggleSelectedVideoPlayback()
{
    if (!videoDocument_) return;
    auto& doc = *videoDocument_;
    if (doc.openingFile || doc.openingArchive || doc.kind == VideoDocumentState::Kind::Camera) return;
    if (doc.waitingNextPart) { InvalidateMediaSelection(); doc.waitingNextPart = false; doc.playing = false; PollVideoPlayback(); return; }
    if (doc.afterOperationPlay) {
        InvalidateMediaSelection();
        doc.afterOperationPlay = false; doc.playing = false;
        doc.player.SetPlaying(false); doc.audio.Close();
        PollVideoPlayback(); return;
    }
    if (doc.followingLive) {
        doc.cameraRotation = rotationQuarterTurns_; doc.cameraRotationSaved = true;
        const auto livePresentation = lastAnnotationPresentation_;
        const auto liveGpuPresentation = lastAnnotationGpuPresentation_;
        InvalidateMediaSelection();
        lastAnnotationPresentation_ = livePresentation;
        lastAnnotationGpuPresentation_ = liveGpuPresentation;
        doc.pendingHold = true; doc.holdEpoch = mediaDocumentEpoch_;
        doc.playing = false;
        const bool retained = CaptureLiveForMediaHold();
        lastAnnotationPresentation_.reset(); lastAnnotationGpuPresentation_.reset();
        doc.followingLive = false;
        if (!retained) {
            doc.pendingHold = false;
            ShowStatusMessage(QStringLiteral("The displayed frame could not be retained. Recorded video remains available."));
        }
    } else if (doc.pendingHold || doc.resumePending) {
        InvalidateMediaSelection(doc.pendingHold);
        doc.resumePending = !doc.resumePending;
    } else if (doc.playing) {
        InvalidateMediaSelection();
        doc.player.SetPlaying(false); doc.audio.Close(); doc.playing = false;
        doc.pausedDisplayHold = true;
        // The native decoder may be ahead of a deferred presentation. Own the
        // actual displayed frame and reseek it on Resume.
        if (doc.presented) { doc.frame = doc.presented; doc.neighbor = doc.presentedNeighbor; doc.framePart = doc.presentedPart; }
    } else if (doc.heldLive) {
        if (!doc.heldUnrecorded && !doc.heldMappingUnavailable) { InvalidateMediaSelection(); doc.resumePending = true; }
        else ShowStatusMessage(doc.heldUnrecorded
            ? QStringLiteral("Held frame was not recorded. Seek to recorded video to continue.")
            : QStringLiteral("The held frame's recorded position could not be verified. Seek to recorded video to continue."));
    } else if (doc.frame) {
        if (doc.playback.state == VideoPlaybackState::Ended && !doc.active &&
            doc.playback.frame && doc.frame->pts100ns == doc.playback.frame->pts100ns) {
            ShowStatusMessage(QStringLiteral("End of video. Seek to an earlier point to play again."), 4000);
        } else {
            InvalidateMediaSelection();
            doc.pausedDisplayHold = false;
            doc.afterOperationPlay = true;
            if (doc.kind == VideoDocumentState::Kind::Recording) doc.OpenPart(doc.framePart, doc.frame->pts100ns, true);
            else if (!SameVideoPath(doc.readerFilePath, doc.filePath)) doc.RecoverFile(doc.frame->pts100ns, true);
            else { doc.requestGeneration = doc.player.Seek100ns(doc.frame->pts100ns); doc.audio.Reset(doc.requestGeneration); }
        }
    }
    PollVideoPlayback();
}

void OkuFlowApp::JumpSelectedVideoToEnd()
{
    if (!videoDocument_) return;
    auto& doc = *videoDocument_;
    if (doc.kind == VideoDocumentState::Kind::Camera || doc.openingFile) return;
    if (doc.followingLive) { doc.cameraRotation = rotationQuarterTurns_; doc.cameraRotationSaved = true; }
    InvalidateMediaSelection();
    doc.player.SetPlaying(false); doc.audio.Close();
    doc.playing = doc.pendingHold = doc.heldLive = doc.pausedDisplayHold = doc.resumePending = doc.heldUnrecorded = false;
    doc.heldAdmission.reset();
    doc.waitingNextPart = false;
    doc.heldMappingUnavailable = false;
    if (doc.kind == VideoDocumentState::Kind::Recording && doc.active && cameraActive_) {
        doc.player.Close(); doc.readerPart.reset();
        doc.requestGeneration = doc.player.Snapshot().requestGeneration;
        doc.completedRequestGeneration = 0;
        doc.frame.reset(); doc.neighbor.reset(); doc.presented.reset();
        doc.followingLive = true;
        if (doc.cameraRotationSaved) { rotationQuarterTurns_ = doc.cameraRotation; UpdateRotationUi(); }
        cudaSceneReady_ = cpuSceneReady_ = false;
        cameraFramePresented_ = false; // Live waits for a new real successful camera presentation
        if (pipelineOrchestrator_) pipelineOrchestrator_->MarkViewportDirty();
    } else if (doc.kind == VideoDocumentState::Kind::Recording) {
        doc.followingLive = false;
        for (std::size_t i = doc.parts.size(); i-- > 0;)
            if (doc.OpenPart(i, 0, false, true)) break;
        if (doc.active && !cameraActive_)
            ShowStatusMessage(QStringLiteral("Camera unavailable. Showing the latest recorded frame."));
    } else {
        doc.afterOperationPlay = false;
        if (!SameVideoPath(doc.readerFilePath, doc.filePath)) doc.RecoverFile(0, false, true);
        else { doc.requestGeneration = doc.player.JumpToEnd(); doc.audio.Reset(doc.requestGeneration); }
    }
    PollVideoPlayback();
}

void OkuFlowApp::PollVideoPlayback()
{
    if (!videoDocument_ || !mainWindow_) return;
    auto& doc = *videoDocument_;
    if (recordingManager_ && !doc.session.id.isEmpty()) {
        if (const auto timeline = recordingManager_->CurrentTimeline();
            timeline && timeline->session.id == doc.session.id && timeline->archive) {
            auto& part = doc.UpdatePart(timeline->segmentIndex, timeline->archive,
                QString::fromStdWString(timeline->archive->Snapshot().path));
            part.firstCaptureClock100ns = timeline->firstCaptureClock100ns;
            part.lastCaptureClock100ns = timeline->lastCaptureClock100ns;
            part.admissions = timeline->recentAdmissions;
        }
    }
    if (doc.heldLive && !doc.heldAdmission) {
        bool provenSkipped{};
        for (std::size_t i = 0; i < doc.parts.size(); ++i) {
            const auto& part = doc.parts[i];
            const auto found = std::find_if(part.admissions.rbegin(), part.admissions.rend(),
                [&](const RecordingFrameAdmission& a) { return a.sequenceNumber == doc.heldSequence &&
                    a.captureClock100ns == doc.heldCaptureClock100ns; });
            if (found == part.admissions.rend()) {
                // Only an intact admission window bracketing this source
                // frame proves it was skipped. Ring expiry proves nothing.
                if (!part.admissions.empty() && part.admissions.front().sequenceNumber < doc.heldSequence &&
                    part.admissions.back().sequenceNumber > doc.heldSequence &&
                    part.admissions.front().captureClock100ns <= doc.heldCaptureClock100ns &&
                    part.admissions.back().captureClock100ns >= doc.heldCaptureClock100ns) provenSkipped = true;
                continue;
            }
            doc.heldAdmission = *found; doc.selectedPart = i;
            doc.framePart = i;
            doc.heldUnrecorded = doc.heldMappingUnavailable = false;
            // Pixels stay unchanged; this only binds the exact writer receipt.
            break;
        }
        if (!doc.heldAdmission && provenSkipped && !doc.heldUnrecorded) {
            doc.heldUnrecorded = true; doc.resumePending = false;
            ShowStatusMessage(QStringLiteral("Held frame was not recorded. The retained image is still available."));
        } else if (!doc.active && !doc.finishing && !doc.pendingHold && !doc.heldAdmission &&
                   !doc.heldUnrecorded && !doc.heldMappingUnavailable) {
            doc.heldMappingUnavailable = true; doc.resumePending = false;
            ShowStatusMessage(QStringLiteral("The held frame's recorded position could not be verified. The retained image is still available."));
        }
    }
    if (doc.resumePending && doc.heldAdmission && doc.selectedPart < doc.parts.size()) {
        const auto archive = doc.parts[doc.selectedPart].archive;
        if (archive && archive->Snapshot().seekableEnd100ns >= doc.heldAdmission->pts100ns) {
            doc.resumePending = doc.heldLive = false;
            doc.OpenPart(doc.selectedPart, doc.heldAdmission->pts100ns, true);
        } else if (!doc.active && !doc.finishing) {
            doc.resumePending = false; doc.heldUnrecorded = true;
            ShowStatusMessage(QStringLiteral("End of available recording. The retained image is still available."));
        }
    }
    auto snapshot = doc.player.Snapshot();
    doc.playback = snapshot;
    if (snapshot.requestGeneration == doc.requestGeneration) {
        const bool settled = snapshot.state != VideoPlaybackState::Opening &&
            snapshot.state != VideoPlaybackState::Seeking && snapshot.state != VideoPlaybackState::Closed;
        const bool exactFrame = snapshot.frame && snapshot.frame->requestGeneration == doc.requestGeneration;
        if (settled && snapshot.state == VideoPlaybackState::Unavailable) {
            doc.openingArchive = doc.openingFile = false;
            doc.afterOpenSeek.reset(); doc.afterOperationPlay = false; doc.playing = false;
            if (doc.lastError != QString::fromStdString(snapshot.error)) {
                doc.lastError = QString::fromStdString(snapshot.error);
                ShowStatusMessage(QStringLiteral("Video unavailable: %1").arg(doc.lastError));
            }
        } else if (settled && exactFrame) {
            if (doc.openingFile && !doc.restoringFile) {
                const auto ownPart = std::find_if(doc.parts.begin(), doc.parts.end(),
                    [&](const VideoDocumentState::Part& part) {
                        return part.archive && part.archive->Snapshot().file.SameFile(snapshot.file);
                    });
                if (ownPart != doc.parts.end()) {
                    doc.openingFile = false;
                    const auto index = static_cast<std::size_t>(ownPart - doc.parts.begin());
                    if (doc.kind == VideoDocumentState::Kind::Recording) {
                        // A confirmed own-output open selects this document
                        // without replacing its held cursor with frame zero.
                        if (doc.frame && !doc.heldLive && !doc.pendingHold) {
                            doc.OpenPart(doc.framePart, doc.frame->pts100ns, doc.beforeOpenPlaying);
                        } else if (!doc.frame && !doc.pendingHold) doc.OpenPart(index, 0, false);
                        doc.playing = false;
                    } else {
                        InvalidateMediaSelection();
                        doc.kind = VideoDocumentState::Kind::Recording;
                        doc.documentId = doc.recordingDocumentId;
                        doc.followingLive = doc.pendingHold = doc.heldLive = doc.pausedDisplayHold = doc.resumePending = false;
                        doc.playing = false; doc.muted = true;
                        rotationQuarterTurns_ = 0; UpdateRotationUi();
                        doc.OpenPart(index, 0, false);
                    }
                    snapshot = doc.player.Snapshot();
                }
            }
            // Own-output rebinding may have issued a new native Open above.
            // Its ordered seek must wait for that Open's own decoded receipt.
            if (snapshot.frame && snapshot.frame->requestGeneration == doc.requestGeneration &&
                snapshot.requestGeneration == doc.requestGeneration &&
                snapshot.state != VideoPlaybackState::Opening && snapshot.state != VideoPlaybackState::Seeking) {
            const bool wasOpening = doc.openingFile || doc.openingArchive;
            if (doc.openingFile) {
                if (!doc.restoringFile) {
                    InvalidateMediaSelection();
                    if (!doc.cameraRotationSaved) { doc.cameraRotation = rotationQuarterTurns_; doc.cameraRotationSaved = true; }
                    doc.kind = VideoDocumentState::Kind::File;
                    doc.documentId = QStringLiteral("media-") + QUuid::createUuid().toString(QUuid::Id128);
                    doc.filePath = doc.openingPath;
                    doc.fileRotation = static_cast<int>(snapshot.frame->rotationDegrees / 90) % 4;
                    rotationQuarterTurns_ = doc.fileRotation; UpdateRotationUi();
                    doc.followingLive = doc.pendingHold = doc.heldLive = doc.pausedDisplayHold = doc.resumePending = false;
                    doc.muted = false;
                }
                doc.openingFile = false;
                doc.restoringFile = false;
                doc.fileDuration100ns = snapshot.duration100ns;
                doc.fileHasAudio = snapshot.hasAudio; doc.fileAudioAvailable = snapshot.audioAvailable;
                doc.player.SetMuted(doc.muted);
            }
            if (wasOpening) {
                doc.openingArchive = false;
                const auto target = doc.afterOpenSeek.value_or(0);
                doc.afterOpenSeek.reset();
                if (doc.afterOpenEnd || target > 0) {
                    doc.requestGeneration = doc.afterOpenEnd ? doc.player.JumpToEnd() : doc.player.Seek100ns(target);
                    doc.afterOpenEnd = false;
                    doc.audio.Reset(doc.requestGeneration);
                    // Keep the old scene while the ordered seek completes.
                    snapshot = doc.player.Snapshot();
                }
            }
            if (snapshot.requestGeneration == doc.requestGeneration &&
                snapshot.frame && snapshot.frame->requestGeneration == doc.requestGeneration &&
                !doc.followingLive && !doc.heldLive && !doc.pausedDisplayHold && !doc.pendingHold && !doc.openingFile) {
                doc.frame = snapshot.frame; doc.neighbor = snapshot.neighborFrame;
                doc.framePart = doc.selectedPart;
                if (doc.afterOperationPlay) {
                    doc.afterOperationPlay = false;
                    doc.player.SetMuted(doc.kind == VideoDocumentState::Kind::Recording || doc.muted);
                    doc.player.SetPlaying(true);
                    snapshot = doc.player.Snapshot();
                }
                if (doc.playing && snapshot.state == VideoPlaybackState::Ended &&
                    doc.kind == VideoDocumentState::Kind::Recording) doc.waitingNextPart = true;
                // Waiting at a verified active prefix retains Resume intent.
                // The worker resumes automatically when another fragment is ready.
                doc.playing = snapshot.state == VideoPlaybackState::Playing ||
                    snapshot.state == VideoPlaybackState::Waiting;
            }
            }
        }
    }
    if (doc.waitingNextPart) {
        const auto next = doc.selectedPart + 1;
        if (next < doc.parts.size() && doc.parts[next].archive &&
            doc.parts[next].archive->Snapshot().seekableEnd100ns >= 0) {
            InvalidateMediaSelection();
            doc.waitingNextPart = false;
            doc.OpenPart(next, 0, true);
            snapshot = doc.player.Snapshot();
        } else if (!doc.active && !doc.finishing) doc.waitingNextPart = false;
    }
    const bool outputMuted = doc.kind != VideoDocumentState::Kind::File || doc.muted || doc.active || doc.finishing;
    const bool actualPlaying = doc.playing && snapshot.requestGeneration == doc.requestGeneration &&
        snapshot.state == VideoPlaybackState::Playing;
    doc.audio.Configure(doc.requestGeneration, actualPlaying, outputMuted);
    if (!outputMuted && actualPlaying) {
        for (auto& packet : doc.player.TakeAudio()) doc.audio.Push(std::move(packet));
        const auto audio = doc.audio.Snapshot();
        if (audio.available && audio.clockValid && audio.requestGeneration == doc.requestGeneration)
            doc.player.SetAudioClock100ns(audio.renderedPts100ns, audio.sampledAtSteady100ns, audio.requestGeneration);
        else doc.player.SetAudioClock100ns(-1, 0, doc.requestGeneration);
        if (!audio.error.empty() && doc.lastError != QString::fromStdString(audio.error)) {
            doc.lastError = QString::fromStdString(audio.error);
            ShowStatusMessage(QStringLiteral("Playback sound unavailable. Video playback continues."));
        }
    }
    else doc.player.SetAudioClock100ns(-1, 0, doc.requestGeneration);
    if (MediaViewSelected() && doc.frame) PresentMediaView();
    if (doc.followingLive && cameraActive_ && cameraFramePresented_)
        doc.completedRequestGeneration = doc.requestGeneration;

    VideoTransport::Snapshot ui;
    ui.sourceGeneration = mediaDocumentEpoch_;
    ui.muted = outputMuted; ui.playing = doc.playing || doc.waitingNextPart;
    ui.canSaveFrame = MediaViewSelected() && doc.presented && !doc.savePending;
    if (doc.kind == VideoDocumentState::Kind::Recording) {
        ui.source = doc.active || doc.finishing ? VideoTransport::Source::Recording : VideoTransport::Source::Video;
        ui.sourceName = doc.active || doc.finishing
            ? QCoreApplication::translate("OkuFlow", "Current recording")
            : (!doc.parts.empty() ? QFileInfo(doc.parts[doc.selectedPart < doc.parts.size() ? doc.selectedPart : 0].path).fileName()
                                  : QCoreApplication::translate("OkuFlow", "Recording"));
        const auto end = doc.End();
        ui.durationMs = std::max<std::int64_t>(0, end) / kTicksPerMs;
        ui.seekable = end >= 0;
        if (doc.heldLive && doc.heldAdmission) ui.positionMs = (doc.Offset(doc.selectedPart) + doc.heldAdmission->pts100ns) / kTicksPerMs;
        else if (doc.followingLive) ui.positionMs = ui.durationMs; // pending tail is never an accessible range value
        else if (doc.presented) ui.positionMs = (doc.Offset(doc.presentedPart) + doc.presented->pts100ns) / kTicksPerMs;
        ui.state = doc.finishing ? VideoTransport::State::Finishing
            : doc.pendingHold || doc.resumePending || doc.waitingNextPart ? VideoTransport::State::Waiting
            : doc.heldUnrecorded || doc.heldMappingUnavailable ? VideoTransport::State::Unavailable
            : doc.followingLive ? (cameraActive_ && cameraFramePresented_ ? VideoTransport::State::Live : VideoTransport::State::Waiting)
            : doc.playing ? VideoTransport::State::BehindLive : VideoTransport::State::Paused;
        if (!doc.finishing && !doc.followingLive && !doc.heldLive && !doc.pausedDisplayHold && snapshot.state == VideoPlaybackState::Ended)
            ui.state = doc.active ? VideoTransport::State::Waiting : VideoTransport::State::End;
    } else if (doc.kind == VideoDocumentState::Kind::File) {
        ui.source = VideoTransport::Source::Video; ui.sourceName = QFileInfo(doc.filePath).fileName();
        ui.durationMs = std::max<std::int64_t>(0, doc.fileDuration100ns) / kTicksPerMs;
        ui.positionMs = doc.presented ? doc.presented->pts100ns / kTicksPerMs : 0;
        ui.seekable = ui.durationMs > 0;
        ui.hasAudio = doc.fileHasAudio && doc.fileAudioAvailable;
        ui.state = snapshot.state == VideoPlaybackState::Ended && !doc.pausedDisplayHold ? VideoTransport::State::End
            : snapshot.state == VideoPlaybackState::Unavailable ? VideoTransport::State::Unavailable
            : doc.playing ? VideoTransport::State::Video : VideoTransport::State::Paused;
    }
    if (doc.openingFile || doc.openingArchive || !doc.waitingSession.isEmpty()) {
        if (ui.source == VideoTransport::Source::None) ui.source = VideoTransport::Source::Video;
        ui.state = doc.waitingSession.isEmpty() ? VideoTransport::State::Opening : VideoTransport::State::Finishing;
        ui.canSaveFrame = false;
    } else if (snapshot.state == VideoPlaybackState::Seeking && !doc.followingLive) ui.state = VideoTransport::State::Seeking;
    else if (snapshot.requestGeneration == doc.requestGeneration &&
             snapshot.state == VideoPlaybackState::Waiting && doc.playing &&
             !doc.followingLive && !doc.finishing && !doc.heldLive && !doc.pausedDisplayHold && !doc.pendingHold)
        ui.state = VideoTransport::State::Waiting;
    mainWindow_->videoTransport()->SetSnapshot(ui);
    const bool selectedMedia = MediaViewSelected();
    if (!doc.controlsInitialized || doc.controlsMediaSelected != selectedMedia) {
        doc.controlsInitialized = true;
        doc.controlsMediaSelected = selectedMedia;
        UpdateControlEnabledStates();
        UpdateKeystoneTrackingUi();
        UpdateProcessingStatusLabel();
    }
}

void OkuFlowApp::SaveSelectedVideoFrame()
{
    bool presentedForSave{};
    if (videoDocument_ && !videoDocument_->savePending && MediaViewSelected() && videoDocument_->presented) {
        // A decoder-ahead candidate cannot change the frame chosen by this
        // click. Regenerate only the actual last-presented owned scene.
        auto pendingFrame = std::exchange(videoDocument_->frame, videoDocument_->presented);
        auto pendingNeighbor = std::exchange(videoDocument_->neighbor, videoDocument_->presentedNeighbor);
        const auto pendingPart = std::exchange(videoDocument_->framePart, videoDocument_->presentedPart);
        presentedForSave = PresentMediaView();
        videoDocument_->frame = std::move(pendingFrame);
        videoDocument_->neighbor = std::move(pendingNeighbor);
        videoDocument_->framePart = pendingPart;
    }
    if (!videoDocument_ || videoDocument_->savePending || !MediaViewSelected() || !presentedForSave ||
        presentationBuffer_.empty() || !presentationWidth_ || !presentationHeight_) {
        ShowStatusMessage(QStringLiteral("Frame not saved: the selected view is unavailable.")); return;
    }
    QImage image(presentationBuffer_.data(), static_cast<int>(presentationWidth_),
        static_cast<int>(presentationHeight_), static_cast<int>(presentationWidth_) * 4, QImage::Format_ARGB32);
    QImage owned = image.copy();
    if (owned.isNull()) { ShowStatusMessage(QStringLiteral("Frame not saved: the image could not be queued.")); return; }
    {
        QPainter painter(&owned);
        if (auto* overlay = mainWindow_->ultraClearTextOverlay(); overlay && overlay->isVisible() && overlay->width() > 0 && overlay->height() > 0) {
            painter.save();
            painter.scale(static_cast<qreal>(presentationWidth_) / overlay->width(),
                          static_cast<qreal>(presentationHeight_) / overlay->height());
            overlay->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
            painter.restore();
        }
        if (auto* ink = mainWindow_->annotationOverlay(); ink && ink->IsActive() && ink->HasInk() && lastAnnotationPresentation_)
            RenderAnnotationStrokes(painter, ink->Strokes(), lastAnnotationPresentation_->transform,
                QSizeF(presentationWidth_, presentationHeight_));
    }
    if (!userDataPaths_) { ShowStatusMessage(QStringLiteral("Frame not saved: the OkuFlow Photos folder is unavailable.")); return; }
    // These are value snapshots only. Canonicalization, cloud-provider access,
    // directory creation and publication all belong to the image worker.
    const QString configuredRoot = userDataPaths_->ConfiguredRoot();
    const QString installDirectory = QCoreApplication::applicationDirPath();
    const QDateTime saveTime = QDateTime::currentDateTime();
    const QDate saveDate = saveTime.date();
    const QString stem = QStringLiteral("FRAME_%1").arg(saveTime.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    QPointer<OkuFlowApp> owner(this);
    const auto document = videoDocument_;
    // A receipt exists only once there is an exact owned image payload. Invalid
    // clicks leave the previous receipt untouched; queue rejection belongs to
    // this newly admitted identity and can never reuse an older committed save.
    ++document->saveRevision;
    document->savePending = true;
    document->saveState = QStringLiteral("writing");
    document->saveDocumentId = document->documentId;
    document->saveFrameSequence = document->presented->sampleId;
    document->saveSelectionGeneration = mediaDocumentEpoch_;
    document->savePts100ns.reset();
    if (document->heldLive && document->heldAdmission)
        document->savePts100ns = document->Offset(document->selectedPart) + document->heldAdmission->pts100ns;
    else if (!document->heldLive)
        document->savePts100ns = (document->kind == VideoDocumentState::Kind::Recording ? document->Offset(document->presentedPart) : 0) + document->presented->pts100ns;
    const bool queued = imageIoPool_ && imageIoPool_->tryStart([owner, document, configuredRoot, installDirectory, saveDate, stem, owned = std::move(owned)]() mutable {
        AnnotationWriteResult saved;
        QString outputError;
        try {
            const UserDataPaths paths(configuredRoot, installDirectory);
            const QString directory = paths.PhotosForDate(saveDate, &outputError);
            if (!directory.isEmpty()) saved = UserDataPaths::WriteAnnotationImage(directory, stem,
                [&owned](QIODevice* output) { return owned.save(output, "PNG"); });
            else if (outputError.isEmpty()) outputError = QStringLiteral("Frame not saved: the OkuFlow Photos folder is unavailable.");
        } catch (const std::exception&) {
            outputError = QStringLiteral("The selected video frame could not be saved.");
        }
        if (!owner) return;
        QMetaObject::invokeMethod(owner, [owner, document, saved, outputError] {
            document->savePending = false;
            document->saveState = saved.committed ? QStringLiteral("committed") : QStringLiteral("failed");
            if (!owner) return;
            owner->ShowStatusMessage(saved.committed ? QStringLiteral("Saved selected video frame.")
                : !outputError.isEmpty() ? outputError : QStringLiteral("The selected video frame could not be saved."));
            owner->PollVideoPlayback();
        }, Qt::QueuedConnection);
    });
    if (!queued) {
        document->savePending = false;
        document->saveState = QStringLiteral("failed");
        ShowStatusMessage(QStringLiteral("Frame not saved: the image writer is busy. Try again."));
    }
}

bool OkuFlowApp::SetSelectedVideoPlaying(bool playing)
{
    if (!videoDocument_ || videoDocument_->kind == VideoDocumentState::Kind::Camera) return false;
    auto& doc = *videoDocument_;
    if (doc.openingFile || doc.openingArchive) {
        if (!playing) { InvalidateMediaSelection(); doc.afterOperationPlay = false; doc.playing = false; doc.audio.Close(); return true; }
        return doc.afterOperationPlay;
    }
    const bool current = doc.followingLive || doc.playing || doc.resumePending || doc.waitingNextPart || doc.afterOperationPlay;
    if (current == playing) return true;
    ToggleSelectedVideoPlayback();
    return playing ? (doc.playing || doc.resumePending || doc.afterOperationPlay)
                   : (!doc.followingLive && !doc.playing && !doc.resumePending);
}

QJsonObject OkuFlowApp::SelectedVideoState() const
{
    if (!videoDocument_) return {{"kind", "camera"}, {"state", "camera"}, {"hasFrame", false}};
    const auto& doc = *videoDocument_;
    const auto snapshot = doc.player.Snapshot();
    const char* kind = doc.kind == VideoDocumentState::Kind::Camera ? "camera"
        : doc.kind == VideoDocumentState::Kind::Recording ? "recording" : "file";
    const char* state = doc.kind == VideoDocumentState::Kind::Camera ? "camera"
        : doc.finishing ? "finishing" : doc.pendingHold ? "holding"
        : doc.resumePending || doc.waitingNextPart ? "waiting" : doc.openingFile || doc.openingArchive ? "opening"
        : doc.followingLive ? (cameraActive_ && cameraFramePresented_ ? "live" : "waiting")
        : snapshot.state == VideoPlaybackState::Seeking ? "seeking"
        : doc.heldUnrecorded ? "held_unrecorded"
        : doc.heldMappingUnavailable ? "unavailable"
        : snapshot.state == VideoPlaybackState::Unavailable ? "unavailable"
        : snapshot.requestGeneration == doc.requestGeneration && snapshot.state == VideoPlaybackState::Waiting && doc.playing ? "waiting"
        : snapshot.state == VideoPlaybackState::Ended && !doc.pausedDisplayHold ? (doc.active ? "waiting" : "end")
        : doc.playing ? "playing" : "paused";
    const auto end = doc.kind == VideoDocumentState::Kind::Recording ? doc.End() : doc.fileDuration100ns;
    const bool liveFrame = doc.followingLive && cameraActive_ && cameraFramePresented_ && lastAnnotationPresentation_.has_value();
    const auto liveSequence = liveFrame && lastAnnotationPresentation_
        ? lastAnnotationPresentation_->frameSeq : 0;
    QJsonObject result{{"kind", kind}, {"state", state}, {"documentId", doc.documentId},
        {"selectionGeneration", static_cast<qint64>(mediaDocumentEpoch_)},
        {"requestGeneration", static_cast<qint64>(doc.requestGeneration)},
        {"completedRequestGeneration", static_cast<qint64>(doc.completedRequestGeneration)},
        {"playing", doc.followingLive || doc.playing || doc.resumePending || doc.waitingNextPart},
        {"frameSequence", static_cast<qint64>(liveFrame ? liveSequence : doc.presented ? doc.presented->sampleId : 0)},
        {"hasFrame", liveFrame || (MediaViewSelected() && bool(doc.presented))},
        {"canSaveFrame", MediaViewSelected() && bool(doc.presented) && !doc.savePending},
        {"pendingHold", doc.pendingHold}, {"heldUnrecorded", doc.heldUnrecorded},
        {"heldMappingUnavailable", doc.heldMappingUnavailable},
        {"seekableEnd100ns", static_cast<qint64>(end)},
        {"saveRevision", static_cast<qint64>(doc.saveRevision)}, {"saveState", doc.saveState},
        {"saveDocumentId", doc.saveDocumentId}, {"saveFrameSequence", static_cast<qint64>(doc.saveFrameSequence)},
        {"saveSelectionGeneration", static_cast<qint64>(doc.saveSelectionGeneration)},
        {"savePts100ns", doc.savePts100ns ? QJsonValue(static_cast<qint64>(*doc.savePts100ns)) : QJsonValue(QJsonValue::Null)}};
    if (doc.heldLive && doc.heldAdmission) {
        result["selectedPts100ns"] = static_cast<qint64>(doc.Offset(doc.selectedPart) + doc.heldAdmission->pts100ns);
    } else if (liveFrame && recordingManager_) {
        result["selectedPts100ns"] = QJsonValue::Null;
        if (const auto timeline = recordingManager_->CurrentTimeline(); timeline && timeline->session.id == doc.session.id) {
            const auto& displayed = presentedOriginalCache_.Displayed();
            const auto admitted = displayed && displayed->cameraEpoch == cameraSessionId_ &&
                displayed->recordingSessionId == doc.session.id &&
                displayed->original->identity.sequenceNumber == liveSequence
                ? timeline->FindAdmission(liveSequence,displayed->original->identity.captureClock100ns)
                : std::optional<RecordingFrameAdmission>{};
            if (admitted) {
                const auto part = std::find_if(doc.parts.begin(), doc.parts.end(),
                    [&](const VideoDocumentState::Part& p) { return p.index == timeline->segmentIndex; });
                if (part != doc.parts.end()) result["selectedPts100ns"] = static_cast<qint64>(
                    doc.Offset(static_cast<std::size_t>(part - doc.parts.begin())) + admitted->pts100ns);
                result["provisionalCaptureClock100ns"] = static_cast<qint64>(admitted->captureClock100ns);
            }
        }
    } else if (doc.presented && !doc.heldLive) {
        result["selectedPts100ns"] = static_cast<qint64>((doc.kind == VideoDocumentState::Kind::Recording ? doc.Offset(doc.presentedPart) : 0) + doc.presented->pts100ns);
    } else result["selectedPts100ns"] = QJsonValue::Null;
    if (doc.heldLive) result["provisionalCaptureClock100ns"] = static_cast<qint64>(doc.heldCaptureClock100ns);
    result["sourceName"] = doc.kind == VideoDocumentState::Kind::File ? QFileInfo(doc.filePath).fileName()
        : doc.kind == VideoDocumentState::Kind::Recording ? QStringLiteral("Current recording") : QString();
    return result;
}

#if defined(OKUFLOW_VIDEO_DOCUMENT_TESTS)
void OkuFlowApp::SeedVideoDocumentForTest(const RecordingSessionInfo& session,
    std::shared_ptr<const VideoPlaybackFrame> frame, bool recording, bool playing)
{
    if (!videoDocument_) InitializeVideoPlayback();
    ReturnVideoToCamera();
    auto& doc = *videoDocument_;
    doc.kind = recording ? VideoDocumentState::Kind::Recording : VideoDocumentState::Kind::File;
    doc.session = session; doc.active = recording; doc.finishing = doc.followingLive = false;
    doc.parts.clear(); doc.selectedPart = doc.framePart = doc.presentedPart = 0; doc.readerPart.reset();
    doc.documentId = QStringLiteral("media-") + QUuid::createUuid().toString(QUuid::Id128);
    doc.recordingDocumentId = doc.documentId;
    doc.frame = doc.presented = std::move(frame); doc.neighbor.reset();
    doc.pausedDisplayHold = true; doc.playing = playing;
    doc.filePath = QStringLiteral("C:/private-fixture/lecture.mp4");
    doc.fileDuration100ns = 90'000'000;
    videoPlaybackTimer_->start();
}

QString OkuFlowApp::PendingVideoOpenForTest() const
{
    return videoDocument_ ? videoDocument_->pendingOpenPath : QString();
}
std::shared_ptr<const VideoPlaybackFrame> OkuFlowApp::HeldVideoFrameForTest() const
{
    return videoDocument_ && videoDocument_->heldLive ? videoDocument_->frame : nullptr;
}
void OkuFlowApp::SeedCommittedVideoSaveForTest()
{
    if (!videoDocument_ || !videoDocument_->presented) return;
    auto& doc = *videoDocument_;
    ++doc.saveRevision;
    doc.savePending = false;
    doc.saveState = QStringLiteral("committed");
    doc.saveDocumentId = doc.documentId;
    doc.saveFrameSequence = doc.presented->sampleId;
    doc.saveSelectionGeneration = mediaDocumentEpoch_;
    doc.savePts100ns = doc.presented->pts100ns;
}
#endif
} // namespace okuflow
#endif
