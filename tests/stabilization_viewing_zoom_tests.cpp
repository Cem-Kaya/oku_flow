#include "okuflow/cuda/cuda_interop.hpp"

#include <iostream>
#include <limits>

int main() {
    okuflow::ProcessingSettings settings{};
    auto expectZoom = [&](float expected, const char* scenario) {
        const float actual = settings.EffectiveViewingMagnification();
        if (actual == expected) {
            return true;
        }
        std::cerr << scenario << ": expected " << expected
                  << "x, got " << actual << "x\n";
        return false;
    };

    bool passed = expectZoom(1.0f, "default unmagnified scene");
    // RunCudaPipeline deliberately leaves enableZoom false: the presenter
    // applies magnification after stateful effects process the full scene.
    for (const float zoom : {1.33f, 4.0f, 16.0f}) {
        settings.zoomAmount = zoom;
        settings.enableZoom = false;
        passed = expectZoom(zoom, "presentation-owned magnification") && passed;
        settings.enableZoom = true;
        passed = expectZoom(zoom, "legacy CUDA magnification") && passed;
    }
    // Disabling magnification changes zoomAmount to 1, not the legacy flag.
    settings.enableZoom = false;
    settings.zoomAmount = 1.0f;
    passed = expectZoom(1.0f, "viewport zoom disabled after magnification") && passed;

    for (const bool imageZoomEnabled : {false, true}) {
        settings.enableZoom = imageZoomEnabled;
        for (const float invalidZoom : {
                 0.0f, -4.0f, 0.5f,
                 std::numeric_limits<float>::quiet_NaN(),
                 std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity()}) {
            settings.zoomAmount = invalidZoom;
            passed = expectZoom(1.0f, "invalid or sub-unity magnification") && passed;
        }
    }
    return passed ? 0 : 1;
}
