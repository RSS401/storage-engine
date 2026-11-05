#include "../testing/memory_disk.hpp"
#include "test_support.hpp"
#include <iostream>
#include <map>
#include <random>
#include <rohandb/engine/storage_engine.hpp>
#include <unistd.h>
using namespace rohandb;
using rohandb::testing::check;
int main() {
    const auto path =
        std::filesystem::temp_directory_path() / ("rohandb-engine-" + std::to_string(::getpid()));
    try {
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".wal");
        EngineOptions options;
        options.buffer_pool_frames = 64;
#ifndef ROHANDB_WAL
        options.buffer_pool_frames = 3;
#endif
        options.leaf_max_size = 4;
        options.internal_max_size = 4;
        std::map<std::uint64_t, std::string> model;
        {
            StorageEngine engine;
            engine.open(path, options);
            for (std::uint64_t i = 0; i < 200; ++i) {
                check(engine.put(i, std::to_string(i)));
                model[i] = std::to_string(i);
            }
            engine.verify();
            for (std::uint64_t i = 200; i-- > 0;)
                check(engine.get(i) == model[i]);
            std::mt19937_64 random(42);
            for (unsigned i = 0; i < 3000; ++i) {
                const auto key = random() % 500;
                if (random() % 3 == 0)
                    check(engine.erase(key) == (model.erase(key) != 0));
                else {
                    const auto value = std::string(static_cast<std::size_t>(random() % 80), 'x');
                    check(engine.put(key, value) == (!model.contains(key)));
                    model[key] = value;
                }
                if (i % 100 == 0)
                    engine.verify();
            }
            engine.verify();
            check(engine.stats().dirty_evictions > 0);
            engine.close();
        }
        {
            StorageEngine engine;
            engine.open(path, options);
            engine.verify();
            for (std::uint64_t i = 0; i < 500; ++i) {
                const auto it = model.find(i);
                check(engine.get(i) ==
                      (it == model.end() ? std::nullopt : std::optional(it->second)));
            }
            std::mt19937_64 starts(519);
            for (unsigned trial = 0; trial < 24; ++trial) {
                const auto from = starts() % 600;
                auto expected = model.lower_bound(from);
                auto actual = engine.scan(from);
                while (expected != model.end()) {
                    check(actual.valid());
                    check(actual.key() == expected->first);
                    check(actual.value() == expected->second);
                    actual.next();
                    ++expected;
                }
                check(!actual.valid());
            }
            for (const auto &[key, value] : model) {
                (void)value;
                check(engine.erase(key));
            }
            engine.verify();
            check(engine.put(0, "again"));
            engine.verify();
            check(engine.put(std::numeric_limits<std::uint64_t>::max(), "last"));
            engine.verify();
        }
        // Reverse insertion and the revision 3.1 separator edge case.
        {
            StorageEngine reverse;
#ifdef ROHANDB_WAL
            reverse.open(std::make_unique<testing::MemoryDisk>(),
                         std::make_unique<testing::MemoryWal>(), options);
#else
            reverse.open(std::make_unique<testing::MemoryDisk>(), options);
#endif
            for (std::uint64_t key = 80; key-- > 0;)
                reverse.put(key, std::to_string(key));
            reverse.verify();
            for (std::uint64_t key = 2; key < 78; key += 2) {
                check(reverse.erase(key));
                check(!reverse.get(key));
                check(reverse.get(key - 1).has_value());
                check(reverse.get(key + 1).has_value());
                reverse.verify();
                check(reverse.put(key, "reinserted"));
                reverse.verify();
            }
        }
        auto disk = std::make_unique<testing::MemoryDisk>();
        auto *trace = disk.get();
        StorageEngine engine;
#ifdef ROHANDB_WAL
        engine.open(std::move(disk), std::make_unique<testing::MemoryWal>(), options);
#else
        engine.open(std::move(disk), options);
#endif
        engine.put(1, "value");
        trace->trace.clear();
        engine.checkpoint();
        const auto &events = trace->trace;
        check(events.size() >= 4 && events[events.size() - 1] == "sync" &&
              events[events.size() - 2] == "write:0" && events[events.size() - 3] == "sync");
        engine.close();
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".wal");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".wal");
        return 1;
    }
}
