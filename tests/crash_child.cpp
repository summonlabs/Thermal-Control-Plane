// Thermal Control Plane — non-interactive crash and restart helper process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "thermal_control_plane/persistence.hpp"
#include "thermal_control_plane/runtime.hpp"
#include "thermal_control_plane/wire.hpp"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace thermal_control_plane;

/// End this process immediately, without unwinding and without any
/// interactive crash handling. TerminateProcess is used rather than abort()
/// so no Windows Error Reporting dialog can ever appear.
[[noreturn]] void die_immediately(unsigned code) {
#if defined(_WIN32)
    ::TerminateProcess(::GetCurrentProcess(), code);
    ::ExitProcess(code);
#else
    ::_exit(static_cast<int>(code));
#endif
    for (;;) {
    }
}

/// A backend that kills the process after a chosen number of durable writes.
///
/// This models a real process death in the middle of the commit protocol: the
/// slot write returned, so the bytes may or may not be on stable storage, and
/// nothing after the write ever ran.
class CrashBackend final : public StorageBackend {
public:
    CrashBackend(std::unique_ptr<StorageBackend> inner, std::size_t crash_after_write)
        : inner_(std::move(inner)), crash_after_(crash_after_write) {}

    Status open() override { return inner_->open(); }
    void close() noexcept override { inner_->close(); }
    std::size_t slot_count() const noexcept override { return inner_->slot_count(); }
    Result<std::vector<std::byte>> read_slot(std::size_t slot) const override {
        return inner_->read_slot(slot);
    }
    Status write_slot(std::size_t slot, std::span<const std::byte> bytes) override {
        const Status status = inner_->write_slot(slot, bytes);
        if (!status.ok()) {
            return status;
        }
        ++writes_;
        if (writes_ >= crash_after_) {
            die_immediately(0xC0DEU);
        }
        return status;
    }
    Status erase_slot(std::size_t slot) override { return inner_->erase_slot(slot); }
    const std::string& canonical_root() const noexcept override { return inner_->canonical_root(); }

private:
    std::unique_ptr<StorageBackend> inner_;
    std::size_t crash_after_ = 0;
    std::size_t writes_ = 0;
};

ThermalSnapshot session_snapshot(std::uint64_t revision, ThermalMode mode) {
    ThermalSnapshot snapshot;
    snapshot.epoch = ControlPlaneEpoch{1};
    snapshot.owner = ProcessIncarnation{1};
    snapshot.revision = StateRevision{revision};
    snapshot.policy_present = true;
    snapshot.policy = ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1});
    snapshot.topology_generation = TopologyGeneration{1};
    snapshot.evidence_generation = EvidenceGeneration{1};
    snapshot.mode.mode = mode;
    snapshot.mode.required = mode;
    snapshot.mode.cause = EscalationCause::EvidenceUnknown;
    snapshot.mode.changed_at = Tick{10};
    snapshot.next_directive_id = 1;
    snapshot.next_audit_sequence = 1;
    return snapshot;
}

bool file_exists(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return static_cast<bool>(stream);
}

int hold_lock(const std::string& store_path, const std::string& marker) {
    auto backend = std::make_unique<FileStorageBackend>(store_path);
    DurableStore store(std::move(backend));
    const Status opened = store.open();
    if (!opened.ok()) {
        std::printf("hold: open failed: %s\n", opened.error().render().c_str());
        return 2;
    }
    {
        std::ofstream marker_stream(marker + ".held", std::ios::binary);
        marker_stream << "held";
    }
    for (int spin = 0; spin < 200000; ++spin) {
        if (file_exists(marker + ".release")) {
            store.close();
            std::printf("hold: released\n");
            return 0;
        }
#if defined(_WIN32)
        ::Sleep(1);
#else
        ::usleep(1000);
#endif
    }
    std::printf("hold: release was never signalled\n");
    return 3;
}

int run_commits(const std::string& store_path, int count) {
    auto backend = std::make_unique<FileStorageBackend>(store_path);
    DurableStore store(std::move(backend));
    const Status opened = store.open();
    if (!opened.ok()) {
        std::printf("commits: open failed\n");
        return 2;
    }
    if (!store.load().has_value()) {
        return 2;
    }
    std::uint64_t revision = 1;
    for (int index = 0; index < count; ++index) {
        const auto committed = store.commit(session_snapshot(revision, ThermalMode::Degraded));
        if (!committed.has_value()) {
            std::printf("commits: commit failed: %s\n", committed.error().render().c_str());
            return 2;
        }
        ++revision;
    }
    std::printf("commits: %d generations, last commit %llu\n", count,
                static_cast<unsigned long long>(store.last_commit().value));
    store.close();
    return 0;
}

int crash_after_write(const std::string& store_path, int crash_after) {
    auto inner = std::make_unique<FileStorageBackend>(store_path);
    auto backend = std::make_unique<CrashBackend>(std::move(inner), static_cast<std::size_t>(crash_after));
    DurableStore store(std::move(backend));
    const Status opened = store.open();
    if (!opened.ok()) {
        std::printf("crash-after-write: open failed\n");
        return 2;
    }
    if (!store.load().has_value()) {
        return 2;
    }
    for (int index = 0; index < 8; ++index) {
        const auto committed = store.commit(session_snapshot(static_cast<std::uint64_t>(index) + 1,
                                                             ThermalMode::Degraded));
        if (!committed.has_value()) {
            std::printf("crash-after-write: commit failed: %s\n", committed.error().render().c_str());
            return 2;
        }
    }
    std::printf("crash-after-write: never crashed\n");
    return 0;
}

