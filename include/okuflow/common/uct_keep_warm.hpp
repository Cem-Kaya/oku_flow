#pragma once
// GPU clock keep-warm around paced reads (rounds 13 and 14 of
// docs/uct_okuread_port.md).
//
// The application reads on timers: 350 ms apart for advancing media, a 1500 ms
// refresh of a live view, 300 ms of settling after a view change. Between such
// reads the GPU's power management lowers its clocks and a sparse frame's read
// becomes several times slower. Keep-warm runs a short single-warp spin on the
// recognizer's own stream while a read is expected, so the clocks are up when
// it arrives. It trades power for latency, so it is a private switch and OFF by
// default:
//
//   OKUFLOW_UCT_TEST_KEEP_WARM        = 0 | change | continuous | preroll  (default off)
//   OKUFLOW_UCT_TEST_KEEP_WARM_SHAPE  = periodMs,busyUs,leadMs,idleMs[,windowMs,tailMs] (optional)
//
// change (round 14): warm from a view change (pan or zoom settling, a scene or
// slide change, a new source, resume, text reading switched on) until the read
// that follows it and its follow-ups have completed, plus a short tail; a
// periodic refresh of an unchanged view never warms. continuous (round 13,
// unchanged): warm whenever a paced read was announced, until no read has been
// submitted for the idle time-out; meant for continuously changing media.
// preroll (round 13): warm only from `lead` before an announced read.
//
// This header is the pure decision logic; RecognitionScheduler owns one under
// its mutex and its worker issues the launches. Nothing here touches the GPU
// or queries the system.
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace okuflow::uct {

enum class KeepWarmMode : std::uint8_t {
    Off,
    Continuous, // warm whenever armed, between reads, until the idle time-out
    PreRoll,    // warm only from `lead` before the announced next read
    Change,     // warm from a view change until its reads and a tail completed
};

struct KeepWarmSettings {
    KeepWarmMode mode{KeepWarmMode::Off};
    // Defaults from the round 13 and 14 characterization (RTX 4090): the
    // clocks hold at 40 percent or more time utilization and step up from the
    // lowest state in about 180 to 300 ms whenever a kernel is resident all
    // the time (one napping warp climbs as fast as every SM spinning). The
    // worker's timed wait is only as fine as the Windows timer tick (15.6 ms),
    // longer than one spin, so at each wake it queues a second spin behind the
    // one it launches: 20 ms of spins per wake keep the stream occupied about
    // 94 percent of the time (one spin per wake measured 67 percent, which
    // holds the clocks but raises them too slowly). The spin length also
    // bounds how long a context-wide CUDA call made on another thread
    // waits behind keep-warm: cudaFree waits for the running and the queued
    // spin (at most 17.8 ms measured with 10 ms spins, 99 ms with round 13's
    // 50 ms), cudaFreeHost for about one spin; cudaMalloc and cudaMallocHost
    // do not wait (evidence r14-clock-1).
    std::chrono::milliseconds period{5};        // the worker tops up at most once per period
    std::chrono::microseconds busy{10000};      // length of one spin
    std::chrono::milliseconds lead{350};        // pre-roll: start this long before the read is due
    std::chrono::milliseconds idleTimeout{2500};// no read submitted for this long: stop until re-armed
    // Change mode: how long after the latest view change its read may still
    // arrive (settling, probe stability, a busy worker), and how long warming
    // continues after the last read of the change finished, for follow-ups
    // (words pending, a second pass) that are submitted within it.
    std::chrono::milliseconds changeWindow{1500};
    std::chrono::milliseconds tail{400};
    bool Enabled() const noexcept { return mode != KeepWarmMode::Off; }
};

// Longest spin the switch accepts. The kernel itself refuses anything over its
// own cap (gpu::KeepWarm), which stays far below the Windows GPU watchdog.
inline constexpr std::chrono::microseconds kKeepWarmMaximumBusy{250000};

struct KeepWarmParse {
    KeepWarmSettings settings; // Off when the text is empty, "0" or invalid
    QString error;             // non-empty when the text was invalid
};

// Strict parser for the two private variables. Bounds: period 1-1000 ms,
// busy 10 us to 250 ms, lead 0-2000 ms, idle time-out 100-10000 ms, change
// window 100-10000 ms, tail 0-5000 ms.
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
    else if (mode == QLatin1String("change")) settings.mode = KeepWarmMode::Change;
    else {
        result.error = QStringLiteral("OKUFLOW_UCT_TEST_KEEP_WARM accepts only 0, change, continuous or preroll.");
        return result;
    }
    if (!shape.isEmpty()) {
        const auto parts = shape.split(QLatin1Char(','));
        long long values[6]{};
        bool ok = parts.size() == 4 || parts.size() == 6;
        for (int i = 0; ok && i < parts.size(); ++i) {
            for (const QChar c : parts[i]) ok = ok && c >= QLatin1Char('0') && c <= QLatin1Char('9');
            ok = ok && !parts[i].isEmpty() && parts[i].size() <= 7;
            if (ok) values[i] = parts[i].toLongLong();
        }
        ok = ok && values[0] >= 1 && values[0] <= 1000 && values[1] >= 10 && values[1] <= kKeepWarmMaximumBusy.count() &&
             values[2] >= 0 && values[2] <= 2000 && values[3] >= 100 && values[3] <= 10000;
        if (ok && parts.size() == 6)
            ok = values[4] >= 100 && values[4] <= 10000 && values[5] >= 0 && values[5] <= 5000;
        if (!ok) {
            result.error = QStringLiteral("OKUFLOW_UCT_TEST_KEEP_WARM_SHAPE must be periodMs,busyUs,leadMs,idleMs[,windowMs,tailMs] "
                                          "with period 1-1000, busy 10-250000, lead 0-2000, idle 100-10000, "
                                          "window 100-10000, tail 0-5000.");
            return result;
        }
        settings.period = std::chrono::milliseconds(values[0]);
        settings.busy = std::chrono::microseconds(values[1]);
        settings.lead = std::chrono::milliseconds(values[2]);
        settings.idleTimeout = std::chrono::milliseconds(values[3]);
        if (parts.size() == 6) {
            settings.changeWindow = std::chrono::milliseconds(values[4]);
            settings.tail = std::chrono::milliseconds(values[5]);
        }
    }
    result.settings = settings;
    return result;
}

