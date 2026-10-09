#ifdef _WIN32

#include "openzoom/common/realtime_native_rtc_carrier.hpp"

#include <QDebug>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include <opus.h>
#include <rtc/rtc.hpp>

#include "openzoom/common/transcript.hpp"

namespace openzoom {

namespace {

using namespace std::chrono_literals;

constexpr auto kRtcCleanupDeadline = 2s;
// The UI-side bridge drains in 50 ms batches while RTP is paced in 20 ms
// packets. Starting on the first packet lets normal timer jitter repeatedly
// starve the sender and produces audible discontinuities without overflowing
// either bounded queue. Hold a small, bounded continuity cushion and rebuild
// it only after a later empty queue actually misses its next media deadline.
constexpr qsizetype kSenderContinuityBufferBytes = 500 * 48000 * 2 / 1000;

// libdatachannel callbacks run on its worker threads. They may only touch
// the carrier through this token: deref happens under the mutex while
// `alive` holds, and the actual work is queued onto the carrier's Qt
// thread. The carrier flips `alive` before releasing any peer object.
struct CallbackToken {
    std::mutex mutex;
    bool alive{true};
};

struct GlobalRtcRuntime {
    std::mutex mutex;
    unsigned int clients{0};
    bool preloaded{false};
    std::shared_future<void> cleanupFuture;
};

GlobalRtcRuntime& RuntimeState()
{
    static GlobalRtcRuntime state;
    return state;
}

bool AcquireRtcRuntime(unsigned int threadCount)
{
    GlobalRtcRuntime& state = RuntimeState();
    std::unique_lock lock(state.mutex);
    if (state.clients > 0) {
        ++state.clients;
        return true;
    }
    if (state.cleanupFuture.valid()) {
        if (state.cleanupFuture.wait_for(kRtcCleanupDeadline) !=
            std::future_status::ready) {
            qWarning() << "native rtc: previous global cleanup timed out";
            return false;
        }
        try {
            state.cleanupFuture.get();
        } catch (const std::exception&) {
            qWarning() << "native rtc: previous global cleanup failed";
            return false;
        }
        state.cleanupFuture = {};
    }
    try {
        // Route only errors, and never the message payloads: libdatachannel
        // log lines can carry local addresses, candidates, or SDP.
        rtc::InitLogger(rtc::LogLevel::Error,
                        [](rtc::LogLevel, rtc::string) {
                            qWarning() << "native rtc: internal error reported";
                        });
        // Two is libdatachannel's minimum. Four gives ICE/DTLS plus paced RTP
        // room without scaling to every logical CPU on high-core machines.
        rtc::SetThreadPoolSize(threadCount);
        rtc::Preload();
        state.preloaded = true;
        state.clients = 1;
        return true;
    } catch (const std::exception&) {
        qWarning() << "native rtc: global initialization failed";
        state.preloaded = false;
        state.clients = 0;
        return false;
    }
}

void ReleaseRtcRuntime()
{
    std::shared_future<void> cleanup;
    {
        GlobalRtcRuntime& state = RuntimeState();
        std::lock_guard lock(state.mutex);
        if (state.clients == 0 || --state.clients != 0 || !state.preloaded) {
            return;
        }
        state.preloaded = false;
        state.cleanupFuture = rtc::Cleanup();
        cleanup = state.cleanupFuture;
    }
    if (cleanup.valid() &&
        cleanup.wait_for(kRtcCleanupDeadline) != std::future_status::ready) {
        // Cleanup owns its state and continues asynchronously. App shutdown
        // stays bounded; a later carrier refuses to initialize until it has
        // actually completed.
        qWarning() << "native rtc: global cleanup continues asynchronously";
        return;
    }
    if (cleanup.valid()) {
        try {
            cleanup.get();
        } catch (const std::exception&) {
            qWarning() << "native rtc: global cleanup failed";
        }
    }
}

template <typename Function>
void PostToCarrier(const std::shared_ptr<CallbackToken>& token,
                   RealtimeNativeRtcCarrier* carrier,
                   Function&& function)
{
    std::lock_guard lock(token->mutex);
    if (!token->alive) {
        return;
    }
    QMetaObject::invokeMethod(carrier, std::forward<Function>(function),
                              Qt::QueuedConnection);
}

} // namespace

struct RealtimeNativeRtcCarrier::Impl {
    struct SenderSession {
        std::mutex mutex;
        std::condition_variable condition;
        std::deque<QByteArray> frames;
        QByteArray pendingPcm;
        qsizetype queuedBytes{0};
        bool accepting{false};
        bool finishing{false};
        bool cancelled{false};
        std::shared_ptr<rtc::Track> track;
        std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
        OpusEncoder* encoder{nullptr};
        int configuredFrameSamples{960};
        qint64 samplesSent{0};

