// Thermal Control Plane — recovery hysteresis gating.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_RECOVERY_HPP
#define THERMAL_CONTROL_PLANE_RECOVERY_HPP

#include <cstdint>
#include <vector>

#include "thermal_control_plane/headroom.hpp"
#include "thermal_control_plane/mode.hpp"
#include "thermal_control_plane/time.hpp"

namespace thermal_control_plane {

/// The state of the recovery gate for the current mode.
///
/// The gate turns "the temperature looks better now" into "the facility may
/// step down one rung". It requires a continuous dwell below a hysteresis
/// threshold and a minimum number of distinct evidence advances, so a single
/// favourable observation can never move the facility.
struct RecoveryGateState {
    /// True once the gate has been opened for a mode and is watching.
    bool armed = false;
    /// The mode the gate was opened for. A mode change disarms the gate.
    ThermalMode armed_for = ThermalMode::Normal;
    /// The mode reached when the gate completes.
    ThermalMode target = ThermalMode::Normal;
    /// Tick of the first favourable observation in the current run.
    Tick favorable_since{};
    /// Distinct evidence advances counted in the current run.
    std::uint32_t samples = 0;
    /// Highest evidence sequence counted, so repeated evaluation is idempotent.
    ObservationSequence last_counted{};
    /// True when the dwell and sample requirements are both met.
    bool satisfied = false;
    /// Why the gate is not favourable, when it is not.
    HeadroomReason blocked_reason = HeadroomReason::NoEvidence;
};

/// Inputs the gate needs that are not part of the evidence itself.
struct RecoveryGateInput {
    ThermalMode current_mode = ThermalMode::Normal;
    Duration dwell{};
    std::uint32_t min_observations = 1;
    TemperatureDelta hysteresis{};
    Tick now{};
};

/// Advance the recovery gate.
///
/// Pure with respect to the runtime: the same gate state, evidence and tick
/// always produce the same next gate state, and repeated evaluation of
/// unchanged evidence never accumulates samples.
[[nodiscard]] RecoveryGateState advance_recovery_gate(const RecoveryGateState& previous,
                                                      const RecoveryGateInput& input,
                                                      const std::vector<ScopeHeadroom>& scopes);

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_RECOVERY_HPP