inline KeepWarmParse KeepWarmFromEnvironment()
{
    return ParseKeepWarm(qEnvironmentVariable("OKUFLOW_UCT_TEST_KEEP_WARM"),
                         qEnvironmentVariable("OKUFLOW_UCT_TEST_KEEP_WARM_SHAPE"));
}

// When to launch.
// - continuous and preroll are armed by an announced paced read (ExpectRead).
//   Ordinary submissions and view changes never arm them. They stop once no
//   read has been submitted for the idle time-out.
// - change is armed only by a view change (ViewChanged); announcements never
//   arm or extend it, so a periodic refresh of an unchanged view stays cold.
//   It stops `tail` after the last read that finished while it was armed,
//   once the change's own read has been submitted, or `changeWindow` after the
//   latest change when no read followed.
// Never due while a read runs, while the view is not visible, while the power
// gate refuses, or after Disarm (cancel, pause, source loss, shutdown). A
// pre-roll warms only inside [due - lead, due + grace], grace = max(lead,
// 100 ms), which covers a read that arrives a little late.
class KeepWarmPolicy {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    // Consulted before every launch, under the scheduler's mutex: must be cheap
    // and must not block. Empty means allowed. A later user-visible setting
    // (for example "not on battery") supplies one; the policy never asks the
    // system itself.
    using PowerGate = std::function<bool()>;

    explicit KeepWarmPolicy(KeepWarmSettings settings = {}) : settings_(settings) {}
    // Replaces the settings and disarms; visibility and the power gate stay.
    void Configure(KeepWarmSettings settings)
    {
        const bool visible = visible_;
        auto gate = std::move(gate_);
        *this = KeepWarmPolicy(settings);
        visible_ = visible;
        gate_ = std::move(gate);
    }
    const KeepWarmSettings& Settings() const noexcept { return settings_; }
    bool Armed() const noexcept { return armed_; }
    bool Reading() const noexcept { return reading_; }
    bool Visible() const noexcept { return visible_; }
    std::optional<TimePoint> Expected() const noexcept { return expected_; }
    // Change mode: the time warming ends unless a change or read extends it.
    std::optional<TimePoint> ChangeUntil() const noexcept
    {
        if (!armed_ || settings_.mode != KeepWarmMode::Change) return std::nullopt;
        return changeUntil_;
    }
    bool AwaitingChangeRead() const noexcept { return awaitingChangeRead_; }

    void SetPowerGate(PowerGate gate) { gate_ = std::move(gate); }
    bool PowerAllowed() const { return !gate_ || gate_(); }

    // Text reading that is not visible (hidden or minimized window) never
    // warms: hiding disarms and drops any announcement; showing again arms
    // nothing, the next announced read or view change does.
    void SetVisible(bool visible) noexcept
    {
        visible_ = visible;
        if (!visible) Disarm();
    }

