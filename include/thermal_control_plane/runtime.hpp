// Thermal Control Plane — facility thermal authority runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_RUNTIME_HPP
#define THERMAL_CONTROL_PLANE_RUNTIME_HPP

#include <cstddef>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "thermal_control_plane/adapter.hpp"
#include "thermal_control_plane/authority.hpp"
#include "thermal_control_plane/coordination.hpp"
#include "thermal_control_plane/evidence.hpp"
#include "thermal_control_plane/interlock.hpp"
#include "thermal_control_plane/limits.hpp"
#include "thermal_control_plane/persistence.hpp"
#include "thermal_control_plane/policy.hpp"
#include "thermal_control_plane/recovery.hpp"

namespace thermal_control_plane {

namespace detail {
/// Opaque runtime state. Defined only inside the library.
struct RuntimeImpl;
}  // namespace detail

/// Every bound the runtime enforces on caller-controlled resources.
struct RuntimeBounds {
    std::size_t max_scopes = 64;
    std::size_t max_sensors = 1024;
    std::size_t max_observations_per_batch = 256;
    std::size_t max_limit_sets = 64;
    std::size_t max_interlocks = 64;
    std::size_t max_audit_records = 512;
    std::size_t max_directives = 256;
    std::size_t max_replay_records = 256;
    std::size_t max_outbound = 256;
    std::size_t max_verifications = 64;
};

/// Construction parameters.
struct RuntimeOptions {
    /// Durable store path. Empty means a volatile runtime with no durable
    /// authority state.
    std::string store_path;
    RuntimeBounds bounds{};
    ControlPlaneEpoch initial_epoch = ControlPlaneEpoch{1};
    TopologyGeneration initial_topology_generation = TopologyGeneration{1};
    EvidenceGeneration initial_evidence_generation = EvidenceGeneration{1};
};

/// The facility thermal authority runtime.
///
/// Single writer: every mutation is serialised by one exclusive lock, and
/// authority decisions are bound to the exact state they were planned against.
/// The public API is safe to call from several threads; no callback is ever
/// invoked while the lock is held, and the runtime never calls back into the
/// clock or the store while holding anything they could need.
class ThermalRuntime {
public:
    [[nodiscard]] static Result<std::unique_ptr<ThermalRuntime>> create(RuntimeOptions options,
                                                                       std::unique_ptr<Clock> clock);
    ~ThermalRuntime();

    ThermalRuntime(const ThermalRuntime&) = delete;
    ThermalRuntime& operator=(const ThermalRuntime&) = delete;
    ThermalRuntime(ThermalRuntime&&) = delete;
    ThermalRuntime& operator=(ThermalRuntime&&) = delete;

    // -- identity -----------------------------------------------------------
    [[nodiscard]] ProcessIncarnation incarnation() const;
    [[nodiscard]] ControlPlaneEpoch epoch() const;
    [[nodiscard]] StateRevision revision() const;
    [[nodiscard]] CommitSequence commit_sequence() const;
    [[nodiscard]] PolicyGeneration policy_generation() const;
    [[nodiscard]] TopologyGeneration topology_generation() const;
    [[nodiscard]] EvidenceGeneration evidence_generation() const;
    [[nodiscard]] ThermalMode mode() const;
    [[nodiscard]] ThermalMode administrative_mode() const;
    [[nodiscard]] bool durable() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] Tick now();

    /// The binding a new attempt must carry to be accepted.
    [[nodiscard]] Result<AuthorityBinding> binding();

    // -- configuration ------------------------------------------------------
    /// Install a policy. expected_current must equal the installed generation,
    /// or the install is refused as stale or future.
    [[nodiscard]] Status install_policy(const ThermalPolicy& policy, PolicyGeneration expected_current);

    /// Publish the limits of one scope at the current topology generation.
    [[nodiscard]] Status publish_limits(const ThermalLimitSet& limits);

    /// Withdraw the limits of one scope.
    [[nodiscard]] Status remove_limits(ScopeRef scope, LimitGeneration generation);

    /// Set the operator-requested mode. Only Normal, Maintenance and Isolated
    /// are administrative modes; anything else is refused.
    [[nodiscard]] Status set_administrative_mode(ThermalMode mode, bool hold, const AuthorityBinding& binding);

    // -- evidence -----------------------------------------------------------
    [[nodiscard]] Result<IngestReport> ingest(const ObservationBatch& batch);

    [[nodiscard]] Status advance_evidence_generation(EvidenceGeneration expected_current,
                                                     EvidenceGeneration next,
                                                     TopologyGeneration topology_generation);

    /// Declare the complete set of currently asserted interlocks.
    ///
    /// Until a baseline is declared, no authority is granted: a missing
    /// interlock statement is not a statement that nothing is asserted.
    [[nodiscard]] Status declare_interlock_baseline(TopologyGeneration topology_generation,
                                                    std::vector<Interlock> asserted);

    [[nodiscard]] Status apply_interlock(const Interlock& interlock);

    // -- decisions ----------------------------------------------------------
    /// Evaluate the policy without changing anything.
    [[nodiscard]] Result<PolicyOutcome> assess();

    /// Submit an authority attempt.
    ///
    /// Only a structurally unusable attempt (no request identity, no valid
    /// idempotency key) is reported as an error. Every other refusal is a
    /// value carrying a stable code.
    [[nodiscard]] Result<AuthorityOutcome> authorize(const AuthorityAttempt& attempt);

    /// Establish whether a directive was borne out by later evidence.
    [[nodiscard]] Result<VerificationResult> verify_directive(DirectiveId directive);

    // -- inspection ---------------------------------------------------------
    [[nodiscard]] Result<FacilityHeadroom> headroom();
    [[nodiscard]] std::vector<AuditRecord> audit_tail(std::size_t count) const;
    [[nodiscard]] std::vector<DeratingDirective> directive_tail(std::size_t count) const;
    [[nodiscard]] std::vector<CoordinationRequest> outbound() const;
    [[nodiscard]] std::vector<CoordinationRequest> drain_outbound();
    [[nodiscard]] std::size_t outbound_size() const;
    [[nodiscard]] Result<PolicyOutcome> last_outcome() const;

    // -- lifecycle ----------------------------------------------------------
    /// Stop accepting work, release the store lock and stop committing.
    [[nodiscard]] Status close();

    /// Re-read the durable store, replacing in-memory authority state.
    [[nodiscard]] Result<RecoveryReport> recover();

private:
    ThermalRuntime();
    std::unique_ptr<detail::RuntimeImpl> impl_;
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_RUNTIME_HPP