        ~SenderSession()
        {
            if (encoder != nullptr) {
                opus_encoder_destroy(encoder);
            }
        }
    };

    std::shared_ptr<CallbackToken> token;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> channel;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    std::shared_ptr<SenderSession> sender;
    std::jthread senderWorker;
    bool channelOpen{false};
    bool trackOpen{false};
    bool readyEmitted{false};
    bool runtimeAcquired{false};

    void StartSender(RealtimeNativeRtcCarrier* carrier,
                     const std::shared_ptr<CallbackToken>& callbackToken,
                     quint64 generation)
    {
        const std::shared_ptr<SenderSession> session = sender;
        senderWorker = std::jthread(
            [carrier, callbackToken, session, generation](std::stop_token stopToken) {
                bool sentAnyFrame = false;
                bool bufferingForContinuity = true;
                auto nextSendAt = std::chrono::steady_clock::now();
                for (;;) {
                    QByteArray frame;
                    {
                        std::unique_lock lock(session->mutex);
                        if (!bufferingForContinuity &&
                            session->frames.empty() && !session->finishing) {
                            // A producer can legitimately refill an empty
                            // queue before the next media deadline. Only an
                            // actual missed deadline is an underflow that must
                            // rebuild the continuity cushion.
                            session->condition.wait_until(
                                lock, nextSendAt, [&]() {
                                    return stopToken.stop_requested() ||
                                           session->cancelled ||
                                           !session->frames.empty() ||
                                           session->finishing;
                                });
                            if (session->frames.empty() &&
                                !session->finishing &&
                                !stopToken.stop_requested() &&
                                !session->cancelled) {
                                bufferingForContinuity = true;
                            }
                        }
                        session->condition.wait(lock, [&]() {
                            const bool continuityBuffered =
                                !session->frames.empty() &&
                                (!bufferingForContinuity || session->finishing ||
                                 session->queuedBytes >=
                                     kSenderContinuityBufferBytes);
                            return stopToken.stop_requested() || session->cancelled ||
                                   continuityBuffered || session->finishing;
                        });
                        if (stopToken.stop_requested() || session->cancelled) {
                            return;
                        }
                        if (session->frames.empty()) {
                            if (!session->finishing) {
                                continue;
                            }
                            lock.unlock();
                            PostToCarrier(
                                callbackToken, carrier,
                                [carrier, generation]() {
                                    emit carrier->AudioDrained(generation);
                                });
                            return;
                        }
                        if (sentAnyFrame) {
                            session->condition.wait_until(
                                lock, nextSendAt, [&]() {
                                    return stopToken.stop_requested() ||
                                           session->cancelled;
                                });
                            if (stopToken.stop_requested() || session->cancelled) {
                                return;
                            }
                        }
                        frame = std::move(session->frames.front());
                        session->frames.pop_front();
                        session->queuedBytes -= frame.size();
                        bufferingForContinuity = false;
                    }

                    if (!sentAnyFrame) {
                        // Anchor the pacing phase when media actually starts,
                        // not when the worker was created before negotiation.
                        nextSendAt = std::chrono::steady_clock::now();
                    }

                    unsigned char encoded[1500];
                    const auto* samples = reinterpret_cast<const opus_int16*>(
                        frame.constData());
                    const int frameSamples = frame.size() / 2;
                    const opus_int32 encodedBytes = opus_encode(
                        session->encoder, samples, frameSamples, encoded,
                        static_cast<opus_int32>(sizeof(encoded)));
                    bool sent = encodedBytes > 0;
                    if (sent) {
                        session->rtpConfig->timestamp =
                            session->rtpConfig->startTimestamp +
                            static_cast<uint32_t>(session->samplesSent);
                        try {
                            // This bounded worker is the sole pacer. There is
                            // deliberately no PacingHandler downstream: its
                            // private queue is unbounded and has no drain
                            // acknowledgement. A successful synchronous handoff
                            // therefore means the frame has left every queue
                            // owned by this carrier.
                            sent = session->track->send(
                                reinterpret_cast<const std::byte*>(encoded),
                                static_cast<size_t>(encodedBytes));
                        } catch (const std::exception&) {
                            sent = false;
                        }
                    }
                    if (!sent) {
                        qint64 droppedSamples = frameSamples;
                        {
                            std::lock_guard lock(session->mutex);
                            droppedSamples += session->queuedBytes / 2;
                            session->frames.clear();
                            session->pendingPcm.clear();
                            session->queuedBytes = 0;
                            session->accepting = false;
                            session->cancelled = true;
                        }
                        PostToCarrier(
                            callbackToken, carrier,
                            [carrier, generation, droppedSamples]() {
                                emit carrier->AudioDropped(generation,
                                                           droppedSamples);
                                carrier->FailCarrier(
                                    generation,
                                    QStringLiteral("connection lost"));
                            });
                        return;
                    }
                    session->samplesSent += frameSamples;
                    sentAnyFrame = true;
                    // Advance by the encoded media duration so normal
                    // encode/send overhead is inside, rather than added to,
                    // each interval. If the worker misses a whole deadline,
                    // re-anchor after completion instead of bursting queued
                    // frames to catch up.
                    const auto framePacingInterval =
                        std::chrono::microseconds(
                            static_cast<qint64>(frameSamples) * 1000000LL /
                            48000LL);
                    nextSendAt += framePacingInterval;
                    const auto completedAt = std::chrono::steady_clock::now();
                    if (nextSendAt <= completedAt) {
                        nextSendAt = completedAt + framePacingInterval;
                    }
                }
            });
    }

