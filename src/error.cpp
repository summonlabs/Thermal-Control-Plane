// Thermal Control Plane — error rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/error.hpp"

namespace thermal_control_plane {

std::string Error::render() const {
    std::string out;
    out.reserve(detail.size() + 32);
    out.append(to_string(code));
    if (!detail.empty()) {
        out.append(": ");
        out.append(detail);
    }
    return out;
}

}  // namespace thermal_control_plane
