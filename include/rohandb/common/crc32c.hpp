#pragma once
#include <rohandb/common/serialization.hpp>
#include <rohandb/storage/page_layout.hpp>
namespace rohandb {
// Uses ARMv8 CRC32C or SSE4.2 instructions when the target has them.
std::uint32_t crc32c(std::span<const std::byte> bytes);
std::uint32_t crc32cSoftware(std::span<const std::byte> bytes); // Portable reference.
void stampChecksum(PageView page);
void verifyChecksum(ConstPageView page);
#ifdef ROHANDB_WAL
inline constexpr std::uint32_t FORMAT_FEATURES = 3;
#elif defined(ROHANDB_CHECKSUMS)
inline constexpr std::uint32_t FORMAT_FEATURES = 1;
#else
inline constexpr std::uint32_t FORMAT_FEATURES = 0;
#endif
} // namespace rohandb
