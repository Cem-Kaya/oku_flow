// Opt-in live gate for plan 36 Carrier D (never part of default ctest).
// Streams a 48 kHz mono PCM16 WAV through the real transcription stack —
// CodexRealtimeTranscriptionClient + RealtimeNativeRtcCarrier +
// TranscriptionSessionController — against the signed-in Codex account, and
// exits 0 only after a clean Completed session with at least one finalized
// user segment and no audio gap.
//
//   native_rtc_live_probe.exe <speech.wav> [--bitrate=64000]
//       [--frame-ms=20] [--feed-ms=20] [--prebuffer-ms=500]
//       [--verbose-partials]
//
// Requires: codex CLI on PATH signed in with ChatGPT, network access.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "openzoom/app/transcription_session_controller.hpp"
#include "openzoom/common/codex_realtime_transcription_client.hpp"
#include "openzoom/common/realtime_native_rtc_carrier.hpp"

using namespace openzoom;

namespace {

constexpr int kMinimumTrailingSilenceMs = 3000;
constexpr int kSetupAndDrainTimeoutMs = 90000;

QByteArray LoadWavPcm(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray raw = file.readAll();
    if (raw.size() < 12 || !raw.startsWith("RIFF") ||
        raw.mid(8, 4) != QByteArrayLiteral("WAVE")) {
        return {};
    }
    bool formatSeen = false;
    qsizetype dataOffset = -1;
    qsizetype dataSize = 0;
    for (qsizetype offset = 12; offset + 8 <= raw.size();) {
        const QByteArray chunkId = raw.mid(offset, 4);
        const quint32 chunkSize = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(raw.constData() + offset + 4));
        const qsizetype payloadOffset = offset + 8;
        if (chunkSize > static_cast<quint64>(raw.size() - payloadOffset)) {
            return {};
        }
        if (chunkId == QByteArrayLiteral("fmt ")) {
            if (chunkSize < 16) {
                return {};
            }
            const auto* format = reinterpret_cast<const uchar*>(
                raw.constData() + payloadOffset);
            const quint16 encoding = qFromLittleEndian<quint16>(format);
            const quint16 channels = qFromLittleEndian<quint16>(format + 2);
            const quint32 sampleRate = qFromLittleEndian<quint32>(format + 4);
            const quint16 blockAlign = qFromLittleEndian<quint16>(format + 12);
            const quint16 bitsPerSample =
                qFromLittleEndian<quint16>(format + 14);
            formatSeen = encoding == 1 && channels == 1 &&
                         sampleRate == 48000 && blockAlign == 2 &&
                         bitsPerSample == 16;
            if (!formatSeen) {
                return {};
            }
        } else if (chunkId == QByteArrayLiteral("data")) {
            dataOffset = payloadOffset;
            dataSize = static_cast<qsizetype>(chunkSize);
        }
        const quint64 nextOffset = static_cast<quint64>(payloadOffset) +
                                   chunkSize + (chunkSize & 1u);
        if (nextOffset > static_cast<quint64>(raw.size())) {
            return {};
        }
        offset = static_cast<qsizetype>(nextOffset);
    }
    if (!formatSeen || dataOffset < 0 || dataSize <= 0 || dataSize % 2 != 0) {
        return {};
    }
    return raw.mid(dataOffset, dataSize);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 2) {
        std::fprintf(
            stderr,
            "usage: native_rtc_live_probe <speech.wav> [--bitrate=N] "
            "[--frame-ms=N] [--feed-ms=N] [--prebuffer-ms=N] "
            "[--verbose-partials]\n");
        return 2;
    }
    RealtimeNativeRtcProfile profile;
    int feedIntervalMs = 20;
    int prebufferMs = 500;
    bool verbosePartials = false;
    const QStringList arguments = application.arguments();
    for (qsizetype index = 2; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--verbose-partials")) {
            verbosePartials = true;
            continue;
        }
        const auto parseInteger = [&argument](const QString& prefix,
                                              int* destination) {
            if (!argument.startsWith(prefix)) {
                return false;
            }
            bool ok = false;
            const int value = argument.mid(prefix.size()).toInt(&ok);
            if (!ok) {
                return false;
            }
            *destination = value;
            return true;
        };
        if (!parseInteger(QStringLiteral("--bitrate="),
                          &profile.opusBitrateBps) &&
            !parseInteger(QStringLiteral("--frame-ms="),
                          &profile.opusFrameDurationMs) &&
            !parseInteger(QStringLiteral("--feed-ms="), &feedIntervalMs) &&
            !parseInteger(QStringLiteral("--prebuffer-ms="), &prebufferMs)) {
            std::fprintf(stderr, "invalid option: %s\n",
                         qUtf8Printable(argument));
            return 2;
        }
    }
    if (!RealtimeNativeRtcCarrier::IsProfileSupported(profile) ||
        feedIntervalMs <= 0 ||
        prebufferMs < 0 || prebufferMs % feedIntervalMs != 0 ||
        prebufferMs > 5000) {
        std::fprintf(stderr, "unsupported native RTC profile\n");
        return 2;
    }
    const QByteArray pcm = LoadWavPcm(QString::fromLocal8Bit(argv[1]));
    if (pcm.isEmpty()) {
        std::fprintf(stderr, "could not read PCM from %s\n", argv[1]);
        return 2;
    }
    std::printf("pcm: %lld bytes (%.1f s)\n",
                static_cast<long long>(pcm.size()),
                static_cast<double>(pcm.size()) / 96000.0);
    std::printf("profile: bitrate=%d frame=%dms feed=%dms prebuffer=%dms\n",
                profile.opusBitrateBps, profile.opusFrameDurationMs,
                feedIntervalMs, prebufferMs);
    const qint64 audioDurationMs =
        static_cast<qint64>(pcm.size()) * 1000 / 96000;
    const int overallTimeoutMs = static_cast<int>(std::min<qint64>(
        std::numeric_limits<int>::max(),
        audioDurationMs + kSetupAndDrainTimeoutMs));

    CodexRealtimeTranscriptionClient client;
    RealtimeNativeRtcCarrier carrier(profile);
    TranscriptionSessionController controller(&client, &carrier);
    controller.SetEnabled(true);

    int finals = 0;
    bool finished = false;
    bool completed = false;
    bool gapDetected = false;
    bool listening = false;
    QString unfinalizedPartial;
    QElapsedTimer streamElapsed;
    QObject::connect(
        &controller, &TranscriptionSessionController::StateChanged,
        [&completed, &listening, &streamElapsed](TranscriptionState state,
                                                 const QString& status) {
            completed = state == TranscriptionState::Completed;
            if (state == TranscriptionState::Listening) {
                // This harness validates the live media path, not negotiation
                // latency. Holding the finite WAV until readiness prevents a
                // short fixture ending before the service listens or a long
                // fixture overflowing production's intentional pre-roll cap.
                listening = true;
                streamElapsed.start();
            }
            std::printf("state=%d %s\n", static_cast<int>(state),
                        qUtf8Printable(status));
            std::fflush(stdout);
        });
    QObject::connect(
        &controller, &TranscriptionSessionController::SegmentFinalized,
        [&finals](const TranscriptSegment& segment) {
            ++finals;
            std::printf("FINAL #%llu: %s\n",
                        static_cast<unsigned long long>(segment.sequence),
                        qUtf8Printable(segment.text));
            std::fflush(stdout);
        });
    QObject::connect(
        &controller, &TranscriptionSessionController::PartialChanged,
        [&unfinalizedPartial, verbosePartials](const QString&, quint64,
                                              const QString& text) {
            unfinalizedPartial = text;
            if (verbosePartials && !text.isEmpty()) {
                std::printf("partial: %s\n", qUtf8Printable(text.right(60)));
                std::fflush(stdout);
            }
        });
    QObject::connect(
        &controller, &TranscriptionSessionController::GapDetected,
        [&gapDetected](const QString&, qint64, qint64) {
            gapDetected = true;
            std::printf("gap reported\n");
        });
    QObject::connect(
        &controller, &TranscriptionSessionController::QuotaChanged,
        [](int remainingPercent, bool known) {
            std::printf("quota: %d%% remaining (known=%d)\n", remainingPercent,
                        known ? 1 : 0);
        });
    QObject::connect(
        &controller, &TranscriptionSessionController::SessionFinished,
        [&application, &finished, &finals, &completed, &gapDetected,
         &unfinalizedPartial, &streamElapsed](const QString&) {
            if (finished) {
                return;
            }
            finished = true;
            std::printf("session finished; finals=%d\n", finals);
            if (!unfinalizedPartial.isEmpty()) {
                std::printf("UNFINALIZED: %s\n",
                            qUtf8Printable(unfinalizedPartial));
            }
            if (streamElapsed.isValid()) {
                std::printf("stream elapsed: %lld ms\n",
                            static_cast<long long>(streamElapsed.elapsed()));
            }
            std::fflush(stdout);
            application.exit(finals > 0 && completed && !gapDetected ? 0 : 1);
        });

    RecordingSessionInfo session;
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.timestampToken = QStringLiteral("liveprobe");
    controller.StartSession(session, QStringLiteral("en"));

    // Pace the capture-side feed independently of the carrier's RTP packet
    // size. A bounded startup cushion keeps ordinary timer jitter and console
    // work from starving the real-time sender; it is audio look-ahead, not an
    // API upload chunk. Production normally has negotiation pre-roll already
    // queued when the track opens, whereas this finite-file probe deliberately
    // waits for Listening so it cannot lose the beginning of its fixture.
    auto* feedTimer = new QTimer(&application);
    feedTimer->setTimerType(Qt::PreciseTimer);
    feedTimer->setInterval(feedIntervalMs);
    const int feedSamples = 48000 * feedIntervalMs / 1000;
    qint64 offset = 0;
    qint64 clock100ns = 0;
    int silenceRemainingMs = kMinimumTrailingSilenceMs;
    bool prebufferQueued = false;
    const auto enqueueFrame = [&]() {
        AudioFrame frame;
        frame.sampleRate = 48000;
        frame.channels = 1;
        frame.bitsPerSample = 16;
        frame.captureClock100ns = clock100ns;
        frame.duration100ns =
            static_cast<qint64>(feedSamples) * 10000000LL / 48000;
        clock100ns += frame.duration100ns;
        const qint64 remaining = pcm.size() - offset;
        if (remaining >= feedSamples * 2) {
            frame.pcm.assign(
                reinterpret_cast<const uint8_t*>(pcm.constData() + offset),
                reinterpret_cast<const uint8_t*>(pcm.constData() + offset +
                                                 feedSamples * 2));
            offset += feedSamples * 2;
        } else if (remaining > 0) {
            frame.pcm.assign(static_cast<std::size_t>(feedSamples) * 2, 0);
            std::memcpy(frame.pcm.data(), pcm.constData() + offset,
                        static_cast<std::size_t>(remaining));
            offset += remaining;
        } else if (silenceRemainingMs > 0) {
            frame.pcm.assign(static_cast<std::size_t>(feedSamples) * 2, 0);
            silenceRemainingMs -= feedIntervalMs;
        } else {
            return false;
        }
        return controller.TryEnqueueAudio(frame);
    };
    QObject::connect(feedTimer, &QTimer::timeout, [&]() {
        if (!listening) {
            return;
        }
        if (!prebufferQueued) {
            const int prebufferFrames = prebufferMs / feedIntervalMs;
            for (int index = 0; index < prebufferFrames; ++index) {
                if (!enqueueFrame()) {
                    std::fprintf(stderr, "prebuffer enqueue failed\n");
                    application.exit(4);
                    return;
                }
            }
            prebufferQueued = true;
        }
        if (!enqueueFrame()) {
            feedTimer->stop();
            std::printf("input exhausted; finishing\n");
            std::fflush(stdout);
            controller.FinishInput();
        }
    });
    feedTimer->start();

    QTimer::singleShot(overallTimeoutMs, &application, [&]() {
        if (!finished) {
            std::printf("timeout; finals=%d\n", finals);
            application.exit(3);
        }
    });

    return application.exec();
}
