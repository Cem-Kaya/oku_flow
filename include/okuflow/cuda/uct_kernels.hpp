#pragma once
#include <cstddef>
#include <cuda_runtime_api.h>

namespace okuflow::uct::gpu
{
struct Quad
{
    float xy[8];
    float score;
    int validWidth;
};
struct Token
{
    int id, first, last;
    float confidence;
};
struct Component
{
    int n;
    long long x, y, xx, yy, xy;
    float c, s, u0, u1, v0, v1;
    float score;
};
void Probe(const void *image, std::size_t pitch, int width, int height, void *out, int decimation,
           cudaStream_t stream);
void Normalize(const void *image, std::size_t pitch, int width, int height, float *out,
               int outWidth, int outHeight, cudaStream_t stream);
void Detect(const float *map, int width, int height, int sourceWidth, int sourceHeight, int *labels,
            Component *components, Quad *quads, int *count, cudaStream_t stream);
void Rectify(const void *image, std::size_t pitch, int width, int height, const Quad *quads,
             int index, float *out, int cropWidth, cudaStream_t stream);
void Decode(const float *probabilities, int steps, int classes, const Quad *quads, int index,
            int cropWidth, int *ids, float *scores, Token *tokens, int *count, cudaStream_t stream);
} // namespace okuflow::uct::gpu
