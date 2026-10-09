#pragma once

namespace okuflow {

enum class YuvMatrix { Bt601, Bt709 };
enum class YuvRange { Limited, Full };

// Untagged/unknown camera metadata retains the historic BT.601 limited default.
// This describes encoded Y'CbCr values, not an RGB gamut or transfer function.
struct YuvColorInfo {
    YuvMatrix matrix{YuvMatrix::Bt601};
    YuvRange range{YuvRange::Limited};
};

struct YuvCoefficients {
    int yOffset;
    int yScale;
    int redV;
    int greenU;
    int greenV;
    int blueU;
};

// ITU luma weights (601: .299/.114, 709: .2126/.0722), scaled to
// 16 fractional bits. Limited Y uses 219 steps and Cb/Cr use 224 steps;
// full range uses 255 for both. Coefficients are selected once per frame.
constexpr YuvCoefficients GetYuvCoefficients(YuvColorInfo color)
{
    const double kr = color.matrix == YuvMatrix::Bt709 ? 0.2126 : 0.299;
    const double kb = color.matrix == YuvMatrix::Bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;
    const bool limited = color.range == YuvRange::Limited;
    const double ys = limited ? 255.0 / 219.0 : 1.0;
    const double cs = limited ? 255.0 / 224.0 : 1.0;
    return {limited ? 16 : 0,
            static_cast<int>(ys * 65536.0 + 0.5),
            static_cast<int>(2.0 * (1.0 - kr) * cs * 65536.0 + 0.5),
            static_cast<int>(2.0 * kb * (1.0 - kb) / kg * cs * 65536.0 + 0.5),
            static_cast<int>(2.0 * kr * (1.0 - kr) / kg * cs * 65536.0 + 0.5),
            static_cast<int>(2.0 * (1.0 - kb) * cs * 65536.0 + 0.5)};
}

#ifdef __CUDACC__
#define OKUFLOW_YUV_HD __host__ __device__
#else
#define OKUFLOW_YUV_HD
#endif

// Clamp only after the complete equation, so footroom chroma is handled
// identically by NV12/YUY2 and CPU/CUDA. All intermediate values fit int32.
OKUFLOW_YUV_HD inline unsigned char YuvChannelToByte(int value)
{
    const int rounded = (value + 32768) / 65536;
    return static_cast<unsigned char>(rounded < 0 ? 0 : rounded > 255 ? 255 : rounded);
}

struct YuvRgbPixel { unsigned char r, g, b; };

OKUFLOW_YUV_HD inline YuvRgbPixel ConvertYuvPixel(
    int y, int u, int v, const YuvCoefficients& c)
{
    const int luma = (y - c.yOffset) * c.yScale;
    const int cb = u - 128;
    const int cr = v - 128;
    return {YuvChannelToByte(luma + c.redV * cr),
            YuvChannelToByte(luma - c.greenU * cb - c.greenV * cr),
            YuvChannelToByte(luma + c.blueU * cb)};
}

#undef OKUFLOW_YUV_HD
} // namespace okuflow