    void StopSender()
    {
        const std::shared_ptr<SenderSession> session = sender;
        if (session) {
            {
                std::lock_guard lock(session->mutex);
                session->accepting = false;
                session->cancelled = true;
                session->frames.clear();
                session->pendingPcm.clear();
                session->queuedBytes = 0;
            }
            session->condition.notify_all();
        }
        if (senderWorker.joinable()) {
            senderWorker.request_stop();
            if (session) {
                session->condition.notify_all();
            }
            senderWorker.join();
        }
        sender.reset();
    }

    ~Impl() { StopSender(); }
};

RealtimeNativeRtcCarrier::RealtimeNativeRtcCarrier(QObject* parent)
    : RealtimeNativeRtcCarrier(RealtimeNativeRtcProfile{}, parent)
{
}

RealtimeNativeRtcCarrier::RealtimeNativeRtcCarrier(
    const RealtimeNativeRtcProfile& profile,
    QObject* parent)
    : RealtimeAudioCarrier(parent), impl_(std::make_unique<Impl>()),
      profile_(profile)
{
    impl_->runtimeAcquired = AcquireRtcRuntime(kRtcThreadPoolSize);
    negotiationDeadlineTimer_ = new QTimer(this);
    negotiationDeadlineTimer_->setSingleShot(true);
    connect(negotiationDeadlineTimer_, &QTimer::timeout, this, [this]() {
        if (activeGeneration_ != 0) {
            FailCarrier(activeGeneration_,
                        QStringLiteral("WebRTC negotiation timed out."));
        }
    });
    disconnectGraceTimer_ = new QTimer(this);
    disconnectGraceTimer_->setSingleShot(true);
    disconnectGraceTimer_->setInterval(kDisconnectGraceMs);
    connect(disconnectGraceTimer_, &QTimer::timeout, this, [this]() {
        if (activeGeneration_ != 0) {
            FailCarrier(activeGeneration_, QStringLiteral("connection lost"));
        }
    });
}

bool RealtimeNativeRtcCarrier::IsProfileSupported(
    const RealtimeNativeRtcProfile& profile)
{
    const bool supportedFrameDuration =
        profile.opusFrameDurationMs == 20 ||
        profile.opusFrameDurationMs == 40 ||
        profile.opusFrameDurationMs == 60;
    const bool supportedBitrate =
        profile.opusBitrateBps >= 6000 && profile.opusBitrateBps <= 128000;
    return supportedFrameDuration && supportedBitrate;
}

RealtimeNativeRtcCarrier::~RealtimeNativeRtcCarrier()
{
    TearDownPeer();
    if (impl_->runtimeAcquired) {
        ReleaseRtcRuntime();
        impl_->runtimeAcquired = false;
    }
}

void RealtimeNativeRtcCarrier::PrepareOffer(quint64 generation)
{
    TearDownPeer();
    activeGeneration_ = generation;
    if (!IsProfileSupported(profile_)) {
        QTimer::singleShot(0, this, [this, generation]() {
            FailCarrier(generation,
                        QStringLiteral("Unsupported native RTC profile."));
        });
        return;
    }
    if (!impl_->runtimeAcquired) {
        QTimer::singleShot(0, this, [this, generation]() {
            FailCarrier(generation,
                        QStringLiteral("WebRTC initialization failed."));
        });
        return;
    }

    auto token = std::make_shared<CallbackToken>();
    impl_->token = token;
    try {
        rtc::Configuration configuration;
        // No ICE servers: host candidates only, no third-party STUN/TURN
        // contacted. The offer/track/channel are authored before one manual
        // local description, so automatic negotiation stays off.
        configuration.disableAutoNegotiation = true;
        impl_->peer = std::make_shared<rtc::PeerConnection>(configuration);

        impl_->peer->onGatheringStateChange(
            [this, token, generation](rtc::PeerConnection::GatheringState state) {
                if (state != rtc::PeerConnection::GatheringState::Complete) {
                    return;
                }
                PostToCarrier(token, this, [this, generation]() {
                    HandleGatheringComplete(generation);
                });
            });
        impl_->peer->onStateChange(
            [this, token, generation](rtc::PeerConnection::State state) {
                PostToCarrier(token, this, [this, generation, state]() {
                    HandleConnectionStateChange(generation,
                                                static_cast<int>(state));
                });
            });

        // The server expects this exact data-channel label. Transcript and
        // audio payloads still arrive through their established paths. We
        // discard every payload without parsing, logging, or forwarding it.
        impl_->channel = impl_->peer->createDataChannel("oai-events");
        impl_->channel->onOpen([this, token, generation]() {
            PostToCarrier(token, this, [this, generation]() {
                HandleChannelOpen(generation);
            });
        });
        impl_->channel->onMessage([](rtc::message_variant) {});

        // Mono Opus out. RFC 7587: the SDP always signals `opus/48000/2`
        // even though the payload is mono. The media is offered SendRecv
        // because the conversational service refuses to wire a send-only
        // session (verified live 2026-08-08); received RTP is discarded
        // below without an Opus decoder ever existing.
        const uint32_t ssrc = 0x4F5A0001u ^ static_cast<uint32_t>(generation);
        rtc::Description::Audio audio("audio",
                                      rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(kOpusPayloadType);
        audio.addAttribute(
            "ptime:" + std::to_string(profile_.opusFrameDurationMs));
        audio.addSSRC(ssrc, "openzoom-audio");
        impl_->rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "openzoom", kOpusPayloadType,
            rtc::OpusRtpPacketizer::DefaultClockRate);
        auto packetizer =
            std::make_shared<rtc::OpusRtpPacketizer>(impl_->rtpConfig);
        // Sender reports help the service's jitter and latency estimation.
        packetizer->addToChain(
            std::make_shared<rtc::RtcpSrReporter>(impl_->rtpConfig));
        impl_->track = impl_->peer->addTrack(audio);
        impl_->track->setMediaHandler(packetizer);
        impl_->track->onOpen([this, token, generation]() {
            PostToCarrier(token, this, [this, generation]() {
                HandleTrackOpen(generation);
            });
        });
        // Assistant return audio: dropped undecoded, routed nowhere.
        impl_->track->onMessage([](rtc::message_variant) {});

        auto sender = std::make_shared<Impl::SenderSession>();
        sender->track = impl_->track;
        sender->rtpConfig = impl_->rtpConfig;
        sender->configuredFrameSamples =
            profile_.opusFrameDurationMs * 48000 / 1000;
        int opusError = OPUS_OK;
        sender->encoder =
            opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &opusError);
        if (opusError != OPUS_OK || sender->encoder == nullptr) {
            FailCarrier(generation,
                        QStringLiteral("WebRTC negotiation failed."));
            return;
        }
        const bool encoderConfigured =
            opus_encoder_ctl(sender->encoder,
                             OPUS_SET_BITRATE(profile_.opusBitrateBps)) ==
                OPUS_OK &&
            opus_encoder_ctl(sender->encoder, OPUS_SET_INBAND_FEC(1)) ==
                OPUS_OK &&
            opus_encoder_ctl(
                sender->encoder,
                OPUS_SET_PACKET_LOSS_PERC(kOpusExpectedLossPercent)) ==
                OPUS_OK &&
            opus_encoder_ctl(sender->encoder, OPUS_SET_DTX(0)) == OPUS_OK;
        if (!encoderConfigured) {
            FailCarrier(generation,
                        QStringLiteral("WebRTC negotiation failed."));
            return;
        }
        // The service's VAD must see a continuous stream; silence stays
        // explicit rather than becoming discontinuous transmission.
        impl_->sender = std::move(sender);
        impl_->StartSender(this, token, generation);

        negotiationDeadlineTimer_->start(kNegotiationDeadlineMs);
        impl_->peer->setLocalDescription();
    } catch (const std::exception& error) {
        qWarning() << "native rtc: offer construction failed:" << error.what();
        FailCarrier(generation, QStringLiteral("WebRTC negotiation failed."));
    }
}

