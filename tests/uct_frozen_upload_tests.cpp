#include "../src/common/uct_frozen_upload.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace {
using okuflow::AsyncCleanup;
using okuflow::uct::detail::FrozenUploadRetirement;
using okuflow::uct::detail::RetireOwnedState;
int failures{};
void Expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
struct Lease {};
struct PinnedStorage { std::vector<int> descriptors{11, 22, 33}; };
struct State {
    std::shared_ptr<Lease> inFlight{std::make_shared<Lease>()};
    std::shared_ptr<PinnedStorage> pinned{std::make_shared<PinnedStorage>()};
    std::shared_ptr<State> retirementQuarantine;
};
struct Witness {
    std::weak_ptr<State> state;
    std::weak_ptr<Lease> lease;
    std::weak_ptr<PinnedStorage> pinned;
    explicit Witness(const std::shared_ptr<State>& value) : state(value), lease(value->inFlight), pinned(value->pinned) {}
    bool Alive() const { return !state.expired() && !lease.expired() && !pinned.expired(); }
    bool Gone() const { return state.expired() && lease.expired() && pinned.expired(); }
};
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{}, release{};
    std::atomic<bool> exactClosure{};
    bool Work(const std::shared_ptr<State>& state, const State* expected) {
        std::unique_lock lock(mutex);
        exactClosure = state.get() == expected && state->inFlight && state->pinned &&
            !state->retirementQuarantine && state->pinned->descriptors == std::vector<int>({11, 22, 33});
        entered = true; changed.notify_all();
        return changed.wait_for(lock, std::chrono::seconds(3), [&] { return release; });
    }
    bool WaitEntered() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; });
    }
    void Release() {
        { std::lock_guard lock(mutex); release = true; }
        changed.notify_all();
    }
};
bool WaitCompleted(const std::shared_ptr<AsyncCleanup>& receipt) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (receipt->Poll() == AsyncCleanup::Status::Running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return receipt->Poll() == AsyncCleanup::Status::Completed;
}

void NoPendingUploadKeepsEngines(bool groupCompleted) {
    auto owner = std::make_shared<State>();
    const auto* original = owner.get();
    Witness witness(owner);
    FrozenUploadRetirement upload;
    if (groupCompleted) { upload.Queued(); upload.Completed(); }
    // Otherwise: a zero-crop frozen region queues no descriptor H2D.
    int retireCalls{};
    const auto receipt = upload.Cancel(owner, [&](std::shared_ptr<State>) {
        ++retireCalls; return std::shared_ptr<AsyncCleanup>{};
    });
    Expect(!receipt && !retireCalls && owner.get() == original,
        "zero crops or a completed first group keep the exact reusable engines");
    Expect(witness.lease.expired() && !witness.state.expired() && !witness.pinned.expired(),
        "the existing completed/no-upload cancellation releases only inFlight");
    owner.reset();
    Expect(witness.Gone(), "completed/no-upload State and pinned storage release normally");
}

void PendingUploadTransfersWholeClosure(bool anotherRegion, bool throwFirst) {
    auto owner = std::make_shared<State>();
    const auto* original = owner.get();
    Witness witness(owner);
    FrozenUploadRetirement upload;
    upload.Queued();
    if (anotherRegion) { upload.Completed(); upload.Queued(); }
    if (throwFirst) {
        bool threw{};
        try { upload.Cancel(owner, [](std::shared_ptr<State>) -> std::shared_ptr<AsyncCleanup> {
            throw std::bad_alloc();
        }); } catch (const std::bad_alloc&) { threw = true; }
        Expect(threw && owner.get() == original && witness.Alive(),
            "failed retirement handoff retains the original State, lease and pinned descriptors");
    }
    auto gate = std::make_shared<Gate>();
    int retireCalls{};
    // This is the production guard's actual retirement handoff. Only the
    // native completion operation is replaced by a deterministic gated wait.
    const auto receipt = upload.Cancel(owner, [&](std::shared_ptr<State> exactState) {
        ++retireCalls;
        return AsyncCleanup::Start([state = std::move(exactState), gate, original] {
            return gate->Work(state, original);
        });
    });
    Expect(receipt && !owner && retireCalls == 1,
        "pending cancellation transfers the exact State out of the reusable owner");
    if (!receipt) return;
    Expect(gate->WaitEntered(), "existing async cleanup enters its gated completion operation");
    Expect(gate->exactClosure && receipt->Poll() == AsyncCleanup::Status::Running && witness.Alive(),
        "cancellation returns while all three exact resources survive until completion");
    owner = std::make_shared<State>();
    owner->pinned->descriptors = {99};
    Expect(owner.get() != original && witness.Alive(),
        "a subsequent job cannot overwrite the retiring pinned descriptor storage");
    gate->Release();
    Expect(WaitCompleted(receipt) && witness.Gone(),
        "successful existing async retirement releases State, input lease and pinned storage together");
}

