#pragma once
#include <map>
#include <rohandb/buffer/buffer_pool_manager.hpp>
#include <rohandb/wal/log_manager.hpp>
namespace rohandb {
// Owns extra pins and before-images for one mutation. No modified page can be evicted.
class OperationContext {
  public:
    OperationContext(BufferPoolManager &pool, DatabaseMetaManager &meta, LogManager &log);
    ~OperationContext();
    void registerPage(FrameId frame, bool allocated = false);
    void commit(bool force);
    void rollback() noexcept;

  private:
    struct Before {
        PageBytes bytes;
        bool dirty;
        bool allocated;
        FrameId frame;
    };
    BufferPoolManager &pool_;
    DatabaseMetaManager &meta_;
    LogManager &log_;
    std::map<PageId, Before> before_;
    PageBytes meta_before_;
    bool meta_dirty_;
    bool finished_ = false;
};
} // namespace rohandb
