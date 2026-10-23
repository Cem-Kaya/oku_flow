// Private, first-party integer decisions shared by worker and CUDA color sampling.
#pragma once

#if defined(__CUDACC__)
#define OKUFLOW_UCT_COLOR_HD __host__ __device__
#else
#define OKUFLOW_UCT_COLOR_HD
#endif

namespace okuflow::uct::color_policy {

struct Rgb { int channel[3]; };

OKUFLOW_UCT_COLOR_HD inline int BodySampleStep(int width, int height)
{
    // Small glyph cores can be only one source pixel wide. Visit every pixel
    // within this fixed dense-sampling budget so a 2px lattice cannot miss a
    // stem and promote its blur halo to representative ink. Larger bodies
    // retain their existing stride and all perimeter/endpoint decisions.
    return width > 0 && height > 0 && width <= 4096 / height ? 1 : 2;
}

OKUFLOW_UCT_COLOR_HD inline int Median(const unsigned* histogram, unsigned count)
{
    unsigned total = 0;
    for (int value = 0; value < 256; ++value) {
        total += histogram[value];
        if (total > count / 2) return value;
    }
    return 0;
}

OKUFLOW_UCT_COLOR_HD inline bool Compatible(const unsigned char* bgra, const Rgb& color)
{
    for (int channel = 0; channel < 3; ++channel) {
        const int delta = int(bgra[2 - channel]) - color.channel[channel];
        if (delta < -24 || delta > 24) return false;
    }
    return true;
}

OKUFLOW_UCT_COLOR_HD inline int Endpoint(const unsigned* lumaHistogram, unsigned count, int side)
{
    // Opaque ink occupies the darker/lighter tail; AA blends cannot drag an
    // arithmetic mean toward paper. Include all ties at the quartile boundary.
    const unsigned rank = (count + 3) / 4;
    unsigned total = 0;
    for (int i = 0; i < 256; ++i) {
        const int value = side == 0 ? i : 255 - i;
        total += lumaHistogram[value];
        if (total >= rank) return value;
    }
    return side == 0 ? 0 : 255;
}

OKUFLOW_UCT_COLOR_HD inline int AdjacentBackground(
    const unsigned sideCounts[4][2], const unsigned* sideTotals)
{
    // The immediately touching surface outranks a distant expanded ring.
    // Four independent sides are required: missing/clipped or mixed edges
    // keep the conservative body/corner/expanded-strip policy below.
    for (int color = 0; color < 2; ++color) {
        bool all = true;
        for (int side = 0; side < 4; ++side)
            all = all && sideTotals[side] >= 3 &&
                  sideCounts[side][color] * 4 >= sideTotals[side] * 3;
        if (all) return color;
    }
    return -1;
}

OKUFLOW_UCT_COLOR_HD inline int Background(const unsigned* bodyCounts,
    const unsigned sideCounts[4][2], const unsigned* sideTotals,
    const unsigned* cornerVotes, bool& useRing)
{
    int support[2]{};
    for (int side = 0; side < 4; ++side)
        for (int color = 0; color < 2; ++color)
            if (sideTotals[side] >= 3 && sideCounts[side][color] * 4 >= sideTotals[side] * 3)
                ++support[color];
    for (int color = 0; color < 2; ++color)
        if (support[color] >= 3 && cornerVotes[color] >= 2) {
            useRing = true;
            return color;
        }
    useRing = false;
    // A neighboring surface cannot win by ring area alone. In-box corners
    // resolve button edges; a genuinely ambiguous tight box keeps the old
    // minority-ink fallback rather than inventing a spatial segmentation.
    if (cornerVotes[0] != cornerVotes[1] &&
        (cornerVotes[0] >= 2 || cornerVotes[1] >= 2))
        return cornerVotes[0] > cornerVotes[1] ? 0 : 1;
    return bodyCounts[0] <= bodyCounts[1] ? 1 : 0;
}

} // namespace okuflow::uct::color_policy

#undef OKUFLOW_UCT_COLOR_HD
