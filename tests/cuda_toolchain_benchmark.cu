// Opt-in, one-time comparison. Not registered with CTest or default builds.
#include "okuflow/cuda/cuda_kernels.hpp"
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(cudaError_t status) {
    if(status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}
struct Buffer {
    void* data{};
    explicit Buffer(size_t bytes) { Check(cudaMalloc(&data,bytes)); Check(cudaMemset(data,0,bytes)); }
    ~Buffer() { cudaFree(data); }
    Buffer(const Buffer&)=delete;
    template<class T> T* As() const { return static_cast<T*>(data); }
};
struct Events {
    cudaEvent_t start{}, stop{};
    Events() { Check(cudaEventCreate(&start)); Check(cudaEventCreate(&stop)); }
    ~Events() { cudaEventDestroy(start); cudaEventDestroy(stop); }
};
float Percentile(std::vector<float> values, float fraction) {
    std::sort(values.begin(),values.end());
    return values[static_cast<size_t>((values.size()-1)*fraction)];
}
void Measure(const char* name,int width,int height,const std::function<void()>& launch,
             const void* output,size_t outputBytes) {
    constexpr int warmup=20, samples=31, batch=10;
    for(int i=0;i<warmup;++i) launch();
    Check(cudaDeviceSynchronize());
    Events events;
    std::vector<float> gpu,host;
    for(int sample=0;sample<samples;++sample) {
        Check(cudaEventRecord(events.start));
        const auto before=std::chrono::steady_clock::now();
        for(int i=0;i<batch;++i) launch();
        const auto after=std::chrono::steady_clock::now();
        Check(cudaGetLastError());
        Check(cudaEventRecord(events.stop)); Check(cudaEventSynchronize(events.stop));
        float milliseconds{}; Check(cudaEventElapsedTime(&milliseconds,events.start,events.stop));
        gpu.push_back(milliseconds/batch);
        host.push_back(std::chrono::duration<float,std::milli>(after-before).count()/batch);
    }
    std::vector<unsigned char> pixels(outputBytes);
    Check(cudaMemcpy(pixels.data(),output,outputBytes,cudaMemcpyDeviceToHost));
    std::uint64_t digest=14695981039346656037ull;
    for(auto byte:pixels) { digest^=byte; digest*=1099511628211ull; }
    std::cout << "{\"case\":\"" << name << "\",\"width\":" << width << ",\"height\":" << height
              << ",\"gpuMedianMs\":" << Percentile(gpu,.5f) << ",\"gpuP95Ms\":" << Percentile(gpu,.95f)
              << ",\"hostSubmitMedianMs\":" << Percentile(host,.5f) << ",\"outputFnv64\":\"" << std::hex << digest
              << std::dec << "\",\"warmup\":" << warmup << ",\"samples\":" << samples << ",\"batch\":" << batch << "}\n";
}
void RunSize(int w,int h) {
    using namespace okuflow;
    const size_t count=static_cast<size_t>(w)*h, pitch=w*sizeof(uchar4), fp=w*sizeof(float);
    Buffer src(count*4),dst(count*4),scratch(count*4),history(count*sizeof(float4));
    Buffer luma(count*4),horizontal(count*4),mean(count*4),sqHorizontal(count*4),sqMean(count*4);
    Buffer mask(count),mask2(count),maskHistory(count),analysis(sizeof(int4));
    Buffer yuv(count*3/2),histogram(256*sizeof(unsigned)),levels(sizeof(float2));
    const int sw=w/4,sh=h/4;
    Buffer small(sw*sh*sizeof(float)),pairs(4096*sizeof(float4)),pairCount(sizeof(unsigned));
    Buffer state(sizeof(StabilizationState));
    std::vector<uchar4> pattern(count);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const unsigned v=(x*13+y*29+((x/9)^(y/7))*53)&255;
        pattern[static_cast<size_t>(y)*w+x]=make_uchar4(v,(v+x*7)&255,(v+y*3)&255,255);
    }
    Check(cudaMemcpy(src.data,pattern.data(),count*4,cudaMemcpyHostToDevice));
    Check(cudaMemset(yuv.data,128,count*3/2));
    if(!UploadGaussianKernel(4,1.5f,nullptr)) throw std::runtime_error("Gaussian setup failed");
    std::uint32_t lut[256]; for(unsigned i=0;i<256;++i)lut[i]=0xff000000u|(i<<16)|(i<<8)|i;
    if(!UploadDisplayColorLut(lut,nullptr))throw std::runtime_error("LUT setup failed");
    auto measure=[&](const char* name,const std::function<void()>& launch) {Measure(name,w,h,launch,dst.data,count*4);};
    measure("zoom",[&]{LaunchZoomLinear(dst.As<uchar4>(),pitch,src.As<uchar4>(),pitch,w,h,2.f,.5f,.5f,nullptr);});
    measure("black_white",[&]{LaunchBlackWhiteLinear(dst.As<uchar4>(),pitch,src.As<uchar4>(),pitch,w,h,.5f,nullptr);});
    measure("gaussian_blur",[&]{LaunchGaussianBlurLinear(dst.As<uchar4>(),pitch,scratch.As<uchar4>(),pitch,src.As<uchar4>(),pitch,w,h,nullptr);});
    measure("temporal_smooth",[&]{LaunchTemporalSmoothLinear(dst.As<uchar4>(),pitch,src.As<uchar4>(),pitch,history.As<float4>(),w*sizeof(float4),w,h,.4f,true,nullptr);});
    measure("rotate_180",[&]{LaunchRotateQuarterLinear(dst.As<uchar4>(),pitch,src.As<uchar4>(),pitch,w,h,2,nullptr);});
    const float transform[9]={1,.01f,0,.01f,1,0,.00001f,0,1};
    measure("keystone_warp",[&]{LaunchKeystoneWarp(dst.As<uchar4>(),pitch,src.As<uchar4>(),pitch,w,h,transform,nullptr);});
    measure("nv12_to_bgra",[&]{LaunchNv12ToBgraLinear(dst.As<uchar4>(),pitch,yuv.As<unsigned char>(),w,yuv.As<unsigned char>()+count,w,w,h,nullptr);});
    Measure("display_color_grade",w,h,[&]{LaunchDisplayColorGradeLinear(src.As<uchar4>(),pitch,w,h,0,1.f,0.f,nullptr,0.f,nullptr);},src.data,count*4);
    Measure("auto_contrast",w,h,[&]{LaunchAutoContrastHistogram(histogram.As<unsigned>(),src.As<uchar4>(),pitch,w,h,nullptr);LaunchAutoContrastAnalysis(histogram.As<unsigned>(),static_cast<int>(count),levels.As<float2>(),false,nullptr);},levels.data,sizeof(float2));
    Measure("text_local_statistics",w,h,[&]{LaunchTextLocalStatistics(luma.As<float>(),fp,horizontal.As<float>(),mean.As<float>(),sqHorizontal.As<float>(),sqMean.As<float>(),src.As<uchar4>(),pitch,w,h,8,nullptr);},mean.data,count*4);
    Measure("sauvola_mask",w,h,[&]{LaunchSauvolaMask(mask.As<unsigned char>(),w,luma.As<float>(),mean.As<float>(),sqMean.As<float>(),fp,w,h,.5f,.15f,0,analysis.As<int4>(),nullptr);},mask.data,count);
    Measure("stroke_weight",w,h,[&]{LaunchStrokeWeight(mask2.As<unsigned char>(),w,mask.As<unsigned char>(),w,w,h,1,nullptr);},mask2.data,count);
    Measure("mask_hysteresis",w,h,[&]{LaunchTextMaskHysteresis(mask.As<unsigned char>(),w,maskHistory.As<unsigned char>(),w,h,.5f,true,nullptr);},mask.data,count);
    measure("mask_composite",[&]{LaunchTextMaskComposite(dst.As<uchar4>(),pitch,src.As<uchar4>(),pitch,mask.As<unsigned char>(),w,w,h,0xffffffff,0xff000000,1,analysis.As<int4>(),nullptr);});
    measure("smart_sharpen",[&]{LaunchSmartSharpenLinear(dst.As<uchar4>(),pitch,scratch.As<uchar4>(),pitch,src.As<uchar4>(),pitch,mask.As<unsigned char>(),w,w,h,.5f,true,nullptr);});
    Measure("stabilization_luma",w,h,[&]{LaunchStabilizationLumaDownsample(small.As<float>(),sw,sh,src.As<uchar4>(),pitch,w,h,4,4,nullptr);},small.data,sw*sh*sizeof(float));
    Measure("stabilization_feature_pairs",w,h,[&]{LaunchStabilizationFeaturePairs(small.As<float>(),small.As<float>(),sw,sh,4.f,4.f,pairs.As<float4>(),pairCount.As<unsigned>(),4096,state.As<StabilizationState>(),nullptr);},pairCount.data,sizeof(unsigned));
    measure("stabilization_warp",[&]{LaunchStabilizationWarp(dst.As<uchar4>(),pitch,src.As<uchar4>(),pitch,w,h,state.As<StabilizationState>(),nullptr);});
    const int iw=w/2,ih=h/2; Buffer input(static_cast<size_t>(iw)*ih*4);
    Check(cudaMemcpy2D(input.data,iw*4,src.data,pitch,iw*4,ih,cudaMemcpyDeviceToDevice));
    measure("nis_2x",[&]{LaunchNisLinear(dst.As<uchar4>(),pitch,input.As<uchar4>(),iw*4,iw,ih,w,h,.5f,nullptr);});
    measure("fsr_easu_rcas_2x",[&]{LaunchFsrEasuRcasLinear(dst.As<uchar4>(),pitch,scratch.As<uchar4>(),pitch,input.As<uchar4>(),iw*4,iw,ih,w,h,.5f,nullptr);});
}
} // namespace
int main() {
    try {
        Check(cudaSetDevice(0)); cudaDeviceProp device{}; Check(cudaGetDeviceProperties(&device,0));
        int driver{}; Check(cudaDriverGetVersion(&driver));
        std::cout << std::fixed << std::setprecision(7)
                  << "{\"metadata\":true,\"gpu\":\"" << device.name << "\",\"sm\":" << device.major*10+device.minor
                  << ",\"nvcc\":\"" << __CUDACC_VER_MAJOR__ << '.' << __CUDACC_VER_MINOR__ << '.' << __CUDACC_VER_BUILD__
                  << "\",\"msvc\":" << _MSC_FULL_VER << ",\"runtime\":" << CUDART_VERSION << ",\"driver\":" << driver << "}\n";
        RunSize(1920,1080); RunSize(3840,2160); return 0;
    } catch(const std::exception& e) {std::cerr << e.what() << '\n';return 1;}
}
