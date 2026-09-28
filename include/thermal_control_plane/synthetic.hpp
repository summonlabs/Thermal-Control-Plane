// Thermal Control Plane — synthetic facility model (not hardware).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_SYNTHETIC_HPP
#define THERMAL_CONTROL_PLANE_SYNTHETIC_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "thermal_control_plane/evidence.hpp"
#include "thermal_control_plane/limits.hpp"
#include "thermal_control_plane/quantity.hpp"

namespace thermal_control_plane {

/// A synthetic thermal zone.
///
/// SYNTHETIC. Every field describes a made-up facility used to exercise the
/// semantics of this runtime. Nothing here models a real chiller plant, CDU,
/// CRAH, airflow path or rack, and no result obtained with it is hardware
/// evidence.
struct SyntheticZoneSpec {
    ZoneRef zone{};
    DomainRef domain{};
    SensorRef sensor{};
    ScopeGeneration scope_generation{};
    /// Temperature with no load and no derating.
    std::int32_t baseline_milli_c = 30000;
    /// Temperature added by full load at zero derating.
    std::int32_t load_rise_milli_c = 45000;
    /// Temperature removed by full derating.
    std::int32_t derate_relief_milli_c = 60000;
    /// Thresholds published for the zone, in limit-kind order.
    std::array<std::int32_t, kLimitKindCount> thresholds{{40000, 75000, 85000, 95000, 105000}};
    BasisPoints max_derate = BasisPoints::from_value_unchecked(4000);
};

/// A deterministic synthetic facility.
///
/// SYNTHETIC. The model is exact integer arithmetic:
///   temperature = baseline + load_rise - relief * derate / 10000
/// so the same derating always produces the same temperature on every host.
class SyntheticFacility {
public:
    SyntheticFacility(FacilityId facility,
                      TopologyGeneration topology_generation,
                      std::vector<SyntheticZoneSpec> zones);

    [[nodiscard]] FacilityId facility() const noexcept { return facility_; }
    [[nodiscard]] TopologyGeneration topology_generation() const noexcept { return topology_generation_; }
    [[nodiscard]] std::size_t zone_count() const noexcept { return zones_.size(); }
    [[nodiscard]] const SyntheticZoneSpec& zone(std::size_t index) const { return zones_.at(index); }
    [[nodiscard]] std::vector<ScopeRef> scopes() const;

    /// Exact synthetic temperature of a zone at a derating.
    [[nodiscard]] std::int32_t temperature(std::size_t index, BasisPoints derate) const;

    /// Override the next reported temperature of a zone, or clear the override.
    void override_temperature(std::size_t index, std::optional<std::int32_t> milli_celsius);
    /// Make a zone report a non-measured observation instead of a value.
    void set_fault(std::size_t index, ObservationQuality quality, EvidenceReason reason);

    [[nodiscard]] ThermalLimitSet limits_for(std::size_t index, LimitGeneration generation) const;
    [[nodiscard]] std::vector<ThermalLimitSet> all_limits(LimitGeneration generation) const;

    /// Produce one observation per zone at the current synthetic state.
    [[nodiscard]] ObservationBatch observe(Tick at,
                                           BasisPoints derate,
                                           ProcessIncarnation source,
                                           EvidenceGeneration generation) const;

    /// The sequence number the next observation of a zone will carry.
    [[nodiscard]] ObservationSequence sequence_of(std::size_t index) const;

    [[nodiscard]] ScopeRef scope_of(std::size_t index) const;

private:
    FacilityId facility_{};
    TopologyGeneration topology_generation_{};
    std::vector<SyntheticZoneSpec> zones_;
    std::vector<std::optional<std::int32_t>> overrides_;
    std::vector<std::pair<ObservationQuality, EvidenceReason>> faults_;
    mutable std::vector<std::uint64_t> sequences_;
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_SYNTHETIC_HPP
