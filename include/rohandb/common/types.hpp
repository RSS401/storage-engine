#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
namespace rohandb {
using PageId = std::uint32_t;
using FrameId = std::uint32_t;
using SlotId = std::uint16_t;
using Lsn = std::uint64_t;
inline constexpr std::size_t PAGE_SIZE = 4096;
inline constexpr PageId META_PAGE_ID = 0;
inline constexpr PageId INVALID_PAGE_ID = std::numeric_limits<PageId>::max();
inline constexpr Lsn INVALID_LSN = 0;
using PageBytes = std::array<std::byte, PAGE_SIZE>; // actual bytes
using PageView = std::span<std::byte, PAGE_SIZE>; // write, borrows
using ConstPageView = std::span<const std::byte, PAGE_SIZE>; // read, borrows
struct RecordId {
    PageId page_id;
    SlotId slot_id;
    bool operator==(const RecordId &) const = default;
};
} // namespace rohandb
