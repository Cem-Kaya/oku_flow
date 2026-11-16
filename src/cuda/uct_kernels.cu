#include "okuflow/cuda/uct_kernels.hpp"
#include "../common/uct_recognizer_width.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <cuda_runtime.h>

namespace okuflow::uct::gpu
{
namespace
{
__device__ float sample(const unsigned char *pixels, std::size_t pitch, int w, int h, float x,
                        float y, int ch)
{
    x = fminf(fmaxf(x, 0), w - 1.f);
    y = fminf(fmaxf(y, 0), h - 1.f);
    const int ix = static_cast<int>(x), iy = static_cast<int>(y), jx = min(ix + 1, w - 1),
              jy = min(iy + 1, h - 1);
    const float a = pixels[iy * pitch + ix * 4 + ch] * (1 - (x - ix)) +
                    pixels[iy * pitch + jx * 4 + ch] * (x - ix);
    const float b = pixels[jy * pitch + ix * 4 + ch] * (1 - (x - ix)) +
                    pixels[jy * pitch + jx * 4 + ch] * (x - ix);
    return a * (1 - (y - iy)) + b * (y - iy);
}
__global__ void normalize(const unsigned char *pixels, std::size_t pitch, int w, int h, float *out,
                          int ow, int oh)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= ow * oh)
        return;
    const float x = (i % ow + .5f) * w / ow - .5f, y = (i / ow + .5f) * h / oh - .5f;
    const float means[3]{.485f, .456f, .406f}, dev[3]{.229f, .224f, .225f};
    for (int c = 0; c < 3; ++c)
        out[c * ow * oh + i] = (sample(pixels, pitch, w, h, x, y, c) / 255 - means[c]) / dev[c];
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
// Parent links only ever decrease towards the component's smallest index, so
// the root a walk reaches does not depend on which links other threads have
// already shortened. The compressing walk replaces each visited link with its
// grandparent (atomic, monotone); the plain walk is the original oracle.
template <bool compress> __device__ int root(int *labels, int i)
{
    if (!compress)
    {
        int next = labels[i];
        while (next != i)
        {
            i = next;
            next = labels[i];
        }
        return i;
    }
    int next = atomicOr(labels + i, 0);
    while (next != i)
    {
        const int ancestor = atomicOr(labels + next, 0);
        atomicMin(labels + i, ancestor);
        i = next;
        next = ancestor;
    }
    return i;
}
template <bool compress> __device__ void join(int *labels, int a, int b)
{
    for (;;)
    {
        a = root<compress>(labels, a);
        b = root<compress>(labels, b);
        if (a == b)
            return;
        const int high = max(a, b), low = min(a, b);
        if (atomicCAS(labels + high, high, low) == high)
            return;
    }
}
template <bool compress> __global__ void connect(int *labels, int w, int h)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= w * h || labels[i] < 0)
        return;
    const int x = i % w, y = i / w;
    if (x && labels[i - 1] >= 0)
        join<compress>(labels, i, i - 1);
    if (y)
        for (int dx = -1; dx <= 1; ++dx)
            if (x + dx >= 0 && x + dx < w && labels[i - w + dx] >= 0)
                join<compress>(labels, i, i - w + dx);
}
template <bool compress> __global__ void moments(int *labels, Component *comps, int w, int h)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= w * h || labels[i] < 0)
        return;
    const int r = root<compress>(labels, i);
    // Other threads may still be shortening this link atomically; r is the
    // smallest value it can ever hold, so an atomic minimum publishes it.
    if (compress)
        atomicMin(labels + i, r);
    else
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
// The rotated-frame test of the original boxes kernel, with the rounding its
// compiled form has under the CUDA 12.9 toolkit for sm_61 through sm_90. The
// compiler hoists the row products and unrolls the column loop by four: the
// unrolled columns fuse the column product into the sum, while the leading
// width % 4 columns of each row add two separately rounded products. A pixel
// that defined an extent can sit exactly on the bound, so this difference
// decides membership and therefore the score. It is written out here so the
// cooperative kernel selects the same pixels as the original.
//
// This is tied to that toolkit. For sm_100 and later the same compiler fuses
// every column, and for the other architectures only the generated
// intermediate code was compared, which does not settle the final rounding.
// DetectKernels::Exact therefore selects the exact sum of this kernel only
// where it ran against the original bit for bit (sm_89) or has identical
// machine code (sm_86). The default tree sum uses the same inclusion test on
// every GPU; it does not claim the original's bits.
// gpuDetectKernelsMatchReferenceBytes holds the two kernels to the same bits
// on the GPU it runs on and fails if a compiler changes either one.
__device__ __forceinline__ bool boxesInside(const Component &c, int x, int y, bool fused)
{
    const float column = static_cast<float>(x), negated = static_cast<float>(-x), row = static_cast<float>(y);
    const float rowU = __fmul_rn(c.s, row), rowV = __fmul_rn(c.c, row);
    const float u = fused ? fmaf(c.c, column, rowU) : __fadd_rn(rowU, __fmul_rn(c.c, column));
    const float v = fused ? fmaf(c.s, negated, rowV) : __fadd_rn(rowV, __fmul_rn(c.s, negated));
    return u >= c.u0 && u <= c.u1 && v >= c.v0 && v <= c.v1;
}
// Adds up to 32 staged values to `score` with exactly the result of adding
// them one at a time in lane order with round-to-nearest-even float adds, but
// without the serial chain. While the running sum stays inside one binade its
// unit in the last place U is fixed, so each addition is the integer step
// "mantissa += x / U rounded to nearest". Only an exact half depends on what
// came before, and only through the mantissa's parity: a tie rounds to an even
// mantissa. Lanes therefore derive their own step from two ballots (ties and
// odd non-tie steps) and the steps are summed as integers. The caller falls
// back to the serial fold whenever this returns false: a value that is
// negative, non-finite or above 4, a sum that is not yet a normal positive
// number, or a sum that would leave its binade within this group. A single
// value of 2^30 U or more is refused before it is shifted; smaller values that
// leave the binade are caught by the final mantissa check.
__device__ __forceinline__ bool boxesAddGroup(float &score, unsigned mask, float value, int lane)
{
    const unsigned bits = __float_as_uint(score);
    const int exponent = static_cast<int>(bits >> 23) - 127; // sign bit clear is checked below
    const bool usable = !(mask >> lane & 1u) || (value >= 0.f && value <= 4.f);
    if (__ballot_sync(0xffffffffu, usable) != 0xffffffffu || !(score >= 1e-30f) || exponent > 100)
        return false;
    const int unit = exponent - 23; // U == 2^unit
    long long step = 0;
    bool tie = false, large = false;
    const unsigned valueBits = __float_as_uint(value);
    const int valueExponent = static_cast<int>(valueBits >> 23 & 0xffu);
    if ((mask >> lane & 1u) && valueExponent) // zero and subnormal values are far below U / 2
    {
        const unsigned mantissa = (valueBits & 0x7fffffu) | 0x800000u;
        const int shift = valueExponent - 150 - unit; // exponent of the value's last bit, in units of U
        if (shift > 6)
            large = true; // at least 2^30 U: far outside this binade
        else if (shift >= 0)
            step = static_cast<long long>(mantissa) << shift; // below 2^30
        else if (shift > -25)
        {
            const unsigned remainder = mantissa & ((1u << -shift) - 1u), half = 1u << (-shift - 1);
            step = mantissa >> -shift;
            tie = remainder == half;
            step += remainder > half;
        }
    }
    if (__ballot_sync(0xffffffffu, large))
        return false;
    const unsigned ties = __ballot_sync(0xffffffffu, tie);
    const unsigned odds = __ballot_sync(0xffffffffu, !tie && (step & 1));
    const unsigned below = (1u << lane) - 1u, tiesBelow = ties & below;
    // Mantissa parity just before this lane: even after the nearest earlier
    // tie, otherwise the incoming parity; then toggled by each odd step since.
    const unsigned since = tiesBelow ? odds & below & ~((2u << (31 - __clz(tiesBelow))) - 1u) : odds & below;
    const unsigned parity = ((tiesBelow ? 0u : bits & 1u) + __popc(since)) & 1u;
    if (tie)
        step += (parity + static_cast<unsigned>(step)) & 1u;
    for (int distance = 16; distance; distance /= 2)
        step += __shfl_xor_sync(0xffffffffu, step, distance);
    const long long mantissa = static_cast<long long>((bits & 0x7fffffu) | 0x800000u) + step;
    if (mantissa >= 0x1000000ll)
        return false; // leaves the binade: U changes, so replay this group serially
    score = __uint_as_float((bits & 0xff800000u) | (static_cast<unsigned>(mantissa) & 0x7fffffu));
    return true;
}
// Test support for BoxSumGroups: one warp per group, no fallback.
__global__ void boxSumGroups(const float *scores, const unsigned *masks, const float *values,
                             float *results, int *fast, int groups)
{
    const int group = blockIdx.x, lane = threadIdx.x;
    if (blockDim.x != 32 || group >= groups)
        return;
    float score = scores[group];
    const unsigned mask = masks[group];
    const float value = (mask >> lane & 1u) ? values[static_cast<std::size_t>(group) * 32 + lane] : 0.f;
    const bool exact = boxesAddGroup(score, mask, value, lane);
    if (!lane)
    {
        results[group] = score;
        fast[group] = exact;
    }
}
// One warp scores one component. Lanes load and test 32 consecutive pixels of
// the bounding scan in parallel. With tree == false the accepted values are
// added in the original raster order, group by group, with the float sum and
// count the boxes kernel computes: boxesAddGroup where it applies, otherwise
// lane zero folds the staged values serially. With tree == true (the default)
// each lane keeps a double partial sum over its fixed stride and a fixed
// shuffle tree combines them: defined and repeatable, but not the original
// rounding. The extents and scan bounds it starts from are compiled float
// arithmetic, as in the original.
__device__ void boxesWarp(int i, const float *map, Component *comps, int w, int h, int sw, int sh,
                          bool tree, float *staged)
{
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
    const int lane = threadIdx.x & 31;
    const int x0 = max(0, static_cast<int>(floorf(minx))), x1 = min(w - 1, static_cast<int>(ceilf(maxx)));
    const int y0 = max(0, static_cast<int>(floorf(miny))), y1 = min(h - 1, static_cast<int>(ceilf(maxy)));
    const int width = x1 - x0 + 1, total = width > 0 && y1 >= y0 ? width * (y1 - y0 + 1) : 0;
    const int unfused = width & 3; // leading columns of every row, as compiled in the original
    float score = 0;
    int samples = 0;
    if (tree)
    {
        double partial = 0;
        int number = 0;
        for (int index = lane; index < total; index += 32)
        {
            const int column = index % width, x = x0 + column, y = y0 + index / width;
            if (boxesInside(c, x, y, column >= unfused))
            {
                partial += map[y * w + x];
                ++number;
            }
        }
        for (int step = 16; step; step /= 2)
        {
            partial += __shfl_down_sync(0xffffffffu, partial, step);
            number += __shfl_down_sync(0xffffffffu, number, step);
        }
        score = static_cast<float>(partial);
        samples = number;
    }
    else
    {
        for (int base = 0; base < total; base += 32)
        {
            const int index = base + lane;
            bool accepted = false;
            float value = 0;
            if (index < total)
            {
                const int column = index % width, x = x0 + column, y = y0 + index / width;
                accepted = boxesInside(c, x, y, column >= unfused);
                if (accepted)
                    value = map[y * w + x];
            }
            // Ascending lane index is ascending raster index within this group
            // of 32, and bases ascend, so lane zero folds in the original order.
            // Each lane owns one slot; the warp barriers order the stores before
            // lane zero's reads and those reads before the next group's stores.
            const unsigned mask = __ballot_sync(0xffffffffu, accepted);
            if (!mask)
                continue;
            samples += __popc(mask);
            if (boxesAddGroup(score, mask, accepted ? value : 0.f, lane))
                continue;
            staged[lane] = value;
            __syncwarp();
            if (!lane)
                for (unsigned rest = mask; rest; rest &= rest - 1)
                    score += staged[__ffs(rest) - 1];
            __syncwarp();
            score = __shfl_sync(0xffffffffu, score, 0);
        }
    }
    if (lane)
        return;
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
// Launch contract: 256 threads per block. The block lists its own roots (their
// order is irrelevant: each is scored independently) and its eight warps take
// them in turn.
__global__ void boxesCooperative(const float *map, const int *labels, Component *comps, int w,
                                 int h, int sw, int sh, bool tree)
{
    __shared__ int roots[256];
    __shared__ float staged[256];
    __shared__ int count;
    if (!threadIdx.x)
        count = 0;
    __syncthreads();
    const int i = blockIdx.x * 256 + threadIdx.x;
    if (blockDim.x == 256 && i < w * h && labels[i] == i && comps[i].n >= 9)
        roots[atomicAdd(&count, 1)] = i;
    __syncthreads();
    for (int j = threadIdx.x / 32; j < count; j += 8)
        boxesWarp(roots[j], map, comps, w, h, sw, sh, tree, staged + (threadIdx.x & ~31));
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
    q.componentRoot = i;
    quads[index] = q;
}
// Optional immutable-input experiment. One bounded scan per selected component;
// labels stay intact because this mode supplies separate prefix-offset scratch.
// Launch contract: 128 blocks x 256 threads, one block per component. Fixed
// strided pixel ownership plus a fixed-order reduction keep counts/extrema
// exact and every sum defined; no atomics or extra transfers are introduced.
// `slots` bins are merged per round by threads 0..slots-1; every bin still
// receives the original fixed thread-order fold, so slots only changes how
// many rounds the block needs. One slot is the original kernel.
template <int slots> __global__ void fitRibbons(const int* labels, const Component* comps, int w, int h,
                          int sw, int sh, Quad* quads, const int* count, ribbon::StraightRule straight)
{
    const int index=blockIdx.x;
    if(blockDim.x!=256||index>=min(*count,128)) return;
    const int root=quads[index].componentRoot;
    if(root<0||root>=w*h) return;
    const auto c=comps[root];
    if(c.n>ribbon::kMaximumFitPixels||c.u1-c.u0<24||c.u1-c.u0<c.v1-c.v0) return;
    float minx=1e20f,miny=1e20f,maxx=-1e20f,maxy=-1e20f;
    for(int i=0;i<4;++i) {
        const float u=(i==0||i==3)?c.u0:c.u1, v=i<2?c.v0:c.v1;
        const float x=u*c.c-v*c.s,y=u*c.s+v*c.c;
        minx=fminf(minx,x);maxx=fmaxf(maxx,x);miny=fminf(miny,y);maxy=fmaxf(maxy,y);
    }
    const int x0=max(0,int(floorf(minx))),x1=min(w-1,int(ceilf(maxx)));
    const int y0=max(0,int(floorf(miny))),y1=min(h-1,int(ceilf(maxy)));
    const long long scanTotal=static_cast<long long>(x1-x0+1)*(y1-y0+1);
    if(scanTotal>ribbon::kMaximumFitPixels) return;
    const int scanWidth=x1-x0+1, scanPixels=static_cast<int>(scanTotal);
    ribbon::Bin mine[ribbon::kBins]{};
    for(int pixel=threadIdx.x;pixel<scanPixels;pixel+=blockDim.x) {
        const int x=x0+pixel%scanWidth,y=y0+pixel/scanWidth;
        if(labels[y*w+x]!=root) continue;
        const double u=x*double(c.c)+y*double(c.s),v=-x*double(c.s)+y*double(c.c);
        ribbon::Add(mine,(u-c.u0)/(c.u1-c.u0),v);
    }
    __shared__ int counts[256*slots];
    __shared__ double lows[256*slots],highs[256*slots],sums[256*slots];
    // Fundamental shared arrays need no device-side dynamic initialization.
    __shared__ int mergedCounts[ribbon::kBins];
    __shared__ double mergedLows[ribbon::kBins],mergedHighs[ribbon::kBins],mergedSums[ribbon::kBins];
    for(int base=0;base<ribbon::kBins;base+=slots) {
        for(int slot=0;slot<slots&&base+slot<ribbon::kBins;++slot) {
            const int bin=base+slot,at=slot*256+threadIdx.x;
            counts[at]=mine[bin].count; lows[at]=mine[bin].low;
            highs[at]=mine[bin].high; sums[at]=mine[bin].sumU;
        }
        __syncthreads();
        if(threadIdx.x<slots&&base+threadIdx.x<ribbon::kBins) {
            const int slot=threadIdx.x,bin=base+slot;
            ribbon::Bin merged{};
            for(int thread=0;thread<blockDim.x;++thread) {
                const int at=slot*256+thread;
                merged.count+=counts[at]; merged.sumU+=sums[at];
                if(lows[at]<merged.low) merged.low=lows[at];
                if(highs[at]>merged.high) merged.high=highs[at];
            }
            mergedCounts[bin]=merged.count; mergedLows[bin]=merged.low;
            mergedHighs[bin]=merged.high; mergedSums[bin]=merged.sumU;
        }
        __syncthreads();
    }
    if(threadIdx.x) return;
    ribbon::Bin bins[ribbon::kBins]{};
    for(int bin=0;bin<ribbon::kBins;++bin) {
        bins[bin].count=mergedCounts[bin]; bins[bin].low=mergedLows[bin];
        bins[bin].high=mergedHighs[bin]; bins[bin].sumU=mergedSums[bin];
    }
    auto& q=quads[index];
    q.ribbon=ribbon::Fit(bins,c.u0,c.u1,c.c,c.s,double(sw)/w,double(sh)/h,sw,sh,true,straight);
    if(q.ribbon.unsupported) {
        auto& r=q.ribbon;r.protectLeft=r.protectRight=q.xy[0];r.protectTop=r.protectBottom=q.xy[1];
        for(int i=1;i<4;++i){r.protectLeft=fmin(r.protectLeft,double(q.xy[2*i]));r.protectRight=fmax(r.protectRight,double(q.xy[2*i]));
            r.protectTop=fmin(r.protectTop,double(q.xy[2*i+1]));r.protectBottom=fmax(r.protectBottom,double(q.xy[2*i+1]));}
    }
    if(q.ribbon.valid) q.validWidth=max(1,int(ceil(48*q.ribbon.Width()/q.ribbon.Height())));
}
__global__ void recognitionWindows(const Quad* detected,int* count,Quad* windows)
{
    int written=0;
    for(int index=0;index<min(*count,128);++index) {
        const auto q=detected[index];
        windows[written++]=q;
        const float width=q.ribbon.valid?float(q.ribbon.Width()):hypotf(q.xy[2]-q.xy[0],q.xy[3]-q.xy[1]);
        const float height=q.ribbon.valid?float(q.ribbon.Height()):hypotf(q.xy[6]-q.xy[0],q.xy[7]-q.xy[1]);
        if(!isfinite(width) || !isfinite(height) || height<1 || width/height<=20)continue;
        const int n=min(8,max(2,int(ceilf((width/height-8)/12))));
        const float span=fmaxf(20,float((width/height+8*(n-1))/n));
        const float fraction=fminf(1,float(span*height/width));
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
            if(q.ribbon.valid) {
                window.ribbon.start=q.ribbon.start+(q.ribbon.end-q.ribbon.start)*start;
                window.ribbon.end=q.ribbon.start+(q.ribbon.end-q.ribbon.start)*end;
                window.validWidth=max(1,int(ceil(48*window.ribbon.Width()/window.ribbon.Height())));
            }
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
__global__ void rectify(const unsigned char *pixels, std::size_t pitch, int w, int h,
                        const Quad *quads, int index, float *out, int ow)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= ow * 48)
        return;
    const Quad& q = quads[index];
    const int x = i % ow, y = i / ow, valid = min(ow, q.validWidth);
    const float u = (x + .5f) / valid, v = (y + .5f) / 48;
    float sx, sy;
    if (q.ribbon.valid) {
        const auto p=q.ribbon.At(u,v); sx=float(p.x)-.5f; sy=float(p.y)-.5f;
    } else {
        sx = q.xy[0] * (1 - u) * (1 - v) + q.xy[2] * u * (1 - v) + q.xy[4] * u * v +
             q.xy[6] * (1 - u) * v - .5f;
        sy = q.xy[1] * (1 - u) * (1 - v) + q.xy[3] * u * (1 - v) + q.xy[5] * u * v +
             q.xy[7] * (1 - u) * v - .5f;
    }
    for (int c = 0; c < 3; ++c)
        out[c * ow * 48 + i] =
            x >= valid ? 0 : (sample(pixels, pitch, w, h, sx, sy, c) / 255 - .5f) * 2;
}
// Keep the scalar kernel above as the comparison oracle. Only sample selection
// and output base differ; the per-pixel expressions and channel loop are exact.
__global__ void rectifyBatch(const unsigned char *pixels, std::size_t pitch, int w, int h,
                             const Quad *quads, int first, float *out, int ow)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= ow * 48)
        return;
    const Quad& q = quads[first + blockIdx.y];
    out += static_cast<std::size_t>(blockIdx.y) * ow * 48 * 3;
    const int x = i % ow, y = i / ow, valid = min(ow, q.validWidth);
    const float u = (x + .5f) / valid, v = (y + .5f) / 48;
    float sx, sy;
    if (q.ribbon.valid) {
        const auto p=q.ribbon.At(u,v); sx=float(p.x)-.5f; sy=float(p.y)-.5f;
    } else {
        sx = q.xy[0] * (1 - u) * (1 - v) + q.xy[2] * u * (1 - v) + q.xy[4] * u * v +
             q.xy[6] * (1 - u) * v - .5f;
        sy = q.xy[1] * (1 - u) * (1 - v) + q.xy[3] * u * (1 - v) + q.xy[5] * u * v +
             q.xy[7] * (1 - u) * v - .5f;
    }
    for (int c = 0; c < 3; ++c)
        out[c * ow * 48 + i] =
            x >= valid ? 0 : (sample(pixels, pitch, w, h, sx, sy, c) / 255 - .5f) * 2;
}
__global__ void argmax(const float *probabilities, int steps, int classes, int *ids, float *scores)
{
    const int t = blockIdx.x;
    if (t >= steps)
        return;
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
__global__ void compact(int *ids, float *scores, int steps, const Quad *quads, int index,
                        int cropWidth, Token *tokens, int *count)
{
    if (threadIdx.x || blockIdx.x)
        return;
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
// Same 256-lane reduction as argmax, with a separate shared array per [N,T].
// Deliberately scans every T row: skipping padding is a separate experiment.
__global__ void argmaxBatch(const float *probabilities, int steps, int classes, int *ids, float *scores)
{
    const int t = blockIdx.x;
    probabilities += static_cast<std::size_t>(blockIdx.y) * steps * classes;
    ids += static_cast<std::size_t>(blockIdx.y) * steps;
    scores += static_cast<std::size_t>(blockIdx.y) * steps;
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
__global__ void compactBatch(int *ids, float *scores, int steps, const Quad *quads, int first,
                             int cropWidth, Token *tokens, int *counts)
{
    // Exactly one lane per sample, unchanged serial blank/repeat/confidence loop.
    const int sampleIndex = threadIdx.x;
    ids += static_cast<std::size_t>(sampleIndex) * steps;
    scores += static_cast<std::size_t>(sampleIndex) * steps;
    tokens += static_cast<std::size_t>(sampleIndex) * steps;
    int *count = counts + sampleIndex;
    const int index = first + sampleIndex;
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
            Quad *quads, int *count, cudaStream_t s, int* ribbonOffsets, DetectKernels kernels,
            ribbon::StraightRule straight)
{
    const int blocks = (w * h + 255) / 256;
    cudaMemsetAsync(count, 0, sizeof(int), s);
    init<<<blocks, 256, 0, s>>>(map, labels, comps, w * h);
    if (kernels.compressRoots)
    {
        connect<true><<<blocks, 256, 0, s>>>(labels, w, h);
        moments<true><<<blocks, 256, 0, s>>>(labels, comps, w, h);
    }
    else
    {
        connect<false><<<blocks, 256, 0, s>>>(labels, w, h);
        moments<false><<<blocks, 256, 0, s>>>(labels, comps, w, h);
    }
    axes<<<blocks, 256, 0, s>>>(comps, w * h);
    extents<<<blocks, 256, 0, s>>>(labels, comps, w, h);
    if (kernels.cooperativeBoxes || kernels.treeBoxes)
        boxesCooperative<<<blocks, 256, 0, s>>>(map, labels, comps, w, h, sw, sh, kernels.treeBoxes);
    else
        boxes<<<blocks, 256, 0, s>>>(map, labels, comps, w, h, sw, sh);
    auto* offsets=ribbonOffsets?ribbonOffsets:labels;
    countBoxes<<<blocks, 256, 0, s>>>(comps, w * h, offsets);
    boxOffsets<<<1, 1, 0, s>>>(offsets, blocks, count);
    emitBoxes<<<blocks, 256, 0, s>>>(comps, w, h, sw, sh, offsets, quads);
    if(ribbonOffsets) {
        if(kernels.parallelRibbonMerge) fitRibbons<4><<<128,256,0,s>>>(labels,comps,w,h,sw,sh,quads,count,straight);
        else fitRibbons<1><<<128,256,0,s>>>(labels,comps,w,h,sw,sh,quads,count,straight);
    }
}
void BoxSumGroups(const float *scores, const unsigned *masks, const float *values, int groups,
                  float *results, int *fast, cudaStream_t s)
{
    if (!scores || !masks || !values || !results || !fast || groups < 1 || groups > (1 << 22))
        throw std::invalid_argument("Unsupported box-sum group count.");
    boxSumGroups<<<groups, 32, 0, s>>>(scores, masks, values, results, fast, groups);
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
void RectifyBatch(const void *p, std::size_t pitch, int w, int h, const Quad *quads, int first,
                  int samples, float *out, int ow, cudaStream_t s)
{
    if (!p || !quads || !out || w < 1 || w > 8192 || h < 1 || h > 8192 ||
        pitch < static_cast<std::size_t>(w) * 4 || ow < 1 || ow > 1024 ||
        samples < 1 || samples > 16 || first < 0 || first > recognizer_width::kMaximumCrops - samples)
        throw std::invalid_argument("Unsupported batched rectification shape.");
    rectifyBatch<<<dim3((ow * 48 + 255) / 256, samples), 256, 0, s>>>(
        static_cast<const unsigned char *>(p), pitch, w, h, quads, first, out, ow);
}
void DecodeBatch(const float *p, int samples, int steps, int classes, const Quad *quads, int first,
                 int cropWidth, int *ids, float *scores, Token *tokens, int *counts, cudaStream_t s)
{
    // Same maximum tensor ceiling as the N16 worker. Check divisions before
    // multiplying; the scalar row index and consumed-width product stay int.
    constexpr std::size_t maximumFloats = 256ull * 1024 * 1024;
    if (!p || !quads || !ids || !scores || !tokens || !counts ||
        samples < 1 || samples > 16 || first < 0 || first > recognizer_width::kMaximumCrops - samples ||
        steps < 1 || classes < 1 || cropWidth < 1 || cropWidth > 1024 ||
        steps > std::numeric_limits<int>::max() / cropWidth ||
        static_cast<std::size_t>(steps) > maximumFloats / classes / samples)
        throw std::invalid_argument("Unsupported batched CTC shape.");
    argmaxBatch<<<dim3(steps, samples), 256, 0, s>>>(p, steps, classes, ids, scores);
    const auto launched = cudaGetLastError();
    if (launched != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(launched));
    compactBatch<<<1, samples, 0, s>>>(ids, scores, steps, quads, first, cropWidth, tokens, counts);
}
namespace
{
__device__ __forceinline__ unsigned long long globalNanoseconds()
{
    unsigned long long value;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(value));
    return value;
}
__global__ void keepWarm(const volatile int *stop, unsigned long long durationNs, unsigned sleepNs)
{
    const unsigned long long start = globalNanoseconds();
    if (sleepNs) {
        // Resident but mostly asleep: one flag read per nap.
        while (globalNanoseconds() - start < durationNs) {
            if (stop && *stop)
                break;
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ >= 700
            __nanosleep(sleepNs);
#endif
        }
        return;
    }
    // Spinning: the host-mapped flag is read once per 64 timer reads, about a
    // microsecond apart, not one PCIe read per loop iteration.
    for (unsigned polls = 0; globalNanoseconds() - start < durationNs; ++polls)
        if (stop && (polls & 63u) == 0 && *stop)
            break;
}
__global__ void sampleClock(ClockSample *out, unsigned long long durationNs)
{
    const unsigned long long start = globalNanoseconds();
    const long long cycles = clock64();
    unsigned long long now = start;
    while (now - start < durationNs)
        now = globalNanoseconds();
    const long long end = clock64();
    if (threadIdx.x == 0)
        *out = ClockSample{static_cast<unsigned long long>(end - cycles), now - start, start};
}
constexpr unsigned long long kMaximumSpinNs = 2000000000ull;
} // namespace
void KeepWarm(const volatile int *stop, unsigned long long durationNs, int blocks, cudaStream_t s, unsigned sleepNs)
{
    if (durationNs == 0 || durationNs > kMaximumSpinNs || blocks < 1 || blocks > 4096 || sleepNs > 1000000)
        throw std::invalid_argument("Unsupported keep-warm duration, block count or nap.");
    keepWarm<<<blocks, 32, 0, s>>>(stop, durationNs, sleepNs);
}
void SampleClock(ClockSample *out, unsigned long long durationNs, cudaStream_t s)
{
    if (!out || durationNs == 0 || durationNs > kMaximumSpinNs)
        throw std::invalid_argument("Unsupported clock sample.");
    sampleClock<<<1, 32, 0, s>>>(out, durationNs);
}
} // namespace okuflow::uct::gpu

#include "uct_validation_kernels.cuh"
#include "uct_ink_outline_kernels.cuh"
#include "uct_formula_kernels.cuh"
