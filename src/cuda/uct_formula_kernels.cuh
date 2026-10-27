#pragma once
#include "../common/uct_formula_input_policy.hpp"
namespace okuflow::uct::gpu {
namespace {
__device__ float formulaCoverage(float r,float g,float b,const Colors& c) {
    const float dr=c.foreground[0]-c.background[0],dg=c.foreground[1]-c.background[1],db=c.foreground[2]-c.background[2];
    const float d=dr*dr+dg*dg+db*db;
    return d>=64?((r-c.background[0])*dr+(g-c.background[1])*dg+(b-c.background[2])*db)/d:0.f;
}
__global__ void formulaColors(const unsigned char* pixels,std::size_t pitch,const Rect* request,FormulaInputInfo* info) {
    const Rect r=*request;const int step=formula_input::SampleStep(r.w,r.h),cols=(r.w+step-1)/step,rows=(r.h+step-1)/step;
    __shared__ unsigned luma[256],rgb[2][3][256],counts[2];__shared__ int threshold;
    luma[threadIdx.x]=0;
    for(int i=threadIdx.x;i<2*3*256;i+=256)reinterpret_cast<unsigned*>(rgb)[i]=0;
    if(threadIdx.x<2)counts[threadIdx.x]=0;__syncthreads();
    for(int i=threadIdx.x;i<cols*rows;i+=256) {
        const auto* p=pixels+(r.y+(i/cols)*step)*pitch+(r.x+(i%cols)*step)*4;
        atomicAdd(&luma[formula_input::Luma(p)],1u);
    }
    __syncthreads();if(threadIdx.x==0)threshold=formula_input::Threshold(luma);__syncthreads();
    for(int i=threadIdx.x;i<cols*rows;i+=256) {
        const auto* p=pixels+(r.y+(i/cols)*step)*pitch+(r.x+(i%cols)*step)*4;
        const int side=formula_input::Luma(p)>threshold;atomicAdd(&counts[side],1u);
        for(int c=0;c<3;++c)atomicAdd(&rgb[side][c][p[2-c]],1u);
    }
    __syncthreads();if(threadIdx.x==0) {
        const int paper=counts[0]>=counts[1]?0:1,ink=1-paper;
        for(int c=0;c<3;++c){info->colors.foreground[c]=color_policy::Median(rgb[ink][c],counts[ink]);info->colors.background[c]=color_policy::Median(rgb[paper][c],counts[paper]);}
        info->valid=counts[0] && counts[1];
    }
}
__global__ void formulaBounds(const unsigned char* pixels,std::size_t pitch,const Rect* request,FormulaInputInfo* info) {
    const Rect r=*request;const Colors c=info->colors;
    int left=r.x+r.w-1,right=r.x,top=r.y+r.h-1,bottom=r.y,count=0;
    for(int i=threadIdx.x;i<r.w*r.h;i+=blockDim.x) {
        const int x=r.x+i%r.w,y=r.y+i/r.w;const auto* p=pixels+y*pitch+x*4;
        if(formulaCoverage(p[2],p[1],p[0],c)>.25f){left=min(left,x);right=max(right,x);top=min(top,y);bottom=max(bottom,y);++count;}
    }
    __shared__ int l[256],t[256],ri[256],b[256],n[256];
    const int id=threadIdx.x;l[id]=left;ri[id]=right;t[id]=top;b[id]=bottom;n[id]=count;__syncthreads();
    for(int step=128;step;step/=2){if(id<step){l[id]=min(l[id],l[id+step]);ri[id]=max(ri[id],ri[id+step]);t[id]=min(t[id],t[id+step]);b[id]=max(b[id],b[id+step]);n[id]+=n[id+step];}__syncthreads();}
    if(id==0) {
        info->valid=info->valid && n[0]>=12 && ri[0]>l[0] && b[0]>t[0] && l[0]>r.x && ri[0]<r.x+r.w-1 && t[0]>r.y && b[0]<r.y+r.h-1;
        const int x=max(r.x,l[0]-2),y=max(r.y,t[0]-2),x1=min(r.x+r.w,ri[0]+3),y1=min(r.y+r.h,b[0]+3);
        info->bounds={x,y,x1-x,y1-y};
    }
}
__global__ void formulaNormalize(const unsigned char* pixels,std::size_t pitch,int sourceWidth,int sourceHeight,const FormulaInputInfo* info,float* out) {
    const int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=768*768)return;
    float value=0;
    if(info->valid) {
        const Rect r=info->bounds;const float scale=fminf(768.f/r.w,768.f/r.h);
        const int width=max(1,int(roundf(r.w*scale))),height=max(1,int(roundf(r.h*scale)));
        const int x=i%768-(768-width)/2,y=i/768-(768-height)/2;
        if(x>=0 && x<width && y>=0 && y<height) {
            const float sx=fminf(fmaxf(r.x+(x+.5f)*r.w/width-.5f,float(r.x)),float(r.x+r.w-1));
            const float sy=fminf(fmaxf(r.y+(y+.5f)*r.h/height-.5f,float(r.y)),float(r.y+r.h-1));
            value=1-fminf(fmaxf(formulaCoverage(sample(pixels,pitch,sourceWidth,sourceHeight,sx,sy,2),
                sample(pixels,pitch,sourceWidth,sourceHeight,sx,sy,1),sample(pixels,pitch,sourceWidth,sourceHeight,sx,sy,0),info->colors),0.f),1.f);
        }
    }
    out[i]=(value-.7931f)/.1738f;
}
}
void FormulaInput(const void* pixels,std::size_t pitch,int w,int h,const Rect* rect,FormulaInputInfo* info,float* output,cudaStream_t stream) {
    formulaColors<<<1,256,0,stream>>>(static_cast<const unsigned char*>(pixels),pitch,rect,info);
    formulaBounds<<<1,256,0,stream>>>(static_cast<const unsigned char*>(pixels),pitch,rect,info);
    formulaNormalize<<<(768*768+255)/256,256,0,stream>>>(static_cast<const unsigned char*>(pixels),pitch,w,h,info,output);
}
}
