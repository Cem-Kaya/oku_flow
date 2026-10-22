#pragma once
#include "okuflow/common/view_transform.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <span>
#include <vector>

namespace okuflow {
// Display-only recipe for already processed scene pixels. No appearance or
// temporal effect is run again, and this recipe grants no OCR acquisition.
struct FrozenSceneRecipe {
    int rotationQuarterTurns{};
    bool linearSampling{}, focusMarker{};
    float focusX{0.5f}, focusY{0.5f};
};
inline bool ComposeFrozenCameraScene(std::span<const std::uint8_t> source,
    std::uint32_t width,std::uint32_t height,const ViewTransform& view,
    std::uint32_t viewportWidth,std::uint32_t viewportHeight,
    const FrozenSceneRecipe& recipe,std::vector<std::uint8_t>& output) {
    constexpr std::size_t sceneLimit=48u*1024*1024,viewportLimit=128u*1024*1024;
    const auto rect=[](float p,float size){return std::isfinite(p) && std::isfinite(size) &&
        p>=0 && size>0 && p+size<=1.00001f;};
    if(!width || !height || width>sceneLimit/4/height || source.size()!=std::size_t(width)*height*4 ||
        !viewportWidth || !viewportHeight || viewportWidth>viewportLimit/4/viewportHeight || !view.valid ||
        !rect(view.sourceX,view.sourceWidth) || !rect(view.sourceY,view.sourceHeight) ||
        !rect(view.destinationX,view.destinationWidth) || !rect(view.destinationY,view.destinationHeight) ||
        !std::isfinite(recipe.focusX) || !std::isfinite(recipe.focusY))return false;
    const int turns=((recipe.rotationQuarterTurns%4)+4)%4;
    const auto rw=(turns&1)?height:width,rh=(turns&1)?width:height;
    const auto mapping=ComputePixelViewMapping(view,rw,rh,viewportWidth,viewportHeight);
    if(!mapping.valid)return false;
    try{output.assign(std::size_t(viewportWidth)*viewportHeight*4,0);}catch(const std::bad_alloc&){return false;}
    for(std::uint32_t y=0;y<mapping.activeHeight;++y)for(std::uint32_t x=0;x<mapping.activeWidth;++x) {
        auto* pixel=output.data()+((std::size_t(y+mapping.offsetY)*viewportWidth)+x+mapping.offsetX)*4;
        if(!recipe.linearSampling) {
            const auto rx=std::clamp(int(std::lround(mapping.startX+x*mapping.stepX)),0,int(rw)-1);
            const auto ry=std::clamp(int(std::lround(mapping.startY+y*mapping.stepY)),0,int(rh)-1);
            const auto sx=turns==1?ry:turns==2?int(width)-1-rx:turns==3?int(width)-1-ry:rx;
            const auto sy=turns==1?int(height)-1-rx:turns==2?int(height)-1-ry:turns==3?rx:ry;
            std::copy_n(source.data()+(std::size_t(sy)*width+sx)*4,4,pixel);
        } else {
            float u=view.sourceX+(x+0.5f)/mapping.activeWidth*view.sourceWidth;
            float v=view.sourceY+(y+0.5f)/mapping.activeHeight*view.sourceHeight;
            const float a=u,b=v;
            if(turns==1){u=b;v=1-a;}else if(turns==2){u=1-a;v=1-b;}else if(turns==3){u=1-b;v=a;}
            const float sx=std::clamp(u*width-0.5f,0.0f,float(width-1));
            const float sy=std::clamp(v*height-0.5f,0.0f,float(height-1));
            const auto x0=std::uint32_t(sx),y0=std::uint32_t(sy),x1=std::min(width-1,x0+1),y1=std::min(height-1,y0+1);
            for(int c=0;c<3;++c) {
                const float p=source[(std::size_t(y0)*width+x0)*4+c],q=source[(std::size_t(y0)*width+x1)*4+c];
                const float r=source[(std::size_t(y1)*width+x0)*4+c],s=source[(std::size_t(y1)*width+x1)*4+c];
                pixel[c]=std::uint8_t(std::lround((p+(q-p)*(sx-x0))*(1-(sy-y0))+(r+(s-r)*(sx-x0))*(sy-y0)));
            }
            pixel[3]=255;
        }
    }
    if(recipe.focusMarker) {
        float fx=recipe.focusX,fy=recipe.focusY;
        if(turns==1){fx=1-recipe.focusY;fy=recipe.focusX;}else if(turns==2){fx=1-fx;fy=1-fy;}
        else if(turns==3){fx=recipe.focusY;fy=1-recipe.focusX;}
        const float cx=(view.destinationX+(fx-view.sourceX)/view.sourceWidth*view.destinationWidth)*viewportWidth;
        const float cy=(view.destinationY+(fy-view.sourceY)/view.sourceHeight*view.destinationHeight)*viewportHeight;
        for(int y=std::max(0,int(std::floor(cy-18)));y<=std::min(int(viewportHeight)-1,int(std::ceil(cy+18)));++y)
            for(int x=std::max(0,int(std::floor(cx-18)));x<=std::min(int(viewportWidth)-1,int(std::ceil(cx+18)));++x) {
                if(recipe.linearSampling && (x<int(mapping.offsetX) || y<int(mapping.offsetY) ||
                    x>=int(mapping.offsetX+mapping.activeWidth) || y>=int(mapping.offsetY+mapping.activeHeight)))continue;
                const float dx=x+0.5f-cx,dy=y+0.5f-cy,distance=dx*dx+dy*dy;
                if(distance>324)continue;
                auto* p=output.data()+(std::size_t(y)*viewportWidth+x)*4;
                if(distance<=36)p[0]=p[1]=p[2]=255;
                else {p[0]=p[1]=recipe.linearSampling?20:0;p[2]=recipe.linearSampling?242:255;}
                p[3]=255;
            }
    }
    return true;
}
} // namespace okuflow
