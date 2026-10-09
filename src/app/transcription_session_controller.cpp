#ifdef _WIN32

#include "okuflow/app/transcription_session_controller.hpp"

#include <QDebug>
#include <QDateTime>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace okuflow {

using transcript_limits::kCarrierChunkBytes;
using transcript_limits::kMaximumBridgeBatchChunks;
using transcript_limits::kMaximumPartialCharacters;
using transcript_limits::kMaximumQueuedAudio100ns;
using transcript_limits::kMaximumQueuedAudioBytes;

TranscriptionSessionController::TranscriptionSessionController(
    RealtimeTranscriptionService* service,
    RealtimeAudioCarrier* carrier,
    QObject* parent)
    : QObject(parent), service_(service), carrier_(carrier)
{
    drainTimer_ = new QTimer(this);
    drainTimer_->setInterval(kDrainIntervalMs);
    connect(drainTimer_, &QTimer::timeout,
            this, &TranscriptionSessionController::DrainAudioQueue);
    finalizeTimer_ = new QTimer(this);
    finalizeTimer_->setSingleShot(true);
    finalizeTimer_->setInterval(kFinalizeGraceMs);
    connect(finalizeTimer_, &QTimer::timeout, this, [this]() {
        if (state_ == TranscriptionState::Finalizing && service_) {
            service_->StopRealtime(generation_);
        }
    });
    partialUpdateTimer_ = new QTimer(this);
    partialUpdateTimer_->setSingleShot(true);
    partialUpdateTimer_->setInterval(75);
    connect(partialUpdateTimer_, &QTimer::timeout,
            this, &TranscriptionSessionController::PublishPartial);

    if (service_) {
        connect(service_, &RealtimeTranscriptionService::SessionReady,
                this, [this](quint64 generation) {
                    if (!GenerationCurrent(generation) ||
                        state_ != TranscriptionState::Starting || !carrier_) {
                        return;
                    }
                    carrier_->PrepareOffer(generation);
                });
        connect(service_, &RealtimeTranscriptionService::AnswerSdp,
                this, [this](quint64 generation, const QString& sdp) {
                    if (!GenerationCurrent(generation) || !carrier_) {
                        return;
                    }
                    carrier_->ApplyAnswer(generation, sdp);
                });
        connect(service_, &RealtimeTranscriptionService::RealtimeStarted,
                this, [this](quint64 generation) {
                    if (!GenerationCurrent(generation)) {
                        return;
                    }
                    realtimeStarted_ = true;
                    EnterListeningIfNegotiated();
                });
        connect(service_, &RealtimeTranscriptionService::UserTranscriptDelta,
                this, [this](quint64 generation, const QString& delta) {
                    if (!GenerationCurrent(generation)) {
                        return;
                    }
                    if (state_ != TranscriptionState::Listening &&
                        state_ != TranscriptionState::Finalizing) {
                        return;
                    }
                    // Keep one bounded current-turn copy for local tail
                    // preservation at clean close. The visible partial is a
                    // shorter rolling tail, so long unfinalized turns cannot
                    // make the overlay grow without bound.
                    const qsizetype pendingAvailable =
                        transcript_limits::kMaximumSegmentCharacters -
                        pendingTranscriptText_.size();
                    if (pendingAvailable > 0) {
                        pendingTranscriptText_ += delta.left(pendingAvailable);
                    }
                    if (delta.size() > pendingAvailable) {
                        pendingTranscriptTruncated_ = true;
                    }
                    partialText_ += delta;
                    if (partialText_.size() > kMaximumPartialCharacters) {
                        partialText_ = partialText_.right(kMaximumPartialCharacters);
                    }
                    if (!partialUpdateTimer_->isActive()) {
                        partialUpdateTimer_->start();
                    }
                });
        connect(service_, &RealtimeTranscriptionService::UserTranscriptDone,
                this, [this](quint64 generation, const QString& text, bool truncated) {
                    if (!GenerationCurrent(generation)) {
                        return;
                    }
                    if (state_ != TranscriptionState::Listening &&
                        state_ != TranscriptionState::Finalizing) {
                        return;
                    }
                    FinalizeTranscriptText(text, truncated);
                });
        connect(service_, &RealtimeTranscriptionService::RealtimeClosed,
                this, [this](quint64 generation, const QString&) {
                    if (!GenerationCurrent(generation)) {
                        return;
                    }
                    // Some v3 sessions close without transcript/done even
                    // after several seconds of trailing silence. Preserve the
                    // already-streamed user delta locally; this is not a
                    // second transcription or a Codex handoff.
                    FinalizeTranscriptText(pendingTranscriptText_,
                                           pendingTranscriptTruncated_);
                    const QString endedSessionId = session_.id;
                    TearDownSession();
                    SetState(TranscriptionState::Completed,
                             QStringLiteral("Transcription finished."));
                    emit SessionFinished(endedSessionId);
                });
        connect(service_, &RealtimeTranscriptionService::SessionFailed,
                this, [this](quint64 generation, const QString& message) {
                    if (!GenerationCurrent(generation)) {
                        return;
                    }
                    if (RecoverIfTransient(message)) {
                        return;
                    }
                    FailWith(message);
                });
        connect(service_, &RealtimeTranscriptionService::RateLimitsChanged,
                this, [this](int remainingPercent, bool known) {
                    emit QuotaChanged(remainingPercent, known);
                });
    }
    if (carrier_) {
        connect(carrier_, &RealtimeAudioCarrier::OfferReady,
                this, [this](quint64 generation, const QString& offerSdp) {
                    if (!GenerationCurrent(generation) ||
                        state_ != TranscriptionState::Starting || !service_) {
                        return;
                    }
                    service_->StartRealtime(generation, offerSdp);
                });
        connect(carrier_, &RealtimeAudioCarrier::AnswerApplied,
                this, [this](quint64 generation) {
                    if (!GenerationCurrent(generation)) {
                        return;
                    }
                    answerApplied_ = true;
                    EnterListeningIfNegotiated();
                });
        connect(carrier_, &RealtimeAudioCarrier::ChannelOpen,
                this, [this](quint64 generation) {
                    if (!GenerationCurrent(generation)) {
                        return;
                    }
                    channelOpen_ = true;
                    EnterListeningIfNegotiated();
                });
        connect(carrier_, &RealtimeAudioCarrier::CarrierFailed,
                this, [this](quint64 generation, const QString& reason) {
                    if (!GenerationCurrent(generation)) {
                        return;
                    }
                    if (RecoverIfTransient(reason)) {
                        return;
                    }
                    FailWith(reason);
                });
        connect(carrier_, &RealtimeAudioCarrier::AudioDropped,
                this, [this](quint64 generation, qint64) {
                    if (!GenerationCurrent(generation)) {
                        return;
                    }
                    {
                        std::lock_guard lock(audioMutex_);
                        gapPending_ = true;
                        gapFromClock100ns_ = newestSentClock100ns_;
                        gapToClock100ns_ = newestSentClock100ns_;
                    }
                    PublishGapIfPending();
                });
        connect(carrier_, &RealtimeAudioCarrier::AudioDrained,
                this, [this](quint64 generation) {
                    if (!GenerationCurrent(generation) ||
                        state_ != TranscriptionState::Finalizing ||
                        !carrierFinishRequested_) {
                        return;
                    }
                    carrierDrained_ = true;
                    BeginFinalizeGraceIfDrained();
                });
    }
}

