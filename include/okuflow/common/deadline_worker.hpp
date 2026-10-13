#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace okuflow {

// One independently owned serial worker for third-party host calls which have
// no cancellation API. A deadline is a permanent failure, never permission to
// destroy resources borrowed by the task. The caller must retain those resources.
class DeadlineWorker {
public:
    enum class Result { Completed, TimedOut, Rejected, Threw };
    DeadlineWorker() : state_(std::make_shared<State>()) {
        std::thread([state = state_] {
            std::unique_lock lock(state->mutex);
            for (;;) {
                state->changed.wait(lock, [&] { return state->stop || state->task; });
                if (!state->task) return;
                auto task = std::move(state->task);
                state->task = {};
                lock.unlock();
                bool threw = false;
                try { task(); } catch (...) { threw = true; }
                task = {};
                lock.lock();
                state->threw = threw;
                state->done = true;
                state->changed.notify_all();
                if (state->stop) return;
            }
        }).detach();
    }
    ~DeadlineWorker() {
        std::lock_guard lock(state_->mutex);
        state_->stop = true;
        state_->changed.notify_all();
    }
    DeadlineWorker(const DeadlineWorker&) = delete;
    DeadlineWorker& operator=(const DeadlineWorker&) = delete;

    Result Execute(std::function<void()> task, std::chrono::milliseconds timeout) {
        std::unique_lock lock(state_->mutex);
        if (state_->stop || !state_->done) return Result::Rejected;
        state_->done = false;
        state_->threw = false;
        state_->task = std::move(task);
        state_->changed.notify_all();
        if (!state_->changed.wait_for(lock, timeout, [&] { return state_->done; })) {
            state_->stop = true;
            return Result::TimedOut;
        }
        return state_->threw ? Result::Threw : Result::Completed;
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::function<void()> task;
        bool done{true}, stop{false}, threw{false};
    };
    std::shared_ptr<State> state_;
};
}
