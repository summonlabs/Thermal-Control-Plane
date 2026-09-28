// Thermal Control Plane — runtime lifecycle and inspection.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "runtime_impl.hpp"

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace thermal_control_plane {

using namespace runtime_detail;

ThermalRuntime::ThermalRuntime() : impl_(std::make_unique<Impl>()) {}

ThermalRuntime::~ThermalRuntime() = default;

Result<std::unique_ptr<ThermalRuntime>> ThermalRuntime::create(RuntimeOptions options,
                                                              std::unique_ptr<Clock> clock) {
    if (clock == nullptr) {
        return Error{ErrorCode::INVALID_ARGUMENT, "runtime requires a clock"};
    }
    const RuntimeBounds& bounds = options.bounds;
    const std::size_t bound_values[] = {bounds.max_scopes,
                                        bounds.max_sensors,
                                        bounds.max_observations_per_batch,
                                        bounds.max_limit_sets,
                                        bounds.max_interlocks,
                                        bounds.max_audit_records,
                                        bounds.max_directives,
                                        bounds.max_replay_records,
                                        bounds.max_outbound,
                                        bounds.max_verifications};
    for (const std::size_t value : bound_values) {
        if (value == 0 || value > kMaxBoundEntries) {
            return Error{ErrorCode::INVALID_ARGUMENT, "runtime bound is zero or beyond the supported maximum"};
        }
    }
    if (options.initial_epoch.is_zero() || options.initial_topology_generation.is_zero() ||
        options.initial_evidence_generation.is_zero()) {
        return Error{ErrorCode::INVALID_ARGUMENT, "initial epoch and generations must be non-zero"};
    }

    std::unique_ptr<ThermalRuntime> runtime(new ThermalRuntime());
    Impl& state = *runtime->impl_;
    state.options = std::move(options);
    state.clock = std::move(clock);
    state.incarnation = make_incarnation();
    state.epoch = state.options.initial_epoch;
    state.revision = StateRevision{1};
    state.topology_generation = state.options.initial_topology_generation;
    state.evidence_generation = state.options.initial_evidence_generation;
    state.limits = LimitRegistry(state.options.bounds.max_limit_sets);
    state.interlocks = InterlockRegistry(state.options.bounds.max_interlocks);
    state.evidence = EvidenceStore(EvidenceBounds{state.options.bounds.max_sensors,
                                                  state.options.bounds.max_scopes,
                                                  state.options.bounds.max_observations_per_batch});

    const Status prepared = state.evidence.reset(state.evidence_generation, state.topology_generation);
    if (!prepared.ok()) {
        return prepared.error();
    }

    const Tick at = state.clock->now();
    bool recovered = false;
    if (!state.options.store_path.empty()) {
        auto backend = std::make_unique<FileStorageBackend>(state.options.store_path);
        state.store = std::make_unique<DurableStore>(std::move(backend));
        const Status opened = state.store->open();
        if (!opened.ok()) {
            return opened.error();
        }
        const auto loaded = state.store->load();
        if (!loaded.has_value()) {
            return loaded.error();
        }
        if (loaded.value().present) {
            recovered = true;
            restore_authority(state, loaded.value().snapshot, at, false);
        }
    }

    AuditInput created;
    created.kind = recovered ? AuditKind::Recovered : AuditKind::Created;
    created.mode = state.mode_record.mode;
    created.detail = recovered ? "recovered durable authority state" : "runtime created";
    push_audit(state, at, created);
    const Status committed = commit_locked(state);
    if (!committed.ok()) {
        return committed.error();
    }
    return runtime;
}

Status ThermalRuntime::close() {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    if (state.closed) {
        return Status::success();
    }
    const Tick at = state.clock->now();
    if (!state.store_faulted) {
        AuditInput closing;
        closing.kind = AuditKind::Closed;
        closing.mode = state.mode_record.mode;
        closing.detail = "runtime closed";
        push_audit(state, at, closing);
        static_cast<void>(commit_locked(state));
    }
    state.closed = true;
    if (state.store != nullptr) {
        state.store->close();
    }
    return Status::success();
}

Result<RecoveryReport> ThermalRuntime::recover() {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    if (state.closed) {
        return Error{ErrorCode::RUNTIME_CLOSED, "runtime is closed"};
    }
    if (state.store == nullptr) {
        return Error{ErrorCode::STORE_IO, "runtime has no durable store to recover from"};
    }
    const auto loaded = state.store->load();
    if (!loaded.has_value()) {
        return loaded.error();
    }
    const Tick at = state.clock->now();
    RecoveryReport report;
    if (loaded.value().present) {
        restore_authority(state, loaded.value().snapshot, at, true);
        report.recovered = true;
        report.slot = loaded.value().slot;
        report.commit_sequence = loaded.value().snapshot.commit_sequence;
        report.revision = state.revision;
        report.mode = state.mode_record.mode;
        report.replay_entries = static_cast<std::uint32_t>(state.replay.size());
        report.audit_entries = static_cast<std::uint32_t>(state.audit.size());
        report.policy_restored = state.policy_present;
        report.interlock_baseline_declared = state.interlock_baseline_declared;
    }
    state.store_faulted = false;
    state.fault_detail.clear();
    AuditInput recovered;
    recovered.kind = AuditKind::Recovered;
    recovered.mode = state.mode_record.mode;
    recovered.detail = "authority state re-read from the durable store";
    push_audit(state, at, recovered);
    const Status committed = commit_locked(state);
    if (!committed.ok()) {
        return committed.error();
    }
    return report;
}

ProcessIncarnation ThermalRuntime::incarnation() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->incarnation;
}

