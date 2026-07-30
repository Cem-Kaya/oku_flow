#pragma once

#ifdef _WIN32

#include <d3d11.h>

#include <string>

enum class InteropStressMode {
    ExternalMemory,
    LegacyRegistration,
};

struct InteropStressResult {
    std::string status{"error"};
    std::string detail;
    std::string path;
    int iterationsRequested{};
    int iterationsCompleted{};
    double averageIterationMs{};
};

InteropStressResult RunCudaInteropStress(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    InteropStressMode mode,
    int iterations);

#endif
