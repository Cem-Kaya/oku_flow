#pragma once

// First-party, model-free recognition geometry. No image/proof storage.
#include <cmath>

#if defined(__CUDACC__)
#define OKUFLOW_RIBBON_HD __host__ __device__
#else
#define OKUFLOW_RIBBON_HD
#endif

namespace okuflow::uct::ribbon {
inline constexpr int kBins = 24;
inline constexpr int kMaximumFitPixels = 262144;
inline constexpr double kMaximumWordErrorPx = 1.0;
inline constexpr double kMaximumWordErrorHeight = .08;
struct Point { double x{}, y{}; };
struct Bin { int count{}; double low{1e30}, high{-1e30}, sumU{}; };

// t is the detector PCA coordinate, not arc length. Cross sections use the
// PCA transverse axis (not intersecting curve normals). Positive height makes
// this quadratic shear bijective. start/end restrict a recognition window.
struct Geometry {
    double ox{}, oy{}, ux{}, uy{}, vx{}, vy{};
    double a{}, b{}, c{}, h0{}, h1{};
    double start{}, end{1};
    bool valid{};
    bool unsupported{}; // curved hypothesis failed a support check; keep source ink
    // Full detector envelope survives overlapping recognition windows, so a
    // partial window cannot shrink protection for an unsupported curve.
    double protectLeft{},protectTop{},protectRight{},protectBottom{};
    OKUFLOW_RIBBON_HD Point At(double u, double v) const {
        const double t = start + (end - start) * u;
        const double cross = a + t * (b + t * c) + (2 * v - 1) * (h0 + h1 * t);
        return {ox + ux * t + vx * cross, oy + uy * t + vy * cross};
    }
    // Exact maximum departure from the endpoint chord: affine height and
    // quadratic center imply the same sagitta on every v cross section.
    OKUFLOW_RIBBON_HD double Error(double u0, double u1) const {
        const double dt = (end - start) * (u1 - u0);
        return ::fabs(c) * dt * dt * .25 * ::sqrt(vx * vx + vy * vy);
    }
    OKUFLOW_RIBBON_HD double Width() const {
        double length = 0;
        auto previous = At(0, .5);
        for (int i = 1; i <= 32; ++i) {
            const auto p = At(i / 32., .5);
            length += ::hypot(p.x - previous.x, p.y - previous.y); previous = p;
        }
        return length;
    }
    OKUFLOW_RIBBON_HD double Height() const {
        const double t = (start + end) * .5;
        return 2 * (h0 + h1 * t) * ::hypot(vx, vy);
    }
    OKUFLOW_RIBBON_HD bool Inside(int width,int height) const {
        const double fields[]{ox,oy,ux,uy,vx,vy,a,b,c,h0,h1,start,end};
        for(double value:fields) if(value!=value||::fabs(value)>=1e20) return false;
        if(start<0||end>1||end<=start||h0+h1*start<=0||h0+h1*end<=0||
           ::fabs(ux*vy-uy*vx)<1e-8) return false;
        // Evaluate the exact extrema of both quadratic boundaries, not only
        // a sparse grid that could miss an out-of-image vertex.
        for(int side=0;side<2;++side)for(int axis=0;axis<2;++axis) {
            const double cross=axis?vy:vx,along=axis?uy:ux;
            const double linear=along+cross*(b+(2*side-1)*h1),quadratic=cross*c;
            const double vertex=quadratic!=0?-linear/(2*quadratic):start;
            const double candidates[]{start,end,vertex};
            for(double t:candidates) if(t>=start&&t<=end) {
                const auto p=At((t-start)/(end-start),side);const double value=axis?p.y:p.x;
                if(value<0||value>(axis?height:width)||value!=value) return false;
            }
        }
        return true;
    }
};

OKUFLOW_RIBBON_HD inline bool Finite(double v) { return v == v && ::fabs(v) < 1e20; }
OKUFLOW_RIBBON_HD inline void Add(Bin* bins, double t, double v) {
    int i = int(t * kBins); if (i < 0) i = 0; if (i >= kBins) i = kBins - 1;
    auto& bin = bins[i]; ++bin.count; bin.sumU += t;
    if (v < bin.low) bin.low = v; if (v > bin.high) bin.high = v;
}

// Bounded least-squares center fit to component cross-section envelopes.
// Refuse holes, degenerate shapes, poorly fitted envelopes and image edges;
// a refusal means the established rectangle, never guessed curved geometry.
OKUFLOW_RIBBON_HD inline Geometry Fit(const Bin* bins, double u0, double u1,
    double cosAxis, double sinAxis, double scaleX, double scaleY, int sw, int sh,
    bool endpointPadding = true) {
    Geometry g;
    const double length = u1 - u0;
    if (!Finite(length) || length < 24 || scaleX <= 0 || scaleY <= 0) return g;
    double matrix[3][4]{}; double meanHeight = 0; int occupied = 0;
    for (int i = 0; i < kBins; ++i) {
        const auto& bin = bins[i]; if (bin.count < 2) continue;
        ++occupied;
        const double t = bin.sumU / bin.count, v = (bin.low + bin.high) * .5;
        const double powers[5]{1, t, t*t, t*t*t, t*t*t*t};
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) matrix[row][col] += powers[row+col];
            matrix[row][3] += powers[row] * v;
        }
        meanHeight += bin.high - bin.low + 1;
    }
    // Full, compact component support only. No extrapolation through gutters.
    if (occupied < 20 || bins[0].count < 2 || bins[kBins-1].count < 2) return g;
    meanHeight /= occupied;
    if (meanHeight < 3 || length < 2 * meanHeight) return g;
    for (int col = 0; col < 3; ++col) {
        int pivot = col;
        for (int row = col+1; row < 3; ++row)
            if (::fabs(matrix[row][col]) > ::fabs(matrix[pivot][col])) pivot = row;
        if (::fabs(matrix[pivot][col]) < 1e-8) return g;
        for (int j = col; j < 4; ++j) {
            const double old = matrix[col][j]; matrix[col][j] = matrix[pivot][j]; matrix[pivot][j] = old;
        }
        const double divisor = matrix[col][col];
        for (int j = col; j < 4; ++j) matrix[col][j] /= divisor;
        for (int row = 0; row < 3; ++row) if (row != col) {
            const double factor = matrix[row][col];
            for (int j = col; j < 4; ++j) matrix[row][j] -= factor * matrix[col][j];
        }
    }
    g.a = matrix[0][3]; g.b = matrix[1][3]; g.c = matrix[2][3];
    // Do not perturb the existing straight-line branch, even when opted in.
    if (!Finite(g.c) || ::fabs(g.c) * .25 < ::fmax(.75, meanHeight * .08)) return {};
    g.unsupported=true;
    if (::fmax(::fabs(g.b), ::fabs(g.b + 2*g.c)) / length > 1) return g;
    double sumT=0, sumTT=0, sumH=0, sumTH=0, maxResidual=0;
    for (int i=0; i<kBins; ++i) if (bins[i].count >= 2) {
        const auto& bin=bins[i]; const double t=bin.sumU/bin.count;
        const double center=g.a+t*(g.b+t*g.c), h=(bin.high-bin.low+1)*.5;
        const double residual=::fabs((bin.low+bin.high)*.5-center);
        if (residual>maxResidual) maxResidual=residual;
        sumT+=t; sumTT+=t*t; sumH+=h; sumTH+=t*h;
    }
    if (maxResidual > ::fmax(1.5, meanHeight * .15)) return g;
    const double denominator=occupied*sumTT-sumT*sumT;
    if (denominator <= 1e-8) return g;
    g.h1=(occupied*sumTH-sumT*sumH)/denominator;
    // Same unclip ratio as the rectangle, using local rather than curved AABB height.
    const double padding=length*meanHeight*1.4/(2*(length+meanHeight));
    g.h0=(sumH-g.h1*sumT)/occupied + padding + maxResidual;
    if (g.h0 < 1 || g.h0+g.h1 < 1 || ::fabs(g.h1) > .5*meanHeight) return g;
    g.ox=u0*cosAxis*scaleX; g.oy=u0*sinAxis*scaleY;
    g.ux=length*cosAxis*scaleX; g.uy=length*sinAxis*scaleY;
    g.vx=-sinAxis*scaleX; g.vy=cosAxis*scaleY;
    if (endpointPadding) {
        // Restore the local unclip margin along the line as well as across it.
        // Rebase the polynomial onto [0,1], preserving Inside's exact domain,
        // positive-height and image-extrema checks for the entire padded crop.
        const double first=-padding/length, span=1-2*first;
        const double oldB=g.b, oldC=g.c, oldH1=g.h1;
        g.ox+=g.ux*first; g.oy+=g.uy*first;
        g.ux*=span; g.uy*=span;
        g.a+=first*(oldB+first*oldC);
        g.b=span*(oldB+2*first*oldC); g.c=oldC*span*span;
        g.h0+=oldH1*first; g.h1=oldH1*span;
    }
    // Stay inside the actual source. Clamping a ribbon would destroy its map.
    if(!g.Inside(sw,sh)) return g;
    g.valid=true; g.unsupported=false; return g;
}
} // namespace okuflow::uct::ribbon
#undef OKUFLOW_RIBBON_HD
