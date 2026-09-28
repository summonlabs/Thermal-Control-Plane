// Thermal Control Plane — real independent process proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support.hpp"

#include "process_util.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <memory>
#include <string>
#include <vector>

#ifndef TCPLANE_CRASH_CHILD
#define TCPLANE_CRASH_CHILD "tcplane_crash_child"
#endif

namespace {

using namespace thermal_control_plane;

std::string child() { return std::string(TCPLANE_CRASH_CHILD); }

/// Load the durable store directly and report what it resolved to.
struct LoadedState {
    bool ok = false;
    bool present = false;
    std::size_t slot = 0;
    CommitSequence commit{};
    StateRevision revision{};
    ThermalMode mode = ThermalMode::Normal;
    ErrorCode error = ErrorCode::NONE;
};

LoadedState load_state(const std::string& store_path) {
    LoadedState state;
    auto backend = std::make_unique<FileStorageBackend>(store_path);
    DurableStore store(std::move(backend));
    const Status opened = store.open();
    if (!opened.ok()) {
        state.error = opened.code();
        return state;
    }
    const auto loaded = store.load();
    if (!loaded.has_value()) {
        state.error = loaded.error().code;
        store.close();
        return state;
    }
    state.ok = true;
    state.present = loaded.value().present;
    state.slot = loaded.value().slot;
    state.commit = loaded.value().snapshot.commit_sequence;
    state.revision = loaded.value().snapshot.revision;
    state.mode = loaded.value().snapshot.mode.mode;
    store.close();
    return state;
}

void clean(const std::string& store_path) {
    tcptest::remove_tree(store_path);
    tcptest::remove_tree(store_path + ".0.tcpslot");
    tcptest::remove_tree(store_path + ".1.tcpslot");
    tcptest::remove_tree(store_path + ".lock");
}

}  // namespace

TCPLANE_CASE(multiprocess, store_exclusion_is_enforced_across_processes) {
    tcptest::suppress_crash_ui();
    const std::string store_path = tcptest::unique_path("mp-exclusive") + ".state";
    const std::string marker = tcptest::unique_path("mp-marker");

    TCPLANE_PHASE("a child process holds the store lock");
    auto backend = std::make_unique<FileStorageBackend>(store_path);
    DurableStore store(std::move(backend));
    TCPLANE_STATUS_OK(store.open());
    TCPLANE_OK(store.load());
    TCPLANE_OK(store.commit([] {
        ThermalSnapshot snapshot;
        snapshot.commit_sequence = CommitSequence{1};
        snapshot.epoch = ControlPlaneEpoch{1};
        snapshot.revision = StateRevision{1};
        snapshot.topology_generation = TopologyGeneration{1};
        snapshot.evidence_generation = EvidenceGeneration{1};
        snapshot.next_directive_id = 1;
        snapshot.next_audit_sequence = 1;
        return snapshot;
    }()));
    store.close();

    tcptest::ChildProcess holder = tcptest::ChildProcess::start(child(), {"hold", store_path, marker});
    TCPLANE_CHECK(holder.started());
    TCPLANE_CHECK(tcptest::wait_for_file(marker + ".held"));

    TCPLANE_PHASE("the parent cannot take the store while the child holds it");
    auto blocked = std::make_unique<FileStorageBackend>(store_path);
    TCPLANE_STATUS_ERROR_CODE(blocked->open(), ErrorCode::STORE_LOCKED);
    RuntimeOptions options;
    options.store_path = store_path;
    auto runtime = ThermalRuntime::create(options, std::make_unique<ManualClock>(Tick{1}));
    TCPLANE_ERROR_CODE(runtime, ErrorCode::STORE_LOCKED);

    TCPLANE_PHASE("releasing the child frees the store for this process");
    tcptest::touch_file(marker + ".release");
    const tcptest::ProcessResult held = holder.wait();
    TCPLANE_CHECK(held.exited);
    TCPLANE_CHECK_EQ(held.exit_code, 0);
    TCPLANE_CHECK(held.output.find("released") != std::string::npos);
    auto reopened = std::make_unique<FileStorageBackend>(store_path);
    TCPLANE_STATUS_OK(reopened->open());
    reopened->close();
    clean(store_path);
    tcptest::remove_tree(marker + ".held");
    tcptest::remove_tree(marker + ".release");
}

TCPLANE_CASE(multiprocess, lock_is_released_when_the_owner_is_killed) {
    tcptest::suppress_crash_ui();
    const std::string store_path = tcptest::unique_path("mp-kill") + ".state";

    TCPLANE_PHASE("a child commits a generation and is then killed abruptly");
    const tcptest::ProcessResult killed = tcptest::run_process(child(), {"kill", store_path});
    TCPLANE_CHECK(killed.started);
    TCPLANE_CHECK(killed.exit_code != 0);

    TCPLANE_PHASE("the store is readable and complete after the abrupt death");
    const LoadedState state = load_state(store_path);
    TCPLANE_CHECK(state.ok);
    TCPLANE_CHECK(state.present);
    TCPLANE_CHECK_EQ(state.commit.value, std::uint64_t{1});
    TCPLANE_CHECK_EQ(state.revision.value, std::uint64_t{1});
    TCPLANE_CHECK(state.mode == ThermalMode::Degraded);

    TCPLANE_PHASE("the lock is free again for a new owner");
    const tcptest::ProcessResult again = tcptest::run_process(child(), {"commits", store_path, "2"});
    TCPLANE_CHECK_EQ(again.exit_code, 0);
    const LoadedState after = load_state(store_path);
    TCPLANE_CHECK(after.ok);
    TCPLANE_CHECK_EQ(after.commit.value, std::uint64_t{3});
    clean(store_path);
}