ControlPlaneEpoch ThermalRuntime::epoch() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->epoch;
}

StateRevision ThermalRuntime::revision() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->revision;
}

CommitSequence ThermalRuntime::commit_sequence() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->commit;
}

PolicyGeneration ThermalRuntime::policy_generation() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->policy_present ? impl_->policy.generation : PolicyGeneration{};
}

TopologyGeneration ThermalRuntime::topology_generation() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->topology_generation;
}

EvidenceGeneration ThermalRuntime::evidence_generation() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->evidence_generation;
}

ThermalMode ThermalRuntime::mode() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->mode_record.mode;
}

ThermalMode ThermalRuntime::administrative_mode() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->administrative_mode;
}

bool ThermalRuntime::durable() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->store != nullptr;
}

bool ThermalRuntime::closed() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->closed;
}

Tick ThermalRuntime::now() {
    std::unique_lock lock(impl_->mutex);
    return impl_->clock->now();
}

Result<AuthorityBinding> ThermalRuntime::binding() {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    if (state.closed) {
        return Error{ErrorCode::RUNTIME_CLOSED, "runtime is closed"};
    }
    AuthorityBinding out;
    out.process = state.incarnation;
    out.epoch = state.epoch;
    out.policy = state.policy_present ? state.policy.generation : PolicyGeneration{};
    out.topology = state.topology_generation;
    out.evidence = state.evidence_generation;
    out.revision = state.revision;
    return out;
}

Result<FacilityHeadroom> ThermalRuntime::headroom() {
    std::unique_lock lock(impl_->mutex);
    Impl& state = *impl_;
    if (state.closed) {
        return Error{ErrorCode::RUNTIME_CLOSED, "runtime is closed"};
    }
    const Tick at = state.clock->now();
    const FreshnessWindow window = state.policy_present ? state.policy.freshness : FreshnessWindow{};
    std::vector<ScopeHeadroom> scopes;
    for (const ScopeRef& scope : state.limits.scopes()) {
        scopes.push_back(evaluate_headroom(state.limits.find(scope), state.evidence.view(scope, at, window)));
    }
    return aggregate_headroom(scopes);
}

std::vector<AuditRecord> ThermalRuntime::audit_tail(std::size_t count) const {
    std::shared_lock lock(impl_->mutex);
    const Impl& state = *impl_;
    std::vector<AuditRecord> out;
    const std::size_t take = count < state.audit.size() ? count : state.audit.size();
    out.reserve(take);
    for (std::size_t i = state.audit.size() - take; i < state.audit.size(); ++i) {
        out.push_back(state.audit[i]);
    }
    return out;
}

std::vector<DeratingDirective> ThermalRuntime::directive_tail(std::size_t count) const {
    std::shared_lock lock(impl_->mutex);
    const Impl& state = *impl_;
    std::vector<DeratingDirective> out;
    const std::size_t take = count < state.directives.size() ? count : state.directives.size();
    out.reserve(take);
    for (std::size_t i = state.directives.size() - take; i < state.directives.size(); ++i) {
        out.push_back(state.directives[i]);
    }
    return out;
}

std::vector<CoordinationRequest> ThermalRuntime::outbound() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->outbound;
}

std::vector<CoordinationRequest> ThermalRuntime::drain_outbound() {
    std::unique_lock lock(impl_->mutex);
    std::vector<CoordinationRequest> out;
    out.swap(impl_->outbound);
    return out;
}

std::size_t ThermalRuntime::outbound_size() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->outbound.size();
}

Result<PolicyOutcome> ThermalRuntime::last_outcome() const {
    std::shared_lock lock(impl_->mutex);
    const Impl& state = *impl_;
    if (!state.have_last_outcome) {
        return Error{ErrorCode::EVIDENCE_REQUIRED, "no assessment has been performed"};
    }
    return state.last_outcome;
}

}  // namespace thermal_control_plane
