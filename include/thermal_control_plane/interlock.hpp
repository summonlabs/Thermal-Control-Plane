// Thermal Control Plane — external interlocks.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_INTERLOCK_HPP
#define THERMAL_CONTROL_PLANE_INTERLOCK_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_control_plane/error.hpp"
#include "thermal_control_plane/identity.hpp"
#include "thermal_control_plane/time.hpp"

namespace thermal_control_plane {

/// What an interlock protects against.
///
/// A safety interlock asserts that a physical protection has tripped. An
/// isolation interlock asserts that the facility has been cut out of
/// coordinated operation. A maintenance interlock marks work in progress. Only
/// the first two have thermal authority; the third restricts actuation.
enum class InterlockClass : std::uint8_t {
    Safety = 1,
    Isolation = 2,
    Maintenance = 3,
};

[[nodiscard]] constexpr std::string_view to_string(InterlockClass klass) noexcept {
    switch (klass) {
        case InterlockClass::Safety: return "SAFETY";
        case InterlockClass::Isolation: return "ISOLATION";
        case InterlockClass::Maintenance: return "MAINTENANCE";
    }
    return "UNRECOGNISED_INTERLOCK_CLASS";
}

enum class InterlockState : std::uint8_t {
    Asserted = 1,
    Cleared = 2,
};

[[nodiscard]] constexpr std::string_view to_string(InterlockState state) noexcept {
    switch (state) {
        case InterlockState::Asserted: return "ASSERTED";
        case InterlockState::Cleared: return "CLEARED";
    }
    return "UNRECOGNISED_INTERLOCK_STATE";
}

/// An interlock as reported by its owner.
///
/// The interlock is not created here and its physical cause is not interpreted
/// here: this runtime records that an authoritative external source asserted or
/// cleared it, at which generation, and refuses authority while it stands.
struct Interlock {
    InterlockId id{};
    InterlockClass klass = InterlockClass::Safety;
    ScopeRef scope{};
    bool facility_wide = true;
    InterlockState state = InterlockState::Asserted;
    ScopeGeneration scope_generation{};
    TopologyGeneration topology_generation{};
    ProcessIncarnation author{};
    Tick observed_at{};
    /// Opaque reference to the owning system's record. Never parsed.
    std::string source;
};

/// Bounded registry of currently known interlocks.
class InterlockRegistry {
public:
    static constexpr std::size_t kMaxSourceLength = 96;

    explicit InterlockRegistry(std::size_t max_entries = 256);

    /// Record or update the state of an interlock.
    ///
    /// A clear must name the interlock that was asserted; clearing an unknown
    /// interlock is refused rather than treated as a no-op, so a late clear
    /// cannot mask an assertion that has already happened.
    [[nodiscard]] Status apply(const Interlock& interlock);

    [[nodiscard]] const Interlock* find(InterlockId id) const;
    /// Every recorded interlock, asserted or cleared, in canonical order.
    [[nodiscard]] std::vector<Interlock> all() const;
    /// Replace the whole registry with a complete statement of state.
    [[nodiscard]] Status replace_all(const std::vector<Interlock>& entries);
    [[nodiscard]] std::vector<Interlock> asserted() const;
    [[nodiscard]] bool any_asserted() const noexcept;
    [[nodiscard]] bool any_asserted(InterlockClass klass) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }

private:
    std::size_t max_entries_ = 256;
    std::map<InterlockId, Interlock> entries_;
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_INTERLOCK_HPP