TCPLANE_CASE(multiprocess, death_during_commit_leaves_a_complete_generation) {
    tcptest::suppress_crash_ui();
    const std::string store_path = tcptest::unique_path("mp-crash") + ".state";

    TCPLANE_PHASE("a child dies after its slot write and before anything else");
    const tcptest::ProcessResult crashed = tcptest::run_process(child(), {"crash-after-write", store_path, "1"});
    TCPLANE_CHECK(crashed.started);
    TCPLANE_CHECK(crashed.exit_code != 0);

    TCPLANE_PHASE("the store resolves to a complete generation, never a hybrid");
    const LoadedState state = load_state(store_path);
    TCPLANE_CHECK(state.ok);
    if (state.present) {
        TCPLANE_CHECK(state.commit.value >= 1);
        TCPLANE_CHECK_EQ(state.revision.value, state.commit.value);
        TCPLANE_CHECK(state.mode == ThermalMode::Degraded);
    }

    TCPLANE_PHASE("the store is usable afterwards and continues the sequence");
    const tcptest::ProcessResult resumed = tcptest::run_process(child(), {"commits", store_path, "1"});
    TCPLANE_CHECK_EQ(resumed.exit_code, 0);
    const LoadedState after = load_state(store_path);
    TCPLANE_CHECK(after.ok);
    TCPLANE_CHECK(after.present);
    TCPLANE_CHECK(after.commit.value >= 2);
    clean(store_path);
}

TCPLANE_CASE(multiprocess, torn_slot_resolves_to_the_previous_generation) {
    tcptest::suppress_crash_ui();
    const std::string store_path = tcptest::unique_path("mp-torn") + ".state";

    TCPLANE_PHASE("a child writes one complete generation and one torn record");
    const tcptest::ProcessResult torn = tcptest::run_process(child(), {"torn", store_path});
    TCPLANE_CHECK(torn.started);
    TCPLANE_CHECK_EQ(torn.exit_code, 0);
    TCPLANE_CHECK(torn.output.find("torn") != std::string::npos);

    TCPLANE_PHASE("the store falls back to the complete generation");
    const LoadedState state = load_state(store_path);
    TCPLANE_CHECK(state.ok);
    TCPLANE_CHECK(state.present);
    TCPLANE_CHECK_EQ(state.commit.value, std::uint64_t{1});
    TCPLANE_CHECK(state.mode == ThermalMode::Degraded);

    TCPLANE_PHASE("the next commit repairs the torn slot");
    const tcptest::ProcessResult repaired = tcptest::run_process(child(), {"commits", store_path, "1"});
    TCPLANE_CHECK_EQ(repaired.exit_code, 0);
    const LoadedState after = load_state(store_path);
    TCPLANE_CHECK(after.ok);
    TCPLANE_CHECK_EQ(after.commit.value, std::uint64_t{2});
    TCPLANE_CHECK_EQ(after.slot, std::size_t{1});
    clean(store_path);
}

TCPLANE_CASE(multiprocess, authority_state_survives_a_real_process_restart) {
    tcptest::suppress_crash_ui();
    const std::string store_path = tcptest::unique_path("mp-session") + ".state";

    TCPLANE_PHASE("a child runs a real runtime session and exits cleanly");
    const tcptest::ProcessResult session = tcptest::run_process(child(), {"session", store_path});
    TCPLANE_CHECK(session.started);
    TCPLANE_CHECK_EQ(session.exit_code, 0);
    TCPLANE_CHECK(session.output.find("session: revision") != std::string::npos);

    TCPLANE_PHASE("the parent recovers the administrative mode and advances the epoch");
    RuntimeOptions options;
    options.store_path = store_path;
    auto created = ThermalRuntime::create(options, std::make_unique<ManualClock>(Tick{100}));
    TCPLANE_OK(created);
    ThermalRuntime& runtime = *created.value();
    TCPLANE_CHECK(runtime.mode() == ThermalMode::Maintenance);
    TCPLANE_CHECK(runtime.administrative_mode() == ThermalMode::Maintenance);
    TCPLANE_CHECK(runtime.policy_generation().value == 1);
    TCPLANE_CHECK_EQ(runtime.epoch().value, std::uint64_t{2});

    TCPLANE_PHASE("recovered authority is not fresh evidence: no limits, no headroom, no grants");
    const auto headroom = runtime.headroom();
    TCPLANE_OK(headroom);
    TCPLANE_CHECK(headroom.value().state != HeadroomState::Known);
    TCPLANE_CHECK(headroom.value().reason == HeadroomReason::LimitUndefined);

    TCPLANE_PHASE("the recovered gate is disarmed: dwell from before the restart does not count");
    const auto outcome = runtime.assess();
    TCPLANE_OK(outcome);
    TCPLANE_CHECK(!outcome.value().gate.satisfied);
    TCPLANE_CHECK(outcome.value().mode == ThermalMode::Maintenance);
    TCPLANE_STATUS_OK(runtime.close());
    clean(store_path);
}
