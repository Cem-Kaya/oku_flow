#include "uct_formula_image.hpp"
#include "uct_formula_input_policy.hpp"
#include <algorithm>
#include <cmath>

namespace okuflow::uct::formula {
ImageInput PrepareImageInput(const QImage& input,const QRect& selection,const std::atomic<bool>& cancel) {
    ImageInput out;
    const auto roi=selection.intersected(input.rect());
    if(input.isNull() || input.width()>8192 || input.height()>8192 || roi.width()<5 || roi.height()<5 || cancel.load()) {
        out.error="A complete selected formula image is required.";return out;
    }
    const auto image=input.convertToFormat(QImage::Format_ARGB32);
    const int step=formula_input::SampleStep(roi.width(),roi.height());
    unsigned luma[256]{},rgb[2][3][256]{},counts[2]{};
    for(int y=roi.top();y<=roi.bottom();y+=step) {
        if(cancel.load()){out.error="cancelled";return out;}
        for(int x=roi.left();x<=roi.right();x+=step)
            ++luma[formula_input::Luma(image.constScanLine(y)+x*4)];
    }
    const int threshold=formula_input::Threshold(luma);
    for(int y=roi.top();y<=roi.bottom();y+=step)for(int x=roi.left();x<=roi.right();x+=step) {
        const auto* p=image.constScanLine(y)+x*4;const int side=formula_input::Luma(p)>threshold;
        ++counts[side];for(int c=0;c<3;++c)++rgb[side][c][p[2-c]];
    }
    if(!counts[0] || !counts[1]){out.error="The selected view has no separated ink.";return out;}
    const int paper=counts[0]>=counts[1]?0:1,ink=1-paper;
    out.colors.foreground=QColor(color_policy::Median(rgb[ink][0],counts[ink]),color_policy::Median(rgb[ink][1],counts[ink]),color_policy::Median(rgb[ink][2],counts[ink]));
    out.colors.background=QColor(color_policy::Median(rgb[paper][0],counts[paper]),color_policy::Median(rgb[paper][1],counts[paper]),color_policy::Median(rgb[paper][2],counts[paper]));
    const float fr=out.colors.foreground.red(),fg=out.colors.foreground.green(),fb=out.colors.foreground.blue();
    const float br=out.colors.background.red(),bg=out.colors.background.green(),bb=out.colors.background.blue();
    const float dr=fr-br,dg=fg-bg,db=fb-bb,den=dr*dr+dg*dg+db*db;
    if(den<64) {out.error="The selected view has too little ink contrast.";return out;}
    int left=roi.right(),right=roi.left(),top=roi.bottom(),bottom=roi.top(),count=0;
    const auto amount=[&](float r,float g,float b){return ((r-br)*dr+(g-bg)*dg+(b-bb)*db)/den;};
    for(int y=roi.top();y<=roi.bottom();++y) {
        if(cancel.load()){out.error="cancelled";return out;}
        const auto* row=reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for(int x=roi.left();x<=roi.right();++x)if(amount(qRed(row[x]),qGreen(row[x]),qBlue(row[x]))>0.25f) {
            left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);++count;
        }
    }
    if(count<12 || right<=left || bottom<=top || left<=roi.left() || right>=roi.right() || top<=roi.top() || bottom>=roi.bottom()) {
        out.error="Include the complete formula with a small margin.";return out;
    }
    out.bounds=QRect(QPoint(left,top),QPoint(right,bottom)).adjusted(-2,-2,2,2).intersected(roi);
    // UniMERNet's 768-square canvas, black padding, gray normalization. Unlike
    // text recognition, stacked fractions and exponents keep their 2D layout.
    constexpr int side=768;
    const float scale=std::min(float(side)/out.bounds.width(),float(side)/out.bounds.height());
    const int width=std::max(1,int(std::round(out.bounds.width()*scale)));
    const int height=std::max(1,int(std::round(out.bounds.height()*scale)));
    const int px=(side-width)/2,py=(side-height)/2;
    out.tensor.assign(side*side,(0.f-0.7931f)/0.1738f);
    for(int y=0;y<height;++y) {
        if(cancel.load()){out.tensor.clear();out.error="cancelled";return out;}
        for(int x=0;x<width;++x) {
            const float sx=std::clamp(out.bounds.left()+(x+.5f)*out.bounds.width()/width-.5f,float(out.bounds.left()),float(out.bounds.right()));
            const float sy=std::clamp(out.bounds.top()+(y+.5f)*out.bounds.height()/height-.5f,float(out.bounds.top()),float(out.bounds.bottom()));
            const int ix=int(sx),iy=int(sy),jx=std::min(ix+1,out.bounds.right()),jy=std::min(iy+1,out.bounds.bottom());
            const float a=sx-ix,b=sy-iy;
            const QRgb p[4]{image.pixel(ix,iy),image.pixel(jx,iy),image.pixel(ix,jy),image.pixel(jx,jy)};
            const auto channel=[&](auto get){return (get(p[0])*(1-a)+get(p[1])*a)*(1-b)+(get(p[2])*(1-a)+get(p[3])*a)*b;};
            const float coverage=std::clamp(amount(channel([](QRgb p){return qRed(p);}),channel([](QRgb p){return qGreen(p);}),channel([](QRgb p){return qBlue(p);})),0.f,1.f);
            out.tensor[(y+py)*side+x+px]=((1-coverage)-0.7931f)/0.1738f;
        }
    }
    return out;
}
Result Engine::RecognizeImage(const QImage& image,const QRect& roi,const std::atomic<bool>& cancel,std::chrono::steady_clock::time_point deadline) {
    auto prepared=PrepareImageInput(image,roi,cancel);
    if(prepared.tensor.empty()){Result result;result.cancelled=cancel.load();result.code="formula_input";result.detail=prepared.error;return result;}
    auto result=Recognize(prepared.tensor,cancel,deadline);
    result.boundsPx=prepared.bounds;result.foreground=prepared.colors.foreground;result.background=prepared.colors.background;return result;
}
Result Engine::RecognizeDevice(SharedDeviceImage,const QRect&,const std::atomic<bool>&,std::chrono::steady_clock::time_point) {
    Result result;result.code="formula_device_unavailable";return result;
}
}
