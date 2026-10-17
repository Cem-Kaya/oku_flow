#include "maxine_fake_sdk.hpp"
#include <cuda.h>
#include <atomic>
#include <cstdlib>
#include <cstring>

// Match the production adapter's C ABI include without emitting the SDK's
// dllexported C++ convenience wrapper or depending on a proprietary runtime.
#pragma push_macro("_MSC_VER")
#undef _MSC_VER
#include "nvVideoEffects.h"
#pragma pop_macro("_MSC_VER")

namespace {
std::atomic_uint allocated{}, deallocated{}, created{}, destroyed{}, loaded{}, ran{}, transferred{};
std::atomic_uint inputTransfers{}, outputTransfers{};
std::atomic_int lastDevice{-1}, failLoad{}, failInputTransfer{}, failEffectRun{}, failOutputTransfer{};
std::atomic<DWORD> lastThread{};
std::atomic<HANDLE> runEntered{}, runRelease{}, runReturned{};
std::atomic_bool fillOutput{};
std::atomic_uint outputBgra{}, inputFirstPixel{};
std::atomic_bool echoInput{};

void ObserveDevice() {
    CUdevice device = -1;
    lastDevice = cuCtxGetDevice(&device) == CUDA_SUCCESS ? device : -1;
    lastThread = GetCurrentThreadId();
}
struct Effect {
    NvCVImage* input{};
    NvCVImage* output{};
    CUstream stream{};
};
} // namespace

extern "C" __declspec(dllexport) void __cdecl OkuFlowFakeRead(MaxineFakeSnapshot* result) {
    *result = {allocated.load(), deallocated.load(), created.load(), destroyed.load(),
        loaded.load(), ran.load(), transferred.load(), inputTransfers.load(), outputTransfers.load(),
        lastDevice.load(), lastThread.load(), inputFirstPixel.load()};
}
extern "C" __declspec(dllexport) void __cdecl OkuFlowFakeReset() {
    allocated = deallocated = created = destroyed = loaded = ran = transferred = 0;
    inputTransfers = outputTransfers = 0;
    lastDevice = -1;
    lastThread = 0;
    failLoad = 0;
    failInputTransfer = failEffectRun = failOutputTransfer = 0;
    runEntered = runRelease = runReturned = nullptr;
    fillOutput = false;
    outputBgra = 0; inputFirstPixel = 0; echoInput = false;
}
extern "C" __declspec(dllexport) void __cdecl OkuFlowFakeFailLoad(int status) { failLoad = status; }
extern "C" __declspec(dllexport) void __cdecl OkuFlowFakeFailStage(MaxineFakeStage stage, int status) {
    switch (stage) {
    case MaxineFakeStage::InputTransfer: failInputTransfer = status; break;
    case MaxineFakeStage::EffectRun: failEffectRun = status; break;
    case MaxineFakeStage::OutputTransfer: failOutputTransfer = status; break;
    }
}
extern "C" __declspec(dllexport) void __cdecl OkuFlowFakeBlockRun(HANDLE entered, HANDLE release, HANDLE returned) {
    runEntered = entered;
    runRelease = release;
    runReturned = returned;
}
extern "C" __declspec(dllexport) void __cdecl OkuFlowFakeFillOutput(bool enabled, unsigned int bgra) {
    outputBgra = bgra;
    fillOutput = enabled;
}
extern "C" __declspec(dllexport) void __cdecl OkuFlowFakeEchoInput(bool enabled) { echoInput = enabled; }
extern "C" __declspec(dllexport) const char* __cdecl NvCV_GetErrorStringFromCode(NvCV_Status) {
    return "private controlled fake SDK status";
}

