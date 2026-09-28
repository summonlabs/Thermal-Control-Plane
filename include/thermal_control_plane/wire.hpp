// Thermal Control Plane — bounded canonical encoding primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_CONTROL_PLANE_WIRE_HPP
#define THERMAL_CONTROL_PLANE_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_control_plane/error.hpp"
#include "thermal_control_plane/version.hpp"

namespace thermal_control_plane::wire {

/// Maximum encoded payload accepted or produced.
inline constexpr std::size_t kMaxPayloadBytes = static_cast<std::size_t>(kMaxStorePayloadBytes);

/// Maximum length of an encoded string field.
inline constexpr std::size_t kMaxStringBytes = 128;

/// Maximum number of elements in one encoded collection.
inline constexpr std::size_t kMaxCollectionElements = 4096;

/// Little-endian canonical writer.
///
/// The writer never allocates from an untrusted size: every write is bounded
/// by kMaxPayloadBytes and the first write that would exceed the bound latches
/// an ENCODING_TOO_LARGE error, which take() reports.
class Writer {
public:
    Writer() { out_.reserve(4096); }

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void boolean(bool value) { u8(value ? 1U : 0U); }
    void raw(std::span<const std::byte> bytes);
    void string(std::string_view text);
    void bytes(std::span<const std::byte> bytes);

    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] const Error& error() const noexcept { return error_; }
    [[nodiscard]] Result<std::vector<std::byte>> take() &&;

private:
    std::vector<std::byte> out_;
    bool overflowed_ = false;
    Error error_{};
};

/// Strict bounded reader.
///
/// Every read is bounds checked against the remaining input. A read past the
/// end latches ENCODING_TRUNCATED; an implausible declared length latches
/// ENCODING_TOO_LARGE. Callers must check ok() before trusting values.
class Reader {
public:
    explicit Reader(std::span<const std::byte> input) : input_(input) {}

    [[nodiscard]] std::uint8_t u8();
    [[nodiscard]] std::uint16_t u16();
    [[nodiscard]] std::uint32_t u32();
    [[nodiscard]] std::uint64_t u64();
    [[nodiscard]] std::int64_t i64();
    [[nodiscard]] bool boolean();
    [[nodiscard]] std::string string();
    [[nodiscard]] std::vector<std::byte> bytes();

    /// Read a collection count and reject implausible values.
    ///
    /// minimum_element_bytes is the smallest number of bytes an element can
    /// occupy, so a declared count that cannot possibly fit in the remaining
    /// input is refused before anything is allocated.
    [[nodiscard]] std::uint32_t count(std::size_t minimum_element_bytes);

    [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return input_.size() - offset_; }

    /// Fail unless every byte of the input has been consumed.
    [[nodiscard]] Status require_end() const;

private:
    [[nodiscard]] bool need(std::size_t bytes);
    void fail(ErrorCode code, std::string detail);

    std::span<const std::byte> input_;
    std::size_t offset_ = 0;
    Error error_{};
};

/// CRC-32C (Castagnoli) over arbitrary bytes.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::string_view text) noexcept;

/// FNV-1a 64-bit digest, used only for human-facing identifiers.
[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] std::string to_hex(std::uint64_t value);

}  // namespace thermal_control_plane::wire

#endif  // THERMAL_CONTROL_PLANE_WIRE_HPP
