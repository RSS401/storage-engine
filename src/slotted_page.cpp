#include <algorithm>
#include <rohandb/storage/slotted_page.hpp>
namespace rohandb {
namespace {
void corrupt(bool condition, const char *message) {
    if (condition)
        throw DbError(ErrorCode::CorruptPage, message);
}
struct Space {
    SlotId slot;        // First deleted slot, or a new slot at the end.
    std::size_t needed; // Record bytes plus any new slot entry.
    bool fits;          // After compaction, if necessary.
};
Space space(ConstPageView bytes, std::size_t record_size) {
    const auto count = readScalar<std::uint16_t>(bytes, layout::SLOT_COUNT);
    SlotId slot = count;
    std::size_t live = 0;
    for (SlotId i = 0; i < count; ++i) {
        const auto entry = layout::DATA_HEADER + i * layout::SLOT_SIZE;
        if (readScalar<std::uint16_t>(bytes, entry + layout::SLOT_FLAGS) == 0) {
            if (slot == count)
                slot = i;
        } else
            live += readScalar<std::uint16_t>(bytes, entry + layout::SLOT_LENGTH);
    }
    const auto begin = readScalar<std::uint16_t>(bytes, layout::FREE_BEGIN);
    const auto needed = record_size + (slot == count ? layout::SLOT_SIZE : 0);
    return {slot, needed, needed <= PAGE_SIZE - begin - live};
}
} // namespace
bool recordFits(ConstPageView bytes, std::size_t record_size) {
    // Bounds-checked reads only; insert still validates the page before changing it.
    return space(bytes, record_size).fits;
}
void SlottedPage::initialise() {
    std::fill(bytes_.begin(), bytes_.end(), std::byte{0});
    writeScalar<std::uint8_t>(bytes_, layout::TYPE,
                              static_cast<std::uint8_t>(layout::PageType::Data));
    writeScalar<std::uint16_t>(bytes_, layout::FREE_BEGIN, layout::DATA_HEADER);
    writeScalar<std::uint16_t>(bytes_, layout::FREE_END, PAGE_SIZE);
}
std::uint16_t SlottedPage::slotCount() const {
    return readScalar<std::uint16_t>(bytes_, layout::SLOT_COUNT);
}
void SlottedPage::validate() const {
    validateDataPage(bytes_);
}
void validateDataPage(ConstPageView bytes, PageId) {
    corrupt(readScalar<std::uint8_t>(bytes, layout::TYPE) !=
                static_cast<std::uint8_t>(layout::PageType::Data),
            "not a data page");
    const auto count = readScalar<std::uint16_t>(bytes, layout::SLOT_COUNT);
    const auto begin = readScalar<std::uint16_t>(bytes, layout::FREE_BEGIN);
    const auto end = readScalar<std::uint16_t>(bytes, layout::FREE_END);
    corrupt(begin != layout::DATA_HEADER + layout::SLOT_SIZE * count || begin > end ||
                end > PAGE_SIZE,
            "invalid data page free space");
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = layout::DATA_HEADER + i * layout::SLOT_SIZE;
        const auto flags = readScalar<std::uint16_t>(bytes, slot + layout::SLOT_FLAGS);
        corrupt(flags > 1, "invalid slot flags");
        if (flags == 0)
            continue;
        const auto offset = readScalar<std::uint16_t>(bytes, slot + layout::SLOT_OFFSET);
        const auto length = readScalar<std::uint16_t>(bytes, slot + layout::SLOT_LENGTH);
        corrupt(offset < end || length < layout::RECORD_HEADER ||
                    static_cast<std::size_t>(offset) + length > PAGE_SIZE,
                "invalid record extent");
        ranges.emplace_back(offset, static_cast<std::size_t>(offset) + length);
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i)
        corrupt(ranges[i].first < ranges[i - 1].second, "overlapping record extents");
}
std::span<const std::byte> recordBytes(ConstPageView bytes, SlotId slot) {
    corrupt(readScalar<std::uint8_t>(bytes, layout::TYPE) !=
                static_cast<std::uint8_t>(layout::PageType::Data),
            "not a data page");
    corrupt(slot >= readScalar<std::uint16_t>(bytes, layout::SLOT_COUNT), "slot does not exist");
    const auto entry = layout::DATA_HEADER + slot * layout::SLOT_SIZE;
    corrupt(readScalar<std::uint16_t>(bytes, entry + layout::SLOT_FLAGS) != 1, "slot is deleted");
    const auto offset = readScalar<std::uint16_t>(bytes, entry + layout::SLOT_OFFSET);
    const auto length = readScalar<std::uint16_t>(bytes, entry + layout::SLOT_LENGTH);
    corrupt(offset < readScalar<std::uint16_t>(bytes, layout::FREE_END) ||
                length < layout::RECORD_HEADER ||
                static_cast<std::size_t>(offset) + length > PAGE_SIZE,
            "invalid record extent");
    return bytes.subspan(offset, length);
}
std::size_t SlottedPage::slotOffset(SlotId slot) const {
    validate();
    corrupt(slot >= slotCount(), "slot does not exist");
    const auto offset = layout::DATA_HEADER + slot * layout::SLOT_SIZE;
    corrupt(readScalar<std::uint16_t>(bytes_, offset + layout::SLOT_FLAGS) == 0, "slot is deleted");
    return offset;
}
std::vector<std::byte> SlottedPage::read(SlotId slot) const {
    const auto entry = slotOffset(slot);
    const auto offset = readScalar<std::uint16_t>(bytes_, entry + layout::SLOT_OFFSET);
    const auto length = readScalar<std::uint16_t>(bytes_, entry + layout::SLOT_LENGTH);
    return {bytes_.begin() + offset, bytes_.begin() + offset + length};
}
void SlottedPage::erase(SlotId slot) {
    const auto entry = slotOffset(slot);
    writeScalar<std::uint16_t>(bytes_, entry + layout::SLOT_FLAGS, 0);
}
void SlottedPage::compact() {
    validate();
    // Copy source first: payload order need not match slot order after slot reuse.
    PageBytes original;
    std::copy(bytes_.begin(), bytes_.end(), original.begin());
    std::size_t end = PAGE_SIZE;
    for (std::size_t i = 0; i < slotCount(); ++i) {
        const auto slot = layout::DATA_HEADER + i * layout::SLOT_SIZE;
        if (readScalar<std::uint16_t>(original, slot + layout::SLOT_FLAGS) == 0)
            continue;
        const auto offset = readScalar<std::uint16_t>(original, slot + layout::SLOT_OFFSET);
        const auto length = readScalar<std::uint16_t>(original, slot + layout::SLOT_LENGTH);
        end -= length;
        std::copy_n(original.begin() + offset, length,
                    bytes_.begin() + static_cast<std::ptrdiff_t>(end));
        writeScalar<std::uint16_t>(bytes_, slot + layout::SLOT_OFFSET,
                                   static_cast<std::uint16_t>(end));
    }
    writeScalar<std::uint16_t>(bytes_, layout::FREE_END, static_cast<std::uint16_t>(end));
}
std::optional<SlotId> SlottedPage::insert(std::span<const std::byte> record) {
    if (record.size() > layout::MAX_RECORD_SIZE)
        throw DbError(ErrorCode::RecordTooLarge, "record exceeds page capacity");
    if (record.size() < layout::RECORD_HEADER)
        throw DbError(ErrorCode::InvalidArgument, "record is too short");
    validate();
    const auto [slot, needed, fits] = space(bytes_, record.size());
    if (!fits)
        return std::nullopt;
    auto begin = readScalar<std::uint16_t>(bytes_, layout::FREE_BEGIN);
    auto end = readScalar<std::uint16_t>(bytes_, layout::FREE_END);
    // Snapshot allows callers to pass a view into the page being compacted.
    const std::vector<std::byte> copy(record.begin(), record.end());
    if (needed > static_cast<std::size_t>(end - begin)) {
        compact();
        end = readScalar<std::uint16_t>(bytes_, layout::FREE_END);
    }
    end = static_cast<std::uint16_t>(end - copy.size());
    std::copy(copy.begin(), copy.end(), bytes_.begin() + end);
    const auto entry = layout::DATA_HEADER + slot * layout::SLOT_SIZE;
    writeScalar<std::uint16_t>(bytes_, entry + layout::SLOT_OFFSET, end);
    writeScalar<std::uint16_t>(bytes_, entry + layout::SLOT_LENGTH,
                               static_cast<std::uint16_t>(copy.size()));
    writeScalar<std::uint16_t>(bytes_, entry + layout::SLOT_FLAGS, 1);
    if (slot == slotCount()) {
        writeScalar<std::uint16_t>(bytes_, layout::SLOT_COUNT,
                                   static_cast<std::uint16_t>(slot + 1));
        begin = static_cast<std::uint16_t>(begin + layout::SLOT_SIZE);
        writeScalar<std::uint16_t>(bytes_, layout::FREE_BEGIN, begin);
    }
    writeScalar<std::uint16_t>(bytes_, layout::FREE_END, end);
    return slot;
}
std::vector<std::byte> encodeRecord(std::uint64_t key, std::string_view value) {
    if (value.size() > layout::MAX_VALUE_SIZE)
        throw DbError(ErrorCode::RecordTooLarge, "value exceeds 4054 bytes");
    std::vector<std::byte> result(layout::RECORD_HEADER + value.size());
    writeScalar<std::uint64_t>(result, layout::RECORD_KEY, key);
    writeScalar<std::uint32_t>(result, layout::RECORD_LENGTH,
                               static_cast<std::uint32_t>(value.size()));
    std::transform(value.begin(), value.end(), result.begin() + layout::RECORD_HEADER,
                   [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });
    return result;
}
std::pair<std::uint64_t, std::string> decodeRecord(std::span<const std::byte> record) {
    const auto key = readScalar<std::uint64_t>(record, layout::RECORD_KEY);
    const auto length = readScalar<std::uint32_t>(record, layout::RECORD_LENGTH);
    corrupt(record.size() < layout::RECORD_HEADER ||
                length != record.size() - layout::RECORD_HEADER,
            "record length mismatch");
    return {key, std::string(reinterpret_cast<const char *>(record.data() + layout::RECORD_HEADER),
                             length)};
}
} // namespace rohandb
