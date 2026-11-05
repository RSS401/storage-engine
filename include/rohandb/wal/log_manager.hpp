#pragma once
#include <rohandb/common/crc32c.hpp>
#include <rohandb/disk_manager.hpp>
#include <rohandb/storage/database_identity.hpp>
#include <rohandb/wal/wal_file.hpp>
namespace rohandb {
namespace wal_layout {
inline constexpr std::size_t HEADER_SIZE = 44, MAGIC = 0, VERSION = 8, RESERVED = 12, BASE_LSN = 16,
                             DATABASE_ID = 24, HEADER_CRC = 40;
inline constexpr std::uint64_t MAGIC_VALUE = 0x314c415742444553ULL;
inline constexpr std::uint32_t VERSION_VALUE = 2;
inline constexpr std::size_t LENGTH = 0, LSN = 4, OP_ID = 12, TYPE = 20, PAGE_ID = 21, PAYLOAD = 25,
                             TRAILER = 4;
inline constexpr std::size_t COMMIT_SIZE = 29, IMAGE_SIZE = 4125;
} // namespace wal_layout
struct LogRecord {
    Lsn lsn;
    std::uint64_t op_id;
    PageId page_id;
    std::vector<std::byte> image;
};
struct WalScan {
    DatabaseId database_id{};
    Lsn base_lsn = 1;
    Lsn next_lsn = 1;
    std::vector<LogRecord> records;
    bool torn_tail = false;
};
WalScan scanWal(std::span<const std::byte> bytes);
std::vector<std::byte> walHeader(Lsn base, const DatabaseId &id);
class LogManager {
  public:
    explicit LogManager(IWalFile &file, const DatabaseId &id);
    Lsn nextLsn() const { return next_; }
    Lsn durableLsn() const { return durable_; }
    Lsn appendImage(std::uint64_t operation, PageId page, ConstPageView bytes);
    void appendCommit(std::uint64_t operation);
    void flushAll();
    void flushTo(Lsn lsn); // Forces everything buffered, but only if lsn is not yet durable.
    void reset();
    void requireHealthy() const;
    bool failed() const { return failed_; }
    std::uint64_t bytesWritten() const { return bytes_written_; }
    std::uint64_t recordsWritten() const { return records_written_; }
    std::uint64_t forces() const { return forces_; }

  private:
    void appendRecord(std::uint64_t operation, PageId page, std::span<const std::byte> image);
    IWalFile &file_;
    DatabaseId database_id_;
    Lsn next_ = 1, durable_ = 0;
    std::vector<std::byte> buffer_;
    bool failed_ = false;
    std::uint64_t bytes_written_ = 0, records_written_ = 0, forces_ = 0;
};
std::uint64_t recover(IDiskManager &disk, const WalScan &scan);
} // namespace rohandb
