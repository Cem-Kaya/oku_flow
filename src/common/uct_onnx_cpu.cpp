#include "okuflow/common/uct_paddle.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <mutex>
#include <stdexcept>

#if OKUFLOW_UCT_ORT
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#endif

namespace okuflow::uct
{
#if OKUFLOW_UCT_ORT
namespace
{
class OnnxCpuSession final : public PaddleCpuSession
{
  public:
    OnnxCpuSession(const QString &runtime, const QString &model)
        : library_(QFileInfo(runtime).absoluteFilePath())
    {
        QFile input(runtime);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!input.open(QIODevice::ReadOnly) || !hash.addData(&input) ||
            hash.result().toHex() !=
                "0c42dc4e278f0968fdf9ebf1b1e54a74c657a9942e70803e453c91993451cf4c")
            throw std::runtime_error("Pinned ONNX Runtime CPU library is missing or changed.");
        library_.setLoadHints(QLibrary::PreventUnloadHint);
        if (!library_.load())
            throw std::runtime_error("ONNX Runtime CPU library is unavailable.");
        using GetApiBase = const OrtApiBase *(ORT_API_CALL *)();
        const auto getApi = reinterpret_cast<GetApiBase>(library_.resolve("OrtGetApiBase"));
        if (!getApi || !getApi()->GetApi(ORT_API_VERSION))
            throw std::runtime_error("ONNX Runtime API version mismatch.");
        static std::once_flag apiInitialized;
        std::call_once(apiInitialized, [&] { Ort::InitApi(getApi()->GetApi(ORT_API_VERSION)); });
        environment_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "OkuFlow UCT CPU");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2);
        options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session_ = std::make_unique<Ort::Session>(
            *environment_, reinterpret_cast<const wchar_t *>(model.utf16()), options);
        if (session_->GetInputCount() != 1 || session_->GetOutputCount() != 1)
            throw std::runtime_error("Unexpected PaddleOCR graph I/O.");
        Ort::AllocatorWithDefaultOptions allocator;
        input_ = session_->GetInputNameAllocated(0, allocator).get();
        output_ = session_->GetOutputNameAllocated(0, allocator).get();
        if (session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementType() !=
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("PaddleOCR requires a float32 input graph.");
    }
    PaddleTensor Run(std::span<const float> data, std::span<const std::int64_t> shape) override
    {
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, const_cast<float *>(data.data()),
                                                      data.size(), shape.data(), shape.size());
        const char *inputs[]{input_.c_str()};
        const char *outputs[]{output_.c_str()};
        auto values = session_->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        auto info = values[0].GetTensorTypeAndShapeInfo();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            info.GetElementCount() > 64ull * 1024 * 1024)
            throw std::runtime_error("Invalid or oversized PaddleOCR output.");
        PaddleTensor result;
        result.shape = info.GetShape();
        const auto *begin = values[0].GetTensorData<float>();
        result.data.assign(begin, begin + info.GetElementCount());
        return result;
    }

  private:
    QLibrary library_;
    std::unique_ptr<Ort::Env> environment_;
    std::unique_ptr<Ort::Session> session_;
    std::string input_, output_;
};
} // namespace
#endif

std::unique_ptr<PaddleCpuSession> OpenPaddleCpuSession(const QString &runtime, const QString &model)
{
#if OKUFLOW_UCT_ORT
    return std::make_unique<OnnxCpuSession>(runtime, model);
#else
    Q_UNUSED(runtime);
    Q_UNUSED(model);
    throw std::runtime_error("This build has no ONNX Runtime CPU adapter.");
#endif
}
} // namespace okuflow::uct
