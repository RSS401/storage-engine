#include <cassert>
#include <rohandb/buffer/buffer_pool_manager.hpp>
#include <rohandb/common/crc32c.hpp>
#include <rohandb/wal/operation_context.hpp>
#include <utility>
namespace rohandb {
PageGuard::PageGuard(PageGuard &&other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), frame_(other.frame_) {}
PageGuard &PageGuard::operator=(PageGuard &&other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::exchange(other.pool_, nullptr);
        frame_ = other.frame_;
    }
    return *this;
}
PageGuard::~PageGuard() {
    release();
}
void PageGuard::release() noexcept {
    if (pool_) {
        pool_->unpin(frame_);
        pool_ = nullptr;
    }
}
PageId PageGuard::pageId() const {
    if (!pool_)
        throw DbError(ErrorCode::InvalidArgument, "empty page guard");
    pool_->requireHealthy();
    return pool_->frames_[frame_].page_id;
}
ConstPageView PageGuard::data() const {
    (void)pageId();
    return pool_->frames_[frame_].data;
}
PageView PageGuard::mutableData() {
    (void)pageId();
    auto &frame = pool_->frames_[frame_];
    if (pool_->log_ && !pool_->operation_)
        throw DbError(ErrorCode::InvalidArgument, "mutable access requires an operation");
    if (pool_->operation_)
        pool_->operation_->registerPage(frame_);
    frame.is_dirty = true;
    return frame.data;
}
BufferPoolManager::BufferPoolManager(std::size_t size, IDiskManager &disk,
                                     DatabaseMetaManager &meta)
    : disk_(disk), meta_(meta) {
    if (size == 0 || size > std::numeric_limits<FrameId>::max())
        throw DbError(ErrorCode::InvalidArgument, "invalid pool size");
    frames_.resize(size);
    for (std::size_t i = size; i != 0; --i)
        free_.push_back(static_cast<FrameId>(i - 1));
}
void BufferPoolManager::requireHealthy() const {
    if (log_)
        log_->requireHealthy();
}
void BufferPoolManager::flushFrame(FrameId id) {
    requireHealthy();
    auto &frame = frames_[id];
    if (!frame.is_dirty)
        return;
    const auto type = readScalar<std::uint8_t>(frame.data, layout::TYPE);
    if (type < 2 || type > 4)
        throw DbError(ErrorCode::CorruptPage, "uninitialised buffer page");
    // Unknown coverage falls back to forcing everything appended so far.
    if (log_)
        log_->flushTo(frame.commit_lsn != 0 ? frame.commit_lsn : log_->nextLsn() - 1);
    stampChecksum(frame.data);
    disk_.writePage(frame.page_id, frame.data);
    ++stats_.disk_writes;
    frame.is_dirty = false;
}
FrameId BufferPoolManager::acquire() {
    if (!free_.empty()) {
        const auto id = free_.back();
        free_.pop_back();
        return id;
    }
    const auto victim = replacer_.candidate();
    if (!victim)
        throw DbError(ErrorCode::AllFramesPinned, "all buffer frames are pinned");
    auto &frame = frames_[*victim];
    const bool dirty = frame.is_dirty;
    // Do not remove the candidate until its write succeeds: failures retain LRU position.
    flushFrame(*victim);
    replacer_.setEvictable(*victim, false);
    table_.erase(frame.page_id);
    frame = PageFrame{};
    ++stats_.evictions;
    if (dirty)
        ++stats_.dirty_evictions;
    return *victim;
}
PageGuard BufferPoolManager::fetchPage(PageId id) {
    requireHealthy();
    if (id < 2 || id >= meta_.nextPageId())
        throw DbError(ErrorCode::CorruptPage, "unallocated page reference");
    ++stats_.logical_page_fetches;
    if (const auto it = table_.find(id); it != table_.end()) {
        auto &frame = frames_[it->second];
        ++frame.pin_count;
        replacer_.setEvictable(it->second, false);
        ++stats_.buffer_hits;
        return PageGuard(this, it->second);
    }
    ++stats_.buffer_misses;
    const auto index = acquire();
    auto &frame = frames_[index];
    try {
        disk_.readPage(id, frame.data);
        ++stats_.disk_reads;
        try {
            verifyChecksum(frame.data);
        } catch (...) {
            ++stats_.checksum_failures;
            throw;
        }
        const auto type = readScalar<std::uint8_t>(frame.data, layout::TYPE);
        if (type < 2 || type > 4)
            throw DbError(ErrorCode::CorruptPage, "invalid buffered page type");
        if (const auto validator = validators_[type])
            validator(frame.data, meta_.nextPageId());
        frame.page_id = id;
        frame.pin_count = 1;
        table_.emplace(id, index);
    } catch (...) {
        frame = PageFrame{};
        free_.push_back(index);
        throw;
    }
    return PageGuard(this, index);
}
PageGuard BufferPoolManager::newPage() {
    requireHealthy();
    const auto id = meta_.allocatePageId();
    const auto index = acquire();
    auto &frame = frames_[index];
    frame = PageFrame{};
    frame.page_id = id;
    frame.pin_count = 1;
    frame.is_dirty = true;
    try {
        table_.emplace(id, index);
    } catch (...) {
        frame = PageFrame{};
        free_.push_back(index);
        throw;
    }
    if (operation_) {
        try {
            operation_->registerPage(index, true);
        } catch (...) {
            table_.erase(id);
            frame = PageFrame{};
            free_.push_back(index);
            throw;
        }
    }
    return PageGuard(this, index);
}
void BufferPoolManager::unpin(FrameId id) noexcept {
    auto &frame = frames_[id];
    assert(frame.pin_count != 0);
    --frame.pin_count;
    if (frame.pin_count == 0)
        replacer_.setEvictable(id, true);
}
bool BufferPoolManager::flushPage(PageId id) {
    const auto it = table_.find(id);
    if (it == table_.end())
        return false;
    flushFrame(it->second);
    return true;
}
void BufferPoolManager::flushAllPages() {
    for (FrameId i = 0; i < frames_.size(); ++i)
        if (frames_[i].page_id != INVALID_PAGE_ID)
            flushFrame(i);
}
std::size_t BufferPoolManager::pinCount() const {
    std::size_t count = 0;
    for (const auto &frame : frames_)
        count += frame.pin_count;
    return count;
}
} // namespace rohandb
