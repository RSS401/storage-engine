#include <rohandb/engine/record_manager.hpp>
namespace rohandb {
RecordId RecordManager::insert(std::span<const std::byte> record) {
    if (record.size() > layout::MAX_RECORD_SIZE)
        throw DbError(ErrorCode::RecordTooLarge, "record exceeds page capacity");
    if (current_ != INVALID_PAGE_ID) {
        auto guard = pool_.fetchPage(current_);
        // Check first: mutable access registers (and logs) the page even if nothing changes.
        if (recordFits(guard.data(), record.size()))
            if (const auto slot = SlottedPage(guard.mutableData()).insert(record))
                return {current_, *slot};
    }
    auto guard = pool_.newPage();
    current_ = guard.pageId();
    SlottedPage page(guard.mutableData());
    page.initialise();
    const auto slot = page.insert(record);
    if (!slot)
        throw DbError(ErrorCode::RecordTooLarge, "record does not fit an empty page");
    return {current_, *slot};
}
std::vector<std::byte> RecordManager::read(RecordId rid) {
    auto guard = pool_.fetchPage(rid.page_id);
    const auto record = recordBytes(guard.data(), rid.slot_id);
    return {record.begin(), record.end()};
}
void RecordManager::erase(RecordId rid) {
    auto guard = pool_.fetchPage(rid.page_id);
    SlottedPage(guard.mutableData()).erase(rid.slot_id);
}
} // namespace rohandb