enum class FinalHandoff { Accepted, AllocationFailure, EmptyReceipt };
void CancellationFailureReachesFinalRetirement(FinalHandoff finalHandoff) {
    auto owner = std::make_shared<State>();
    const auto* original = owner.get();
    Witness witness(owner);
    auto gate = std::make_shared<Gate>();
    FrozenUploadRetirement upload;
    upload.Queued();
    int attempts{};
    bool caught{};
    std::shared_ptr<AsyncCleanup> receipt;
    const auto retire = [&](std::shared_ptr<State> exactState) -> std::shared_ptr<AsyncCleanup> {
        ++attempts;
        if (attempts == 1 || finalHandoff == FinalHandoff::AllocationFailure) throw std::bad_alloc();
        if (finalHandoff == FinalHandoff::EmptyReceipt) return {};
        return AsyncCleanup::Start([state = std::move(exactState), gate, original] {
            return gate->Work(state, original);
        });
    };
    static_assert(noexcept(RetireOwnedState(owner, retire)));
    try {
        upload.Cancel(owner, retire);
    } catch (const std::bad_alloc&) {
        caught = true;
        Expect(owner.get() == original && witness.Alive(),
            "first cancellation allocation failure reaches catch with the full exact owner intact");
        // The actual production Retire()/Preparation-destructor policy, not
        // a second direct Cancel retry: its final failed handoff must not throw.
        receipt = RetireOwnedState(owner, retire);
    }
    Expect(caught && attempts == 2 && !owner,
        "outer final retirement consumes reusable ownership after the failed cancellation handoff");
    if (finalHandoff == FinalHandoff::Accepted) {
        Expect(receipt && gate->WaitEntered() && witness.Alive() && gate->exactClosure,
            "a successful outer handoff retains State, lease and pinned storage through actual async completion");
        gate->Release();
        if (receipt) Expect(WaitCompleted(receipt) && witness.Gone(),
            "normal outer async retirement releases all resources without a self-cycle");
    } else {
        Expect(!receipt && witness.Alive(),
            "repeated failure quarantines all resources without fabricating a completion receipt");
        auto quarantined = witness.state.lock();
        Expect(quarantined && quarantined.get() == original &&
            quarantined->retirementQuarantine.get() == original && quarantined->inFlight && quarantined->pinned,
            "the pre-existing State self-owner preserves its entire closure after final handoff failure");
        quarantined.reset();
        owner = std::make_shared<State>();
        owner->pinned->descriptors = {99};
        Expect(owner.get() != original && witness.Alive(),
            "a new reusable State cannot overwrite quarantined input or pinned descriptors");
        // Intentionally retained until this fixture process exits, as in the
        // production unknown-completion policy. No native work is submitted.
    }
}
}

int main() {
    NoPendingUploadKeepsEngines(false); // zero crops / no frozen upload
    NoPendingUploadKeepsEngines(true);  // cancellation after first completed group
    PendingUploadTransfersWholeClosure(false, false); // first upload before any group wait
    PendingUploadTransfersWholeClosure(true, false);  // a later frozen region re-arms pending upload
    PendingUploadTransfersWholeClosure(false, true);  // retirement allocation/handoff error
    CancellationFailureReachesFinalRetirement(FinalHandoff::AllocationFailure);
    CancellationFailureReachesFinalRetirement(FinalHandoff::EmptyReceipt);
    CancellationFailureReachesFinalRetirement(FinalHandoff::Accepted);
    std::cout << (failures ? "FAIL" : "PASS") << " frozen-upload retirement ownership\n";
    return failures ? 1 : 0;
}
