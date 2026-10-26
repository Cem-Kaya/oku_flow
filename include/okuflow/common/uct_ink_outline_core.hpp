#pragma once
#include <cstdint>
#include <type_traits>

#if defined(__CUDACC__)
#define OKUFLOW_INK_HD __host__ __device__
#else
#define OKUFLOW_INK_HD
#endif

namespace okuflow::uct::ink
{
inline constexpr int MaximumWidth = 512, MaximumHeight = 256, MaximumPixels = 65536;
inline constexpr int MaximumJobPixels = 131072, MaximumRegions = 8;
inline constexpr int Levels = 4, MaximumSegments = 8192, MaximumVertices = 16384;
struct Region
{
    int x{}, y{}, w{}, h{};
    int foreground[3]{}, background[3]{};
};
struct Segment
{
    int ax{}, ay{}, bx{}, by{};
};
struct Summary
{
    int cuts[Levels]{}, counts[Levels]{}, border{}, invalid{}, contrast{};
};
static_assert(sizeof(Region) == 40 && sizeof(Segment) == 16 && sizeof(Summary) == 44);
static_assert(std::is_standard_layout_v<Region> && std::is_trivially_copyable_v<Region>);
static_assert(std::is_standard_layout_v<Segment> && std::is_trivially_copyable_v<Segment>);
static_assert(std::is_standard_layout_v<Summary> && std::is_trivially_copyable_v<Summary>);

// Exact integer projection onto the measured background-to-ink colour axis.
// Only contour segments and scalar diagnostics leave a resident GPU image.
OKUFLOW_INK_HD inline int Contrast(const Region &r)
{
    int n = 0;
    for (int c = 0; c < 3; ++c)
    {
        const int d = r.foreground[c] - r.background[c];
        n += d * d;
    }
    return n;
}
OKUFLOW_INK_HD inline unsigned char Coverage(const unsigned char *bgra, const Region &r)
{
    const long long n = Contrast(r);
    if (n < 4096)
        return 0;
    long long dot = 0;
    for (int c = 0; c < 3; ++c)
        dot += (int(bgra[2 - c]) - r.background[c]) * (r.foreground[c] - r.background[c]);
    const long long value = (255 * dot + n / 2) / n;
    return static_cast<unsigned char>(value < 0 ? 0 : value > 255 ? 255 : value);
}
OKUFLOW_INK_HD inline Summary Thresholds(const int *histogram, int pixels, const Region &r)
{
    Summary s;
    s.contrast = Contrast(r);
    if (pixels < 1 || s.contrast < 4096)
    {
        s.invalid = 1;
        return s;
    }
    int cumulative = 0, low = -1, high = -1;
    for (int i = 0; i < 256; ++i)
    {
        cumulative += histogram[i];
        if (low < 0 && cumulative >= (pixels * 20 + 99) / 100)
            low = i;
        if (high < 0 && cumulative >= (pixels * 98 + 99) / 100)
            high = i;
    }
    if (high - low < 64)
    {
        s.invalid = 1;
        return s;
    }
    const int span = high - low, mid = (low + high) / 2, delta = span / 10 > 0 ? span / 10 : 1;
    s.cuts[0] = low + span * 3 / 10;
    s.cuts[1] = mid - delta;
    s.cuts[2] = mid;
    s.cuts[3] = mid + delta;
    return s;
}

// Pixel centres are fixed-point 24.8. An odd doubled isovalue avoids vertices
// landing exactly on a sample and makes CPU/CUDA contour topology identical.
OKUFLOW_INK_HD inline int Cell(const int *q, int cut, int x, int y, Segment *output)
{
    const int level = 2 * cut + 1;
    int mask = 0;
    for (int i = 0; i < 4; ++i)
        if (2 * q[i] > level)
            mask |= 1 << i;
    int pairs[4]{}, n = 1;
    switch (mask)
    {
    case 0:
    case 15:
        return 0;
    case 1:
        pairs[0] = 0;
        pairs[1] = 3;
        break;
    case 2:
        pairs[0] = 1;
        pairs[1] = 0;
        break;
    case 3:
        pairs[0] = 1;
        pairs[1] = 3;
        break;
    case 4:
        pairs[0] = 2;
        pairs[1] = 1;
        break;
    case 5:
        pairs[0] = 0;
        pairs[1] = 1;
        pairs[2] = 2;
        pairs[3] = 3;
        n = 2;
        break;
    case 6:
        pairs[0] = 2;
        pairs[1] = 0;
        break;
    case 7:
        pairs[0] = 2;
        pairs[1] = 3;
        break;
    case 8:
        pairs[0] = 3;
        pairs[1] = 2;
        break;
    case 9:
        pairs[0] = 0;
        pairs[1] = 2;
        break;
    case 10:
        pairs[0] = 3;
        pairs[1] = 0;
        pairs[2] = 1;
        pairs[3] = 2;
        n = 2;
        break;
    case 11:
        pairs[0] = 1;
        pairs[1] = 2;
        break;
    case 12:
        pairs[0] = 3;
        pairs[1] = 1;
        break;
    case 13:
        pairs[0] = 0;
        pairs[1] = 1;
        break;
    case 14:
        pairs[0] = 3;
        pairs[1] = 0;
        break;
    }
    const int starts[4]{0, 1, 3, 0}, ends[4]{1, 2, 2, 3};
    int px[4]{}, py[4]{};
    for (int k = 0; k < 2 * n; ++k)
    {
        const int edge = pairs[k], a = starts[edge], b = ends[edge];
        const int denominator = 2 * (q[b] - q[a]);
        if (!denominator)
            return -1;
        int t = (level - 2 * q[a]) * 256 / denominator;
        t = t < 1 ? 1 : t > 255 ? 255 : t;
        px[edge] = x * 256 + 128 + ((edge == 1) ? 256 : (edge == 3) ? 0 : t);
        py[edge] = y * 256 + 128 + ((edge == 0) ? 0 : (edge == 2) ? 256 : t);
    }
    for (int i = 0; i < n; ++i)
    {
        const int a = pairs[i * 2], b = pairs[i * 2 + 1];
        Segment s{px[a], py[a], px[b], py[b]};
        // The case table orients ink on the left, including eight-connected
        // saddles. A local bilinear gradient can reverse in a saddle and is
        // not an orientation authority for this connectivity rule.
        if (output)
            output[i] = s;
    }
    return n;
}
} // namespace okuflow::uct::ink
#undef OKUFLOW_INK_HD