void TranscriptionSessionController::SetEnabled(bool enabled)
{
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (unavailable_) {
        return;
    }
    if (!enabled_) {
        if (state_ != TranscriptionState::Off) {
            Cancel();
            SetState(TranscriptionState::Off);
        }
        return;
    }
    if (state_ == TranscriptionState::Off) {
        SetState(TranscriptionState::Ready);
    }
}

void TranscriptionSessionController::SetUnavailable(const QString& status)
{
    unavailable_ = true;
    unavailableStatus_ = status;
    if (state_ == TranscriptionState::Starting ||
        state_ == TranscriptionState::Listening ||
        state_ == TranscriptionState::Finalizing) {
        Cancel();
    }
    SetState(TranscriptionState::Unavailable, status);
}

void TranscriptionSessionController::SetCodexExecutable(const QString& configuredExecutable)
{
    if (service_) {
        service_->SetExecutablePath(configuredExecutable);
    }
}

void TranscriptionSessionController::StartSession(const RecordingSessionInfo& session,
                                                  const QString& languageCode)
{
    if (!enabled_ || !service_ || !carrier_) {
        return;
    }
    if (unavailable_) {
        SetState(TranscriptionState::Unavailable, unavailableStatus_);
        return;
    }
    if (state_ == TranscriptionState::Starting ||
        state_ == TranscriptionState::Listening ||
        state_ == TranscriptionState::Finalizing) {
        // A stale session may not leak into the new recording.
        Cancel();
    }
    ++generation_;
    session_ = session;
    languageCode_ = languageCode;
    answerApplied_ = false;
    channelOpen_ = false;
    realtimeStarted_ = false;
    finishRequested_ = false;
    carrierFinishRequested_ = false;
    carrierDrained_ = false;
    finalizeGraceStarted_ = false;
    recoveryAttempts_ = 0;
    nextSequence_ = 1;
    pendingTranscriptText_.clear();
    pendingTranscriptTruncated_ = false;
    partialText_.clear();
    lastFinalText_.clear();
    lastFinalAtMs_ = -1;
    pcmAccumulator_.clear();
    recordingOriginClock100ns_.store(-1);
    newestSentClock100ns_ = -1;
    {
        std::lock_guard lock(audioMutex_);
        audioQueue_.clear();
        queuedBytes_ = 0;
        queuedDuration100ns_ = 0;
        acceptingAudio_ = true;  // pre-roll while WebRTC negotiates
        gapPending_ = false;
        gapFromClock100ns_ = -1;
        gapToClock100ns_ = -1;
    }
    SetState(TranscriptionState::Starting,
             QStringLiteral("Preparing live transcription..."));
    service_->BeginSession(generation_);
}