    // A paced reader announces its next read. Arming starts the idle clock;
    // repeated announcements without submissions do not extend it. Change
    // mode ignores announcements.
    void ExpectRead(TimePoint due, TimePoint now)
    {
        if (!settings_.Enabled() || !visible_ || settings_.mode == KeepWarmMode::Change) return;
        if (!armed_) {
            armed_ = true;
            activity_ = now;
        }
        expected_ = due;
    }
    // The view changed and a read will follow. Arms change mode only:
    // continuous and pre-roll keep round 13's arming by announcements.
    void ViewChanged(TimePoint now)
    {
        if (settings_.mode != KeepWarmMode::Change || !visible_) return;
        if (!armed_) changeUntil_ = now;
        armed_ = true;
        awaitingChangeRead_ = true;
        changeUntil_ = std::max(changeUntil_, now + settings_.changeWindow);
    }
    void Disarm() noexcept
    {
        armed_ = false;
        awaitingChangeRead_ = false;
        expected_.reset();
    }
    // Any submitted read keeps an armed continuous or pre-roll policy alive.
    // Only a full read (`fulfils`: not a regional pass, not a formula) from the
    // pre-roll start on fulfils the announced read, and only a full read is
    // the read a view change waits for.
    void ReadSubmitted(TimePoint now, bool fulfils = true)
    {
        activity_ = std::max(activity_, now);
        if (!fulfils) return;
        if (expected_ && now >= *expected_ - settings_.lead) expected_.reset();
        if (armed_ && settings_.mode == KeepWarmMode::Change) awaitingChangeRead_ = false;
    }
    void ReadStarted() noexcept { reading_ = true; }
    void ReadFinished(TimePoint now) noexcept
    {
        reading_ = false;
        if (!armed_ || settings_.mode != KeepWarmMode::Change) return;
        // Follow-ups get the tail. While a newer change still waits for its
        // read, its window stays as well.
        changeUntil_ = awaitingChangeRead_ ? std::max(changeUntil_, now + settings_.tail) : now + settings_.tail;
    }
    void ReadFinished() noexcept { ReadFinished(Clock::now()); }
    // An armed policy whose time is up (and no read runs): stop until re-armed.
    bool TimedOut(TimePoint now) const
    {
        if (!armed_) return false;
        if (settings_.mode == KeepWarmMode::Change) return !reading_ && now > changeUntil_;
        return now - activity_ > settings_.idleTimeout;
    }
    // When an armed policy times out unless something extends it; the worker
    // wakes then even when no launch is due, so it disarms on time.
    std::optional<TimePoint> IdleDeadline() const
    {
        if (!armed_) return std::nullopt;
        if (settings_.mode == KeepWarmMode::Change) return changeUntil_ + std::chrono::milliseconds(1);
        return activity_ + settings_.idleTimeout + std::chrono::milliseconds(1);
    }
    // The earliest time a launch may be due, or nothing: then wait for work.
    std::optional<TimePoint> NextWarm(TimePoint now) const
    {
        if (!settings_.Enabled() || !armed_ || !visible_ || reading_ || TimedOut(now) || !PowerAllowed())
            return std::nullopt;
        TimePoint earliest = now;
        if (warmed_) earliest = std::max(earliest, lastWarm_ + settings_.period);
        if (settings_.mode == KeepWarmMode::PreRoll) {
            if (!expected_) return std::nullopt;
            if (now > WindowEnd()) return std::nullopt;
            earliest = std::max(earliest, *expected_ - settings_.lead);
            if (earliest > WindowEnd()) return std::nullopt;
        }
        // The time-out bounds every wait for a launch.
        if (settings_.mode == KeepWarmMode::Change) {
            if (earliest > changeUntil_) return std::nullopt;
        } else if (earliest > activity_ + settings_.idleTimeout) {
            return std::nullopt;
        }
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
    // pre-roll or change launch never outlasts its window (a spin queued
    // behind the running one can still end up to one spin later).
    std::chrono::microseconds Busy(TimePoint now) const
    {
        auto busy = settings_.busy;
        std::optional<TimePoint> end;
        if (settings_.mode == KeepWarmMode::PreRoll && expected_) end = WindowEnd();
        if (settings_.mode == KeepWarmMode::Change && armed_) end = changeUntil_;
        if (end) {
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(*end - now);
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
    bool armed_{}, reading_{}, warmed_{}, visible_{true}, awaitingChangeRead_{};
    std::optional<TimePoint> expected_;
    TimePoint activity_{}, lastWarm_{}, changeUntil_{};
    PowerGate gate_;
};

} // namespace okuflow::uct
