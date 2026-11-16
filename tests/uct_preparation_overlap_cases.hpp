#pragma once
// Model-free checks of the recognizer load beside the detector (round 11 and
// its follow-up, src/common/uct_preparation_overlap.hpp).
//
// The driver below has the shape of the production preparation thread
// (TensorRtRecognizer::BeginPreparation and PrepareRuntimeRecognizer in
// uct_tensorrt.cpp): it starts the helper, runs the detector side through the
// production `DetectorSide` (with `Settle` before a detector build and before
// the detector's FP32 fallback), decides the recognizer through the production
// `Decide`, settles again after the recognizer point and refuses to publish an
// unresolved helper. Only the vendor calls and the unchanged catch bodies are
// fakes; they record the latch, the failure memos and the decision.
//
// Every scenario runs twice: with the helper, and without it (the production
// code with no helper is the sequential code of 6e454cc). The decision, the
// latch state and the memo writes must be equal. A helper that actually ended
// in a fault is the one deliberate exception, with its own rule: the fault is
// raised (never discarded and never recorded against the detector), the
// recognizer latch is held as after a terminal fault in its own stage unless a
// memo or another holder refuses it, and no memo is written.
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
#include <vector>

namespace uct_preparation_overlap_test
{
using namespace okuflow::uct;
using preparation_overlap::Ahead;
using preparation_overlap::Point;
using Clock = std::chrono::steady_clock;

constexpr const char* kKey = "runtime-recognizer:test";
constexpr const char* kDetectorKey = "detector:test";

// How a load ends. Interrupted is a stopped hash: for the helper, its own
// deadline; for the original load, the sequential code's.
enum class Load { Loaded, NeedsBuild, Artifact, Runtime, Interrupted, Cuda, Api, OutOfMemory, Unknown };
// When the helper ends relative to the preparation thread's arrival where it
// takes or settles the helper.
enum class Timing { HelperFirst, MainFirst, Together };
// The detector side before the recognizer point.
enum class Detector { Ready, NeedsBuild, FallsBack, Fails, Cancelled };

// Ends met during the context creation, after the helper's gate; the others
// are met before it (verification, plan lookup, deserialization).
inline bool AfterGate(Load load)
{
    return load == Load::Loaded || load == Load::Api || load == Load::OutOfMemory;
}

inline bool IsFaultLoad(Load load)
{
    return load == Load::Cuda || load == Load::Api || load == Load::OutOfMemory || load == Load::Unknown;
}

inline void Throw(Load load)
{
    switch (load) {
    case Load::Artifact: throw runtime_precision::ArtifactError("plan refused");
    case Load::Runtime: throw std::runtime_error("plain runtime failure");
    case Load::Interrupted: throw detail::VerificationInterrupted("stopped while verifying");
    case Load::Cuda: throw detail::CudaFailure("device fault");
    case Load::Api: throw detail::TensorRtApiFailure("execution context creation failed");
    case Load::OutOfMemory: throw std::bad_alloc();
    case Load::Unknown: throw 7;
    default: return;
    }
}

// The process latch and the failure memos, shared by every preparation.
struct World {
    std::mutex mutex;
    std::set<std::string> latch, memos;
    std::vector<std::string> memoWrites;
    int begins{};
    bool Held(const std::string& key = kKey)
    {
        std::lock_guard lock(mutex);
        return latch.count(key) != 0;
    }
    bool Memo(const std::string& key)
    {
        std::lock_guard lock(mutex);
        return memos.count(key) != 0;
    }
    bool Begin(const std::string& key)
    {
        std::lock_guard lock(mutex);
        ++begins;
        return latch.insert(key).second;
    }
    void Erase(const std::string& key)
    {
        std::lock_guard lock(mutex);
        latch.erase(key);
    }
    void WriteMemo(const std::string& key)
    {
        std::lock_guard lock(mutex);
        memoWrites.push_back(key);
        memos.insert(key);
    }
};

// The parts of the State the recognizer point touches.
struct Fake {
    enum class Recognizer { None, Adopted, InPlace } recognizer{Recognizer::None};
    bool aheadNetwork{};   // State::recognizerAhead
    std::string owned;     // State::ownedAttempts[1]
    std::string ownedDetector;
};

inline Point MakePoint(World& world, Fake& state, std::function<void()> loadInPlace = {})
{
    return {[&world] { if (world.Memo(kKey)) throw runtime_precision::ArtifactError("previously refused"); },
            [&world] { return world.Begin(kKey); },
            [] { throw runtime_precision::ArtifactError("already failed or in flight"); },
            [&state] { state.owned = kKey; },
            [&state] {
                if (!state.aheadNetwork) throw std::logic_error("adopted without a helper network");
                state.aheadNetwork = false;
                state.recognizer = Fake::Recognizer::Adopted;
            },
            [&state] { state.aheadNetwork = false; },
            std::move(loadInPlace)};
}

struct Scenario {
    Load helper{Load::Loaded}, inPlace{Load::Loaded};
    Timing timing{Timing::HelperFirst};
    Detector detector{Detector::Ready};
    bool memoAtStart{}, memoAtRecognizer{}, heldElsewhere{}, cancelAtRecognizer{};
};

struct Observed {
    std::string decision; // bf16, fp32, cancelled, failed, fault
    std::string message;
    bool latchHeld{}, detectorLatchHeld{};
    std::vector<std::string> memoWrites;
    bool adopted{}, publishedClean{true}, aheadLeftAfterRefusal{};
    // Helper facts, for the expectation.
    bool helperRan{}, helperFaulted{}, helperStopped{};
    bool operator==(const Observed& other) const
    {
        return decision == other.decision && latchHeld == other.latchHeld &&
               detectorLatchHeld == other.detectorLatchHeld && memoWrites == other.memoWrites;
    }
};

inline std::string Classify(const std::exception_ptr& failure, bool cancelled)
{
    if (preparation_overlap::IsFault(failure)) return "fault";
    return cancelled ? "cancelled" : "failed";
}

// One preparation in the order of the production preparation thread.
inline Observed Prepare(World& world, const Scenario& s, bool helper)
{
    Observed observed;
    Fake state;
    std::atomic<bool> cancel{}, mainArrived{}, helperParked{};
    const auto cancelled = [&cancel] { return cancel.load(); };
    {
        std::lock_guard lock(world.mutex);
        if (s.memoAtStart) world.memos.insert(kKey);
        if (s.heldElsewhere) world.latch.insert(kKey);
    }
    const auto settlement = MakePoint(world, state);
    Ahead ahead;
    if (helper) {
        state.aheadNetwork = true;
        const bool started = ahead.Start(kKey,
            [&world] { if (world.Memo(kKey)) throw runtime_precision::ArtifactError("previously refused"); },
            [&](const std::function<bool()>& stop, const std::function<bool()>& gate) {
                observed.helperRan = true;
                if (s.timing != Timing::HelperFirst) {
                    const auto until = Clock::now() + std::chrono::seconds(5);
                    while (!mainArrived && !stop() && Clock::now() < until) std::this_thread::yield();
                    if (s.timing == Timing::MainFirst) {
                        const auto pause = Clock::now() + std::chrono::microseconds(300);
                        while (!stop() && Clock::now() < pause) std::this_thread::yield();
                    }
                }
                // A hash in progress returns at stop; a vendor call runs to its end.
                if (stop()) {
                    observed.helperStopped = true;
                    helperParked = true;
                    throw detail::VerificationInterrupted("stopped while verifying");
                }
                if (!AfterGate(s.helper)) {
                    observed.helperFaulted = IsFaultLoad(s.helper);
                    helperParked = true;
                    Throw(s.helper);
                    return s.helper == Load::NeedsBuild;
                }
                helperParked = true; // waits for the detector's context
                if (!gate()) {
                    observed.helperStopped = true;
                    throw detail::VerificationInterrupted("stopped before the execution context");
                }
                observed.helperFaulted = IsFaultLoad(s.helper);
                Throw(s.helper);
                return false;
            },
            cancelled);
        if (!started) state.aheadNetwork = false;
        // HelperFirst: everything the helper can do before the detector's
        // context exists is done before the preparation thread goes on.
        if (started && s.timing == Timing::HelperFirst) {
            const auto until = Clock::now() + std::chrono::seconds(5);
            while (!helperParked && Clock::now() < until) std::this_thread::yield();
        }
    }
    // The production cancellation release (outer handler, and the detector
    // side before it settles the helper).
    const auto releaseCancelled = [&](const std::exception_ptr& failure) {
        if (!cancelled() || !(detail::AllowsTensorRtCapacityFallback(failure) || detail::IsVerificationInterrupted(failure)))
            return;
        for (auto* key : {&state.owned, &state.ownedDetector})
            if (!key->empty()) { world.Erase(*key); key->clear(); }
    };
    const auto arrive = [&] {
        mainArrived = true;
        if (s.timing == Timing::Together) std::this_thread::yield();
    };
    try {
        preparation_overlap::DetectorSide(ahead, settlement, [&] {
            if (s.detector == Detector::Ready) {
                ahead.Open(); // the detector's context exists
                return;
            }
            if (s.detector == Detector::NeedsBuild) {
                arrive();
                if (ahead.Pending()) preparation_overlap::Settle(ahead, settlement); // before the build
                ahead.Open();
                return;
            }
            if (s.detector == Detector::FallsBack) {
                state.ownedDetector = kDetectorKey;
                world.Begin(kDetectorKey);
                try { throw runtime_precision::ArtifactError("detector plan refused"); }
                catch (const std::exception&) {
                    arrive();
                    preparation_overlap::Settle(ahead, settlement, std::current_exception());
                    if (cancelled()) throw std::runtime_error("detector cancelled before FP32 fallback");
                    world.WriteMemo(kDetectorKey);
                    state.ownedDetector.clear(); // its latch stays held, as in production
                }
                ahead.Open(); // the FP32 detector's context
                return;
            }
            state.ownedDetector = kDetectorKey;
            world.Begin(kDetectorKey);
            arrive();
            if (s.detector == Detector::Cancelled) {
                cancel = true;
                throw detail::VerificationInterrupted("detector stopped while verifying");
            }
            throw detail::CudaFailure("detector device fault");
        }, releaseCancelled);
        // The recognizer point.
        {
            std::lock_guard lock(world.mutex);
            if (s.memoAtRecognizer) world.memos.insert(kKey);
        }
        if (s.cancelAtRecognizer) cancel = true;
        arrive();
        try {
            preparation_overlap::Decide(&ahead, kKey, MakePoint(world, state, [&] {
                state.recognizer = Fake::Recognizer::InPlace;
                // The sequential code meets a fault the helper met at its load
                // before it sees a later cancellation.
                if (IsFaultLoad(s.inPlace)) Throw(s.inPlace);
                if (cancelled()) throw detail::VerificationInterrupted("stopped while verifying");
                Throw(s.inPlace);
            }));
            // Compatibility check and completion (ConfirmPublicCompletion).
            if (cancelled()) throw std::runtime_error("completion cancelled");
            world.Erase(kKey); // Passed
            state.owned.clear();
            observed.adopted = state.recognizer == Fake::Recognizer::Adopted;
            observed.decision = "bf16";
        } catch (const runtime_precision::ArtifactError&) {
            observed.aheadLeftAfterRefusal = state.aheadNetwork;
            if (cancelled()) throw std::runtime_error("recognizer cancelled before FP32 fallback");
            state.recognizer = Fake::Recognizer::None;
            state.aheadNetwork = false; // the production fallback resets it too
            if (!state.owned.empty()) world.WriteMemo(state.owned);
            state.owned.clear();
            observed.decision = "fp32";
        }
        preparation_overlap::Settle(ahead, settlement);
        observed.publishedClean = !ahead.Pending() && !state.aheadNetwork;
        if (!observed.publishedClean) throw std::logic_error("unresolved recognizer load at publication");
    } catch (...) {
        const auto failure = std::current_exception();
        observed.message = preparation_overlap::Message(failure);
        releaseCancelled(failure); // the production outer catch
        observed.decision = Classify(failure, cancelled());
    }
    (void)ahead.Abandon(); // resolved on every path above; makes the helper's writes visible
    observed.latchHeld = world.Held(kKey);
    observed.detectorLatchHeld = world.Held(kDetectorKey);
    observed.memoWrites = world.memoWrites;
    return observed;
}

inline const char* Name(Load load)
{
    switch (load) {
    case Load::Loaded: return "loaded";
    case Load::NeedsBuild: return "needs-build";
    case Load::Artifact: return "artifact";
    case Load::Runtime: return "runtime_error";
    case Load::Interrupted: return "interrupted";
    case Load::Cuda: return "cuda";
    case Load::Api: return "tensorrt-api";
    case Load::OutOfMemory: return "bad_alloc";
    case Load::Unknown: return "unknown";
    }
    return "?";
}

inline QByteArray Describe(const Scenario& s, const Observed& got, const Observed& want)
{
    const auto memos = [](const Observed& o) {
        QByteArray text;
        for (const auto& key : o.memoWrites) text += QByteArray::fromStdString(key) + ",";
        return text;
    };
    return QByteArray("helper=") + Name(s.helper) + " inPlace=" + Name(s.inPlace) +
           " timing=" + QByteArray::number(int(s.timing)) + " detector=" + QByteArray::number(int(s.detector)) +
           " memoStart=" + QByteArray::number(s.memoAtStart) + " memoRecognizer=" + QByteArray::number(s.memoAtRecognizer) +
           " heldElsewhere=" + QByteArray::number(s.heldElsewhere) + " cancelAtRecognizer=" +
           QByteArray::number(s.cancelAtRecognizer) + " helperFaulted=" + QByteArray::number(got.helperFaulted) +
           " | got " + QByteArray::fromStdString(got.decision) + " held=" + QByteArray::number(got.latchHeld) +
           " detectorHeld=" + QByteArray::number(got.detectorLatchHeld) + " memos=" + memos(got) +
           " (" + QByteArray::fromStdString(got.message) + ") | want " + QByteArray::fromStdString(want.decision) +
           " held=" + QByteArray::number(want.latchHeld) + " detectorHeld=" + QByteArray::number(want.detectorLatchHeld) +
           " memos=" + memos(want) + " (" + QByteArray::fromStdString(want.message) + ")";
}

// The rule for a helper that ended in a fault (see the file comment).
inline Observed AfterHelperFault(const Scenario& s)
{
    Observed want;
    want.decision = "fault";
    // Raised at the recognizer point (memo read there) or on the detector side
    // (memo read there, which is the start's: none, or no helper would run).
    const bool remembered = s.detector == Detector::Ready ? s.memoAtRecognizer : s.memoAtStart;
    want.latchHeld = s.heldElsewhere || !remembered;
    // A failed detector attempt stays held; a cancelled one was released by
    // the detector side before the helper was settled, as in the sequential order.
    want.detectorLatchHeld = s.detector == Detector::FallsBack || s.detector == Detector::Fails;
    return want;
}

inline void MatchesSequential()
{
    int scenarios = 0, adopted = 0, faultsRaised = 0, redone = 0;
    const auto loads = {Load::Loaded, Load::NeedsBuild, Load::Artifact, Load::Runtime, Load::Interrupted,
                        Load::Cuda, Load::Api, Load::OutOfMemory, Load::Unknown};
    for (const auto helper : loads)
    for (const auto inPlace : loads) {
        if (inPlace == Load::NeedsBuild) continue; // the original load builds in place
        // A loaded plan loads again; a fault is met by the sequential load too;
        // any other end of the helper is decided again by the original load.
        if (helper == Load::Loaded && inPlace != Load::Loaded) continue;
        if (IsFaultLoad(helper) && inPlace != helper) continue;
        for (const auto timing : {Timing::HelperFirst, Timing::MainFirst, Timing::Together})
        for (const auto detector : {Detector::Ready, Detector::NeedsBuild, Detector::FallsBack, Detector::Fails,
                                    Detector::Cancelled})
        for (const bool memoAtStart : {false, true})
        for (const bool memoAtRecognizer : {false, true})
        for (const bool heldElsewhere : {false, true})
        for (const bool cancelAtRecognizer : {false, true}) {
            if (cancelAtRecognizer && (detector == Detector::Fails || detector == Detector::Cancelled)) continue;
            const Scenario s{helper, inPlace, timing, detector, memoAtStart, memoAtRecognizer, heldElsewhere,
                             cancelAtRecognizer};
            World overlapped, sequential;
            const auto got = Prepare(overlapped, s, true);
            const auto reference = Prepare(sequential, s, false);
            const auto want = got.helperFaulted ? AfterHelperFault(s) : reference;
            QVERIFY2(got == want, Describe(s, got, want).constData());
            // No helper with a memo at the start; otherwise it always ran.
            QCOMPARE(got.helperRan, !memoAtStart);
            // Never left beside a published State, never left after a refusal.
            QVERIFY2(got.publishedClean, Describe(s, got, want).constData());
            QVERIFY2(!got.aheadLeftAfterRefusal, Describe(s, got, want).constData());
            // Only a load that ended loaded is adopted.
            if (got.adopted) {
                QVERIFY(helper == Load::Loaded && !got.helperStopped);
                ++adopted;
            }
            if (got.helperFaulted) {
                ++faultsRaised;
                // Raised with the detector side's failure, never recorded against it.
                if (detector == Detector::FallsBack || detector == Detector::Fails || detector == Detector::Cancelled)
                    QVERIFY2(got.message.find("detector") != std::string::npos &&
                             got.message.find("recognizer prepared beside the detector failed") != std::string::npos,
                             got.message.c_str());
                QVERIFY(got.memoWrites.empty());
            } else if (!got.adopted && got.decision != "cancelled" && helper != Load::Loaded) {
                ++redone;
            }
            ++scenarios;
        }
    }
    // Every class of outcome was reached.
    QVERIFY(adopted > 0);
    QVERIFY(faultsRaised > 0);
    QVERIFY(redone > 0);
    qInfo().noquote() << "preparationOverlap scenarios" << scenarios << "adopted" << adopted << "faultsRaised"
                      << faultsRaised << "decidedAgainInPlace" << redone;
}

// Item 2 of the follow-up: preparation A is cancelled while its helper is
// inside a vendor call, and preparation B with the same recognizer reaches its
// recognizer point before A has joined that call. The helper holds nothing,
// so B takes the latch and adopts its own BF16 load, as when A had finished.
inline void AbandonedOwnerDoesNotRefuseAnother()
{
    for (const bool faultAfterwards : {false, true}) {
        World world;
        std::atomic<bool> inVendorCall{}, releaseVendorCall{}, aSettling{}, aFinished{}, aStarted{};
        std::atomic<bool> cancelA{};
        Fake stateA;
        Observed a;
        std::thread preparationA([&] {
            const auto settlement = MakePoint(world, stateA);
            Ahead ahead;
            stateA.aheadNetwork = true;
            aStarted = ahead.Start(kKey, [] {}, [&](const std::function<bool()>&, const std::function<bool()>&) {
                inVendorCall = true;
                // A vendor call does not look at stop.
                while (!releaseVendorCall) std::this_thread::yield();
                if (faultAfterwards) throw detail::TensorRtApiFailure("execution context creation failed");
                return false;
            }, [&cancelA] { return cancelA.load(); });
            if (!aStarted) { aFinished = true; return; }
            try {
                preparation_overlap::DetectorSide(ahead, settlement, [&] {
                    while (!inVendorCall) std::this_thread::yield();
                    cancelA = true;
                    aSettling = true;
                    throw detail::VerificationInterrupted("detector stopped while verifying");
                }, [](const std::exception_ptr&) {});
            } catch (...) {
                const auto failure = std::current_exception();
                a.message = preparation_overlap::Message(failure);
                if (cancelA && (detail::AllowsTensorRtCapacityFallback(failure) || detail::IsVerificationInterrupted(failure)))
                    if (!stateA.owned.empty()) { world.Erase(stateA.owned); stateA.owned.clear(); }
                a.decision = Classify(failure, cancelA);
            }
            aFinished = true;
        });
        while (!aSettling && !aFinished) std::this_thread::yield();
        QVERIFY(aStarted);
        // B runs to its decision while A still waits for its helper.
        Scenario b;
        const auto observedB = Prepare(world, b, true);
        QCOMPARE(observedB.decision, std::string("bf16"));
        QVERIFY(observedB.adopted);
        QVERIFY(!world.Held()); // B passed and released; A never took it
        QCOMPARE(world.begins, 1);
        QVERIFY(!aFinished); // A was still waiting for its helper's vendor call
        releaseVendorCall = true;
        preparationA.join();
        QVERIFY(world.memoWrites.empty());
        if (!faultAfterwards) {
            // The sequential order: cancelled before the recognizer, nothing held.
            QCOMPARE(a.decision, std::string("cancelled"));
            QVERIFY(!world.Held());
            QCOMPARE(world.begins, 1);
        } else {
            // A's own fault: raised with its cancellation, held as after a
            // terminal fault in A's recognizer stage.
            QCOMPARE(a.decision, std::string("fault"));
            QVERIFY(a.message.find("recognizer prepared beside the detector failed") != std::string::npos);
            QVERIFY(world.Held());
            QCOMPARE(world.begins, 2);
        }
    }
}

// The remaining unit rules: no helper with a memo at the start; a helper
// started for another attempt is never adopted; destruction stops and joins;
// the stop seen by the load stays callable after the unit is gone.
inline void AbandonStopsJoinsAndReleases()
{
    {
        World world;
        world.memos.insert(kKey);
        Ahead ahead;
        bool ran = false;
        QVERIFY(!ahead.Start(kKey, [&world] { if (world.Memo(kKey)) throw runtime_precision::ArtifactError("memo"); },
                             [&](const std::function<bool()>&, const std::function<bool()>&) { ran = true; return false; }, [] { return false; }));
        QVERIFY(!ahead.Started() && !ahead.Pending());
        QVERIFY(!ran);
        QCOMPARE(world.begins, 0);
    }
    {
        World world;
        Fake state;
        state.aheadNetwork = true;
        Ahead ahead;
        QVERIFY(ahead.Start("runtime-recognizer:other", [] {}, [](const std::function<bool()>&, const std::function<bool()>&) { return false; },
                            [] { return false; }));
        bool inPlace = false;
        preparation_overlap::Decide(&ahead, kKey, MakePoint(world, state, [&] { inPlace = true; }));
        QVERIFY(inPlace);
        QVERIFY(!state.aheadNetwork);
        QVERIFY(state.recognizer != Fake::Recognizer::Adopted);
        QVERIFY(!ahead.Pending());
        QCOMPARE(state.owned, std::string(kKey));
    }
    {
        std::function<bool()> kept;
        std::atomic<bool> finished{};
        const auto started = Clock::now();
        {
            Ahead ahead;
            QVERIFY(ahead.Start(kKey, [] {}, [&](const std::function<bool()>& stop, const std::function<bool()>&) -> bool {
                kept = stop; // as a Network keeps its build progress callback
                while (!stop()) std::this_thread::yield();
                finished = true;
                throw detail::VerificationInterrupted("stopped while verifying");
            }, [] { return false; }));
            QVERIFY(ahead.Pending());
        }
        QVERIFY(finished);
        QVERIFY(kept && kept()); // abandoned stays true and safe to read
        QVERIFY(Clock::now() - started < std::chrono::seconds(5));
    }
    {
        // Take twice, Start twice: nothing changes after the first.
        Ahead ahead;
        QVERIFY(ahead.Start(kKey, [] {}, [](const std::function<bool()>&, const std::function<bool()>&) { return true; }, [] { return false; }));
        QCOMPARE(ahead.Take().kind, preparation_overlap::Outcome::Kind::NeedsBuild);
        QCOMPARE(ahead.Take().kind, preparation_overlap::Outcome::Kind::NotStarted);
        QVERIFY(!ahead.Abandon());
        QVERIFY(!ahead.Start(kKey, [] {}, [](const std::function<bool()>&, const std::function<bool()>&) { return false; }, [] { return false; }));
    }
    {
        // The gate holds the context creation until Open; Take opens it;
        // abandonment releases it with false.
        std::atomic<bool> parked{}, passed{};
        Ahead ahead;
        QVERIFY(ahead.Start(kKey, [] {}, [&](const std::function<bool()>&, const std::function<bool()>& gate) {
            parked = true;
            passed = gate();
            return false;
        }, [] { return false; }));
        while (!parked) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        QVERIFY(!passed);
        ahead.Open();
        QCOMPARE(ahead.Take().kind, preparation_overlap::Outcome::Kind::Loaded);
        QVERIFY(passed);
        Ahead taken;
        QVERIFY(taken.Start(kKey, [] {}, [](const std::function<bool()>&, const std::function<bool()>& gate) {
            if (!gate()) throw detail::VerificationInterrupted("stopped before the execution context");
            return false;
        }, [] { return false; }));
        QCOMPARE(taken.Take().kind, preparation_overlap::Outcome::Kind::Loaded); // no Open: Take opens
        Ahead abandoned;
        QVERIFY(abandoned.Start(kKey, [] {}, [](const std::function<bool()>&, const std::function<bool()>& gate) {
            if (!gate()) throw detail::VerificationInterrupted("stopped before the execution context");
            return false;
        }, [] { return false; }));
        QVERIFY(!abandoned.Abandon()); // released with false: interrupted, not a fault
        std::atomic<bool> cancel{};
        Ahead cancelled;
        QVERIFY(cancelled.Start(kKey, [] {}, [](const std::function<bool()>&, const std::function<bool()>& gate) {
            if (!gate()) throw detail::VerificationInterrupted("stopped before the execution context");
            return false;
        }, [&cancel] { return cancel.load(); }));
        cancel = true; // polled
        const auto outcome = cancelled.Take();
        QCOMPARE(outcome.kind, preparation_overlap::Outcome::Kind::Failed); // cancellation wins over a late Open
    }
    {
        // A fault raised after a refusal keeps its class and both messages.
        try {
            preparation_overlap::RaiseFault(std::make_exception_ptr(detail::CudaFailure("device fault")),
                                            std::make_exception_ptr(runtime_precision::ArtifactError("previously refused")));
            QFAIL("RaiseFault returned");
        } catch (const detail::CudaFailure& error) {
            const std::string message = error.what();
            QVERIFY(message.find("previously refused") == 0);
            QVERIFY(message.find("device fault") != std::string::npos);
        }
        try {
            preparation_overlap::RaiseFault(std::make_exception_ptr(std::bad_alloc()));
            QFAIL("RaiseFault returned");
        } catch (const detail::TensorRtApiFailure& error) {
            QVERIFY(std::string(error.what()).find("out of memory") != std::string::npos);
        }
    }
}
} // namespace uct_preparation_overlap_test
