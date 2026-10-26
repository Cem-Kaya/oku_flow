#pragma once
#include <cstdint>

namespace okuflow {
// Presentation owns one selected BGRA source, not a decoder queue/lookahead.
// Match the document worker's 16-Mpixel/64-MiB ceiling without weakening the
// separate VideoPlayback memory planner used to admit decoded video streams.
constexpr bool MediaFrameLayoutFits(std::uint32_t width, std::uint32_t height,
                                    std::int64_t strideBytes) {
    constexpr std::uint64_t kFrameBytes = 64ull * 1024 * 1024;
    return width > 0 && height > 0 && width <= 16384 && height <= 16384 &&
        strideBytes >= std::int64_t(width) * 4 &&
        std::uint64_t(strideBytes) <= kFrameBytes / height;
}
}
