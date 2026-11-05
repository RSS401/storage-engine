#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <rohandb/engine/storage_engine.hpp>
#include <unistd.h>
using namespace rohandb;
using Clock = std::chrono::steady_clock;
int main(int argc, char **argv) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("rohandb-bench-" + std::to_string(::getpid()) + ".db");
    try {
        const std::size_t count = argc > 1 ? std::stoull(argv[1]) : 10000;
        const std::size_t frames = argc > 2 ? std::stoull(argv[2]) : 64;
        const bool random_order = argc > 3 && std::string(argv[3]) == "random";
        const bool every_operation = argc > 4 && std::string(argv[4]) == "every";
        if (count == 0 || count > 1000000)
            throw std::invalid_argument("key count must be 1..1000000");
        if (std::filesystem::exists(path))
            throw std::runtime_error("benchmark temporary path already exists");
        StorageEngine engine;
        EngineOptions options;
        options.buffer_pool_frames = frames;
        options.sync_mode = every_operation ? SyncMode::EveryOperation : SyncMode::OnFlush;
        engine.open(path, options);
        std::vector<std::uint64_t> keys(count);
        std::iota(keys.begin(), keys.end(), 0);
        if (random_order) {
            std::mt19937_64 rng(42);
            std::shuffle(keys.begin(), keys.end(), rng);
        }
        const auto start = Clock::now();
        for (auto key : keys)
            engine.put(key, "benchmark-value");
        engine.flush();
        const auto write_end = Clock::now();
        const auto before = engine.stats();
        std::vector<double> latency;
        latency.reserve(count);
        for (auto key : keys) {
            const auto t = Clock::now();
            if (engine.get(key) != std::optional<std::string>("benchmark-value"))
                throw std::runtime_error("wrong benchmark result");
            latency.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t).count());
        }
        const auto end = Clock::now();
        const auto stats = engine.stats();
        engine.verify();
        engine.close();
        std::sort(latency.begin(), latency.end());
        const auto hits = stats.buffer_hits - before.buffer_hits,
                   misses = stats.buffer_misses - before.buffer_misses;
        std::cout << "{\"schema\":1,\"keys\":" << count << ",\"frames\":" << frames
                  << ",\"order\":\"" << (random_order ? "random" : "sequential")
                  << "\",\"put_ops_s\":"
                  << static_cast<double>(count) /
                         std::chrono::duration<double>(write_end - start).count()
                  << ",\"get_ops_s\":"
                  << static_cast<double>(count) /
                         std::chrono::duration<double>(end - write_end).count()
                  << ",\"get_p50_us\":" << latency[count / 2] << ",\"get_p95_us\":"
                  << latency[static_cast<std::size_t>(static_cast<double>(count - 1) * 0.95)]
                  << ",\"get_p99_us\":"
                  << latency[static_cast<std::size_t>(static_cast<double>(count - 1) * 0.99)]
                  << ",\"hit_rate\":"
                  << (hits + misses ? static_cast<double>(hits) / static_cast<double>(hits + misses)
                                    : 0.0)
                  << ",\"disk_reads_per_get\":"
                  << static_cast<double>(stats.disk_reads - before.disk_reads) /
                         static_cast<double>(count)
                  << ",\"wal_bytes_per_put\":"
                  << static_cast<double>(stats.wal_bytes) / static_cast<double>(count)
                  << ",\"log_forces\":" << stats.log_forces << ",\"sync_mode\":\""
                  << (every_operation ? "EveryOperation" : "OnFlush") << "\""
                  << ",\"disk_writes\":" << stats.disk_writes
                  << ",\"evictions\":" << stats.evictions
                  << ",\"dirty_evictions\":" << stats.dirty_evictions << "}\n";
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".wal");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
