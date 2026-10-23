#pragma once
#include "uct_public_bf16.hpp"

namespace okuflow::uct::bf16_runtime {
// Precision selection is not an OCR accuracy test. Keys bind the selected
// graph, GPU and builder/profile recipe, never expected words or driver version.
inline QByteArray PlanKey(const QByteArray& graph,const QByteArray& uuid,bool detection,int capacity,bool variable,
                          const QByteArray& builder=tensorrt_identity::kBuilderVersion) {
    const QByteArray role=detection?"uct-bf16-detector-v1\n":"uct-bf16-recognizer-v1\n";
    return precision_artifacts::Digest(role+tensorrt_identity::Fp32PlanKey(graph,uuid,detection,capacity,variable,builder));
}
inline void CheckRecognizerProfile(const precision_artifacts::Profile& profile,int capacity) {
    const precision_artifacts::Profile expected{{1,3,48,1024},{capacity,3,48,1024},{capacity,3,48,1024}};
    if(profile.minimum!=expected.minimum || profile.optimum!=expected.optimum || profile.maximum!=expected.maximum)
        throw runtime_precision::ArtifactError("BF16 recognizer input profile differs from selected capacity");
}
} // namespace okuflow::uct::bf16_runtime
