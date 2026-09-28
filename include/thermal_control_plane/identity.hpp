// Thermal Control Plane — strongly typed identities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_IDENTITY_HPP
#define THERMAL_CONTROL_PLANE_IDENTITY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "thermal_control_plane/error.hpp"
#include "thermal_control_plane/version.hpp"

namespace thermal_control_plane {

namespace detail {

/// A tagged integer identity.
///
/// The tag makes materially different identities distinct types, so an epoch
/// can never be passed where a generation is expected and a state revision
/// can never be compared against an observation sequence.
template <class Tag, class Rep>
struct StrongId {
    using rep_type = Rep;
    using tag_type = Tag;

    Rep value{};

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(Rep v) noexcept : value(v) {}

    [[nodiscard]] constexpr bool is_zero() const noexcept { return value == Rep{0}; }

    /// Successor, saturating at the maximum representable value rather than
    /// wrapping. Wrapping an authoritative counter would silently resurrect a
    /// superseded generation.
    [[nodiscard]] constexpr StrongId next() const noexcept {
        if (value == static_cast<Rep>(~static_cast<Rep>(0))) {
            return *this;
        }
        return StrongId{static_cast<Rep>(value + Rep{1})};
    }

    friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value == b.value; }
    friend constexpr bool operator!=(StrongId a, StrongId b) noexcept { return a.value != b.value; }
    friend constexpr bool operator<(StrongId a, StrongId b) noexcept { return a.value < b.value; }
    friend constexpr bool operator>(StrongId a, StrongId b) noexcept { return a.value > b.value; }
    friend constexpr bool operator<=(StrongId a, StrongId b) noexcept { return a.value <= b.value; }
    friend constexpr bool operator>=(StrongId a, StrongId b) noexcept { return a.value >= b.value; }
};

}  // namespace detail

struct SiteTag;
struct FacilityTag;
struct ZoneTag;
struct DomainTag;
struct SensorTag;
struct PolicyTag;
struct LimitSetTag;
struct InterlockTag;
struct DirectiveTag;
struct RequestTag;
struct AttemptTag;
struct ObservationTag;
struct SequenceTag;
struct EpochTag;
struct IncarnationTag;
struct RevisionTag;
struct CommitTag;
struct PolicyGenerationTag;
struct TopologyGenerationTag;
struct EvidenceGenerationTag;
struct ScopeGenerationTag;
struct LimitGenerationTag;

using SiteId = detail::StrongId<SiteTag, std::uint32_t>;
using FacilityId = detail::StrongId<FacilityTag, std::uint32_t>;
using ZoneRef = detail::StrongId<ZoneTag, std::uint64_t>;
using DomainRef = detail::StrongId<DomainTag, std::uint64_t>;
using SensorRef = detail::StrongId<SensorTag, std::uint64_t>;
using PolicyId = detail::StrongId<PolicyTag, std::uint64_t>;
using LimitSetId = detail::StrongId<LimitSetTag, std::uint64_t>;
using InterlockId = detail::StrongId<InterlockTag, std::uint64_t>;
using DirectiveId = detail::StrongId<DirectiveTag, std::uint64_t>;
using RequestId = detail::StrongId<RequestTag, std::uint64_t>;
using AttemptId = detail::StrongId<AttemptTag, std::uint64_t>;
using ObservationId = detail::StrongId<ObservationTag, std::uint64_t>;
using ObservationSequence = detail::StrongId<SequenceTag, std::uint64_t>;
using CommitSequence = detail::StrongId<CommitTag, std::uint64_t>;

/// Incarnation of the control-plane process that owns this state.
using ProcessIncarnation = detail::StrongId<IncarnationTag, std::uint64_t>;
/// Monotonic authority epoch of the control plane.
using ControlPlaneEpoch = detail::StrongId<EpochTag, std::uint64_t>;
/// Revision of the in-memory authoritative state.
using StateRevision = detail::StrongId<RevisionTag, std::uint64_t>;

/// Generation of the installed thermal policy.
using PolicyGeneration = detail::StrongId<PolicyGenerationTag, std::uint64_t>;
/// Generation of the facility thermal topology as reported by its owner.
using TopologyGeneration = detail::StrongId<TopologyGenerationTag, std::uint64_t>;
/// Generation of the externally supplied thermal evidence.
using EvidenceGeneration = detail::StrongId<EvidenceGenerationTag, std::uint64_t>;
/// Generation of an externally owned thermal scope (zone or domain object).
using ScopeGeneration = detail::StrongId<ScopeGenerationTag, std::uint64_t>;
/// Generation of an externally owned limit set.
using LimitGeneration = detail::StrongId<LimitGenerationTag, std::uint64_t>;

/// Identity of a thermal scope.
///
/// A scope is a reference into externally owned structure: this runtime never
/// creates zones or domains, it is told which ones exist and at which
/// generation. Ordering is total and canonical so that deterministic
/// tie-breaking over scopes is possible.
struct ScopeRef {
    FacilityId facility{};
    ZoneRef zone{};
    DomainRef domain{};

    friend constexpr bool operator==(const ScopeRef& a, const ScopeRef& b) noexcept {
        return a.facility == b.facility && a.zone == b.zone && a.domain == b.domain;
    }
    friend constexpr bool operator!=(const ScopeRef& a, const ScopeRef& b) noexcept { return !(a == b); }
    friend constexpr bool operator<(const ScopeRef& a, const ScopeRef& b) noexcept {
        if (a.facility != b.facility) {
            return a.facility < b.facility;
        }
        if (a.zone != b.zone) {
            return a.zone < b.zone;
        }
        return a.domain < b.domain;
    }
    friend constexpr bool operator>(const ScopeRef& a, const ScopeRef& b) noexcept { return b < a; }
    friend constexpr bool operator<=(const ScopeRef& a, const ScopeRef& b) noexcept { return !(b < a); }
    friend constexpr bool operator>=(const ScopeRef& a, const ScopeRef& b) noexcept { return !(a < b); }

    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return facility.is_zero() && zone.is_zero() && domain.is_zero();
    }
};

[[nodiscard]] std::string to_string(const ScopeRef& scope);

/// The facility-wide scope, used by directives that are not scope-local.
[[nodiscard]] ScopeRef facility_scope(FacilityId facility) noexcept;

/// A bounded, validated idempotency key.
///
/// The key is an opaque caller-chosen token. It is deliberately not free-form
/// text: keys are restricted to printable ASCII so that encodings, logs and
/// ordering stay unambiguous across platforms.
class IdempotencyKey {
public:
    static constexpr std::size_t kMaxLength = 64;

    IdempotencyKey() = default;

    [[nodiscard]] static Result<IdempotencyKey> parse(std::string_view text);

    [[nodiscard]] bool valid() const noexcept { return length_ != 0; }
    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(bytes_.data(), static_cast<std::size_t>(length_));
    }
    [[nodiscard]] std::string str() const { return std::string(view()); }

    friend bool operator==(const IdempotencyKey& a, const IdempotencyKey& b) noexcept {
        return a.length_ == b.length_ && a.bytes_ == b.bytes_;
    }
    friend bool operator!=(const IdempotencyKey& a, const IdempotencyKey& b) noexcept { return !(a == b); }
    friend bool operator<(const IdempotencyKey& a, const IdempotencyKey& b) noexcept {
        return a.view() < b.view();
    }

private:
    std::array<char, kMaxLength> bytes_{};
    std::uint8_t length_ = 0;
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_IDENTITY_HPP
