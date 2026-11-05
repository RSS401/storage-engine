#pragma once
#include <cstdint>
namespace rohandb {
struct StorageStats {
    std::uint64_t logical_page_fetches = 0, buffer_hits = 0, buffer_misses = 0;
    std::uint64_t disk_reads = 0, disk_writes = 0, evictions = 0, dirty_evictions = 0;
    std::uint64_t checksum_failures = 0, wal_records = 0, wal_bytes = 0, log_forces = 0;
    std::uint64_t ops_committed = 0, ops_rolled_back = 0, recovery_pages_redone = 0;
};
} // namespace rohandb
