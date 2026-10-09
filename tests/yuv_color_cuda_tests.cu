#include "okuflow/cuda/cuda_kernels.hpp"
#include "yuv_color_reference.hpp"
#include <cuda_runtime.h>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Check(cudaError_t result)
{
    if (result != cudaSuccess) throw std::runtime_error(cudaGetErrorString(result));
}
struct DeviceBytes {
    unsigned char* data{};
    explicit DeviceBytes(std::size_t size) { Check(cudaMalloc(&data, size)); }
    ~DeviceBytes() { cudaFree(data); }
};
} // namespace

int main()
{
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) return 77;
    try {
        // Each color is a 2x2 block; deliberately padded rows exercise both
        // raw converter pitches in addition to matrix and range selection.
        constexpr int width = 20, height = 72, pitch = 48, outputPitch = 96;
        std::vector<unsigned char> nv12(pitch * height * 3 / 2, 0xee);
        std::vector<unsigned char> yuy2(pitch * height, 0xee);
        struct Sample { int y, u, v; };
        std::vector<Sample> samples;
        for (int y : {0, 15, 16, 17, 64, 128, 234, 235, 236, 255})
            for (int u : {0, 16, 90, 128, 240, 255})
                for (int v : {0, 16, 90, 128, 240, 255}) samples.push_back({y, u, v});
        for (int i = 0; i < int(samples.size()); ++i) {
            const auto s = samples[i];
            const int x = (i % 10) * 2, y = (i / 10) * 2;
            for (int dy = 0; dy < 2; ++dy) {
                nv12[(y + dy) * pitch + x] = nv12[(y + dy) * pitch + x + 1] = s.y;
                auto* pair = yuy2.data() + (y + dy) * pitch + x * 2;
                pair[0] = pair[2] = s.y;
                pair[1] = s.u; pair[3] = s.v;
            }
            nv12[pitch * height + (y / 2) * pitch + x] = s.u;
            nv12[pitch * height + (y / 2) * pitch + x + 1] = s.v;
        }
        DeviceBytes nv(nv12.size()), yy(yuy2.size()), out(outputPitch * height);
        Check(cudaMemcpy(nv.data, nv12.data(), nv12.size(), cudaMemcpyHostToDevice));
        Check(cudaMemcpy(yy.data, yuy2.data(), yuy2.size(), cudaMemcpyHostToDevice));
        std::vector<unsigned char> result(outputPitch * height);
        for (bool bt709 : {false, true}) for (bool full : {false, true}) {
            const okuflow::YuvColorInfo color{
                bt709 ? okuflow::YuvMatrix::Bt709 : okuflow::YuvMatrix::Bt601,
                full ? okuflow::YuvRange::Full : okuflow::YuvRange::Limited};
            for (bool planar : {false, true}) {
                if (planar) okuflow::LaunchNv12ToBgraLinear(
                    reinterpret_cast<uchar4*>(out.data), outputPitch,
                    nv.data, pitch, nv.data + pitch * height, pitch, width, height, nullptr, color);
                else okuflow::LaunchYuy2ToBgraLinear(
                    reinterpret_cast<uchar4*>(out.data), outputPitch,
                    yy.data, pitch, width, height, nullptr, color);
                Check(cudaMemcpy(result.data(), out.data, result.size(), cudaMemcpyDeviceToHost));
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                    const auto s = samples[(y / 2) * 10 + x / 2];
                    const auto rgb = ReferenceYuvRgb(s.y, s.u, s.v, bt709, full);
                    const auto* p = result.data() + y * outputPitch + x * 4;
                    if (std::abs(int(p[0]) - rgb[2]) > 1 ||
                        std::abs(int(p[1]) - rgb[1]) > 1 ||
                        std::abs(int(p[2]) - rgb[0]) > 1 || p[3] != 255) {
                        throw std::runtime_error("GPU YUV conversion differs from reference");
                    }
                }
            }
        }
        std::cout << "YUV 601/709 limited/full NV12/YUY2 reference checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
