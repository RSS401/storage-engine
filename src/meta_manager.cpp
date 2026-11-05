#include <rohandb/common/crc32c.hpp>
#include <rohandb/storage/meta_manager.hpp>
#include <rohandb/wal/log_manager.hpp>
namespace rohandb {
void DatabaseMetaManager::initialiseNew() {
    bytes_.fill(std::byte{0});
    writeScalar<std::uint8_t>(bytes_, layout::TYPE,
                              static_cast<std::uint8_t>(layout::PageType::Meta));
    writeScalar<std::uint64_t>(bytes_, layout::MAGIC, layout::MAGIC_VALUE);
    writeScalar<std::uint32_t>(bytes_, layout::VERSION, layout::VERSION_VALUE);
    writeScalar<std::uint32_t>(bytes_, layout::PAGE_BYTES, PAGE_SIZE);
    writeScalar<std::uint32_t>(bytes_, layout::FEATURES, FORMAT_FEATURES);
    writeScalar<PageId>(bytes_, layout::NEXT_PAGE, 2);
    writeScalar<PageId>(bytes_, layout::ROOT, INVALID_PAGE_ID);
    writeScalar<PageId>(bytes_, layout::FREE_HEAD, INVALID_PAGE_ID);
    writeId(bytes_, layout::DATABASE_ID, newDatabaseId());
    dirty_ = true;
}
void DatabaseMetaManager::publishInitial(ConstPageView root) {
    if (rootPageId() != 2 || nextPageId() != 3)
        throw DbError(ErrorCode::InvalidArgument,
                      "bootstrap requires one root and immutable identity");
    std::array<PageBytes, 3> initial;
    initial[0] = bytes_;
    initial[1] = makeIdentityPage(databaseId());
    std::copy(root.begin(), root.end(), initial[2].begin());
    stampChecksum(initial[0]);
    stampChecksum(initial[2]);
    disk_.publishInitial(initial);
    load();
}
void DatabaseMetaManager::load() {
    if (disk_.byteSize() % PAGE_SIZE != 0 || disk_.pageCountOnDisk() < 3)
        throw DbError(ErrorCode::CorruptPage,
                      "database must contain whole metadata and root pages");
    disk_.readPage(0, bytes_);
    verifyChecksum(bytes_);
    if (readScalar<std::uint8_t>(bytes_, layout::TYPE) !=
        static_cast<std::uint8_t>(layout::PageType::Meta))
        throw DbError(ErrorCode::CorruptPage, "invalid metadata page type");
    if (readScalar<std::uint64_t>(bytes_, layout::MAGIC) != layout::MAGIC_VALUE)
        throw DbError(ErrorCode::BadMagic, "invalid database magic");
    if (readScalar<std::uint32_t>(bytes_, layout::VERSION) != layout::VERSION_VALUE ||
        readScalar<std::uint32_t>(bytes_, layout::FEATURES) != FORMAT_FEATURES)
        throw DbError(ErrorCode::UnsupportedVersion, "unsupported database format");
    if (readScalar<std::uint32_t>(bytes_, layout::PAGE_BYTES) != PAGE_SIZE)
        throw DbError(ErrorCode::PageSizeMismatch, "unexpected page size");
    if (nextPageId() > disk_.pageCountOnDisk() || rootPageId() < 2 || rootPageId() >= nextPageId())
        throw DbError(ErrorCode::CorruptPage, "invalid allocation or root metadata");
    if (databaseId() != readDatabaseId(disk_))
        throw DbError(ErrorCode::CorruptPage, "metadata identity differs from immutable identity");
    dirty_ = false;
}
PageId DatabaseMetaManager::allocatePageId() {
    const auto id = nextPageId();
    if (id == INVALID_PAGE_ID)
        throw DbError(ErrorCode::InvalidArgument, "page IDs exhausted");
    writeScalar<PageId>(bytes_, layout::NEXT_PAGE, id + 1);
    dirty_ = true;
    return id;
}
void DatabaseMetaManager::setRootPageId(PageId id) {
    if (id < 2 || id >= nextPageId())
        throw DbError(ErrorCode::InvalidArgument, "unallocated root");
    writeScalar<PageId>(bytes_, layout::ROOT, id);
    dirty_ = true;
}
void DatabaseMetaManager::writeIfDirty() {
    if (!dirty_)
        return;
    if (log_)
        log_->flushAll();
    stampChecksum(bytes_);
    disk_.writePage(0, bytes_);
    dirty_ = false;
}
} // namespace rohandb
