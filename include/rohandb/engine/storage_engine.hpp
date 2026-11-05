#pragma once
#include <rohandb/index/bplus_tree.hpp>
#include <rohandb/index/range_iterator.hpp>
#include <rohandb/wal/operation_context.hpp>
namespace rohandb {
enum class SyncMode { OnFlush, EveryOperation };
struct EngineOptions {
    std::size_t buffer_pool_frames = 64;
    std::uint64_t checkpoint_wal_bytes = 64 * 1024 * 1024;
    SyncMode sync_mode = SyncMode::OnFlush;
    std::uint16_t leaf_max_size = 0, internal_max_size = 0;
};
class StorageEngine {
  public:
    StorageEngine() = default;
    ~StorageEngine();
    StorageEngine(const StorageEngine &) = delete;
    StorageEngine &operator=(const StorageEngine &) = delete;
    void open(const std::filesystem::path &path, const EngineOptions &options = {});
    void open(std::unique_ptr<IDiskManager> disk, const EngineOptions &options = {});
    void open(std::unique_ptr<IDiskManager> disk, std::unique_ptr<IWalFile> wal,
              const EngineOptions &options = {});
    void close();
    void checkpoint();
    bool put(std::uint64_t key, std::string_view value);
    std::optional<std::string> get(std::uint64_t key);
    bool erase(std::uint64_t key);
    void flush();
    void verify();
    RangeIterator scan(std::uint64_t from_key);
    StorageStats stats() const;

  private:
    void requireOpen() const;
    void release() noexcept;
    std::unique_ptr<IDiskManager> disk_;
    std::unique_ptr<IWalFile> wal_;
    std::unique_ptr<LogManager> log_;
    std::unique_ptr<DatabaseMetaManager> meta_;
    std::unique_ptr<BufferPoolManager> pool_;
    std::unique_ptr<RecordManager> records_;
    std::unique_ptr<BPlusTree> tree_;
    bool failed_ = false;
    EngineOptions options_;
    std::uint64_t recovered_pages_ = 0;
    std::uint64_t checkpoint_bytes_ = 0;
};
} // namespace rohandb
