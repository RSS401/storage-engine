#include <iostream>
#include <rohandb/engine/storage_engine.hpp>
namespace rohandb {
StorageEngine::~StorageEngine() {
    try {
        close();
    } catch (const std::exception &e) {
        std::cerr << "RohanDB close failed: " << e.what() << '\n';
    }
}
void StorageEngine::requireOpen() const {
    if (failed_ || (log_ && log_->failed()))
        throw DbError(ErrorCode::EngineFailed, "engine failed; close without flushing");
    if (!tree_)
        throw DbError(ErrorCode::InvalidArgument, "engine is not open");
}
void StorageEngine::open(const std::filesystem::path &path, const EngineOptions &options) {
    if (disk_)
        throw DbError(ErrorCode::InvalidArgument, "engine already open");
#ifdef ROHANDB_WAL
    const auto wal_path = path.string() + ".wal";
    if ((!std::filesystem::exists(path) || std::filesystem::file_size(path) == 0) &&
        std::filesystem::exists(wal_path) && std::filesystem::file_size(wal_path) != 0)
        throw DbError(ErrorCode::WalCorrupt,
                      "refusing an orphan WAL beside a missing or empty database");
    open(DiskManager::open(path, true), std::make_unique<WalFile>(path.string() + ".wal"), options);
#else
    open(DiskManager::open(path, true), options);
#endif
}
void StorageEngine::open(std::unique_ptr<IDiskManager> disk, const EngineOptions &options) {
    open(std::move(disk), nullptr, options);
}
void StorageEngine::open(std::unique_ptr<IDiskManager> disk, std::unique_ptr<IWalFile> wal,
                         const EngineOptions &options) {
    if (disk_)
        throw DbError(ErrorCode::InvalidArgument, "engine already open");
    if (!disk || options.buffer_pool_frames == 0 || options.checkpoint_wal_bytes == 0 ||
        (!wal && options.sync_mode != SyncMode::OnFlush))
        throw DbError(ErrorCode::InvalidArgument,
                      "invalid options or missing WAL for durable operation mode");
#ifdef ROHANDB_WAL
    if (!wal)
        throw DbError(ErrorCode::InvalidArgument, "this build requires a WAL interface");
#endif
    disk_ = std::move(disk);
    wal_ = std::move(wal);
    options_ = options;
    failed_ = false;
    checkpoint_bytes_ = 0;
    recovered_pages_ = 0;
    try {
        WalScan scan;
        if (wal_) {
            const auto wal_bytes = wal_->readAll();
            if (disk_->byteSize() == 0 && !wal_bytes.empty())
                throw DbError(ErrorCode::WalCorrupt, "refusing orphan WAL for an empty database");
            scan = scanWal(wal_bytes);
            if (disk_->byteSize() != 0)
                recovered_pages_ = recover(*disk_, scan);
        }
        meta_ = std::make_unique<DatabaseMetaManager>(*disk_);
        pool_ = std::make_unique<BufferPoolManager>(options.buffer_pool_frames, *disk_, *meta_);
        records_ = std::make_unique<RecordManager>(*pool_);
        tree_ = std::make_unique<BPlusTree>(*pool_, *meta_, options.leaf_max_size,
                                            options.internal_max_size);
        if (disk_->byteSize() == 0) {
            meta_->initialiseNew();
            tree_->createEmptyRoot();
            auto root = pool_->fetchPage(meta_->rootPageId());
            meta_->publishInitial(root.data());
        } else {
            meta_->load();
            (void)tree_->readNode(meta_->rootPageId());
        }
        if (wal_) {
            wal_->reset(walHeader(scan.next_lsn, meta_->databaseId()));
            log_ = std::make_unique<LogManager>(*wal_, meta_->databaseId());
            pool_->setLog(log_.get());
            meta_->setLog(log_.get());
        }
    } catch (...) {
        release();
        throw;
    }
}
void StorageEngine::release() noexcept {
    tree_.reset();
    records_.reset();
    pool_.reset();
    meta_.reset();
    log_.reset();
    wal_.reset();
    disk_.reset();
    failed_ = false;
}
void StorageEngine::close() {
    if (!disk_)
        return;
    try {
        if (!failed_ && !(log_ && log_->failed())) {
            if (log_)
                checkpoint();
            else
                flush();
        }
    } catch (...) {
        release();
        throw;
    }
    release();
}
void StorageEngine::flush() {
    requireOpen();
    try {
        if (log_) {
            log_->flushAll();
            return;
        }
        pool_->flushAllPages();
        disk_->sync();
        meta_->writeIfDirty();
        disk_->sync();
    } catch (...) {
        failed_ = true;
        throw;
    }
}
void StorageEngine::checkpoint() {
    requireOpen();
    if (!log_) {
        flush();
        return;
    }
    try {
        log_->flushAll();
        pool_->flushAllPages();
        disk_->sync();
        // Never create an unlogged metadata change solely for an informational LSN.
        // Dirty metadata is covered by a committed PAGE_IMAGE.
        meta_->writeIfDirty();
        disk_->sync();
        log_->reset();
        checkpoint_bytes_ = log_->bytesWritten();
    } catch (...) {
        failed_ = true;
        throw;
    }
}
bool StorageEngine::put(std::uint64_t key, std::string_view value) {
    requireOpen();
    const auto encoded = encodeRecord(key, value);
    tree_->invalidateIterators();
    const auto hint = records_->insertionHint();
    std::unique_ptr<OperationContext> operation;
    if (log_)
        operation = std::make_unique<OperationContext>(*pool_, *meta_, *log_);
    bool committing = false;
    try {
        const auto rid = records_->insert(encoded);
        const auto old = tree_->upsert(key, rid);
        if (old)
            records_->erase(*old);
        if (operation) {
            committing = true;
            operation->commit(options_.sync_mode == SyncMode::EveryOperation);
        }
        if (log_ && log_->bytesWritten() - checkpoint_bytes_ >= options_.checkpoint_wal_bytes)
            checkpoint();
        return !old.has_value();
    } catch (...) {
        if (operation && !committing && !log_->failed()) {
            operation->rollback();
            records_->restoreHint(hint);
        } else
            failed_ = true;
        throw;
    }
}
std::optional<std::string> StorageEngine::get(std::uint64_t key) {
    requireOpen();
    const auto rid = tree_->find(key);
    if (!rid)
        return std::nullopt;
    auto [stored, value] = decodeRecord(records_->read(*rid));
    if (stored != key)
        throw DbError(ErrorCode::CorruptPage, "index and record keys disagree");
    return value;
}
bool StorageEngine::erase(std::uint64_t key) {
    requireOpen();
    const auto rid = tree_->find(key);
    if (!rid)
        return false;
    tree_->invalidateIterators();
    std::unique_ptr<OperationContext> operation;
    if (log_)
        operation = std::make_unique<OperationContext>(*pool_, *meta_, *log_);
    bool committing = false;
    try {
        tree_->erase(key);
        records_->erase(*rid);
        if (operation) {
            committing = true;
            operation->commit(options_.sync_mode == SyncMode::EveryOperation);
        }
        if (log_ && log_->bytesWritten() - checkpoint_bytes_ >= options_.checkpoint_wal_bytes)
            checkpoint();
        return true;
    } catch (...) {
        if (operation && !committing && !log_->failed())
            operation->rollback();
        else
            failed_ = true;
        throw;
    }
}
RangeIterator StorageEngine::scan(std::uint64_t from_key) {
    requireOpen();
    auto iterator = tree_->lowerBound(from_key);
    iterator.records_ = records_.get();
    return iterator;
}
void StorageEngine::verify() {
    requireOpen();
    tree_->verify(*records_);
}
StorageStats StorageEngine::stats() const {
    requireOpen();
    auto result = pool_->stats();
    result.recovery_pages_redone = recovered_pages_;
    if (log_) {
        result.wal_records = log_->recordsWritten();
        result.wal_bytes = log_->bytesWritten();
        result.log_forces = log_->forces();
    }
    return result;
}
} // namespace rohandb
