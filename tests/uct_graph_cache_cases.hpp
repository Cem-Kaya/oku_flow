#pragma once
// Model-free bookkeeping of the private TensorRT graph-replay experiment.
#include "../src/common/uct_graph_cache.hpp"

#include <QtTest>

#include <chrono>
#include <future>
#include <thread>

namespace uct_graph_cache_test
{
using namespace okuflow::uct::graph_cache;

// Carries out a decision the way Network does when nothing fails.
inline Decision Run(Policy& policy, Shape shape, const void* input, const void* output, bool mayCapture = true)
{
    const auto decision = policy.Plan(shape, input, output, mayCapture);
    policy.Bound(decision.slot);
    policy.Ran(decision.slot, decision.launch);
    return decision;
}

// ReplayWork::Turn, the context-creation policy, with real threads. Every
// assertion has a lower bound only (a turn may not come early) or a generous
// upper bound (it must come).
inline void CreationTurns()
{
    using namespace std::chrono;
    using namespace std::chrono_literals;
    struct Creator {
        ReplayWork& work;
        std::atomic<bool>& wanted;
        std::atomic<bool> stopped{};
        std::uint64_t since{};
        // Runs one Turn on a thread; the future says whether it granted a creation.
        std::future<bool> Start()
        {
            return std::async(std::launch::async, [this] {
                std::unique_lock lock(work.mutex);
                return work.Turn(lock, [this] { return wanted.load(); }, [this] { return stopped.load(); }, since);
            });
        }
    };
    const auto pending = [](std::future<bool>& turn, milliseconds wait) {
        return turn.wait_for(wait) == std::future_status::timeout;
    };

    // Idle gap: no read in flight, but not before kQuiet after the last read ended.
    {
        ReplayWork work;
        std::atomic<bool> wanted{true};
        Creator creator{work, wanted};
        work.Begin();
        work.End();
        const auto ended = steady_clock::now();
        auto turn = creator.Start();
        QVERIFY(turn.wait_for(5s) == std::future_status::ready);
        QVERIFY(turn.get());
        QVERIFY(steady_clock::now() - ended >= ReplayWork::kQuiet);
        QCOMPARE(work.started.load(), std::uint64_t(1));
        QCOMPARE(work.startedUnderLoad.load(), std::uint64_t(0));
    }
    // Back-to-back reads leave no idle gap: no turn after one read ends, a turn
    // (under load) once kReadsUnderLoad reads have ended.
    {
        ReplayWork work;
        std::atomic<bool> wanted{true};
        Creator creator{work, wanted};
        work.Begin();
        auto turn = creator.Start();
        QVERIFY(pending(turn, 30ms));
        work.End();
        work.Begin(); // the next read starts at once
        QVERIFY(pending(turn, 30ms));
        work.End();
        work.Begin();
        QVERIFY(turn.wait_for(5s) == std::future_status::ready);
        QVERIFY(turn.get());
        QCOMPARE(work.started.load(), std::uint64_t(1));
        QCOMPARE(work.startedUnderLoad.load(), std::uint64_t(1));
        // The count starts again from the reads ended at that creation.
        creator.since = work.readsEnded;
        auto next = creator.Start();
        work.End();
        work.Begin();
        QVERIFY(pending(next, 30ms));
        work.End();
        work.Begin();
        QVERIFY(next.wait_for(5s) == std::future_status::ready);
        QVERIFY(next.get());
        work.End();
    }
    // Reads that end while nothing is wanted do not count towards a turn under load.
    {
        ReplayWork work;
        std::atomic<bool> wanted{false};
        Creator creator{work, wanted};
        work.Begin();
        auto turn = creator.Start();
        for (int i = 0; i < 3; ++i) {
            QVERIFY(pending(turn, 15ms));
            work.End();
            work.Begin();
        }
        QVERIFY(pending(turn, 15ms));
        wanted = true;
        work.Notify();
        QVERIFY(pending(turn, 30ms)); // a read is in flight and none ended since
        work.End();
        work.Begin();
        QVERIFY(pending(turn, 30ms));
        work.End();
        work.Begin();
        QVERIFY(turn.wait_for(5s) == std::future_status::ready);
        QVERIFY(turn.get());
        work.End();
    }
    // Stop wakes a waiting thread, which gets no turn; so does the caller's own
    // stop condition (a cancelled preparation) once the thread is woken.
    {
        ReplayWork work;
        std::atomic<bool> wanted{false};
        Creator creator{work, wanted};
        auto turn = creator.Start();
        QVERIFY(pending(turn, 20ms));
        work.Stop();
        QVERIFY(turn.wait_for(5s) == std::future_status::ready);
        QVERIFY(!turn.get());
        QCOMPARE(work.started.load(), std::uint64_t(0));
    }
    {
        ReplayWork work;
        std::atomic<bool> wanted{true};
        Creator creator{work, wanted};
        work.Begin();
        auto turn = creator.Start();
        QVERIFY(pending(turn, 20ms));
        creator.stopped = true;
        work.Notify();
        QVERIFY(turn.wait_for(5s) == std::future_status::ready);
        QVERIFY(!turn.get());
        work.End();
    }
}

inline void Policies()
{
    int in{}, other{}, out{};
    const Shape n8{8, 48, 1024}, n3{3, 48, 1024}, whole{1, 544, 960}, tile{1, 960, 928};

    // A new shape: new context, shape and addresses set, plain enqueue first.
    Policy policy(4);
    auto d = Run(policy, n8, &in, &out);
    QCOMPARE(d.slot, 0);
    QVERIFY(d.create && d.reshape && d.rebind && !d.drain && !d.dropGraph && !d.evicted);
    QCOMPARE(d.launch, Launch::Plain);
    // Second call: capture, then replays with no rebinding at all.
    d = Run(policy, n8, &in, &out);
    QVERIFY(!d.create && !d.reshape && !d.rebind && !d.drain);
    QCOMPARE(d.launch, Launch::Capture);
    for (int i = 0; i < 3; ++i) {
        d = Run(policy, n8, &in, &out);
        QCOMPARE(d.launch, Launch::Replay);
        QVERIFY(!d.reshape && !d.rebind && !d.drain);
    }
    // Other shapes get their own contexts; returning to n8 still replays.
    d = Run(policy, n3, &in, &out);
    QVERIFY(d.create && d.slot == 1 && d.launch == Launch::Plain && !d.drain);
    d = Run(policy, n8, &in, &out);
    QCOMPARE(d.launch, Launch::Replay);
    QCOMPARE(d.slot, 0);
    d = Run(policy, n3, &in, &out);
    QCOMPARE(d.launch, Launch::Capture);
    QCOMPARE(policy.Count().captures, std::uint64_t(2));

    // Capture is never decided while the caller forbids it (look-ahead); a
    // graph that already exists is still replayed.
    d = Run(policy, whole, &in, &out, false);
    QCOMPARE(d.launch, Launch::Plain);
    d = Run(policy, whole, &in, &out, false);
    QCOMPARE(d.launch, Launch::Plain);
    d = Run(policy, whole, &in, &out, true);
    QCOMPARE(d.launch, Launch::Capture);
    d = Run(policy, whole, &in, &out, false);
    QCOMPARE(d.launch, Launch::Replay);

    // A moved input: drain, drop the graph, rebind, warm again; same context.
    d = Run(policy, n8, &other, &out);
    QVERIFY(d.slot == 0 && d.rebind && d.drain && d.dropGraph && !d.reshape && !d.create);
    QCOMPARE(d.launch, Launch::Plain);
    QVERIFY(!policy.HasGraph(0));
    d = Run(policy, n8, &other, &out);
    QCOMPARE(d.launch, Launch::Capture);
    // A moved address on a slot without a graph still drains: its plain
    // enqueue may be queued. The same for a moved output alone.
    d = Run(policy, tile, &in, &out);
    QVERIFY(d.create && d.slot == 3);
    d = Run(policy, tile, &other, &out);
    QVERIFY(d.rebind && d.drain && !d.dropGraph && d.launch == Launch::Plain);
    d = Run(policy, tile, &other, &in);
    QVERIFY(d.rebind && d.drain && !d.dropGraph && !d.reshape && d.launch == Launch::Plain);

    // Full cache: the least recently used slot is recycled (reshaped, its
    // graph dropped after a drain), never a new context.
    // Use order now: n3 (slot 1) oldest, then whole (2), n8 (0), tile (3).
    const Shape n5{5, 48, 1024};
    d = Run(policy, n5, &in, &out);
    QCOMPARE(d.slot, 1);
    QVERIFY(!d.create && d.reshape && d.rebind && d.drain && d.dropGraph && d.evicted);
    QCOMPARE(d.launch, Launch::Plain);
    QCOMPARE(policy.Slots(), 4);
    QCOMPARE(policy.Count().creates, std::uint64_t(4));
    QCOMPARE(policy.Count().evictions, std::uint64_t(1));
    // The evicted shape is a miss again and evicts the next oldest (whole).
    d = Run(policy, n3, &in, &out);
    QCOMPARE(d.slot, 2);
    QVERIFY(d.evicted && d.launch == Launch::Plain);

    // One graph per engine (capacity 1): every change of shape drains and
    // reshapes the single context and loses its graph.
    Policy single(1);
    Run(single, whole, &in, &out);
    QCOMPARE(Run(single, whole, &in, &out).launch, Launch::Capture);
    d = Run(single, tile, &in, &out);
    QVERIFY(d.slot == 0 && d.drain && d.dropGraph && d.reshape && d.launch == Launch::Plain);
    d = Run(single, whole, &in, &out);
    QVERIFY(d.slot == 0 && d.launch == Launch::Plain && d.evicted);
    QCOMPARE(single.Slots(), 1);

    // Refusal: the shape runs plainly from then on and is never captured
    // again, also after eviction and return. Other shapes still capture.
    Policy refusing(1);
    Run(refusing, n8, &in, &out);
    d = refusing.Plan(n8, &in, &out, true);
    QCOMPARE(d.launch, Launch::Capture);
    refusing.Bound(d.slot);
    refusing.Refuse(d.slot);
    refusing.Ran(d.slot, Launch::Plain);
    QCOMPARE(Run(refusing, n8, &in, &out).launch, Launch::Plain);
    Run(refusing, n3, &in, &out);
    QCOMPARE(Run(refusing, n3, &in, &out).launch, Launch::Capture);
    Run(refusing, n8, &in, &out);
    QCOMPARE(Run(refusing, n8, &in, &out).launch, Launch::Plain);
    QCOMPARE(refusing.Count().refusals, std::uint64_t(1));
    // Refusing the same shape again changes nothing.
    refusing.Refuse(0);
    QVERIFY(!refusing.Blocked());
    // A capture that was only put off (the caller ran the call plainly
    // without refusing) is offered again on the next call.
    Policy deferring(1);
    Run(deferring, n8, &in, &out);
    d = deferring.Plan(n8, &in, &out, true);
    QCOMPARE(d.launch, Launch::Capture);
    deferring.Bound(d.slot);
    deferring.Ran(d.slot, Launch::Plain);
    QCOMPARE(deferring.Plan(n8, &in, &out, true).launch, Launch::Capture);
    QCOMPARE(deferring.Count().refusals, std::uint64_t(0));
    // With the 32nd distinct refused shape capture stops altogether.
    Policy blocking(2);
    for (int i = 1; i <= kMaximumRefusedShapes; ++i) {
        QVERIFY(!blocking.Blocked());
        const Shape shape{1, 32, 32 * i};
        Run(blocking, shape, &in, &out);
        d = blocking.Plan(shape, &in, &out, true);
        QCOMPARE(d.launch, Launch::Capture);
        blocking.Bound(d.slot);
        blocking.Refuse(d.slot);
        blocking.Ran(d.slot, Launch::Plain);
    }
    QVERIFY(blocking.Blocked());
    Run(blocking, n8, &in, &out);
    QCOMPARE(Run(blocking, n8, &in, &out).launch, Launch::Plain);

    // A binding that failed (Plan without Bound) is never treated as a hit,
    // and its slot is recycled before any healthy one.
    Policy failing(2);
    Run(failing, n8, &in, &out);
    Run(failing, n8, &in, &out); // captured
    d = failing.Plan(n3, &in, &out, true); // slot 1, binding fails
    QVERIFY(d.create && d.slot == 1);
    QVERIFY(!failing.IsBound(1));
    d = failing.Plan(n3, &in, &out, true);
    QVERIFY(d.slot == 1 && d.reshape && d.rebind && !d.create && !d.evicted);
    failing.Bound(d.slot);
    failing.Ran(d.slot, d.launch);
    QCOMPARE(Run(failing, n8, &in, &out).launch, Launch::Replay);
    d = failing.Plan(n5, &in, &out, true); // a failed rebind of the oldest
    QCOMPARE(d.slot, 1);
    d = failing.Plan(whole, &in, &out, true);
    QCOMPARE(d.slot, 1); // the unbound slot again, not slot 0's graph
    QVERIFY(failing.HasGraph(0));

    // Without a ready context a miss below capacity runs on the plain overflow
    // context: no slot, no eviction, no drain. A full cache still recycles.
    Policy starving(4);
    d = starving.Plan(n8, &in, &out, true, false);
    QVERIFY(d.overflow && d.slot == -1 && !d.create && !d.evicted && !d.drain && d.launch == Launch::Plain);
    starving.RanOverflow();
    QCOMPARE(starving.Slots(), 0);
    d = Run(starving, n8, &in, &out); // a context is ready now
    QVERIFY(d.create && d.slot == 0 && !d.overflow);
    d = starving.Plan(n3, &in, &out, true, false);
    QVERIFY(d.overflow && !d.evicted);
    starving.RanOverflow();
    QCOMPARE(Run(starving, n8, &in, &out).launch, Launch::Capture); // n8 kept its slot
    QCOMPARE(starving.Count().overflow, std::uint64_t(2));
    QCOMPARE(starving.Count().plain, std::uint64_t(3));
    // Once contexts are ready the cache grows again; a full cache recycles
    // instead of overflowing, whatever the caller says.
    Run(starving, n3, &in, &out);
    Run(starving, n5, &in, &out);
    Run(starving, whole, &in, &out);
    QCOMPARE(starving.Slots(), 4);
    d = starving.Plan(tile, &in, &out, true, false);
    QVERIFY(!d.overflow && d.evicted && d.slot >= 0);
    starving.Bound(d.slot);
    starving.Ran(d.slot, d.launch);
    QCOMPARE(starving.Count().overflow, std::uint64_t(2));
    QCOMPARE(starving.Count().evictions, std::uint64_t(1));
    // Distinct shapes asked for are counted (the contexts worth creating),
    // including evicted ones, never twice, and only up to the bound.
    QCOMPARE(starving.Seen(), 5);
    Run(starving, n3, &in, &out);
    QCOMPARE(starving.Seen(), 5);
    Policy counting(2);
    for (int i = 1; i <= kMaximumSeenShapes + 10; ++i) Run(counting, Shape{1, 32, 8 * i}, &in, &out);
    QCOMPARE(counting.Seen(), kMaximumSeenShapes);

    // Per-engine capacities. Off stays off; the default gives the detector
    // the default bound and a fixed-canvas recognizer at least one slot per
    // batch size; the bucketed recognizer width runs its recognizer plainly;
    // an explicit comparison bound applies to both engines as given.
    QCOMPARE(ForConfiguration(0, false, 8, false), (Capacities{0, 0}));
    QCOMPARE(ForConfiguration(kDefaultShapes, false, 8, false), (Capacities{8, 8}));
    QCOMPARE(ForConfiguration(kDefaultShapes, false, 1, false), (Capacities{8, 8}));
    QCOMPARE(ForConfiguration(kDefaultShapes, false, 16, false), (Capacities{8, 16}));
    QCOMPARE(ForConfiguration(kDefaultShapes, false, 8, true), (Capacities{8, 0}));
    QCOMPARE(ForConfiguration(kDefaultShapes, false, 16, true), (Capacities{8, 0}));
    QCOMPARE(ForConfiguration(1, true, 8, false), (Capacities{1, 1}));
    QCOMPARE(ForConfiguration(4, true, 16, false), (Capacities{4, 4}));
    QCOMPARE(ForConfiguration(2, true, 8, true), (Capacities{2, 0}));
    QCOMPARE(ForConfiguration(99, true, 8, false), (Capacities{kMaximumShapes, kMaximumShapes}));
    // Contexts worth creating: a recognizer on the fixed canvas has one shape
    // per batch size; a plain engine needs none.
    QCOMPARE(UsefulContexts(8, false, 8), 8);
    QCOMPARE(UsefulContexts(8, true, 8), 8);
    QCOMPARE(UsefulContexts(8, true, 1), 1);
    QCOMPARE(UsefulContexts(16, true, 16), 16);
    QCOMPARE(UsefulContexts(1, true, 8), 1);
    QCOMPARE(UsefulContexts(0, true, 8), 0);

    // Fault plans fire only for their fault and engine, after `skip`
    // eligible events, `count` times.
    FaultPlan plan;
    plan.fault = Fault::MoveInput;
    plan.engine = 1;
    plan.skip = 1;
    plan.count = 2;
    QVERIFY(!plan.Fire(Fault::RejectShape, 1));
    QVERIFY(!plan.Fire(Fault::MoveInput, 0));
    QVERIFY(!plan.Fire(Fault::MoveInput, 1)); // skipped
    QVERIFY(plan.Fire(Fault::MoveInput, 1));
    QVERIFY(plan.Fire(Fault::MoveInput, 1));
    QVERIFY(!plan.Fire(Fault::MoveInput, 1));
    QCOMPARE(plan.injected.load(), 2);

    // Bounds: capacity is clamped to 1..16 and slots never exceed it.
    QCOMPARE(Policy(0).Capacity(), 1);
    QCOMPARE(Policy(99).Capacity(), kMaximumShapes);
    Policy bounded(kMaximumShapes);
    for (int round = 0; round < 3; ++round)
        for (int i = 1; i <= 40; ++i) Run(bounded, Shape{1, 32, 32 * i}, &in, &out);
    QCOMPARE(bounded.Slots(), kMaximumShapes);
    QCOMPARE(bounded.Count().creates, std::uint64_t(kMaximumShapes));
    QCOMPARE(bounded.Count().misses, std::uint64_t(120));
}
} // namespace uct_graph_cache_test
