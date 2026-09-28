// Thermal Control Plane — canonical encoding primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_control_plane/wire.hpp"

#include <array>

namespace thermal_control_plane::wire {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0x82F63B78U;
            } else {
                crc >>= 1U;
            }
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

}  // namespace

void Writer::u8(std::uint8_t value) {
    if (overflowed_) {
        return;
    }
    if (out_.size() + 1 > kMaxPayloadBytes) {
        overflowed_ = true;
        error_ = Error{ErrorCode::ENCODING_TOO_LARGE, "encoded payload exceeds the configured bound"};
        return;
    }
    out_.push_back(static_cast<std::byte>(value));
}

void Writer::u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xFFU));
    u8(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void Writer::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void Writer::u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void Writer::i64(std::int64_t value) {
    u64(static_cast<std::uint64_t>(value));
}

void Writer::raw(std::span<const std::byte> bytes) {
    if (overflowed_) {
        return;
    }
    if (out_.size() + bytes.size() > kMaxPayloadBytes) {
        overflowed_ = true;
        error_ = Error{ErrorCode::ENCODING_TOO_LARGE, "encoded payload exceeds the configured bound"};
        return;
    }
    out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void Writer::string(std::string_view text) {
    if (text.size() > kMaxStringBytes) {
        overflowed_ = true;
        error_ = Error{ErrorCode::ENCODING_TOO_LARGE, "string field exceeds the configured bound"};
        return;
    }
    u16(static_cast<std::uint16_t>(text.size()));
    raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void Writer::bytes(std::span<const std::byte> bytes) {
    if (bytes.size() > kMaxStringBytes) {
        overflowed_ = true;
        error_ = Error{ErrorCode::ENCODING_TOO_LARGE, "byte field exceeds the configured bound"};
        return;
    }
    u16(static_cast<std::uint16_t>(bytes.size()));
    raw(bytes);
}

Result<std::vector<std::byte>> Writer::take() && {
    if (overflowed_) {
        return error_;
    }
    return std::move(out_);
}

bool Reader::need(std::size_t bytes) {
    if (!error_.ok()) {
        return false;
    }
    if (bytes > input_.size() - offset_) {
        fail(ErrorCode::ENCODING_TRUNCATED, "encoded input ended before the record was complete");
        return false;
    }
    return true;
}

void Reader::fail(ErrorCode code, std::string detail) {
    if (error_.ok()) {
        error_ = Error{code, std::move(detail)};
    }
}

std::uint8_t Reader::u8() {
    if (!need(1)) {
        return 0;
    }
    const auto value = static_cast<std::uint8_t>(input_[offset_]);
    ++offset_;
    return value;
}

std::uint16_t Reader::u16() {
    if (!need(2)) {
        return 0;
    }
    std::uint16_t value = 0;
    for (int shift = 0; shift < 16; shift += 8) {
        value = static_cast<std::uint16_t>(value |
                                           static_cast<std::uint16_t>(static_cast<std::uint16_t>(input_[offset_]) << shift));
        ++offset_;
    }
    return value;
}

std::uint32_t Reader::u32() {
    if (!need(4)) {
        return 0;
    }
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(input_[offset_]) << shift;
        ++offset_;
    }
    return value;
}

std::uint64_t Reader::u64() {
    if (!need(8)) {
        return 0;
    }
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(input_[offset_]) << shift;
        ++offset_;
    }
    return value;
}

std::int64_t Reader::i64() { return static_cast<std::int64_t>(u64()); }

bool Reader::boolean() {
    const std::uint8_t raw = u8();
    if (!error_.ok()) {
        return false;
    }
    if (raw > 1U) {
        fail(ErrorCode::ENCODING_INVALID, "boolean field holds a value other than zero or one");
        return false;
    }
    return raw == 1U;
}

std::string Reader::string() {
    const std::uint16_t length = u16();
    if (!error_.ok()) {
        return {};
    }
    if (length > kMaxStringBytes) {
        fail(ErrorCode::ENCODING_TOO_LARGE, "declared string length exceeds the configured bound");
        return {};
    }
    if (!need(length)) {
        return {};
    }
    const auto* begin = reinterpret_cast<const char*>(input_.data() + offset_);
    std::string out(begin, static_cast<std::size_t>(length));
    offset_ += length;
    return out;
}

std::vector<std::byte> Reader::bytes() {
    const std::uint16_t length = u16();
    if (!error_.ok()) {
        return {};
    }
    if (length > kMaxStringBytes) {
        fail(ErrorCode::ENCODING_TOO_LARGE, "declared byte field length exceeds the configured bound");
        return {};
    }
    if (!need(length)) {
        return {};
    }
    std::vector<std::byte> out(input_.begin() + static_cast<std::ptrdiff_t>(offset_),
                               input_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
    offset_ += length;
    return out;
}

std::uint32_t Reader::count(std::size_t minimum_element_bytes) {
    const std::uint32_t declared = u32();
    if (!error_.ok()) {
        return 0;
    }
    if (declared > kMaxCollectionElements) {
        fail(ErrorCode::ENCODING_TOO_LARGE, "declared collection length exceeds the configured bound");
        return 0;
    }
    const std::size_t minimum = minimum_element_bytes == 0 ? 1 : minimum_element_bytes;
    if (static_cast<std::size_t>(declared) > remaining() / minimum) {
        fail(ErrorCode::ENCODING_TRUNCATED, "declared collection length cannot fit in the remaining input");
        return 0;
    }
    return declared;
}

Status Reader::require_end() const {
    if (!error_.ok()) {
        return Status{error_};
    }
    if (offset_ != input_.size()) {
        return Status::failure(ErrorCode::ENCODING_TRAILING_BYTES, "encoded input has unconsumed trailing bytes");
    }
    return Status::success();
}

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte raw : bytes) {
        const auto index = static_cast<std::size_t>((crc ^ static_cast<std::uint32_t>(raw)) & 0xFFU);
        crc = (crc >> 8U) ^ kCrcTable[index];
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t crc32c(std::string_view text) noexcept {
    return crc32c(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte raw : bytes) {
        hash ^= static_cast<std::uint64_t>(raw);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string to_hex(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xFU];
        value >>= 4U;
    }
    return out;
}

}  // namespace thermal_control_plane::wire
