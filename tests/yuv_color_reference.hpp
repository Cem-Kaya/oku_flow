#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// Independent floating-point reference using normalized Y'CbCr and the
// defining Kr/Kb equations, rather than production fixed-point coefficients.
inline std::array<int, 3> ReferenceYuvRgb(int y, int u, int v, bool bt709, bool full)
{
    const double kr = bt709 ? .2126 : .299;
    const double kb = bt709 ? .0722 : .114;
    const double luma = (y - (full ? 0.0 : 16.0)) / (full ? 255.0 : 219.0);
    const double cb = (u - 128.0) / (full ? 255.0 : 224.0);
    const double cr = (v - 128.0) / (full ? 255.0 : 224.0);
    const double red = luma + (2.0 - 2.0 * kr) * cr;
    const double blue = luma + (2.0 - 2.0 * kb) * cb;
    const double green = (luma - kr * red - kb * blue) / (1.0 - kr - kb);
    const auto byte = [](double value) {
        return std::clamp(static_cast<int>(std::lround(value * 255.0)), 0, 255);
    };
    return {byte(red), byte(green), byte(blue)};
}
