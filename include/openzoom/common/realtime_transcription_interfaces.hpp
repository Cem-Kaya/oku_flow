#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QByteArray>
#include <QObject>
#include <QString>

namespace openzoom {

// Control-plane service for one live-transcription session (production:
// CodexRealtimeTranscriptionClient over a dedicated app-server child).
// Abstract so TranscriptionSessionController is testable with fakes.
//
// Every signal carries the generation passed to BeginSession; stale
// generations must be ignored by the receiver.
class RealtimeTranscriptionService : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    virtual void SetExecutablePath(const QString& configuredExecutable) = 0;
    virtual bool IsSessionActive() const = 0;
    virtual void BeginSession(quint64 generation) = 0;
    virtual void StartRealtime(quint64 generation, const QString& offerSdp) = 0;
    virtual void StopRealtime(quint64 generation) = 0;
    virtual void EndSession(quint64 generation) = 0;
    virtual void RefreshRateLimits() = 0;

signals:
    void SessionReady(quint64 generation);
    void AnswerSdp(quint64 generation, const QString& sdp);
    void RealtimeStarted(quint64 generation);
    void UserTranscriptDelta(quint64 generation, const QString& delta);
    void UserTranscriptDone(quint64 generation, const QString& text, bool truncated);
    void RealtimeClosed(quint64 generation, const QString& reason);
    void SessionFailed(quint64 generation, const QString& message);
    void RateLimitsChanged(int remainingPercent, bool known);
};

// Media-plane carrier that turns native PCM into the realtime session's
// WebRTC audio track (production: RealtimeNativeRtcCarrier, Carrier D).
// PCM crossing SendAudioChunks is signed 16-bit mono; batches are bounded by
// transcript_limits::kMaximumBridgeBatchChunks.
class RealtimeAudioCarrier : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    // Build the peer, the SendRecv audio m-line (incoming audio is discarded)
    // and the oai-events channel, then emit OfferReady. Failure emits
    // CarrierFailed.
    virtual void PrepareOffer(quint64 generation) = 0;
    virtual void ApplyAnswer(quint64 generation, const QString& answerSdp) = 0;
    // Bounded, non-blocking hand-off toward the audio track. Returns false
    // when the carrier cannot accept audio right now; the caller decides gap
    // accounting. Never blocks the caller.
    virtual bool SendAudioChunks(quint64 generation,
                                 const QByteArray& pcm16Mono,
                                 int sampleRate) = 0;
    // Close the input side after all accepted audio has been queued. The
    // carrier emits AudioDrained only after its worker has encoded and paced
    // every accepted frame toward the track. Idempotent and non-blocking.
    virtual void FinishAudioInput(quint64 generation) = 0;
    // Tear down tracks, channel, and peer for this generation. Idempotent.
    virtual void StopCarrier(quint64 generation) = 0;

signals:
    void OfferReady(quint64 generation, const QString& offerSdp);
    void AnswerApplied(quint64 generation);
    // Both the data channel and the audio track are open. The historical
    // signal name is retained to avoid churn in controller/test wiring.
    void ChannelOpen(quint64 generation);
    void AudioDrained(quint64 generation);
    // The carrier accepted native audio but its downstream bounded queue had
    // to discard samples. The controller turns this into a durable gap note;
    // recording is never affected.
    void AudioDropped(quint64 generation, qint64 droppedSamples);
    void CarrierFailed(quint64 generation, const QString& reason);
};

} // namespace openzoom

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