bool TranscriptionSessionController::TryEnqueueAudio(const AudioFrame& frame)
{
    // Capture-thread path: copy, account, return. No Qt, no I/O, no waits,
    // and no locks shared with the recorder.
    if (frame.pcm.empty() || frame.sampleRate != 48000 ||
        frame.channels != 1 || frame.bitsPerSample != 16) {
        return false;
    }
    std::lock_guard lock(audioMutex_);
    if (!acceptingAudio_) {
        return false;
    }
    if (frame.captureClock100ns >= 0) {
        qint64 unset = -1;
        recordingOriginClock100ns_.compare_exchange_strong(
            unset, frame.captureClock100ns);
    }
    const qsizetype frameBytes = static_cast<qsizetype>(frame.pcm.size());
    audioQueue_.push_back(frame);  // copies PCM + timestamps
    queuedBytes_ += frameBytes;
    queuedDuration100ns_ += frame.duration100ns;
    while (!audioQueue_.empty() &&
           (queuedBytes_ > kMaximumQueuedAudioBytes ||
            queuedDuration100ns_ > kMaximumQueuedAudio100ns)) {
        const AudioFrame& oldest = audioQueue_.front();
        if (!gapPending_) {
            gapPending_ = true;
            gapFromClock100ns_ = oldest.captureClock100ns;
        }
        gapToClock100ns_ = oldest.captureClock100ns >= 0
                               ? oldest.captureClock100ns + oldest.duration100ns
                               : gapToClock100ns_;
        queuedBytes_ -= static_cast<qsizetype>(oldest.pcm.size());
        queuedDuration100ns_ -= oldest.duration100ns;
        audioQueue_.pop_front();
    }
    return true;
}

