#if defined(_WIN32) && defined(OKUFLOW_ENABLE_CUDA) && OKUFLOW_ENABLE_CUDA
#include "okuflow/common/maxine_async_producer.hpp"
#include "okuflow/common/maxine_superres.hpp"
#include "okuflow/cuda/timing_event_pair.hpp"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <stdexcept>

namespace okuflow {
struct MaxineAsyncProducer::State {
    enum class Phase { Idle, Preparing, Ready, Submitted, Computing, Done, Leased, Consuming, Faulted };
    struct Desired { Ticket ticket; Config config; };
    mutable std::mutex mutex;
    std::condition_variable changed;
    Phase phase{Phase::Idle};
    bool stop{}, fault{}, retryConfiguration{};
    std::uint64_t epoch{}, jobEpoch{}, retryAfterEpoch{};
    int device{};
    std::optional<Desired> desired;
    Config prepared;
    Ticket submitted;
    Output output;
    Statistics stats;
    std::string error, configurationFailure;
    cudaStream_t stream{};
    cudaEvent_t inputReady{}, start{}, done{}, consumed{};
    void* input{};
    void* pixels{};
    std::size_t inputPitch{}, outputPitch{};
    std::unique_ptr<MaxineSuperRes> model;
    std::shared_ptr<State> quarantined; // allocation-free lifetime retention on failure
    // No destructor frees CUDA resources: cleanup is explicit and worker-only,
    // and unknown completion retains this whole closure until process exit.
};
namespace {
void Check(cudaError_t error) {
    if (error != cudaSuccess) throw std::runtime_error(cudaGetErrorString(error));
}
}
MaxineAsyncProducer::MaxineAsyncProducer(int device) : state_(std::make_shared<State>()) {
    state_->device = device;
    std::thread(&MaxineAsyncProducer::Run, state_).detach();
}
MaxineAsyncProducer::~MaxineAsyncProducer() {
    std::lock_guard lock(state_->mutex);
    state_->stop = true;
    state_->desired.reset();
    state_->changed.notify_one();
}
void MaxineAsyncProducer::Request(const Ticket& ticket, const Config& config) {
    std::lock_guard lock(state_->mutex);
    if (state_->stop || state_->fault || !ticket.processingSerial) return;
    if (state_->desired && state_->desired->ticket != ticket &&
        state_->desired->ticket != state_->submitted) ++state_->stats.desiredOverwritten;
    state_->desired = State::Desired{ticket, config};
    state_->changed.notify_one();
}
bool MaxineAsyncProducer::TrySubmit(cudaArray_t scene, std::size_t x, std::size_t y,
                                    cudaStream_t stream, const Ticket& ticket) {
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock || state_->stop || state_->fault || state_->phase != State::Phase::Ready ||
        !state_->desired || state_->desired->ticket != ticket || ticket == state_->submitted ||
        state_->desired->config != state_->prepared || state_->retryConfiguration || !scene) return false;
    state_->submitted = ticket;
    state_->jobEpoch = state_->epoch;
    if (!state_->configurationFailure.empty()) {
        state_->output = {ticket, nullptr, 0, 0, 0, -1, false, state_->configurationFailure};
        state_->phase = State::Phase::Done;
        return false;
    }
    // The caller's stream orders the snapshot after base-scene production.
    // This is the only borrowed reference; it does not escape this enqueue.
    auto result = cudaMemcpy2DFromArrayAsync(state_->input, state_->inputPitch, scene, x, y,
        static_cast<std::size_t>(state_->prepared.sourceWidth) * 4, state_->prepared.sourceHeight,
        cudaMemcpyDeviceToDevice, stream);
    if (result == cudaSuccess) result = cudaEventRecord(state_->inputReady, stream);
    if (result != cudaSuccess) {
        state_->error = cudaGetErrorString(result);
        state_->fault = true; state_->phase = State::Phase::Faulted;
        state_->changed.notify_one();
        return false;
    }
    ++state_->stats.submitted;
    state_->phase = State::Phase::Submitted;
    state_->changed.notify_one();
    return true;
}
bool MaxineAsyncProducer::TryTake(const Ticket& current, Output& output) {
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    if (!lock || state_->phase != State::Phase::Done) return false;
    if (state_->stop || state_->jobEpoch != state_->epoch ||
        !state_->desired || state_->output.ticket != current || state_->desired->ticket != current) {
        output.gpuMs = state_->output.gpuMs;
        output.config = state_->output.config;
        ++state_->stats.staleDiscarded;
        state_->phase = State::Phase::Ready;
        state_->changed.notify_one();
        return false;
    }
    output = state_->output;
    state_->phase = State::Phase::Leased;
    return true;
}
bool MaxineAsyncProducer::ReleaseOutput(cudaStream_t stream) {
    std::lock_guard lock(state_->mutex);
    if (state_->phase != State::Phase::Leased || state_->fault) return false;
    const auto result = cudaEventRecord(state_->consumed, stream);
    if (result != cudaSuccess) {
        state_->error = cudaGetErrorString(result);
        state_->fault = true; state_->phase = State::Phase::Faulted;
        state_->changed.notify_one();
        return false;
    }
    state_->phase = State::Phase::Consuming;
    state_->changed.notify_one();
    return true;
}
void MaxineAsyncProducer::Invalidate(bool retryFailures) {
    std::lock_guard lock(state_->mutex);
    ++state_->epoch;
    state_->desired.reset();
    if (retryFailures) {
        state_->retryAfterEpoch = state_->epoch;
        if (!state_->configurationFailure.empty()) state_->retryConfiguration = true;
    }
    if (state_->phase == State::Phase::Done) {
        ++state_->stats.staleDiscarded;
        state_->phase = State::Phase::Ready;
    }
    state_->changed.notify_one();
}
bool MaxineAsyncProducer::HasWork() const {
    std::unique_lock lock(state_->mutex, std::try_to_lock);
    return lock && !state_->stop && !state_->fault &&
        (state_->phase == State::Phase::Done || (state_->phase == State::Phase::Ready &&
         state_->desired && !state_->retryConfiguration && state_->desired->config == state_->prepared &&
         state_->desired->ticket != state_->submitted));
}
bool MaxineAsyncProducer::IsFaulted() const { std::lock_guard lock(state_->mutex); return state_->fault; }
std::string MaxineAsyncProducer::LastError() const { std::lock_guard lock(state_->mutex); return state_->error; }
MaxineAsyncProducer::Statistics MaxineAsyncProducer::Stats() const {
    std::lock_guard lock(state_->mutex); return state_->stats;
}
void MaxineAsyncProducer::Run(std::shared_ptr<State> state) {
    // Retaining the closure also retains every CUDA handle, private allocation,
    // adapter and any still-running SDK task. No surface/presenter is captured.
    const auto retain = [&] { state->quarantined = state; };
    try {
        Check(cudaSetDevice(state->device));
        int least{}, greatest{};
        Check(cudaDeviceGetStreamPriorityRange(&least, &greatest));
        Check(cudaStreamCreateWithPriority(&state->stream, cudaStreamNonBlocking, least));
        Check(cudaEventCreateWithFlags(&state->inputReady, cudaEventDisableTiming));
        cudaError_t timingError = cudaSuccess;
        if (!EnsureTimingEventPair(state->start,state->done,[&](cudaEvent_t* event) {
                timingError = cudaEventCreate(event); return timingError == cudaSuccess;
            },[](cudaEvent_t event) { (void)cudaEventDestroy(event); })) Check(timingError);
        Check(cudaEventCreateWithFlags(&state->consumed, cudaEventDisableTiming));
        state->model = std::make_unique<MaxineSuperRes>();
        std::unique_lock lock(state->mutex);
        for (;;) {
            state->changed.wait(lock, [&] {
                return state->stop || state->fault || state->phase == State::Phase::Submitted ||
                    state->phase == State::Phase::Consuming ||
                    ((state->phase == State::Phase::Idle || state->phase == State::Phase::Ready) &&
                     state->desired && (state->phase == State::Phase::Idle || state->retryConfiguration || state->prepared != state->desired->config));
            });
            if (state->fault) { lock.unlock(); retain(); return; }
            if (state->phase == State::Phase::Consuming) {
                lock.unlock(); Check(cudaEventSynchronize(state->consumed)); lock.lock();
                state->phase = State::Phase::Ready;
                continue;
            }
            if (state->phase == State::Phase::Submitted) {
                const auto ticket = state->submitted;
                const auto config = state->prepared;
                state->phase = State::Phase::Computing;
                const bool stopping = state->stop;
                lock.unlock();
                Check(cudaStreamWaitEvent(state->stream, state->inputReady, 0));
                bool success = false;
                std::string error;
                if (!stopping) {
                    Check(cudaEventRecord(state->start, state->stream));
                    success = state->model->Run(state->input, state->inputPitch, state->pixels, state->outputPitch);
                    if (state->model->IsFaulted()) throw std::runtime_error(state->model->LastError());
                    if (!success) error = state->model->LastError();
                }
                Check(cudaEventRecord(state->done, state->stream));
                Check(cudaEventSynchronize(state->done)); // worker only, never presentation
                float elapsed = -1;
                if (!stopping) Check(cudaEventElapsedTime(&elapsed, state->start, state->done));
                lock.lock();
                ++state->stats.completed;
                if (!success && !stopping) {
                    state->configurationFailure = error;
                    if (state->jobEpoch < state->retryAfterEpoch) state->retryConfiguration = true;
                }
                state->output = {ticket, state->pixels, state->outputPitch,
                    config.outputWidth, config.outputHeight, elapsed, success, std::move(error), config};
                state->phase = State::Phase::Done;
                if (state->jobEpoch != state->epoch || !state->desired) {
                    ++state->stats.staleDiscarded;
                    state->phase = State::Phase::Ready;
                }
                continue;
            }
            if (state->stop) {
                if (state->phase == State::Phase::Leased) { lock.unlock(); retain(); return; }
                break;
            }
            const auto desired = *state->desired;
            const bool retry = state->retryConfiguration;
            const auto preparationEpoch = state->epoch;
            state->retryConfiguration = false;
            state->phase = State::Phase::Preparing;
            lock.unlock();
            // Previous producer and consumer events have retired before resize.
            Check(cudaStreamSynchronize(state->stream));
            if (retry) {
                state->model->Teardown();
                if (state->model->IsFaulted()) throw std::runtime_error(state->model->LastError());
            }
            if (state->input) { Check(cudaFree(state->input)); state->input = nullptr; }
            if (state->pixels) { Check(cudaFree(state->pixels)); state->pixels = nullptr; }
            Check(cudaMallocPitch(&state->input, &state->inputPitch,
                static_cast<std::size_t>(desired.config.sourceWidth) * 4, desired.config.sourceHeight));
            Check(cudaMallocPitch(&state->pixels, &state->outputPitch,
                static_cast<std::size_t>(desired.config.outputWidth) * 4, desired.config.outputHeight));
            state->model->SetStrength(desired.config.strength);
            const bool ready = state->model->Ensure(desired.config.sourceWidth, desired.config.sourceHeight,
                desired.config.outputWidth, desired.config.outputHeight, state->stream);
            if (state->model->IsFaulted()) throw std::runtime_error(state->model->LastError());
            const auto error = ready ? std::string{} : state->model->LastError();
            lock.lock();
            state->prepared = desired.config;
            state->configurationFailure = error;
            if (!error.empty() && preparationEpoch < state->retryAfterEpoch) state->retryConfiguration = true;
            state->phase = State::Phase::Ready;
        }
        lock.unlock();
        Check(cudaStreamSynchronize(state->stream));
        state->model->Teardown();
        if (state->model->IsFaulted()) throw std::runtime_error(state->model->LastError());
        state->model.reset();
        if (state->input) Check(cudaFree(state->input));
        if (state->pixels) Check(cudaFree(state->pixels));
        Check(cudaEventDestroy(state->inputReady)); Check(cudaEventDestroy(state->start));
        Check(cudaEventDestroy(state->done)); Check(cudaEventDestroy(state->consumed));
        Check(cudaStreamDestroy(state->stream));
    } catch (const std::exception& error) {
        { std::lock_guard lock(state->mutex); state->error = error.what(); state->fault = true; state->phase = State::Phase::Faulted; }
        retain();
    } catch (...) {
        { std::lock_guard lock(state->mutex); state->error = "NVIDIA SuperRes private producer failed";
          state->fault = true; state->phase = State::Phase::Faulted; }
        retain();
    }
}
}
#endif
