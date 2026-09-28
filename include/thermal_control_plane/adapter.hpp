// Thermal Control Plane — injected clock adapters.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_ADAPTER_HPP
#define THERMAL_CONTROL_PLANE_ADAPTER_HPP

#include <cstdint>

#include "thermal_control_plane/time.hpp"

namespace thermal_control_plane {

/// The monotonic tick source the runtime reads.
///
/// The contract is narrow on purpose: reading the clock must be cheap, must
/// never fail, must never call back into the runtime, and must never move
/// backwards. Everything that decides authority is expressed in ticks, so the
/// policy layer can be evaluated without a clock at all.
class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual Tick now() = 0;
};

/// A clock advanced explicitly by the caller.
///
/// Deterministic by construction, which is what the examples, the property
/// tests and the CLI use.
class ManualClock final : public Clock {
public:
    explicit ManualClock(Tick start = Tick{1}) noexcept : now_(start) {}

    [[nodiscard]] Tick now() override { return now_; }

    void advance(std::uint64_t ticks) noexcept { now_.value += ticks; }
    void set(Tick tick) noexcept { now_ = tick; }
    [[nodiscard]] Tick peek() const noexcept { return now_; }

private:
    Tick now_{};
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_ADAPTER_HPP
