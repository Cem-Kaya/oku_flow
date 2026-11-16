#pragma once
// Model-free checks of the recognizer load beside the detector (round 11,
// src/common/uct_preparation_overlap.hpp): for every outcome of the load, with
// the detector side succeeding, failing or being cancelled, and with the
// helper ending before, after or at the same time as the preparation thread
// arrives, the latch, the memo decision and the outcome handed to the
// original code equal what the sequential code decides.
#include "../src/common/uct_preparation_overlap.hpp"
#include "../src/common/uct_runtime_precision.hpp"
#include "../src/common/uct_tensorrt_failures.hpp"

#include <QtTest>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace uct_preparation_overlap_test
{
using namespace okuflow::uct;
using preparation_overlap::Adoption;
using preparation_overlap::Ahead;
using preparation_overlap::Steps;
using Clock = std::chrono::steady_clock;

// How the helper's load ends: through its own work, or because it saw stop.
enum class Load { Loaded, NeedsBuild, Refused, Interrupted, Fault, OutOfMemory, Unknown };
// When the helper ends relative to the preparation thread's arrival at the
// recognizer point (or at its own failure).
enum class Timing { HelperFirst, MainFirst, Together };
// What the detector side does before the recognizer point.
enum class Detector { Ready, Fails, Cancelled };

constexpr const char* kKey = "runtime-recognizer:test";

// The process latch and the memo store, as the production steps use them.
struct World {
    std::mutex mutex;
    std::set<std::string> latch;
    bool memo{};
    int begins{}, releases{}, memoReads{};
    bool Held()
    {
        std::lock_guard lock(mutex);
        return latch.count(kKey) != 0;
    }
};

// The recognizer stage's outcome as the original code receives it.
struct Result {
    enum class Path { Sequential, Check, Build, Rethrown, Refused, NotReached } path{Path::NotReached};
    std::string failure; // type of a rethrown or refused exception
    bool latchHeld{};
    bool operator==(const Result& other) const
    {
        return path == other.path && failure == other.failure && latchHeld == other.latchHeld;
    }
};

inline std::string TypeOf(const std::exception_ptr& failure)
{
    try { std::rethrow_exception(failure); }
    catch (const detail::VerificationInterrupted&) { return "interrupted"; }
    catch (const detail::CudaFailure&) { return "cuda"; }
    catch (const runtime_precision::ArtifactError&) { return "artifact"; }
    catch (const std::bad_alloc&) { return "bad_alloc"; }
    catch (const std::exception&) { return "other"; }
    catch (...) { return "unknown"; }
}

inline void Throw(Load load)
{
    switch (load) {
    case Load::Refused: throw runtime_precision::ArtifactError("plan refused");
    case Load::Interrupted: throw detail::VerificationInterrupted("stopped while verifying");
    case Load::Fault: throw detail::CudaFailure("device fault");
    case Load::OutOfMemory: throw std::bad_alloc();
    case Load::Unknown: throw 7;
    default: return;
    }
}

inline Steps MakeSteps(World& world, std::function<bool(const std::function<bool()>&)> load)
{
    return {[&world] {
                std::lock_guard lock(world.mutex);
                ++world.memoReads;
                if (world.memo) throw runtime_precision::ArtifactError("previously refused");
            },
            [&world] {
                std::lock_guard lock(world.mutex);
                ++world.begins;
                return world.latch.insert(kKey).second;
            },
            [&world] {
                std::lock_guard lock(world.mutex);
                ++world.releases;
                world.latch.erase(kKey);
            },
            std::move(load)};
}

// The original code at the recognizer point: memo, latch, then the load in place.
inline Result Sequential(World& world, Load load, bool cancelled)
{
    Result result;
    try {
        {
            std::lock_guard lock(world.mutex);
            if (world.memo) throw runtime_precision::ArtifactError("previously refused");
        }
        {
            std::lock_guard lock(world.mutex);
            if (!world.latch.insert(kKey).second) throw runtime_precision::ArtifactError("already in flight");
        }
        // A cancelled hash stops verification as VerificationInterrupted.
        if (cancelled) throw detail::VerificationInterrupted("stopped while verifying");
        Throw(load);
        result.path = load == Load::NeedsBuild ? Result::Path::Build : Result::Path::Check;
    } catch (const runtime_precision::ArtifactError& error) {
        // Distinguish a refusal before the latch from one thrown by the load.
        result.path = std::string(error.what()) == "plan refused" ? Result::Path::Rethrown : Result::Path::Refused;
        result.failure = "artifact";
    } catch (...) {
        result.path = Result::Path::Rethrown;
        result.failure = TypeOf(std::current_exception());
    }
    result.latchHeld = world.Held();
    return result;
}

struct Scenario {
    Load load{Load::Loaded};
    Timing timing{Timing::HelperFirst};
    Detector detector{Detector::Ready};
    bool memoAtStart{}, memoAtRecognizer{}, heldElsewhere{};
};

struct Observed {
    Result result;
    bool loadRan{}, sawStop{}, helperDone{};
};

// Runs the production unit the way BeginPreparation drives it.
inline Observed Overlapped(const Scenario& scenario)
{
    World world;
    world.memo = scenario.memoAtStart;
    if (scenario.heldElsewhere) world.latch.insert(kKey);
    std::atomic<bool> cancel{}, mainArrived{}, helperArrived{}, loadRan{}, sawStop{}, helperDone{};
    Observed observed;
    {
        Ahead ahead;
        const auto stopped = [&cancel] { return cancel.load(); };
        const bool started = ahead.Start(MakeSteps(world, [&](const std::function<bool()>& stop) {
            loadRan = true;
            helperArrived = true;
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            if (scenario.timing == Timing::MainFirst || scenario.timing == Timing::Together) {
                // Wait for the preparation thread (or for stop).
                while (!mainArrived && !stop() && Clock::now() < deadline) std::this_thread::yield();
                if (scenario.timing == Timing::MainFirst)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            // A hash in progress returns at stop; a vendor call runs to its end.
            if (stop()) {
                sawStop = true;
                helperDone = true;
                throw detail::VerificationInterrupted("stopped while verifying");
            }
            helperDone = true;
            Throw(scenario.load);
            return scenario.load == Load::NeedsBuild;
        }), stopped);
        if (scenario.timing == Timing::HelperFirst && started) {
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (!helperDone && Clock::now() < deadline) std::this_thread::yield();
        }
        if (scenario.timing == Timing::Together && started) {
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (!helperArrived && Clock::now() < deadline) std::this_thread::yield();
        }
        {
            std::lock_guard lock(world.mutex);
            world.memo = scenario.memoAtRecognizer;
        }
        if (scenario.detector == Detector::Cancelled) cancel = true;
        mainArrived = true;
        if (scenario.detector == Detector::Ready) {
            try {
                const auto adoption = ahead.Adopt();
                switch (adoption.kind) {
                case Adoption::Kind::NotStarted: {
                    // The original code runs unchanged at this point.
                    observed.result = Sequential(world, scenario.load, false);
                    break;
                }
                case Adoption::Kind::Loaded: observed.result.path = Result::Path::Check; break;
                case Adoption::Kind::NeedsBuild: observed.result.path = Result::Path::Build; break;
                case Adoption::Kind::Failed:
                    observed.result.path = Result::Path::Rethrown;
                    observed.result.failure = TypeOf(adoption.failure);
                    break;
                }
            } catch (const runtime_precision::ArtifactError&) {
                observed.result.path = Result::Path::Refused;
                observed.result.failure = "artifact";
            }
            observed.result.latchHeld = world.Held();
        }
    } // a failed or cancelled detector side: destruction stops, joins, releases
    if (scenario.detector != Detector::Ready) observed.result.latchHeld = world.Held();
    observed.loadRan = loadRan;
    observed.sawStop = sawStop;
    observed.helperDone = helperDone || !loadRan;
    return observed;
}

// What the sequential code decides for the same scenario.
inline Result Expected(const Scenario& scenario)
{
    if (scenario.detector != Detector::Ready) {
        // The recognizer point is never reached: no latch of this preparation.
        Result result;
        result.latchHeld = scenario.heldElsewhere;
        return result;
    }
    World world;
    world.memo = scenario.memoAtRecognizer;
    if (scenario.heldElsewhere) world.latch.insert(kKey);
    return Sequential(world, scenario.load, false);
}

inline const char* Name(Load load)
{
    switch (load) {
    case Load::Loaded: return "loaded";
    case Load::NeedsBuild: return "needs-build";
    case Load::Refused: return "refused";
    case Load::Interrupted: return "interrupted";
    case Load::Fault: return "fault";
    case Load::OutOfMemory: return "bad_alloc";
    case Load::Unknown: return "unknown";
    }
    return "?";
}

inline QByteArray Describe(const Scenario& s, const Result& got, const Result& want)
{
    return QByteArray("load=") + Name(s.load) + " timing=" + QByteArray::number(int(s.timing)) +
           " detector=" + QByteArray::number(int(s.detector)) + " memoStart=" + QByteArray::number(s.memoAtStart) +
           " memoRecognizer=" + QByteArray::number(s.memoAtRecognizer) + " heldElsewhere=" +
           QByteArray::number(s.heldElsewhere) + " got path=" + QByteArray::number(int(got.path)) + " " +
           QByteArray::fromStdString(got.failure) + " held=" + QByteArray::number(got.latchHeld) +
           " want path=" + QByteArray::number(int(want.path)) + " " + QByteArray::fromStdString(want.failure) +
           " held=" + QByteArray::number(want.latchHeld);
}

inline void MatchesSequential()
{
    int scenarios = 0;
    for (const auto load : {Load::Loaded, Load::NeedsBuild, Load::Refused, Load::Interrupted, Load::Fault,
                            Load::OutOfMemory, Load::Unknown})
    for (const auto timing : {Timing::HelperFirst, Timing::MainFirst, Timing::Together})
    for (const auto detector : {Detector::Ready, Detector::Fails, Detector::Cancelled})
    for (const bool memoAtStart : {false, true})
    for (const bool memoAtRecognizer : {false, true})
    for (const bool heldElsewhere : {false, true}) {
        const Scenario scenario{load, timing, detector, memoAtStart, memoAtRecognizer, heldElsewhere};
        const auto observed = Overlapped(scenario);
        const auto expected = Expected(scenario);
        QVERIFY2(observed.result == expected, Describe(scenario, observed.result, expected).constData());
        // No helper without a free latch and no memo at the start.
        QCOMPARE(observed.loadRan, !memoAtStart && !heldElsewhere);
        // The helper ended before the unit was gone, on every path.
        QVERIFY(observed.helperDone);
        ++scenarios;
    }
    QCOMPARE(scenarios, 7 * 3 * 3 * 2 * 2 * 2);
}

// A failed or cancelled detector side stops a load that is still waiting and
// joins it; the stop seen by the load stays callable after the unit is gone.
inline void AbandonStopsJoinsAndReleases()
{
    World world;
    std::function<bool()> kept;
    std::atomic<bool> finished{};
    const auto started = Clock::now();
    {
        Ahead ahead;
        QVERIFY(ahead.Start(MakeSteps(world, [&](const std::function<bool()>& stop) {
            kept = stop; // as a Network keeps its build progress callback
            while (!stop()) std::this_thread::yield();
            finished = true;
            if (stop()) throw detail::VerificationInterrupted("stopped while verifying");
            return false;
        }), [] { return false; }));
        QVERIFY(world.Held());
    }
    QVERIFY(finished);
    QVERIFY(!world.Held());
    QCOMPARE(world.releases, 1);
    QVERIFY(kept && kept()); // abandoned stays true and safe to read
    QVERIFY(Clock::now() - started < std::chrono::seconds(5));

    // The caller's own stop (cancellation, deadline) reaches the load too.
    World cancelled;
    std::atomic<bool> cancel{};
    {
        Ahead ahead;
        QVERIFY(ahead.Start(MakeSteps(cancelled, [&](const std::function<bool()>& stop) {
            cancel = true;
            if (stop()) throw detail::VerificationInterrupted("stopped while verifying");
            return false;
        }), [&cancel] { return cancel.load(); }));
        const auto adoption = ahead.Adopt();
        QCOMPARE(adoption.kind, Adoption::Kind::Failed);
        QCOMPARE(TypeOf(adoption.failure), std::string("interrupted"));
        QVERIFY(cancelled.Held()); // handed to the caller, whose cancellation rule releases it
    }
    QVERIFY(cancelled.Held()); // adopted: never released by the unit
    QCOMPARE(cancelled.releases, 0);

    // Adopt twice, or after NotStarted, changes nothing.
    World again;
    {
        Ahead ahead;
        QVERIFY(ahead.Start(MakeSteps(again, [](const std::function<bool()>&) { return false; }), [] { return false; }));
        QCOMPARE(ahead.Adopt().kind, Adoption::Kind::Loaded);
        QCOMPARE(ahead.Adopt().kind, Adoption::Kind::NotStarted);
        QVERIFY(!ahead.Start(MakeSteps(again, [](const std::function<bool()>&) { return false; }), [] { return false; }));
    }
    QVERIFY(again.Held());
    QCOMPARE(again.begins, 1);
}
} // namespace uct_preparation_overlap_test