#if defined(OKUFLOW_FAKE_IMAGE_MODULE)
extern "C" NvCV_Status NvCV_API NvCVImage_Alloc(
    NvCVImage* image, unsigned width, unsigned height, NvCVImage_PixelFormat format,
    NvCVImage_ComponentType type, unsigned layout, unsigned memory, unsigned) {
    ObserveDevice();
    const size_t bytes = static_cast<size_t>(width) * height * 3 * sizeof(float);
    void* pixels = std::malloc(bytes);
    if (!pixels) return NVCV_ERR_GENERAL;
    std::memset(image, 0, sizeof(*image));
    image->width = width;
    image->height = height;
    image->pitch = static_cast<int>(width * sizeof(float));
    image->pixelFormat = format;
    image->componentType = type;
    image->planar = static_cast<unsigned char>(layout);
    image->gpuMem = static_cast<unsigned char>(memory);
    image->pixels = pixels;
    image->bufferBytes = bytes;
    ++allocated;
    return NVCV_SUCCESS;
}
extern "C" void NvCV_API NvCVImage_Dealloc(NvCVImage* image) {
    ObserveDevice();
    if (image->pixels) {
        std::free(image->pixels);
        ++deallocated;
    }
    std::memset(image, 0, sizeof(*image));
}
extern "C" NvCV_Status NvCV_API NvCVImage_Transfer(
    const NvCVImage* source, NvCVImage* destination, float, CUstream stream, NvCVImage*) {
    ObserveDevice();
    if (!source || !destination || !source->pixels || !destination->pixels || !stream)
        return NVCV_ERR_PARAMETER;
    const bool input = source->componentType == NVCV_U8;
    if (input) {
        ++inputTransfers;
        if (echoInput) {
            unsigned pixel{};
            if (cuStreamSynchronize(stream) != CUDA_SUCCESS ||
                cuMemcpyDtoH(&pixel, reinterpret_cast<CUdeviceptr>(source->pixels), sizeof(pixel)) != CUDA_SUCCESS)
                return NVCV_ERR_CUDA;
            inputFirstPixel = pixel;
        }
    } else ++outputTransfers;
    const int failure = input ? failInputTransfer.exchange(0) : failOutputTransfer.exchange(0);
    if (failure) return static_cast<NvCV_Status>(failure);
    if (!input && fillOutput && cuMemsetD2D32Async(
            reinterpret_cast<CUdeviceptr>(destination->pixels), static_cast<size_t>(destination->pitch),
            (echoInput ? inputFirstPixel.load() : outputBgra.load()), destination->width, destination->height, stream) != CUDA_SUCCESS)
        return NVCV_ERR_CUDA;
    ++transferred;
    // This fixture validates ownership and ABI transitions, never SR pixels.
    return NVCV_SUCCESS;
}
#else
extern "C" NvCV_Status NvVFX_API NvVFX_CreateEffect(NvVFX_EffectSelector, NvVFX_Handle* handle) {
    ObserveDevice();
    *handle = reinterpret_cast<NvVFX_Handle>(new Effect);
    ++created;
    return NVCV_SUCCESS;
}
extern "C" void NvVFX_API NvVFX_DestroyEffect(NvVFX_Handle handle) {
    ObserveDevice();
    delete reinterpret_cast<Effect*>(handle);
    ++destroyed;
}
extern "C" NvCV_Status NvVFX_API NvVFX_SetU32(NvVFX_Handle, NvVFX_ParameterSelector, unsigned) {
    return NVCV_SUCCESS;
}
extern "C" NvCV_Status NvVFX_API NvVFX_SetF32(NvVFX_Handle, NvVFX_ParameterSelector, float) {
    return NVCV_SUCCESS;
}
extern "C" NvCV_Status NvVFX_API NvVFX_SetString(NvVFX_Handle, NvVFX_ParameterSelector, const char*) {
    return NVCV_SUCCESS;
}
extern "C" NvCV_Status NvVFX_API NvVFX_SetImage(
    NvVFX_Handle handle, NvVFX_ParameterSelector selector, NvCVImage* image) {
    auto* effect = reinterpret_cast<Effect*>(handle);
    if (std::strcmp(selector, NVVFX_INPUT_IMAGE) == 0) effect->input = image;
    else if (std::strcmp(selector, NVVFX_OUTPUT_IMAGE) == 0) effect->output = image;
    else return NVCV_ERR_PARAMETER;
    return NVCV_SUCCESS;
}
extern "C" NvCV_Status NvVFX_API NvVFX_SetCudaStream(
    NvVFX_Handle handle, NvVFX_ParameterSelector, CUstream stream) {
    reinterpret_cast<Effect*>(handle)->stream = stream;
    return NVCV_SUCCESS;
}
extern "C" NvCV_Status NvVFX_API NvVFX_Load(NvVFX_Handle handle) {
    ObserveDevice();
    ++loaded;
    const int failure = failLoad.exchange(0);
    if (failure) return static_cast<NvCV_Status>(failure);
    const auto* effect = reinterpret_cast<Effect*>(handle);
    return effect->input && effect->output && effect->stream ? NVCV_SUCCESS : NVCV_ERR_PARAMETER;
}
extern "C" NvCV_Status NvVFX_API NvVFX_Run(NvVFX_Handle, int asynchronous) {
    ObserveDevice();
    ++ran;
    if (asynchronous) return NVCV_ERR_PARAMETER;
    const int failure = failEffectRun.exchange(0);
    if (failure) return static_cast<NvCV_Status>(failure);
    const auto entered = runEntered.load();
    const auto release = runRelease.load();
    const auto returned = runReturned.load();
    if (entered) SetEvent(entered);
    if (release && WaitForSingleObject(release, INFINITE) != WAIT_OBJECT_0) return NVCV_ERR_GENERAL;
    if (returned) SetEvent(returned);
    return NVCV_SUCCESS;
}
#endif
