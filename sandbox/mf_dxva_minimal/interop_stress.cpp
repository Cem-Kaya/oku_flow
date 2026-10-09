#ifdef _WIN32

#include "interop_stress.hpp"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#if OKUFLOW_PROBE_HAS_CUDA
#include <cuda_d3d11_interop.h>
#include <cuda_runtime.h>
#endif

#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

using Microsoft::WRL::ComPtr;

std::string HrText(HRESULT result)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                  static_cast<unsigned long>(result));
    return buffer;
}

void CheckHr(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        throw std::runtime_error(
            std::string(operation) + " failed (" + HrText(result) + ")");
    }
}

#if OKUFLOW_PROBE_HAS_CUDA

void CheckCuda(cudaError_t result, const char* operation)
{
    if (result != cudaSuccess) {
        throw std::runtime_error(
            std::string(operation) + " failed (" +
            cudaGetErrorString(result) + ")");
    }
}

class WindowsSecurityAttributes {
public:
    WindowsSecurityAttributes()
    {
        CheckHr(
            InitializeSecurityDescriptor(
                &descriptor_, SECURITY_DESCRIPTOR_REVISION)
                ? S_OK
                : HRESULT_FROM_WIN32(GetLastError()),
            "InitializeSecurityDescriptor");
        CheckHr(
            SetSecurityDescriptorDacl(
                &descriptor_, TRUE, nullptr, FALSE)
                ? S_OK
                : HRESULT_FROM_WIN32(GetLastError()),
            "SetSecurityDescriptorDacl");
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
    }

    SECURITY_ATTRIBUTES* get()
    {
        return &attributes_;
    }

private:
    SECURITY_ATTRIBUTES attributes_{};
    SECURITY_DESCRIPTOR descriptor_{};
};

class HandleScope {
public:
    ~HandleScope()
    {
        reset();
    }

    HANDLE* put()
    {
        reset();
        return &handle_;
    }

    HANDLE get() const
    {
        return handle_;
    }

    void reset()
    {
        if (handle_) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HANDLE handle_{};
};

int SelectCudaDevice(ID3D11Device* device)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    CheckHr(
        device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf())),
        "QueryInterface(IDXGIDevice)");
    ComPtr<IDXGIAdapter> adapter;
    CheckHr(
        dxgiDevice->GetAdapter(adapter.GetAddressOf()),
        "IDXGIDevice::GetAdapter");
    DXGI_ADAPTER_DESC adapterDescription{};
    CheckHr(
        adapter->GetDesc(&adapterDescription),
        "IDXGIAdapter::GetDesc");

    int count = 0;
    CheckCuda(cudaGetDeviceCount(&count), "cudaGetDeviceCount");
    for (int index = 0; index < count; ++index) {
        cudaDeviceProp properties{};
        CheckCuda(
            cudaGetDeviceProperties(&properties, index),
            "cudaGetDeviceProperties");
        if (std::memcmp(
                properties.luid, &adapterDescription.AdapterLuid,
                sizeof(adapterDescription.AdapterLuid)) == 0) {
            CheckCuda(cudaSetDevice(index), "cudaSetDevice");
            return index;
        }
    }
    throw std::runtime_error(
        "No CUDA device matches the D3D11 camera adapter LUID");
}

