#pragma once
#include "okuflow/common/uct_model_settings.hpp"
#include <array>
#include <string_view>

namespace okuflow::uct {
// Reviewed immutable model identities. The processing fixture reconciles every
// entry with scripts/uct/pins.json, including hashes, repository and revision.
struct PaddleModelPin {
    bool detection; ModelTier tier;
    std::string_view directory, modelSha256, configSha256, repository, revision;
};
inline constexpr std::array<PaddleModelPin,6> kPaddleModelPins{{
    {true,ModelTier::Tiny,"det","193bab7a04fca699a6c82e6abb5b81bdb28177f0abd4062552b04908dafb19f8","3ac018be6f97499a08faa3bbdeb33640968d9307f6736d152902747a9f259593","PaddlePaddle/PP-OCRv6_tiny_det_onnx","2ba1506c0380b8f0b03dd142459aac66d4421f6c"},
    {false,ModelTier::Small,"rec","5435fd747c9e0efe15a96d0b378d5bd157e9492ed8fd80edf08f30d02fa24634","ab078671bb49f06228eadccd34f1bb501e157f7a047095ffb943ba81512c77d1","PaddlePaddle/PP-OCRv6_small_rec_onnx","b8f84f0b80c529de40b4fbb3544b84fa7233a513"},
    {true,ModelTier::Medium,"det/medium","eb13b44b25bb36f89528b68720af8a61d9cf381176107f465db1757b65d086e1","7298d5ead546584af2504d03355f881ac7a7bc0eb1e282d3e159277c1d0af871","PaddlePaddle/PP-OCRv6_medium_det_onnx","61323801669c338b7891481ec7bac61ce31b576a"},
    {false,ModelTier::Medium,"rec/medium","9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba","991b700facf5b50a7de193468207d5f4255b538dde0d312ae3b7c7a9b6873129","PaddlePaddle/PP-OCRv6_medium_rec_onnx","50c7eacafc52fa7bcf4194e8cd08e46f8558504b"},
    {true,ModelTier::Small,"det/small","d73e0058b7a8086bbd57f3d10b8bcd4ff95363f67e06e2762b5e814fe9c9410e","193f435274bf9f0b5f71a929bbfbcf148282df7e633b34e7c373e8f44741b516","PaddlePaddle/PP-OCRv6_small_det_onnx","28fe5895c24fd108c19eb3e8479f4ab385fbfc62"},
    {false,ModelTier::Tiny,"rec/tiny","9ef676d6ed3c88256a2d92c640c44f25b0c40947e111b14b8be8f594091563e6","66170210bad538e83fff3c4a3867e547d6bf20b50d64b20347c4b913f3034ea1","PaddlePaddle/PP-OCRv6_tiny_rec_onnx","2612ab37152ae0a677521bae4e1e3d4fb4cf7c30"},
}};
inline const PaddleModelPin* FindPaddleModelPin(bool detection,ModelTier tier) {
    for(const auto& pin:kPaddleModelPins)if(pin.detection==detection && pin.tier==tier)return &pin;
    return nullptr;
}
} // namespace okuflow::uct