void RealtimeNativeRtcCarrier::HandleGatheringComplete(quint64 generation)
{
    if (generation != activeGeneration_ || !impl_->peer) {
        return;
    }
    auto description = impl_->peer->localDescription();
    if (!description.has_value()) {
        FailCarrier(generation, QStringLiteral("WebRTC negotiation failed."));
        return;
    }
    const QString sdp =
        QString::fromStdString(std::string(description.value()));
    if (sdp.isEmpty() ||
        sdp.size() > transcript_limits::kMaximumSdpCharacters) {
        FailCarrier(generation,
                    QStringLiteral("The WebRTC offer was rejected."));
        return;
    }
    // The deadline stays armed until both the data channel and media track
    // explicitly report open.
    emit OfferReady(generation, sdp);
}

void RealtimeNativeRtcCarrier::ApplyAnswer(quint64 generation,
                                           const QString& answerSdp)
{
    if (generation != activeGeneration_ || !impl_->peer) {
        return;
    }
    if (answerSdp.isEmpty() ||
        answerSdp.size() > transcript_limits::kMaximumSdpCharacters) {
        FailCarrier(generation, QStringLiteral("WebRTC negotiation failed."));
        return;
    }
    try {
        impl_->peer->setRemoteDescription(
            rtc::Description(answerSdp.toStdString(), "answer"));
    } catch (const std::exception& error) {
        qWarning() << "native rtc: answer rejected:" << error.what();
        FailCarrier(generation, QStringLiteral("WebRTC negotiation failed."));
        return;
    }
    negotiationDeadlineTimer_->start(kNegotiationDeadlineMs);
    emit AnswerApplied(generation);
}