void TranscriptionSessionController::FinishInput()
{
    if (state_ == TranscriptionState::Starting) {
        bool hasAudio = false;
        {
            std::lock_guard lock(audioMutex_);
            acceptingAudio_ = false;
            hasAudio = !audioQueue_.empty();
        }
        if (!hasAudio) {
            const QString endedSessionId = session_.id;
            TearDownSession();
            SetState(TranscriptionState::Completed,
                     QStringLiteral("Transcription finished."));
            emit SessionFinished(endedSessionId);
            return;
        }
        // Negotiation remains bounded by the service deadline. Once ready,
        // the captured pre-roll is drained before realtime stop.
        finishRequested_ = true;
        SetState(TranscriptionState::Starting,
                 QStringLiteral("Finishing transcript..."));
        return;
    }
    if (state_ != TranscriptionState::Listening) {
        return;
    }
    {
        std::lock_guard lock(audioMutex_);
        acceptingAudio_ = false;
    }
    finishRequested_ = true;
    SetState(TranscriptionState::Finalizing,
             QStringLiteral("Finishing transcript..."));
    DrainAudioQueue();
}

void TranscriptionSessionController::FinishForSession(const QString& recordingSessionId)
{
    if (recordingSessionId.isEmpty() || session_.id != recordingSessionId) {
        return;
    }
    FinishInput();
}

void TranscriptionSessionController::Cancel()
{
    const TranscriptionState previous = state_;
    TearDownSession();
    if (previous == TranscriptionState::Starting ||
        previous == TranscriptionState::Listening ||
        previous == TranscriptionState::Finalizing) {
        SetState(enabled_ && !unavailable_ ? TranscriptionState::Ready
                                           : TranscriptionState::Off);
    }
}

void TranscriptionSessionController::SetState(TranscriptionState state, const QString& status)
{
    state_ = state;
    emit StateChanged(state_, status);
}

void TranscriptionSessionController::EnterListeningIfNegotiated()
{
    if (state_ != TranscriptionState::Starting) {
        return;
    }
    if (!answerApplied_ || !channelOpen_ || !realtimeStarted_) {
        return;
    }
    SetState(TranscriptionState::Listening,
             QStringLiteral("Listening and transcribing..."));
    if (finishRequested_) {
        SetState(TranscriptionState::Finalizing,
                 QStringLiteral("Finishing transcript..."));
    }
    drainTimer_->start();
    DrainAudioQueue();
}

void TranscriptionSessionController::FailWith(const QString& message)
{
    const QString endedSessionId = session_.id;
    TearDownSession();
    SetState(TranscriptionState::Failed, message);
    emit SessionFinished(endedSessionId);
}

bool TranscriptionSessionController::RecoverIfTransient(const QString& message)
{
    if (finishRequested_ || recoveryAttempts_ >= kMaximumRecoveryAttempts ||
        (state_ != TranscriptionState::Starting &&
         state_ != TranscriptionState::Listening)) {
        return false;
    }
    const QString lower = message.toLower();
    const bool transient =
        lower.contains(QStringLiteral("connection")) ||
        lower.contains(QStringLiteral("network")) ||
        lower.contains(QStringLiteral("timed out")) ||
        lower.contains(QStringLiteral("timeout")) ||
        lower.contains(QStringLiteral("temporar")) ||
        message == QStringLiteral("Codex stopped unexpectedly.") ||
        message == QStringLiteral("WebRTC component stopped.");
    if (!transient) {
        return false;
    }
    BeginRecovery();
    return true;
}

