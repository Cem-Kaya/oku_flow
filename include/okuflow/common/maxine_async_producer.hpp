#pragma once
#include <cuda_runtime_api.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace okuflow {
// One private inference slot. No SDK calls, allocation, GPU waits or joins on
// the caller. Request replaces a CPU descriptor; Submit alone snapshots pixels.
class MaxineAsyncProducer {
public:
    struct Ticket {
        std::uint64_t cameraEpoch{}, frameSequence{}, processingSerial{}, settingsGeneration{}, requestGeneration{};
        bool operator==(const Ticket&) const = default;
    };
    struct Config {
        unsigned sourceWidth{}, sourceHeight{}, outputWidth{}, outputHeight{};
        float strength{};
        bool operator==(const Config&) const = default;
    };
    struct Output {
        Ticket ticket;
        void* pixels{};
        std::size_t pitch{};
        unsigned width{}, height{};
        float gpuMs{-1};
        bool success{};
        std::string error;
        Config config;
        // A rejected result exposes only timing/config, never its pixel lease.

    };
    struct Statistics { std::uint64_t submitted{}, completed{}, staleDiscarded{}, desiredOverwritten{}; };
    explicit MaxineAsyncProducer(int cudaDevice);
    ~MaxineAsyncProducer();
    MaxineAsyncProducer(const MaxineAsyncProducer&) = delete;
    MaxineAsyncProducer& operator=(const MaxineAsyncProducer&) = delete;
    void Request(const Ticket&, const Config&);
    bool TrySubmit(cudaArray_t fullScene, std::size_t sourceXBytes, std::size_t sourceY,
                   cudaStream_t copyStream, const Ticket&);
    bool TryTake(const Ticket& current, Output&);
    // Every successful take, including errors, needs one release. Output is
    // exclusively leased until the consumer's event retires on the worker.
    bool ReleaseOutput(cudaStream_t consumerStream);
    void Invalidate(bool retryFailures = true);
    bool HasWork() const;
    bool IsFaulted() const;
    std::string LastError() const;
    Statistics Stats() const;
private:
    struct State;
    std::shared_ptr<State> state_;
    static void Run(std::shared_ptr<State>);
};
}
