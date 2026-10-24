#pragma once

#include "okuflow/common/ultra_clear_text.hpp"
#include <algorithm>
#include <cmath>
#include <span>

namespace okuflow::uct::current_ink {
// Offline/shadow diagnostic only. These verdicts are not text admission,
// display eligibility, tracking authority, or permission to defer full OCR.
enum class Verdict { Changed, Unchanged, Unverifiable };
enum class Reason { Compared, InvalidInput, Resolution, Bounds, Budget, Contrast, Blur, Ambiguous };
inline constexpr int kMaximumWidth=512, kMaximumHeight=96, kMaximumSamples=32768, kHalo=3;
struct Patch {
    std::vector<float> luma; // privately owned samples, never a borrowed frame
    int width{}, height{};
    bool Valid() const { return width>2*kHalo && height>2*kHalo &&
        width<=kMaximumWidth && height<=kMaximumHeight && luma.size()==std::size_t(width)*height &&
        luma.size()<=kMaximumSamples; }
};
struct Reference {
    Patch patch;
    SourceStamp stamp;
    TextQuad sourceQuad;
};
struct Result {
    Verdict verdict{Verdict::Unverifiable};
    Reason reason{Reason::InvalidInput};
    double worstResidual{}, alignmentResidual{}, contrastRatio{}, edgeRatio{};
    double blurredResidual{-1}; // matched-blur worst residual, -1 when not scored
    QPointF jitterPx;
};

inline bool CompatiblePixels(const QImage& source,const SourceStamp& stamp) {
    return !source.isNull() && source.width()==stamp.viewportWidth && source.height()==stamp.viewportHeight &&
        !stamp.temporallySmoothed && stamp.holdDisplay!=HoldDisplay::Crossfade &&
        stamp.holdDisplay!=HoldDisplay::Unknown &&
        (source.format()==QImage::Format_ARGB32||source.format()==QImage::Format_RGB32);
}
// Sample a bounded word plus halo from the exact analysis image. No resize,
// thumbnail, decimated probe, camera-native claim, or whole-frame copy.
inline Patch Capture(const QImage& source,const SourceStamp& stamp,const TextQuad& quad) {
    Patch patch;
    if(!CompatiblePixels(source,stamp)||!quad.valid||QuadArea(quad)<=0) return patch;
    const double w=std::max(std::hypot((quad.points[1]-quad.points[0]).x(),(quad.points[1]-quad.points[0]).y()),
                            std::hypot((quad.points[2]-quad.points[3]).x(),(quad.points[2]-quad.points[3]).y()));
    const double h=std::max(std::hypot((quad.points[3]-quad.points[0]).x(),(quad.points[3]-quad.points[0]).y()),
                            std::hypot((quad.points[2]-quad.points[1]).x(),(quad.points[2]-quad.points[1]).y()));
    if(!std::isfinite(w)||!std::isfinite(h)||w<3||h<3||w>kMaximumWidth-2*kHalo||h>kMaximumHeight-2*kHalo) return patch;
    patch.width=int(std::ceil(w))+2*kHalo; patch.height=int(std::ceil(h))+2*kHalo;
    if(patch.width*patch.height>kMaximumSamples) return {};
    patch.luma.resize(std::size_t(patch.width)*patch.height);
    const auto at=[&](double u,double v) {
        return quad.points[0]*((1-u)*(1-v))+quad.points[1]*(u*(1-v))+
               quad.points[2]*(u*v)+quad.points[3]*((1-u)*v);
    };
    for(int y=0;y<patch.height;++y)for(int x=0;x<patch.width;++x) {
        const auto p=at((x-kHalo+.5)/(patch.width-2*kHalo),(y-kHalo+.5)/(patch.height-2*kHalo))-QPointF(.5,.5);
        if(!std::isfinite(p.x())||!std::isfinite(p.y())||p.x()<0||p.y()<0||
            p.x()>=source.width()-1||p.y()>=source.height()-1) return {};
        const int ix=int(p.x()),iy=int(p.y()); const double dx=p.x()-ix,dy=p.y()-iy;
        const auto* a=reinterpret_cast<const QRgb*>(source.constScanLine(iy));
        const auto* b=reinterpret_cast<const QRgb*>(source.constScanLine(iy+1));
        patch.luma[std::size_t(y)*patch.width+x]=float((qGray(a[ix])*(1-dx)+qGray(a[ix+1])*dx)*(1-dy)+
            (qGray(b[ix])*(1-dx)+qGray(b[ix+1])*dx)*dy);
    }
    return patch;
}
inline Reference Build(const QImage& image,const SourceStamp& stamp,const TextQuad& word) {
    return {Capture(image,stamp,word),stamp,word};
}

// Resident-ready bounded sample oracle: consumes owned grids, not a device
// pointer or a decimated probe. A future GPU lease/kernel must match this seam.
inline Result Compare(const Patch& reference,const Patch& current) {
    Result result;
    if(!reference.Valid()||!current.Valid()||reference.width!=current.width||reference.height!=current.height) return result;
    const int w=reference.width,h=reference.height;
    auto a=reference.luma,b=current.luma; // at most 2 * 32768 floats
    const auto normalize=[](std::vector<float>& values) {
        std::array<int,256> histogram{};
        for(float value:values) {
            if(!std::isfinite(value)||value<0||value>255) return 0.;
            ++histogram[int(value)];
        }
        const auto quantile=[&](int wanted) { int count=0;for(int i=0;i<256;++i){count+=histogram[i];if(count>=wanted)return i;}return 255; };
        const int low=quantile(std::max(1,int(values.size()/50))),high=quantile(std::max(1,int(values.size()*49/50)));
        const double contrast=high-low;
        if(contrast<32) return 0.;
        for(auto& value:values) value=float(std::clamp((high-value)/contrast,0.,1.));
        return contrast;
    };
    const double contrastA=normalize(a),contrastB=normalize(b);
    if(contrastA<=0||contrastB<=0) {result.reason=Reason::Contrast;return result;}
    result.contrastRatio=contrastB/contrastA;
    if(result.contrastRatio<.6||result.contrastRatio>1.6) {result.reason=Reason::Contrast;return result;}
    const auto energy=[&](const auto& values) {
        double total=0; for(int y=kHalo;y<h-kHalo-1;++y)for(int x=kHalo;x<w-kHalo-1;++x) {
            const auto i=y*w+x; total+=std::abs(values[i+1]-values[i])+std::abs(values[i+w]-values[i]);
        }return total;
    };
    const double energyA=energy(a),energyB=energy(b);
    if(energyA<2||energyB<2) {result.reason=Reason::Contrast;return result;}
    result.edgeRatio=energyB/energyA;
    if(result.edgeRatio<.6||result.edgeRatio>1.65) {result.reason=Reason::Blur;return result;}
    const auto sample=[&](const auto& values,double x,double y) {
        const int ix=int(x),iy=int(y);const double dx=x-ix,dy=y-iy;
        return (values[iy*w+ix]*(1-dx)+values[iy*w+ix+1]*dx)*(1-dy)+
            (values[(iy+1)*w+ix]*(1-dx)+values[(iy+1)*w+ix+1]*dx)*dy;
    };
    // One translation for the complete word, never independent local dilation:
    // a one-pixel stroke edit must not be erased by choosing a different shift
    // at every pixel. Symmetric half-pixel search, radius two native grid pixels.
    double best=1e30; int bestX=0,bestY=0;
    for(int sy=-4;sy<=4;++sy)for(int sx=-4;sx<=4;++sx) {
        double sum=0; for(int y=kHalo;y<h-kHalo;++y)for(int x=kHalo;x<w-kHalo;++x) {
            sum+=std::abs(a[y*w+x]-sample(b,x+sx*.5,y+sy*.5));
            sum+=std::abs(b[y*w+x]-sample(a,x-sx*.5,y-sy*.5));
        }
        const int distance=sx*sx+sy*sy,oldDistance=bestX*bestX+bestY*bestY;
        if(sum<best-1e-9||(std::abs(sum-best)<=1e-9&&distance<oldDistance)) {best=sum;bestX=sx;bestY=sy;}
    }
    result.jitterPx={bestX*.5,bestY*.5};
    result.alignmentResidual=best/(2*(w-2*kHalo)*(h-2*kHalo));
    const int side=std::clamp((h-2*kHalo)/3,4,12);
    // Exhaustive small windows through a summed-area table. Fixed image/sample
    // bounds cap both memory and visits; score the worst region of either direction.
    const auto worstOf=[&](const std::vector<float>& first,const std::vector<float>& second) {
        double worst=0;
        std::vector<double> integral(std::size_t(w+1)*(h+1));
        for(int direction:{-1,1}) {
            std::fill(integral.begin(),integral.end(),0.);
            const auto& ref=direction==1?first:second;const auto& cur=direction==1?second:first;
            for(int y=kHalo;y<h-kHalo;++y)for(int x=kHalo;x<w-kHalo;++x) {
                const double residual=std::abs(ref[y*w+x]-sample(cur,x+direction*bestX*.5,y+direction*bestY*.5));
                const auto i=(y+1)*(w+1)+x+1;
                integral[i]=residual+integral[i-1]+integral[i-w-1]-integral[i-w-2];
            }
            for(int y=kHalo;y<h-kHalo;++y)for(int x=kHalo;x<w-kHalo;++x) {
                const int x1=std::min(w-kHalo,x+side),y1=std::min(h-kHalo,y+side);
                if(x1-x<side||y1-y<side) continue;
                const double sum=integral[y1*(w+1)+x1]-integral[y*(w+1)+x1]-integral[y1*(w+1)+x]+integral[y*(w+1)+x];
                worst=std::max(worst,sum/((x1-x)*(y1-y)));
            }
        }
        return worst;
    };
    result.worstResidual=worstOf(a,b);
    // Deliberately provisional offline thresholds, not calibrated safety policy.
    if(result.worstResidual>=.10) {
        // Identical [1,2,1]/4 smoothing of both grids removes defocus and
        // resampling mismatch. It can only demote Changed to Unverifiable:
        // Unchanged still requires the sharp residual above, so no edit can
        // gain acceptance from this second look.
        const auto blurred=[&](const std::vector<float>& values) {
            std::vector<float> pass(values.size()),out(values.size());
            for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
                const int xm=std::max(0,x-1),xp=std::min(w-1,x+1);
                pass[std::size_t(y)*w+x]=(values[std::size_t(y)*w+xm]+2*values[std::size_t(y)*w+x]+values[std::size_t(y)*w+xp])*.25f;
            }
            for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
                const int ym=std::max(0,y-1),yp=std::min(h-1,y+1);
                out[std::size_t(y)*w+x]=(pass[std::size_t(ym)*w+x]+2*pass[std::size_t(y)*w+x]+pass[std::size_t(yp)*w+x])*.25f;
            }
            return out;
        };
        result.blurredResidual=worstOf(blurred(a),blurred(b));
        if(result.blurredResidual<=.035) {result.reason=Reason::Blur;return result;}
        result.verdict=Verdict::Changed;
    }
    else if(result.worstResidual<=.035) result.verdict=Verdict::Unchanged;
    else {result.reason=Reason::Ambiguous;return result;}
    result.reason=Reason::Compared;return result;
}
inline Result Check(const Reference& reference,const QImage& current,const SourceStamp& stamp) {
    const auto& a=reference.stamp.viewTransform;const auto& b=stamp.viewTransform;
    if(!reference.stamp.SameScene(stamp)||reference.stamp.viewportWidth!=stamp.viewportWidth||
       reference.stamp.viewportHeight!=stamp.viewportHeight||a.valid!=b.valid||
       a.sourceX!=b.sourceX||a.sourceY!=b.sourceY||a.sourceWidth!=b.sourceWidth||a.sourceHeight!=b.sourceHeight||
       a.destinationX!=b.destinationX||a.destinationY!=b.destinationY||
       a.destinationWidth!=b.destinationWidth||a.destinationHeight!=b.destinationHeight) return {};
    return Compare(reference.patch,Capture(current,stamp,reference.sourceQuad));
}
} // namespace okuflow::uct::current_ink
