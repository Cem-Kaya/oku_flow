#ifdef _WIN32
#include "app_internal.hpp"
#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/common/video_audio_output.hpp"
#include "okuflow/common/video_playback.hpp"
#include "okuflow/common/video_recap.hpp"
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
#if OKUFLOW_ENABLE_CUDA
std::uint64_t PackedAdapter(const LUID& luid)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
}
#endif
}

struct OkuFlowApp::VideoDocumentState {
    enum class Kind { Camera, Recap, Recording, File };
    struct Part {
        int index{1};
        std::shared_ptr<RecordingArchive> archive;
        QString path;
        std::int64_t firstCaptureClock100ns{-1}, lastCaptureClock100ns{-1};
        std::vector<RecordingFrameAdmission> admissions;
    };
    VideoPlayback player;
    RamVideoRecap recap;
    VideoRecapSnapshot recapSnapshot;
    std::shared_ptr<const VideoRecapSegment> recapReader, recapFrame, recapPresented;
    std::optional<std::pair<std::int64_t,bool>> recapDeferredSeek;
    QString recapDocumentId;
    std::uint64_t recapEpoch{}, recapSourceEpoch{};
    GUID recapSubtype{GUID_NULL}; UINT recapWidth{},recapHeight{},recapRateN{},recapRateD{};
    int recapRotation{-1};
    bool recapUnsupported{};
    struct CameraView {float zoom{1},x{0.5f},y{0.5f};bool enabled{};
        settings::ViewportFitModeSetting fit{settings::ViewportFitModeSetting::Fill};};
    std::optional<CameraView> cameraView;
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
    bool controlsInitialized{}, controlsMediaSelected{}, controlsMediaGpuAvailable{};
    bool gpuFallbackPending{};
    // Transport identity follows the selected document only. Seeks, pauses
    // and backend fallback advance the selection epoch, never this value.
    std::uint64_t transportGeneration{};
    QString transportDocumentId;
    // Newest seek target (document ms) until that request's frame is shown,
    // so repeated relative seeks accumulate from the requested position.
    std::optional<std::int64_t> pendingSeekMs;
    std::optional<std::int64_t> afterOpenSeek;
    bool afterOperationPlay{};
    int cameraRotation{}, fileRotation{};
    bool cameraRotationSaved{};
    VideoPlaybackSnapshot playback;
    struct GpuSave {
        GpuVideoFrame image;
        QElapsedTimer deadline;
        UINT64 readback{};
    };
    std::optional<GpuSave> gpuSave;
    UINT64 retiredGpuSaveReadback{};

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
    if(settingsController_)videoDocument_->recap.SetDurationMinutes(settingsController_->Settings().liveRecapMinutes);
    connect(mainWindow_.get(),&MainWindow::liveRecapMinutesChanged,this,[this](int minutes) {
        settingsController_->MutableSettings().liveRecapMinutes=std::clamp(minutes,1,5);
        if(videoDocument_) {
            auto& d=*videoDocument_;d.recap.SetDurationMinutes(minutes);
            const auto window=d.recap.Snapshot();
            if(d.kind==VideoDocumentState::Kind::Recap && !d.followingLive && d.presented && d.recapPresented &&
                d.recapPresented->begin100ns+d.presented->pts100ns<window.first100ns) {
                // Keep the exact displayed lease as a bounded paused hold
                // outside the shorter seek window, never relabelled as first.
                if(!d.player.PauseAtFrame(d.presented))d.player.SetPlaying(false);
                d.frame=d.presented;d.recapFrame=d.recapPresented;d.neighbor=d.presentedNeighbor;
                d.playing=d.afterOperationPlay=d.waitingNextPart=false;d.pausedDisplayHold=true;
            }
        }
        // A history preference must not persist the temporary replay zoom,
        // camera selector or framing as the user's startup camera intent.
        const auto config=settingsController_->Settings().currentConfig;
        if(!settingsController_->Save(config))ShowStatusMessage(QStringLiteral("OkuFlow could not save settings."));
        PollVideoPlayback();
    });
    videoPlaybackTimer_ = new QTimer(this);
    videoPlaybackTimer_->setInterval(16);
    connect(videoPlaybackTimer_, &QTimer::timeout, this, &OkuFlowApp::PollVideoPlayback);
    connect(mainWindow_.get(), &MainWindow::openVideoRequested, this, [this] {
        const auto path = QFileDialog::getOpenFileName(mainWindow_.get(),
            QCoreApplication::translate("OkuFlow", "Open video"), {},
            QCoreApplication::translate("OkuFlow", "Video files (*.mp4 *.m4v *.mov *.avi *.wmv *.mkv);;All files (*)"));
        if (!path.isEmpty()) OpenVideoFile(path);
        else PollVideoPlayback(); // Cancel keeps the actual viewed source selected.
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
    mediaGpuAnalysis_.reset();mediaGpuNeighborAnalysis_.reset();
    if(mediaGpuScene_)mediaGpuScene_->Invalidate();
    if(mediaGpuNeighborScene_)mediaGpuNeighborScene_->Invalidate();
}

bool OkuFlowApp::LiveRecapActive() const { return videoDocument_ && cameraActive_; }
bool OkuFlowApp::LiveRecapWantsFrame(UINT w,UINT h,std::int64_t clock) const {
    return LiveRecapActive() && videoDocument_->recap.WantsFrame(w,h,clock);
}
QString OkuFlowApp::LiveOriginalRetentionId() const {
    if(recordingManager_)if(const auto session=recordingManager_->CurrentSessionInfo())return session->id;
    return videoDocument_?videoDocument_->recapDocumentId:QString();
}
void OkuFlowApp::ResetLiveRecap() {
    if(!videoDocument_)return;auto& d=*videoDocument_;
    d.recap.Reset(++d.recapSourceEpoch,-1);d.recapWidth=d.recapHeight=0;d.recapRotation=-1;
    d.recapDocumentId.clear();presentedCameraScene_.reset();presentedCameraCapture_.reset();presentedOriginalCache_.Reset();
    if(d.kind==VideoDocumentState::Kind::Recap) {
        ReturnVideoToCamera();if(videoPlaybackTimer_)videoPlaybackTimer_->stop();
    }
}
void OkuFlowApp::ObserveLiveRecapSource(const MediaFrame& frame) {
    if(!videoDocument_ || !cameraActive_)return;auto& d=*videoDocument_;
    const int rotation=CameraOriginalRotationQuarterTurns();
    if(d.recapSubtype!=frame.subtype || d.recapWidth!=frame.width || d.recapHeight!=frame.height ||
        d.recapRotation!=rotation || d.recapRateN!=frame.frameRateNumerator || d.recapRateD!=frame.frameRateDenominator) {
        ResetLiveRecap();d.recapSubtype=frame.subtype;d.recapWidth=frame.width;d.recapHeight=frame.height;
        d.recapRotation=rotation;d.recapRateN=frame.frameRateNumerator;d.recapRateD=frame.frameRateDenominator;
        const auto policy=VideoRecapPolicy::ForSize(frame.width,frame.height);
        d.recapUnsupported=!policy;
        mainWindow_->setLiveRecapEstimate(policy?policy->bitrate:0);
        d.recap.Reset(++d.recapSourceEpoch,rotation);
        d.recapDocumentId=QStringLiteral("recap-")+QUuid::createUuid().toString(QUuid::Id128);
    }
    if(d.kind==VideoDocumentState::Kind::Camera) {
        d.kind=VideoDocumentState::Kind::Recap;d.documentId=d.recapDocumentId;d.followingLive=true;
    }
    if(videoPlaybackTimer_ && !videoPlaybackTimer_->isActive())videoPlaybackTimer_->start();
}
void OkuFlowApp::SubmitLiveRecapOriginal(const CapturedFrame& original) {
    if(!LiveRecapActive() || !original.IsValid() ||
        !LiveRecapWantsFrame(original.width,original.height,original.identity.captureClock100ns))return;
    VideoRecapFrame frame;frame.width=original.width;frame.height=original.height;frame.identity=original.identity;
    if(original.HasGpuScene())frame.gpu=original.gpuScene;
    else try{frame.pixels=std::make_shared<const std::vector<std::uint8_t>>(original.pixels);}catch(const std::bad_alloc&){return;}
    videoDocument_->recap.Submit(std::move(frame));
}
bool OkuFlowApp::OpenLiveRecapAt(std::int64_t target,bool play) {
    if(!videoDocument_)return false;auto& d=*videoDocument_;
    const auto range=d.recap.Snapshot();
    if(!range.available || range.first100ns<0 || range.end100ns<range.first100ns)return false;
    target=std::clamp(target,range.first100ns,range.end100ns);
    auto segment=d.recapReader && d.recapReader->epoch==range.epoch &&
        target>=d.recapReader->begin100ns && target<=d.recapReader->end100ns
        ? d.recapReader:d.recap.Acquire(target);
    if(!segment) {
        // Two pins can be occupied by the exact displayed scene and a reader
        // being superseded. Cancel its generation, keep only the newest intent,
        // and retry after native retirement frees the second bounded pin.
        d.recapDeferredSeek={{target,play}};d.pendingSeekMs=target/kTicksPerMs;
        d.player.Close();d.requestGeneration=d.player.Snapshot().requestGeneration;
        d.recapReader.reset();d.openingArchive=false;d.afterOperationPlay=d.playing=false;
        d.frame=d.presented;d.recapFrame=d.recapPresented;d.neighbor=d.presentedNeighbor;
        return true;
    }
    d.recapDeferredSeek.reset();
    if(!d.cameraView) d.cameraView=VideoDocumentState::CameraView{zoomAmount_,zoomCenterX_,zoomCenterY_,zoomEnabled_,pipelineOrchestrator_->ViewportFitMode()};
    if(d.followingLive){d.cameraRotation=rotationQuarterTurns_;d.cameraRotationSaved=true;}
    InvalidateMediaSelection();mediaHoldBakedScene_=false;
    d.followingLive=d.pendingHold=d.heldLive=d.pausedDisplayHold=d.resumePending=d.waitingNextPart=false;
    d.playing=false;d.pendingSeekMs=target/kTicksPerMs;d.afterOperationPlay=play;
    d.kind=VideoDocumentState::Kind::Recap;d.documentId=d.recapDocumentId;
    rotationQuarterTurns_=0;UpdateRotationUi();
    const auto local=std::max<std::int64_t>(0,target-segment->begin100ns);
    if(!d.recapReader || d.recapReader->id!=segment->id || d.recapReader->epoch!=segment->epoch) {
        d.requestGeneration=d.player.OpenMemory(segment->source,segment);d.recapReader=std::move(segment);
        d.openingArchive=true;d.afterOpenSeek=local;
    } else d.requestGeneration=d.player.Seek100ns(local);
    d.audio.Close();d.player.SetMuted(true);return true;
}

void OkuFlowApp::PollLiveRecap() {
    auto& d=*videoDocument_;d.recapSnapshot=d.recap.Snapshot();
    const auto& r=d.recapSnapshot;
    const bool changedEpoch=d.recapEpoch && r.epoch!=d.recapEpoch;
    d.recapEpoch=r.epoch;
    if(changedEpoch && !d.followingLive){ReturnVideoToCamera();return;}
    if(d.recapDeferredSeek) {
        const auto intent=*d.recapDeferredSeek;
        // Avoid repeatedly cancelling the reader while its existing close is
        // retiring. Acquire is non-native and obeys the same two-pin quota.
        if(r.pinnedSegments<VideoRecapPolicy::kPinnedSegments)OpenLiveRecapAt(intent.first,intent.second);
    }
    if(d.resumePending && d.heldLive && r.available && r.captureOrigin100ns>=0 && d.heldCaptureClock100ns>=0) {
        // Resume begins at the first genuine coded sample after the manual
        // freeze action. The uncompressed exact scene is never relabelled as
        // a lossy decoder sample or offered as its corroborating neighbour.
        const auto target=std::max<std::int64_t>(0,d.heldCaptureClock100ns-r.captureOrigin100ns);
        if(target<=r.end100ns)OpenLiveRecapAt(std::max(target,r.first100ns),true);
    }
    auto snapshot=d.player.Snapshot();d.playback=snapshot;
    if(snapshot.requestGeneration==d.requestGeneration && !d.followingLive && !d.heldLive && !d.pendingHold) {
        if(snapshot.state==VideoPlaybackState::Unavailable) {
            d.openingArchive=false;d.afterOpenSeek.reset();d.afterOperationPlay=d.playing=false;
        } else if(snapshot.frame && snapshot.frame->requestGeneration==d.requestGeneration &&
            snapshot.state!=VideoPlaybackState::Opening && snapshot.state!=VideoPlaybackState::Seeking) {
            if(d.openingArchive) {
                d.openingArchive=false;const auto target=d.afterOpenSeek.value_or(0);d.afterOpenSeek.reset();
                if(target>0){d.requestGeneration=d.player.Seek100ns(target);snapshot=d.player.Snapshot();}
            }
            if(snapshot.frame && snapshot.frame->requestGeneration==d.requestGeneration) {
                if(!d.pausedDisplayHold){d.frame=snapshot.frame;d.neighbor=snapshot.neighborFrame;d.recapFrame=d.recapReader;}
                else if(snapshot.frame==d.frame)d.neighbor=snapshot.neighborFrame;
                if(d.afterOperationPlay){d.afterOperationPlay=false;d.player.SetPlaying(true);snapshot=d.player.Snapshot();}
                const bool wasPlaying=d.playing;
                d.playing=snapshot.state==VideoPlaybackState::Playing;
                if(snapshot.state==VideoPlaybackState::Ended && !d.pausedDisplayHold && d.playing==false &&
                    d.recapReader && (d.waitingNextPart || wasPlaying))d.waitingNextPart=true;
            }
        }
    }
    if(d.waitingNextPart && d.recapReader) {
        if(r.end100ns>d.recapReader->end100ns)OpenLiveRecapAt(d.recapReader->end100ns+1,true);
        else if(cameraActive_ && r.available)JumpSelectedVideoToEnd();
    }
    if(MediaViewSelected() && d.frame && !d.recapDeferredSeek &&
        (d.heldLive || d.frame->requestGeneration==d.requestGeneration))PresentMediaView();
    if(d.pendingSeekMs && d.completedRequestGeneration==d.requestGeneration && !d.openingArchive &&
        !d.heldLive && !d.pendingHold && !d.recapDeferredSeek)d.pendingSeekMs.reset();
    VideoTransport::Snapshot ui;ui.source=VideoTransport::Source::Recap;
    ui.sourceName=QCoreApplication::translate("OkuFlow","Live recap");
    ui.diagnosticDetail=QString::fromStdString(r.error);
    if(d.transportDocumentId!=d.documentId){d.transportDocumentId=d.documentId;++d.transportGeneration;}
    ui.sourceGeneration=d.transportGeneration;ui.playing=d.followingLive || d.playing || d.resumePending || d.afterOperationPlay || d.waitingNextPart;
    ui.firstMs=std::max<std::int64_t>(0,r.first100ns)/kTicksPerMs;
    ui.durationMs=std::max<std::int64_t>(0,r.end100ns)/kTicksPerMs;ui.seekable=r.available;
    ui.canPauseLive=d.followingLive && cameraFramePresented_ && presentedCameraScene_ && presentedCameraScene_->captureClock100ns>=0;
    ui.canSaveFrame=MediaViewSelected() && d.presented && !d.savePending;
    const auto liveCapture=presentedCameraCapture_?presentedCameraCapture_->clock100ns:-1;
    ui.positionMs=d.followingLive && liveCapture>=0 && r.captureOrigin100ns>=0?std::max<std::int64_t>(0,liveCapture-r.captureOrigin100ns)/kTicksPerMs
        : d.presented && d.recapPresented?(d.recapPresented->begin100ns+d.presented->pts100ns)/kTicksPerMs
        : d.pendingSeekMs.value_or(ui.firstMs);
    ui.state=d.pendingHold || d.resumePending || (!r.available && !d.heldLive) ? VideoTransport::State::Buffering
        : d.recapDeferredSeek ? VideoTransport::State::Seeking
        : d.openingArchive ? VideoTransport::State::Opening
        : snapshot.state==VideoPlaybackState::Seeking && !d.heldLive ? VideoTransport::State::Seeking
        : !r.error.empty() && !r.available ? VideoTransport::State::Unavailable
        : d.followingLive ? (cameraFramePresented_?VideoTransport::State::Live:VideoTransport::State::Buffering)
        : d.playing ? VideoTransport::State::BehindLive : VideoTransport::State::Paused;
    if((!r.error.empty() || d.recapUnsupported) && !r.available && !d.heldLive)ui.state=VideoTransport::State::Unavailable;
    if(snapshot.state==VideoPlaybackState::Unavailable && !d.followingLive && !d.heldLive)ui.state=VideoTransport::State::Unavailable;
    mainWindow_->videoTransport()->SetSnapshot(ui);
    mainWindow_->setVideoSourceSelected(MediaViewSelected(),selectedCameraIndex_);
    const bool selected=MediaViewSelected(),gpu=SelectedMediaGpuProcessingAvailable();
    if(!d.controlsInitialized || d.controlsMediaSelected!=selected || d.controlsMediaGpuAvailable!=gpu) {
        d.controlsInitialized=true;d.controlsMediaSelected=selected;d.controlsMediaGpuAvailable=gpu;
        UpdateControlEnabledStates();UpdatePhoneLightUi();UpdateKeystoneTrackingUi();UpdateProcessingStatusLabel();
    }
}

void OkuFlowApp::InvalidateMediaSelection(bool preservePendingReadback)
{
    ++mediaDocumentEpoch_;
    mediaSceneCacheValid_ = false;
    mediaScenePixels_.reset();
    mediaGpuAnalysis_.reset();mediaGpuNeighborAnalysis_.reset();
    mediaGpuAnalysisRetryAtMs_=0;
    mediaGpuAnalysisAttempts_ = 0;
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
            ShowStatusMessage(QStringLiteral("Pause unavailable: the displayed frame could not be retained."));
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
            // StopRecording validates this exact session before it marks the
            // document finishing. Its only refusal after this observation is a
            // concurrent terminal transition, whose queued end callback still
            // opens the file. Only a session that keeps recording is a failure.
            if (recordingManager_->IsActive() && !StopRecording(current->id)) {
                const auto still = recordingManager_->CurrentSessionInfo();
                if (still && still->id == current->id && recordingManager_->IsActive()) {
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
    doc.gpuFallbackPending=false;
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
    doc.pendingSeekMs.reset();
    // Open captures backend intent. Selecting the adapter only in the later
    // timer poll would leave the first file on CPU until its next reopen.
    doc.player.SetGpuAdapter(SelectedMediaGpuAdapter());
    doc.requestGeneration = doc.player.OpenFile(doc.openingPath.toStdWString());
    doc.audio.Reset(doc.requestGeneration);
    doc.lastError.clear();
    videoPlaybackTimer_->start();
    PollVideoPlayback();
}

void OkuFlowApp::ReturnVideoToCamera()
{
    if (!videoDocument_) return;
    const bool wasMedia=MediaViewSelected();
    // The private media scene never becomes the live camera's cached output.
    // Retire its CUDA/native resources on workers while the camera resumes.
    if(mediaGpuScene_)mediaGpuScene_->Invalidate();
    if(mediaGpuNeighborScene_)mediaGpuNeighborScene_->Invalidate();
    auto& doc = *videoDocument_;
    mediaHoldBakedScene_=false;
    if (doc.followingLive) doc.cameraRotation = rotationQuarterTurns_;
    InvalidateMediaSelection();
    doc.player.Close(); doc.audio.Close();
    doc.requestGeneration = doc.player.Snapshot().requestGeneration;
    doc.completedRequestGeneration = 0;
    doc.frame.reset(); doc.neighbor.reset(); doc.presented.reset(); doc.presentedNeighbor.reset();
    doc.filePath.clear(); doc.openingPath.clear(); doc.pendingOpenPath.clear(); doc.waitingSession.clear();
    doc.gpuFallbackPending=false;
    doc.pendingSeekMs.reset();
    doc.openingFile = doc.openingArchive = doc.pendingHold = doc.heldLive = doc.pausedDisplayHold = doc.resumePending = false;
    doc.heldAdmission.reset(); doc.heldUnrecorded = false; doc.playing = false;
    doc.waitingNextPart = false;
    doc.heldMappingUnavailable = false;
    if (doc.cameraRotationSaved) {
        rotationQuarterTurns_ = doc.cameraRotation; UpdateRotationUi();
    }
    doc.cameraRotationSaved = doc.active;
    doc.recapReader.reset();doc.recapFrame.reset();doc.recapPresented.reset();
    doc.recapDeferredSeek.reset();
    if(doc.cameraView) {
        zoomAmount_=doc.cameraView->zoom;zoomEnabled_=doc.cameraView->enabled;
        SetZoomCenter(doc.cameraView->x,doc.cameraView->y,true,true,false);
        if(pipelineOrchestrator_->ViewportFitMode()!=doc.cameraView->fit)SetViewportFitMode(doc.cameraView->fit,false);
        if(uiState_->zoomSlider_){const QSignalBlocker b(uiState_->zoomSlider_);uiState_->zoomSlider_->setValue(int(std::lround(zoomAmount_*kZoomSliderScale)));}
        if(uiState_->zoomCheckbox_){const QSignalBlocker b(uiState_->zoomCheckbox_);uiState_->zoomCheckbox_->setChecked(zoomEnabled_);}
        doc.cameraView.reset();
    }
    doc.kind = doc.active ? VideoDocumentState::Kind::Recording : cameraActive_ && !doc.recapDocumentId.isEmpty()
        ? VideoDocumentState::Kind::Recap : VideoDocumentState::Kind::Camera;
    if (!doc.active) doc.documentId.clear();
    if(doc.kind==VideoDocumentState::Kind::Recap)doc.documentId=doc.recapDocumentId;
    doc.followingLive = doc.active || doc.kind==VideoDocumentState::Kind::Recap;
    if(wasMedia){cameraFramePresented_=false;presentedCameraScene_.reset();presentedCameraCapture_.reset();}
    cudaSceneReady_ = cpuSceneReady_ = false; // never present cached file pixels as camera
    uctProvenance_.Reset();
    if (pipelineOrchestrator_) pipelineOrchestrator_->MarkViewportDirty();
    if (!doc.active && !cameraActive_ && videoPlaybackTimer_) videoPlaybackTimer_->stop();
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

bool OkuFlowApp::MediaGpuViewSelected() const {
    // Image actions and diagnostics describe the last successful display,
    // which may differ from a decoded successor awaiting GPU admission.
    return MediaViewSelected() && videoDocument_ && videoDocument_->presented && mediaPresentedWithCuda_;
}

bool OkuFlowApp::SelectedMediaGpuProcessingAvailable() const {
    // Decode can fall back to host pixels independently of CUDA processing.
    // Native media controls may be edited while the private scene initializes.
    return MediaViewSelected() && !mediaHoldBakedScene_ && videoDocument_ && videoDocument_->frame &&
        SelectedMediaGpuAdapter().has_value();
}

std::optional<LUID> OkuFlowApp::SelectedMediaGpuAdapter() const
{
#if OKUFLOW_ENABLE_CUDA
    if (!presenter_ || !presenter_->GetDevice() || presenter_->IsFaulted() ||
        (cudaSurface_ && cudaSurface_->IsFaulted())) return std::nullopt;
    const LUID adapter = presenter_->GetDevice()->GetAdapterLuid();
    const auto packed = PackedAdapter(adapter);
    if (std::any_of(mediaCudaFailedAdapters_.begin(), mediaCudaFailedAdapters_.end(),
            [packed](const MediaCudaAdapterFailure& failure) { return failure.adapter == packed; }))
        return std::nullopt;
    return adapter;
#else
    return std::nullopt;
#endif
}

void OkuFlowApp::MarkMediaCudaAdapterUnavailable(const std::string& reason)
{
#if OKUFLOW_ENABLE_CUDA
    if (presenter_ && presenter_->GetDevice()) {
        const auto packed = PackedAdapter(presenter_->GetDevice()->GetAdapterLuid());
        if (std::none_of(mediaCudaFailedAdapters_.begin(), mediaCudaFailedAdapters_.end(),
                [packed](const MediaCudaAdapterFailure& failure) { return failure.adapter == packed; })) {
            mediaCudaFailedAdapters_.push_back({packed, reason});
            qWarning() << "Selected media uses CPU compatibility on this adapter for this session:"
                       << QString::fromStdString(reason);
        }
    }
#else
    (void)reason;
#endif
    if (videoDocument_) videoDocument_->player.SetGpuAdapter(SelectedMediaGpuAdapter());
}

void OkuFlowApp::RequestMediaCpuFallback(const std::string& reason)
{
    // Only an actual app-side media CUDA scene failure reaches here. Decoder
    // incompatibility is retried on CPU by the playback worker itself, and
    // pressure or revoked archive bytes are never a backend verdict.
    MarkMediaCudaAdapterUnavailable(reason);
    if (!videoDocument_ || videoDocument_->gpuFallbackPending) return;
    auto& doc = *videoDocument_;
    // An unfinished open, seek or live hold owns the decoder cursor. Replacing
    // it would discard that selection; its own GPU result requests this again.
    const bool settled = doc.pausedDisplayHold ||
        (doc.frame && doc.frame->requestGeneration == doc.requestGeneration);
    if (!doc.frame || !settled || doc.openingFile || doc.openingArchive || doc.pendingHold || doc.heldLive) return;
    doc.gpuFallbackPending = true;
    const bool resume = doc.playing || doc.afterOperationPlay;
    // Re-decode the frame actually shown: the decoder may be ahead of a
    // deferred presentation, and a paused hold owns its displayed point. Only
    // a current selection not yet shown (seek, jump, open) targets doc.frame.
    const bool useShown = !doc.pausedDisplayHold && doc.presented &&
        doc.completedRequestGeneration == doc.requestGeneration;
    const auto anchor = useShown ? doc.presented : doc.frame;
    const auto part = useShown ? doc.presentedPart : doc.framePart;
    InvalidateMediaSelection();
    if(mediaGpuScene_)mediaGpuScene_->Invalidate();
    if(mediaGpuNeighborScene_)mediaGpuNeighborScene_->Invalidate();
    bool issued{};
    if (doc.kind == VideoDocumentState::Kind::Recording) {
        // The adapter is CPU-only now, so opening another part decodes on CPU.
        if (doc.readerPart && *doc.readerPart == part) doc.player.UseCpuFallback(reason);
        issued = doc.OpenPart(part, anchor->pts100ns, resume);
    } else if(doc.kind==VideoDocumentState::Kind::Recap && doc.recapFrame) {
        doc.player.UseCpuFallback(reason);
        issued=OpenLiveRecapAt(doc.recapFrame->begin100ns+anchor->pts100ns,resume);
    } else if (SameVideoPath(doc.readerFilePath, doc.filePath)) {
        doc.player.UseCpuFallback(reason);
        doc.requestGeneration = doc.player.Seek100ns(anchor->pts100ns);
        doc.audio.Reset(doc.requestGeneration);
        doc.afterOperationPlay = resume;
        issued = true;
    } else {
        doc.RecoverFile(anchor->pts100ns, resume);
        issued = true;
    }
    if (!issued) {
        doc.requestGeneration = doc.player.UseCpuFallback(reason);
        doc.audio.Reset(doc.requestGeneration);
        doc.afterOperationPlay = resume;
    }
    // The CPU result at the shown point becomes the selection, including for
    // a paused hold, whose exact-frame ownership this re-decode preserves.
    doc.pausedDisplayHold = false;
    doc.playing = false;
    qWarning()<<"Selected video uses CPU compatibility:"<<QString::fromStdString(reason);
}

int OkuFlowApp::CameraOriginalRotationQuarterTurns() const
{
    return videoDocument_ && videoDocument_->cameraRotationSaved && MediaViewSelected()
        ? videoDocument_->cameraRotation : rotationQuarterTurns_;
}

std::int64_t OkuFlowApp::SelectedMediaDocumentPts100ns(std::int64_t localPts100ns) const {
    if(videoDocument_ && videoDocument_->kind==VideoDocumentState::Kind::Recap)
        return videoDocument_->heldLive ? -1 : videoDocument_->recapFrame
            ? videoDocument_->recapFrame->begin100ns+localPts100ns : -1;
    if (localPts100ns < 0 || !videoDocument_ ||
        videoDocument_->kind != VideoDocumentState::Kind::Recording) return localPts100ns;
    const auto& doc = *videoDocument_;
    // A live hold is retained before its recording admission may arrive. Its
    // default frame PTS is not a verified recording position.
    if (doc.heldLive) {
        if (!doc.heldAdmission) return -1;
        localPts100ns = doc.heldAdmission->pts100ns;
    }
    const auto offset = doc.Offset(doc.heldLive ? doc.selectedPart : doc.framePart);
    return localPts100ns >= 0 && offset <= std::numeric_limits<std::int64_t>::max() - localPts100ns
        ? offset + localPts100ns : -1;
}

bool OkuFlowApp::PresentMediaView()
{
    if (!MediaViewSelected() || !videoDocument_->frame) return false;
    auto& doc = *videoDocument_;
    const auto& frame = *doc.frame;
    const bool presented = doc.heldLive && mediaHoldBakedScene_ ? PresentFrozenMediaScene() :
        (frame.gpu || SelectedMediaGpuProcessingAvailable())
        ? PresentSelectedMediaGpuFrame(frame,doc.neighbor.get(),MediaPlaybackPlaying()):
        frame.bgra && PresentSelectedMediaPixels(frame.bgra->data(), frame.width, frame.height,
            frame.strideBytes, mediaDocumentEpoch_, frame.sampleId, doc.neighbor.get(), frame.pts100ns,MediaPlaybackPlaying());
    if (presented) {
        doc.presented = doc.frame;
        doc.presentedNeighbor = doc.neighbor;
        doc.presentedPart = doc.framePart;
        if(doc.kind==VideoDocumentState::Kind::Recap)doc.recapPresented=doc.recapFrame;
        if (!doc.heldLive && !doc.pendingHold && doc.frame->requestGeneration == doc.requestGeneration)
            doc.completedRequestGeneration = doc.requestGeneration;
    }
    return presented;
}

bool OkuFlowApp::PresentFrozenMediaScene() {
    if(!videoDocument_ || !videoDocument_->frame || !videoDocument_->frame->bgra || !presenter_ ||
        !uiState_->renderWidget_ || !uiState_->renderWidget_->isPresenterReady())return false;
    const auto& f=*videoDocument_->frame;
    const auto width=presenter_->ViewportWidth(),height=presenter_->ViewportHeight();
    if(!width || !height)return false;
    const auto& held=mediaHoldViewTransform_;
    const float originalZoom=1.0f/std::max(held.sourceWidth,held.sourceHeight);
    const bool unmoved=rotationQuarterTurns_==0 && std::abs(zoomAmount_-originalZoom)<0.00001f &&
        std::abs(zoomCenterX_-(held.sourceX+held.sourceWidth*0.5f))<0.00001f &&
        std::abs(zoomCenterY_-(held.sourceY+held.sourceHeight*0.5f))<0.00001f;
    const bool swapped=(rotationQuarterTurns_&1)!=0;
    const auto transform=unmoved?held:ComputeViewTransform(swapped?f.height:f.width,swapped?f.width:f.height,width,height,zoomEnabled_?zoomAmount_:1.0f,
        zoomCenterX_,zoomCenterY_,pipelineOrchestrator_->ViewportFitMode()==settings::ViewportFitModeSetting::Fit?ViewportFitMode::kFit:ViewportFitMode::kFill);
    if(!transform.valid)return false;
    MediaViewportAdmission::Stamp stamp;
    stamp.recipe[0]=mediaDocumentEpoch_;stamp.recipe[1]=f.sampleId;stamp.transform=transform;stamp.width=width;stamp.height=height;
    stamp.recipe[2]=rotationQuarterTurns_;
    // The live camera keeps processing behind this baked scene. Its dirty
    // flag cannot change held pixels; this scene's own stamp covers view edits.
    if(!mediaViewportAdmission_.NeedsPresentation(stamp,false,presenter_->NeedsScenePresent()) &&
        lastAnnotationPresentation_ && !presentationBuffer_.empty())return true;
    auto recipe=mediaHoldDisplayRecipe_;recipe.rotationQuarterTurns=rotationQuarterTurns_;
    if(!ComposeFrozenCameraScene(*f.bgra,f.width,f.height,transform,width,height,recipe,presentationBuffer_))return false;
    if(ultraClearText_)ultraClearText_->SetMediaOverlaySuppressed(true);
    presenter_->Present(presentationBuffer_.data(),width,height);
    if(presenter_->IsFaulted() || presenter_->NeedsScenePresent())return false;
    presentationWidth_=width;presentationHeight_=height;
    UctFrameContext context;context.geometry.settings.sourceEpoch=mediaDocumentEpoch_;
    context.geometry.sceneWidth=f.width;context.geometry.sceneHeight=f.height;context.requested=context.geometry.settings;
    context.frameSeq=f.sampleId;context.transform=transform;context.viewportWidth=width;context.viewportHeight=height;
    context.compositeFrames=true;lastAnnotationPresentation_=context;lastAnnotationGpuPresentation_.reset();
    annotationPresentationCuda_=false;mediaPresentedWithCuda_=false;mediaSuperResPresented_=false;
    mainWindow_->setAnnotationViewTransform(transform);mediaViewportAdmission_.Presented(stamp);
    pipelineOrchestrator_->MarkViewportPresented();return true;
}

bool OkuFlowApp::AcceptLiveMediaHold(std::vector<std::uint8_t> pixels, UINT width, UINT height,
                                   std::uint64_t sequence, std::int64_t captureClock100ns)
{
    if (!videoDocument_ || !videoDocument_->pendingHold ||
        videoDocument_->holdEpoch != mediaDocumentEpoch_) return false;
    auto& doc = *videoDocument_;
    doc.pendingHold = false;
    if (!VideoPlaybackLayoutFits(width, height, static_cast<std::int64_t>(width) * 4) ||
        pixels.size() != static_cast<std::size_t>(width) * height * 4 ||
        (doc.kind==VideoDocumentState::Kind::Recap && captureClock100ns<0)) {
        doc.resumePending = false;
        // A failed manual hold must not abandon a still-recording live view.
        // Finishing recordings retain their separate unavailable receipt.
        doc.followingLive = (doc.active && doc.kind == VideoDocumentState::Kind::Recording) ||
            (cameraActive_ && doc.kind==VideoDocumentState::Kind::Recap);
        doc.heldUnrecorded = !doc.followingLive;
        ShowStatusMessage(QStringLiteral("Pause unavailable: the displayed frame could not be retained."));
        return false;
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
    if (exactFraming && !mediaHoldBakedScene_ && presenter_ && presenter_->ViewportWidth() && presenter_->ViewportHeight()) {
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
        doc.followingLive = (doc.active && doc.kind == VideoDocumentState::Kind::Recording) ||
            (cameraActive_ && doc.kind==VideoDocumentState::Kind::Recap);
        doc.heldUnrecorded = !doc.followingLive;
        ShowStatusMessage(QStringLiteral("Pause unavailable: the displayed framing could not be retained exactly."));
        return false;
    }
    auto frame = std::make_shared<VideoPlaybackFrame>();
    frame->bgra = std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels)); frame->width = width; frame->height = height;
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
    return true;
}

void OkuFlowApp::SeekSelectedVideo(qint64 positionMs)
{
    if (!videoDocument_) return;
    auto& doc = *videoDocument_;
    if(doc.kind==VideoDocumentState::Kind::Recap){OpenLiveRecapAt(std::clamp<std::int64_t>(positionMs,0,INT64_MAX/kTicksPerMs)*kTicksPerMs,false);PollVideoPlayback();return;}
    if (doc.kind == VideoDocumentState::Kind::Camera || doc.openingFile) return;
    if (doc.followingLive) { doc.cameraRotation = rotationQuarterTurns_; doc.cameraRotationSaved = true; }
    InvalidateMediaSelection();
    doc.followingLive = doc.pendingHold = doc.heldLive = doc.pausedDisplayHold = doc.resumePending = doc.heldUnrecorded = false;
    doc.heldAdmission.reset(); doc.playing = false;
    doc.waitingNextPart = false;
    doc.heldMappingUnavailable = false;
    const auto target = std::clamp<std::int64_t>(positionMs, 0,
        std::numeric_limits<std::int64_t>::max() / kTicksPerMs) * kTicksPerMs;
    doc.pendingSeekMs.reset();
    if (doc.kind == VideoDocumentState::Kind::File) {
        if (!SameVideoPath(doc.readerFilePath, doc.filePath)) doc.RecoverFile(target, false);
        else { doc.requestGeneration = doc.player.Seek100ns(target); doc.audio.Reset(doc.requestGeneration); }
        doc.pendingSeekMs = (doc.fileDuration100ns > 0 ? std::min(target, doc.fileDuration100ns) : target) / kTicksPerMs;
    } else {
        rotationQuarterTurns_ = 0; UpdateRotationUi();
        bool sought{};
        for (std::size_t i = doc.parts.size(); i-- > 0;) {
            if (!doc.parts[i].archive) continue;
            const auto end = doc.parts[i].archive->Snapshot().seekableEnd100ns;
            const auto offset = doc.Offset(i);
            if (end >= 0 && target >= offset) {
                sought = doc.OpenPart(i, std::min(end, target - offset), false);
                if (sought) doc.pendingSeekMs = (offset + std::min(end, target - offset)) / kTicksPerMs;
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
    if(doc.kind==VideoDocumentState::Kind::Recap && !doc.followingLive) {
        if(doc.pendingHold || doc.heldLive){doc.resumePending=!doc.resumePending;PollVideoPlayback();return;}
        if(doc.playing || doc.waitingNextPart || doc.afterOperationPlay) {
            InvalidateMediaSelection();doc.playing=doc.waitingNextPart=doc.afterOperationPlay=false;doc.pausedDisplayHold=true;
            if(doc.presented){doc.frame=doc.presented;doc.recapFrame=doc.recapPresented;}
            doc.neighbor.reset();doc.presentedNeighbor.reset();
            if(!doc.player.PauseAtFrame(doc.frame))doc.player.SetPlaying(false);
        } else if(doc.frame && doc.recapFrame)OpenLiveRecapAt(doc.recapFrame->begin100ns+doc.frame->pts100ns,true);
        PollVideoPlayback();return;
    }
    if (doc.waitingNextPart) { InvalidateMediaSelection(); doc.waitingNextPart = false; doc.playing = false; PollVideoPlayback(); return; }
    if (doc.afterOperationPlay) {
        InvalidateMediaSelection();
        doc.afterOperationPlay = false; doc.playing = false;
        doc.player.SetPlaying(false); doc.audio.Close();
        PollVideoPlayback(); return;
    }
    if (doc.followingLive) {
        if(!doc.cameraView)doc.cameraView=VideoDocumentState::CameraView{zoomAmount_,zoomCenterX_,zoomCenterY_,zoomEnabled_,pipelineOrchestrator_->ViewportFitMode()};
        if(doc.kind==VideoDocumentState::Kind::Recap) {
            const auto r=doc.recap.Snapshot();
            if(r.captureOrigin100ns>=0 && presentedCameraCapture_ && presentedCameraCapture_->clock100ns>=0)
                doc.pendingSeekMs=std::max<std::int64_t>(0,
                    presentedCameraCapture_->clock100ns-r.captureOrigin100ns)/kTicksPerMs;
            else doc.pendingSeekMs.reset();
        }
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
            doc.followingLive = true;
            lastAnnotationPresentation_ = livePresentation;
            lastAnnotationGpuPresentation_ = liveGpuPresentation;
            ShowStatusMessage(QStringLiteral("Pause unavailable: the displayed frame could not be retained."));
        }
    } else if (doc.pendingHold || doc.resumePending) {
        InvalidateMediaSelection(doc.pendingHold);
        doc.resumePending = !doc.resumePending;
    } else if (doc.playing) {
        InvalidateMediaSelection();
        doc.audio.Close(); doc.playing = false;
        doc.pausedDisplayHold = true;
        // The native decoder may be ahead of a deferred presentation. Own the
        // actual displayed frame and reseek it on Resume.
        if (doc.presented) { doc.frame = doc.presented; doc.framePart = doc.presentedPart; }
        // A predecessor can belong to the other side of a scene cut. Only
        // the decoder's frame-bound following receipt may corroborate Pause.
        doc.neighbor.reset(); doc.presentedNeighbor.reset();
        if (!doc.player.PauseAtFrame(doc.frame)) doc.player.SetPlaying(false);
    } else if (doc.heldLive) {
        if (!doc.heldUnrecorded && !doc.heldMappingUnavailable) { InvalidateMediaSelection(); doc.resumePending = true; }
        else ShowStatusMessage(doc.heldUnrecorded
            ? QStringLiteral("Held frame was not recorded. Seek to recorded video to continue.")
            : QStringLiteral("The held frame's recorded position could not be verified. Seek to recorded video to continue."));
    } else if (doc.pendingSeekMs && (!doc.frame || doc.frame->requestGeneration != doc.requestGeneration)) {
        // Resume continues from the pending seek target, never the older frame
        // still displayed. That request starts playback once its frame lands.
        doc.afterOperationPlay = true;
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
    if(doc.kind==VideoDocumentState::Kind::Recap){ReturnVideoToCamera();return;}
    if (doc.kind == VideoDocumentState::Kind::Camera || doc.openingFile) return;
    if (doc.followingLive) { doc.cameraRotation = rotationQuarterTurns_; doc.cameraRotationSaved = true; }
    InvalidateMediaSelection();
    doc.player.SetPlaying(false); doc.audio.Close();
    doc.playing = doc.pendingHold = doc.heldLive = doc.pausedDisplayHold = doc.resumePending = doc.heldUnrecorded = false;
    doc.heldAdmission.reset();
    doc.waitingNextPart = false;
    doc.heldMappingUnavailable = false;
    doc.pendingSeekMs.reset();
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
    ExpireAnnotationReadback();
    auto& doc = *videoDocument_;
    doc.player.SetGpuAdapter(SelectedMediaGpuAdapter());
    if(doc.kind==VideoDocumentState::Kind::Recap && !doc.openingFile && doc.waitingSession.isEmpty()){PollLiveRecap();return;}
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
            // The playback worker already retried genuine GPU incompatibility
            // on CPU. Pressure, deadlines and revoked archive bytes are not a
            // backend verdict, so the GPU path stays available for later work.
            doc.openingArchive = doc.openingFile = false;
            doc.afterOpenSeek.reset(); doc.afterOperationPlay = false; doc.playing = false;
            doc.pendingSeekMs.reset();
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
                        doc.frame.reset(); doc.neighbor.reset();
                        doc.presented.reset(); doc.presentedNeighbor.reset();
                        doc.completedRequestGeneration = 0;
                        mediaPresentedWithCuda_ = mediaSuperResPresented_ = false;
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
                    mediaHoldBakedScene_=false;
                    doc.documentId = QStringLiteral("media-") + QUuid::createUuid().toString(QUuid::Id128);
                    // The preceding document may remain physically visible until
                    // this frame is submitted, but cannot authorize a new-file
                    // Save/observation or inherit the new document's identity.
                    doc.presented.reset(); doc.presentedNeighbor.reset();
                    doc.completedRequestGeneration = 0;
                    mediaPresentedWithCuda_ = mediaSuperResPresented_ = false;
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
            if (doc.pausedDisplayHold && snapshot.requestGeneration == doc.requestGeneration &&
                snapshot.frame == doc.frame && snapshot.frame &&
                snapshot.frame->requestGeneration == doc.requestGeneration &&
                !doc.followingLive && !doc.heldLive && !doc.pendingHold && !doc.openingFile) {
                // Ancillary evidence may arrive after the exact displayed
                // frame was frozen. Never adopt a decoder-ahead frame here.
                const auto& neighbor = snapshot.neighborFrame;
                doc.neighbor = neighbor && neighbor->requestGeneration == doc.requestGeneration &&
                    neighbor->sampleId != doc.frame->sampleId && neighbor->pts100ns > doc.frame->pts100ns &&
                    neighbor->pts100ns - doc.frame->pts100ns <= 2'500'000
                    ? neighbor : std::shared_ptr<const VideoPlaybackFrame>{};
            }
            if (snapshot.requestGeneration == doc.requestGeneration &&
                snapshot.frame && snapshot.frame->requestGeneration == doc.requestGeneration &&
                !doc.followingLive && !doc.heldLive && !doc.pausedDisplayHold && !doc.pendingHold && !doc.openingFile) {
                doc.frame = snapshot.frame; doc.neighbor = snapshot.neighborFrame;
                if(!snapshot.frame->gpu)doc.gpuFallbackPending=false;
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
    // A seek target is shown until the frame of its final ordered request has
    // a successful presentation (an archive open may still owe its seek).
    if (doc.pendingSeekMs && doc.completedRequestGeneration == doc.requestGeneration &&
        !doc.openingFile && !doc.openingArchive && !doc.afterOpenSeek && !doc.afterOpenEnd)
        doc.pendingSeekMs.reset();
    if (doc.transportDocumentId != doc.documentId) {
        doc.transportDocumentId = doc.documentId;
        ++doc.transportGeneration;
    }

    VideoTransport::Snapshot ui;
    ui.sourceGeneration = doc.transportGeneration;
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
        else if (doc.pendingSeekMs) ui.positionMs = *doc.pendingSeekMs;
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
        ui.positionMs = doc.pendingSeekMs ? *doc.pendingSeekMs
            : doc.presented ? doc.presented->pts100ns / kTicksPerMs : 0;
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
    const bool videoSource = MediaViewSelected() || doc.openingFile || !doc.waitingSession.isEmpty();
    const bool sourceUiChanged = mainWindow_->videoSourceSelected() != videoSource;
    mainWindow_->setVideoSourceSelected(videoSource, selectedCameraIndex_);
    const bool selectedMedia = MediaViewSelected();
    const bool mediaGpuAvailable = selectedMedia && SelectedMediaGpuProcessingAvailable();
    if (!doc.controlsInitialized || sourceUiChanged || doc.controlsMediaSelected != selectedMedia ||
        doc.controlsMediaGpuAvailable != mediaGpuAvailable) {
        doc.controlsInitialized = true;
        doc.controlsMediaSelected = selectedMedia;
        doc.controlsMediaGpuAvailable = mediaGpuAvailable;
        UpdateControlEnabledStates();
        UpdatePhoneLightUi();
        UpdateKeystoneTrackingUi();
        UpdateProcessingStatusLabel();
    }
}

bool OkuFlowApp::PresentSelectedMediaFrameForCapture()
{
    if (!videoDocument_ || !MediaViewSelected() || !videoDocument_->presented) return false;
    // Decoder-ahead candidates never retarget a click from the displayed frame.
    auto pendingFrame = std::exchange(videoDocument_->frame, videoDocument_->presented);
    auto pendingNeighbor = std::exchange(videoDocument_->neighbor, videoDocument_->presentedNeighbor);
    const auto pendingPart = std::exchange(videoDocument_->framePart, videoDocument_->presentedPart);
    const bool presented = PresentMediaView();
    videoDocument_->frame = std::move(pendingFrame);
    videoDocument_->neighbor = std::move(pendingNeighbor);
    videoDocument_->framePart = pendingPart;
    return presented;
}

void OkuFlowApp::BeginSelectedVideoSaveReceipt()
{
    auto& doc = *videoDocument_;
    ++doc.saveRevision;
    doc.savePending = true;
    doc.saveState = QStringLiteral("writing");
    doc.saveDocumentId = doc.documentId;
    doc.saveFrameSequence = doc.presented->sampleId;
    doc.saveSelectionGeneration = mediaDocumentEpoch_;
    doc.savePts100ns.reset();
    if (doc.heldLive && doc.heldAdmission)
        doc.savePts100ns = doc.Offset(doc.selectedPart) + doc.heldAdmission->pts100ns;
    else if (!doc.heldLive)
        doc.savePts100ns = (doc.kind == VideoDocumentState::Kind::Recording ? doc.Offset(doc.presentedPart) : 0) + doc.presented->pts100ns;
    if(doc.kind==VideoDocumentState::Kind::Recap && !doc.heldLive && doc.recapPresented)
        doc.savePts100ns=doc.recapPresented->begin100ns+doc.presented->pts100ns;
}

void OkuFlowApp::SaveSelectedVideoFrame()
{
    if (!videoDocument_ || videoDocument_->savePending || !PresentSelectedMediaFrameForCapture() ||
        !presentationWidth_ || !presentationHeight_) {
        ShowStatusMessage(QStringLiteral("Frame not saved: the selected view is unavailable.")); return;
    }
    const bool gpu = MediaGpuViewSelected();
    QImage owned;
    if (gpu) {
        owned = QImage(presentationWidth_, presentationHeight_, QImage::Format_ARGB32_Premultiplied);
        if (!owned.isNull()) owned.fill(Qt::transparent);
    } else if (!presentationBuffer_.empty()) {
        owned = QImage(presentationBuffer_.data(), presentationWidth_, presentationHeight_,
            presentationWidth_*4, QImage::Format_ARGB32).copy();
    }
    if (owned.isNull()) { ShowStatusMessage(QStringLiteral("Frame not saved: the image could not be queued.")); return; }
    {
        QPainter painter(&owned);
        if (auto* overlay = mainWindow_->ultraClearTextOverlay()) {
            const auto transform = lastAnnotationPresentation_
                ? lastAnnotationPresentation_->transform : overlay->CurrentViewTransform();
            overlay->CaptureSnapshot(transform, overlay->size()).Paint(painter, owned.size());
        }
        if (auto* ink = mainWindow_->annotationOverlay(); ink && ink->IsActive() && ink->HasInk() && lastAnnotationPresentation_)
            RenderAnnotationStrokes(painter, ink->Strokes(), lastAnnotationPresentation_->transform,
                QSizeF(presentationWidth_, presentationHeight_));
    }
    if (!gpu) { QueueSelectedVideoFrameImage(std::move(owned)); return; }
    if (!mediaGpuScene_ || !mediaGpuScene_->SceneReady() || !lastAnnotationGpuPresentation_ || !presenter_) {
        ShowStatusMessage(QStringLiteral("Frame not saved: the selected view is unavailable.")); return;
    }
    owned=owned.convertToFormat(QImage::Format_ARGB32); // shader expects straight alpha
    const auto& shown = *lastAnnotationGpuPresentation_;
    const auto& view = shown.transform;
    const RecordingViewTransform transform{view.sourceX,view.sourceY,view.sourceWidth,view.sourceHeight,
        view.destinationX,view.destinationY,view.destinationWidth,view.destinationHeight,view.valid};
    const auto sync = mediaGpuScene_->PresentationSync();
    ViewportPresentationOptions options{};options.drawFocusMarker=shown.focusMarker;
    options.focusX=shown.focusX;options.focusY=shown.focusY;
    UINT64 readSignal{};
    auto snapshot = presenter_->RequestRecordingFrame(static_cast<ID3D12Resource*>(const_cast<void*>(shown.texture)),shown.width,shown.height,
        transform,presentationWidth_,presentationHeight_,owned.constBits(),owned.bytesPerLine(),
        nullptr,0,&sync,&readSignal,&options);
    if (readSignal) mediaGpuScene_->RecordPresented(readSignal);
    if (!snapshot.IsValid()) {
        ShowStatusMessage(QStringLiteral("Frame not saved: the image could not be queued.")); return;
    }
    // The owned GPU clone is the exact clicked payload. Later presentation,
    // seeking, resize or source selection cannot overwrite it before PNG export.
    BeginSelectedVideoSaveReceipt();
    videoDocument_->gpuSave.emplace();
    videoDocument_->gpuSave->image=std::move(snapshot);
    videoDocument_->gpuSave->deadline.start();
    PollSelectedVideoSave();
}

void OkuFlowApp::PollSelectedVideoSave()
{
    if (!videoDocument_ || !videoDocument_->gpuSave || !presenter_) return;
    auto& doc=*videoDocument_;
    auto& save=*doc.gpuSave;
    if (save.deadline.elapsed()>3000 || presenter_->IsFaulted()) {
        if (save.readback) doc.retiredGpuSaveReadback=save.readback;
        doc.gpuSave.reset();doc.savePending=false;doc.saveState=QStringLiteral("failed");
        ShowStatusMessage(QStringLiteral("The selected video frame could not be saved."));return;
    }
    // Resize drops the old ring receipt after draining the GPU. Retry copying
    // the immutable clone, never the current viewport or a different frame.
    if (save.readback && !presenter_->HasReadbackRequest(save.readback)) save.readback=0;
    if (!save.readback) presenter_->RequestReadback(save.image,&save.readback);
}

bool OkuFlowApp::ConsumeVideoGpuReadback(UINT64 requestId,const std::vector<std::uint8_t>& pixels,UINT width,UINT height)
{
    if (!videoDocument_) return false;
    if (requestId && requestId==videoDocument_->retiredGpuSaveReadback) {
        videoDocument_->retiredGpuSaveReadback=0;return true;
    }
    if (!videoDocument_->gpuSave || videoDocument_->gpuSave->readback!=requestId) return false;
    const auto& image=videoDocument_->gpuSave->image;
    QImage owned;
    if (width==image.width && height==image.height && pixels.size()==std::size_t(width)*height*4)
        owned=QImage(pixels.data(),width,height,width*4,QImage::Format_ARGB32).copy();
    videoDocument_->gpuSave.reset();
    if (owned.isNull()) {
        videoDocument_->savePending=false;videoDocument_->saveState=QStringLiteral("failed");
        ShowStatusMessage(QStringLiteral("The selected video frame could not be saved."));return true;
    }
    QueueSelectedVideoFrameImage(std::move(owned),true);
    return true;
}

void OkuFlowApp::QueueSelectedVideoFrameImage(QImage owned,bool receiptStarted)
{
    if (!userDataPaths_ || !videoDocument_) {
        if (videoDocument_ && receiptStarted) {
            videoDocument_->savePending=false;videoDocument_->saveState=QStringLiteral("failed");
        }
        ShowStatusMessage(QStringLiteral("Frame not saved: the OkuFlow Photos folder is unavailable."));return;
    }
    // These are value snapshots only. Canonicalization, cloud-provider access,
    // directory creation and publication all belong to the image worker.
    const QString configuredRoot = userDataPaths_->ConfiguredRoot();
    const QString installDirectory = QCoreApplication::applicationDirPath();
    const QDateTime saveTime = QDateTime::currentDateTime();
    const QDate saveDate = saveTime.date();
    const QString stem = QStringLiteral("FRAME_%1").arg(saveTime.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    QPointer<OkuFlowApp> owner(this);
    const auto document = videoDocument_;
    if (!receiptStarted) BeginSelectedVideoSaveReceipt();
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
        : doc.kind == VideoDocumentState::Kind::Recap ? "recap" : doc.kind == VideoDocumentState::Kind::Recording ? "recording" : "file";
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
    const auto end = doc.kind == VideoDocumentState::Kind::Recap ? doc.recapSnapshot.end100ns :
        doc.kind == VideoDocumentState::Kind::Recording ? doc.End() : doc.fileDuration100ns;
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
        if(doc.kind==VideoDocumentState::Kind::Recap && doc.recapPresented)
            result["selectedPts100ns"]=static_cast<qint64>(doc.recapPresented->begin100ns+doc.presented->pts100ns);
    } else result["selectedPts100ns"] = QJsonValue::Null;
    if (doc.heldLive) result["provisionalCaptureClock100ns"] = static_cast<qint64>(doc.heldCaptureClock100ns);
    result["sourceName"] = doc.kind == VideoDocumentState::Kind::File ? QFileInfo(doc.filePath).fileName()
        : doc.kind == VideoDocumentState::Kind::Recording ? QStringLiteral("Current recording")
        : doc.kind == VideoDocumentState::Kind::Recap ? QStringLiteral("Live recap") : QString();
    if(doc.kind==VideoDocumentState::Kind::Recap) {
        result["seekableFirst100ns"]=static_cast<qint64>(doc.recapSnapshot.first100ns);
        result["recapBytes"]=static_cast<qint64>(doc.recapSnapshot.compressedBytes);
        result["recapDroppedFrames"]=static_cast<qint64>(doc.recapSnapshot.droppedFrames);
        result["recapAdmittedFrames"]=static_cast<qint64>(doc.recapSnapshot.admittedFrames);
        result["recapCompletedFrames"]=static_cast<qint64>(doc.recapSnapshot.completedFrames);
        result["recapError"]=QString::fromStdString(doc.recapSnapshot.error);
        result["recapSourceWidth"]=static_cast<int>(doc.recapWidth);
        result["recapSourceHeight"]=static_cast<int>(doc.recapHeight);
        result["recapFrameRateNumerator"]=static_cast<int>(doc.recapRateN);
        result["recapFrameRateDenominator"]=static_cast<int>(doc.recapRateD);
        result["recapWarming"]=doc.recapSnapshot.warming;result["recapPressure"]=doc.recapSnapshot.pressure;
        result["recapRetainedOutsideWindow"]=doc.presented && doc.recapPresented &&
            doc.recapPresented->begin100ns+doc.presented->pts100ns<doc.recapSnapshot.first100ns;
    }
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
void OkuFlowApp::SeedPendingLiveHoldForTest(bool finishing)
{
    if (!videoDocument_) InitializeVideoPlayback();
    auto& doc = *videoDocument_;
    doc.kind = VideoDocumentState::Kind::Recording;
    doc.finishing = finishing;
    doc.active = !finishing;
    doc.followingLive = false;
    doc.pendingHold = true;
    doc.holdEpoch = mediaDocumentEpoch_;
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
std::shared_ptr<const VideoPlaybackFrame> OkuFlowApp::HoldPresentedVideoFrameForTest(
    std::shared_ptr<const VideoPlaybackFrame> presented)
{
    if (!videoDocument_ || !presented) return nullptr;
    auto& doc = *videoDocument_;
    // PresentMediaView success bookkeeping followed by the playing->Pause hold.
    doc.presented = presented; doc.presentedNeighbor.reset(); doc.presentedPart = doc.framePart;
    if (presented->requestGeneration == doc.requestGeneration) doc.completedRequestGeneration = doc.requestGeneration;
    doc.pendingSeekMs.reset();
    doc.player.SetPlaying(false); doc.playing = doc.afterOperationPlay = false;
    doc.frame = doc.presented; doc.neighbor.reset(); doc.pausedDisplayHold = true;
    return doc.frame;
}
std::shared_ptr<const VideoPlaybackFrame> OkuFlowApp::SelectedVideoFrameForTest() const
{
    return videoDocument_ ? videoDocument_->frame : nullptr;
}
VideoPlaybackSnapshot OkuFlowApp::VideoPlaybackSnapshotForTest() const
{
    return videoDocument_ ? videoDocument_->player.Snapshot() : VideoPlaybackSnapshot{};
}
#endif
} // namespace okuflow
#endif
