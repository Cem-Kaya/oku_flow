#pragma once

// Private experiment OKUFLOW_UCT_TEST_TRT_GRAPH, off by default: replay each
// TensorRT enqueue of a Network as a CUDA graph.
//
// This header is the bookkeeping only (the cache policy, and ReplayWork, when
// the context-creation thread may create). It owns no CUDA or TensorRT
// resource and is tested without a model; Network in uct_tensorrt.cpp carries out each
// decision. A graph freezes the tensor shapes, the tensor addresses and the
// activation memory of the execution context it was captured from, so:
//
// - Every cached shape has its own execution context and at most one graph.
//   No context is ever shared by two live graphs: reshaping or readdressing a
//   context first destroys its graph. (OkuRead found that one context shared
//   across several graphs changed later output.)
// - The contexts of one Network share one activation arena, including its
//   plain overflow context. That is safe only while every context of the
//   engine runs in order on the one stream of its State: a change that runs
//   two contexts of one engine on different streams at once (for example
//   concurrent recognition groups, round 8) must give each its own arena.
// - A context whose work may still be queued is never reshaped, readdressed or
//   stripped of its graph before the stream is drained (`drain`).
// - A new or reshaped binding is first run as a plain enqueue, which lets
//   TensorRT finish its shape-dependent setup; only a later call captures.
// - The caller says whether capture is allowed (`mayCapture`): never while an
//   earlier inference of the same read is still queued (recognition
//   look-ahead, detection of equal-shaped regions queued behind each other).
// - The cache is bounded. When it is full the least recently used slot is
//   recycled: its context is reshaped, it is never destroyed and recreated.
// - The read path never creates an execution context (creating one takes tens
//   of milliseconds). The caller says whether a ready context is available
//   (`canCreate`); without one, while the cache is not full, a miss runs on
//   the engine's plain overflow context exactly as the plain path would,
//   evicts nothing, and the cache grows later.
// - A shape whose capture was refused is not tried again; with the
//   kMaximumRefusedShapes-th distinct refusal capture stops altogether. A
//   refused shape still runs, as a plain enqueue. A capture that is only put
//   off (too little free device memory at that moment, or the device running
//   out of memory during the capture) is not a refusal; the Network then
//   holds every capture off for kCaptureHoldOffReads reads.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace okuflow::uct::graph_cache
{
constexpr int kMaximumShapes = 16;
// Chosen from the round 3 measurements (docs/uct_okuread_port.md): with the
// fixed 1024 canvas a recognizer uses up to eight batch sizes and a detector
// two to four region shapes per frame size.
constexpr int kDefaultShapes = 8;
constexpr int kMaximumRefusedShapes = 32;
constexpr int kMaximumSeenShapes = 64;
// After a capture is put off for lack of memory, an engine runs every call
// that would capture as a plain enqueue for the rest of that read and the next
// kCaptureHoldOffReads - 1 reads, so it does not ask the device for its free
// memory, or try a capture that runs out of it, on every call while memory
// stays short. Counted in reads, not calls: how many calls want a capture per
// read depends on the scene, a read count does not.
constexpr int kCaptureHoldOffReads = 16;

// How many shapes each engine caches; 0 runs that engine as a plain enqueue.
// `requested` is 0 (replay off), kDefaultShapes, or the explicit comparison
// bound OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES (`explicitBound`).
struct Capacities {
    int detector{}, recognizer{};
    friend bool operator==(const Capacities&, const Capacities&) = default;
};
inline Capacities ForConfiguration(int requested, bool explicitBound, int recognizerBatch, bool variableWidth)
{
    if (requested <= 0) return {};
    const int bound = std::clamp(requested, 1, kMaximumShapes);
    Capacities result{bound, bound};
    // The bucketed recognizer width has up to four canvases per batch size,
    // 32 shapes at batch eight: more than any bound, so a cache would evict
    // and drain on most calls. That recognizer runs plainly.
    if (variableWidth) result.recognizer = 0;
    // With the fixed canvas a recognizer has one shape per batch size; batch
    // sixteen needs sixteen slots to avoid evicting its own shapes.
    else if (!explicitBound) result.recognizer = std::clamp(std::max(bound, recognizerBatch), 1, kMaximumShapes);
    return result;
}
// How many execution contexts are worth creating for one engine: no more than
// it can cache, and for a fixed-canvas recognizer no more than its batch sizes.
inline int UsefulContexts(int capacity, bool recognizer, int recognizerBatch)
{
    if (capacity <= 0) return 0;
    return recognizer ? std::clamp(recognizerBatch, 1, capacity) : capacity;
}

struct Shape {
    int batch{}, height{}, width{};
    friend bool operator==(const Shape&, const Shape&) = default;
};

enum class Launch { Plain, Capture, Replay };

struct Decision {
    int slot{-1};
    bool create{};    // a new slot: create its execution context first
    bool drain{};     // wait for the stream before touching the slot
    bool dropGraph{}; // destroy the slot's graph (after the drain)
    bool reshape{};   // set the slot's input shape
    bool rebind{};    // set the slot's tensor addresses
    bool evicted{};   // the slot held another shape that is now forgotten
    bool overflow{};  // no slot: run on the plain overflow context (slot is -1)
    Launch launch{Launch::Plain};
};

struct Counters {
    std::uint64_t hits{}, misses{}, creates{}, evictions{}, rebinds{}, drains{};
    std::uint64_t plain{}, captures{}, replays{}, refusals{};
    std::uint64_t overflow{}; // misses below capacity run plainly on the overflow context for want of a ready context
};

class Policy
{
  public:
    explicit Policy(int capacity) : capacity_(std::clamp(capacity, 1, kMaximumShapes))
    {
        slots_.reserve(std::size_t(capacity_));
        refused_.reserve(kMaximumRefusedShapes);
        seen_.reserve(kMaximumSeenShapes);
    }
    int Capacity() const { return capacity_; }
    int Slots() const { return int(slots_.size()); }
    // Distinct shapes asked for so far, counted up to kMaximumSeenShapes: how
    // many execution contexts this engine could use.
    int Seen() const { return int(seen_.size()); }
    const Counters& Count() const { return counters_; }
    bool Blocked() const { return blocked_; }

    // Decide how to run one inference of `shape` reading `input` and writing
    // `output`. The slot's bookkeeping is updated as if the decision succeeds,
    // but the slot is not usable again until Bound() confirms the binding.
    // `canCreate`: the caller has a ready execution context for a new slot.
    Decision Plan(const Shape& shape, const void* input, const void* output, bool mayCapture,
                  bool canCreate = true)
    {
        Decision decision;
        if (int(seen_.size()) < kMaximumSeenShapes && std::find(seen_.begin(), seen_.end(), shape) == seen_.end())
            seen_.push_back(shape);
        int index = -1;
        for (int i = 0; i < int(slots_.size()); ++i)
            if (slots_[std::size_t(i)].bound && slots_[std::size_t(i)].shape == shape) { index = i; break; }
        if (index >= 0) {
            ++counters_.hits;
            auto& slot = slots_[std::size_t(index)];
            if (slot.input != input || slot.output != output) {
                // New addresses: the graph has the old ones baked in, and a
                // plain enqueue of this context may still be queued.
                decision.rebind = true;
                decision.dropGraph = slot.graph;
                decision.drain = slot.used;
                slot.graph = false;
                slot.warmed = false;
                slot.bound = false;
                ++counters_.rebinds;
            }
        } else {
            ++counters_.misses;
            if (int(slots_.size()) < capacity_ && !canCreate) {
                // No ready context yet: run plainly beside the cache.
                ++counters_.overflow;
                decision.overflow = true;
                return decision;
            }
            if (int(slots_.size()) < capacity_) {
                index = int(slots_.size());
                slots_.push_back({});
                decision.create = true;
                ++counters_.creates;
            } else {
                // Recycle a slot whose binding failed first, else the least
                // recently used one. Ties cannot occur: every use is stamped.
                index = 0;
                for (int i = 0; i < int(slots_.size()); ++i) {
                    const auto& candidate = slots_[std::size_t(i)];
                    const auto& best = slots_[std::size_t(index)];
                    if ((!candidate.bound && best.bound) ||
                        (candidate.bound == best.bound && candidate.lastUse < best.lastUse)) index = i;
                }
                auto& slot = slots_[std::size_t(index)];
                decision.evicted = slot.bound;
                decision.drain = slot.used;
                decision.dropGraph = slot.graph;
                counters_.evictions += decision.evicted;
            }
            auto& slot = slots_[std::size_t(index)];
            slot.shape = shape;
            slot.graph = slot.warmed = slot.bound = false;
            decision.reshape = decision.rebind = true;
        }
        auto& slot = slots_[std::size_t(index)];
        slot.input = input;
        slot.output = output;
        slot.lastUse = ++clock_;
        counters_.drains += decision.drain;
        decision.slot = index;
        if (slot.graph) decision.launch = Launch::Replay;
        else if (slot.warmed && mayCapture && !blocked_ && !Refused(shape)) decision.launch = Launch::Capture;
        else decision.launch = Launch::Plain;
        return decision;
    }
    // The slot's shape and addresses are set on its context.
    void Bound(int index) { slots_.at(std::size_t(index)).bound = true; }
    // What actually ran. A capture counts only when its graph was launched.
    void Ran(int index, Launch launch)
    {
        auto& slot = slots_.at(std::size_t(index));
        slot.used = true;
        if (launch == Launch::Plain) { slot.warmed = true; ++counters_.plain; }
        else if (launch == Launch::Capture) { slot.graph = true; ++counters_.captures; }
        else ++counters_.replays;
    }
    // An overflow decision ran as a plain enqueue.
    void RanOverflow() { ++counters_.plain; }
    // Capture of this slot's shape was refused; the caller runs it plainly.
    void Refuse(int index)
    {
        auto& slot = slots_.at(std::size_t(index));
        slot.graph = false;
        ++counters_.refusals;
        if (Refused(slot.shape)) return;
        refused_.push_back(slot.shape);
        if (int(refused_.size()) == kMaximumRefusedShapes) blocked_ = true;
    }
    bool Refused(const Shape& shape) const
    {
        return std::find(refused_.begin(), refused_.end(), shape) != refused_.end();
    }
    bool HasGraph(int index) const { return slots_.at(std::size_t(index)).graph; }
    bool IsBound(int index) const { return slots_.at(std::size_t(index)).bound; }
    Shape SlotShape(int index) const { return slots_.at(std::size_t(index)).shape; }

  private:
    struct Slot {
        Shape shape{};
        const void* input{};
        const void* output{};
        std::uint64_t lastUse{};
        bool bound{}, warmed{}, graph{}, used{};
    };
    int capacity_;
    std::vector<Slot> slots_;
    std::vector<Shape> refused_, seen_;
    std::uint64_t clock_{};
    bool blocked_{};
    Counters counters_;
};

// Graph replay: wakes the thread that creates execution contexts after the
// State is published, and tells it whether a read is in flight. Creating a
// context slows inference running at the same time (measured on scene set v1:
// a read that overlaps a creation takes a median 13 percent longer when it
// takes 50 ms or more, 76 percent when it is shorter, docs/uct_okuread_port.md,
// round 10 follow-up), so the thread follows this policy (Turn):
// - A creation starts only while no read is in flight and at least kQuiet
//   after the last read ended (an idle gap), or
// - under continuous load, when kReadsUnderLoad reads have ended since the
//   last creation (or since a context became wanted) without such a gap: then
//   it starts at once, while a read may be in flight, and slows that read.
// A read that begins while a creation runs proceeds. A failing read retires
// its State, which stops the thread, before the read's marker ends.
struct ReplayWork {
    static constexpr auto kQuiet = std::chrono::milliseconds(10);
    static constexpr std::uint64_t kReadsUnderLoad = 2;
    std::mutex mutex;
    std::condition_variable wake;
    int reading{};               // reads in flight (guarded by mutex)
    std::uint64_t readsEnded{};  // guarded by mutex
    std::chrono::steady_clock::time_point lastEnd{};
    std::atomic<bool> stop{};
    // Observations for the profile receipt; written by the creating thread.
    std::atomic<bool> creating{};
    std::atomic<std::uint64_t> started{}, startedUnderLoad{};
    void Notify() noexcept
    {
        try { std::lock_guard lock(mutex); } catch (...) {}
        wake.notify_all();
    }
    void Stop() noexcept
    {
        stop = true;
        Notify();
    }
    void Begin()
    {
        std::lock_guard lock(mutex);
        ++reading;
    }
    void End() noexcept
    {
        try {
            std::lock_guard lock(mutex);
            --reading;
            ++readsEnded;
            lastEnd = std::chrono::steady_clock::now();
        } catch (...) {}
        wake.notify_all();
    }
    // The creating thread, holding `lock` on `mutex`: wait until a context
    // may be created under the policy above. `wanted` says whether any engine
    // wants one; `stopped` whether the thread must end. Returns false to end.
    template <class Wanted, class Stopped>
    bool Turn(std::unique_lock<std::mutex>& lock, Wanted&& wanted, Stopped&& stopped, std::uint64_t& since)
    {
        for (;;) {
            if (stop.load() || stopped()) return false;
            if (!wanted()) {
                since = readsEnded; // nothing wanted: reads that end now do not count
                wake.wait(lock);
                continue;
            }
            if (readsEnded - since >= kReadsUnderLoad) {
                ++started;
                if (reading > 0) ++startedUnderLoad;
                return true;
            }
            if (reading == 0) {
                const auto quiet = lastEnd + kQuiet;
                if (std::chrono::steady_clock::now() >= quiet) {
                    ++started;
                    return true;
                }
                wake.wait_until(lock, quiet);
                continue;
            }
            wake.wait(lock);
        }
    }
};

// Test-only fault injection for the replay path. Network calls these hooks
// only in builds that define OKUFLOW_UCT_GRAPH_FAULT_HOOKS, which is the
// uct_paddle_tests executable alone; the application never compiles them, and
// no environment variable reaches them.
enum class Fault {
    None,
    InvalidateCapture,              // a real illegal call inside the capture: CUDA refuses it
    LowFreeMemory,                  // the free-memory check sees too little: the capture is put off
    CaptureEnqueueFails,            // the enqueue inside the capture fails with no CUDA reason: a fault
    CaptureEnqueueFailsInvalidated, // the enqueue inside the capture fails with a capture status: a refusal
    QueueBeforeDrain,               // host work queued on the stream just before a drain
    MoveInput,                      // the input arrives at a different address (a copy)
    RejectShape,                    // TensorRT rejects the shape between Plan and Bound: a fault
    ContextCreationFails,           // a background context creation fails
    ArenaUnavailable,               // the shared activation arena cannot be allocated
    OutputStorageUnavailable,       // the fixed replay output storage cannot be allocated
};
// Named points of one replay call, for tests that cancel exactly there.
enum class Checkpoint { Planned, Captured, Drained, Launched };
struct FaultPlan {
    // Called on the worker at each checkpoint (detector 0, recognizer 1).
    std::function<void(Checkpoint, int)> checkpoint;
    // Called on the context-creation thread of a published State each time it
    // has found a context still wanted and is about to wait for its turn
    // (before it takes the wait's lock).
    std::function<void()> creatorWaiting;
    Fault fault{Fault::None};
    int engine{-1}; // 0 detector, 1 recognizer, -1 either
    int skip{};     // eligible events let through first
    int count{1};   // events injected after those
    std::atomic<int> eligible{}, injected{};
    // Observations. callsEndedCapturing and drainsBeforeQueuedWorkDone must stay 0.
    std::atomic<int> callsEndedCapturing{}, replayCalls{};
    std::atomic<int> drainsAfterQueuedWork{}, drainsBeforeQueuedWorkDone{};
    // MoveInput copies; the test frees them after the recognizer is gone.
    void* scratch[2]{};
    std::size_t scratchBytes[2]{};
    int nextScratch{};
    bool Fire(Fault at, int network)
    {
        if (fault != at || (engine >= 0 && engine != network)) return false;
        const int n = eligible++;
        if (n < skip || n >= skip + count) return false;
        ++injected;
        return true;
    }
};
#if defined(OKUFLOW_UCT_GRAPH_FAULT_HOOKS)
// Set before a recognizer is created and cleared after it and its State are
// gone. Every user loads its own reference, so a plan outlives the last call
// that reads it, also on a context-creation thread that is still finishing.
inline std::atomic<std::shared_ptr<FaultPlan>>& FaultPlanForTest()
{
    static std::atomic<std::shared_ptr<FaultPlan>> plan{};
    return plan;
}
// Context-creation threads of published States that have not ended yet.
inline std::atomic<int>& CreatorThreadsForTest()
{
    static std::atomic<int> threads{};
    return threads;
}
#endif
} // namespace okuflow::uct::graph_cache