void TranscriptionSessionController::BeginRecovery()
{
    const QString sessionId = session_.id;
    TearDownSession();
    ++recoveryAttempts_;
    ++generation_;
    answerApplied_ = false;
    channelOpen_ = false;
    realtimeStarted_ = false;
    finishRequested_ = false;
    carrierFinishRequested_ = false;
    carrierDrained_ = false;
    finalizeGraceStarted_ = false;
    pcmAccumulator_.clear();
    pendingTranscriptText_.clear();
    pendingTranscriptTruncated_ = false;
    partialText_.clear();
    partialUpdateTimer_->stop();
    emit PartialChanged(sessionId, generation_, QString());
    {
        std::lock_guard lock(audioMutex_);
        audioQueue_.clear();
        queuedBytes_ = 0;
        queuedDuration100ns_ = 0;
        acceptingAudio_ = true;
        gapPending_ = false;
    }
    emit GapDetected(sessionId, -1, -1);
    SetState(TranscriptionState::Starting,
             QStringLiteral("Reconnecting live transcription..."));
    const quint64 recoveryGeneration = generation_;
    QTimer::singleShot(kRecoveryDelayMs, this, [this, recoveryGeneration]() {
        if (!GenerationCurrent(recoveryGeneration) ||
            state_ != TranscriptionState::Starting || !service_) {
            return;
        }
        service_->BeginSession(recoveryGeneration);
    });
}

void TranscriptionSessionController::TearDownSession()
{
    drainTimer_->stop();
    finalizeTimer_->stop();
    partialUpdateTimer_->stop();
    {
        std::lock_guard lock(audioMutex_);
        acceptingAudio_ = false;
        audioQueue_.clear();
        queuedBytes_ = 0;
        queuedDuration100ns_ = 0;
        gapPending_ = false;
    }
    pcmAccumulator_.clear();
    pendingTranscriptText_.clear();
    pendingTranscriptTruncated_ = false;
    partialText_.clear();
    finishRequested_ = false;
    carrierFinishRequested_ = false;
    carrierDrained_ = false;
    finalizeGraceStarted_ = false;
    if (service_) {
        service_->EndSession(generation_);
    }
    if (carrier_) {
        carrier_->StopCarrier(generation_);
    }
}

void TranscriptionSessionController::PublishGapIfPending()
{
    qint64 from = -1;
    qint64 to = -1;
    {
        std::lock_guard lock(audioMutex_);
        if (!gapPending_) {
            return;
        }
        gapPending_ = false;
        from = gapFromClock100ns_;
        to = gapToClock100ns_;
        gapFromClock100ns_ = -1;
        gapToClock100ns_ = -1;
    }
    // Later text must not read as continuous across dropped audio.
    pcmAccumulator_.clear();
    pendingTranscriptText_.clear();
    pendingTranscriptTruncated_ = false;
    partialText_.clear();
    emit PartialChanged(session_.id, generation_, partialText_);
    emit GapDetected(session_.id, from, to);
}

void TranscriptionSessionController::PublishPartial()
{
    if (state_ == TranscriptionState::Listening ||
        state_ == TranscriptionState::Finalizing) {
        emit PartialChanged(session_.id, generation_, partialText_);
    }
}

