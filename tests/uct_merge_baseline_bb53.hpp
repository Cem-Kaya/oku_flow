#pragma once
// Frozen bb53ac884c24d15f0c77b2dc43cace0e8e650c24 oracle. The two sibling
// .cpp files preserve the separate merge/geometry translation units and exact
// original function bodies apart from Frozen name prefixes. Do not refresh
// from the candidate; see docs/uct_merge_optimization.md.
#include "okuflow/common/uct_paddle.hpp"
namespace okuflow::uct::merge_replay {
double FrozenQuadArea(const TextQuad& quad);
double FrozenQuadIntersectionArea(const TextQuad& a, const TextQuad& b);
void FrozenPaddleMergeRecognition(QVector<RecognizedLine>& output, RecognizedLine line,
                                 QRect region, QSize source);
} // namespace okuflow::uct::merge_replay
