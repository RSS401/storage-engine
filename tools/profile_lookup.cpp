#include <chrono>
#include <iostream>
#include <rohandb/engine/storage_engine.hpp>
#include <unistd.h>
// An isolated warmed-read workload for sampling profilers. No profiling hooks in the library.
int main(int argc, char **argv) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("rohandb-profile-" + std::to_string(::getpid()) + ".db");
    try {
        const auto seconds = argc > 1 ? std::stoul(argv[1]) : 20UL;
        if (seconds == 0 || seconds > 300)
            throw std::invalid_argument("duration must be 1..300 seconds");
        if (std::filesystem::exists(path) || std::filesystem::exists(path.string() + ".wal"))
            throw std::runtime_error("profiling path exists");
        rohandb::StorageEngine engine;
        rohandb::EngineOptions options;
        options.buffer_pool_frames = 1024;
        engine.open(path, options);
        constexpr std::uint64_t keys = 10000;
        for (std::uint64_t i = 0; i < keys; ++i)
            engine.put(i, "profile-value");
        engine.checkpoint();
        for (std::uint64_t i = 0; i < keys; ++i)
            (void)engine.get(i);
        std::cout << "READY " << ::getpid() << std::endl;
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t reads = 0;
        do {
            for (std::uint64_t i = 0; i < keys; ++i) {
                if (engine.get(i) != std::optional<std::string>("profile-value"))
                    throw std::runtime_error("wrong read");
                ++reads;
            }
        } while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds));
        std::cout << "reads=" << reads << " seconds="
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                  << '\n';
        engine.close();
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".wal");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
