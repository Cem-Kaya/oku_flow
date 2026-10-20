#pragma once
#include <cstddef>
#include <cuda_runtime_api.h>

namespace okuflow::uct::gpu
{
// Worker-only resident validation primitives. Rectangles are half-open pixels.
struct Rect { int x, y, w, h; };
struct Colors { int foreground[3], background[3]; }; // RGB
struct Ink { Rect search; double x, y, w, h; int detached; };
struct Fit { double x, y, w, h; };
struct Grid { double x, y, dx, dy, halfW, halfH; int columns, rows; };
struct MatchResult { int visible, matched; double score, x, y; };
struct ProbeMap { double x, y, sx, sy; }; // scene -> pixels: scene*scale+origin
struct ReferenceRequest { Grid grid; float* samples; };
struct MatchRequest { Grid grid; const float* samples; double seedX, seedY, sceneX, sceneY; };
void References(const unsigned*, int, int, const ReferenceRequest*, int, int*, cudaStream_t);
void MatchReferences(const unsigned*, int, int, const MatchRequest*, int, double, double, int,
                     MatchResult*, cudaStream_t);
void ValidationIntegral(const void*, std::size_t, int, int, int, unsigned*, cudaStream_t);
void Reference(const unsigned*, int, int, Grid, float*, int*, cudaStream_t);
void MatchReference(const unsigned*, int, int, Grid, const float*, double, double, int,
                    double, double, double, double, MatchResult*, cudaStream_t);
void Similarity(const unsigned*, int, int, ProbeMap, const unsigned*, int, int, ProbeMap,
                 double, double, double, double, double*, cudaStream_t);
void LineColors(const void*, std::size_t, int, int, const Rect*, int, Colors*, cudaStream_t);
void InkBounds(const void*, std::size_t, int, int, const Ink*, const int*, int,
                const Colors*, Fit*, cudaStream_t);
// Domain-separated SHA-256 tree over every packed BGRA byte (padding excluded).
void FingerprintLeaves(const void*, std::size_t, int, int, unsigned char*, cudaStream_t);
void FingerprintParents(const unsigned char*, int, unsigned char*, cudaStream_t);

struct Quad
{
    float xy[8];
    float score;
    int validWidth;
    int internalEdges{};
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
// 128 detected lines -> at most 128*(original+8 overlapping windows).
void RecognitionWindows(const Quad* detected, int* count, Quad* windows, cudaStream_t stream);
void Rectify(const void *image, std::size_t pitch, int width, int height, const Quad *quads,
             int index, float *out, int cropWidth, cudaStream_t stream);
void Decode(const float *probabilities, int steps, int classes, const Quad *quads, int index,
            int cropWidth, int *ids, float *scores, Token *tokens, int *count, cudaStream_t stream);
} // namespace okuflow::uct::gpu
