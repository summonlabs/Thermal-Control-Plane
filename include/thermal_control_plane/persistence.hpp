// Thermal Control Plane — durable thermal state store.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_PERSISTENCE_HPP
#define THERMAL_CONTROL_PLANE_PERSISTENCE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "thermal_control_plane/authority.hpp"
#include "thermal_control_plane/error.hpp"
#include "thermal_control_plane/policy.hpp"
#include "thermal_control_plane/recovery.hpp"
#include "thermal_control_plane/version.hpp"

namespace thermal_control_plane {

/// The authoritative mode record.
struct ModeRecord {
    ThermalMode mode = ThermalMode::Normal;
    ThermalMode required = ThermalMode::Normal;
    EscalationCause cause = EscalationCause::Nominal;
    ScopeRef reason_scope{};
    LimitKind reason_limit = LimitKind::Advisory;
    bool reason_limit_defined = false;
    Tick changed_at{};
};

/// The complete durable thermal state.
///
/// Dynamic observations are deliberately absent: a measurement that survived a
/// restart is not current evidence. Limits are absent for the same reason.
/// What survives is authority, configuration and audit.
struct ThermalSnapshot {
    std::uint32_t format_version = kStoreFormatVersion;
    CommitSequence commit_sequence{};
    ControlPlaneEpoch epoch{};
    ProcessIncarnation owner{};
    StateRevision revision{};

    bool policy_present = false;
    ThermalPolicy policy{};

    TopologyGeneration topology_generation{};
    EvidenceGeneration evidence_generation{};

    ThermalMode administrative_mode = ThermalMode::Normal;
    bool administrative_hold = false;
    ModeRecord mode{};
    RecoveryGateState gate{};

    bool interlock_baseline_declared = false;
    std::vector<Interlock> interlocks{};

    std::uint64_t next_directive_id = 1;
    std::uint64_t next_audit_sequence = 1;

    std::vector<DeratingDirective> directives{};
    std::vector<AuditRecord> audit{};
    std::vector<ReplayRecord> replay{};
};

/// Canonical encoding of a snapshot.
[[nodiscard]] Result<std::vector<std::byte>> encode_snapshot(const ThermalSnapshot& snapshot);

/// Strict decode. Rejects truncation, trailing bytes, unknown enum values,
/// reserved fields holding data, duplicate keys and out-of-range lengths.
[[nodiscard]] Result<ThermalSnapshot> decode_snapshot(std::span<const std::byte> payload);

/// Byte-level view of the persisted record header plus payload.
struct StoreRecord {
    CommitSequence commit_sequence{};
    std::uint32_t format_version = 0;
    std::vector<std::byte> payload;
};

/// A byte-slot storage backend.
///
/// The store is written as alternating generations across two slots. A
/// backend therefore only has to provide durable, readable slots; it does not
/// have to provide atomic rename, and a torn slot write is detected by the
/// record integrity check rather than trusted.
class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    /// Acquire exclusive ownership of the store. Must fail rather than block
    /// when another process holds it.
    [[nodiscard]] virtual Status open() = 0;
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual std::size_t slot_count() const noexcept = 0;
    /// Read a slot. Returns an empty vector when the slot does not exist.
    [[nodiscard]] virtual Result<std::vector<std::byte>> read_slot(std::size_t slot) const = 0;
    /// Durably write a slot. Returns only after the bytes are on stable
    /// storage.
    [[nodiscard]] virtual Status write_slot(std::size_t slot, std::span<const std::byte> bytes) = 0;
    [[nodiscard]] virtual Status erase_slot(std::size_t slot) = 0;

    [[nodiscard]] virtual const std::string& canonical_root() const noexcept = 0;
};

/// File-backed store: two slots plus a lock file.
///
/// The path is canonicalised before the lock is taken, so two processes that
/// spell the same logical store differently cannot obtain different locks.
class FileStorageBackend final : public StorageBackend {
public:
    static constexpr std::size_t kSlots = 2;
    static constexpr std::uint64_t kMaxSlotBytes = kMaxStorePayloadBytes + 1024ULL;

    explicit FileStorageBackend(std::string path);
    ~FileStorageBackend() override;

    FileStorageBackend(const FileStorageBackend&) = delete;
    FileStorageBackend& operator=(const FileStorageBackend&) = delete;

    [[nodiscard]] Status open() override;
    void close() noexcept override;
    [[nodiscard]] std::size_t slot_count() const noexcept override { return kSlots; }
    [[nodiscard]] Result<std::vector<std::byte>> read_slot(std::size_t slot) const override;
    [[nodiscard]] Status write_slot(std::size_t slot, std::span<const std::byte> bytes) override;
    [[nodiscard]] Status erase_slot(std::size_t slot) override;
    [[nodiscard]] const std::string& canonical_root() const noexcept override { return canonical_root_; }

private:
    [[nodiscard]] std::filesystem::path slot_path(std::size_t slot) const;

    std::string requested_path_;
    std::string canonical_root_;
    std::filesystem::path slot_directory_;
    std::string slot_stem_;
    bool open_ = false;
    void* lock_handle_ = nullptr;
};

/// The result of opening a durable store.
struct LoadOutcome {
    /// False when neither slot holds a committed generation.
    bool present = false;
    ThermalSnapshot snapshot{};
    std::size_t slot = 0;
};

/// The recovery outcome: what was restored and what is now suspect.
struct RecoveryReport {
    bool recovered = false;
    std::size_t slot = 0;
    CommitSequence commit_sequence{};
    StateRevision revision{};
    ThermalMode mode = ThermalMode::Normal;
    std::uint32_t replay_entries = 0;
    std::uint32_t audit_entries = 0;
    bool policy_restored = false;
    bool interlock_baseline_declared = false;
};

/// Two-slot durable store over a storage backend.
class DurableStore {
public:
    explicit DurableStore(std::unique_ptr<StorageBackend> backend);

    [[nodiscard]] Status open();
    void close() noexcept;

    /// Resolve the authoritative generation.
    ///
    /// Exactly one generation is authoritative after this call. A slot that
    /// fails integrity checking is ignored rather than merged, and two slots
    /// claiming the same commit sequence are only accepted when their bytes
    /// are identical.
    [[nodiscard]] Result<LoadOutcome> load();

    /// Publish a new generation and verify it by reading the slot back.
    [[nodiscard]] Result<CommitSequence> commit(ThermalSnapshot snapshot);

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] CommitSequence last_commit() const noexcept { return last_commit_; }
    [[nodiscard]] std::size_t active_slot() const noexcept { return active_slot_; }
    [[nodiscard]] const StorageBackend& backend() const noexcept { return *backend_; }

private:
    [[nodiscard]] Result<StoreRecord> decode_record(std::span<const std::byte> raw) const;

    std::unique_ptr<StorageBackend> backend_;
    bool open_ = false;
    /// Set by load(). A commit is only meaningful once the authoritative
    /// generation has been resolved, so committing an unresolved store is
    /// refused rather than silently restarting the sequence.
    bool resolved_ = false;
    bool have_generation_ = false;
    CommitSequence last_commit_{};
    std::size_t active_slot_ = 0;
};

}  // namespace thermal_control_plane

#endif  // THERMAL_CONTROL_PLANE_PERSISTENCE_HPP
