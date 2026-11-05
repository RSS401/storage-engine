#include "../testing/simulated_storage.hpp"
#include "test_support.hpp"
#include <iostream>
#include <map>
#include <rohandb/engine/storage_engine.hpp>
using namespace rohandb;
using namespace rohandb::testing;
using rohandb::testing::check;
void open(StorageEngine &engine, const std::shared_ptr<SimulatedStorage> &state,
          EngineOptions options = {}) {
    engine.open(std::make_unique<FaultInjectingDiskManager>(state),
                std::make_unique<FaultInjectingWalFile>(state), options);
}
void walTest() {
    auto state = std::make_shared<SimulatedStorage>();
    FaultInjectingWalFile file(state);
    LogManager log(file, newDatabaseId());
    PageBytes bytes{};
    writeScalar<std::uint8_t>(bytes, layout::TYPE, 2);
    stampChecksum(bytes);
    log.appendImage(1, 1, bytes);
    log.appendCommit(1);
    log.flushAll();
    const auto complete = file.readAll();
    check(scanWal(complete).records.size() == 2);
    // Every byte boundary of both record types, including a complete image with no COMMIT.
    for (std::size_t cut = wal_layout::HEADER_SIZE; cut < complete.size(); ++cut) {
        const auto scan = scanWal(std::span(complete).first(cut));
        check(scan.records.size() <= 1);
    }
    auto corrupt = complete;
    corrupt[wal_layout::HEADER_SIZE + 30] ^= std::byte{1};
    check(scanWal(corrupt).records.empty());
}
void durableTest(SyncMode mode) {
    auto state = std::make_shared<SimulatedStorage>();
    EngineOptions options;
    options.sync_mode = mode;
    {
        StorageEngine engine;
        open(engine, state, options);
        engine.put(42, "durable");
        if (mode == SyncMode::OnFlush)
            engine.flush();
        state->crashNow(CrashMode::DropAllUnsynced, 1);
    }
    state->reopen();
    {
        StorageEngine engine;
        open(engine, state, options);
        check(engine.get(42) == std::optional<std::string>("durable"));
        engine.verify();
    }
}
void rollbackTest() {
    auto state = std::make_shared<SimulatedStorage>();
    EngineOptions options;
    options.buffer_pool_frames = 2;
    options.leaf_max_size = 4;
    StorageEngine engine;
    open(engine, state, options);
    for (std::uint64_t i = 0; i < 4; ++i)
        engine.put(i, "x");
    bool failed = false;
    try {
        engine.put(4, "split needs more frames");
    } catch (const DbError &e) {
        check(e.code() == ErrorCode::AllFramesPinned);
        failed = true;
    }
    check(failed);
    check(!engine.get(4));
    engine.verify();
    check(engine.get(3) == std::optional<std::string>("x"));
    check(engine.stats().ops_rolled_back == 1);
}
void crashSweep() {
    using Model = std::map<std::uint64_t, std::string>;
    std::vector<std::pair<std::uint64_t, std::optional<std::string>>> operations;
    for (std::uint64_t i = 0; i < 24; ++i)
        operations.emplace_back(i, std::string(600, static_cast<char>('a' + i)));
    operations.emplace_back(2, std::string(900, 'z'));
    operations.emplace_back(0, std::nullopt);
    operations.emplace_back(7, std::nullopt);
    operations.emplace_back(7, std::string("reinserted"));
    std::vector<Model> models(1);
    for (const auto &[key, value] : operations) {
        auto model = models.back();
        if (value)
            model[key] = *value;
        else
            model.erase(key);
        models.push_back(std::move(model));
    }
    const auto apply = [&](StorageEngine &engine, std::size_t i) {
        const auto &[key, value] = operations[i];
        if (value)
            engine.put(key, *value);
        else
            engine.erase(key);
        if (i == 7)
            engine.flush();
    };
    for (const auto sync_mode : {SyncMode::OnFlush, SyncMode::EveryOperation}) {
        EngineOptions options;
        options.leaf_max_size = 4;
        options.internal_max_size = 4;
        options.buffer_pool_frames = 16;
        options.checkpoint_wal_bytes = 64 * 1024;
        options.sync_mode = sync_mode;
        auto baseline = std::make_shared<SimulatedStorage>();
        {
            StorageEngine engine;
            open(engine, baseline, options);
            engine.checkpoint();
        }
        baseline->events.clear();
        baseline->event_count = 0;
        auto probe = std::make_shared<SimulatedStorage>(*baseline);
        std::uint64_t total = 0;
        {
            StorageEngine engine;
            open(engine, probe, options);
            for (std::size_t i = 0; i < operations.size(); ++i)
                apply(engine, i);
            engine.checkpoint();
            total = probe->event_count;
        }
        std::cout << "Crash sweep: " << total
                  << " events x 3 modes x 3 seeds (including byte tears), sync="
                  << static_cast<int>(sync_mode) << "\n";
        for (const auto seed : {17ULL, 421ULL, 999ULL})
            for (const auto mode : {CrashMode::DropAllUnsynced, CrashMode::KeepRandomSubset,
                                    CrashMode::TearOneWrite}) {
                for (std::uint64_t event = 1; event <= total; ++event) {
                    auto state = std::make_shared<SimulatedStorage>(*baseline);
                    std::size_t durable = 0, attempted = 0;
                    {
                        StorageEngine engine;
                        state->tear_alignment = seed == 421 ? 1 : 512;
                        state->crashAtEvent(event, mode, event * seed);
                        try {
                            open(engine, state, options);
                            for (std::size_t i = 0; i < operations.size(); ++i) {
                                attempted = i + 1;
                                apply(engine, i);
                                if (sync_mode == SyncMode::EveryOperation || i == 7)
                                    durable = i + 1;
                            }
                            engine.checkpoint();
                            durable = operations.size();
                        } catch (const DbError &e) {
                            check(e.code() == ErrorCode::IoError);
                        }
                        if (!state->crashed)
                            state->crashNow(mode, event * seed);
                    }
                    state->reopen();
                    {
                        StorageEngine engine;
                        open(engine, state, options);
                        engine.verify();
                        Model actual;
                        {
                            for (auto it = engine.scan(0); it.valid(); it.next())
                                actual[it.key()] = it.value();
                        }
                        bool matched = false;
                        for (std::size_t i = durable; i <= attempted; ++i)
                            if (actual == models[i])
                                matched = true;
                        if (!matched)
                            throw std::runtime_error("non-prefix state at event " +
                                                     std::to_string(event));
                    }
                }
            }
    }
}
void recoveryInterruptedAndTornMeta() {
    auto state = std::make_shared<SimulatedStorage>();
    EngineOptions options;
    options.sync_mode = SyncMode::EveryOperation;
    options.leaf_max_size = 4;
    {
        StorageEngine engine;
        open(engine, state, options);
        for (std::uint64_t key = 0; key < 8; ++key)
            engine.put(key, "recovery");
        state->crashNow(CrashMode::DropAllUnsynced, 7);
    }
    state->reopen();
    // Page zero was logged by allocation/root splits and can be repaired blindly.
    state->flipBit(0, 80);
    state->flipBit(0, layout::DATABASE_ID * 8); // The damaged metadata ID is not trusted.
    state->crashAtEvent(2, CrashMode::TearOneWrite, 11);
    try {
        StorageEngine engine;
        open(engine, state, options);
        throw std::runtime_error("expected recovery crash");
    } catch (const DbError &e) {
        check(e.code() == ErrorCode::IoError);
    }
    state->reopen();
    {
        StorageEngine engine;
        open(engine, state, options);
        engine.verify();
        for (std::uint64_t key = 0; key < 8; ++key)
            check(engine.get(key) == std::optional<std::string>("recovery"));
    }
}
void beforeCommitAndSyncFailure() {
    auto state = std::make_shared<SimulatedStorage>();
    {
        StorageEngine engine;
        open(engine, state);
        engine.put(1, "not forced");
        state->crashNow(CrashMode::DropAllUnsynced, 9);
    }
    state->reopen();
    {
        StorageEngine engine;
        open(engine, state);
        check(!engine.get(1));
    }
    EngineOptions options;
    options.sync_mode = SyncMode::EveryOperation;
    {
        StorageEngine engine;
        open(engine, state, options);
        state->failNthSync(1);
        bool failed = false;
        try {
            engine.put(2, "uncertain");
        } catch (const DbError &e) {
            check(e.code() == ErrorCode::IoError);
            failed = true;
        }
        check(failed);
        try {
            (void)engine.get(2);
            throw std::runtime_error("expected Failed state");
        } catch (const DbError &e) {
            check(e.code() == ErrorCode::EngineFailed);
        }
    }
}
int main() {
    try {
        walTest();
        durableTest(SyncMode::OnFlush);
        durableTest(SyncMode::EveryOperation);
        rollbackTest();
        recoveryInterruptedAndTornMeta();
        beforeCommitAndSyncFailure();
        crashSweep();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
