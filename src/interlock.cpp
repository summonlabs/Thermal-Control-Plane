// Thermal Control Plane — interlock registry.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/interlock.hpp"

namespace thermal_control_plane {

InterlockRegistry::InterlockRegistry(std::size_t max_entries)
    : max_entries_(max_entries < 1 ? 1 : max_entries) {}

Status InterlockRegistry::apply(const Interlock& interlock) {
    if (interlock.id.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "interlock has no identity");
    }
    if (interlock.topology_generation.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "interlock has no topology generation");
    }
    if (interlock.observed_at.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "interlock has no monotonic stamp");
    }
    if (interlock.source.size() > kMaxSourceLength) {
        return Status::failure(ErrorCode::OUT_OF_RANGE, "interlock source reference is too long");
    }
    for (const char raw : interlock.source) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte < 0x20U || byte > 0x7EU) {
            return Status::failure(ErrorCode::INVALID_ARGUMENT,
                                   "interlock source reference must be printable ASCII");
        }
    }
    if (!interlock.facility_wide && interlock.scope.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "scoped interlock has no scope");
    }
    if (!interlock.facility_wide && interlock.scope_generation.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT, "scoped interlock has no scope generation");
    }
    if (interlock.facility_wide && !interlock.scope.is_zero()) {
        return Status::failure(ErrorCode::INVALID_ARGUMENT,
                               "facility-wide interlock must not name a scope");
    }

    const auto existing = entries_.find(interlock.id);
    if (existing == entries_.end()) {
        if (interlock.state == InterlockState::Cleared) {
            return Status::failure(ErrorCode::UNKNOWN_INTERLOCK,
                                   "cannot clear an interlock that was never asserted");
        }
        if (entries_.size() >= max_entries_) {
            return Status::failure(ErrorCode::RESOURCE_EXHAUSTED, "interlock registry bound exceeded");
        }
        entries_.emplace(interlock.id, interlock);
        return Status::success();
    }

    if (interlock.observed_at < existing->second.observed_at) {
        return Status::failure(ErrorCode::STALE_DIRECTIVE, "interlock update is older than the recorded state");
    }
    if (existing->second.klass != interlock.klass || existing->second.scope != interlock.scope ||
        existing->second.facility_wide != interlock.facility_wide) {
        return Status::failure(ErrorCode::DUPLICATE_INTERLOCK,
                               "interlock identity reused with different semantics");
    }
    existing->second = interlock;
    return Status::success();
}

std::vector<Interlock> InterlockRegistry::all() const {
    std::vector<Interlock> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back(entry.second);
    }
    return out;
}

Status InterlockRegistry::replace_all(const std::vector<Interlock>& entries) {
    if (entries.size() > max_entries_) {
        return Status::failure(ErrorCode::RESOURCE_EXHAUSTED, "interlock registry bound exceeded");
    }
    std::map<InterlockId, Interlock> replacement;
    for (const Interlock& interlock : entries) {
        if (interlock.id.is_zero()) {
            return Status::failure(ErrorCode::INVALID_ARGUMENT, "interlock has no identity");
        }
        if (interlock.topology_generation.is_zero()) {
            return Status::failure(ErrorCode::INVALID_ARGUMENT, "interlock has no topology generation");
        }
        if (interlock.observed_at.is_zero()) {
            return Status::failure(ErrorCode::INVALID_ARGUMENT, "interlock has no monotonic stamp");
        }
        if (interlock.source.size() > kMaxSourceLength) {
            return Status::failure(ErrorCode::OUT_OF_RANGE, "interlock source reference is too long");
        }
        for (const char raw : interlock.source) {
            const auto byte = static_cast<unsigned char>(raw);
            if (byte < 0x20U || byte > 0x7EU) {
                return Status::failure(ErrorCode::INVALID_ARGUMENT,
                                       "interlock source reference must be printable ASCII");
            }
        }
        if (interlock.facility_wide && !interlock.scope.is_zero()) {
            return Status::failure(ErrorCode::INVALID_ARGUMENT,
                                   "facility-wide interlock must not name a scope");
        }
        if (!interlock.facility_wide && (interlock.scope.is_zero() || interlock.scope_generation.is_zero())) {
            return Status::failure(ErrorCode::INVALID_ARGUMENT,
                                   "scoped interlock must name a scope and its generation");
        }
        if (!replacement.emplace(interlock.id, interlock).second) {
            return Status::failure(ErrorCode::DUPLICATE_INTERLOCK,
                                   "baseline asserts the same interlock identity twice");
        }
    }
    entries_.swap(replacement);
    return Status::success();
}

const Interlock* InterlockRegistry::find(InterlockId id) const {
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<Interlock> InterlockRegistry::asserted() const {
    std::vector<Interlock> out;
    for (const auto& entry : entries_) {
        if (entry.second.state == InterlockState::Asserted) {
            out.push_back(entry.second);
        }
    }
    return out;
}

bool InterlockRegistry::any_asserted() const noexcept {
    for (const auto& entry : entries_) {
        if (entry.second.state == InterlockState::Asserted) {
            return true;
        }
    }
    return false;
}

bool InterlockRegistry::any_asserted(InterlockClass klass) const noexcept {
    for (const auto& entry : entries_) {
        if (entry.second.state == InterlockState::Asserted && entry.second.klass == klass) {
            return true;
        }
    }
    return false;
}

}  // namespace thermal_control_plane
