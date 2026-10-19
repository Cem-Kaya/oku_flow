// First-party CUDA implementation of UCT's CPU validation/color/ink policy.
// Included by uct_kernels.cu so every CUDA consumer has the same primitives.
#include <cstdint>

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
    const int y = blockIdx.x * blockDim.x + threadIdx.x, ow = w / d, oh = h / d;
    if (y >= oh)
        return;
    const int step = max(1, d / 2), per = (d + step - 1) / step;
    unsigned sum = 0;
    out[(y + 1) * (ow + 1)] = 0;
    for (int x = 0; x < ow; ++x)
    {
        int v = 0;
        for (int a = 0; a < d; a += step)
            for (int b = 0; b < d; b += step)
                v += luma(p + (y * d + a) * pitch + (x * d + b) * 4);
        sum += v / (per * per);
        out[(y + 1) * (ow + 1) + x + 1] = sum;
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
        if (!box(p, w, h, g.x + (i % g.columns) * g.dx, g.y + (i / g.columns) * g.dy, g.halfW,
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
        if (!box(p, w, h, g.x + (i % g.columns) * g.dx + dx, g.y + (i / g.columns) * g.dy + dy,
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
        done = !out->visible || best >= score + .08;
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
__global__ void similarity(const unsigned *a, int aw, int ah, ProbeMap ma, const unsigned *b,
                           int bw, int bh, ProbeMap mb, double x, double y, double w, double h,
                           double *out)
{
    // One block, per-thread correlations then a bounded block reduction.
    __shared__ double sums[6][128];
    const int t = threadIdx.x;
    double u = 0, v = 0, uu = 0, vv = 0, uv = 0, n = 0;
    for (int i = t; i < 48 * 27; i += 128)
    {
        float p, q;
        double sx = x + (i % 48 + .5) * w / 48, sy = y + (i / 48 + .5) * h / 27;
        if (!box(a, aw, ah, sx * ma.sx + ma.x, sy * ma.sy + ma.y, .5 * w / 48 * ma.sx,
                 .5 * h / 27 * ma.sy, p) ||
            !box(b, bw, bh, sx * mb.sx + mb.x, sy * mb.sy + mb.y, .5 * w / 48 * mb.sx,
                 .5 * h / 27 * mb.sy, q))
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
        *out = n < 64 ? 2.
                      : (va < n * 16 && vb < n * 16
                             ? (fabs(u / n - v / n) < 8 ? 1. : 0.)
                             : (va <= 1e-9 || vb <= 1e-9 ? 0. : (uv - u * v / n) / sqrt(va * vb)));
    }
}
__global__ void colors(const unsigned char *p, size_t pitch, int w, int h, const Rect *rects,
                       Colors *out)
{
    const int t = threadIdx.x, index = blockIdx.x;
    const auto b = rects[index];
    __shared__ unsigned hist[2][3][256];
    __shared__ unsigned sums[8], lum[256];
    __shared__ int threshold;
    for (int i = t; i < 1536; i += 256)
        reinterpret_cast<unsigned *>(hist)[i] = 0;
    if (t < 8)
        sums[t] = 0;
    unsigned sum = 0;
    const int nx = (b.w + 1) / 2, ny = (b.h + 1) / 2;
    for (int i = t; i < nx * ny; i += 256)
        sum += luma(p + (b.y + (i / nx) * 2) * pitch + (b.x + (i % nx) * 2) * 4);
    lum[t] = sum;
    __syncthreads();
    for (int d = 128; d; d /= 2)
    {
        if (t < d)
            lum[t] += lum[t + d];
        __syncthreads();
    }
    if (!t)
        threshold = nx * ny ? int(lum[0] / (nx * ny)) : 0;
    __syncthreads();
    for (int i = t; i < nx * ny; i += 256)
    {
        const auto *px = p + (b.y + (i / nx) * 2) * pitch + (b.x + (i % nx) * 2) * 4;
        int side = luma(px) < threshold ? 0 : 1;
        atomicAdd(sums + side * 4, 1u);
        for (int c = 0; c < 3; ++c)
            atomicAdd(sums + side * 4 + c + 1, unsigned(px[2 - c]));
    }
    const int m = min(12, max(2, b.h / 6)), x = max(0, b.x - m), y = max(0, b.y - m),
              rw = min(w, b.x + b.w + m) - x, rh = min(h, b.y + b.h + m) - y;
    const int sw = (rw + 1) / 2, sh = (rh + 1) / 2;
    for (int i = t; i < sw * sh; i += 256)
    {
        const int xx = x + (i % sw) * 2, yy = y + (i / sw) * 2;
        if (xx >= b.x && xx < b.x + b.w && yy >= b.y && yy < b.y + b.h)
            continue;
        const auto *px = p + yy * pitch + xx * 4;
        int side = luma(px) < threshold ? 0 : 1;
        for (int c = 0; c < 3; ++c)
            atomicAdd(&hist[side][c][px[2 - c]], 1u);
    }
    __syncthreads();
    if (!t)
    {
        unsigned counts[2]{};
        for (int i = 0; i < 256; ++i)
        {
            counts[0] += hist[0][0][i];
            counts[1] += hist[1][0][i];
        }
        const int fg = counts[0] != counts[1] ? (counts[1] > counts[0] ? 0 : 1)
                                              : (sums[0] <= sums[4] ? 0 : 1),
                  bg = 1 - fg;
        Colors c{{0, 0, 0}, {255, 255, 255}};
        if (sums[fg * 4] && (sums[bg * 4] || counts[bg] >= 8))
            for (int k = 0; k < 3; ++k)
            {
                c.foreground[k] = sums[fg * 4 + k + 1] / sums[fg * 4];
                c.background[k] = sums[bg * 4] ? sums[bg * 4 + k + 1] / sums[bg * 4] : 255;
                if (counts[bg] >= 8)
                {
                    unsigned total = 0;
                    for (int v = 0; v < 256; ++v)
                    {
                        total += hist[bg][k][v];
                        if (total > counts[bg] / 2)
                        {
                            c.background[k] = v;
                            break;
                        }
                    }
                }
            }
        out[index] = c;
    }
}
__global__ void ink(const unsigned char *p, size_t pitch, int w, int h, const Ink *words,
                    const int *lines, const Colors *cs, Fit *out)
{
    const int i = blockIdx.x, t = threadIdx.x;
    const auto a = words[i];
    const auto b = a.search;
    const auto c = cs[lines[i]];
    __shared__ int rows[8192], total, top, bottom, left, right, threshold, polarity, step, usable;
    for (int y = t; y < b.h; y += 256)
        rows[y] = 0;
    if (!t)
    {
        out[i] = {a.x, a.y, a.w, a.h};
        total = 0;
        top = bottom = 0;
        left = b.x + b.w;
        right = b.x - 1;
        int fg = (77 * c.foreground[0] + 150 * c.foreground[1] + 29 * c.foreground[2]) / 256;
        int bg = (77 * c.background[0] + 150 * c.background[1] + 29 * c.background[2]) / 256;
        usable = abs(fg - bg) >= 24 && b.w > 0 && b.h > 0;
        threshold = (fg + bg) / 2;
        polarity = fg < bg;
        step = max(1, int(ceil(sqrt(double(b.w) * b.h / 16384))));
    }
    __syncthreads();
    if (!usable)
        return;
    const int nx = (b.w + step - 1) / step, ny = (b.h + step - 1) / step;
    for (int k = t; k < nx * ny; k += 256)
    {
        int x = b.x + (k % nx) * step, y = b.y + (k / nx) * step, v = luma(p + y * pitch + x * 4);
        if (polarity ? v >= threshold : v <= threshold)
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
        int v = luma(p + y * pitch + x * 4);
        if (polarity ? v >= threshold : v <= threshold)
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
    validation::integralRows<<<(h / d + 127) / 128, 128, 0, s>>>(
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
    radius = radius < 1 ? 1 : (radius > 10 ? 10 : radius);
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
    radius = radius < 1 ? 1 : (radius > 10 ? 10 : radius);
    if (n > 0)
        validation::matches<<<n, 128, 0, s>>>(p, w, h, requests, score, visible, radius, out);
}
void Similarity(const unsigned *a, int aw, int ah, ProbeMap ma, const unsigned *b, int bw, int bh,
                ProbeMap mb, double x, double y, double w, double h, double *out, cudaStream_t s)
{
    validation::similarity<<<1, 128, 0, s>>>(a, aw, ah, ma, b, bw, bh, mb, x, y, w, h, out);
}
void LineColors(const void *p, size_t pitch, int w, int h, const Rect *rects, int n, Colors *out,
                cudaStream_t s)
{
    if (n)
        validation::colors<<<n, 256, 0, s>>>(static_cast<const unsigned char *>(p), pitch, w, h,
                                             rects, out);
}
void InkBounds(const void *p, size_t pitch, int w, int h, const Ink *words, const int *lines, int n,
               const Colors *cs, Fit *out, cudaStream_t s)
{
    if (n)
        validation::ink<<<n, 256, 0, s>>>(static_cast<const unsigned char *>(p), pitch, w, h, words,
                                          lines, cs, out);
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
