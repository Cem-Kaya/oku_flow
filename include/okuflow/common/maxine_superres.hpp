#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace okuflow {

// Runtime-only adapter for NVIDIA Video Effects SuperRes. The implementation
// resolves every SDK entry point with LoadLibrary/GetProcAddress; OkuFlow does
// not link or redistribute NVIDIA's proprietary runtime.
class MaxineSuperRes {
public:
    MaxineSuperRes();
    ~MaxineSuperRes();

    MaxineSuperRes(const MaxineSuperRes&) = delete;
    MaxineSuperRes& operator=(const MaxineSuperRes&) = delete;

    bool Ensure(unsigned int sourceWidth,
                unsigned int sourceHeight,
                unsigned int destinationWidth,
                unsigned int destinationHeight,
                void* cudaStream);
    bool Ensure(int sourceWidth, int sourceHeight, int scale, void* cudaStream);
    bool Run(const void* sourceDevicePixels,
             std::size_t sourcePitchBytes,
             void* destinationDevicePixels,
             std::size_t destinationPitchBytes);
    void SetStrength(float strength);
    void Teardown();

    bool IsReady() const;
    bool IsAvailable();
    const std::string& LastError() const;
    const std::wstring& RuntimeDirectory() const;

    static std::wstring FindRuntimeDirectory(const std::wstring& overrideDirectory = {});
    static bool IsRuntimeInstalled(const std::wstring& overrideDirectory = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace okuflow
