#pragma once
// Recognizer load beside the detector (round 11, on by default, exact).
//
// The preparation thread used to prepare the detector and then the
// recognizer. The two are independent until the State is published, so the
// production BF16 recognizer's cached-plan load (source read and verify,
// runtime, plan read and verify, deserialization, engine contract, execution
// context) runs on one helper thread while the preparation thread prepares
// and smoke-checks the detector. Every decision stays with the sequential
// code, in its order, at its point:
//
// - The helper holds nothing: no attempt latch, no memo, no file or
//   directory write, no stream work, no builder. A cache miss ends its load
//   as "needs build". It reports to its own logger (State::recognizerAheadLogger),
//   so neither engine's message reaches the other's failure text.
// - Its stop is cancellation, abandonment or its own deadline, which starts
//   when the helper starts and governs only the helper's own work.
// - `Decide` runs at the original recognizer point, with that point's fresh
//   deadline. It joins the helper, then reads the failure memo and takes the
//   latch exactly as the sequential code does. A memo or a held latch
//   discards what the helper loaded and refuses as before. With the latch:
//   a loaded engine is adopted and continues to the original compatibility
//   check; a cache miss, or any failure that is not a fault (artifact
//   refusal, interrupted verification, plain runtime error, including the
//   helper's own deadline), is discarded and the original load runs in place,
//   so the sequential code decides it again under its own deadline; a fault
//   (CUDA, TensorRT API, allocation, unknown) is attached to the State and
//   raised, as the sequential code's own load would have raised it.
// - `Settle` abandons the helper (stop, join) whenever the detector side ends
//   first (`DetectorSide`), before the detector falls back to FP32, before any
//   detector build (so a build never shares the device with the helper), and
//   after the recognizer point as a guard. Without a fault the helper's
//   Network is discarded and the preparation continues exactly as the
//   sequential code would. A fault is never discarded: the recognizer is
//   held as after a terminal fault in its own stage (latch taken unless a
//   memo or another holder refuses it, never released, no memo written) and
//   the fault is raised with the detector side's failure and the helper's
//   message, so it cannot be recorded against the detector.
//
// The preparation thread resolves the helper on every path, so the helper
// never outlives the preparation, and nothing joins either thread at process
// exit. No verified state survives the call. Vendor thread rules: "Round 11"
// in docs/uct_okuread_port.md.
#include "uct_tensorrt_failures.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace okuflow::uct::preparation_overlap {

// A failure the sequential code treats as terminal wherever it happens.
inline bool IsFault(const std::exception_ptr& failure) noexcept
{
    return failure && !detail::AllowsTensorRtCapacityFallback(failure) && !detail::IsVerificationInterrupted(failure);
}

inline std::string Message(const std::exception_ptr& failure) noexcept
{
    try {
        try { std::rethrow_exception(failure); }
        catch (const std::bad_alloc&) { return "out of memory"; }
        catch (const std::exception& error) { return error.what(); }
        catch (...) { return "unknown exception"; }
    } catch (...) { return {}; }
}

// Raises the helper's fault as a fault of the same class (CUDA stays CUDA,
// everything else is a TensorRT API fault), carrying `cause`, the failure of
// the side that ended first or refused, when there is one.
[[noreturn]] inline void RaiseFault(const std::exception_ptr& fault, const std::exception_ptr& cause = {})
{
    auto message = "recognizer prepared beside the detector failed: " + Message(fault);
    if (cause) message = Message(cause) + "; " + message;
    bool cuda = false;
    try { std::rethrow_exception(fault); }
    catch (const detail::CudaFailure&) { cuda = true; }
    catch (...) {}
    if (cuda) throw detail::CudaFailure(message);
    throw detail::TensorRtApiFailure(message);
}

struct Outcome {
    enum class Kind { NotStarted, Loaded, NeedsBuild, Failed, Fault };
    Kind kind{Kind::NotStarted};
    std::exception_ptr failure; // Failed and Fault: the helper's exception, unchanged
};

