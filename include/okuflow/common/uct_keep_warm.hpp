#pragma once
// GPU clock keep-warm between paced reads (round 13 of docs/uct_okuread_port.md).
//
// The application reads on timers: 350 ms apart for advancing media, a 1500 ms
// refresh of a live view. Between such reads the GPU's power management lowers
// its clocks and a sparse frame's read becomes several times slower. Keep-warm
// runs a short single-warp spin on the recognizer's own stream while a paced
// read is expected, so the clocks are still up when it arrives. It trades power
// for latency, so it is a private switch and OFF by default:
//
//   OKUFLOW_UCT_TEST_KEEP_WARM        = 0 | continuous | preroll   (default off)
//   OKUFLOW_UCT_TEST_KEEP_WARM_SHAPE  = periodMs,busyUs,leadMs,idleMs (optional)
//
// This header is the pure decision logic; RecognitionScheduler owns one under
// its mutex and its worker issues the launches. Nothing here touches the GPU.
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace okuflow::uct {

enum class KeepWarmMode : std::uint8_t {
    Off,
    Continuous, // warm whenever armed, between reads, until the idle time-out
    PreRoll,    // warm only from `lead` before the announced next read
};

struct KeepWarmSettings {
    KeepWarmMode mode{KeepWarmMode::Off};
    std::chrono::milliseconds period{16};       // one launch per period at most
    std::chrono::microseconds busy{4000};       // spin of one launch; never longer than the period
    std::chrono::milliseconds lead{200};        // pre-roll: start this long before the read is due
    std::chrono::milliseconds idleTimeout{2500};// no read submitted for this long: stop until re-armed
    bool Enabled() const noexcept { return mode != KeepWarmMode::Off; }
};

struct KeepWarmParse {
    KeepWarmSettings settings; // Off when the text is empty, "0" or invalid
    QString error;             // non-empty when the text was invalid
};

// Strict parser for the two private variables. Bounds: period 1-1000 ms,
// busy 10 us to the period, lead 0-2000 ms, idle time-out 100-10000 ms.
inline KeepWarmParse ParseKeepWarm(const QString& mode, const QString& shape)
{
    KeepWarmParse result;
    if (mode.isEmpty() || mode == QLatin1String("0")) {
        if (!shape.isEmpty()) result.error = QStringLiteral("OKUFLOW_UCT_TEST_KEEP_WARM_SHAPE needs OKUFLOW_UCT_TEST_KEEP_WARM.");
        return result;
    }
    KeepWarmSettings settings;
    if (mode == QLatin1String("continuous")) settings.mode = KeepWarmMode::Continuous;
    else if (mode == QLatin1String("preroll")) settings.mode = KeepWarmMode::PreRoll;
    else {
        result.error = QStringLiteral("OKUFLOW_UCT_TEST_KEEP_WARM accepts only 0, continuous or preroll.");
        return result;
    }
    if (!shape.isEmpty()) {
        const auto parts = shape.split(QLatin1Char(','));
        long long values[4]{};
        bool ok = parts.size() == 4;
        for (int i = 0; ok && i < 4; ++i) {
            for (const QChar c : parts[i]) ok = ok && c >= QLatin1Char('0') && c <= QLatin1Char('9');
            ok = ok && !parts[i].isEmpty() && parts[i].size() <= 6;
            if (ok) values[i] = parts[i].toLongLong();
        }
        ok = ok && values[0] >= 1 && values[0] <= 1000 && values[1] >= 10 && values[1] <= values[0] * 1000 &&
             values[2] >= 0 && values[2] <= 2000 && values[3] >= 100 && values[3] <= 10000;
        if (!ok) {
            result.error = QStringLiteral("OKUFLOW_UCT_TEST_KEEP_WARM_SHAPE must be periodMs,busyUs,leadMs,idleMs "
                                          "with period 1-1000, busy 10 to the period, lead 0-2000, idle 100-10000.");
            return result;
        }
        settings.period = std::chrono::milliseconds(values[0]);
        settings.busy = std::chrono::microseconds(values[1]);
        settings.lead = std::chrono::milliseconds(values[2]);
        settings.idleTimeout = std::chrono::milliseconds(values[3]);
    }
    result.settings = settings;
    return result;
}

inline KeepWarmParse KeepWarmFromEnvironment()
{
    return ParseKeepWarm(qEnvironmentVariable("OKUFLOW_UCT_TEST_KEEP_WARM"),
                         qEnvironmentVariable("OKUFLOW_UCT_TEST_KEEP_WARM_SHAPE"));
}

// When to launch. Armed only by an announced paced read (ExpectRead); ordinary
// submissions never arm it. Never due while a read runs, after Disarm (cancel,
// pause, source loss, shutdown) or once no read has been submitted for the
// idle time-out. A pre-roll warms only inside [due - lead, due + grace], where
// grace = max(lead, 100 ms) covers a read that arrives a little late.
class KeepWarmPolicy {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit KeepWarmPolicy(KeepWarmSettings settings = {}) : settings_(settings) {}
    void Configure(KeepWarmSettings settings)
    {
        *this = KeepWarmPolicy(settings);
    }
    const KeepWarmSettings& Settings() const noexcept { return settings_; }
    bool Armed() const noexcept { return armed_; }
    bool Reading() const noexcept { return reading_; }
    std::optional<TimePoint> Expected() const noexcept { return expected_; }

    // A paced reader announces its next read. Arming starts the idle clock;
    // repeated announcements without submissions do not extend it.
    void ExpectRead(TimePoint due, TimePoint now)
    {
        if (!settings_.Enabled()) return;
        if (!armed_) {
            armed_ = true;
            activity_ = now;
        }
        expected_ = due;
    }
    void Disarm() noexcept
    {
        armed_ = false;
        expected_.reset();
    }
    // Any submitted read keeps an armed policy alive. It fulfils the announced
    // read only from the pre-roll start on; an earlier, unrelated read (a
    // regional pass, say) leaves the announcement in place.
    void ReadSubmitted(TimePoint now)
    {
        activity_ = std::max(activity_, now);
        if (expected_ && now >= *expected_ - settings_.lead) expected_.reset();
    }
    void ReadStarted() noexcept { reading_ = true; }
    void ReadFinished() noexcept { reading_ = false; }
    bool TimedOut(TimePoint now) const
    {
        return armed_ && now - activity_ > settings_.idleTimeout;
    }
    // When an armed policy times out unless a read is submitted first; the
    // worker wakes then even when no launch is due, so it disarms on time.
    std::optional<TimePoint> IdleDeadline() const
    {
        if (!armed_) return std::nullopt;
        return activity_ + settings_.idleTimeout + std::chrono::milliseconds(1);
    }
    // The earliest time a launch may be due, or nothing: then wait for work.
    std::optional<TimePoint> NextWarm(TimePoint now) const
    {
        if (!settings_.Enabled() || !armed_ || reading_ || TimedOut(now)) return std::nullopt;
        TimePoint earliest = now;
        if (warmed_) earliest = std::max(earliest, lastWarm_ + settings_.period);
        if (settings_.mode == KeepWarmMode::PreRoll) {
            if (!expected_) return std::nullopt;
            if (now > WindowEnd()) return std::nullopt;
            earliest = std::max(earliest, *expected_ - settings_.lead);
            if (earliest > WindowEnd()) return std::nullopt;
        }
        // The idle time-out bounds every wait for a launch.
        if (earliest > activity_ + settings_.idleTimeout) return std::nullopt;
        return earliest;
    }
    bool Due(TimePoint now) const
    {
        const auto next = NextWarm(now);
        return next && *next <= now;
    }
    void Warmed(TimePoint now)
    {
        warmed_ = true;
        lastWarm_ = now;
    }
    // How long one launch may spin: the configured busy time, cut so that a
    // pre-roll launch never outlasts its window.
    std::chrono::microseconds Busy(TimePoint now) const
    {
        auto busy = settings_.busy;
        if (settings_.mode == KeepWarmMode::PreRoll && expected_) {
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(WindowEnd() - now);
            busy = std::min(busy, std::max(left, std::chrono::microseconds(10)));
        }
        return busy;
    }

private:
    TimePoint WindowEnd() const
    {
        return *expected_ + std::max<std::chrono::milliseconds>(settings_.lead, std::chrono::milliseconds(100));
    }
    KeepWarmSettings settings_;
    bool armed_{}, reading_{}, warmed_{};
    std::optional<TimePoint> expected_;
    TimePoint activity_{}, lastWarm_{};
};

} // namespace okuflow::uct
