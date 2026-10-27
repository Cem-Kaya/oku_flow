#pragma once
#include "okuflow/common/uct_paddle.hpp"

namespace okuflow::uct {
struct PreparedModels {
    std::unique_ptr<Recognizer> recognizer;
    BackendStatus backend;
    QString error;
};
// Worker-only: prepare the selected GPU pair when available and the selected
// CPU fallback. A completed real blank-image inference checks compatibility,
// never recognition accuracy. Ready objects transfer to the OCR worker once.
PreparedModels PrepareInstalledModels(const ModelSettings& settings,const std::atomic<bool>& cancelled,
                                     const QString& preparationCache = {});
}