class Ahead {
public:
    // Helper thread. Loads without building; returns true for "needs build".
    // `stop` stays valid after this object is gone (objects made by the load
    // may keep it).
    using Load = std::function<bool(const std::function<bool()>& stop)>;
    Ahead() = default;
    Ahead(const Ahead&) = delete;
    Ahead& operator=(const Ahead&) = delete;
    ~Ahead() noexcept { (void)Abandon(); } // production resolves explicitly; this is the last guard
    // Preparation thread, before the detector. `refuseRemembered` throws when
    // a failure memo exists for `key`: then no helper (the sequential code
    // reads the memo again and refuses). `stop` must be thread-safe.
    bool Start(std::string key, const std::function<void()>& refuseRemembered, Load load, std::function<bool()> stop)
    {
        if (started_) return false;
        try { refuseRemembered(); }
        catch (...) { return false; }
        try {
            auto shared = std::make_shared<Shared>();
            shared->stop = std::move(stop);
            thread_ = std::thread([shared, load = std::move(load)] {
                const std::function<bool()> stop = [shared] { return shared->abandoned.load() || shared->stop(); };
                try { shared->needsBuild = load(stop); }
                catch (...) { shared->failure = std::current_exception(); }
            });
            shared_ = std::move(shared);
        } catch (...) { // std::system_error or allocation: no helper, nothing loaded
            return false;
        }
        key_ = std::move(key);
        started_ = true;
        return true;
    }
    bool Started() const { return started_; }
    // Started and neither taken nor abandoned: the helper may still run.
    bool Pending() const { return started_ && !resolved_; }
    const std::string& Key() const { return key_; }
    // Preparation thread, at the recognizer point: joins without stopping.
    Outcome Take()
    {
        if (!Pending()) return {};
        Join();
        resolved_ = true;
        if (shared_->failure)
            return {IsFault(shared_->failure) ? Outcome::Kind::Fault : Outcome::Kind::Failed, shared_->failure};
        return {shared_->needsBuild ? Outcome::Kind::NeedsBuild : Outcome::Kind::Loaded, {}};
    }
    // Stops and joins. Returns the helper's fault, if it ended in one.
    std::exception_ptr Abandon() noexcept
    {
        if (!Pending()) return {};
        shared_->abandoned = true;
        Join();
        resolved_ = true;
        return IsFault(shared_->failure) ? shared_->failure : std::exception_ptr{};
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
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
    std::string key_;
    bool started_{}, resolved_{};
};

// The recognizer point's actions, in the production code's terms. Each is
// what the sequential code does there; the helper adds `adopt` and `discard`.
struct Point {
    std::function<void()> refuseRemembered; // throws the memo refusal
    std::function<bool()> begin;            // takes the session attempt latch
    std::function<void()> refuseHeld;       // throws the refusal for a latch held elsewhere
    std::function<void()> own;              // the State owns the latch from here
    std::function<void()> adopt;            // the helper's Network becomes the attached recognizer
    std::function<void()> discard;          // drops the helper's Network (it queued nothing)
    std::function<void()> loadInPlace;      // the sequential load: new Network, attached, loaded
};

// Abandons a pending helper; see the file comment. `cause` is the failure of
// the side that ended first, when there is one.
inline void Settle(Ahead& ahead, const Point& point, const std::exception_ptr& cause = {})
{
    if (!ahead.Pending()) return;
    const auto fault = ahead.Abandon();
    if (!fault) {
        point.discard();
        return;
    }
    // As after a terminal fault in the recognizer's own load: memo first,
    // then the latch, which stays held. The Network stays with the State.
    try {
        bool remembered = false;
        try { point.refuseRemembered(); }
        catch (...) { remembered = true; }
        if (!remembered && point.begin()) point.own();
    } catch (...) {} // the fault is raised either way
    RaiseFault(fault, cause);
}

// The detector side: on any failure the helper is settled before the failure
// leaves, so a helper fault is raised with it and nothing else changes.
template<class Stage> void DetectorSide(Ahead& ahead, const Point& point, Stage&& stage)
{
    try {
        std::forward<Stage>(stage)();
    } catch (...) {
        Settle(ahead, point, std::current_exception());
        throw;
    }
}

// The original recognizer point (memo, latch, load). Returns with the
// recognizer loaded and attached, or throws what the sequential code throws.
// `key` is the attempt of this point; a helper started for another attempt is
// settled and never adopted.
inline void Decide(Ahead* ahead, const std::string& key, const Point& point)
{
    using Kind = Outcome::Kind;
    Outcome outcome;
    if (ahead && ahead->Pending()) {
        if (ahead->Key() != key) Settle(*ahead, point);
        else outcome = ahead->Take();
    }
    try {
        point.refuseRemembered();
        if (!point.begin()) point.refuseHeld();
    } catch (...) {
        // The sequential code refuses here without loading. A fault the
        // helper met is still a fault on this device and is not discarded.
        if (outcome.kind == Kind::Fault) RaiseFault(outcome.failure, std::current_exception());
        if (outcome.kind != Kind::NotStarted) point.discard();
        throw;
    }
    point.own();
    switch (outcome.kind) {
    case Kind::Loaded:
        point.adopt();
        return;
    case Kind::Fault:
        point.adopt();
        std::rethrow_exception(outcome.failure);
    case Kind::NeedsBuild:
    case Kind::Failed:
        point.discard();
        [[fallthrough]];
    case Kind::NotStarted:
        point.loadInPlace();
        return;
    }
}

} // namespace okuflow::uct::preparation_overlap