InteropStressResult RunLegacyStress(
    ID3D11Device* device,
    ID3D11Texture2D* texture,
    const D3D11_TEXTURE2D_DESC& description,
    int iterations)
{
    InteropStressResult result;
    result.path = "legacyD3D11Registration";
    result.iterationsRequested = iterations;
    SelectCudaDevice(device);

    std::uint8_t* destination = nullptr;
    std::size_t destinationPitch = 0;
    CheckCuda(
        cudaMallocPitch(
            reinterpret_cast<void**>(&destination),
            &destinationPitch,
            static_cast<std::size_t>(description.Width) * 4u,
            description.Height),
        "cudaMallocPitch");
    const auto releaseDestination = [&]() {
        if (destination) {
            cudaFree(destination);
            destination = nullptr;
        }
    };

    const auto started = std::chrono::steady_clock::now();
    try {
        for (int iteration = 0; iteration < iterations; ++iteration) {
            cudaGraphicsResource_t resource = nullptr;
            CheckCuda(
                cudaGraphicsD3D11RegisterResource(
                    &resource, texture, cudaGraphicsRegisterFlagsNone),
                "cudaGraphicsD3D11RegisterResource");
            bool mapped = false;
            try {
                CheckCuda(
                    cudaGraphicsMapResources(1, &resource, nullptr),
                    "cudaGraphicsMapResources");
                mapped = true;
                cudaArray_t array = nullptr;
                CheckCuda(
                    cudaGraphicsSubResourceGetMappedArray(
                        &array, resource, 0, 0),
                    "cudaGraphicsSubResourceGetMappedArray");
                CheckCuda(
                    cudaMemcpy2DFromArray(
                        destination,
                        destinationPitch,
                        array,
                        0,
                        0,
                        static_cast<std::size_t>(description.Width) * 4u,
                        description.Height,
                        cudaMemcpyDeviceToDevice),
                    "cudaMemcpy2DFromArray");
                CheckCuda(
                    cudaDeviceSynchronize(),
                    "cudaDeviceSynchronize");
                CheckCuda(
                    cudaGraphicsUnmapResources(1, &resource, nullptr),
                    "cudaGraphicsUnmapResources");
                mapped = false;
                CheckCuda(
                    cudaGraphicsUnregisterResource(resource),
                    "cudaGraphicsUnregisterResource");
                resource = nullptr;
            } catch (...) {
                if (mapped) {
                    (void)cudaGraphicsUnmapResources(
                        1, &resource, nullptr);
                }
                if (resource) {
                    (void)cudaGraphicsUnregisterResource(resource);
                }
                throw;
            }
            ++result.iterationsCompleted;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started);
        result.averageIterationMs =
            elapsed.count() / static_cast<double>(iterations);
        result.status = "ok";
        result.detail =
            "Legacy CUDA-D3D11 register/map/unregister loop completed";
    } catch (const std::exception& error) {
        result.detail = error.what();
    }
    releaseDestination();
    return result;
}

