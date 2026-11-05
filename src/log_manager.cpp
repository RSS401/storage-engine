#include <rohandb/wal/log_manager.hpp>
#include <set>
namespace rohandb {
std::vector<std::byte> walHeader(Lsn base, const DatabaseId &id) {
    std::vector<std::byte> bytes(wal_layout::HEADER_SIZE);
    writeScalar<std::uint64_t>(bytes, wal_layout::MAGIC, wal_layout::MAGIC_VALUE);
    writeScalar<std::uint32_t>(bytes, wal_layout::VERSION, wal_layout::VERSION_VALUE);
    writeScalar<Lsn>(bytes, wal_layout::BASE_LSN, base);
    writeId(bytes, wal_layout::DATABASE_ID, id);
    writeScalar<std::uint32_t>(bytes, wal_layout::HEADER_CRC,
                               crc32c(std::span(bytes).first(wal_layout::HEADER_CRC)));
    return bytes;
}
WalScan scanWal(std::span<const std::byte> bytes) {
    WalScan scan;
    if (bytes.empty())
        return scan;
    if (bytes.size() < wal_layout::HEADER_SIZE ||
        readScalar<std::uint64_t>(bytes, wal_layout::MAGIC) != wal_layout::MAGIC_VALUE ||
        readScalar<std::uint32_t>(bytes, wal_layout::VERSION) != wal_layout::VERSION_VALUE ||
        readScalar<std::uint32_t>(bytes, wal_layout::HEADER_CRC) !=
            crc32c(bytes.first(wal_layout::HEADER_CRC)))
        throw DbError(ErrorCode::WalCorrupt, "invalid WAL header");
    scan.database_id = readId(bytes, wal_layout::DATABASE_ID);
    if (scan.database_id == DatabaseId{})
        throw DbError(ErrorCode::WalCorrupt, "empty WAL database ID");
    scan.base_lsn = scan.next_lsn = readScalar<Lsn>(bytes, wal_layout::BASE_LSN);
    if (scan.base_lsn == 0)
        throw DbError(ErrorCode::WalCorrupt, "invalid WAL base LSN");
    std::size_t offset = wal_layout::HEADER_SIZE;
    while (offset < bytes.size()) {
        auto remaining = bytes.subspan(offset);
        if (remaining.size() < wal_layout::COMMIT_SIZE) {
            scan.torn_tail = true;
            break;
        }
        const auto length = readScalar<std::uint32_t>(remaining, wal_layout::LENGTH);
        const auto type = readScalar<std::uint8_t>(remaining, wal_layout::TYPE);
        if ((type != 1 && type != 2) ||
            length != (type == 1 ? wal_layout::IMAGE_SIZE : wal_layout::COMMIT_SIZE) ||
            length > remaining.size()) {
            scan.torn_tail = true;
            break;
        }
        auto record = remaining.first(length);
        if (readScalar<std::uint32_t>(record, length - 4) != crc32c(record.first(length - 4)) ||
            readScalar<Lsn>(record, wal_layout::LSN) != scan.next_lsn) {
            scan.torn_tail = true;
            break;
        }
        const auto page = readScalar<PageId>(record, wal_layout::PAGE_ID);
        if ((type == 1 && page == INVALID_PAGE_ID) || (type == 2 && page != INVALID_PAGE_ID)) {
            scan.torn_tail = true;
            break;
        }
        LogRecord decoded{
            scan.next_lsn, readScalar<std::uint64_t>(record, wal_layout::OP_ID), page, {}};
        if (type == 1)
            decoded.image.assign(record.begin() + wal_layout::PAYLOAD,
                                 record.begin() + wal_layout::PAYLOAD + PAGE_SIZE);
        scan.records.push_back(std::move(decoded));
        ++scan.next_lsn;
        offset += length;
    }
    return scan;
}
LogManager::LogManager(IWalFile &file, const DatabaseId &id) : file_(file), database_id_(id) {
    const auto bytes = file_.readAll();
    if (bytes.empty())
        file_.reset(walHeader(1, database_id_));
    const auto scan = scanWal(bytes);
    if (!bytes.empty() && scan.database_id != database_id_)
        throw DbError(ErrorCode::WalCorrupt, "WAL belongs to a different database");
    if (scan.torn_tail)
        throw DbError(ErrorCode::WalCorrupt, "WAL must be recovered before append");
    next_ = scan.next_lsn;
    durable_ = next_ - 1;
}
void LogManager::requireHealthy() const {
    if (failed_)
        throw DbError(ErrorCode::EngineFailed, "WAL failed; reopen required");
}
void LogManager::appendRecord(std::uint64_t operation, PageId page,
                              std::span<const std::byte> image) {
    requireHealthy();
    if (next_ == std::numeric_limits<Lsn>::max())
        throw DbError(ErrorCode::InvalidArgument, "LSN exhausted");
    const auto length = wal_layout::COMMIT_SIZE + image.size();
    // Encode in place at the end of the pending buffer; a throw before the end leaves it intact.
    const auto start = buffer_.size();
    buffer_.resize(start + length);
    try {
        const auto bytes = std::span(buffer_).subspan(start);
        writeScalar<std::uint32_t>(bytes, wal_layout::LENGTH, static_cast<std::uint32_t>(length));
        writeScalar<Lsn>(bytes, wal_layout::LSN, next_);
        writeScalar<std::uint64_t>(bytes, wal_layout::OP_ID, operation);
        writeScalar<std::uint8_t>(bytes, wal_layout::TYPE, image.empty() ? 2 : 1);
        writeScalar<PageId>(bytes, wal_layout::PAGE_ID, page);
        std::copy(image.begin(), image.end(), bytes.begin() + wal_layout::PAYLOAD);
        writeScalar<std::uint32_t>(bytes, length - 4, crc32c(bytes.first(length - 4)));
    } catch (...) {
        buffer_.resize(start);
        throw;
    }
    ++next_;
    ++records_written_;
    bytes_written_ += length;
}
Lsn LogManager::appendImage(std::uint64_t operation, PageId page, ConstPageView bytes) {
    const auto lsn = next_;
    appendRecord(operation, page, bytes);
    return lsn;
}
void LogManager::appendCommit(std::uint64_t operation) {
    appendRecord(operation, INVALID_PAGE_ID, {});
}
void LogManager::flushAll() {
    requireHealthy();
    if (buffer_.empty())
        return;
    try {
        file_.append(buffer_);
        file_.sync();
        durable_ = next_ - 1;
        buffer_.clear();
        ++forces_;
    } catch (...) {
        failed_ = true;
        throw;
    }
}
void LogManager::flushTo(Lsn lsn) {
    requireHealthy();
    if (lsn > durable_)
        flushAll();
}
void LogManager::reset() {
    requireHealthy();
    try {
        file_.reset(walHeader(next_, database_id_));
        buffer_.clear();
        durable_ = next_ - 1;
    } catch (...) {
        failed_ = true;
        throw;
    }
}
std::uint64_t recover(IDiskManager &disk, const WalScan &scan) {
    const auto id = readDatabaseId(disk);
    if (scan.database_id != DatabaseId{} && scan.database_id != id)
        throw DbError(ErrorCode::WalCorrupt, "WAL belongs to a different database");
    // Validate the complete replay set before writing even the first page.
    for (const auto &record : scan.records)
        if (!record.image.empty()) {
            if (record.page_id == IDENTITY_PAGE_ID)
                throw DbError(ErrorCode::WalCorrupt, "WAL must never modify immutable identity");
            if (record.page_id == 0 && readId(record.image, layout::DATABASE_ID) != id)
                throw DbError(ErrorCode::WalCorrupt, "metadata image identity mismatch");
        }
    std::set<std::uint64_t> committed;
    for (const auto &record : scan.records)
        if (record.image.empty())
            committed.insert(record.op_id);
    std::uint64_t count = 0;
    for (const auto &record : scan.records)
        if (!record.image.empty() && committed.contains(record.op_id)) {
            disk.writePage(record.page_id, ConstPageView(record.image.data(), PAGE_SIZE));
            ++count;
        }
    if (count)
        disk.sync();
    // Keep the old WAL until metadata validation has succeeded in StorageEngine::open.
    return count;
}
} // namespace rohandb
