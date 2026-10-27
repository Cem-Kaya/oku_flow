#pragma once
#include "okuflow/common/uct_model_installation.hpp"

namespace okuflow::uct::installation::detail {
// Worker storage algorithm separated from transport for deterministic tests.
// Production Run validates every field against Catalog before entering here.
using Fetch = std::function<void(const File&,const QString&,const std::atomic<bool>&,
                                 qint64,qint64,const Notify&)>;
Result RunPinned(const QVector<Model>& models,const QString& bundledRoot,const QString& cacheRoot,
                 const std::atomic<bool>& cancelled,const Notify& notify,const Prepare& prepare,const Fetch& fetch);
}
