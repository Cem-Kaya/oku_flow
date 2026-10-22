#include "okuflow/cuda/uct_kernels.hpp"
#include "../common/uct_recognizer_width.hpp"
#include <cmath>
#include <stdexcept>
#include <cuda_runtime.h>

namespace okuflow::uct::gpu
{
namespace
{
__device__ float bilinear(float fx, float fy, float c00, float c10, float c01, float c11)
{
    const float a = c00 * (1 - fx) + c10 * fx;
    const float b = c01 * (1 - fx) + c11 * fx;
    return a * (1 - fy) + b * fy;
}
// All three BGRA channels with one shared clamp/index/weight computation.
// Each channel evaluates the identical expression the former per-channel
// sampling used, so results are unchanged; only redundant recomputation goes.
__device__ float3 sample3(const unsigned char *pixels, std::size_t pitch, int w, int h, float x,
                          float y)
{
    x = fminf(fmaxf(x, 0), w - 1.f);
    y = fminf(fmaxf(y, 0), h - 1.f);
    const int ix = static_cast<int>(x), iy = static_cast<int>(y), jx = min(ix + 1, w - 1),
              jy = min(iy + 1, h - 1);
    const float fx = x - ix, fy = y - iy;
    const unsigned char *p00 = pixels + iy * pitch + ix * 4, *p10 = pixels + iy * pitch + jx * 4,
                        *p01 = pixels + jy * pitch + ix * 4, *p11 = pixels + jy * pitch + jx * 4;
    return {bilinear(fx, fy, p00[0], p10[0], p01[0], p11[0]),
            bilinear(fx, fy, p00[1], p10[1], p01[1], p11[1]),
            bilinear(fx, fy, p00[2], p10[2], p01[2], p11[2])};
}
__global__ void normalize(const unsigned char *pixels, std::size_t pitch, int w, int h, float *out,
                          int ow, int oh)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= ow * oh)
        return;
    const float x = (i % ow + .5f) * w / ow - .5f, y = (i / ow + .5f) * h / oh - .5f;
    const float means[3]{.485f, .456f, .406f}, dev[3]{.229f, .224f, .225f};
    const float3 value = sample3(pixels, pitch, w, h, x, y);
    const float channels[3]{value.x, value.y, value.z};
    for (int c = 0; c < 3; ++c)
        out[c * ow * oh + i] = (channels[c] / 255 - means[c]) / dev[c];
}
__global__ void probe(const unsigned char *pixels, std::size_t pitch, int w, int h,
                      unsigned char *out, int d)
{
    const int ow = w / d, oh = h / d, i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= ow * oh)
        return;
    const int step = max(1, d / 2), per = (d + step - 1) / step, area = per * per;
    for (int ch = 0; ch < 4; ++ch)
    {
        int sum = 0;
        for (int y = 0; y < d; y += step)
            for (int x = 0; x < d; x += step)
                sum += pixels[((i / ow) * d + y) * pitch + ((i % ow) * d + x) * 4 + ch];
        out[i * 4 + ch] = static_cast<unsigned char>(sum / area);
    }
}
__global__ void init(const float *map, int *labels, Component *comps, int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n)
        return;
    labels[i] = isfinite(map[i]) && map[i] > .2f ? i : -1;
    Component c{};
    c.u0 = c.v0 = 1e20f;
    c.u1 = c.v1 = -1e20f;
    comps[i] = c;
}
__device__ int root(int *labels, int i)
{
    int next = labels[i];
    while (next != i)
    {
        i = next;
        next = labels[i];
    }
    return i;
}
__device__ void join(int *labels, int a, int b)
{
    for (;;)
    {
        a = root(labels, a);
        b = root(labels, b);
        if (a == b)
            return;
        const int high = max(a, b), low = min(a, b);
        if (atomicCAS(labels + high, high, low) == high)
            return;
    }
}
__global__ void connect(int *labels, int w, int h)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= w * h || labels[i] < 0)
        return;
    const int x = i % w, y = i / w;
    if (x && labels[i - 1] >= 0)
        join(labels, i, i - 1);
    if (y)
        for (int dx = -1; dx <= 1; ++dx)
            if (x + dx >= 0 && x + dx < w && labels[i - w + dx] >= 0)
                join(labels, i, i - w + dx);
}
__global__ void moments(int *labels, Component *comps, int w, int h)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= w * h || labels[i] < 0)
        return;
    const int r = root(labels, i);
    labels[i] = r;
    // Local moments avoid cancellation from large absolute image coordinates.
    const long long x = (i % w) - (r % w), y = (i / w) - (r / w);
    atomicAdd(&comps[r].n, 1);
    // Exact bounded integer moments are independent of atomic arrival order.
    atomicAdd(reinterpret_cast<unsigned long long *>(&comps[r].x),
              static_cast<unsigned long long>(x));
    atomicAdd(reinterpret_cast<unsigned long long *>(&comps[r].y),
              static_cast<unsigned long long>(y));
    atomicAdd(reinterpret_cast<unsigned long long *>(&comps[r].xx),
              static_cast<unsigned long long>(x * x));
    atomicAdd(reinterpret_cast<unsigned long long *>(&comps[r].yy),
              static_cast<unsigned long long>(y * y));
    atomicAdd(reinterpret_cast<unsigned long long *>(&comps[r].xy),
              static_cast<unsigned long long>(x * y));
}
__global__ void axes(Component *comps, int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n || comps[i].n < 9)
        return;
    auto &c = comps[i];
    const double count = c.n, x = c.x, y = c.y;
    const double a = .5 * atan2(2 * (c.xy - x * y / count), c.xx - x * x / count - c.yy + y * y / count);
    c.c = static_cast<float>(cos(a));
    c.s = static_cast<float>(sin(a));
}
__device__ void amin(float *p, float v)
{
    int *i = reinterpret_cast<int *>(p);
    int old = *i, assumed;
    do
    {
        assumed = old;
        if (__int_as_float(old) <= v)
            return;
        old = atomicCAS(i, assumed, __float_as_int(v));
    } while (old != assumed);
}
__device__ void amax(float *p, float v)
{
    int *i = reinterpret_cast<int *>(p);
    int old = *i, assumed;
    do
    {
        assumed = old;
        if (__int_as_float(old) >= v)
            return;
        old = atomicCAS(i, assumed, __float_as_int(v));
    } while (old != assumed);
}
__global__ void extents(const int *labels, Component *comps, int w, int h)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= w * h || labels[i] < 0)
        return;
    auto &c = comps[labels[i]];
    if (c.n < 9)
        return;
    const float x = i % w, y = i / w;
    const float u = x * c.c + y * c.s, v = -x * c.s + y * c.c;
    amin(&c.u0, u);
    amax(&c.u1, u);
    amin(&c.v0, v);
    amax(&c.v1, v);
}
__global__ void boxes(const float *map, const int *labels, Component *comps, int w, int h, int sw,
                      int sh)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= w * h || labels[i] != i || comps[i].n < 9)
        return;
    const auto c = comps[i];
    const float bw = c.u1 - c.u0 + 1, bh = c.v1 - c.v0 + 1;
    if (bw < 3 || bh < 3)
        return;
    float minx = 1e20f, miny = 1e20f, maxx = -1e20f, maxy = -1e20f;
    for (int k = 0; k < 4; ++k)
    {
        const float u = (k == 0 || k == 3) ? c.u0 : c.u1, v = k < 2 ? c.v0 : c.v1;
        const float x = u * c.c - v * c.s, y = u * c.s + v * c.c;
        minx = fminf(minx, x);
        maxx = fmaxf(maxx, x);
        miny = fminf(miny, y);
        maxy = fmaxf(maxy, y);
    }
    float score = 0;
    int samples = 0;
    for (int y = max(0, static_cast<int>(floorf(miny)));
         y <= min(h - 1, static_cast<int>(ceilf(maxy))); ++y)
        for (int x = max(0, static_cast<int>(floorf(minx)));
             x <= min(w - 1, static_cast<int>(ceilf(maxx))); ++x)
        {
            const float u = x * c.c + y * c.s, v = -x * c.s + y * c.c;
            if (u >= c.u0 && u <= c.u1 && v >= c.v0 && v <= c.v1)
            {
                score += map[y * w + x];
                ++samples;
            }
        }
    if (!samples || !isfinite(score) || score / samples < .4f)
        return;
    const float d = bw * bh * 1.4f / (2 * (bw + bh));
    Quad q{};
    q.score = score / samples;
    for (int k = 0; k < 4; ++k)
    {
        const float u = (k == 0 || k == 3) ? c.u0 - d : c.u1 + d, v = k < 2 ? c.v0 - d : c.v1 + d;
        q.xy[k * 2] = fminf(fmaxf((u * c.c - v * c.s) * sw / w, 0), static_cast<float>(sw));
        q.xy[k * 2 + 1] = fminf(fmaxf((u * c.s + v * c.c) * sh / h, 0), static_cast<float>(sh));
    }
    if (bh > bw * 1.5f)
    {
        Quad old = q;
        for (int k = 0; k < 4; ++k)
        {
            q.xy[k * 2] = old.xy[((k + 3) % 4) * 2];
            q.xy[k * 2 + 1] = old.xy[((k + 3) % 4) * 2 + 1];
        }
    }
    const float qw = hypotf(q.xy[2] - q.xy[0], q.xy[3] - q.xy[1]),
                qh = hypotf(q.xy[6] - q.xy[0], q.xy[7] - q.xy[1]);
    if (qw < 2 || qh < 2)
        return;
    comps[i].score = q.score;
}
// Select the first 128 valid connected components in raster/root order, just
// like the CPU detector. Atomic arrival order must never choose a dense page's
// visible text. The label scratch becomes block offsets after boxes finishes.
__device__ int eligibleRank(bool eligible, int *warpCounts)
{
    const unsigned mask = __ballot_sync(0xffffffffu, eligible);
    const int lane = threadIdx.x & 31, warp = threadIdx.x / 32;
    if (!lane)
        warpCounts[warp] = __popc(mask);
    __syncthreads();
    int rank = __popc(mask & ((1u << lane) - 1u));
    for (int i = 0; i < warp; ++i)
        rank += warpCounts[i];
    return rank;
}
__global__ void countBoxes(const Component *comps, int n, int *counts)
{
    __shared__ int warpCounts[8];
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const bool eligible = i < n && comps[i].score > 0;
    const int rank = eligibleRank(eligible, warpCounts);
    if (threadIdx.x == 255)
        counts[blockIdx.x] = rank + eligible;
}
__global__ void boxOffsets(int *counts, int blocks, int *count)
{
    int total = 0;
    for (int i = 0; i < blocks; ++i)
    {
        const int size = counts[i];
        counts[i] = total;
        total += size;
    }
    *count = total; // reports truncation separately from the bounded output
}
__global__ void emitBoxes(const Component *comps, int w, int h, int sw, int sh, const int *offsets,
                          Quad *quads)
{
    __shared__ int warpCounts[8];
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const bool eligible = i < w * h && comps[i].score > 0;
    const int index = offsets[blockIdx.x] + eligibleRank(eligible, warpCounts);
    if (!eligible || index >= 128)
        return;
    const auto c = comps[i];
    const float bw = c.u1 - c.u0 + 1, bh = c.v1 - c.v0 + 1;
    const float d = bw * bh * 1.4f / (2 * (bw + bh));
    Quad q{};
    q.score = c.score;
    for (int k = 0; k < 4; ++k)
    {
        const float u = (k == 0 || k == 3) ? c.u0 - d : c.u1 + d;
        const float v = k < 2 ? c.v0 - d : c.v1 + d;
        q.xy[k * 2] = fminf(fmaxf((u * c.c - v * c.s) * sw / w, 0), static_cast<float>(sw));
        q.xy[k * 2 + 1] = fminf(fmaxf((u * c.s + v * c.c) * sh / h, 0), static_cast<float>(sh));
    }
    if (bh > bw * 1.5f)
    {
        const auto old = q;
        for (int k = 0; k < 4; ++k)
        {
            q.xy[k * 2] = old.xy[((k + 3) % 4) * 2];
            q.xy[k * 2 + 1] = old.xy[((k + 3) % 4) * 2 + 1];
        }
    }
    const float qw = hypotf(q.xy[2] - q.xy[0], q.xy[3] - q.xy[1]);
    const float qh = hypotf(q.xy[6] - q.xy[0], q.xy[7] - q.xy[1]);
    q.validWidth = max(1, static_cast<int>(ceilf(48 * qw / fmaxf(qh, 1))));
    quads[index] = q;
}
__global__ void recognitionWindows(const Quad* detected,int* count,Quad* windows)
{
    int written=0;
    for(int index=0;index<min(*count,128);++index) {
        const auto q=detected[index];
        windows[written++]=q;
        const float width=hypotf(q.xy[2]-q.xy[0],q.xy[3]-q.xy[1]);
        const float height=hypotf(q.xy[6]-q.xy[0],q.xy[7]-q.xy[1]);
        if(!isfinite(width) || !isfinite(height) || height<1 || width/height<=20)continue;
        const int n=min(8,max(2,int(ceilf((width/height-8)/12))));
        const float span=fmaxf(20,(width/height+8*(n-1))/n);
        const float fraction=fminf(1,span*height/width);
        for(int i=0;i<n;++i) {
            const float start=i*(1-fraction)/(n-1),end=start+fraction;
            auto window=q;
            for(int axis=0;axis<2;++axis) {
                window.xy[axis]=q.xy[axis]*(1-start)+q.xy[2+axis]*start;
                window.xy[2+axis]=q.xy[axis]*(1-end)+q.xy[2+axis]*end;
                window.xy[4+axis]=q.xy[6+axis]*(1-end)+q.xy[4+axis]*end;
                window.xy[6+axis]=q.xy[6+axis]*(1-start)+q.xy[4+axis]*start;
            }
            const float ww=hypotf(window.xy[2]-window.xy[0],window.xy[3]-window.xy[1]);
            const float wh=hypotf(window.xy[6]-window.xy[0],window.xy[7]-window.xy[1]);
            window.validWidth=max(1,int(ceilf(48*ww/fmaxf(wh,1))));
            window.internalEdges=(i>0?1:0)|(i+1<n?2:0);
            windows[written++]=window;
        }
    }
    *count=written;
}
__global__ void recognitionGroupWidths(const Quad* quads, const int* count, int groupSize, int* widths)
{
    const int group = blockIdx.x * blockDim.x + threadIdx.x;
    const int groups = (recognizer_width::kMaximumCrops + groupSize - 1) / groupSize;
    if (group >= groups) return;
    const int n = *count, first = group * groupSize;
    // Every copied descriptor is initialized, including unused trailing slots.
    widths[group] = n < 0 || n > recognizer_width::kMaximumCrops || first >= n ? 0
        : recognizer_width::OrderedGroup(quads + first, min(groupSize, n - first));
}
__device__ void rectifyOne(const unsigned char *pixels, std::size_t pitch, int w, int h,
                           const Quad *quads, int index, float *out, int ow, int i)
{
    const auto q = quads[index];
    const int x = i % ow, y = i / ow, valid = min(ow, q.validWidth);
    if (x >= valid)
    {
        out[i] = out[ow * 48 + i] = out[2 * ow * 48 + i] = 0;
        return;
    }
    const float u = (x + .5f) / valid, v = (y + .5f) / 48;
    const float sx = q.xy[0] * (1 - u) * (1 - v) + q.xy[2] * u * (1 - v) + q.xy[4] * u * v +
                     q.xy[6] * (1 - u) * v - .5f;
    const float sy = q.xy[1] * (1 - u) * (1 - v) + q.xy[3] * u * (1 - v) + q.xy[5] * u * v +
                     q.xy[7] * (1 - u) * v - .5f;
    const float3 value = sample3(pixels, pitch, w, h, sx, sy);
    out[i] = (value.x / 255 - .5f) * 2;
    out[ow * 48 + i] = (value.y / 255 - .5f) * 2;
    out[2 * ow * 48 + i] = (value.z / 255 - .5f) * 2;
}
__global__ void rectify(const unsigned char *pixels, std::size_t pitch, int w, int h,
                        const Quad *quads, int index, float *out, int ow)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= ow * 48)
        return;
    rectifyOne(pixels, pitch, w, h, quads, index, out, ow, i);
}
__global__ void rectifyBatch(const unsigned char *pixels, std::size_t pitch, int w, int h,
                             const Quad *quads, const int *slotIndices, float *out, int ow)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= ow * 48)
        return;
    const int sample = blockIdx.y;
    rectifyOne(pixels, pitch, w, h, quads, slotIndices[sample],
               out + static_cast<std::size_t>(sample) * ow * 48 * 3, ow, i);
}
__device__ void argmaxStep(const float *probabilities, int t, int classes, int *ids,
                           float *scores)
{
    __shared__ float vals[256];
    __shared__ int indices[256];
    float best = -1;
    int id = 0;
    bool bad = false;
    for (int c = threadIdx.x; c < classes; c += blockDim.x)
    {
        const float v = probabilities[t * classes + c];
        if (!isfinite(v) || v < 0 || v > 1.0001f)
            bad = true;
        if (v > best)
        {
            best = v;
            id = c;
        }
    }
    vals[threadIdx.x] = bad ? NAN : best;
    indices[threadIdx.x] = id;
    __syncthreads();
    for (int delta = 128; delta; delta /= 2)
    {
        if (threadIdx.x < delta)
        {
            if (!isfinite(vals[threadIdx.x + delta]))
                vals[threadIdx.x] = NAN;
            else if (isfinite(vals[threadIdx.x]) &&
                     (vals[threadIdx.x + delta] > vals[threadIdx.x] ||
                      (vals[threadIdx.x + delta] == vals[threadIdx.x] &&
                       indices[threadIdx.x + delta] < indices[threadIdx.x])))
            {
                vals[threadIdx.x] = vals[threadIdx.x + delta];
                indices[threadIdx.x] = indices[threadIdx.x + delta];
            }
        }
        __syncthreads();
    }
    if (!threadIdx.x)
    {
        ids[t] = indices[0];
        scores[t] = vals[0];
    }
}
__global__ void argmax(const float *probabilities, int steps, int classes, int *ids, float *scores)
{
    const int t = blockIdx.x;
    if (t >= steps)
        return;
    argmaxStep(probabilities, t, classes, ids, scores);
}
__global__ void argmaxBatch(const float *probabilities, std::size_t sampleOutputs, int steps,
                            int classes, int *ids, float *scores)
{
    const int t = blockIdx.x;
    if (t >= steps)
        return;
    const int sample = blockIdx.y;
    argmaxStep(probabilities + sample * sampleOutputs, t, classes, ids + sample * steps,
               scores + sample * steps);
}
__device__ void compactOne(int *ids, float *scores, int steps, const Quad *quads, int index,
                           int cropWidth, Token *tokens, int *count)
{
    int n = 0, previous = -1;
    const int limit =
        min(steps, static_cast<int>(ceilf(steps * min(cropWidth, quads[index].validWidth) /
                                          static_cast<float>(cropWidth))));
    for (int t = 0; t < limit; ++t)
    {
        if (!isfinite(scores[t]))
        {
            *count = -1;
            return;
        }
        if (ids[t])
        {
            if (ids[t] == previous && n)
                tokens[n - 1].last = t;
            else
                tokens[n++] = {ids[t], t, t, scores[t]};
        }
        previous = ids[t];
    }
    *count = n;
}
__global__ void compact(int *ids, float *scores, int steps, const Quad *quads, int index,
                        int cropWidth, Token *tokens, int *count)
{
    if (threadIdx.x || blockIdx.x)
        return;
    compactOne(ids, scores, steps, quads, index, cropWidth, tokens, count);
}
__global__ void compactBatch(int *ids, float *scores, int steps, const Quad *quads,
                             const int *slotIndices, int cropWidth, Token *tokens, int *counts)
{
    if (threadIdx.x)
        return;
    const int sample = blockIdx.x;
    compactOne(ids + sample * steps, scores + sample * steps, steps, quads,
               slotIndices[sample], cropWidth, tokens + sample * steps, counts + sample);
}
} // namespace
void Normalize(const void *p, std::size_t pitch, int w, int h, float *out, int ow, int oh,
               cudaStream_t s)
{
    normalize<<<(ow * oh + 255) / 256, 256, 0, s>>>(static_cast<const unsigned char *>(p), pitch, w,
                                                    h, out, ow, oh);
}
void Probe(const void *p, std::size_t pitch, int w, int h, void *out, int d, cudaStream_t s)
{
    probe<<<((w / d) * (h / d) + 255) / 256, 256, 0, s>>>(
        static_cast<const unsigned char *>(p), pitch, w, h, static_cast<unsigned char *>(out), d);
}
void Detect(const float *map, int w, int h, int sw, int sh, int *labels, Component *comps,
            Quad *quads, int *count, cudaStream_t s)
{
    const int blocks = (w * h + 255) / 256;
    cudaMemsetAsync(count, 0, sizeof(int), s);
    init<<<blocks, 256, 0, s>>>(map, labels, comps, w * h);
    connect<<<blocks, 256, 0, s>>>(labels, w, h);
    moments<<<blocks, 256, 0, s>>>(labels, comps, w, h);
    axes<<<blocks, 256, 0, s>>>(comps, w * h);
    extents<<<blocks, 256, 0, s>>>(labels, comps, w, h);
    boxes<<<blocks, 256, 0, s>>>(map, labels, comps, w, h, sw, sh);
    countBoxes<<<blocks, 256, 0, s>>>(comps, w * h, labels);
    boxOffsets<<<1, 1, 0, s>>>(labels, blocks, count);
    emitBoxes<<<blocks, 256, 0, s>>>(comps, w, h, sw, sh, labels, quads);
}
void RecognitionWindows(const Quad* detected,int* count,Quad* windows,cudaStream_t stream)
{
    recognitionWindows<<<1,1,0,stream>>>(detected,count,windows);
}
void RecognitionGroupWidths(const Quad* quads, const int* count, int groupSize, int* widths,
                            cudaStream_t stream)
{
    // The worker supplies the existing bounded submission group, 8 or16.
    if (groupSize != 8 && groupSize != 16) throw std::invalid_argument("Unsupported recognition submission group.");
    const int groups = (recognizer_width::kMaximumCrops + groupSize - 1) / groupSize;
    recognitionGroupWidths<<<(groups + 127) / 128, 128, 0, stream>>>(quads,count,groupSize,widths);
}
void Rectify(const void *p, std::size_t pitch, int w, int h, const Quad *quads, int index,
             float *out, int ow, cudaStream_t s)
{
    rectify<<<(ow * 48 + 255) / 256, 256, 0, s>>>(static_cast<const unsigned char *>(p), pitch, w,
                                                  h, quads, index, out, ow);
}
void Decode(const float *p, int steps, int classes, const Quad *quads, int index, int cropWidth,
            int *ids, float *scores, Token *tokens, int *count, cudaStream_t s)
{
    argmax<<<steps, 256, 0, s>>>(p, steps, classes, ids, scores);
    compact<<<1, 1, 0, s>>>(ids, scores, steps, quads, index, cropWidth, tokens, count);
}
void RectifyBatch(const void *p, std::size_t pitch, int w, int h, const Quad *quads,
                  const int *slotIndices, int batchSize, float *out, int ow, cudaStream_t s)
{
    if (batchSize < 1 || batchSize > 16 || ow < 1)
        throw std::invalid_argument("Unsupported batched rectification shape.");
    const dim3 grid(static_cast<unsigned>((ow * 48 + 255) / 256),
                    static_cast<unsigned>(batchSize));
    rectifyBatch<<<grid, 256, 0, s>>>(static_cast<const unsigned char *>(p), pitch, w, h, quads,
                                      slotIndices, out, ow);
}
void DecodeBatch(const float *p, std::size_t sampleOutputs, int steps, int classes,
                 const Quad *quads, const int *slotIndices, int batchSize, int cropWidth,
                 int *ids, float *scores, Token *tokens, int *counts, cudaStream_t s)
{
    if (batchSize < 1 || batchSize > 16 || steps < 1 || classes < 1)
        throw std::invalid_argument("Unsupported batched CTC decode shape.");
    const dim3 grid(static_cast<unsigned>(steps), static_cast<unsigned>(batchSize));
    argmaxBatch<<<grid, 256, 0, s>>>(p, sampleOutputs, steps, classes, ids, scores);
    compactBatch<<<batchSize, 1, 0, s>>>(ids, scores, steps, quads, slotIndices, cropWidth,
                                         tokens, counts);
}
} // namespace okuflow::uct::gpu

#include "uct_validation_kernels.cuh"