void RealtimeNativeRtcCarrier::HandleChannelOpen(quint64 generation)
{
    if (generation != activeGeneration_ || !impl_->channel) {
        return;
    }
    impl_->channelOpen = true;
    EmitReadyIfComplete(generation);
}

void RealtimeNativeRtcCarrier::HandleTrackOpen(quint64 generation)
{
    if (generation != activeGeneration_) {
        return;
    }
    impl_->trackOpen = true;
    EmitReadyIfComplete(generation);
}

void RealtimeNativeRtcCarrier::EmitReadyIfComplete(quint64 generation)
{
    if (generation != activeGeneration_ || impl_->readyEmitted ||
        !impl_->channelOpen || !impl_->trackOpen || !impl_->sender) {
        return;
    }
    {
        std::lock_guard lock(impl_->sender->mutex);
        if (impl_->sender->cancelled) {
            return;
        }
        impl_->sender->accepting = true;
    }
    impl_->readyEmitted = true;
    negotiationDeadlineTimer_->stop();
    emit ChannelOpen(generation);
}

bool RealtimeNativeRtcCarrier::SendAudioChunks(quint64 generation,
                                               const QByteArray& pcm16Mono,
                                               int sampleRate)
{
    constexpr int kInputChunkBytes = kInputChunkSamples * 2;
    if (generation != activeGeneration_ || sampleRate != 48000 ||
        !impl_->readyEmitted || !impl_->sender || pcm16Mono.isEmpty() ||
        pcm16Mono.size() % kInputChunkBytes != 0 ||
        pcm16Mono.size() > transcript_limits::kMaximumQueuedAudioBytes) {
        return false;
    }
    const std::shared_ptr<Impl::SenderSession> session = impl_->sender;
    {
        std::lock_guard lock(session->mutex);
        if (!session->accepting || session->finishing || session->cancelled ||
            session->queuedBytes + pcm16Mono.size() >
                transcript_limits::kMaximumQueuedAudioBytes) {
            return false;
        }
        session->pendingPcm.append(pcm16Mono);
        const qsizetype configuredFrameBytes =
            static_cast<qsizetype>(session->configuredFrameSamples) * 2;
        while (session->pendingPcm.size() >= configuredFrameBytes) {
            session->frames.emplace_back(session->pendingPcm.constData(),
                                         configuredFrameBytes);
            session->pendingPcm.remove(0, configuredFrameBytes);
        }
        session->queuedBytes += pcm16Mono.size();
    }
    session->condition.notify_one();
    return true;
}

