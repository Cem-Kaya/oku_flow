#pragma once
// Model-free bookkeeping of the private TensorRT graph-replay experiment.
#include "../src/common/uct_graph_cache.hpp"

#include <QtTest>

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
    // A moved address on a slot without a graph needs no drain.
    d = Run(policy, tile, &in, &out);
    QVERIFY(d.create && d.slot == 3);
    d = Run(policy, tile, &other, &out);
    QVERIFY(d.rebind && !d.drain && !d.dropGraph && d.launch == Launch::Plain);

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
    // After 32 distinct refused shapes capture stops altogether.
    Policy blocking(2);
    for (int i = 1; i <= kMaximumRefusedShapes + 1; ++i) {
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
