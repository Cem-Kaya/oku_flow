#pragma once
#include <QDir>
#include <QFileInfo>

namespace okuflow::uct::runtime_paths {
inline QString PaddleRoot(const QString& configured,const QString& application,const QString& localData) {
    if(!configured.isEmpty())return QDir(configured).absolutePath();
    const auto bundled=QDir(application).filePath("ocr");
    // An explicit bundle marker selects even a partial/damaged inventory so
    // LoadPaddleAssets reports its exact missing pin instead of borrowing a
    // developer's LocalAppData models. Preserve legacy flat bundles as well.
    if(QFileInfo::exists(QDir(bundled).filePath("pins.json")) ||
       QFileInfo::exists(QDir(bundled).filePath("det/inference.onnx")))return bundled;
    return QDir(localData).filePath("OkuFlow/ocr-runtime");
}
inline QString TensorRtRoot(const QString& configured,const QString& application,const QString& localData) {
    if(!configured.isEmpty())return QDir(configured).absolutePath();
    const auto bundled=QDir(application).filePath("ocr/tensorrt");
    if(QFileInfo::exists(QDir(bundled).filePath("bin/nvinfer_11.dll")))return bundled;
    return QDir(localData).filePath("OkuFlow/ocr-deps/TensorRT-11.4.0.106-cuda12.9/TensorRT-11.4.0.106");
}
inline QString CudaBin(const QString& configured,const QString& sdk,const QString& toolkit,const QString& localData) {
    if(!configured.isEmpty())return QDir(configured).absolutePath();
    const auto bundled=QDir(sdk).filePath("cuda/bin");
    if(QFileInfo(bundled).isDir())return bundled;
    if(!toolkit.isEmpty())return QDir(toolkit).filePath("bin");
    return QDir(localData).filePath("OkuFlow/toolchains/cuda-12.9.2/toolkit/bin");
}
} // namespace okuflow::uct::runtime_paths
