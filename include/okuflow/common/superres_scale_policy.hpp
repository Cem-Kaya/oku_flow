#pragma once

namespace okuflow {

// NVIDIA Video Effects' SuperRes resolution table limits 3x/4x models to
// 720p/540p input, while 4/3x, 1.5x and 2x accept up to 2160p input.
// This is a necessary height check, not a promise that every width/layout is
// supported by the installed SDK. Its other validation/errors still apply.
// https://docs.nvidia.com/deeplearning/maxine/vfx-sdk-programming-guide/index.html
inline constexpr bool IsSuperResInputHeightSupported(
    unsigned int height, unsigned int numerator, unsigned int denominator) {
    unsigned int maximum = 0;
    if (numerator == 4u && denominator == 1u) {
        maximum = 540u;
    } else if (numerator == 3u && denominator == 1u) {
        maximum = 720u;
    } else if ((numerator == 2u && denominator == 1u) ||
               (numerator == 3u && denominator == 2u) ||
               (numerator == 4u && denominator == 3u)) {
        maximum = 2160u;
    }
    return height >= 90u && height <= maximum;
}

} // namespace okuflow
