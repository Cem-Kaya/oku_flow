#pragma once
#include "okuflow/common/async_cleanup.hpp"
#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/cuda/uct_kernels.hpp"
#include "../common/uct_surface_geometry.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cuda_runtime.h>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#endif

// Private implementation shared only with uct_analysis_pool.cpp. No image or
// feature array crosses to the host: just colors, boxes, scores and digests.
namespace okuflow::uct::resident
{
struct Allocation
{
    void *data{};
    int device{};
    bool memoryPools{};
};
using Memory = std::shared_ptr<Allocation>;

// One process-lifetime retirement worker, rather than one thread per line.
// Last owners are released only after a known completion fence. Unknown work
// keeps its owners in the quarantined context and never reaches this queue.
class RetirementQueue
{
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Allocation *> queue_;
    std::map<int, cudaStream_t> streams_;
    std::vector<Allocation *> retained_;
    RetirementQueue()
    {
        std::thread([this] { Run(); }).detach();
    }
    void Run()
    {
        for (;;)
        {
            Allocation *value;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return !queue_.empty(); });
                value = queue_.front();
                queue_.pop_front();
            }
            bool success = cudaSetDevice(value->device) == cudaSuccess;
            if (success && value->data)
            {
                if (value->memoryPools)
                {
                    auto &stream = streams_[value->device];
                    if (!stream)
                        success = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
                                  cudaSuccess;
                    if (success)
                        success = cudaFreeAsync(value->data, stream) == cudaSuccess;
                }
                else
                    success = cudaFree(value->data) == cudaSuccess;
            }
            if (success)
                delete value;
            else
                retained_.push_back(value); // do not retry/free an uncertain pointer
        }
    }

  public:
    static void Retire(Allocation *value) noexcept
    {
        try
        {
            static auto *queue = new RetirementQueue;
            {
                std::lock_guard lock(queue->mutex_);
                queue->queue_.push_back(value);
            }
            queue->wake_.notify_one();
        }
        catch (...)
        {
            // Allocation/thread exhaustion must not terminate the caller while
            // releasing a shared descriptor. Retain this allocation until exit.
        }
    }
};
struct Context
{
    cudaStream_t stream{};
    cudaEvent_t ready{};
    void *scratch{};
    void *host{};
    size_t capacity{};
    int device{};
    bool memoryPools{};
#if defined(_WIN32)
    HANDLE pauseTimer{};
#endif
    std::vector<std::shared_ptr<const void>> owners;
    void Pause()
    {
#if defined(_WIN32)
        // No process-wide timer-resolution change. Coarse Sleep(1) can add a
        // scheduler tick per line; this private one-shot wait stays responsive.
        if (pauseTimer)
        {
            LARGE_INTEGER due{};
            due.QuadPart = -1000; // request 100 microseconds
            if (SetWaitableTimer(pauseTimer, &due, 0, nullptr, nullptr, FALSE))
            {
                WaitForSingleObject(pauseTimer, 5);
                return;
            }
        }
#endif
        std::this_thread::yield();
    }
    ~Context()
    {
#if defined(_WIN32)
        if (pauseTimer)
            CloseHandle(pauseTimer);
#endif
        cudaSetDevice(device);
        if (scratch)
        {
            if (memoryPools)
                cudaFreeAsync(scratch, stream);
            else
                cudaFree(scratch);
        }
        if (host)
            cudaFreeHost(host);
        if (ready)
            cudaEventDestroy(ready);
        if (stream)
            cudaStreamDestroy(stream);
    }
};
inline thread_local std::shared_ptr<Context> cachedContext;
inline void Quarantine(const std::shared_ptr<Context> &c)
{
    if (cachedContext == c)
        cachedContext.reset();
    AsyncCleanup::Start([c] {
        return cudaSetDevice(c->device) == cudaSuccess &&
               (!c->stream || cudaStreamSynchronize(c->stream) == cudaSuccess);
    });
}
inline std::shared_ptr<Context> Worker(int device, size_t bytes)
{
    if (cudaSetDevice(device) != cudaSuccess)
        return {};
    if (cachedContext && (cachedContext->device != device || cachedContext->capacity < bytes))
    {
        auto old = std::move(cachedContext);
        AsyncCleanup::Start([old] {
            return cudaSetDevice(old->device) == cudaSuccess &&
                   cudaStreamSynchronize(old->stream) == cudaSuccess;
        });
    }
    if (!cachedContext)
    {
        auto c = std::make_shared<Context>();
        c->device = device;
        c->capacity = std::max(size_t(256 * 1024), bytes);
        int supported{};
        c->memoryPools = cudaDeviceGetAttribute(&supported, cudaDevAttrMemoryPoolsSupported,
                                                device) == cudaSuccess &&
                         supported;
        int least{}, greatest{};
        cudaDeviceGetStreamPriorityRange(&least, &greatest);
        if (cudaStreamCreateWithPriority(&c->stream, cudaStreamNonBlocking, least) != cudaSuccess ||
            cudaEventCreateWithFlags(&c->ready, cudaEventDisableTiming) != cudaSuccess ||
            (c->memoryPools ? cudaMallocAsync(&c->scratch, c->capacity, c->stream)
                            : cudaMalloc(&c->scratch, c->capacity)) != cudaSuccess ||
            cudaHostAlloc(&c->host, c->capacity, cudaHostAllocDefault) != cudaSuccess)
        {
            Quarantine(c);
            return {};
        }
#if defined(_WIN32)
        c->pauseTimer =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_MODIFY_STATE | SYNCHRONIZE);
#endif
        cachedContext = c;
    }
    return cachedContext;
}
inline Memory Allocate(size_t bytes, const std::shared_ptr<Context> &c)
{
    auto p = Memory(new Allocation{nullptr, c->device, c->memoryPools}, RetirementQueue::Retire);
    const auto status =
        c->memoryPools ? cudaMallocAsync(&p->data, bytes, c->stream) : cudaMalloc(&p->data, bytes);
    if (status != cudaSuccess)
        return {};
    return p;
}
inline bool Finish(const std::shared_ptr<Context> &c, const std::atomic<bool> *cancelled = nullptr)
{
    if (cudaGetLastError() != cudaSuccess || cudaEventRecord(c->ready, c->stream) != cudaSuccess)
    {
        Quarantine(c);
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int polls = 0;
    for (;;)
    {
        const auto status = cudaEventQuery(c->ready);
        if (status == cudaSuccess)
        {
            c->owners.clear();
            return !cancelled || !cancelled->load();
        }
        if (status != cudaErrorNotReady || (cancelled && cancelled->load()) ||
            std::chrono::steady_clock::now() > deadline)
        {
            Quarantine(c);
            return false;
        }
        if (polls++ < 8)
            std::this_thread::yield();
        else
            c->Pause();
    }
}
inline bool CopyResult(const std::shared_ptr<Context> &c, const void *from, size_t bytes,
                       const std::atomic<bool> *cancel = nullptr)
{
    if (cudaMemcpyAsync(c->host, from, bytes, cudaMemcpyDeviceToHost, c->stream) != cudaSuccess)
    {
        Quarantine(c);
        return false;
    }
    return Finish(c, cancel);
}
inline bool Upload(const std::shared_ptr<Context> &c, const void *from, size_t bytes,
                   size_t offset = 0)
{
    if (!bytes)
        return true;
    // Caller vectors remain live until Finish. Pinned staging prevents pageable
    // host uploads from silently serializing the legacy/default CUDA stream.
    std::memcpy(static_cast<char *>(c->host) + offset, from, bytes);
    if (cudaMemcpyAsync(static_cast<char *>(c->scratch) + offset,
                        static_cast<char *>(c->host) + offset, bytes, cudaMemcpyHostToDevice,
                        c->stream) != cudaSuccess)
    {
        Quarantine(c);
        return false;
    }
    return true;
}
inline gpu::ProbeMap Mapping(const SourceStamp &stamp, int w, int h)
{
    const auto &t = stamp.viewTransform;
    const double sx = static_cast<double>(t.destinationWidth) * w / t.sourceWidth,
                 sy = static_cast<double>(t.destinationHeight) * h / t.sourceHeight;
    return {static_cast<double>(t.destinationX) * w - t.sourceX * sx,
            static_cast<double>(t.destinationY) * h - t.sourceY * sy, sx, sy};
}
inline QRectF ViewArea(const SourceStamp &stamp, int w, int h, double border = 0)
{
    const auto &t = stamp.viewTransform;
    const double x = t.sourceX + (border / w - t.destinationX) / t.destinationWidth * t.sourceWidth;
    const double y =
        t.sourceY + (border / h - t.destinationY) / t.destinationHeight * t.sourceHeight;
    const double right =
        t.sourceX + ((w - border) / w - t.destinationX) / t.destinationWidth * t.sourceWidth;
    const double bottom =
        t.sourceY + ((h - border) / h - t.destinationY) / t.destinationHeight * t.sourceHeight;
    return {x, y, right - x, bottom - y};
}
inline gpu::Grid Project(const SourceStamp &stamp, int w, int h, const ReferencePatch &p)
{
    if(p.sceneQuad.valid) {
        const auto quad=SceneQuadToView(stamp.viewTransform,w,h,p.sceneQuad);
        const auto u=(quad.points[1]-quad.points[0])/p.columns;
        const auto v=(quad.points[3]-quad.points[0])/p.rows;
        const auto warp=(quad.points[2]-quad.points[1]-quad.points[3]+quad.points[0])/(p.columns*p.rows);
        const auto origin=quad.points[0]+u*.5+v*.5+warp*.25;
        const double cw=std::min(QLineF(quad.points[0],quad.points[1]).length(),
                                 QLineF(quad.points[3],quad.points[2]).length())/p.columns;
        const double ch=std::min(QLineF(quad.points[0],quad.points[3]).length(),
                                 QLineF(quad.points[1],quad.points[2]).length())/p.rows;
        const double half=.5*std::min(cw,ch);
        return {origin.x(),origin.y(),u.x()+warp.x()*.5,v.y()+warp.y()*.5,half,half,
                p.columns,p.rows,v.x()+warp.x()*.5,u.y()+warp.y()*.5,warp.x(),warp.y()};
    }
    const auto m = Mapping(stamp, w, h);
    const double dx = p.sceneRect.width() / p.columns * m.sx,
                 dy = p.sceneRect.height() / p.rows * m.sy;
    return {p.sceneRect.x() * m.sx + m.x + dx * .5,
            p.sceneRect.y() * m.sy + m.y + dy * .5,
            dx,
            dy,
            dx * .5,
            dy * .5,
            p.columns,
            p.rows};
}
struct Patch final : ResidentPatch
{
    Memory samples;
    size_t offset{};
    int device{};
};
class Probe final : public ResidentProbe
{
  public:
    Memory integral;
    int width{}, height{}, device{};
    mutable std::atomic<bool> available{true};
    int Width() const noexcept override
    {
        return width;
    }
    int Height() const noexcept override
    {
        return height;
    }
    ReferencePatch Reference(const SourceStamp &stamp, const QRectF &rect) const override
    {
        const auto out = References(stamp, {rect});
        return out.isEmpty() ? ReferencePatch{} : out.front();
    }
    QVector<ReferencePatch> References(const SourceStamp &stamp,
                                       const QVector<QRectF> &rects) const override
    {
        QVector<ReferencePatch> out(rects.size());
        if (!available.load() || rects.isEmpty() || rects.size() > 1024)
            return out;
        const auto area = ViewArea(stamp, width, height, 1);
        const auto m = Mapping(stamp, width, height);
        for (int i = 0; i < rects.size(); ++i)
        {
            const auto rect = rects[i];
            if (rect.isEmpty() || !area.contains(rect))
                continue;
            auto &p = out[i];
            p.sceneRect = rect.adjusted(-rect.height() * .15, -rect.height() * .3,
                                        rect.height() * .15, rect.height() * .3)
                              .intersected(area);
            p.columns = std::clamp(int(std::lround(p.sceneRect.width() * m.sx)), 8, 64);
            p.rows = std::clamp(int(std::lround(p.sceneRect.height() * m.sy)), 4, 12);
        }
        return CaptureReferences(stamp,std::move(out));
    }
    QVector<ReferencePatch> QuadReferences(const SourceStamp& stamp,const QVector<TextQuad>& quads) const override
    {
        QVector<ReferencePatch> out;out.reserve(quads.size());
        if(!available.load() || quads.isEmpty() || quads.size()>1024) return QVector<ReferencePatch>(quads.size());
        for(const auto& quad:quads) out.push_back(PlanQuadReference(stamp,width,height,quad));
        return CaptureReferences(stamp,std::move(out));
    }
    QVector<ReferencePatch> CaptureReferences(const SourceStamp& stamp,QVector<ReferencePatch> out) const
    {
        QVector<gpu::ReferenceRequest> requests;
        QVector<int> indices;
        for(int i=0;i<out.size();++i) {
            if(out[i].columns<1 || out[i].rows<1) continue;
            indices.push_back(i);
            requests.push_back({Project(stamp,width,height,out[i]),nullptr});
        }
        if (requests.isEmpty())
            return out;
        const size_t resultBytes = requests.size() * sizeof(int),
                     requestAt = (resultBytes + 7) & ~size_t(7);
        auto c = Worker(device, requestAt + requests.size() * sizeof(gpu::ReferenceRequest));
        if (!c)
        {
            available = false;
            return {};
        }
        constexpr size_t stride = 64 * 12 * sizeof(float);
        auto samples = Allocate(requests.size() * stride, c);
        if (!samples)
        {
            available = false;
            Quarantine(c);
            return {};
        }
        c->owners = {integral, samples};
        for (int i = 0; i < requests.size(); ++i)
        {
            auto patch = std::make_shared<Patch>();
            patch->device = device;
            patch->samples = samples;
            patch->offset = i * stride;
            requests[i].samples =
                reinterpret_cast<float *>(static_cast<char *>(samples->data) + patch->offset);
            out[indices[i]].resident = patch;
        }
        if (!Upload(c, requests.constData(), requests.size() * sizeof(gpu::ReferenceRequest),
                    requestAt))
        {
            available = false;
            return {};
        }
        gpu::References(static_cast<unsigned *>(integral->data), width, height,
                        reinterpret_cast<const gpu::ReferenceRequest *>(
                            static_cast<char *>(c->scratch) + requestAt),
                        requests.size(), static_cast<int *>(c->scratch), c->stream);
        if (!CopyResult(c, c->scratch, resultBytes))
        {
            available = false;
            return {};
        }
        const auto *valid = static_cast<int *>(c->host);
        for (int i = 0; i < requests.size(); ++i)
        {
            auto &p = out[indices[i]];
            p.valid = valid[i] != 0;
            if (!p.valid)
                p.resident.reset();
        }
        return out;
    }
    PatchMatch Match(const SourceStamp &stamp, const ReferencePatch &patch,
                     const MatchTuning &tuning, std::optional<QPointF> seed) const override
    {
        const auto out = Matches(stamp, {patch}, tuning, {seed.value_or(QPointF())});
        return out.isEmpty() ? PatchMatch{} : out.front();
    }
    QVector<PatchMatch> Matches(const SourceStamp &stamp, const QVector<ReferencePatch> &patches,
                                const MatchTuning &tuning, const QVector<QPointF> &seeds,
                                const std::atomic<bool> *cancelled = nullptr) const override
    {
        QVector<PatchMatch> out(patches.size());
        if (!available.load() || patches.isEmpty() || patches.size() > 1024 ||
            !std::isfinite(tuning.minimumScore) || !std::isfinite(tuning.minimumVisibleFraction) ||
            (cancelled && cancelled->load()))
            return {};
        const size_t resultBytes = patches.size() * sizeof(gpu::MatchResult),
                     requestAt = (resultBytes + 7) & ~size_t(7),
                     sampleAt = requestAt + patches.size() * sizeof(gpu::MatchRequest);
        auto c = Worker(device, sampleAt + patches.size() * 64 * 12 * sizeof(float));
        if (!c)
        {
            available = false;
            return {};
        }
        c->owners = {integral};
        QVector<gpu::MatchRequest> requests;
        QVector<int> indices;
        QVector<float> cpuSamples;
        const auto m = Mapping(stamp, width, height);
        for (int i = 0; i < patches.size(); ++i)
        {
            const auto &p = patches[i];
            if (!p.valid || p.columns < 1 || p.columns > 64 || p.rows < 1 || p.rows > 12 ||
                (p.sceneQuad.valid && QuadArea(p.sceneQuad)<=0))
                continue;
            const float *samples{};
            int cpuOffset = -1;
            if (p.resident)
            {
                const auto patch = std::dynamic_pointer_cast<const Patch>(p.resident);
                if (!patch || patch->device != device)
                    continue;
                c->owners.push_back(patch->samples);
                samples = reinterpret_cast<const float *>(
                    static_cast<char *>(patch->samples->data) + patch->offset);
            }
            else
            {
                if (p.values.size() != p.columns * p.rows)
                    continue;
                cpuOffset = cpuSamples.size();
                cpuSamples += p.values;
                samples =
                    reinterpret_cast<const float *>(static_cast<char *>(c->scratch) + sampleAt) +
                    cpuOffset;
            }
            const auto seed = i < seeds.size() ? seeds[i] : QPointF();
            requests.push_back({Project(stamp, width, height, p), samples,
                                std::round(seed.x() * m.sx), std::round(seed.y() * m.sy), 1 / m.sx,
                                1 / m.sy});
            indices.push_back(i);
        }
        if (requests.isEmpty())
        {
            c->owners.clear();
            return out;
        }
        if (!Upload(c, cpuSamples.constData(), cpuSamples.size() * sizeof(float), sampleAt) ||
            !Upload(c, requests.constData(), requests.size() * sizeof(gpu::MatchRequest),
                    requestAt))
        {
            available = false;
            return {};
        }
        gpu::MatchReferences(static_cast<unsigned *>(integral->data), width, height,
                             reinterpret_cast<const gpu::MatchRequest *>(
                                 static_cast<char *>(c->scratch) + requestAt),
                             requests.size(), tuning.minimumScore, tuning.minimumVisibleFraction,
                             tuning.searchRadiusPx, static_cast<gpu::MatchResult *>(c->scratch),
                             c->stream);
        if (!CopyResult(c, c->scratch, requests.size() * sizeof(gpu::MatchResult), cancelled))
        {
            // A revoked consumer is not a failed immutable probe. Unknown work
            // remains quarantined with its owners; a later request gets a new
            // private context and still waits on its own fence.
            if (!cancelled || !cancelled->load())
                available = false;
            return {};
        }
        const auto *matches = static_cast<gpu::MatchResult *>(c->host);
        for (int i = 0; i < requests.size(); ++i)
        {
            const auto r = matches[i];
            out[indices[i]] = {bool(r.matched), bool(r.visible), r.score, {r.x, r.y}};
        }
        return out;
    }
    std::optional<double> Similarity(const SourceStamp &a, const ResidentProbe &other,
                                     const SourceStamp &b) const override
    {
        const auto *p = dynamic_cast<const Probe *>(&other);
        if (!available.load() || !p || !p->available.load() || p->device != device)
            return {};
        const auto va = ViewArea(a, width, height).intersected(QRectF(0, 0, 1, 1));
        const auto common =
            va.intersected(ViewArea(b, p->width, p->height).intersected(QRectF(0, 0, 1, 1)));
        if (common.isEmpty() || va.isEmpty() ||
            common.width() * common.height() < .25 * va.width() * va.height())
            return {};
        auto c = Worker(device, 16);
        if (!c)
        {
            available = false;
            return {};
        }
        c->owners = {integral, p->integral};
        gpu::Similarity(static_cast<unsigned *>(integral->data), width, height,
                        Mapping(a, width, height), static_cast<unsigned *>(p->integral->data),
                        p->width, p->height, Mapping(b, p->width, p->height), common.x(),
                        common.y(), common.width(), common.height(),
                        static_cast<double *>(c->scratch), c->stream);
        if (!CopyResult(c, c->scratch, sizeof(double)))
        {
            available = false;
            return {};
        }
        const double value = *static_cast<double *>(c->host);
        return !std::isfinite(value) || value == 2.
                   ? std::nullopt
                   : std::optional<double>(std::clamp(value, -1., 1.));
    }
};
inline gpu::Rect Rect(const QRectF &rect, int w, int h)
{
    auto r = rect.toAlignedRect().intersected(QRect(0, 0, w, h));
    return {r.x(), r.y(), r.width(), r.height()};
}
inline bool Prepare(const DeviceImage &image, RecognitionResult &result, int decimation,
                    bool probeOnly, const std::atomic<bool> &cancel, QString &error)
{
    if (cancel.load())
        return false;
    // Probes stay bounded even with 8K scenes; full resolution still feeds OCR,
    // color sampling and ink fitting. Never retain the analysis-pool lease.
    decimation = std::max({1, decimation, (std::max(image.width, image.height) + 959) / 960});
    const int w = image.width / decimation, h = image.height / decimation;
    auto c = Worker(image.device, 4 * 1024 * 1024);
    if (!c || w < 8 || h < 8 || !image.WaitReady(c->stream, error) || !image.WaitValidationReady(c->stream, error))
        return false;
    c->owners = {image.shared_from_this()};
    auto probe = std::make_shared<Probe>();
    probe->device = image.device;
    probe->width = w;
    probe->height = h;
    probe->integral = Allocate(size_t(w + 1) * (h + 1) * sizeof(unsigned), c);
    if (!probe->integral)
    {
        // WaitReady already queued a dependency on the full image. Keep that
        // exact image/event alive even when output allocation fails.
        error = "Resident UCT integral allocation failed.";
        Quarantine(c);
        return false;
    }
    c->owners.push_back(probe->integral);
    gpu::ValidationIntegral(image.ValidationData(), image.ValidationPitch(), image.width, image.height, decimation,
                            static_cast<unsigned *>(probe->integral->data), c->stream);
    if (!Finish(c, &cancel))
    {
        error = "Resident UCT probe did not complete.";
        return false;
    }
    result.reference.stamp = result.stamp;
    result.reference.resident = probe;
    if (probeOnly)
        return true;

    // Line/word descriptors, not images, enter the device. Results are tiny
    // QColor/QRectF metadata. Preserve every original protected omission span.
    QVector<gpu::Rect> rects;
    QVector<gpu::OrientedInk> colorFrames;
    QVector<gpu::Ink> words;
    QVector<int> indices;
    // Tilted words fit in their own rotated frame (same policy as the CPU).
    QVector<gpu::OrientedInk> oriented;
    QVector<int> orientedIndices;
    QVector<gpu::SurfaceRequest> surfaces;
    QVector<int> surfaceWordIndices;
    if (result.output.lines.size() > 1024) {
        error = "UCT metadata budget exceeded.";
        return false;
    }
    for (auto& line : result.output.lines) {
        QRectF bounds;
        for (auto& word : line.words) {
            bounds = bounds.isNull() ? word.boundsPx : bounds.united(word.boundsPx);
            if (word.protectedBoundsPx.isEmpty()) word.protectedBoundsPx = word.boundsPx;
        }
        rects.push_back(Rect(bounds, image.width, image.height));
    }
    const int lineCount = rects.size();
    colorFrames.resize(lineCount); // Zero width keeps line rectangle sampling.
    const auto descriptor = [](const TextQuad& quad, double detectedWidth = 0, int detached = 0) {
        const auto origin = quad.points[0];
        const double width = QuadWidth(quad), height = QuadHeight(quad);
        if (QuadArea(quad) <= 0.0 || !(width >= 1) || !(height >= 1)) return gpu::OrientedInk{};
        const auto u = (quad.points[1] - origin) / width;
        const auto v = (quad.points[3] - origin) / height;
        const auto warp = quad.points[2] - quad.points[1] - quad.points[3] + origin;
        return gpu::OrientedInk{origin.x(), origin.y(), u.x(), u.y(), v.x(), v.y(),
            width, height, detectedWidth, detached, warp.x(), warp.y()};
    };
    int wordCount = 0;
    for (auto& line : result.output.lines) for (auto& word : line.words) {
        if (++wordCount > 8192) {
            error = "UCT metadata budget exceeded.";
            return false;
        }
        // Preserve logical line references, but sample each word's own surface.
        // A selected button can have the opposite polarity to its neighbours.
        const int colorIndex = rects.size();
        rects.push_back(Rect(word.boundsPx, image.width, image.height));
        colorFrames.push_back(QuadIsRotated(word.quadPx) ? descriptor(word.quadPx) : gpu::OrientedInk{});
        if (!word.paddedBounds || word.disposition != TokenDisposition::kCandidate) continue;
        bool marks = word.text.contains(QLatin1Char('i'), Qt::CaseInsensitive) ||
                     word.text.contains(QLatin1Char('j'), Qt::CaseInsensitive);
        for (auto ch : word.text.normalized(QString::NormalizationForm_D))
            marks = marks || ch.isMark() || QStringLiteral("!?:;").contains(ch);
        if (QuadIsRotated(word.quadPx)) {
            const TextQuad search = word.inkSearchQuadPx.valid ? word.inkSearchQuadPx : word.quadPx;
            if(surfaces.size()<surface_geometry::WordLimit){
                surfaces.push_back({descriptor(search),colorIndex,int(oriented.size()),1});
                surfaceWordIndices.push_back(wordCount-1);
            }
            oriented.push_back(descriptor(search, QuadWidth(word.quadPx), int(marks)));
            orientedIndices.push_back(colorIndex);
            continue;
        }
        const auto r = word.boundsPx;
        if(surfaces.size()<surface_geometry::WordLimit){
            surfaces.push_back({descriptor(TextQuad::FromRect(r)),colorIndex,int(words.size()),0});
            surfaceWordIndices.push_back(wordCount-1);
        }
        words.push_back(
            {Rect(word.inkSearchPx.isEmpty() ? r : word.inkSearchPx, image.width, image.height),
             r.x(), r.y(), r.width(), r.height(), int(marks)});
        indices.push_back(colorIndex);
    }
    const size_t colorsAt = 0, rectsAt = rects.size() * sizeof(gpu::Colors),
                 wordsAt = rectsAt + rects.size() * sizeof(gpu::Rect),
                 indicesAt = wordsAt + words.size() * sizeof(gpu::Ink),
                 fitsAt = (indicesAt + indices.size() * sizeof(int) + 7) & ~size_t(7),
                 orientedAt = fitsAt + words.size() * sizeof(gpu::Fit),
                 orientedIndicesAt = orientedAt + oriented.size() * sizeof(gpu::OrientedInk),
                 orientedFitsAt = (orientedIndicesAt + orientedIndices.size() * sizeof(int) + 7) & ~size_t(7),
                 colorFramesAt = orientedFitsAt + oriented.size() * sizeof(gpu::OrientedFit);
    const size_t surfacesAt=colorFramesAt+colorFrames.size()*sizeof(gpu::OrientedInk),
        surfaceFitsAt=(surfacesAt+surfaces.size()*sizeof(gpu::SurfaceRequest)+7)&~size_t(7);
    if (surfaceFitsAt + surfaces.size() * sizeof(gpu::SurfaceFit) > c->capacity)
    {
        error = "Resident UCT metadata scratch budget exceeded.";
        return false;
    }
    const size_t hostAt = 2 * 1024 * 1024, colorsBytes = rects.size() * sizeof(gpu::Colors),
                 fitsBytes = words.size() * sizeof(gpu::Fit),
                 orientedBytes = oriented.size() * sizeof(gpu::OrientedFit),
                 surfaceBytes=surfaces.size()*sizeof(gpu::SurfaceFit);
    if (hostAt + colorsBytes + fitsBytes + orientedBytes + surfaceBytes > c->capacity)
    {
        error = "Resident UCT metadata result budget exceeded.";
        return false;
    }
    auto at = [&](size_t n) { return static_cast<char *>(c->scratch) + n; };
    if (!Upload(c, rects.constData(), rects.size() * sizeof(gpu::Rect), rectsAt) ||
        !Upload(c, words.constData(), words.size() * sizeof(gpu::Ink), wordsAt) ||
        !Upload(c, indices.constData(), indices.size() * sizeof(int), indicesAt) ||
        !Upload(c, oriented.constData(), oriented.size() * sizeof(gpu::OrientedInk), orientedAt) ||
        !Upload(c, orientedIndices.constData(), orientedIndices.size() * sizeof(int), orientedIndicesAt) ||
        !Upload(c, colorFrames.constData(), colorFrames.size() * sizeof(gpu::OrientedInk), colorFramesAt) ||
        !Upload(c,surfaces.constData(),surfaces.size()*sizeof(gpu::SurfaceRequest),surfacesAt))
        return false;
    c->owners = {image.shared_from_this()};
    gpu::LineColors(image.Data(), image.pitch, image.width, image.height,
                    reinterpret_cast<gpu::Rect *>(at(rectsAt)), rects.size(),
                    reinterpret_cast<gpu::Colors *>(at(colorsAt)), c->stream,
                    reinterpret_cast<gpu::OrientedInk *>(at(colorFramesAt)));
    gpu::InkBounds(image.Data(), image.pitch, image.width, image.height,
                   reinterpret_cast<gpu::Ink *>(at(wordsAt)),
                   reinterpret_cast<int *>(at(indicesAt)), words.size(),
                   reinterpret_cast<gpu::Colors *>(at(colorsAt)),
                   reinterpret_cast<gpu::Fit *>(at(fitsAt)), c->stream);
    gpu::OrientedInkBounds(image.Data(), image.pitch, image.width, image.height,
                           reinterpret_cast<gpu::OrientedInk *>(at(orientedAt)),
                           reinterpret_cast<int *>(at(orientedIndicesAt)), oriented.size(),
                           reinterpret_cast<gpu::Colors *>(at(colorsAt)),
                           reinterpret_cast<gpu::OrientedFit *>(at(orientedFitsAt)), c->stream);
    gpu::SurfaceFrames(image.Data(),image.pitch,image.width,image.height,
        reinterpret_cast<gpu::SurfaceRequest*>(at(surfacesAt)),surfaces.size(),
        reinterpret_cast<gpu::Colors*>(at(colorsAt)),reinterpret_cast<gpu::Fit*>(at(fitsAt)),
        reinterpret_cast<gpu::OrientedFit*>(at(orientedFitsAt)),
        reinterpret_cast<gpu::SurfaceFit*>(at(surfaceFitsAt)),c->stream);
    // Copy outputs to distinct pinned offsets, leaving H2D staging immutable
    // until its DMA completes. No pageable-vector lifetime races.
    if (cudaMemcpyAsync(static_cast<char *>(c->host) + hostAt, at(colorsAt), colorsBytes,
                        cudaMemcpyDeviceToHost, c->stream) != cudaSuccess ||
        cudaMemcpyAsync(static_cast<char *>(c->host) + hostAt + colorsBytes, at(fitsAt), fitsBytes,
                        cudaMemcpyDeviceToHost, c->stream) != cudaSuccess ||
        (orientedBytes && cudaMemcpyAsync(static_cast<char *>(c->host) + hostAt + colorsBytes + fitsBytes,
                                          at(orientedFitsAt), orientedBytes, cudaMemcpyDeviceToHost,
                                          c->stream) != cudaSuccess) ||
        (surfaceBytes && cudaMemcpyAsync(static_cast<char*>(c->host)+hostAt+colorsBytes+fitsBytes+orientedBytes,
            at(surfaceFitsAt),surfaceBytes,cudaMemcpyDeviceToHost,c->stream)!=cudaSuccess))
    {
        Quarantine(c);
        return false;
    }
    if (!Finish(c, &cancel))
    {
        error = "Resident UCT metadata did not complete.";
        return false;
    }
    const auto *colors =
        reinterpret_cast<const gpu::Colors *>(static_cast<char *>(c->host) + hostAt);
    const auto *fits =
        reinterpret_cast<const gpu::Fit *>(static_cast<char *>(c->host) + hostAt + colorsBytes);
    const auto *orientedFits = reinterpret_cast<const gpu::OrientedFit *>(
        static_cast<char *>(c->host) + hostAt + colorsBytes + fitsBytes);
    const auto* surfaceFits=reinterpret_cast<const gpu::SurfaceFit*>(
        static_cast<char*>(c->host)+hostAt+colorsBytes+fitsBytes+orientedBytes);
    const auto pair = [](const gpu::Colors& v) {
        return LineColors{QColor(v.foreground[0], v.foreground[1], v.foreground[2]),
                          QColor(v.background[0], v.background[1], v.background[2])};
    };
    int k = 0, o = 0, colorIndex = lineCount;
    for (int i = 0; i < result.output.lines.size(); ++i) {
        result.lineColors.push_back(pair(colors[i]));
        for (auto& word : result.output.lines[i].words) {
            word.colors = pair(colors[colorIndex++]);
            if (!word.paddedBounds || word.disposition != TokenDisposition::kCandidate) continue;
            if (QuadIsRotated(word.quadPx)) {
                const auto f = orientedFits[o];
                const auto& d = oriented[o++];
                if (!f.fitted) continue; // ambiguous ink keeps the detector span
                const QPointF origin(d.ox, d.oy), u(d.ux, d.uy), v(d.vx, d.vy);
                const auto at = [&](double x, double y) {
                    return origin + u * x + v * y + QPointF(d.warpX, d.warpY) * ((x / d.width) * (y / d.height));
                };
                word.quadPx.points = {at(f.u0, f.v0), at(f.u1, f.v0), at(f.u1, f.v1), at(f.u0, f.v1)};
                word.quadPx.valid = true;
                word.boundsPx = word.quadPx.Bounds();
                word.inkFitted = true;
                continue;
            }
            const auto f = fits[k++];
            const QRectF detected = word.boundsPx;
            word.boundsPx = {f.x, f.y, f.w, f.h};
            word.inkFitted = word.boundsPx != detected;
            if (word.quadPx.valid) word.quadPx = TextQuad::FromRect(word.boundsPx);
        }
    }
    int sourceWord=0,surfaceIndex=0;
    for(auto& line:result.output.lines)for(auto& word:line.words){
        if(surfaceIndex<surfaceWordIndices.size()&&surfaceWordIndices[surfaceIndex]==sourceWord){
            const auto& fit=surfaceFits[surfaceIndex++];
            if(fit.fitted){for(int j=0;j<4;++j)word.quadPx.points[j]={fit.xy[j*2],fit.xy[j*2+1]};
                word.quadPx.valid=true;word.boundsPx=word.quadPx.Bounds();}
        }
        ++sourceWord;
    }
    if (image.kind == AnalysisKind::TemporallySmoothed && !result.output.lines.isEmpty())
    {
        // Trackability of the raw image alone says nothing about stale words
        // still visible in an EMA. Register the recognized pixels against the
        // exact current image first, on this worker and without image egress.
        auto recognized = std::make_shared<Probe>();
        recognized->device = image.device; recognized->width = w; recognized->height = h;
        recognized->integral = Allocate(size_t(w + 1) * (h + 1) * sizeof(unsigned), c);
        if (!recognized->integral) { error = "Smoothed UCT registration allocation failed."; return false; }
        c->owners = {image.shared_from_this(), recognized->integral};
        gpu::ValidationIntegral(image.Data(), image.pitch, image.width, image.height, decimation,
            static_cast<unsigned *>(recognized->integral->data), c->stream);
        if (!Finish(c, &cancel)) { error = "Smoothed UCT registration did not complete."; return false; }
        QVector<QRectF> sceneRects;
        QVector<int> lineIndices;
        QVector<bool> hasCandidate(result.output.lines.size(), false);
        result.currentInputMatches.fill(true, result.output.lines.size());
        const auto map = Mapping(result.stamp, image.width, image.height);
        for (int i = 0; i < result.output.lines.size(); ++i) {
            for (const auto& word : result.output.lines[i].words) {
                if (word.disposition != TokenDisposition::kCandidate) continue;
                hasCandidate[i] = true;
                const auto pixels = word.boundsPx;
                const QRectF r((pixels.x() - map.x) / map.sx, (pixels.y() - map.y) / map.sy,
                    pixels.width() / map.sx, pixels.height() / map.sy);
                if (r.isEmpty() || !std::isfinite(r.x()) || !std::isfinite(r.y()) ||
                    !std::isfinite(r.width()) || !std::isfinite(r.height())) {
                    result.currentInputMatches[i] = false;
                    continue;
                }
                sceneRects.push_back(r);
                lineIndices.push_back(i);
            }
            if (!hasCandidate[i]) result.currentInputMatches[i] = false;
        }
        if (sceneRects.size() > 8192) { error = "Smoothed UCT registration budget exceeded."; return false; }
        // Per-word proof prevents a mostly unchanged line hiding one stale
        // word. Batches retain the 1024-descriptor bound and never copy patches
        // to the host. Zero search excludes both translation and refinement.
        for (qsizetype base = 0; base < sceneRects.size(); base += 1024) {
            const auto rects = sceneRects.mid(base, 1024);
            const auto patches = recognized->References(result.stamp, rects);
            const auto matches = probe->Matches(result.stamp, patches, kInputRegistrationTuning, {}, &cancel);
            if (matches.size() != rects.size()) { error = "Smoothed UCT registration is incomplete."; return false; }
            for (int i = 0; i < matches.size(); ++i)
                result.currentInputMatches[lineIndices[base + i]] =
                    result.currentInputMatches[lineIndices[base + i]] &&
                    matches[i].matched && matches[i].sceneOffset.isNull();
        }
    }
    if (!result.stamp.liveCameraCapture)
    {
        int n = (image.width * image.height * 4 + 4095) / 4096;
        const size_t offset = size_t(n) * 32;
        if (offset * 2 > c->capacity)
        {
            error = "Resident UCT fingerprint scratch budget exceeded.";
            return false;
        }
        auto *a = static_cast<unsigned char *>(c->scratch);
        auto *b = a + offset;
        c->owners = {image.shared_from_this()};
        gpu::FingerprintLeaves(image.ValidationData(), image.ValidationPitch(), image.width, image.height, a, c->stream);
        while (n > 1)
        {
            gpu::FingerprintParents(a, n, b, c->stream);
            n = (n + 15) / 16;
            std::swap(a, b);
        }
        if (!CopyResult(c, a, 32, &cancel))
        {
            error = "Resident UCT fingerprint did not complete.";
            return false;
        }
        result.sourcePixelDigest = QByteArray(static_cast<const char *>(c->host), 32);
        result.sourcePixelDigestKind = RecognitionResult::DigestKind::GpuTreeSha256V1;
    }
    return !cancel.load();
}
} // namespace okuflow::uct::resident
