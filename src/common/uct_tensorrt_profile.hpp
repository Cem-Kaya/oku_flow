#pragma once

// Private, explicitly enabled OCR worker instrumentation. Event resources live
// with the inference state, including its unknown-completion quarantine.
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <cuda_runtime.h>
#include <stdexcept>
#include <vector>

namespace okuflow::uct {

class TensorRtProfile {
public:
    enum Stage { Normalize, Detector, Regions, Rectify, Recognizer, Ctc, Metadata, StageCount };
    TensorRtProfile() : enabled_(qEnvironmentVariable("OKUFLOW_UCT_TRT_PROFILE") == "1") {
        // Allocate the bounded host slots before any CUDA event exists, so
        // publishing a newly created pair cannot leak it on host allocation.
        if (enabled_) events_.reserve(kMaximumPairs);
    }
    ~TensorRtProfile() {
        for (const auto& pair : events_) {
            if (pair.begin) cudaEventDestroy(pair.begin);
            if (pair.end) cudaEventDestroy(pair.end);
        }
    }
    bool Enabled() const { return enabled_; }
    void Reset() {
        used_ = omitted_ = detectorPasses_ = recognizerCalls_ = recognizerSamples_ = 0;
        shapeSets_ = addressSets_ = 0;
        metadataBytes_ = 0;
        inputBytes_ = outputBytes_ = outputLimitBytes_ = 0;
        maximumBatch_ = 0;
        enqueueMs_ = mergeMs_ = 0;
        if (enabled_) {
            Check(cudaMemGetInfo(&freeBefore_, &totalBytes_));
            wall_.start();
        }
    }
    template<class Operation> void Measure(Stage stage, cudaStream_t stream, Operation operation) {
        if (!enabled_) { operation(); return; }
        if (used_ == kMaximumPairs) { ++omitted_; operation(); return; }
        const auto index = used_++;
        if (index == events_.size()) {
            EventPair created;
            const auto begin = cudaEventCreate(&created.begin);
            const auto end = begin == cudaSuccess ? cudaEventCreate(&created.end) : begin;
            if (begin != cudaSuccess || end != cudaSuccess) {
                if (created.begin) cudaEventDestroy(created.begin);
                if (created.end) cudaEventDestroy(created.end);
                --used_;
                Check(begin != cudaSuccess ? begin : end);
            }
            events_.push_back(created);
        }
        auto& pair = events_[index];
        pair.stage = stage;
        Check(cudaEventRecord(pair.begin, stream));
        operation();
        Check(cudaEventRecord(pair.end, stream));
    }
    void NetworkCall(bool detector, int batch, double hostMs, int shapeSets, int addressSets) {
        if (!enabled_) return;
        if (detector) ++detectorPasses_;
        else { ++recognizerCalls_; recognizerSamples_ += batch; }
        enqueueMs_ += hostMs;
        shapeSets_ += shapeSets;
        addressSets_ += addressSets;
    }
    void Merge(double hostMs) { if (enabled_) mergeMs_ += hostMs; }
    void Transfer(std::size_t bytes) { if (enabled_) metadataBytes_ += bytes; }
    void Tensor(int batch, std::size_t inputBytes, std::size_t outputBytes, std::size_t outputLimitBytes) {
        if (!enabled_) return;
        maximumBatch_ = std::max(maximumBatch_, batch);
        inputBytes_ = std::max(inputBytes_, inputBytes);
        outputBytes_ = std::max(outputBytes_, outputBytes);
        outputLimitBytes_ = std::max(outputLimitBytes_, outputLimitBytes);
    }
    // Call only after the exact inference stream has completed successfully.
    void Emit(int batchCapacity) {
        if (!enabled_) return;
        std::array<double, StageCount> stageMs{};
        for (std::size_t i = 0; i < used_; ++i) {
            float ms{};
            Check(cudaEventElapsedTime(&ms, events_[i].begin, events_[i].end));
            stageMs[events_[i].stage] += ms;
        }
        QJsonObject gpu;
        const char* names[]{"normalize", "detector", "regions", "rectify", "recognizer", "ctc", "metadataCopies"};
        for (int i = 0; i < StageCount; ++i) gpu[names[i]] = stageMs[i];
        std::size_t freeAfter{}, totalAfter{};
        Check(cudaMemGetInfo(&freeAfter, &totalAfter));
        // Stream event intervals include launch gaps and external GPU contention;
        // these are completion intervals, not an isolated sum of kernel runtimes.
        const QJsonObject report{{"batchCapacity", batchCapacity},
            {"workerWallMs", wall_.nsecsElapsed() / 1e6}, {"streamStageMs", gpu},
            {"detectorPasses", detectorPasses_}, {"recognizerCalls", recognizerCalls_},
            {"recognizerSamples", recognizerSamples_}, {"networkHostMs", enqueueMs_},
            {"mergeHostMs", mergeMs_}, {"shapeSets", shapeSets_}, {"addressSets", addressSets_},
            {"metadataBytes", static_cast<qint64>(metadataBytes_)}, {"sourceUploadMeasured", false},
            {"maximumActualBatch", maximumBatch_},
            {"recognizerInputCapacityBytes", static_cast<qint64>(inputBytes_)},
            {"recognizerOutputCapacityBytes", static_cast<qint64>(outputBytes_)},
            {"recognizerOutputAllocationCeilingBytes", static_cast<qint64>(outputLimitBytes_)},
            {"cudaFreeBytesBefore", static_cast<qint64>(freeBefore_)},
            {"cudaFreeBytesAfter", static_cast<qint64>(freeAfter)},
            {"cudaFreeBytesDeltaApprox", static_cast<qint64>(freeBefore_)-static_cast<qint64>(freeAfter)},
            {"cudaDeviceTotalBytes", static_cast<qint64>(totalBytes_)},
            {"memoryDeltaIsNotPeakOrExclusiveOwned", true},
            {"eventPairs", static_cast<int>(used_)}, {"omittedEventPairs", omitted_}};
        qInfo().noquote() << "UCT_TRT_PROFILE" << QJsonDocument(report).toJson(QJsonDocument::Compact);
    }
private:
    static void Check(cudaError_t status) {
        if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
    }
    struct EventPair { cudaEvent_t begin{}, end{}; Stage stage{}; };
    static constexpr std::size_t kMaximumPairs = 2048;
    bool enabled_{};
    std::vector<EventPair> events_;
    std::size_t used_{};
    int omitted_{}, detectorPasses_{}, recognizerCalls_{}, recognizerSamples_{};
    int shapeSets_{}, addressSets_{};
    double enqueueMs_{}, mergeMs_{};
    std::size_t metadataBytes_{};
    std::size_t inputBytes_{}, outputBytes_{}, outputLimitBytes_{}, freeBefore_{}, totalBytes_{};
    int maximumBatch_{};
    QElapsedTimer wall_;
};

} // namespace okuflow::uct