void RealtimeNativeRtcCarrier::FinishAudioInput(quint64 generation)
{
    if (generation != activeGeneration_ || !impl_->sender) {
        return;
    }
    const std::shared_ptr<Impl::SenderSession> session = impl_->sender;
    {
        std::lock_guard lock(session->mutex);
        if (session->cancelled || session->finishing) {
            return;
        }
        session->accepting = false;
        // Controller input is always aligned to 20 ms. A 40/60 ms probe may
        // end with one shorter legal Opus frame; queue it as-is instead of
        // padding or dropping real speech.
        if (!session->pendingPcm.isEmpty()) {
            session->frames.push_back(std::move(session->pendingPcm));
            session->pendingPcm.clear();
        }
        session->finishing = true;
    }
    session->condition.notify_one();
}

void RealtimeNativeRtcCarrier::StopCarrier(quint64 generation)
{
    if (generation != activeGeneration_) {
        return;
    }
    TearDownPeer();
}

void RealtimeNativeRtcCarrier::HandleConnectionStateChange(quint64 generation,
                                                           int state)
{
    if (generation != activeGeneration_) {
        return;
    }
    switch (static_cast<rtc::PeerConnection::State>(state)) {
    case rtc::PeerConnection::State::Connected:
        disconnectGraceTimer_->stop();
        break;
    case rtc::PeerConnection::State::Disconnected:
        // Transient by policy: give ICE a bounded chance to recover before
        // the controller's retry path takes over.
        if (!disconnectGraceTimer_->isActive()) {
            disconnectGraceTimer_->start();
        }
        break;
    case rtc::PeerConnection::State::Failed:
    case rtc::PeerConnection::State::Closed:
        FailCarrier(generation, QStringLiteral("connection lost"));
        break;
    case rtc::PeerConnection::State::New:
    case rtc::PeerConnection::State::Connecting:
        break;
    }
}

void RealtimeNativeRtcCarrier::FailCarrier(quint64 generation,
                                           const QString& reason)
{
    if (generation != activeGeneration_) {
        return;
    }
    TearDownPeer();
    emit CarrierFailed(generation, reason);
}

void RealtimeNativeRtcCarrier::TearDownPeer()
{
    negotiationDeadlineTimer_->stop();
    disconnectGraceTimer_->stop();
    activeGeneration_ = 0;
    if (impl_->token) {
        std::lock_guard lock(impl_->token->mutex);
        impl_->token->alive = false;
    }
    impl_->StopSender();
    impl_->token.reset();
    if (impl_->channel) {
        impl_->channel->resetCallbacks();
        try {
            impl_->channel->close();
        } catch (const std::exception&) {
            // Already closed.
        }
    }
    if (impl_->track) {
        impl_->track->resetCallbacks();
    }
    if (impl_->peer) {
        impl_->peer->onGatheringStateChange(nullptr);
        impl_->peer->onStateChange(nullptr);
        try {
            impl_->peer->close();
        } catch (const std::exception&) {
            // Already closed.
        }
    }
    impl_->channel.reset();
    impl_->track.reset();
    impl_->rtpConfig.reset();
    impl_->peer.reset();
    impl_->channelOpen = false;
    impl_->trackOpen = false;
    impl_->readyEmitted = false;
}

} // namespace openzoom

#endif // _WIN32
