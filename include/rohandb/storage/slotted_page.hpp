#pragma once
#include <optional>
#include <rohandb/common/serialization.hpp>
#include <rohandb/storage/page_layout.hpp>
#include <vector>
namespace rohandb {
// A non-owning view. Slot numbers survive compaction, but deleted slots can be reused.
class SlottedPage {
  public:
    explicit SlottedPage(PageView bytes) : bytes_(bytes) {}
    void initialise();
    void validate() const;
    std::optional<SlotId> insert(std::span<const std::byte> record);
    std::vector<std::byte> read(SlotId slot) const;
    void erase(SlotId slot);
    void compact();
    std::uint16_t slotCount() const;

  private:
    PageView bytes_;
    std::size_t slotOffset(SlotId slot) const;
};
// Full structural check of a data page, including overlapping extents.
void validateDataPage(ConstPageView bytes, PageId next_page_id = INVALID_PAGE_ID);
// One record from an already validated page, with O(1) checks of that slot only.
std::span<const std::byte> recordBytes(ConstPageView bytes, SlotId slot);
// Whether insert would succeed (possibly after compaction), without modifying the page.
bool recordFits(ConstPageView bytes, std::size_t record_size);
std::vector<std::byte> encodeRecord(std::uint64_t key, std::string_view value);
std::pair<std::uint64_t, std::string> decodeRecord(std::span<const std::byte> record);
} // namespace rohandb