void TranscriptionSessionController::FinalizeTranscriptText(
    const QString& text,
    bool truncated)
{
    QString normalized = text.trimmed();
    if (normalized.size() > transcript_limits::kMaximumSegmentCharacters) {
        normalized = normalized.left(
            transcript_limits::kMaximumSegmentCharacters);
        truncated = true;
    }

    partialUpdateTimer_->stop();
    pendingTranscriptText_.clear();
    pendingTranscriptTruncated_ = false;
    partialText_.clear();
    emit PartialChanged(session_.id, generation_, partialText_);
    if (normalized.isEmpty()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    // The current app-server event has no stable item id. Suppress only
    // immediate exact replays, while allowing a speaker to repeat the same
    // phrase normally later.
    if (normalized == lastFinalText_ && lastFinalAtMs_ >= 0 &&
        nowMs - lastFinalAtMs_ < 1000) {
        return;
    }
    lastFinalText_ = normalized;
    lastFinalAtMs_ = nowMs;

    TranscriptSegment segment;
    segment.recordingSessionId = session_.id;
    segment.sequence = nextSequence_++;
    segment.languageCode = languageCode_;
    segment.text = normalized;
    segment.truncated = truncated;
    segment.observedCaptureClock100ns = newestSentClock100ns_;
    const qint64 origin = recordingOriginClock100ns_.load();
    if (newestSentClock100ns_ >= 0 && origin >= 0 &&
        newestSentClock100ns_ >= origin) {
        segment.approximateOffset100ns = newestSentClock100ns_ - origin;
    }
    emit SegmentFinalized(segment);
}

void TranscriptionSessionController::BeginFinalizeGraceIfDrained()
{
    if (state_ != TranscriptionState::Finalizing || finalizeGraceStarted_) {
        return;
    }
    bool queueEmpty = false;
    {
        std::lock_guard lock(audioMutex_);
        queueEmpty = audioQueue_.empty();
    }
    if (!queueEmpty || !pcmAccumulator_.isEmpty()) {
        return;
    }
    if (!carrierFinishRequested_) {
        carrierFinishRequested_ = true;
        carrierDrained_ = false;
        // This closes only the carrier's input side. The service remains
        // active so final transcript events can arrive while the bounded
        // Opus/RTP worker drains.
        carrier_->FinishAudioInput(generation_);
        return;
    }
    if (!carrierDrained_) {
        return;
    }
    finalizeGraceStarted_ = true;
    drainTimer_->stop();
    // Bounded grace for a naturally emitted final; the service stop window
    // continues accepting a final already in flight.
    finalizeTimer_->start();
}

void TranscriptionSessionController::DrainAudioQueue()
{
    if ((state_ != TranscriptionState::Listening &&
         state_ != TranscriptionState::Finalizing) || !carrier_) {
        return;
    }
    PublishGapIfPending();

    std::deque<AudioFrame> drained;
    {
        std::lock_guard lock(audioMutex_);
        qsizetype takenBytes = 0;
        while (!audioQueue_.empty() && takenBytes < kMaximumDrainBytesPerTick) {
            takenBytes += static_cast<qsizetype>(audioQueue_.front().pcm.size());
            queuedBytes_ -= static_cast<qsizetype>(audioQueue_.front().pcm.size());
            queuedDuration100ns_ -= audioQueue_.front().duration100ns;
            drained.push_back(std::move(audioQueue_.front()));
            audioQueue_.pop_front();
        }
    }
    for (const AudioFrame& frame : drained) {
        if (frame.captureClock100ns >= 0) {
            newestSentClock100ns_ = frame.captureClock100ns + frame.duration100ns;
        }
        pcmAccumulator_.append(reinterpret_cast<const char*>(frame.pcm.data()),
                               static_cast<qsizetype>(frame.pcm.size()));
    }

    bool queueEmpty = false;
    {
        std::lock_guard lock(audioMutex_);
        queueEmpty = audioQueue_.empty();
    }
    if (state_ == TranscriptionState::Finalizing && queueEmpty &&
        !pcmAccumulator_.isEmpty() && pcmAccumulator_.size() < kCarrierChunkBytes) {
        pcmAccumulator_.append(QByteArray(kCarrierChunkBytes - pcmAccumulator_.size(), '\0'));
    }

    while (pcmAccumulator_.size() >= kCarrierChunkBytes) {
        const qsizetype wholeChunks =
            std::min<qsizetype>(pcmAccumulator_.size() / kCarrierChunkBytes,
                                kMaximumBridgeBatchChunks);
        const qsizetype batchBytes = wholeChunks * kCarrierChunkBytes;
        const QByteArray batch = pcmAccumulator_.left(batchBytes);
        pcmAccumulator_.remove(0, batchBytes);
        if (!carrier_->SendAudioChunks(generation_, batch, 48000)) {
            // Carrier congestion: this audio is lost to the transcript (the
            // recorder is untouched). Surface it as a gap.
            {
                std::lock_guard lock(audioMutex_);
                gapPending_ = true;
                gapFromClock100ns_ = newestSentClock100ns_;
                gapToClock100ns_ = newestSentClock100ns_;
            }
            pcmAccumulator_.clear();
            PublishGapIfPending();
            BeginFinalizeGraceIfDrained();
            return;
        }
    }
    BeginFinalizeGraceIfDrained();
}

} // namespace okuflow

#endif // _WIN32
