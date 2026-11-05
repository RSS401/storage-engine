#pragma once
#include <array>
#include <rohandb/buffer/lru_replacer.hpp>
#include <rohandb/common/stats.hpp>
#include <rohandb/storage/meta_manager.hpp>
#include <unordered_map>
#include <vector>
namespace rohandb {
class BufferPoolManager;
class LogManager;
class OperationContext;
// Move-only borrowed pin. Must be destroyed before its buffer pool.
class PageGuard {
  public:
    PageGuard() = default;
    PageGuard(PageGuard &&other) noexcept;
    PageGuard &operator=(PageGuard &&other) noexcept;
    PageGuard(const PageGuard &) = delete;
    PageGuard &operator=(const PageGuard &) = delete;
    ~PageGuard();
    PageId pageId() const;
    ConstPageView data() const;
    PageView mutableData();
    void release() noexcept;

  private:
    friend class BufferPoolManager;
    PageGuard(BufferPoolManager *pool, FrameId frame) : pool_(pool), frame_(frame) {}
    BufferPoolManager *pool_ = nullptr;
    FrameId frame_ = 0;
};
struct PageFrame {
    alignas(64) PageBytes data{};
    PageId page_id = INVALID_PAGE_ID;
    std::uint32_t pin_count = 0;
    bool is_dirty = false;
    Lsn commit_lsn = 0; // COMMIT covering the frame's bytes; it must be durable before writing.
};
// Structural check run once when a page is read from disk; cached frames are then trusted.
using PageValidator = void (*)(ConstPageView bytes, PageId next_page_id);
class BufferPoolManager {
  public:
    BufferPoolManager(std::size_t size, IDiskManager &disk, DatabaseMetaManager &meta);
    void requireHealthy() const;
    void setLog(LogManager *log) { log_ = log; }
    void setValidator(layout::PageType type, PageValidator validator) {
        validators_[static_cast<std::size_t>(type)] = validator;
    }
    PageGuard fetchPage(PageId id);
    PageGuard newPage();
    bool flushPage(PageId id);
    void flushAllPages();
    const StorageStats &stats() const { return stats_; }
    std::size_t pinCount() const;
    bool resident(PageId id) const { return table_.contains(id); }

  private:
    friend class PageGuard;
    friend class OperationContext;
    OperationContext *operation_ = nullptr;
    LogManager *log_ = nullptr;
    std::array<PageValidator, 5> validators_{};
    FrameId acquire();
    void flushFrame(FrameId id);
    void unpin(FrameId id) noexcept;
    std::vector<PageFrame> frames_;
    std::unordered_map<PageId, FrameId> table_;
    std::vector<FrameId> free_;
    LruReplacer replacer_;
    IDiskManager &disk_;
    DatabaseMetaManager &meta_;
    StorageStats stats_;
};
} // namespace rohandb
