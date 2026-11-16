#pragma once
// Recognizer load beside the detector (round 11, on by default, exact).
//
// The preparation thread used to prepare the detector and then the
// recognizer. The two are independent until the State is published, so the
// recognizer's cached-plan load (source read and verify, runtime, plan read
// and verify, deserialization, engine contract, execution context) now runs
// on one helper thread while the preparation thread prepares and smoke-checks
// the detector. Everything that decides something stays where it was:
//
// - The helper never builds an engine and never touches the stream: a cache
//   miss ends its load as "needs build" and the build runs in the original
//   place, after the detector, through the original call.
// - The attempt latch is taken early (`Start`), after the failure memo was
//   read, and the memo is read again at the original point (`Adopt`). A memo,
//   a latch already held by another preparation, or a thread that cannot be
//   started at `Start` means no helper: the original code then runs unchanged
//   and takes both decisions itself.
// - At the original recognizer point `Adopt` joins the helper and hands its
//   outcome to the original code: a loaded engine continues to the original
//   compatibility check, a cache miss to the original build, a failure is
//   rethrown into the original catch, so the BF16-to-FP32 fallback, the
//   failure memo and the latch follow the same rules as before.
// - If the detector side fails or is cancelled first, the object is
//   destroyed without `Adopt`: it stops the helper, joins it and releases the
//   early latch, because the sequential code never reached the recognizer.
//   The helper queued no stream work, so its completion is known.
//
// The preparation thread joins the helper on every path, so the helper never
// outlives the preparation, and nothing joins either thread at process exit.
// No verified state survives the call. Vendor thread rules: "Round 11" in
// docs/uct_okuread_port.md.
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

namespace okuflow::uct::preparation_overlap {

struct Steps {
    // Throws a typed refusal when a failure memo exists for this attempt.
    std::function<void()> refuseRemembered;
    // Takes this process's attempt latch; false when it is already held.
    std::function<bool()> begin;
    // Releases the latch as never attempted (completion is known: nothing queued).
    std::function<void()> release;
    // Helper thread. Loads without building; returns true for "needs build".
    // `stop` stays valid after this object is gone (objects made by the load
    // may keep it).
    std::function<bool(const std::function<bool()>& stop)> load;
};

struct Adoption {
    enum class Kind { NotStarted, Loaded, NeedsBuild, Failed };
    Kind kind{Kind::NotStarted};
    std::exception_ptr failure; // Failed only: the helper's own exception, unchanged
};

class Ahead {
public:
    Ahead() = default;
    Ahead(const Ahead&) = delete;
    Ahead& operator=(const Ahead&) = delete;
    ~Ahead() noexcept
    {
        if (shared_) shared_->abandoned = true; // the detector side ended first
        Join();
        if (begun_ && !adopted_) Release();
    }
    // Preparation thread, before the detector. `stop` must be thread-safe.
    // True when the helper runs; false leaves every decision to the original
    // sequential code (no latch kept, nothing loaded).
    bool Start(Steps steps, std::function<bool()> stop)
    {
        if (started_ || begun_) return false;
        steps_ = std::move(steps);
        try { steps_.refuseRemembered(); }
        catch (...) { return false; } // the original code reads it again and refuses there
        try { begun_ = steps_.begin(); }
        catch (...) { begun_ = false; }
        if (!begun_) return false;
        try {
            shared_ = std::make_shared<Shared>();
            shared_->stop = std::move(stop);
            const auto shared = shared_;
            const auto load = steps_.load;
            thread_ = std::thread([shared, load] {
                const std::function<bool()> stop = [shared] { return shared->abandoned.load() || shared->stop(); };
                try { shared->needsBuild = load(stop); }
                catch (...) { shared->failure = std::current_exception(); }
            });
        } catch (...) { // std::system_error or allocation: no helper
            shared_.reset();
            Release();
            begun_ = false;
            return false;
        }
        started_ = true;
        return true;
    }
    bool Started() const { return started_; }
    // Preparation thread, at the original recognizer point. After NotStarted
    // the caller runs its original code. Loaded, NeedsBuild and Failed hand
    // the latch to the caller, which then owns it exactly as if it had taken
    // it there itself. A memo found now refuses as the original code would:
    // the latch is released and the refusal rethrown.
    Adoption Adopt()
    {
        if (!started_ || adopted_) return {};
        Join(); // not abandoned: the load runs to its own end
        adopted_ = true;
        try { steps_.refuseRemembered(); }
        catch (...) {
            Release();
            begun_ = false;
            throw;
        }
        if (shared_->failure) return {Adoption::Kind::Failed, shared_->failure};
        return {shared_->needsBuild ? Adoption::Kind::NeedsBuild : Adoption::Kind::Loaded, {}};
    }

private:
    struct Shared {
        std::atomic<bool> abandoned{};
        std::function<bool()> stop;
        bool needsBuild{};           // written by the helper, read after join
        std::exception_ptr failure;  // likewise
    };
    void Join() noexcept
    {
        if (thread_.joinable()) thread_.join();
    }
    void Release() noexcept
    {
        try { steps_.release(); } catch (...) {} // a latch left held only refuses BF16 later
    }
    Steps steps_;
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
    bool begun_{}, started_{}, adopted_{};
};

} // namespace okuflow::uct::preparation_overlap
