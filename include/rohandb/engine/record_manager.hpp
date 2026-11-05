#pragma once
#include <rohandb/buffer/page_guard.hpp>
#include <rohandb/storage/slotted_page.hpp>
namespace rohandb {
class RecordManager {
  public:
    explicit RecordManager(BufferPoolManager &pool) : pool_(pool) {
        pool_.setValidator(layout::PageType::Data, validateDataPage);
    }
    RecordId insert(std::span<const std::byte> record);
    std::vector<std::byte> read(RecordId rid);
    void erase(RecordId rid);
    PageId insertionHint() const { return current_; }
    void restoreHint(PageId id) { current_ = id; }

  private:
    BufferPoolManager &pool_;
    PageId current_ = INVALID_PAGE_ID;
};
} // namespace rohandb
