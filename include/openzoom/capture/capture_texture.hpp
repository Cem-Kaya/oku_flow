#pragma once

#ifdef _WIN32
#include <d3d11.h>
#include <algorithm>

namespace openzoom {

// Copy only matching BGRA pixels. YUV/RGBA conversion belongs to the video
// processor. The caller owns source sample retention, output reuse, and the
// completion query; successful submission does not imply GPU completion.
inline bool CopyBgraCaptureTexture(ID3D11DeviceContext* context,
                                   ID3D11Texture2D* source,
                                   UINT sourceSubresource,
                                   ID3D11Texture2D* destination,
                                   UINT width, UINT height)
{
    if (!context || !source || !destination || source == destination ||
        width == 0 || height == 0) return false;
    D3D11_TEXTURE2D_DESC input{}, output{};
    source->GetDesc(&input);
    destination->GetDesc(&output);
    const UINT mipLevels = std::max(1u, input.MipLevels);
    const UINT mip = sourceSubresource % mipLevels;
    if (input.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        output.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        input.SampleDesc.Count != 1 || output.SampleDesc.Count != 1 ||
        output.Width != width || output.Height != height ||
        output.ArraySize != 1 || output.MipLevels != 1 ||
        sourceSubresource / mipLevels >= input.ArraySize || mip >= 32 ||
        width > std::max(1u, input.Width >> mip) ||
        height > std::max(1u, input.Height >> mip)) return false;
    const D3D11_BOX visible{0, 0, 0, width, height, 1};
    context->CopySubresourceRegion(destination, 0, 0, 0, 0,
                                   source, sourceSubresource, &visible);
    return true;
}

} // namespace openzoom
#endif
