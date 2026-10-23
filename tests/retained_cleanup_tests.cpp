#include "okuflow/common/retained_cleanup.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>

namespace {
using okuflow::AsyncCleanup;
using okuflow::detail::AsyncCleanupSetupPoint;
using okuflow::detail::StartRetainedCleanup;
using namespace std::chrono_literals;

int failures{}, cases{};
void Expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void Case(const char* kind, int origin, int outcome) {
    std::cout << "CASE " << ++cases << ' ' << kind << " origin=" << origin
              << " outcome=" << outcome << std::endl;
}

enum ResourceId { ContextId, InputId, IntegralId, ScratchId, ResourceCount };
enum class Outcome { Success, Error, Throw };
// These are reasons for entering the shared terminal handoff, not a fake CUDA
// implementation of Finish. Source review checks the actual CUDA call sites.
enum class Entry { Error, Timeout, Cancel, Replacement };
struct Evidence {
    std::array<std::atomic<int>, ResourceCount> destroyed{};
    std::atomic<int> premature{}, drains{}, closureDestroyed{};
    std::atomic<bool> confirmed{}, exact{};
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{}, released{};
    Outcome outcome{Outcome::Success};
    Entry entry{};
    const void* expectedContext{};
    std::array<const void*, ResourceCount> identities{};
    void Destroyed(ResourceId id) {
        if (!confirmed.load()) ++premature;
        ++destroyed[id];
    }
    bool WaitEntered() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return entered; });
    }
    void Release(Outcome result) {
        { std::lock_guard lock(mutex); outcome = result; released = true; }
        changed.notify_all();
    }
    bool Counts(int count) const {
        for (const auto& value : destroyed) if (value.load() != count) return false;
        return premature.load() == 0;
    }
};
struct Resource {
    std::shared_ptr<Evidence> evidence;
    ResourceId id;
    Resource(std::shared_ptr<Evidence> state, ResourceId identity)
        : evidence(std::move(state)), id(identity) {}
    ~Resource() { evidence->Destroyed(id); }
};
struct Context {
    std::shared_ptr<Evidence> evidence;
    std::shared_ptr<Resource> input, integral, scratch;
    std::shared_ptr<Context> cleanupRetention;
    explicit Context(const std::shared_ptr<Evidence>& state)
        : evidence(state), input(std::make_shared<Resource>(state, InputId)),
          integral(std::make_shared<Resource>(state, IntegralId)),
          scratch(std::make_shared<Resource>(state, ScratchId)) {
        state->expectedContext = this;
        state->identities = {this, input.get(), integral.get(), scratch.get()};
    }
    ~Context() { evidence->Destroyed(ContextId); }
};
struct Witness {
    std::weak_ptr<Context> context;
    std::weak_ptr<Resource> input, integral, scratch;
    explicit Witness(const std::shared_ptr<Context>& owner)
        : context(owner), input(owner->input), integral(owner->integral), scratch(owner->scratch) {}
    bool Alive() const {
        return !context.expired() && !input.expired() && !integral.expired() && !scratch.expired();
    }
    bool Gone() const {
        return context.expired() && input.expired() && integral.expired() && scratch.expired();
    }
};
bool Exact(const Context& c) {
    const auto& ids = c.evidence->identities;
    return &c == ids[ContextId] && c.input.get() == ids[InputId] &&
        c.integral.get() == ids[IntegralId] && c.scratch.get() == ids[ScratchId] &&
        c.cleanupRetention.get() == &c;
}
bool AwaitOutcome(Evidence& evidence) {
    ++evidence.drains;
    std::unique_lock lock(evidence.mutex);
    evidence.entered = true;
    evidence.changed.notify_all();
    if (!evidence.changed.wait_for(lock, 2s, [&] { return evidence.released; })) return false;
    if (evidence.outcome == Outcome::Throw) throw std::runtime_error("unknown fake completion");
    if (evidence.outcome == Outcome::Error) return false;
    evidence.confirmed = true;
    return true;
}
bool Drain(Context& c) {
    auto evidence = c.evidence; // the work owns every dependency, including its gate
    evidence->exact = Exact(c);
    return AwaitOutcome(*evidence);
}
struct ReleaseGuard {
    std::shared_ptr<Evidence> evidence;
    ~ReleaseGuard() { evidence->Release(Outcome::Error); }
};
bool WaitStatus(const std::shared_ptr<AsyncCleanup>& receipt, AsyncCleanup::Status wanted) {
    if (!receipt) return false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (receipt->Poll() == AsyncCleanup::Status::Running &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    return receipt->Poll() == wanted;
}

// Fault hooks are thread-local and compiled into this one standalone TU only.
// Control-block failure is raised by its actual shared_ptr allocator. The
// other checkpoints throw immediately before the named conversion/new/thread
// operation, preserving destruction/unwinding of the real cleanup closure.
struct Fault {
    AsyncCleanupSetupPoint point;
    const Witness& witness;
    const std::shared_ptr<Context>& cache;
    int trips{};
    unsigned visited{};
    static inline thread_local Fault* active{};
    static void Check(AsyncCleanupSetupPoint point) {
        auto& fault = *active;
        fault.visited |= 1u << static_cast<unsigned>(point);
        if (point != fault.point) return;
        ++fault.trips;
        auto exact = fault.witness.context.lock();
        Expect(exact && Exact(*exact) && fault.witness.Alive() && !fault.cache,
            "the exact self anchor precedes cache removal and every failing setup stage");
        if (point == AsyncCleanupSetupPoint::ThreadStart)
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        throw std::bad_alloc();
    }
    Fault(AsyncCleanupSetupPoint where, const Witness& held, const std::shared_ptr<Context>& reusable)
        : point(where), witness(held), cache(reusable) {
        active = this;
        okuflow::detail::asyncCleanupSetupCheckpoint = Check;
    }
    ~Fault() { okuflow::detail::asyncCleanupSetupCheckpoint = nullptr; active = nullptr; }
};

void SetupFailure(Entry entry, AsyncCleanupSetupPoint point) {
    Case("setup", static_cast<int>(entry), static_cast<int>(point));
    auto state = std::make_shared<Evidence>();
    state->entry = entry;
    auto cache = std::make_shared<Context>(state);
    Witness held(cache);
    std::shared_ptr<AsyncCleanup> receipt;
    {
        Fault fault(point, held, cache);
        // Deliberate alias: replacement used to move the cache before Start.
        receipt = StartRetainedCleanup(cache, cache, &Context::cleanupRetention, Drain);
        Expect(fault.trips == 1, "each requested setup failure is actually injected exactly once");
        const auto stages = static_cast<unsigned>(point) + 1;
        Expect(fault.visited == (1u << stages) - 1,
            "all preceding real setup stages ran and later stages did not run");
    }
    if (point == AsyncCleanupSetupPoint::ThreadStart)
        Expect(receipt && receipt->Poll() == AsyncCleanup::Status::Failed,
            "real AsyncCleanup thread-start catch records failure");
    else Expect(!receipt, "pre-thread allocation failure produces no fabricated cleanup receipt");
    receipt.reset();
    Expect(!cache && held.Alive() && state->Counts(0) && state->drains == 0,
        "failed setup retains context/input/integral/scratch after all caller owners disappear");
    // There is no completion proof and no retry. These few fake resources are
    // deliberately retained until this process exits, as production requires.
}

void Completion(Entry entry, Outcome outcome, bool unrelatedCache, bool alreadyReleased) {
    Case("completion", static_cast<int>(entry), static_cast<int>(outcome));
    auto state = std::make_shared<Evidence>();
    state->entry = entry;
    ReleaseGuard unblock{state};
    auto owner = std::make_shared<Context>(state);
    Witness held(owner);
    auto otherState = std::make_shared<Evidence>();
    otherState->confirmed = true; // no work was submitted to the unrelated cache
    auto cache = unrelatedCache ? std::make_shared<Context>(otherState) : owner;
    const auto* const expectedCache = cache.get();
    if (alreadyReleased) state->Release(outcome);
    auto receipt = StartRetainedCleanup(owner, cache, &Context::cleanupRetention, Drain);
    owner.reset();
    Expect(unrelatedCache ? cache.get() == expectedCache : !cache,
        "handoff removes only the exact matching reusable cache");
    if (!alreadyReleased) {
        Expect(state->WaitEntered() && held.Alive() && state->Counts(0),
            "all submitted resources remain owned while completion is pending");
    }
    cache.reset(); // detached work must not reference caller/cache storage
    if (!alreadyReleased) state->Release(outcome);
    const auto wanted = outcome == Outcome::Success ? AsyncCleanup::Status::Completed : AsyncCleanup::Status::Failed;
    Expect(WaitStatus(receipt, wanted) && state->exact && state->drains == 1,
        "the real detached cleanup drains the exact context once and reports its outcome");
    receipt.reset();
    if (outcome == Outcome::Success) {
        Expect(state->confirmed && held.Gone() && state->Counts(1),
            "known completion breaks the anchor and destroys every exact resource once");
    } else {
        Expect(!state->confirmed && held.Alive() && state->Counts(0),
            "error/throw after submission retains the entire unknown-completion closure");
        auto retained = held.context.lock();
        Expect(retained && Exact(*retained), "failed drain never breaks the self anchor");
    }
}

// Match production Quarantine: the receipt, cache and all caller-local strong
// owners leave scope while the real detached worker is still gated.
Witness StartAndDiscardReceipt(const std::shared_ptr<Evidence>& state) {
    auto cache = std::make_shared<Context>(state);
    Witness held(cache);
    {
        auto receipt = StartRetainedCleanup(cache, cache, &Context::cleanupRetention, Drain);
        Expect(receipt && !cache && state->WaitEntered(),
            "discarded-receipt setup reaches pending work before its caller scope exits");
    }
    return held;
}
void DiscardedReceiptWhilePending() {
    Case("discarded-receipt", 0, 0);
    auto state = std::make_shared<Evidence>();
    ReleaseGuard unblock{state};
    auto held = StartAndDiscardReceipt(state);
    Expect(held.Alive() && state->Counts(0) && !state->confirmed,
        "pending work keeps all exact resources after receipt and caller scope disappear");
    state->Release(Outcome::Success);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((!held.Gone() || !state->Counts(1)) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    Expect(state->confirmed && state->exact && state->drains == 1 && held.Gone() && state->Counts(1),
        "discarding the receipt never prevents the exact successful drain and one-time release");
}

// No Context, anchor or second strong owner can hide loss of this capture.
struct ClosureToken {
    std::shared_ptr<Evidence> evidence;
    explicit ClosureToken(std::shared_ptr<Evidence> state) : evidence(std::move(state)) {}
    ~ClosureToken() {
        if (!evidence->confirmed) ++evidence->premature;
        ++evidence->closureDestroyed;
    }
};
struct ThreadStartFault {
    int trips{};
    static inline thread_local ThreadStartFault* active{};
    static void Check(AsyncCleanupSetupPoint point) {
        if (point != AsyncCleanupSetupPoint::ThreadStart) return;
        ++active->trips;
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    ThreadStartFault() {
        active = this;
        okuflow::detail::asyncCleanupSetupCheckpoint = Check;
    }
    ~ThreadStartFault() { okuflow::detail::asyncCleanupSetupCheckpoint = nullptr; active = nullptr; }
};
void IndependentClosureLifetime(Outcome outcome, bool failThreadStart) {
    Case("independent-closure", failThreadStart ? 1 : 0, static_cast<int>(outcome));
    auto state = std::make_shared<Evidence>();
    ReleaseGuard unblock{state};
    std::weak_ptr<ClosureToken> capture;
    std::shared_ptr<AsyncCleanup> receipt;
    {
        auto token = std::make_shared<ClosureToken>(state);
        capture = token;
        const auto* const identity = token.get();
        auto work = [token, identity] {
            token->evidence->exact = token.get() == identity;
            return AwaitOutcome(*token->evidence);
        };
        token.reset();
        if (failThreadStart) {
            ThreadStartFault fault;
            receipt = AsyncCleanup::Start(std::move(work));
            Expect(fault.trips == 1, "independent closure case injects actual thread-start catch");
        } else receipt = AsyncCleanup::Start(std::move(work));
    } // even the moved-from caller closure has been destroyed
    if (!failThreadStart) {
        Expect(state->WaitEntered() && !capture.expired() && state->closureDestroyed == 0,
            "the real worker exclusively owns the independent capture while pending");
        state->Release(outcome);
    }
    const bool success = !failThreadStart && outcome == Outcome::Success;
    const auto wanted = success ? AsyncCleanup::Status::Completed : AsyncCleanup::Status::Failed;
    const bool terminal = WaitStatus(receipt, wanted);
    Expect(terminal, "independent closure case reaches its expected terminal result");
    if (terminal) {
        // The terminal acquire makes this private observation race-free. It
        // catches missing self-retention even if the worker has not exited yet.
        Expect(receipt->TestHasSelfRetention() == !success,
            "unknown completion independently retains cleanup itself; success adds no cycle");
    }
    std::weak_ptr<AsyncCleanup> cleanup = receipt;
    receipt.reset();
    if (success) {
        Expect(capture.expired() && state->closureDestroyed == 1 && state->premature == 0,
            "known completion destroys the independently owned capture exactly once");
    } else {
        Expect(!cleanup.expired() && !capture.expired() && state->closureDestroyed == 0 &&
                   state->premature == 0 && !state->confirmed,
            "cleanup and independent closure survive failure after the final receipt disappears");
    }
    Expect(state->drains == (failThreadStart ? 0 : 1),
        "independent capture is executed once, or never when thread construction fails");
}
} // namespace

int main() {
    constexpr std::array entries{Entry::Error, Entry::Timeout, Entry::Cancel, Entry::Replacement};
    constexpr std::array points{AsyncCleanupSetupPoint::ClosureConversion,
        AsyncCleanupSetupPoint::ObjectAllocation, AsyncCleanupSetupPoint::ControlBlockAllocation,
        AsyncCleanupSetupPoint::ThreadStart};
    for (auto entry : entries) {
        for (auto point : points) SetupFailure(entry, point);
        Completion(entry, Outcome::Success, false, false);
        Completion(entry, Outcome::Error, false, false);
        Completion(entry, Outcome::Throw, false, false);
    }
    Completion(Entry::Replacement, Outcome::Success, true, false);
    Completion(Entry::Cancel, Outcome::Success, false, true);
    DiscardedReceiptWhilePending();
    IndependentClosureLifetime(Outcome::Success, false);
    IndependentClosureLifetime(Outcome::Error, false);
    IndependentClosureLifetime(Outcome::Throw, false);
    IndependentClosureLifetime(Outcome::Error, true);
    Expect(cases == 35, "all 35 authored lifetime/fault cases are reached");
    std::cout << (failures ? "FAIL" : "PASS") << " retained-cleanup ownership (" << cases << " cases)\n";
    return failures ? 1 : 0;
}
