// Thermal Control Plane — build and format version identity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_VERSION_HPP
#define THERMAL_CONTROL_PLANE_VERSION_HPP

#include <cstdint>

#ifndef TCPLANE_VERSION_MAJOR
#define TCPLANE_VERSION_MAJOR 1
#endif
#ifndef TCPLANE_VERSION_MINOR
#define TCPLANE_VERSION_MINOR 0
#endif
#ifndef TCPLANE_VERSION_PATCH
#define TCPLANE_VERSION_PATCH 0
#endif

namespace thermal_control_plane {

/// Semantic version of the library actually linked.
struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    friend constexpr bool operator==(const Version& a, const Version& b) noexcept {
        return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
    }
    friend constexpr bool operator!=(const Version& a, const Version& b) noexcept { return !(a == b); }
};

[[nodiscard]] Version version() noexcept;
[[nodiscard]] const char* version_string() noexcept;

/// On-disk format version of the durable thermal state store.
///
/// Increasing this value is a breaking change: a store written by a newer
/// format is refused rather than guessed at.
inline constexpr std::uint32_t kStoreFormatVersion = 1;

/// Maximum accepted durable payload. Declared payload lengths above this are
/// refused before any allocation is attempted.
inline constexpr std::uint64_t kMaxStorePayloadBytes = 1024ULL * 1024ULL;

/// Maximum length of an idempotency key.
inline constexpr std::uint64_t kMaxIdempotencyKeyBytes = 64ULL;

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_VERSION_HPP
