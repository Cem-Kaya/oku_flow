#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QString>

#include <memory>

#include "openzoom/common/realtime_transcription_interfaces.hpp"

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace openzoom {

// Production uses the live-tested 48 kbit/s, 20 ms profile. The explicit
// profile is also used by the opt-in live probe to compare codec bitrate and
// packet duration against the same lecture fixture; invalid Opus durations
// are rejected before a peer or encoder is created.
struct RealtimeNativeRtcProfile {
    int opusBitrateBps{48000};
    int opusFrameDurationMs{20};
};

// Carrier D (plan 36): native WebRTC media plane built on the pinned
// libdatachannel + opus + MbedTLS stack.
//
// - One mono Opus track (RFC 7587 signaling `opus/48000/2`, payload type
//   111, 20 ms frames) plus the server-expected `oai-events` data channel.
//   The m-line is SendRecv because the service requires that shape; incoming
//   RTP is discarded undecoded. The offer is authored locally.
// - No ICE servers are configured: the proven path uses host candidates
//   only, so no third-party STUN/TURN service is contacted. Media flows only
//   to the endpoints in the app-server's SDP answer.
// - The assistant's return audio is never decoded or played: no Opus decoder
//   exists in the process. Data-channel payloads are discarded unparsed —
//   transcript events arrive over stdio.
// - PCM crosses a bounded five-second worker queue. That worker performs Opus
//   encoding and phase-locked real-time RTP pacing away from the UI thread. A
//   bounded 500 ms continuity cushion decouples the 50 ms UI drain cadence
//   from 20 ms RTP consumption and is rebuilt after a real empty-queue media
//   deadline; missed deadlines re-anchor without catch-up bursts, and
//   finalization waits for an explicit AudioDrained acknowledgement.
// - libdatachannel callbacks arrive on its worker threads; they are gated by
//   a lifetime token and marshalled onto the carrier's Qt thread before any
//   state or signal is touched. Late events are dropped by generation
//   exactly like every other carrier.
// - The carrier is always available: no loader, browser runtime, or staged
//   assets exist. Failure reasons are chosen so the controller's transient
//   classifier can retry connection losses and timeouts.
class RealtimeNativeRtcCarrier : public RealtimeAudioCarrier {
    Q_OBJECT
public:
    explicit RealtimeNativeRtcCarrier(QObject* parent = nullptr);
    explicit RealtimeNativeRtcCarrier(const RealtimeNativeRtcProfile& profile,
                                      QObject* parent = nullptr);
    ~RealtimeNativeRtcCarrier() override;

    static bool IsProfileSupported(const RealtimeNativeRtcProfile& profile);

    void PrepareOffer(quint64 generation) override;
    void ApplyAnswer(quint64 generation, const QString& answerSdp) override;
    bool SendAudioChunks(quint64 generation,
                         const QByteArray& pcm16Mono,
                         int sampleRate) override;
    void FinishAudioInput(quint64 generation) override;
    void StopCarrier(quint64 generation) override;

private:
    struct Impl;

    void HandleGatheringComplete(quint64 generation);
    void HandleConnectionStateChange(quint64 generation, int state);
    void HandleChannelOpen(quint64 generation);
    void HandleTrackOpen(quint64 generation);
    void EmitReadyIfComplete(quint64 generation);
    void FailCarrier(quint64 generation, const QString& reason);
    void TearDownPeer();

    static constexpr int kNegotiationDeadlineMs = 15000;
    static constexpr int kDisconnectGraceMs = 3000;
    static constexpr int kOpusPayloadType = 111;
    static constexpr int kOpusExpectedLossPercent = 5;
    static constexpr int kInputChunkSamples = 960;  // 20 ms at 48 kHz
    static constexpr int kRtcThreadPoolSize = 4;

    std::unique_ptr<Impl> impl_;
    RealtimeNativeRtcProfile profile_;
    QTimer* negotiationDeadlineTimer_{nullptr};
    QTimer* disconnectGraceTimer_{nullptr};
    quint64 activeGeneration_{0};
};

} // namespace openzoom

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
