#pragma once
#include <cstddef>
#include <cuda_runtime_api.h>
#include <type_traits>
#include "okuflow/common/uct_ribbon.hpp"
#include "okuflow/common/uct_ink_outline_core.hpp"

namespace okuflow::uct::gpu
{
// Worker-only resident validation primitives. Rectangles are half-open pixels.
struct Rect { int x, y, w, h; };
struct Colors { int foreground[3], background[3]; }; // RGB
struct FormulaInputInfo { Rect bounds;Colors colors;int valid; };
// Current validation pixels only. Bounded source-ink colours, 2D bounds and
// model normalization; only the compact descriptor returns to the host.
void FormulaInput(const void*,std::size_t,int,int,const Rect*,FormulaInputInfo*,float*,cudaStream_t);
struct Ink { Rect search; double x, y, w, h; int detached; };
struct Fit { double x, y, w, h; };
struct Grid {
    double x, y, dx, dy, halfW, halfH; int columns, rows;
    double crossX{}, crossY{}, warpX{}, warpY{};
};
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
void RegionalSimilarities(const unsigned*, int, int, ProbeMap, const unsigned*, int, int, ProbeMap,
                          double, double, double, double, double*, cudaStream_t);
struct OrientedInk;
// Optional frames have one descriptor per rectangle; width == 0 retains the
// rectangle path. Nonzero frames sample body/corners/perimeters in local space.
void LineColors(const void*, std::size_t, int, int, const Rect*, int, Colors*, cudaStream_t,
                const OrientedInk* frames = nullptr);
void InkBounds(const void*, std::size_t, int, int, const Ink*, const int*, int,
                const Colors*, Fit*, cudaStream_t);
// Rotated-frame counterpart of InkBounds, sample-for-sample identical to the
// CPU FitOrientedWordInk reference. Origin/axes are the search quad's TL and
// unit edge directions in pixels; the fit is returned in local pixel units.
struct OrientedInk { double ox, oy, ux, uy, vx, vy, width, height, detectedWidth; int detached; double warpX{}, warpY{}; };
inline constexpr int kOrientedInkMaxPropagationPasses = 64;
enum class OrientedFitReason : int { InvalidInput, Incomplete, UnanchoredInk, TooFewSamples,
                                     AmbiguousBands, ShrinkGuard, Fitted };
struct OrientedFit { double u0, u1, v0, v1; int fitted; int propagationPasses{}, propagationComplete{};
                     OrientedFitReason reason{OrientedFitReason::InvalidInput}; };
void OrientedInkBounds(const void*, std::size_t, int, int, const OrientedInk*, const int*, int,
                       const Colors*, OrientedFit*, cudaStream_t);
struct SurfaceRequest { OrientedInk frame; int colorIndex,fitIndex,oriented; };
struct SurfaceFit { double xy[8]; int fitted; };
// Optional closed-surface perspective, at most 128 requests; metadata only.
void SurfaceFrames(const void*,std::size_t,int,int,const SurfaceRequest*,int,const Colors*,
                   const Fit*,const OrientedFit*,SurfaceFit*,cudaStream_t);
// Domain-separated SHA-256 tree over every packed BGRA byte (padding excluded).
void FingerprintLeaves(const void*, std::size_t, int, int, unsigned char*, cudaStream_t);
void FingerprintParents(const unsigned char*, int, unsigned char*, cudaStream_t);

struct Quad
{
    float xy[8];
    float score;
    int validWidth;
    int internalEdges{};
    ribbon::Geometry ribbon;
    int componentRoot{-1}; // worker-private detector component identity
};
// Quad arrays cross the MSVC/nvcc boundary by byte copy; lock the layout.
static_assert(std::is_standard_layout_v<ribbon::Geometry>);
static_assert(sizeof(ribbon::Bin) == 32);
static_assert(sizeof(ribbon::Geometry) == 144);
static_assert(std::is_standard_layout_v<Quad>);
static_assert(offsetof(Quad, ribbon) == 48);
static_assert(offsetof(Quad, componentRoot) == 192);
static_assert(sizeof(Quad) == 200);
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
// One bounded original-ink region. Raster coverage and scan scratch stay on
// device. Output is four fixed-capacity lists of contour segments + scalars.
void InkOutline(const void*,std::size_t,int,int,ink::Region,unsigned char*,int*,int*,
                ink::Summary*,ink::Segment*,cudaStream_t);
void Probe(const void *image, std::size_t pitch, int width, int height, void *out, int decimation,
           cudaStream_t stream);
void Normalize(const void *image, std::size_t pitch, int width, int height, float *out,
               int outWidth, int outHeight, cudaStream_t stream);
// Detector post-processing implementations. The original kernels stay
// selectable as the comparison oracle (Reference()).
//
// Defaults, on every GPU: path-compressing union-find roots and four ribbon
// bins merged per round, both exact by construction, and treeBoxes: one warp
// per component whose lanes sum their pixels in double and combine through a
// fixed shuffle tree. That sum is defined and repeatable: the membership
// test and the order of additions are spelled out. (A component's extents and
// scan bounds still follow the compiler's rounding per architecture, so scores
// are not claimed equal across GPU generations.) It is not the original
// raster-order float sum: a score differs by the original's accumulated
// rounding, which reaches the output only when a component's mean lands that
// close to the 0.4 acceptance threshold. The owner accepted differences of
// that size for the speed (docs/uct_okuread_port.md, round 9).
//
// cooperativeBoxes without treeBoxes is the exact variant: the original
// raster-order float sum, reproduced without the serial chain. It also has to
// select the same pixels as the original kernel, whose inclusion test rounds
// the way one compiler happened to compile it. That was executed bit for bit
// on sm_89 with the CUDA 12.9 toolkit, and sm_86 has the same machine code for
// both kernels; Exact() selects it only there and the original box kernel
// everywhere else.
inline constexpr bool kCooperativeBoxesToolkit = CUDART_VERSION >= 12090 && CUDART_VERSION < 12100;
struct DetectKernels
{
    bool compressRoots{true}, cooperativeBoxes{false}, parallelRibbonMerge{true}, treeBoxes{true};
    static constexpr DetectKernels Reference() { return {false, false, false, false}; }
    // The fastest selection whose output equals Reference() bit for bit on a
    // device with this compute capability.
    static constexpr DetectKernels Exact(int computeMajor, int computeMinor)
    {
        return {true, kCooperativeBoxesToolkit && computeMajor == 8 && (computeMinor == 6 || computeMinor == 9),
                true, false};
    }
};
// Test support. For each of `groups` warps: adds the masked values (32 per
// group, lane order) to scores[group] with the exact group sum box scoring
// uses. fast[group] is 1 when that sum applied and results[group] then holds
// the float a sequential fold produces; 0 means box scoring would fold this
// group serially and results[group] is unspecified.
void BoxSumGroups(const float *scores, const unsigned *masks, const float *values, int groups,
                  float *results, int *fast, cudaStream_t stream);
void Detect(const float *map, int width, int height, int sourceWidth, int sourceHeight, int *labels,
            Component *components, Quad *quads, int *count, cudaStream_t stream,
            int* ribbonOffsets = nullptr, DetectKernels kernels = {});
// 128 detected lines -> at most 128*(original+8 overlapping windows).
void RecognitionWindows(const Quad* detected, int* count, Quad* windows, cudaStream_t stream);
// Compact ordered-group canvas widths; all ceil(1152/groupSize) slots are
// initialized. Optional width policy only, with no image transfer or sorting.
void RecognitionGroupWidths(const Quad*, const int* count, int groupSize, int* widths, cudaStream_t);
void Rectify(const void *image, std::size_t pitch, int width, int height, const Quad *quads,
             int index, float *out, int cropWidth, cudaStream_t stream);
// Contiguous detector order, 1..16 samples; out is [samples,3,48,cropWidth].
// Uses exactly the scalar Rectify sampling/padding arithmetic, without indices.
void RectifyBatch(const void *image, std::size_t pitch, int width, int height, const Quad *quads,
                  int first, int samples, float *out, int cropWidth, cudaStream_t stream);
void Decode(const float *probabilities, int steps, int classes, const Quad *quads, int index,
            int cropWidth, int *ids, float *scores, Token *tokens, int *count, cudaStream_t stream);
// Actual [samples,steps,classes] output, contiguous quads[first..first+samples).
// IDs/scores/tokens have sample stride steps; counts has one entry per sample.
// Two launches; all probability rows are checked, including the unused suffix.
void DecodeBatch(const float *probabilities, int samples, int steps, int classes,
                 const Quad *quads, int first, int cropWidth, int *ids, float *scores,
                 Token *tokens, int *counts, cudaStream_t stream);
} // namespace okuflow::uct::gpu
