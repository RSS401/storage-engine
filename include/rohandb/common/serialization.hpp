#pragma once

#include <bit>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include <rohandb/common/error.hpp>

namespace rohandb {

static_assert(CHAR_BIT == 8, "The file format requires eight-bit bytes");

// bool is an unsigned_integral in C++, but is not a stored integer field.
// Fixed-width unsigned integers up to 64 bits are the supported format types.
template <typename T>
concept DiskScalar =
    std::unsigned_integral<T> && (!std::same_as<T, bool>) && (sizeof(T) <= sizeof(std::uint64_t));

namespace detail {
// Out of line and cold, so the bounds check below stays small enough to inline.
[[noreturn, gnu::cold, gnu::noinline]] inline void scalarOutOfBounds() {
    throw DbError(ErrorCode::CorruptPage, "scalar extends beyond byte buffer");
}
inline void checkScalarBounds(std::size_t size, std::size_t offset, std::size_t width) {
    // Short-circuit before subtraction; offset + width could overflow.
    if (offset > size || width > size - offset) [[unlikely]]
        scalarOutOfBounds();
}
} // namespace detail

// A borrowed view: no allocation, alignment requirement, or host-endian dependency.
// Invalid accesses throw in release builds too; callers use named layout offsets.
template <DiskScalar T>
[[nodiscard]] T readScalar(std::span<const std::byte> bytes, std::size_t offset) {
    detail::checkScalarBounds(bytes.size(), offset, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        T value;
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    } else {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value |= std::to_integer<std::uint64_t>(bytes[offset + i]) << (8 * i);
        }
        return static_cast<T>(value);
    }
}

template <DiskScalar T> void writeScalar(std::span<std::byte> bytes, std::size_t offset, T value) {
    // Check the whole field first, so a rejected write changes no bytes.
    detail::checkScalarBounds(bytes.size(), offset, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(bytes.data() + offset, &value, sizeof(T));
    } else {
        const auto wide = static_cast<std::uint64_t>(value);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bytes[offset + i] = static_cast<std::byte>((wide >> (8 * i)) & 0xffU);
        }
    }
}

} // namespace rohandb
