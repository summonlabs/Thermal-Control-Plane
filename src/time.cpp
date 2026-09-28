// Thermal Control Plane — monotonic time arithmetic.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/time.hpp"

namespace thermal_control_plane {

Result<Duration> Duration::from_millis(std::int64_t millis) {
    if (millis < 0) {
        return Error{ErrorCode::OUT_OF_RANGE, "duration must not be negative"};
    }
    return from_millis_unchecked(millis);
}

Result<Duration> elapsed(Tick from, Tick to) {
    if (to < from) {
        return Error{ErrorCode::OUT_OF_RANGE, "monotonic clock moved backwards"};
    }
    const std::uint64_t delta = to.value - from.value;
    if (delta > static_cast<std::uint64_t>(9223372036854775807LL)) {
        return Error{ErrorCode::ARITHMETIC_OVERFLOW, "elapsed duration exceeds representable range"};
    }
    return Duration::from_millis_unchecked(static_cast<std::int64_t>(delta));
}

FreshnessState classify_freshness(Tick now, Tick observed, FreshnessWindow window) noexcept {
    if (observed.is_zero()) {
        return FreshnessState::Unstamped;
    }
    if (observed > now) {
        return FreshnessState::Future;
    }
    const std::uint64_t age = now.value - observed.value;
    const std::int64_t max_age = window.max_age.millis;
    if (max_age < 0) {
        return FreshnessState::Stale;
    }
    if (age > static_cast<std::uint64_t>(max_age)) {
        return FreshnessState::Stale;
    }
    return FreshnessState::Fresh;
}

}  // namespace thermal_control_plane
