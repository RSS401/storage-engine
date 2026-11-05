#pragma once
#include <rohandb/common/types.hpp>
namespace rohandb::layout {
enum class PageType : std::uint8_t { Meta = 1, Data = 2, Leaf = 3, Internal = 4 };
inline constexpr std::size_t CHECKSUM = 0, TYPE = 4, LSN = 8, COMMON_SIZE = 16;
inline constexpr std::size_t MAGIC = 16, VERSION = 24, PAGE_BYTES = 28, FEATURES = 32;
inline constexpr std::size_t NEXT_PAGE = 36, ROOT = 40, FREE_HEAD = 44, CHECKPOINT = 48;
inline constexpr std::size_t DATABASE_ID = 56;
inline constexpr std::uint64_t MAGIC_VALUE = 0x3130305642444553ULL;
inline constexpr std::uint32_t VERSION_VALUE = 4;
inline constexpr std::size_t SLOT_COUNT = 16, FREE_BEGIN = 18, FREE_END = 20;
inline constexpr std::size_t DATA_HEADER = 24, SLOT_SIZE = 6, SLOT_OFFSET = 0, SLOT_LENGTH = 2,
                             SLOT_FLAGS = 4;
inline constexpr std::size_t RECORD_KEY = 0, RECORD_LENGTH = 8, RECORD_HEADER = 12;
inline constexpr std::size_t MAX_RECORD_SIZE = PAGE_SIZE - DATA_HEADER - SLOT_SIZE;
inline constexpr std::size_t MAX_VALUE_SIZE = MAX_RECORD_SIZE - RECORD_HEADER;
inline constexpr std::size_t NODE_SIZE = 16, NODE_MAX = 18, NEXT_LEAF = 24, ENTRIES = 32;
inline constexpr std::size_t LEAF_ENTRY = 14, LEAF_KEY = 0, LEAF_RID_PAGE = 8, LEAF_RID_SLOT = 12;
inline constexpr std::size_t FIRST_CHILD = 32, FIRST_SEPARATOR = 36, INTERNAL_ENTRY = 12,
                             CHILD_IN_ENTRY = 8;
inline constexpr std::uint16_t LEAF_CAPACITY = (PAGE_SIZE - ENTRIES) / LEAF_ENTRY;
inline constexpr std::uint16_t INTERNAL_CAPACITY =
    1 + (PAGE_SIZE - FIRST_SEPARATOR) / INTERNAL_ENTRY;
static_assert(LEAF_CAPACITY == 290 && INTERNAL_CAPACITY == 339 && MAX_VALUE_SIZE == 4054);
} // namespace rohandb::layout
