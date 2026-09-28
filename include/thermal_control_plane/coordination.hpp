// Thermal Control Plane — coordination surface.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_COORDINATION_HPP
#define THERMAL_CONTROL_PLANE_COORDINATION_HPP

// The coordination surface lives in authority.hpp with the attempt and outcome
// types it belongs to. This header exists so that a translation unit that only
// needs to publish or consume coordination notices can include a single,
// intention-revealing name.

#include "thermal_control_plane/authority.hpp"

#endif  // THERMAL_CONTROL_PLANE_COORDINATION_HPP