InteropStressResult RunExternalMemoryStress(
    ID3D11Device* device,
    ID3D11Texture2D* texture,
    const D3D11_TEXTURE2D_DESC& description,
    int iterations)
{
    InteropStressResult result;
    result.path = "d3d11D3d12CudaExternalMemory";
    result.iterationsRequested = iterations;
    SelectCudaDevice(device);

    ComPtr<IDXGIDevice> dxgiDevice;
    CheckHr(
        device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf())),
        "QueryInterface(IDXGIDevice)");
    ComPtr<IDXGIAdapter> adapter;
    CheckHr(
        dxgiDevice->GetAdapter(adapter.GetAddressOf()),
        "IDXGIDevice::GetAdapter");
    ComPtr<ID3D12Device> d3d12Device;
    CheckHr(
        D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(d3d12Device.GetAddressOf())),
        "D3D12CreateDevice");

    ComPtr<IDXGIResource1> dxgiResource;
    CheckHr(
        texture->QueryInterface(IID_PPV_ARGS(dxgiResource.GetAddressOf())),
        "QueryInterface(IDXGIResource1)");
    HandleScope d3d11Handle;
    CheckHr(
        dxgiResource->CreateSharedHandle(
            nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr,
            d3d11Handle.put()),
        "IDXGIResource1::CreateSharedHandle");

    std::uint8_t* destination = nullptr;
    std::size_t destinationPitch = 0;
    CheckCuda(
        cudaMallocPitch(
            reinterpret_cast<void**>(&destination),
            &destinationPitch,
            static_cast<std::size_t>(description.Width) * 4u,
            description.Height),
        "cudaMallocPitch");
    const auto releaseDestination = [&]() {
        if (destination) {
            cudaFree(destination);
            destination = nullptr;
        }
    };

    const auto started = std::chrono::steady_clock::now();
    try {
        for (int iteration = 0; iteration < iterations; ++iteration) {
            ComPtr<ID3D12Resource> d3d12Resource;
            CheckHr(
                d3d12Device->OpenSharedHandle(
                    d3d11Handle.get(),
                    IID_PPV_ARGS(d3d12Resource.GetAddressOf())),
                "ID3D12Device::OpenSharedHandle");
            WindowsSecurityAttributes security;
            HandleScope d3d12Handle;
            CheckHr(
                d3d12Device->CreateSharedHandle(
                    d3d12Resource.Get(),
                    security.get(),
                    GENERIC_ALL,
                    nullptr,
                    d3d12Handle.put()),
                "ID3D12Device::CreateSharedHandle");

            const D3D12_RESOURCE_DESC resourceDescription =
                d3d12Resource->GetDesc();
            const D3D12_RESOURCE_ALLOCATION_INFO allocation =
                d3d12Device->GetResourceAllocationInfo(
                    0, 1, &resourceDescription);
            if (allocation.SizeInBytes == 0 ||
                allocation.SizeInBytes == UINT64_MAX) {
                throw std::runtime_error(
                    "D3D12 returned an invalid shared allocation size");
            }

            cudaExternalMemory_t externalMemory = nullptr;
            cudaMipmappedArray_t mipArray = nullptr;
            try {
                cudaExternalMemoryHandleDesc memoryDescription{};
                memoryDescription.type =
                    cudaExternalMemoryHandleTypeD3D12Resource;
                memoryDescription.handle.win32.handle =
                    d3d12Handle.get();
                memoryDescription.size = allocation.SizeInBytes;
                memoryDescription.flags = cudaExternalMemoryDedicated;
                CheckCuda(
                    cudaImportExternalMemory(
                        &externalMemory, &memoryDescription),
                    "cudaImportExternalMemory");

                cudaExternalMemoryMipmappedArrayDesc arrayDescription{};
                arrayDescription.offset = 0;
                arrayDescription.numLevels = 1;
                arrayDescription.extent =
                    make_cudaExtent(
                        description.Width, description.Height, 1);
                arrayDescription.formatDesc =
                    cudaCreateChannelDesc<uchar4>();
                arrayDescription.flags =
                    cudaArraySurfaceLoadStore |
                    cudaArrayColorAttachment;
                CheckCuda(
                    cudaExternalMemoryGetMappedMipmappedArray(
                        &mipArray,
                        externalMemory,
                        &arrayDescription),
                    "cudaExternalMemoryGetMappedMipmappedArray");
                cudaArray_t level0 = nullptr;
                CheckCuda(
                    cudaGetMipmappedArrayLevel(&level0, mipArray, 0),
                    "cudaGetMipmappedArrayLevel");
                CheckCuda(
                    cudaMemcpy2DFromArray(
                        destination,
                        destinationPitch,
                        level0,
                        0,
                        0,
                        static_cast<std::size_t>(description.Width) * 4u,
                        description.Height,
                        cudaMemcpyDeviceToDevice),
                    "cudaMemcpy2DFromArray");
                CheckCuda(
                    cudaDeviceSynchronize(),
                    "cudaDeviceSynchronize");
                CheckCuda(
                    cudaFreeMipmappedArray(mipArray),
                    "cudaFreeMipmappedArray");
                mipArray = nullptr;
                CheckCuda(
                    cudaDestroyExternalMemory(externalMemory),
                    "cudaDestroyExternalMemory");
                externalMemory = nullptr;
            } catch (...) {
                if (mipArray) {
                    (void)cudaFreeMipmappedArray(mipArray);
                }
                if (externalMemory) {
                    (void)cudaDestroyExternalMemory(externalMemory);
                }
                throw;
            }
            ++result.iterationsCompleted;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started);
        result.averageIterationMs =
            elapsed.count() / static_cast<double>(iterations);
        result.status = "ok";
        result.detail =
            "D3D11 -> D3D12 -> CUDA external-memory loop completed";
    } catch (const std::exception& error) {
        result.detail = error.what();
    }
    releaseDestination();
    return result;
}

#endif

} // namespace

InteropStressResult RunCudaInteropStress(
    ID3D11Device* device,
    ID3D11DeviceContext*,
    ID3D11Texture2D* texture,
    InteropStressMode mode,
    int iterations)
{
    InteropStressResult result;
    result.path =
        mode == InteropStressMode::ExternalMemory
            ? "d3d11D3d12CudaExternalMemory"
            : "legacyD3D11Registration";
    result.iterationsRequested = iterations;
    if (!device || !texture || iterations <= 0) {
        result.detail = "Invalid stress-test input";
        return result;
    }
#if !OKUFLOW_PROBE_HAS_CUDA
    result.status = "skip";
    result.detail = "This probe build does not include CUDA";
    return result;
#else
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        description.Width == 0 || description.Height == 0) {
        result.detail =
            "Stress texture must be a non-empty BGRA8 allocation";
        return result;
    }
    try {
        return mode == InteropStressMode::ExternalMemory
                   ? RunExternalMemoryStress(
                         device, texture, description, iterations)
                   : RunLegacyStress(
                         device, texture, description, iterations);
    } catch (const std::exception& error) {
        result.detail = error.what();
        return result;
    }
#endif
}

#endif
