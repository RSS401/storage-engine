#include <rohandb/wal/operation_context.hpp>
namespace rohandb {
OperationContext::OperationContext(BufferPoolManager &pool, DatabaseMetaManager &meta,
                                   LogManager &log)
    : pool_(pool), meta_(meta), log_(log), meta_before_(meta.bytes_), meta_dirty_(meta.dirty_) {
    if (pool_.operation_)
        throw DbError(ErrorCode::InvalidArgument, "nested operation");
    pool_.operation_ = this;
}
OperationContext::~OperationContext() {
    if (!finished_)
        rollback();
}
void OperationContext::registerPage(FrameId index, bool allocated) {
    auto &frame = pool_.frames_[index];
    if (before_.contains(frame.page_id))
        return;
    before_.emplace(frame.page_id, Before{frame.data, frame.is_dirty, allocated, index});
    ++frame.pin_count;
    pool_.replacer_.setEvictable(index, false);
}
void OperationContext::commit(bool force) {
    const auto operation = log_.nextLsn();
    // Errors from here are fatal to the engine: the log may contain part of this commit.
    for (auto &[id, before] : before_) {
        auto &frame = pool_.frames_[before.frame];
        writeScalar<Lsn>(frame.data, layout::LSN, log_.nextLsn());
        stampChecksum(frame.data);
        log_.appendImage(operation, id, frame.data);
    }
    if (meta_.bytes_ != meta_before_) {
        writeScalar<Lsn>(meta_.bytes_, layout::LSN, log_.nextLsn());
        stampChecksum(meta_.bytes_);
        log_.appendImage(operation, 0, meta_.bytes_);
    }
    const auto commit_lsn = log_.nextLsn();
    log_.appendCommit(operation);
    for (const auto &[id, before] : before_) {
        (void)id;
        pool_.frames_[before.frame].commit_lsn = commit_lsn;
    }
    if (force)
        log_.flushAll();
    pool_.operation_ = nullptr;
    for (const auto &[id, before] : before_) {
        (void)id;
        pool_.unpin(before.frame);
    }
    ++pool_.stats_.ops_committed;
    finished_ = true;
}
void OperationContext::rollback() noexcept {
    pool_.operation_ = nullptr;
    for (const auto &[id, before] : before_) {
        auto &frame = pool_.frames_[before.frame];
        if (before.allocated) {
            pool_.replacer_.setEvictable(before.frame, false);
            pool_.table_.erase(id);
            frame = PageFrame{};
            pool_.free_.push_back(before.frame);
        } else {
            frame.data = before.bytes;
            frame.is_dirty = before.dirty;
            pool_.unpin(before.frame);
        }
    }
    meta_.bytes_ = meta_before_;
    meta_.dirty_ = meta_dirty_;
    ++pool_.stats_.ops_rolled_back;
    finished_ = true;
}
} // namespace rohandb
