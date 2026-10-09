#pragma once

#include <array>
#include <algorithm>
#include <cstdlib>
#include <limits>

namespace okuflow::app_constants {

inline constexpr int kZoomSliderScale = 100;
inline constexpr int kZoomSliderMaxMultiplier = 12;
inline constexpr int kZoomFocusSliderScale = 100;
inline constexpr int kDisplayColorModeCount = 17;
inline constexpr float kPanKeyboardStep = 0.01f;
inline constexpr float kPanJoystickStep = 0.008f;
inline constexpr float kPanKeyboardUnitsPerSecond = 0.625f;
inline constexpr float kPanJoystickUnitsPerSecond = 0.5f;
inline constexpr int kBlurSigmaSliderMin = 1;
inline constexpr int kBlurSigmaSliderMax = 50;
inline constexpr float kBlurSigmaStep = 0.1f;
inline constexpr std::array<int, 19> kSupportedBlurRadii{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, 35, 40, 45, 50};

inline float SliderValueToSigma(int sliderValue)
{
    const int clamped = std::clamp(sliderValue, kBlurSigmaSliderMin, kBlurSigmaSliderMax);
    return static_cast<float>(clamped) * kBlurSigmaStep;
}

inline int SnapBlurRadius(int value)
{
    const int clamped = std::clamp(value, kSupportedBlurRadii.front(), kSupportedBlurRadii.back());
    int bestRadius = kSupportedBlurRadii.front();
    int bestDelta = std::numeric_limits<int>::max();
    for (const int candidate : kSupportedBlurRadii) {
        const int delta = std::abs(candidate - clamped);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestRadius = candidate;
        } else if (delta == bestDelta && candidate > bestRadius) {
            bestRadius = candidate;
        }
    }
    return bestRadius;
}

} // namespace okuflow::app_constants