/// Leave the newest slot holding a torn record.
int torn_slot(const std::string& store_path) {
    auto backend = std::make_unique<FileStorageBackend>(store_path);
    DurableStore store(std::move(backend));
    const Status opened = store.open();
    if (!opened.ok()) {
        std::printf("torn: open failed\n");
        return 2;
    }
    if (!store.load().has_value()) {
        return 2;
    }
    const auto first = store.commit(session_snapshot(1, ThermalMode::Degraded));
    if (!first.has_value()) {
        return 2;
    }
    const std::size_t active = store.active_slot();
    store.close();

    // Build a fully valid record for the next generation, then persist only
    // half of it into the slot the next commit would have used.
    const auto payload = encode_snapshot(session_snapshot(2, ThermalMode::Emergency));
    if (!payload.has_value()) {
        return 2;
    }
    std::vector<std::byte> record(40 + payload.value().size());
    const char magic[8] = {'T', 'C', 'P', 'L', 'N', '0', '0', '1'};
    for (std::size_t i = 0; i < 8; ++i) {
        record[i] = static_cast<std::byte>(magic[i]);
    }
    const auto put32 = [&record](std::size_t offset, std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            record[offset + static_cast<std::size_t>(shift / 8)] =
                static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU);
        }
    };
    const auto put64 = [&record](std::size_t offset, std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            record[offset + static_cast<std::size_t>(shift / 8)] =
                static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFU);
        }
    };
    put32(8, kStoreFormatVersion);
    put32(12, 0);
    put64(16, 2);
    put64(24, static_cast<std::uint64_t>(payload.value().size()));
    put32(32, wire::crc32c(std::span<const std::byte>(payload.value().data(), payload.value().size())));
    put32(36, wire::crc32c(std::span<const std::byte>(record.data(), 36)));
    for (std::size_t i = 0; i < payload.value().size(); ++i) {
        record[40 + i] = payload.value()[i];
    }
    record.resize(record.size() / 2);

    const std::string target = store_path + "." + std::to_string((active + 1) % 2) + ".tcpslot";
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));
    stream.close();
    std::printf("torn: slot %zu holds %zu torn bytes\n", (active + 1) % 2, record.size());
    return 0;
}

/// Run a real runtime session and exit cleanly.
int run_session(const std::string& store_path) {
    RuntimeOptions options;
    options.store_path = store_path;
    auto created = ThermalRuntime::create(options, std::make_unique<ManualClock>(Tick{100}));
    if (!created.has_value()) {
        std::printf("session: create failed: %s\n", created.error().render().c_str());
        return 2;
    }
    std::unique_ptr<ThermalRuntime>& runtime = created.value();
    const Status installed =
        runtime->install_policy(ThermalPolicy::baseline(PolicyId{1}, PolicyGeneration{1}),
                                runtime->policy_generation());
    if (!installed.ok()) {
        std::printf("session: policy install failed: %s\n", installed.error().render().c_str());
        return 2;
    }
    const Status mode_set =
        runtime->set_administrative_mode(ThermalMode::Maintenance, true, runtime->binding().value());
    if (!mode_set.ok()) {
        std::printf("session: administrative mode failed: %s\n", mode_set.error().render().c_str());
        return 2;
    }
    std::printf("session: revision %llu commit %llu\n",
                static_cast<unsigned long long>(runtime->revision().value),
                static_cast<unsigned long long>(runtime->commit_sequence().value));
    const Status closed = runtime->close();
    return closed.ok() ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc < 3) {
        std::printf("usage: crash_child <mode> <store> [argument]\n");
        return 64;
    }
    const std::string mode = argv[1];
    const std::string store_path = argv[2];

    if (mode == "hold") {
        if (argc < 4) {
            return 64;
        }
        return hold_lock(store_path, argv[3]);
    }
    if (mode == "kill") {
        auto backend = std::make_unique<FileStorageBackend>(store_path);
        DurableStore store(std::move(backend));
        if (!store.open().ok()) {
            return 2;
        }
        if (!store.load().has_value()) {
            return 2;
        }
        if (!store.commit(session_snapshot(1, ThermalMode::Degraded)).has_value()) {
            return 2;
        }
        die_immediately(0xDEADU);
    }
    if (mode == "commits") {
        if (argc < 4) {
            return 64;
        }
        return run_commits(store_path, std::atoi(argv[3]));
    }
    if (mode == "crash-after-write") {
        if (argc < 4) {
            return 64;
        }
        return crash_after_write(store_path, std::atoi(argv[3]));
    }
    if (mode == "torn") {
        return torn_slot(store_path);
    }
    if (mode == "session") {
        return run_session(store_path);
    }
    std::printf("unknown mode: %s\n", mode.c_str());
    return 64;
}