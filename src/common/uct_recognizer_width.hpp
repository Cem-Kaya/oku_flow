// Private scalar policy shared by TensorRT, descriptor kernels and fixtures.
#pragma once

#if defined(__CUDACC__)
#define OKUFLOW_UCT_WIDTH_HD __host__ __device__
#else
#define OKUFLOW_UCT_WIDTH_HD
#endif

namespace okuflow::uct::recognizer_width {

inline constexpr int kMinimum = 128, kOptimum = 512, kMaximum = 1024;
inline constexpr int kMaximumCrops = 128 * 9, kMaximumGroup = 16;
// Includes every profile dimension and bucket policy; append actual N below.
inline constexpr char kCacheTag[] = "-rec-width-v1-buckets128-256-512-1024-min1x3x48x128-optNx3x48x512-maxNx3x48x1024-n";
inline constexpr char kFixedToken[] = "fixed1024";
inline constexpr char kBucketedToken[] = "bucketed";

OKUFLOW_UCT_WIDTH_HD inline bool IsCanvas(int width)
{
    return width == 128 || width == 256 || width == 512 || width == 1024;
}

OKUFLOW_UCT_WIDTH_HD inline int Canvas(int validWidth)
{
    // Invalid/oversized metadata retains the original full canvas. Positive
    // widths are rounded upward, so no useful source sample is newly clipped.
    if (validWidth <= 0) return kMaximum;
    return validWidth <= 128 ? 128 : validWidth <= 256 ? 256 : validWidth <= 512 ? 512 : 1024;
}

template<class Crop>
OKUFLOW_UCT_WIDTH_HD inline int OrderedGroup(const Crop* crops, int count)
{
    if (!crops || count < 1 || count > kMaximumGroup) return kMaximum;
    int maximum = 0;
    for (int i = 0; i < count; ++i) {
        if (crops[i].validWidth <= 0) return kMaximum;
        if (crops[i].validWidth > maximum) maximum = crops[i].validWidth;
    }
    return Canvas(maximum);
}

// Used after deserialization to reject an old fixed-width plan copied under a
// new cache filename, independently of the file payload's integrity digest.
template<class Extent>
inline bool MatchesProfile(const Extent* dims, int rank, int batch, int width)
{
    return dims && rank == 4 && dims[0] == batch && dims[1] == 3 && dims[2] == 48 && dims[3] == width;
}

} // namespace okuflow::uct::recognizer_width

#undef OKUFLOW_UCT_WIDTH_HD
