#pragma once
#include <cmath>
#include <cstdint>
#include <optional>

namespace okuflow {
// One caller-issued camera submission identity. Polling never updates its age.
struct ControlFrameIdentity {
    std::uint64_t sourceEpoch{}, processingVersion{}, viewVersion{}, geometryEpoch{}, frameSequence{};
    std::int64_t submittedAtMs{};
    bool operator==(const ControlFrameIdentity&) const = default;
};
struct ControlDiagnosticSnapshot {
    ControlFrameIdentity frame;
    std::int64_t readyAtMs{};
    std::optional<bool> held, converging, keystoneQuadValid;
    bool stabilizationModelValid{};
};

// Decode completed device values only. Keystone convergence in snapshot belongs
// to the same submitted output. Unknown readiness is never reported as live.
inline void DecodeControlDiagnosticValues(ControlDiagnosticSnapshot& snapshot,
    float modelValid, float tripodState, bool bumpEnabled, float bumpMode, float bumpReadiness) {
    const bool finite = std::isfinite(modelValid) && std::isfinite(tripodState);
    snapshot.stabilizationModelValid = finite && modelValid >= 0.5f;
    if (finite && (tripodState == 0 || tripodState == 1 || tripodState == 2 || tripodState == 3 || tripodState == 4)) {
        const bool unstable = !snapshot.stabilizationModelValid || (tripodState != 1 && tripodState != 4);
        snapshot.converging = unstable || snapshot.converging.value_or(true);
    } else snapshot.converging.reset();
    if (!bumpEnabled) snapshot.held = false;
    else if (std::isfinite(bumpReadiness) && bumpReadiness >= 1.5f &&
             (bumpMode == 0 || bumpMode == 1 || bumpMode == 2)) {
        snapshot.held = bumpMode != 0; // recovery blends held pixels into live
    } else snapshot.held.reset();
}
} // namespace okuflow
