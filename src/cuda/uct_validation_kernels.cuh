// First-party CUDA implementation of UCT's CPU validation/color/ink policy.
// Included by uct_kernels.cu so every CUDA consumer has the same primitives.
#include <cstdint>
#include "../common/uct_color_policy.hpp"
#include "../common/uct_surface_geometry.hpp"

namespace okuflow::uct::gpu
{
namespace validation
{
__device__ int luma(const unsigned char *p)
{
    return (29 * p[0] + 150 * p[1] + 77 * p[2]) >> 8;
}
__global__ void integralRows(const unsigned char *p, size_t pitch, int w, int h, int d,
                             unsigned *out)
{
    const int y = blockIdx.x, ow = w / d, oh = h / d;
    if (y >= oh)
        return;
    const int step = max(1, d / 2), per = (d + step - 1) / step;
    // One complete 256-thread block cooperates on a row. Integer prefix sums
    // retain the serial pass's exact table while adjacent lanes access adjacent
    // source/output pixels. Every lane participates, including zero-filled tails.
    __shared__ unsigned offsets[8], carry;
    const int lane = threadIdx.x & 31, warp = threadIdx.x / 32;
    if (!threadIdx.x) {
        carry = 0;
        out[(y + 1) * (ow + 1)] = 0;
    }
    __syncthreads();
    for (int tile = 0; tile < ow; tile += 256)
    {
        const int x = tile + threadIdx.x;
        int v = 0;
        if (x < ow)
            for (int a = 0; a < d; a += step)
                for (int b = 0; b < d; b += step)
                    v += luma(p + (y * d + a) * pitch + (x * d + b) * 4);
        unsigned prefix = v / (per * per);
        for (int delta = 1; delta < 32; delta *= 2) {
            const unsigned previous = __shfl_up_sync(0xffffffffu, prefix, delta);
            if (lane >= delta) prefix += previous;
        }
        if (lane == 31) offsets[warp] = prefix;
        __syncthreads();
        if (!threadIdx.x)
            for (int i = 0; i < 8; ++i) {
                const unsigned total = offsets[i];
                offsets[i] = carry;
                carry += total;
            }
        __syncthreads();
        if (x < ow) out[(y + 1) * (ow + 1) + x + 1] = offsets[warp] + prefix;
        // No lane may overwrite an offset before all lanes consumed this tile.
        __syncthreads();
    }
}
__global__ void integralColumns(unsigned *p, int w, int h)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    if (x > w)
        return;
    unsigned sum = 0;
    p[x] = 0;
    for (int y = 1; y <= h; ++y)
    {
        sum += p[y * (w + 1) + x];
        p[y * (w + 1) + x] = sum;
    }
}
__device__ double at(const unsigned *p, int w, int h, double x, double y)
{
    x = fmin(fmax(x, 0.), double(w));
    y = fmin(fmax(y, 0.), double(h));
    const int a = int(x), b = int(y), c = min(a + 1, w), d = min(b + 1, h);
    const double ax = x - a, ay = y - b;
    const double u = p[b * (w + 1) + a] + (double(p[b * (w + 1) + c]) - p[b * (w + 1) + a]) * ax;
    const double v = p[d * (w + 1) + a] + (double(p[d * (w + 1) + c]) - p[d * (w + 1) + a]) * ax;
    return u + (v - u) * ay;
}
__device__ bool box(const unsigned *p, int w, int h, double x, double y, double hx, double hy,
                    float &v)
{
    hx = fmax(hx, .5);
    hy = fmax(hy, .5);
    if (x - hx < -1e-7 || y - hy < -1e-7 || x + hx > w + 1e-7 || y + hy > h + 1e-7)
        return false;
    v = float((at(p, w, h, x + hx, y + hy) - at(p, w, h, x - hx, y + hy) -
               at(p, w, h, x + hx, y - hy) + at(p, w, h, x - hx, y - hy)) /
              (4 * hx * hy));
    return true;
}
__device__ void referenceBody(const unsigned *p, int w, int h, Grid g, float *values, int *valid)
{
    if (threadIdx.x)
        return;
    *valid = 0;
    double sum = 0, sq = 0;
    const int n = g.columns * g.rows;
    for (int i = 0; i < n; ++i)
    {
        float v;
        const int column=i%g.columns,row=i/g.columns;
        if (!box(p, w, h, g.x + column*g.dx + row*g.crossX + column*row*g.warpX,
                 g.y + row*g.dy + column*g.crossY + column*row*g.warpY, g.halfW,
                 g.halfH, v))
            return;
        values[i] = v;
        sum += v;
        sq += double(v) * v;
    }
    const double mean = sum / n, dev = sqrt(fmax(0., sq / n - mean * mean));
    if (dev < 6)
        return;
    for (int i = 0; i < n; ++i)
        values[i] = float((values[i] - mean) / dev);
    *valid = 1;
}
__global__ void reference(const unsigned *p, int w, int h, Grid g, float *values, int *valid)
{
    referenceBody(p, w, h, g, values, valid);
}
__global__ void references(const unsigned *p, int w, int h, const ReferenceRequest *requests,
                           int *valid)
{
    const auto r = requests[blockIdx.x];
    referenceBody(p, w, h, r.grid, r.samples, valid + blockIdx.x);
}
struct Correlation
{
    double score, visible;
};
__device__ Correlation correlate(const unsigned *p, int w, int h, Grid g, const float *ref,
                                 double dx, double dy, int stride)
{
    double a = 0, b = 0, aa = 0, bb = 0, ab = 0;
    int n = 0, total = 0;
    for (int i = 0; i < g.columns * g.rows; i += stride)
    {
        ++total;
        float v;
        const int column=i%g.columns,row=i/g.columns;
        if (!box(p, w, h, g.x + column*g.dx + row*g.crossX + column*row*g.warpX + dx,
                 g.y + row*g.dy + column*g.crossY + column*row*g.warpY + dy,
                 g.halfW, g.halfH, v))
            continue;
        ++n;
        const double u = ref[i];
        a += u;
        b += v;
        aa += u * u;
        bb += double(v) * v;
        ab += u * v;
    }
    Correlation c{-1., total ? double(n) / total : 0.};
    if (n < 8)
        return c;
    const double va = aa - a * a / n, vb = bb - b * b / n;
    c.score = va <= 1e-9 || vb <= n * 9. ? 0. : (ab - a * b / n) / sqrt(va * vb);
    return c;
}
__device__ void matchBody(const unsigned *p, int w, int h, Grid g, const float *ref, double score,
                          double visible, int radius, double seedX, double seedY, double sceneX,
                          double sceneY, MatchResult *out)
{
    __shared__ double best, bx, by, sx, sy;
    __shared__ int done;
    __shared__ Correlation candidates[128];
    const int t = threadIdx.x;
    if (!t)
    {
        *out = {};
        const auto z = correlate(p, w, h, g, ref, 0, 0, 1);
        out->visible = z.visible >= visible;
        best = z.score;
        bx = by = sx = sy = 0;
        done = !out->visible || radius == 0 || best >= score + .08;
        if (!done && (seedX != 0 || seedY != 0) && fabs(seedX) <= radius && fabs(seedY) <= radius)
        {
            const auto c = correlate(p, w, h, g, ref, seedX, seedY, 1);
            if (c.visible >= visible && c.score >= score + .08)
            {
                best = c.score;
                bx = sx = seedX;
                by = sy = seedY;
                done = 2;
            }
        }
    }
    __syncthreads();
    if (!done)
    {
        const int axis = radius + 1, n = axis * axis;
        if (t < n)
            candidates[t] =
                correlate(p, w, h, g, ref, -radius + (t % axis) * 2, -radius + (t / axis) * 2, 3);
        __syncthreads();
        if (!t)
        {
            double coarse = -1;
            int index = -1;
            for (int i = 0; i < n; ++i)
                if (candidates[i].visible >= visible && candidates[i].score > coarse)
                {
                    coarse = candidates[i].score;
                    index = i;
                }
            if (index >= 0)
            {
                sx = -radius + (index % axis) * 2;
                sy = -radius + (index / axis) * 2;
            }
        }
    }
    __syncthreads();
    if (!done || done == 2)
    {
        if (t < 9)
            candidates[t] = correlate(p, w, h, g, ref, sx + (t % 3) - 1, sy + (t / 3) - 1, 1);
        __syncthreads();
        if (!t)
        { // Dense centre precedes neighbours, preserving CPU tie behavior.
            const int order[9]{4, 0, 1, 2, 3, 5, 6, 7, 8};
            for (int k = 0; k < 9; ++k)
            {
                int i = order[k];
                double x = sx + (i % 3) - 1, y = sy + (i / 3) - 1;
                if (fabs(x) <= radius + 1 && fabs(y) <= radius + 1 &&
                    candidates[i].visible >= visible && candidates[i].score > best)
                {
                    best = candidates[i].score;
                    bx = x;
                    by = y;
                }
            }
        }
        __syncthreads();
        if (t < 4)
            candidates[t] = correlate(p, w, h, g, ref,
                                      bx + (t == 0   ? -1
                                            : t == 1 ? 1
                                                     : 0),
                                      by + (t == 2   ? -1
                                            : t == 3 ? 1
                                                     : 0),
                                      1);
        __syncthreads();
        if (!t)
        {
            double sub[2]{};
            for (int a = 0; a < 2; ++a)
            {
                auto l = candidates[a * 2], r = candidates[a * 2 + 1];
                const double den = l.score - 2 * best + r.score;
                if (l.visible >= visible && r.visible >= visible && l.score > -1 && r.score > -1 &&
                    den < -1e-9)
                    sub[a] = fmin(.5, fmax(-.5, .5 * (l.score - r.score) / den));
            }
            const auto c = correlate(p, w, h, g, ref, bx + sub[0], by + sub[1], 1);
            if (c.visible >= visible && c.score >= best)
            {
                best = c.score;
                bx += sub[0];
                by += sub[1];
            }
        }
    }
    __syncthreads();
    if (!t && out->visible)
    {
        out->score = best;
        out->matched = best >= score;
        out->x = bx * sceneX;
        out->y = by * sceneY;
    }
}
__global__ void match(const unsigned *p, int w, int h, Grid g, const float *ref, double score,
                      double visible, int radius, double seedX, double seedY, double sceneX,
                      double sceneY, MatchResult *out)
{
    matchBody(p, w, h, g, ref, score, visible, radius, seedX, seedY, sceneX, sceneY, out);
}
__global__ void matches(const unsigned *p, int w, int h, const MatchRequest *requests, double score,
                        double visible, int radius, MatchResult *out)
{
    const auto r = requests[blockIdx.x];
    matchBody(p, w, h, r.grid, r.samples, score, visible, radius, r.seedX, r.seedY, r.sceneX,
              r.sceneY, out + blockIdx.x);
}
template<bool regional = false>
__global__ void similarity(const unsigned *a, int aw, int ah, ProbeMap ma, const unsigned *b,
                           int bw, int bh, ProbeMap mb, double x, double y, double w, double h,
                           double *out)
{
    constexpr int columns = regional ? 12 : 48, rows = regional ? 8 : 27;
    if constexpr (regional) {
        w /= 8; h /= 8;
        x += (blockIdx.x % 8) * w; y += (blockIdx.x / 8) * h;
        out += blockIdx.x;
    }
    // One block, per-thread correlations then a bounded block reduction.
    __shared__ double sums[6][128];
    const int t = threadIdx.x;
    double u = 0, v = 0, uu = 0, vv = 0, uv = 0, n = 0;
    for (int i = t; i < columns * rows; i += 128)
    {
        float p, q;
        double sx = x + (i % columns + .5) * w / columns, sy = y + (i / columns + .5) * h / rows;
        if (!box(a, aw, ah, sx * ma.sx + ma.x, sy * ma.sy + ma.y, .5 * w / columns * ma.sx,
                 .5 * h / rows * ma.sy, p) ||
            !box(b, bw, bh, sx * mb.sx + mb.x, sy * mb.sy + mb.y, .5 * w / columns * mb.sx,
                 .5 * h / rows * mb.sy, q))
            continue;
        u += p;
        v += q;
        uu += double(p) * p;
        vv += double(q) * q;
        uv += double(p) * q;
        ++n;
    }
    sums[0][t] = u;
    sums[1][t] = v;
    sums[2][t] = uu;
    sums[3][t] = vv;
    sums[4][t] = uv;
    sums[5][t] = n;
    __syncthreads();
    for (int d = 64; d; d /= 2)
    {
        if (t < d)
            for (int k = 0; k < 6; ++k)
                sums[k][t] += sums[k][t + d];
        __syncthreads();
    }
    if (!t)
    {
        u = sums[0][0];
        v = sums[1][0];
        uu = sums[2][0];
        vv = sums[3][0];
        uv = sums[4][0];
        n = sums[5][0];
        const double va = uu - u * u / fmax(n, 1.), vb = vv - v * v / fmax(n, 1.);
        *out = n < 64 || (regional && (va < n * 16 || vb < n * 16)) ? 2.
                      : (va < n * 16 && vb < n * 16
                             ? (fabs(u / n - v / n) < 8 ? 1. : 0.)
                             : (va <= 1e-9 || vb <= 1e-9 ? 0. : (uv - u * v / n) / sqrt(va * vb)));
    }
}
// Shared floor/bilinear sampling for local colors and oriented ink. Explicit
// non-fused operations retain the CPU grid at integer pixel boundaries.
__device__ const unsigned char* orientedPixel(const unsigned char* p, size_t pitch, int w, int h,
                                             const OrientedInk& a, double du, double dv)
{
    const double f = __dmul_rn(__ddiv_rn(du, a.width), __ddiv_rn(dv, a.height));
    const double px = __dadd_rn(__dadd_rn(__dadd_rn(a.ox, __dmul_rn(du, a.ux)), __dmul_rn(dv, a.vx)), __dmul_rn(a.warpX, f));
    const double py = __dadd_rn(__dadd_rn(__dadd_rn(a.oy, __dmul_rn(du, a.uy)), __dmul_rn(dv, a.vy)), __dmul_rn(a.warpY, f));
    if (!(px >= 0 && py >= 0 && px < w && py < h)) return nullptr;
    return p + size_t(floor(py)) * pitch + size_t(floor(px)) * 4;
}
__global__ void colors(const unsigned char *p, size_t pitch, int w, int h, const Rect *rects,
                       Colors *out, const OrientedInk* frames)
{
    using namespace color_policy;
    const int t = threadIdx.x, index = blockIdx.x;
    const OrientedInk frame = frames ? frames[index] : OrientedInk{};
    const bool oriented = frame.width > 0;
    const auto b = oriented ? Rect{0, 0, int(ceil(frame.width)), int(ceil(frame.height))} : rects[index];
    const auto pixel = [&](int x, int y) {
        if (oriented) return orientedPixel(p, pitch, w, h, frame, double(x), double(y));
        return x >= 0 && y >= 0 && x < w && y < h ? p + size_t(y) * pitch + size_t(x) * 4 : nullptr;
    };
    // Fixed 9.5KiB shared scratch per word/line; hist is reused for body,
    // qualified ring, then ink endpoint. Pixels never leave resident storage.
    __shared__ unsigned hist[2][3][256], lumaHist[2][256], reduction[256], validCounts[256];
    __shared__ unsigned bodyCounts[2], ringCounts[2], sideCounts[4][2], sideTotals[4], cornerVotes[2];
    __shared__ int threshold, endpoint[2], fg, adjacentBackground;
    __shared__ Rgb body[2];
    __shared__ Colors measured;
    for (int i = t; i < 1536; i += 256)
        reinterpret_cast<unsigned *>(hist)[i] = 0;
    for (int i = t; i < 512; i += 256)
        reinterpret_cast<unsigned *>(lumaHist)[i] = 0;
    if (t < 2) bodyCounts[t] = ringCounts[t] = cornerVotes[t] = 0;
    if (t < 8) reinterpret_cast<unsigned *>(sideCounts)[t] = 0;
    if (t < 4) sideTotals[t] = 0;
    unsigned sum = 0, valid = 0;
    const int bodyStep = BodySampleStep(b.w, b.h);
    const int nx = (b.w + bodyStep - 1) / bodyStep, ny = (b.h + bodyStep - 1) / bodyStep;
    for (int i = t; i < nx * ny; i += 256) {
        const auto* px = pixel(b.x + (i % nx) * bodyStep, b.y + (i / nx) * bodyStep);
        if (px) { sum += luma(px); ++valid; }
    }
    reduction[t] = sum;
    validCounts[t] = valid;
    __syncthreads();
    for (int d = 128; d; d /= 2) {
        if (t < d) { reduction[t] += reduction[t + d]; validCounts[t] += validCounts[t + d]; }
        __syncthreads();
    }
    if (!t) {
        threshold = validCounts[0] ? int(reduction[0] / validCounts[0]) : 0;
        measured = {{0, 0, 0}, {255, 255, 255}};
    }
    __syncthreads();
    for (int i = t; i < nx * ny; i += 256) {
        const auto *px = pixel(b.x + (i % nx) * bodyStep, b.y + (i / nx) * bodyStep);
        if (!px) continue;
        const int value = luma(px), side = value < threshold ? 0 : 1;
        atomicAdd(bodyCounts + side, 1u);
        atomicAdd(&lumaHist[side][value], 1u);
        for (int c = 0; c < 3; ++c) atomicAdd(&hist[side][c][px[2 - c]], 1u);
    }
    __syncthreads();
    if (!bodyCounts[0] || !bodyCounts[1]) {
        if (!t) out[index] = measured;
        return;
    }
    if (!t) {
        for (int side = 0; side < 2; ++side) {
            for (int c = 0; c < 3; ++c) body[side].channel[c] = Median(hist[side][c], bodyCounts[side]);
            endpoint[side] = Endpoint(lumaHist[side], bodyCounts[side], side);
        }
        if (b.w >= 3 && b.h >= 3)
            for (int corner = 0; corner < 4; ++corner) {
                const int left = corner % 2 ? b.x + b.w - 3 : b.x;
                const int top = corner / 2 ? b.y + b.h - 3 : b.y;
                unsigned matches[2]{};
                for (int yy = top; yy < top + 3; ++yy)
                    for (int xx = left; xx < left + 3; ++xx) {
                        const auto *px = pixel(xx, yy);
                        if (!px) continue;
                        const int side = luma(px) < threshold ? 0 : 1;
                        if (Compatible(px, body[side])) ++matches[side];
                    }
                for (int side = 0; side < 2; ++side)
                    if (matches[side] >= 5) ++cornerVotes[side];
            }
    }
    __syncthreads();
    for (int i = t; i < 1536; i += 256) reinterpret_cast<unsigned *>(hist)[i] = 0;
    __syncthreads();
    const Rect adjacent[4]{
        {b.x, b.y - 1, b.w, oriented || b.y > 0 ? 1 : 0},
        {b.x, b.y + b.h, b.w, oriented || b.y + b.h < h ? 1 : 0},
        {b.x - 1, b.y, oriented || b.x > 0 ? 1 : 0, b.h},
        {b.x + b.w, b.y, oriented || b.x + b.w < w ? 1 : 0, b.h}};
    for (int s = 0; s < 4; ++s) {
        const auto r = adjacent[s];
        const int sw = (r.w + 1) / 2, sh = (r.h + 1) / 2;
        for (int i = t; i < sw * sh; i += 256) {
            const auto *px = pixel(r.x + (i % sw) * 2, r.y + (i / sw) * 2);
            if (!px) continue;
            const int side = luma(px) < threshold ? 0 : 1;
            atomicAdd(sideTotals + s, 1u);
            if (!Compatible(px, body[side])) continue;
            atomicAdd(&sideCounts[s][side], 1u);
            atomicAdd(ringCounts + side, 1u);
            for (int c = 0; c < 3; ++c) atomicAdd(&hist[side][c][px[2 - c]], 1u);
        }
    }
    __syncthreads();
    if (!t) {
        adjacentBackground = AdjacentBackground(sideCounts, sideTotals);
        if (adjacentBackground >= 0) {
            fg = 1 - adjacentBackground;
            for (int c = 0; c < 3; ++c)
                measured.background[c] = Median(hist[adjacentBackground][c], ringCounts[adjacentBackground]);
        }
    }
    __syncthreads();
    // The branch is block-uniform. Reuse the same histogram/count scratch;
    // successful immediate consensus avoids the expanded strip pass entirely.
    if (adjacentBackground < 0) {
        for (int i = t; i < 1536; i += 256) reinterpret_cast<unsigned *>(hist)[i] = 0;
        if (t < 2) ringCounts[t] = 0;
        if (t < 8) reinterpret_cast<unsigned *>(sideCounts)[t] = 0;
        if (t < 4) sideTotals[t] = 0;
        __syncthreads();
        const int m = min(12, max(2, b.h / 6));
        const int x0 = oriented ? b.x - m : max(0, b.x - m);
        const int y0 = oriented ? b.y - m : max(0, b.y - m);
        const int x1 = oriented ? b.x + b.w + m : min(w, b.x + b.w + m);
        const int y1 = oriented ? b.y + b.h + m : min(h, b.y + b.h + m);
        const Rect sides[4]{
            {b.x, y0, b.w, b.y - y0},
            {b.x, b.y + b.h, b.w, y1 - b.y - b.h},
            {x0, b.y, b.x - x0, b.h},
            {b.x + b.w, b.y, x1 - b.x - b.w, b.h}};
        for (int s = 0; s < 4; ++s) {
            const auto r = sides[s];
            const int sw = (r.w + 1) / 2, sh = (r.h + 1) / 2;
            for (int i = t; i < sw * sh; i += 256) {
                const auto *px = pixel(r.x + (i % sw) * 2, r.y + (i / sw) * 2);
                if (!px) continue;
                const int side = luma(px) < threshold ? 0 : 1;
                atomicAdd(sideTotals + s, 1u);
                if (!Compatible(px, body[side])) continue;
                atomicAdd(&sideCounts[s][side], 1u);
                atomicAdd(ringCounts + side, 1u);
                for (int c = 0; c < 3; ++c) atomicAdd(&hist[side][c][px[2 - c]], 1u);
            }
        }
        __syncthreads();
        if (!t) {
            bool useRing = false;
            const int bg = Background(bodyCounts, sideCounts, sideTotals, cornerVotes, useRing);
            fg = 1 - bg;
            for (int c = 0; c < 3; ++c)
                measured.background[c] = useRing && ringCounts[bg] >= 8
                    ? Median(hist[bg][c], ringCounts[bg]) : body[bg].channel[c];
        }
    }
    __syncthreads();
    for (int i = t; i < 1536; i += 256) reinterpret_cast<unsigned *>(hist)[i] = 0;
    if (!t) ringCounts[0] = 0;
    __syncthreads();
    for (int i = t; i < nx * ny; i += 256) {
        const auto *px = pixel(b.x + (i % nx) * bodyStep, b.y + (i / nx) * bodyStep);
        if (!px) continue;
        const int value = luma(px);
        if ((value < threshold ? 0 : 1) != fg ||
            (fg == 0 ? value > endpoint[fg] : value < endpoint[fg])) continue;
        atomicAdd(ringCounts, 1u);
        for (int c = 0; c < 3; ++c) atomicAdd(&hist[fg][c][px[2 - c]], 1u);
    }
    __syncthreads();
    if (!t) {
        for (int c = 0; c < 3; ++c) measured.foreground[c] = Median(hist[fg][c], ringCounts[0]);
        out[index] = measured;
    }
}
__device__ bool substantialInkComponent(const unsigned* ink, int columns, int rows, int seed);
__global__ void ink(const unsigned char *p, size_t pitch, int w, int h, const Ink *words,
                    const int *lines, const Colors *cs, Fit *out)
{
    const int i = blockIdx.x, t = threadIdx.x;
    const auto a = words[i];
    const auto b = a.search;
    const auto c = cs[lines[i]];
    __shared__ unsigned inkBits[1024], reach[6554];
    __shared__ int rows[4096], total, top, bottom, left, right, threshold, fringeThreshold,
        polarity, step, usable, changed, complete;
    if (!t)
    {
        out[i] = {a.x, a.y, a.w, a.h};
        total = complete = 0;
        top = bottom = 0;
        left = b.x + b.w;
        right = b.x - 1;
        int fg = (77 * c.foreground[0] + 150 * c.foreground[1] + 29 * c.foreground[2]) / 256;
        int bg = (77 * c.background[0] + 150 * c.background[1] + 29 * c.background[2]) / 256;
        usable = abs(fg - bg) >= 24 && b.w > 0 && b.h > 0;
        threshold = (fg + bg) / 2;
        fringeThreshold = (threshold + bg) / 2;
        polarity = fg < bg;
        step = max(1, int(ceil(sqrt(double(b.w) * b.h / 16384))));
        usable = usable && b.w <= 8192 && b.h <= 4096 &&
            ((b.w + step - 1) / step) * ((b.h + step - 1) / step) <= 32768;
    }
    __syncthreads();
    if (!usable)
        return;
    const int nx = (b.w + step - 1) / step, ny = (b.h + step - 1) / step;
    for (int k = t; k < 1024; k += 256) inkBits[k] = 0;
    for (int k = t; k < 6554; k += 256) reach[k] = 0;
    __syncthreads();
    for (int k = t; k < nx * ny; k += 256) {
        const int u = k % nx, v = k / nx;
        const int x = b.x + u * step, y = b.y + v * step;
        const int value = luma(p + size_t(y) * pitch + size_t(x) * 4);
        if (polarity ? value >= fringeThreshold : value <= fringeThreshold) continue;
        atomicOr(inkBits + k / 32, 1u << (k % 32));
        const bool core = polarity ? value < threshold : value > threshold;
        const bool withinX = x + .5 >= a.x && x + .5 < a.x + a.w;
        const bool withinY = y + .5 >= a.y && y + .5 < a.y + a.h;
        const double margin = a.detached ? fmax(2., a.h * .22) : 0.;
        const bool mark = core && withinX && a.detached &&
            y + .5 >= a.y - margin && y + .5 < a.y + a.h + margin;
        const unsigned anchor = (core && withinX && withinY) || mark ? 16u : 0u;
        const unsigned detectorSupport = withinX && withinY ? 32u : 0u;
        const unsigned labels = (!u ? 1u : 0u) | (u == nx - 1 ? 2u : 0u) |
            (!v ? 4u : 0u) | (v == ny - 1 ? 8u : 0u) | anchor | detectorSupport;
        atomicOr(reach + k / 5, labels << ((k % 5) * 6));
    }
    __syncthreads();
    // Same fixed bound as the oriented path. Incomplete propagation retains
    // the complete detector, never partially filtered word coordinates.
    for (int pass = 0; pass < kOrientedInkMaxPropagationPasses; ++pass) {
        if (!t) changed = 0;
        __syncthreads();
        for (int v = t; v < ny; v += 256) {
            int u = 0;
            while (u < nx) {
                const int start = u;
                unsigned labels = 0;
                while (u < nx) {
                    const int k = v * nx + u;
                    if (!(inkBits[k / 32] & (1u << (k % 32)))) break;
                    labels |= (atomicOr(reach + k / 5, 0u) >> ((k % 5) * 6)) & 63u;
                    for (int dy = -1; dy <= 1; dy += 2) for (int dx = -1; dx <= 1; ++dx) {
                        const int x = u + dx, y = v + dy;
                        if (x < 0 || y < 0 || x >= nx || y >= ny) continue;
                        const int n = y * nx + x;
                        labels |= (atomicOr(reach + n / 5, 0u) >> ((n % 5) * 6)) & 63u;
                    }
                    ++u;
                }
                if (labels) for (int x = start; x < u; ++x) {
                    const int k = v * nx + x;
                    const unsigned bits = labels << ((k % 5) * 6);
                    if ((atomicOr(reach + k / 5, bits) & bits) != bits) atomicExch(&changed, 1);
                }
                ++u;
            }
        }
        __syncthreads();
        const bool again = changed != 0;
        __syncthreads();
        if (!again) { if (!t) complete = 1; break; }
    }
    __syncthreads();
    if (!complete) return;
    for (int k = t; k < nx * ny; k += 256) {
        if (!(inkBits[k / 32] & (1u << (k % 32)))) continue;
        const unsigned labels = (reach[k / 5] >> ((k % 5) * 6)) & 63u;
        const bool spanning = (labels & 3) == 3 || (labels & 12) == 12;
        if (spanning && (labels & 16u) && (labels & 32u)) atomicExch(&usable, 0);
        if (!spanning && !(labels & 16u) && (labels & 32u) &&
            substantialInkComponent(inkBits, nx, ny, k)) atomicExch(&usable, 0);
    }
    __syncthreads();
    if (!usable) return;
    const auto inkAt = [&](int k) {
        const unsigned labels = (reach[k / 5] >> ((k % 5) * 6)) & 63u;
        return (inkBits[k / 32] & (1u << (k % 32))) && (labels & 16u) &&
            (labels & 3) != 3 && (labels & 12) != 12;
    };

    for (int y = t; y < b.h; y += 256) rows[y] = 0;
    __syncthreads();
    for (int k = t; k < nx * ny; k += 256)
    {
        int y = b.y + (k / nx) * step;
        if (!inkAt(k))
            continue;
        atomicAdd(rows + y - b.y, 1);
        atomicAdd(&total, 1);
    }
    __syncthreads();
    if (!t)
    {
        int mt = 0, mb = 0, mass = 0, rt = -1, rb = 0, rm = 0;
        for (int y = 0; y <= b.h; ++y)
        {
            if (y < b.h && rows[y])
            {
                if (rt < 0)
                    rt = y;
                else if (y - rb > step * 2)
                {
                    if (rm > mass)
                    {
                        mt = rt;
                        mb = rb;
                        mass = rm;
                    }
                    rt = y;
                    rm = 0;
                }
                rb = y;
                rm += rows[y];
            }
        }
        if (rm > mass)
        {
            mt = rt;
            mb = rb;
            mass = rm;
        }
        usable = total >= 8 && mass >= total * .55;
        top = mt;
        bottom = mb;
        if (usable && a.detached)
        {
            rt = -1;
            rm = 0;
            for (int y = 0; y <= b.h; ++y)
            {
                if (y < b.h && !rows[y])
                    continue;
                if (y < b.h && (rt < 0 || y - rb <= step * 2))
                {
                    if (rt < 0)
                        rt = y;
                    rb = y;
                    rm += rows[y];
                    continue;
                }
                if (rt >= 0 && rt != mt && rm <= mass * .3 &&
                    max(max(top - rb, rt - bottom), 0) <= fmax(2., a.h * .22))
                {
                    top = min(top, rt);
                    bottom = max(bottom, rb);
                }
                rt = y < b.h ? y : -1;
                rb = y;
                rm = rt >= 0 ? rows[y] : 0;
            }
        }
        top += b.y;
        bottom += b.y;
    }
    __syncthreads();
    if (!usable)
        return;
    for (int k = t; k < nx * ny; k += 256)
    {
        int x = b.x + (k % nx) * step, y = b.y + (k / nx) * step;
        if (y < top || y > bottom)
            continue;
        if (!inkAt(k))
            continue;
        atomicMin(&left, x);
        atomicMax(&right, x);
    }
    __syncthreads();
    if (!t)
    {
        double ww = min(b.x + b.w, right + step) - left, hh = min(b.y + b.h, bottom + step) - top;
        if (ww >= a.w * .35 && hh >= a.h * .35)
            out[i] = {double(left), double(top), ww, hh};
    }
}

// Rotated-frame ink fit. Mirrors FitOrientedWordInk exactly: identical step,
// grid, nearest-pixel floor sampling (non-fused double arithmetic), run
// selection, detached-mark rule and bounded shrink guards. One block per word.
__device__ bool orientedInkAt(const unsigned char *p, size_t pitch, int w, int h,
                              const OrientedInk &a, int step, int iu, int iv, int threshold, int polarity)
{
    const double du = __dmul_rn(double(iu), double(step)), dv = __dmul_rn(double(iv), double(step));
    const auto* px = orientedPixel(p, pitch, w, h, a, du, dv);
    if (!px) return false;
    const int v = luma(px);
    return polarity ? v < threshold : v > threshold;
}
__device__ bool substantialInkComponent(const unsigned* ink, int columns, int rows, int seed)
{
    // Match the CPU's existing eight-sample minimum. Isolated AA fragments
    // cannot veto a fit; a complete unanchored stroke must preserve the word.
    int nodes[8]; nodes[0] = seed;
    int count = 1;
    for (int next = 0; next < count && count < 8; ++next) {
        const int x = nodes[next] % columns, y = nodes[next] / columns;
        for (int dy = -1; dy <= 1 && count < 8; ++dy) for (int dx = -1; dx <= 1 && count < 8; ++dx) {
            const int u = x + dx, v = y + dy;
            if (u < 0 || v < 0 || u >= columns || v >= rows) continue;
            const int index = v * columns + u;
            if (!(ink[index / 32] & (1u << (index % 32)))) continue;
            bool seen = false;
            for (int i = 0; i < count; ++i) seen = seen || nodes[i] == index;
            if (!seen) nodes[count++] = index;
        }
    }
    return count == 8;
}
__global__ void orientedInk(const unsigned char *p, size_t pitch, int w, int h, const OrientedInk *words,
                            const int *lines, const Colors *cs, OrientedFit *out)
{
    const int i = blockIdx.x, t = threadIdx.x;
    const auto a = words[i];
    const auto c = cs[lines[i]];
    // One ink bit and six reachability bits per cell plus row masses: ~46KiB.
    // Only opposite-edge-spanning components are exterior. Retain legitimate
    // glyphs/dots touching a single search edge or corner.
    __shared__ unsigned inkBits[1024], reach[6554];
    __shared__ int rows[4096], changed, total, top, bottom, left, right, threshold, polarity, step, usable, columns, count;
    if (!t)
    {
        out[i] = {0, 0, 0, 0, 0};
        total = 0;
        left = 1 << 30;
        right = -1;
        const int fg = (77 * c.foreground[0] + 150 * c.foreground[1] + 29 * c.foreground[2]) / 256;
        const int bg = (77 * c.background[0] + 150 * c.background[1] + 29 * c.background[2]) / 256;
        usable = abs(fg - bg) >= 24 && a.width >= 1 && a.height >= 1 && isfinite(a.width) && isfinite(a.height);
        threshold = (fg + bg) / 2;
        polarity = fg < bg;
        step = 1;
        columns = count = 0;
        if (usable)
        {
            step = max(max(1, int(ceil(sqrt(a.width * a.height / 16384.0)))),
                       max(int(ceil(a.height / 4096.0)), int(ceil(a.width / 8192.0))));
            columns = int(ceil(a.width / step));
            count = int(ceil(a.height / step));
            usable = columns > 0 && count > 0 && count <= 4096 && columns <= 8192 && columns * count <= 32768;
        }
    }
    __syncthreads();
    if (!usable)
        return;
    for (int k = t; k < 1024; k += 256) inkBits[k] = 0;
    for (int k = t; k < 6554; k += 256) reach[k] = 0;
    __syncthreads();
    for (int k = t; k < columns * count; k += 256) {
        const int iu = k % columns, iv = k / columns;
        if (!orientedInkAt(p, pitch, w, h, a, step, iu, iv, threshold, polarity)) continue;
        atomicOr(inkBits + k / 32, 1u << (k % 32));
        const unsigned sides = (!iu ? 1u : 0u) | (iu == columns - 1 ? 2u : 0u) |
                               (!iv ? 4u : 0u) | (iv == count - 1 ? 8u : 0u);
        const auto* px = orientedPixel(p, pitch, w, h, a, double(iu) * step, double(iv) * step);
        const color_policy::Rgb ink{{c.foreground[0], c.foreground[1], c.foreground[2]}};
        const unsigned labels = sides | (color_policy::Compatible(px, ink) ? 16u : 0u) |
                                (iu && iv && iu < columns - 1 && iv < count - 1 ? 32u : 0u);
        atomicOr(reach + k / 5, labels << ((k % 5) * 6));
    }
    __syncthreads();
    // Propagate whole horizontal runs per worker, including diagonal links in
    // adjacent rows. Every side label advances at least one graph edge per
    // pass; N cells is the mathematical bound. Enforce a much smaller fixed
    // work cap; incomplete labels must never supply a partially pruned fit.
    if (!t) out[i].reason = OrientedFitReason::Incomplete;
    for (int pass = 0; pass < kOrientedInkMaxPropagationPasses; ++pass) {
        if (!t) changed = 0;
        __syncthreads();
        for (int iv = t; iv < count; iv += 256) {
            int iu = 0;
            while (iu < columns) {
                const int start = iu;
                unsigned sides = 0;
                while (iu < columns) {
                    const int k = iv * columns + iu;
                    if (!(inkBits[k / 32] & (1u << (k % 32)))) break;
                    sides |= (atomicOr(reach + k / 5, 0u) >> ((k % 5) * 6)) & 63u;
                    for (int dy = -1; dy <= 1; dy += 2) for (int dx = -1; dx <= 1; ++dx) {
                        const int x = iu + dx, y = iv + dy;
                        if (x < 0 || y < 0 || x >= columns || y >= count) continue;
                        const int n = y * columns + x;
                        sides |= (atomicOr(reach + n / 5, 0u) >> ((n % 5) * 6)) & 63u;
                    }
                    ++iu;
                }
                if (sides) for (int x = start; x < iu; ++x) {
                    const int k = iv * columns + x;
                    const unsigned bits = sides << ((k % 5) * 6);
                    if ((atomicOr(reach + k / 5, bits) & bits) != bits) atomicExch(&changed, 1);
                }
                ++iu;
            }
        }
        __syncthreads();
        const bool again = changed != 0;
        __syncthreads(); // Every thread reads before thread zero resets it.
        if (!t) {
            out[i].propagationPasses = pass + 1;
            out[i].propagationComplete = !again;
        }
        if (!again) break;
    }
    __syncthreads();
    if (!out[i].propagationComplete) return;
    if (!t) out[i].reason = OrientedFitReason::UnanchoredInk;
    for (int k = t; k < columns * count; k += 256) {
        if (!(inkBits[k / 32] & (1u << (k % 32)))) continue;
        const unsigned labels = (reach[k / 5] >> ((k % 5) * 6)) & 63u;
        const unsigned sides = labels & 15u;
        const bool compatible = labels & 16u;
        const bool exterior = (sides & 3) == 3 || (sides & 12) == 12 ||
                              (!compatible && (sides & (sides - 1)) != 0);
        if (!exterior && !compatible && sides != 0 && (labels & 32u) && substantialInkComponent(inkBits, columns, count, k))
            atomicExch(&usable, 0);
    }
    __syncthreads();
    if (!usable) return; // An unanchored interior stroke may be a real glyph.
    const auto inkAt = [&](int iu, int iv) {
        const int k = iv * columns + iu;
        const unsigned bit = 1u << (k % 32);
        const unsigned labels = (reach[k / 5] >> ((k % 5) * 6)) & 63u;
        const unsigned sides = labels & 15u;
        return (inkBits[k / 32] & bit) && ((labels & 16u) || sides == 0) &&
               (sides & 3) != 3 && (sides & 12) != 12;
    };
    for (int y = t; y < count; y += 256)
        rows[y] = 0;
    __syncthreads();
    for (int k = t; k < columns * count; k += 256)
    {
        if (!inkAt(k % columns, k / columns))
            continue;
        atomicAdd(rows + k / columns, 1);
        atomicAdd(&total, 1);
    }
    __syncthreads();
    if (!t)
    {
        usable = total >= 8;
        out[i].reason = usable ? OrientedFitReason::AmbiguousBands : OrientedFitReason::TooFewSamples;
        // Runs exactly as the CPU builds them: empty rows are skipped and an
        // ink row more than two sample rows below the run starts a new run.
        // The y == count sentinel closes the last run; the first maximum wins.
        int mt = -1, mb = -1, mass = -1, rt = -1, rb = -1, rm = 0;
        for (int y = 0; y <= count; ++y)
        {
            if (y < count && !rows[y]) continue;
            if (y < count && rt >= 0 && y - rb <= 2) { rb = y; rm += rows[y]; continue; }
            if (rt >= 0 && rm > mass) { mt = rt; mb = rb; mass = rm; }
            if (y < count) { rt = rb = y; rm = rows[y]; }
        }
        usable = usable && mass >= total * .55;
        top = mt;
        bottom = mb;
        if (usable && a.detached)
        {
            rt = -1;
            for (int y = 0; y <= count; ++y)
            {
                if (y < count && !rows[y]) continue;
                if (y < count && rt >= 0 && y - rb <= 2) { rb = y; rm += rows[y]; continue; }
                if (rt >= 0 && rt != mt && rm <= mass * .3)
                {
                    const int gap = max(max(top - rb, rt - bottom), 0) * step;
                    if (gap <= fmax(2.0, a.height * .22)) { top = min(top, rt); bottom = max(bottom, rb); }
                }
                if (y < count) { rt = rb = y; rm = rows[y]; }
            }
        }
    }
    __syncthreads();
    if (!usable)
        return;
    for (int k = t; k < columns * (bottom - top + 1); k += 256)
    {
        const int iu = k % columns, iv = top + k / columns;
        if (!inkAt(iu, iv))
            continue;
        atomicMin(&left, iu);
        atomicMax(&right, iu);
    }
    __syncthreads();
    if (!t && right >= left)
    {
        out[i].reason = OrientedFitReason::ShrinkGuard;
        const double u0 = double(left) * step, u1 = fmin(a.width, double(right) * step + step);
        const double v0 = double(top) * step, v1 = fmin(a.height, double(bottom) * step + step);
        if (u1 - u0 >= a.detectedWidth * .35 && v1 - v0 >= fmax(4.0, a.height * .25)) {
            out[i].u0 = u0, out[i].u1 = u1, out[i].v0 = v0, out[i].v1 = v1, out[i].fitted = 1;
            out[i].reason = OrientedFitReason::Fitted;
        }
    }
}

struct SurfaceSampler {
    const unsigned char* pixels;size_t pitch;int width,height;OrientedInk frame;
    __device__ bool operator()(double x,double y,surface_geometry::Rgb& rgb)const{
        const double px=frame.ox+frame.ux*x+frame.vx*y,py=frame.oy+frame.uy*x+frame.vy*y;
        if(px<0||py<0||px>=width||py>=height)return false;
        const auto* p=pixels+int(floor(py))*pitch+int(floor(px))*4;rgb={p[2],p[1],p[0]};return true;
    }
};
// One request per block. Members share every loop of the helper; their
// combinations are exact, so the result matches a single serial member.
inline constexpr int SurfaceThreads=128;
struct SurfaceTeam {
    static constexpr bool FilteredInk=true; // FP64 is a small fraction of FP32 throughput
    __device__ int Rank()const{return threadIdx.x;}
    __device__ int Size()const{return SurfaceThreads;}
    __device__ void Sync()const{__syncthreads();}
    template<class T,class F> __device__ T Combine(T value,F combine)const{
        static_assert(sizeof(T)%sizeof(int)==0,"combined values are shuffled as words");
        for(int lane=16;lane;lane>>=1){T other;
            const int* from=reinterpret_cast<const int*>(&value);int* to=reinterpret_cast<int*>(&other);
            for(int i=0;i<int(sizeof(T)/sizeof(int));++i)to[i]=__shfl_xor_sync(0xffffffffu,from[i],lane);
            value=combine(value,other);
        }
        // Every member then folds the warp results in the same fixed order.
        __shared__ T warps[SurfaceThreads/32];
        __syncthreads();
        if(!(threadIdx.x&31))warps[threadIdx.x>>5]=value;
        __syncthreads();
        T result=warps[0];for(int i=1;i<SurfaceThreads/32;++i)result=combine(result,warps[i]);
        return result;
    }
    __device__ int First(bool value)const{
        const int first=Combine(value?int(threadIdx.x):SurfaceThreads,[](int a,int b){return a<b?a:b;});
        return first<SurfaceThreads?first:-1;
    }
    __device__ void AtomicMin(int& target,int value)const{atomicMin(&target,value);}
    __device__ void AtomicMax(int& target,int value)const{atomicMax(&target,value);}
    __device__ bool Claim(int& state)const{return state==1&&atomicCAS(&state,1,2)==1;}
    __device__ int Append(int& size)const{return atomicAdd(&size,1);}
};
__global__ void __launch_bounds__(SurfaceThreads) surfaceFrames(const unsigned char* pixels,size_t pitch,int w,int h,
    const SurfaceRequest* requests,const Colors* colors,const Fit* fits,const OrientedFit* oriented,SurfaceFit* out)
{
    __shared__ surface_geometry::Scratch scratch;
    const int i=blockIdx.x;if(!threadIdx.x)out[i]={};const auto request=requests[i];auto frame=request.frame;
    if(request.oriented){
        const auto f=oriented[request.fitIndex];if(!f.fitted)return;
        if(hypot(frame.warpX,frame.warpY)>fmin(frame.width,frame.height)*.002)return;
        frame.ox+=frame.ux*f.u0+frame.vx*f.v0;frame.oy+=frame.uy*f.u0+frame.vy*f.v0;
        frame.width=f.u1-f.u0;frame.height=f.v1-f.v0;
    }else{
        const auto f=fits[request.fitIndex];
        if(f.x==frame.ox&&f.y==frame.oy&&f.w==frame.width&&f.h==frame.height)return;
        frame.ox=f.x;frame.oy=f.y;frame.width=f.w;frame.height=f.h;
        frame.ux=frame.vy=1;frame.uy=frame.vx=0;
    }
    const auto c=colors[request.colorIndex];
    const auto result=surface_geometry::Refine(SurfaceTeam{},frame.width,frame.height,
        surface_geometry::Rgb{c.foreground[0],c.foreground[1],c.foreground[2]},
        surface_geometry::Rgb{c.background[0],c.background[1],c.background[2]},
        SurfaceSampler{pixels,pitch,w,h,frame},scratch);
    if(threadIdx.x||!result.fitted)return;
    for(int k=0;k<4;++k){out[i].xy[k*2]=frame.ox+frame.ux*result.quad[k].x+frame.vx*result.quad[k].y;
        out[i].xy[k*2+1]=frame.oy+frame.uy*result.quad[k].x+frame.vy*result.quad[k].y;}
    out[i].fitted=1;
}

// SHA-256 compression is written here from the published algorithm. Each leaf
// hashes one complete 4096-byte packed source chunk. Parents hash at most 16
// child digests. Distinct leaf/parent tags prevent structural ambiguity.
__device__ unsigned rotr(unsigned x, int n)
{
    return (x >> n) | (x << (32 - n));
}
__device__ void compress(unsigned *s, const unsigned char *block)
{
    const unsigned k[64]{0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
                         0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
                         0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
                         0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                         0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
                         0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
                         0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
                         0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                         0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
                         0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                         0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    unsigned w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (unsigned(block[i * 4]) << 24) | (unsigned(block[i * 4 + 1]) << 16) |
               (unsigned(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
    for (int i = 16; i < 64; ++i)
        w[i] = w[i - 16] + (rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
               w[i - 7] + (rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10));
    unsigned a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; ++i)
    {
        unsigned u =
            h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        unsigned v = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + u;
        d = c;
        c = b;
        b = a;
        a = u + v;
    }
    const unsigned v[8]{a, b, c, d, e, f, g, h};
    for (int i = 0; i < 8; ++i)
        s[i] += v[i];
}
__global__ void fingerprint(const unsigned char *p, size_t pitch, int w, int h, int count,
                            unsigned char *out, int leaf)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    const int total = leaf ? w * h * 4 : w, offset = i * (leaf ? 4096 : 512),
              bytes = min(leaf ? 4096 : 512, total - offset), length = bytes + 1;
    unsigned s[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const int blocks = (length + 9 + 63) / 64;
    for (int b = 0; b < blocks; ++b)
    {
        unsigned char block[64];
        for (int c = 0; c < 64; ++c)
        {
            int n = b * 64 + c;
            unsigned char v = 0;
            if (n == 0)
                v = leaf ? 0 : 1;
            else if (n < length)
            {
                int pos = offset + n - 1;
                v = leaf ? p[(pos / (w * 4)) * pitch + pos % (w * 4)] : p[pos];
            }
            else if (n == length)
                v = 128;
            else if (n >= blocks * 64 - 8)
                v = static_cast<unsigned char>((static_cast<unsigned long long>(length) * 8) >>
                                               ((blocks * 64 - 1 - n) * 8));
            block[c] = v;
        }
        compress(s, block);
    }
    for (int j = 0; j < 32; ++j)
        out[i * 32 + j] = static_cast<unsigned char>(s[j / 4] >> (24 - (j % 4) * 8));
}
} // namespace validation
void ValidationIntegral(const void *p, size_t pitch, int w, int h, int d, unsigned *out,
                        cudaStream_t s)
{
    validation::integralRows<<<h / d, 256, 0, s>>>(
        static_cast<const unsigned char *>(p), pitch, w, h, d, out);
    validation::integralColumns<<<(w / d + 128) / 128, 128, 0, s>>>(out, w / d, h / d);
}
void Reference(const unsigned *p, int w, int h, Grid g, float *values, int *valid, cudaStream_t s)
{
    validation::reference<<<1, 1, 0, s>>>(p, w, h, g, values, valid);
}
void MatchReference(const unsigned *p, int w, int h, Grid g, const float *ref, double score,
                    double visible, int radius, double sx, double sy, double ux, double uy,
                    MatchResult *out, cudaStream_t s)
{
    radius = radius < 0 ? 0 : (radius > 10 ? 10 : radius);
    validation::match<<<1, 128, 0, s>>>(p, w, h, g, ref, score, visible, radius, sx, sy, ux, uy,
                                        out);
}
void References(const unsigned *p, int w, int h, const ReferenceRequest *requests, int n,
                int *valid, cudaStream_t s)
{
    if (n > 0)
        validation::references<<<n, 1, 0, s>>>(p, w, h, requests, valid);
}
void MatchReferences(const unsigned *p, int w, int h, const MatchRequest *requests, int n,
                     double score, double visible, int radius, MatchResult *out, cudaStream_t s)
{
    radius = radius < 0 ? 0 : (radius > 10 ? 10 : radius);
    if (n > 0)
        validation::matches<<<n, 128, 0, s>>>(p, w, h, requests, score, visible, radius, out);
}
void Similarity(const unsigned *a, int aw, int ah, ProbeMap ma, const unsigned *b, int bw, int bh,
                ProbeMap mb, double x, double y, double w, double h, double *out, cudaStream_t s)
{
    validation::similarity<false><<<1, 128, 0, s>>>(a, aw, ah, ma, b, bw, bh, mb, x, y, w, h, out);
}
void RegionalSimilarities(const unsigned *a, int aw, int ah, ProbeMap ma, const unsigned *b, int bw, int bh,
                         ProbeMap mb, double x, double y, double w, double h, double *out, cudaStream_t s)
{
    validation::similarity<true><<<64, 128, 0, s>>>(a, aw, ah, ma, b, bw, bh, mb, x, y, w, h, out);
}
void LineColors(const void *p, size_t pitch, int w, int h, const Rect *rects, int n, Colors *out,
                cudaStream_t s, const OrientedInk* frames)
{
    if (n)
        validation::colors<<<n, 256, 0, s>>>(static_cast<const unsigned char *>(p), pitch, w, h,
                                             rects, out, frames);
}
void InkBounds(const void *p, size_t pitch, int w, int h, const Ink *words, const int *lines, int n,
               const Colors *cs, Fit *out, cudaStream_t s)
{
    if (n)
        validation::ink<<<n, 256, 0, s>>>(static_cast<const unsigned char *>(p), pitch, w, h, words,
                                          lines, cs, out);
}
void OrientedInkBounds(const void *p, size_t pitch, int w, int h, const OrientedInk *words, const int *lines,
                       int n, const Colors *cs, OrientedFit *out, cudaStream_t s)
{
    if (n)
        validation::orientedInk<<<n, 256, 0, s>>>(static_cast<const unsigned char *>(p), pitch, w, h, words,
                                                  lines, cs, out);
}
void SurfaceFrames(const void* p,size_t pitch,int w,int h,const SurfaceRequest* requests,int n,
    const Colors* colors,const Fit* fits,const OrientedFit* oriented,SurfaceFit* out,cudaStream_t stream)
{
    if(n<0||n>surface_geometry::WordLimit)throw std::invalid_argument("UCT surface request budget exceeded");
    if(n)validation::surfaceFrames<<<n,validation::SurfaceThreads,0,stream>>>(static_cast<const unsigned char*>(p),pitch,w,h,
        requests,colors,fits,oriented,out);
}
void FingerprintLeaves(const void *p, size_t pitch, int w, int h, unsigned char *out,
                       cudaStream_t s)
{
    int n = (w * h * 4 + 4095) / 4096;
    validation::fingerprint<<<(n + 127) / 128, 128, 0, s>>>(static_cast<const unsigned char *>(p),
                                                            pitch, w, h, n, out, 1);
}
void FingerprintParents(const unsigned char *p, int n, unsigned char *out, cudaStream_t s)
{
    int count = (n + 15) / 16;
    validation::fingerprint<<<(count + 127) / 128, 128, 0, s>>>(p, 0, n * 32, 1, count, out, 0);
}
} // namespace okuflow::uct::gpu
