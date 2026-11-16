#pragma once

// Private, explicitly enabled OCR worker instrumentation. Event resources live
// with the inference state, including its unknown-completion quarantine.
#include <QDebug>
#include "okuflow/common/uct_ribbon.hpp"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRect>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
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
        if (enabled_) {
            events_.reserve(kMaximumPairs);
            cropHash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
        }
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
        rectifyLaunches_ = ctcLaunches_ = recognitionCopies_ = 0;
        ribbonCrops_ = unsupportedRibbonCrops_ = 0;
        ribbonOutcomes_ = {}; ribbonBows_ = {}; ribbonWords_ = {}; ribbonBoxes_ = {};
        metadataBytes_ = 0;
        inputBytes_ = outputBytes_ = outputLimitBytes_ = 0;
        maximumBatch_ = 0;
        enqueueMs_ = mergeMs_ = 0;
        detectorWaitMs_ = groupWaitMs_ = 0;
        reconcile_ = {}; reconciled_ = false;
        graph_ = {}; graphUsed_ = false; graphReceipt_ = {};
        widthBins_ = {}; canvasBins_ = {}; invalidWidths_ = oversizedWidths_ = 0;
        requestedWidth_ = usedWidth_ = canvasWidth_ = 0;
        if (enabled_) {
            cropHash_->reset();
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
    // Private graph-replay experiment: how one network call ran (0 plain,
    // 1 captured then launched, 2 replayed) and what it cost on the host.
    void GraphCall(int launch, bool created, double contextMs, double captureMs, bool drained, bool evicted) {
        if (!enabled_) return;
        graphUsed_ = true;
        ++graph_.launches[std::clamp(launch, 0, 2)];
        graph_.contexts += created; graph_.drains += drained; graph_.evictions += evicted;
        graph_.contextMs += contextMs; graph_.captureMs += captureMs;
    }
    void GraphReceipt(QJsonObject receipt) { if (enabled_) graphReceipt_ = std::move(receipt); }
    void Merge(double hostMs) { if (enabled_) mergeMs_ += hostMs; }
    // Private region-reconciliation experiment: detections and why some were not recognized.
    void Reconcile(int detections, int clipped, int whole, int tile, int restored, bool wellFormed, double hostMs) {
        if (enabled_) {
            reconcile_ = {detections, clipped, whole, tile, restored, wellFormed ? 1 : 0};
            reconcileMs_ = hostMs; reconciled_ = true;
        }
    }
    template<class Operation> void Wait(bool detectorCount, Operation operation) {
        if (!enabled_) { operation(); return; }
        QElapsedTimer timer;timer.start();
        operation();
        const double ms = timer.nsecsElapsed() / 1e6;
        (detectorCount ? detectorWaitMs_ : groupWaitMs_) += ms;
    }
    void CropWidth(int requested, int canvas) {
        if (!enabled_) return;
        if (requested <= 0 || canvas <= 0) { ++invalidWidths_; return; }
        requestedWidth_ += requested;
        usedWidth_ += std::min(requested, canvas);
        canvasWidth_ += canvas;
        if (canvas == 128) ++canvasBins_[0];
        else if (canvas == 256) ++canvasBins_[1];
        else if (canvas == 512) ++canvasBins_[2];
        else if (canvas == 1024) ++canvasBins_[3];
        const auto bin = std::lower_bound(kWidthBounds.begin(), kWidthBounds.end(), requested);
        if (bin == kWidthBounds.end()) ++oversizedWidths_;
        else ++widthBins_[bin - kWidthBounds.begin()];
    }
    // Hash only descriptors already copied for publication, never image bytes.
    // Explicit LE words avoid C++ padding/ABI-dependent comparison evidence.
    void CropDescriptor(const float* xy, float score, int requested, int edges,
                        const QRect& region, int canvas, const ribbon::Geometry* curve = nullptr) {
        if (!enabled_) return;
        CropWidth(requested, canvas);
        std::array<quint32, 16> words{};
        words[0] = 1; // descriptor encoding version
        words[1] = static_cast<quint32>(region.x());
        words[2] = static_cast<quint32>(region.y());
        words[3] = static_cast<quint32>(region.width());
        words[4] = static_cast<quint32>(region.height());
        for (int i = 0; i < 8; ++i) std::memcpy(&words[5+i], xy+i, sizeof(float));
        std::memcpy(&words[13], &score, sizeof(float));
        words[14] = static_cast<quint32>(requested);
        words[15] = static_cast<quint32>(edges);
        for (auto& word : words) word = qToLittleEndian(word);
        cropHash_->addData(QByteArrayView(reinterpret_cast<const char*>(words.data()), sizeof(words)));
        if(curve && (curve->valid || curve->unsupported)) {
            ribbonCrops_ += curve->valid;
            unsupportedRibbonCrops_ += curve->unsupported;
            const double fields[]{curve->ox,curve->oy,curve->ux,curve->uy,curve->vx,curve->vy,
                curve->a,curve->b,curve->c,curve->h0,curve->h1,curve->start,curve->end,
                curve->protectLeft,curve->protectTop,curve->protectRight,curve->protectBottom};
            cropHash_->addData(QByteArrayView("ribbon-v1",9));
            const quint32 status=qToLittleEndian(quint32(curve->valid?1:2));
            cropHash_->addData(QByteArrayView(reinterpret_cast<const char*>(&status),sizeof(status)));
            for(double value:fields) {
                quint64 bits{};std::memcpy(&bits,&value,sizeof(bits));bits=qToLittleEndian(bits);
                cropHash_->addData(QByteArrayView(reinterpret_cast<const char*>(&bits),sizeof(bits)));
            }
        }

    }
    // Ribbon fit outcome of each recognized crop and the words its decode
    // produced, split by detections (a detection's own crop, internalEdges 0)
    // and all crops (detections plus their overlapping sub-windows). Bounded
    // counters; nothing is kept per crop. `confident` counts words at the
    // ordinary 0.7 confidence whatever their disposition, `invalid` those the
    // decode itself marked invalid geometry (before any merge inheritance).
    // Refused detections also record their protection box, and detections a
    // StraightRule moved to the rectangle path the same box of their quad, in
    // frame pixels (bounded): today's boxes can then be matched per page.
    void RibbonCrop(const ribbon::Geometry& curve, int edges, int words, int confident, int invalid,
                    const float* xy = nullptr, QPoint origin = {}) {
        if (!enabled_) return;
        if (edges == 0 && xy && (curve.unsupported || curve.outcome == ribbon::kSpanStraight) &&
            ribbonBoxes_.size() < kMaximumRibbonBoxes) {
            double left = xy[0], top = xy[1], right = xy[0], bottom = xy[1];
            for (int i = 1; i < 4; ++i) {
                left = std::min(left, double(xy[2*i])); right = std::max(right, double(xy[2*i]));
                top = std::min(top, double(xy[2*i+1])); bottom = std::max(bottom, double(xy[2*i+1]));
            }
            if (curve.unsupported) { left = curve.protectLeft; top = curve.protectTop;
                                     right = curve.protectRight; bottom = curve.protectBottom; }
            ribbonBoxes_.push_back(QJsonArray{curve.unsupported ? 1 : 0, left + origin.x(), top + origin.y(),
                                              right + origin.x(), bottom + origin.y()});
        }
        const int outcome = curve.outcome < ribbon::kFitOutcomes ? curve.outcome : 0;
        const int scope = edges == 0 ? 0 : 1;
        ++ribbonOutcomes_[scope][outcome];
        auto& counts = ribbonWords_[std::size_t(scope * ribbon::kFitOutcomes + outcome)];
        counts[0] += words; counts[1] += confident; counts[2] += invalid;
        if (scope == 0 && outcome != ribbon::kNotTried) {
            const int kind = outcome == ribbon::kStraight ? 0 : outcome == ribbon::kSpanStraight ? 1
                           : outcome == ribbon::kFitted ? 2 : 3;
            const auto bin = std::upper_bound(kBowBounds.begin(), kBowBounds.end(), curve.bow);
            ++ribbonBows_[std::size_t(kind)][std::size_t(bin - kBowBounds.begin())];
        }
    }
    void Transfer(std::size_t bytes) { if (enabled_) metadataBytes_ += bytes; }
    void RecognitionHelpers(int rectify, int ctc, int copies) {
        if (!enabled_) return;
        rectifyLaunches_ += rectify; ctcLaunches_ += ctc; recognitionCopies_ += copies;
    }
    void Tensor(int batch, std::size_t inputBytes, std::size_t outputBytes, std::size_t outputLimitBytes) {
        if (!enabled_) return;
        maximumBatch_ = std::max(maximumBatch_, batch);
        inputBytes_ = std::max(inputBytes_, inputBytes);
        outputBytes_ = std::max(outputBytes_, outputBytes);
        outputLimitBytes_ = std::max(outputLimitBytes_, outputLimitBytes);
    }
    // Call only after the exact inference stream has completed successfully.
    void Emit(int batchCapacity, bool variableWidth = false) {
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
        QJsonArray widthBounds, widthCounts;
        for (int i = 0; i < kWidthBounds.size(); ++i) {
            widthBounds.push_back(kWidthBounds[i]);
            widthCounts.push_back(widthBins_[i]);
        }
        const QJsonObject widths{{"inclusiveUpperBounds", widthBounds}, {"counts", widthCounts},
            {"above1024", oversizedWidths_}, {"invalid", invalidWidths_},
            {"requestedWidthSum", static_cast<qint64>(requestedWidth_)},
            {"usedWidthSum", static_cast<qint64>(usedWidth_)},
            {"canvasWidthSum", static_cast<qint64>(canvasWidth_)},
            {"paddedWidthFraction", canvasWidth_ ? 1.0 - double(usedWidth_) / canvasWidth_ : 0.0}};
        std::size_t freeAfter{}, totalAfter{};
        Check(cudaMemGetInfo(&freeAfter, &totalAfter));
        // Stream event intervals include launch gaps and external GPU contention;
        // these are completion intervals, not an isolated sum of kernel runtimes.
        QJsonArray canvases;
        for (const auto n : canvasBins_) canvases.push_back(n);
        const QJsonObject report{{"batchCapacity", batchCapacity},
            {"recognizerWidthPolicy", variableWidth ? "bucketed" : "fixed1024"},
            {"recognizerCanvasWidths", QJsonArray{128,256,512,1024}}, {"recognizerCanvasCounts", canvases},
            {"workerWallMs", wall_.nsecsElapsed() / 1e6}, {"streamStageMs", gpu},
            {"detectorPasses", detectorPasses_}, {"recognizerCalls", recognizerCalls_},
            {"recognizerSamples", recognizerSamples_}, {"networkHostMs", enqueueMs_},
            {"ribbonRecognitionCrops",ribbonCrops_},{"unsupportedRibbonRecognitionCrops",unsupportedRibbonCrops_},
            {"mergeHostMs", mergeMs_}, {"shapeSets", shapeSets_}, {"addressSets", addressSets_},
            {"detectorCountHostWaitMs", detectorWaitMs_}, {"recognizerGroupHostWaitMs", groupWaitMs_},
            {"cropWidthHistogram", widths},
            {"orderedCropDescriptorSha256", QString::fromLatin1(cropHash_->result().toHex())},
            {"cropDescriptorEncoding", "v1-le32:version,region-xywh,quad-xy8,score,validWidth,internalEdges"},
            {"metadataBytes", static_cast<qint64>(metadataBytes_)}, {"sourceUploadMeasured", false},
            {"recognitionRectifyLaunches", rectifyLaunches_}, {"recognitionCtcLaunches", ctcLaunches_},
            {"recognitionMetadataCopies", recognitionCopies_},
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
        auto complete = report;
        {
            const char* outcomes[]{"notTried","straight","fitted","steep","residual","degenerate","height","outside",
                                   "spanStraight"};
            static_assert(std::size(outcomes) == ribbon::kFitOutcomes);
            QJsonObject ribbon;
            const char* scopes[]{"detections","windows"};
            for (int scope = 0; scope < 2; ++scope) {
                QJsonObject byOutcome;
                for (int o = 0; o < ribbon::kFitOutcomes; ++o) {
                    const auto& words = ribbonWords_[std::size_t(scope * ribbon::kFitOutcomes + o)];
                    if (!ribbonOutcomes_[scope][o]) continue;
                    byOutcome[outcomes[o]] = QJsonArray{ribbonOutcomes_[scope][o], words[0], words[1], words[2]};
                }
                ribbon[scopes[scope]] = byOutcome;
            }
            ribbon["encoding"] = "outcome: [crops, words, words with confidence >= 0.7, words marked invalid geometry by the decode]";
            QJsonArray bounds;
            for (const float b : kBowBounds) bounds.push_back(b);
            ribbon["bowUpperBounds"] = bounds;
            const char* kinds[]{"straight","spanStraight","fitted","refused"};
            for (int k = 0; k < 4; ++k) {
                QJsonArray bins;
                for (const int n : ribbonBows_[std::size_t(k)]) bins.push_back(n);
                ribbon[QString("bow_") + kinds[k]] = bins;
            }
            ribbon["boxes"] = ribbonBoxes_;
            ribbon["boxesEncoding"] = "[refused 1 (protection box) or moved by a straight rule 0 (quad box), left, top, right, bottom], frame pixels";
            complete["ribbonFit"] = ribbon;
        }
        if (reconciled_)
            complete["regionReconcile"] = QJsonObject{{"detections", reconcile_[0]}, {"clippedDropped", reconcile_[1]},
                {"wholeDropped", reconcile_[2]}, {"tileDropped", reconcile_[3]}, {"restored", reconcile_[4]},
                {"windowLayoutRecognized", reconcile_[5] != 0}, {"hostMs", reconcileMs_}};
        if (graphUsed_) {
            auto graph = graphReceipt_;
            graph["read"] = QJsonObject{{"plain", graph_.launches[0]}, {"captures", graph_.launches[1]},
                {"replays", graph_.launches[2]}, {"slotsOpened", graph_.contexts},
                {"contextCreateMs", graph_.contextMs}, {"captureMs", graph_.captureMs},
                {"drains", graph_.drains}, {"evictions", graph_.evictions}};
            complete["graphReplay"] = graph;
        }
        qInfo().noquote() << "UCT_TRT_PROFILE" << QJsonDocument(complete).toJson(QJsonDocument::Compact);
    }
private:
    static void Check(cudaError_t status) {
        if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
    }
    struct EventPair { cudaEvent_t begin{}, end{}; Stage stage{}; };
    static constexpr std::size_t kMaximumPairs = 2048;
    static constexpr std::array<int, 7> kWidthBounds{64, 128, 256, 384, 512, 768, 1024};
    std::array<int, 7> widthBins_{};
    std::array<int, 4> canvasBins_{};
    int invalidWidths_{}, oversizedWidths_{};
    std::uint64_t requestedWidth_{}, usedWidth_{}, canvasWidth_{};
    std::unique_ptr<QCryptographicHash> cropHash_;
    double detectorWaitMs_{}, groupWaitMs_{};
    std::array<int, 6> reconcile_{};
    struct GraphCounts { std::array<int, 3> launches{}; int contexts{}, drains{}, evictions{}; double contextMs{}, captureMs{}; };
    GraphCounts graph_{};
    bool graphUsed_{};
    QJsonObject graphReceipt_;
    double reconcileMs_{};
    bool reconciled_{};
    bool enabled_{};
    std::vector<EventPair> events_;
    std::size_t used_{};
    int omitted_{}, detectorPasses_{}, recognizerCalls_{}, recognizerSamples_{};
    int shapeSets_{}, addressSets_{};
    int rectifyLaunches_{}, ctcLaunches_{}, recognitionCopies_{};
    int ribbonCrops_{}, unsupportedRibbonCrops_{};
    // Fitted sagitta over mean height of detections whose fit reached the
    // curvature test; the last bin is open.
    static constexpr std::array<float, 9> kBowBounds{.04f, .08f, .12f, .16f, .24f, .32f, .48f, .64f, 1.f};
    std::array<std::array<int, ribbon::kFitOutcomes>, 2> ribbonOutcomes_{};
    std::array<std::array<int, kBowBounds.size() + 1>, 4> ribbonBows_{};
    std::array<std::array<int, 3>, 2 * ribbon::kFitOutcomes> ribbonWords_{};
    static constexpr qsizetype kMaximumRibbonBoxes = 512;
    QJsonArray ribbonBoxes_;
    double enqueueMs_{}, mergeMs_{};
    std::size_t metadataBytes_{};
    std::size_t inputBytes_{}, outputBytes_{}, outputLimitBytes_{}, freeBefore_{}, totalBytes_{};
    int maximumBatch_{};
    QElapsedTimer wall_;
};

} // namespace okuflow::uct
