#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <deque>
#include <mutex>

#include "okuflow/capture/audio_capture.hpp"
#include "okuflow/common/realtime_transcription_interfaces.hpp"
#include "okuflow/common/transcript.hpp"

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace okuflow {

// Recording-coupled state for one live-transcription session (plan 36).
// Owns the operation generation, the bounded native PCM queue and 20 ms
// chunker, the user-only transcript reducer, and the bounded finalize timer.
// It does not own RecordingManager and cannot stop recording: every failure
// here ends only the transcript.
//
// Threading: lives on the UI thread. TryEnqueueAudio is the only member
// callable from another thread (the Media Foundation capture callback); it
// copies PCM into a mutex-guarded bounded queue and returns immediately —
// no allocation-free guarantee, but no locks shared with the recorder, no
// Qt calls, no I/O, and no waits. A UI-thread timer drains, chunks, and
// hands bounded batches to the carrier.
class TranscriptionSessionController : public QObject {
    Q_OBJECT
public:
    // Non-owning; both must outlive the controller or be parented alongside.
    TranscriptionSessionController(RealtimeTranscriptionService* service,
                                   RealtimeAudioCarrier* carrier,
                                   QObject* parent = nullptr);

    void SetEnabled(bool enabled);
    // Marks the feature unavailable for this app session (for example a
    // runtime missing). Overrides enabled until restart.
    void SetUnavailable(const QString& status);
    void SetCodexExecutable(const QString& configuredExecutable);

    TranscriptionState State() const { return state_; }
    QString CurrentSessionId() const { return session_.id; }

    // Called after recording preflight passed and the microphone is running.
    void StartSession(const RecordingSessionInfo& session,
                      const QString& languageCode);
    // Any thread. Returns false when audio is not being accepted.
    bool TryEnqueueAudio(const AudioFrame& frame);
    // Stop feeding, allow a bounded grace for a naturally emitted final, then
    // stop the realtime session. A closing streamed tail is preserved locally,
    // without a second model pass. Never blocks; never gates recorder or
    // microphone release.
    void FinishInput();
    // Idempotent finish keyed by recording id, for recorder-side end paths
    // (session-ended callback, watchdog, encoder failure). A stale id from
    // an older recording is ignored.
    void FinishForSession(const QString& recordingSessionId);
    // Immediate teardown, discarding the partial. Prior finals remain valid.
    void Cancel();

signals:
    // status is a fixed English sentence (translated at display time), or
    // empty when the state alone tells the story.
    void StateChanged(okuflow::TranscriptionState state, const QString& status);
    void PartialChanged(const QString& sessionId, quint64 generation, const QString& text);
    void SegmentFinalized(const okuflow::TranscriptSegment& segment);
    void SessionFinished(const QString& sessionId);
    void GapDetected(const QString& sessionId, qint64 fromClock100ns, qint64 toClock100ns);
    void QuotaChanged(int remainingPercent, bool known);

private:
    void SetState(TranscriptionState state, const QString& status = {});
    void EnterListeningIfNegotiated();
    void FailWith(const QString& message);
    bool RecoverIfTransient(const QString& message);
    void BeginRecovery();
    void TearDownSession();
    void DrainAudioQueue();
    void PublishGapIfPending();
    void PublishPartial();
    void FinalizeTranscriptText(const QString& text, bool truncated);
    void BeginFinalizeGraceIfDrained();
    bool GenerationCurrent(quint64 generation) const { return generation == generation_; }

    static constexpr int kDrainIntervalMs = 50;
    // Bounded pre-stop grace so a phrase already detected by server VAD can
    // finalize before thread/realtime/stop tears the session down. The
    // service's own stop deadline bounds the rest.
    static constexpr int kFinalizeGraceMs = 1500;
    // At most 500 ms of PCM leaves the queue per drain tick.
    static constexpr qsizetype kMaximumDrainBytesPerTick = 48000;
    static constexpr int kMaximumRecoveryAttempts = 3;
    static constexpr int kRecoveryDelayMs = 1000;

    RealtimeTranscriptionService* service_{};
    RealtimeAudioCarrier* carrier_{};
    QTimer* drainTimer_{};
    QTimer* finalizeTimer_{};
    QTimer* partialUpdateTimer_{};

    bool enabled_{false};
    bool unavailable_{false};
    QString unavailableStatus_;
    TranscriptionState state_{TranscriptionState::Off};
    quint64 generation_{0};
    RecordingSessionInfo session_;
    QString languageCode_;
    bool answerApplied_{false};
    bool channelOpen_{false};
    bool realtimeStarted_{false};
    bool finishRequested_{false};
    bool carrierFinishRequested_{false};
    bool carrierDrained_{false};
    bool finalizeGraceStarted_{false};
    int recoveryAttempts_{0};
    quint64 nextSequence_{1};
    // Full current service turn retained only so a clean close can preserve a
    // tail that never received transcript/done. The shorter partialText_ is a
    // presentation tail and may be trimmed without losing durable text.
    QString pendingTranscriptText_;
    bool pendingTranscriptTruncated_{false};
    QString partialText_;
    QString lastFinalText_;
    qint64 lastFinalAtMs_{-1};

    // Capture-thread hand-off. Only these members are touched off the UI
    // thread, only under audioMutex_.
    std::mutex audioMutex_;
    std::deque<AudioFrame> audioQueue_;
    qsizetype queuedBytes_{0};
    qint64 queuedDuration100ns_{0};
    bool acceptingAudio_{false};
    bool gapPending_{false};
    qint64 gapFromClock100ns_{-1};
    qint64 gapToClock100ns_{-1};
    // First microphone clock observed after recording start. Atomic because
    // capture establishes it and the UI thread uses it for note offsets.
    std::atomic<qint64> recordingOriginClock100ns_{-1};

    // UI-thread chunker state.
    QByteArray pcmAccumulator_;
    qint64 newestSentClock100ns_{-1};
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
