// Thermal Control Plane — mode precedence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/mode.hpp"

namespace thermal_control_plane {

bool reason_outranks(const ModeReason& a, const ModeReason& b) noexcept {
    const std::uint8_t pa = cause_precedence(a.cause);
    const std::uint8_t pb = cause_precedence(b.cause);
    if (pa != pb) {
        return pa < pb;
    }
    if (!(a.scope == b.scope)) {
        return a.scope < b.scope;
    }
    if (a.limit_defined != b.limit_defined) {
        return a.limit_defined;
    }
    if (a.limit_defined && a.limit != b.limit) {
        return limit_severity(a.limit) > limit_severity(b.limit);
    }
    if (!(a.sensor == b.sensor)) {
        return a.sensor < b.sensor;
    }
    return false;
}

bool same_reason(const ModeReason& a, const ModeReason& b) noexcept {
    return a == b;
}

}  // namespace thermal_control_plane
