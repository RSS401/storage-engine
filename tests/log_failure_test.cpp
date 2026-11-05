#include "../testing/simulated_storage.hpp"
#include "test_support.hpp"
#include <iostream>
#include <rohandb/engine/storage_engine.hpp>
using namespace rohandb;
using namespace rohandb::testing;
void directFailure(SyncFailureMode mode) {
    auto state = std::make_shared<SimulatedStorage>();
    state->sync_failure_mode = mode;
    FaultInjectingWalFile file(state);
    LogManager log(file, newDatabaseId());
    PageBytes bytes{};
    log.appendImage(1, 1, bytes);
    log.appendCommit(1);
    state->failNthSync(1);
    expectError(ErrorCode::IoError, [&] { log.flushAll(); });
    const auto events = state->ioEventCount();
    expectError(ErrorCode::EngineFailed, [&] { log.flushAll(); });
    expectError(ErrorCode::EngineFailed, [&] { log.reset(); });
    expectError(ErrorCode::EngineFailed, [&] { log.appendImage(2, 1, bytes); });
    expectError(ErrorCode::EngineFailed, [&] { log.appendCommit(2); });
    check(state->ioEventCount() == events);
}
void evictionFailure(SyncFailureMode mode, bool iterator) {
    auto state = std::make_shared<SimulatedStorage>();
    state->sync_failure_mode = mode;
    StorageEngine engine;
    EngineOptions options;
    options.buffer_pool_frames = 4;
    engine.open(std::make_unique<FaultInjectingDiskManager>(state),
                std::make_unique<FaultInjectingWalFile>(state), options);
    // More record pages than frames. The last put's pages stay dirty with a buffered COMMIT, so
    // reading other records eventually evicts one and must force the WAL.
    for (std::uint64_t i = 0; i < 12; ++i)
        engine.put(i, std::string(3000, 'x'));
    auto it = engine.scan(0); // Pins the resident root; reading values needs data-page fetches.
    state->failNthSync(1);
    expectError(ErrorCode::IoError, [&] {
        for (std::uint64_t key = 0; key < 11; ++key) {
            if (iterator) {
                (void)it.value();
                it.next();
            } else
                (void)engine.get(key);
        }
    });
    const auto events = state->ioEventCount();
    expectError(ErrorCode::EngineFailed, [&] { engine.flush(); });
    expectError(ErrorCode::EngineFailed, [&] { engine.put(9999, "must not be acknowledged"); });
    expectError(ErrorCode::EngineFailed, [&] { (void)engine.get(11); });
    expectError(ErrorCode::EngineFailed, [&] { (void)it.value(); });
    it = RangeIterator{};
    engine.close();
    check(state->ioEventCount() == events);
}
void iteratorNextFailure(SyncFailureMode mode) {
    auto state = std::make_shared<SimulatedStorage>();
    state->sync_failure_mode = mode;
    FaultInjectingDiskManager disk(state);
    DatabaseMetaManager meta(disk);
    meta.initialiseNew();
    for (unsigned i = 0; i < 4; ++i)
        (void)meta.allocatePageId();
    meta.setRootPageId(2);
    PageBytes root{};
    writeScalar<std::uint8_t>(root, layout::TYPE, 4);
    writeScalar<std::uint16_t>(root, layout::NODE_SIZE, 2);
    writeScalar<std::uint16_t>(root, layout::NODE_MAX, 4);
    writeScalar<PageId>(root, layout::FIRST_CHILD, 3);
    writeScalar<std::uint64_t>(root, layout::FIRST_SEPARATOR, 2);
    writeScalar<PageId>(root, layout::FIRST_SEPARATOR + layout::CHILD_IN_ENTRY, 4);
    stampChecksum(root);
    disk.writePage(2, root);
    for (PageId id : {3U, 4U}) {
        PageBytes leaf{};
        writeScalar<std::uint8_t>(leaf, layout::TYPE, 3);
        writeScalar<std::uint16_t>(leaf, layout::NODE_SIZE, 1);
        writeScalar<std::uint16_t>(leaf, layout::NODE_MAX, 4);
        writeScalar<PageId>(leaf, layout::NEXT_LEAF, id == 3 ? 4 : INVALID_PAGE_ID);
        writeScalar<std::uint64_t>(leaf, layout::ENTRIES, id == 3 ? 1 : 2);
        writeScalar<PageId>(leaf, layout::ENTRIES + layout::LEAF_RID_PAGE, 5);
        stampChecksum(leaf);
        disk.writePage(id, leaf);
    }
    disk.sync();
    FaultInjectingWalFile file(state);
    LogManager log(file, meta.databaseId());
    BufferPoolManager pool(2, disk, meta);
    pool.setLog(&log);
    BPlusTree tree(pool, meta);
    auto it = tree.lowerBound(1);
    // The iterator pins the left leaf. Root is the only possible dirty victim.
    {
        OperationContext operation(pool, meta, log);
        {
            auto guard = pool.fetchPage(2);
            (void)guard.mutableData();
        }
        operation.commit(false);
    }
    state->failNthSync(1);
    expectError(ErrorCode::IoError, [&] { it.next(); });
    const auto events = state->ioEventCount();
    expectError(ErrorCode::EngineFailed, [&] { it.next(); });
    expectError(ErrorCode::EngineFailed, [&] { (void)it.valid(); });
    check(state->ioEventCount() == events);
}
// A dirty eviction forces the WAL only when the frame's COMMIT is not yet durable.
void evictionForcesOnlyUncoveredCommits() {
    auto state = std::make_shared<SimulatedStorage>();
    FaultInjectingDiskManager disk(state);
    DatabaseMetaManager meta(disk);
    meta.initialiseNew();
    for (unsigned i = 0; i < 3; ++i)
        (void)meta.allocatePageId();
    for (PageId id : {2U, 3U, 4U}) {
        PageBytes leaf{};
        writeScalar<std::uint8_t>(leaf, layout::TYPE, 3);
        writeScalar<std::uint16_t>(leaf, layout::NODE_MAX, 4);
        stampChecksum(leaf);
        disk.writePage(id, leaf);
    }
    disk.sync();
    FaultInjectingWalFile file(state);
    LogManager log(file, meta.databaseId());
    BufferPoolManager pool(2, disk, meta);
    pool.setLog(&log);
    const auto modify = [&](PageId id, bool force) {
        OperationContext operation(pool, meta, log);
        {
            auto guard = pool.fetchPage(id);
            writeScalar<std::uint16_t>(guard.mutableData(), layout::NODE_MAX, 5);
        }
        operation.commit(force);
    };
    modify(2, true);
    modify(3, false);
    check(log.forces() == 1);
    // Evicts page 2: its COMMIT is durable, so the buffered records of page 3 stay buffered.
    (void)pool.fetchPage(4);
    check(log.forces() == 1 && pool.stats().disk_writes == 1);
    // Evicts page 3, whose COMMIT is only buffered.
    (void)pool.fetchPage(2);
    check(log.forces() == 2 && log.durableLsn() == log.nextLsn() - 1);
    check(pool.stats().disk_writes == 2);
}
void checkpointProtection() {
    auto state = std::make_shared<SimulatedStorage>();
    StorageEngine engine;
    engine.open(std::make_unique<FaultInjectingDiskManager>(state),
                std::make_unique<FaultInjectingWalFile>(state));
    engine.put(1, "first");
    engine.checkpoint();
    const auto metadata =
        std::vector<std::byte>(state->database.begin(), state->database.begin() + PAGE_SIZE);
    state->events.clear();
    engine.put(1, "replacement");
    engine.checkpoint();
    engine.checkpoint();
    for (const auto &event : state->trace())
        check(event.wal || event.operation != "write" || event.offset != 0);
    check(std::equal(metadata.begin(), metadata.end(), state->database.begin()));
    engine.close();
}
int main() {
    try {
        checkpointProtection();
        evictionForcesOnlyUncoveredCommits();
        for (auto mode : {SyncFailureMode::RetainPending, SyncFailureMode::DiscardPending}) {
            directFailure(mode);
            evictionFailure(mode, false);
            evictionFailure(mode, true);
            iteratorNextFailure(mode);
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
