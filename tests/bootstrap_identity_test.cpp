#include "../testing/simulated_storage.hpp"
#include "test_support.hpp"
#include <fstream>
#include <iostream>
#include <rohandb/engine/storage_engine.hpp>
#include <unistd.h>
using namespace rohandb;
using namespace rohandb::testing;
void openEngine(StorageEngine &engine, const std::shared_ptr<SimulatedStorage> &state) {
    EngineOptions options;
    options.sync_mode = SyncMode::EveryOperation;
    engine.open(std::make_unique<FaultInjectingDiskManager>(state),
                std::make_unique<FaultInjectingWalFile>(state), options);
}
std::shared_ptr<SimulatedStorage> committedState() {
    auto state = std::make_shared<SimulatedStorage>();
    {
        StorageEngine engine;
        openEngine(engine, state);
        engine.put(7, "committed");
        state->crashNow(CrashMode::DropAllUnsynced, 1);
    }
    state->reopen();
    return state;
}
void identityCases() {
    auto a = committedState();
    auto b = committedState();
    check(readId(a->database, PAGE_SIZE + identity_layout::ID) !=
          readId(b->database, PAGE_SIZE + identity_layout::ID));
    const auto database = b->database;
    b->wal = a->wal;
    const auto events = b->ioEventCount();
    expectError(ErrorCode::WalCorrupt, [&] {
        StorageEngine engine;
        openEngine(engine, b);
    });
    check(b->database == database && b->ioEventCount() == events);
    b->database.clear();
    expectError(ErrorCode::WalCorrupt, [&] {
        StorageEngine engine;
        openEngine(engine, b);
    });
    check(b->database.empty());
    a->flipBit(IDENTITY_PAGE_ID, identity_layout::ID * 8);
    expectError(ErrorCode::CorruptPage, [&] {
        StorageEngine engine;
        openEngine(engine, a);
    });
}
void bootstrapSweep() {
    auto probe = std::make_shared<SimulatedStorage>();
    std::uint64_t total;
    {
        StorageEngine engine;
        openEngine(engine, probe);
        total = probe->ioEventCount();
    }
    for (auto mode :
         {CrashMode::DropAllUnsynced, CrashMode::KeepRandomSubset, CrashMode::TearOneWrite})
        for (std::uint64_t event = 1; event <= total; ++event) {
            auto state = std::make_shared<SimulatedStorage>();
            state->crashAtEvent(event, mode, 41);
            expectError(ErrorCode::IoError, [&] {
                StorageEngine engine;
                openEngine(engine, state);
            });
            state->reopen();
            {
                StorageEngine engine;
                openEngine(engine, state);
                engine.verify();
                check(!engine.get(7));
            }
        }
}
void realFileCases() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("rohandb-bootstrap-" + std::to_string(::getpid()));
    std::filesystem::create_directory(directory);
    const auto path = directory / "test.db";
    try {
        {
            std::ofstream wal(path.string() + ".wal", std::ios::binary);
            wal << "orphan";
        }
        expectError(ErrorCode::WalCorrupt, [&] {
            StorageEngine engine;
            engine.open(path);
        });
        check(!std::filesystem::exists(path));
        std::filesystem::remove(path.string() + ".wal");
        {
            StorageEngine engine;
            engine.open(path);
            engine.put(8, "real-file");
            engine.close();
        }
        {
            StorageEngine engine;
            engine.open(path);
            check(engine.get(8) == std::optional<std::string>("real-file"));
            engine.verify();
        }
        check(std::filesystem::file_size(path) >= 3 * PAGE_SIZE);
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            check(entry.path().filename().string().find("bootstrap.") == std::string::npos);
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}
int main() {
    try {
        identityCases();
        bootstrapSweep();
        realFileCases();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
