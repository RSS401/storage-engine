#pragma once
#include <rohandb/common/crc32c.hpp>
#include <rohandb/disk_manager.hpp>
namespace rohandb {
using DatabaseId = std::array<std::byte, 16>;
inline constexpr PageId IDENTITY_PAGE_ID = 1;
namespace identity_layout {
inline constexpr std::size_t MAGIC = 16, VERSION = 24, FEATURES = 28, ID = 32;
inline constexpr std::uint64_t MAGIC_VALUE = 0x3430444942444852ULL;
inline constexpr std::uint8_t TYPE = 5;
} // namespace identity_layout
DatabaseId newDatabaseId();
DatabaseId readId(std::span<const std::byte> bytes, std::size_t offset);
void writeId(std::span<std::byte> bytes, std::size_t offset, const DatabaseId &id);
PageBytes makeIdentityPage(const DatabaseId &id);
DatabaseId readDatabaseId(IDiskManager &disk);
} // namespace rohandb
