// Thermal Control Plane — version constants.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/version.hpp"

namespace thermal_control_plane {

Version version() noexcept {
    return Version{TCPLANE_VERSION_MAJOR, TCPLANE_VERSION_MINOR, TCPLANE_VERSION_PATCH};
}

const char* version_string() noexcept { return "1.0.0"; }

}  // namespace thermal_control_plane
