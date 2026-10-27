#pragma once

// First-party native precision recipe. No Python, ONNX rewriting or external
// quantizer is run by the application. FLOAT interfaces and non-linear/shape
// operations remain unchanged; only convolution/matrix products use BF16.
#include <QByteArray>
#include <QCryptographicHash>
#include <cstdint>
#include <stdexcept>

namespace okuflow::uct::native_precision {
inline constexpr char kRecipe[] = "okuflow-native-bf16-conv-matmul-v1";
inline QByteArray GraphIdentity(const QByteArray& sourceDigest) {
    return QCryptographicHash::hash(QByteArray(kRecipe) + '\n' + sourceDigest,
                                    QCryptographicHash::Sha256);
}
inline std::uint16_t Bfloat16(std::uint32_t bits) {
    if ((bits & 0x7f800000u) == 0x7f800000u)
        throw std::runtime_error("Non-finite native BF16 weight.");
    const auto rounded = std::uint16_t((bits + 0x7fffu + ((bits >> 16) & 1u)) >> 16);
    if ((rounded & 0x7f80u) == 0x7f80u)
        throw std::runtime_error("Native BF16 weight overflow.");
    return rounded;
}
} // namespace okuflow::uct::native_precision

#if OKUFLOW_UCT_TRT
#include <NvInfer.h>
#include "uct_tensorrt_failures.hpp"
#include <cstring>
#include <deque>
#include <functional>
#include <vector>

namespace okuflow::uct::native_precision {
struct Weights {
    // Parser/build may retain these pointers. Keep this whole object in the
    // exact preparation closure through confirmed completion or quarantine.
    std::deque<std::vector<std::uint16_t>> buffers;
    nvinfer1::Weights Convert(nvinfer1::Weights original) {
        if (original.type != nvinfer1::DataType::kFLOAT || original.count < 0 ||
            original.count > 64ll * 1024 * 1024 || (original.count && !original.values))
            throw std::runtime_error("Unsupported native BF16 convolution weight.");
        buffers.emplace_back(std::size_t(original.count));
        auto& converted = buffers.back();
        const auto* bytes = static_cast<const unsigned char*>(original.values);
        for (std::int64_t i = 0; i < original.count; ++i) {
            std::uint32_t bits{};
            std::memcpy(&bits, bytes + i * 4, 4);
            converted[std::size_t(i)] = Bfloat16(bits);
        }
        return {nvinfer1::DataType::kBF16, converted.data(), original.count};
    }
};

inline int Apply(nvinfer1::INetworkDefinition& network, Weights& weights,
                 const std::function<bool()>& stop) {
    using namespace nvinfer1;
    if (network.getNbInputs() != 1 || network.getNbOutputs() != 1 ||
        network.getInput(0)->getType() != DataType::kFLOAT ||
        network.getOutput(0)->getType() != DataType::kFLOAT)
        throw std::runtime_error("Native BF16 requires original FLOAT interfaces.");
    std::vector<ILayer*> originals;
    const auto count = network.getNbLayers();
    if (count < 1 || count > 16384) throw std::runtime_error("Native BF16 network bound.");
    for (int i = 0; i < count; ++i) originals.push_back(network.getLayer(i));
    const auto cast = [&](ITensor& tensor, DataType type) -> ITensor& {
        auto* layer = network.addCast(tensor, type);
        if (!layer) throw detail::TensorRtApiFailure("Native BF16 cast creation failed.");
        return *layer->getOutput(0);
    };
    int converted = 0;
    for (auto* layer : originals) {
        if (stop()) throw std::runtime_error("Native BF16 preparation cancelled.");
        if (layer->getType() != LayerType::kCONVOLUTION &&
            layer->getType() != LayerType::kMATRIX_MULTIPLY) continue;
        if (layer->getNbOutputs() != 1 || layer->getOutput(0)->getType() != DataType::kFLOAT)
            throw std::runtime_error("Unsupported native BF16 linear output.");
        for (int input = 0; input < layer->getNbInputs(); ++input) {
            auto* tensor = layer->getInput(input);
            if (tensor && tensor->getType() != DataType::kFLOAT)
                throw std::runtime_error("Unsupported native BF16 linear operand.");
        }
        auto* originalOutput = layer->getOutput(0);
        const bool graphOutput = originalOutput->isNetworkOutput();
        const QByteArray outputName(originalOutput->getName());
        if (graphOutput) network.unmarkOutput(*originalOutput);
        // Convert static convolution weights and bias together. An empty bias
        // still needs BF16 type; FLOAT bias with BF16 input is invalid in TRT11.
        if (layer->getType() == LayerType::kCONVOLUTION) {
            auto& convolution = static_cast<IConvolutionLayer&>(*layer);
            convolution.setKernelWeights(weights.Convert(convolution.getKernelWeights()));
            convolution.setBiasWeights(weights.Convert(convolution.getBiasWeights()));
        }
        const int inputs = layer->getNbInputs();
        for (int input = 0; input < inputs; ++input)
            if (auto* tensor = layer->getInput(input)) layer->setInput(input, cast(*tensor, DataType::kBF16));
        auto* restore = network.addCast(*originalOutput, DataType::kFLOAT);
        if (!restore) throw detail::TensorRtApiFailure("Native BF16 FLOAT restoration failed.");
        // Redirect current consumers, including casts added for an earlier
        // converted neighbour. Never redirect the restoration into itself.
        for (int i = 0; i < network.getNbLayers(); ++i) {
            auto* consumer = network.getLayer(i);
            if (consumer == restore) continue;
            for (int input = 0; input < consumer->getNbInputs(); ++input)
                if (consumer->getInput(input) == originalOutput)
                    consumer->setInput(input, *restore->getOutput(0));
        }
        if (graphOutput) {
            originalOutput->setName((outputName + "__native_bf16").constData());
            restore->getOutput(0)->setName(outputName.constData());
            network.markOutput(*restore->getOutput(0));
        }
        ++converted;
    }
    if (!converted) throw std::runtime_error("No eligible native BF16 linear operators.");
    return converted;
}
} // namespace okuflow::uct::native_precision
#endif
