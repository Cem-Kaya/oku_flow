#pragma once
#include "okuflow/common/uct_formula.hpp"
#include "okuflow/common/ultra_clear_text.hpp"
#include <vector>

namespace okuflow::uct::formula {
struct ImageInput {std::vector<float> tensor;QRect bounds;LineColors colors;QString error;};
ImageInput PrepareImageInput(const QImage& image,const QRect& selection,const std::atomic<bool>& cancel);
}
