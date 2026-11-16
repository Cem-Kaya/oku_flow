#pragma once
// Frozen ec7282d merge oracle; see uct_merge_baseline_ec72.cpp. Do not refresh
// from the candidate.
#include "okuflow/common/uct_paddle.hpp"
namespace okuflow::uct::merge_replay {
void FrozenEc72PaddleMergeRecognition(QVector<RecognizedLine>& output, RecognizedLine line,
                                      QRect region, QSize source);
} // namespace okuflow::uct::merge_replay
