#pragma once
#include "uct_color_policy.hpp"
#if defined(__CUDACC__)
#define OKUFLOW_FORMULA_HD __host__ __device__
#else
#define OKUFLOW_FORMULA_HD
#endif
namespace okuflow::uct::formula_input {
OKUFLOW_FORMULA_HD inline int Luma(const unsigned char* bgra) {
    return (29*bgra[0]+150*bgra[1]+77*bgra[2])>>8;
}
OKUFLOW_FORMULA_HD inline int SampleStep(int w,int h) {
    int step=1;while(((w+step-1)/step)*((h+step-1)/step)>65536)++step;return step;
}
// A whole selected formula has no detector's tight word boxes. Use a bounded
// two-class histogram to separate sparse source ink from textured paper first.
// This is input/color preparation, never an OCR accuracy/admission threshold.
OKUFLOW_FORMULA_HD inline int Threshold(const unsigned* histogram) {
    double total=0,sum=0;for(int i=0;i<256;++i){total+=histogram[i];sum+=double(i)*histogram[i];}
    double weight=0,partial=0,best=-1;int threshold=0;
    for(int i=0;i<255;++i){weight+=histogram[i];partial+=double(i)*histogram[i];
        if(weight==0 || weight==total)continue;
        const double d=sum*weight-partial*total;const double score=d*d/(weight*(total-weight));
        if(score>best*(1+1e-12)){best=score;threshold=i;}
    }
    return threshold;
}
}
#undef OKUFLOW_FORMULA_HD
