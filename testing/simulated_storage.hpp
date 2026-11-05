#pragma once
#include <memory>
#include <random>
#include <rohandb/wal/log_manager.hpp>
#include <string>
namespace rohandb::testing {
enum class SyncFailureMode { RetainPending, DiscardPending };
enum class CrashMode { DropAllUnsynced, KeepRandomSubset, TearOneWrite };
struct IoEvent {
    std::string operation;
    bool wal;
    std::uint64_t offset;
};
// Owned by a test, outlives all engine instances. Reads overlay pending writes.
class SimulatedStorage {
  public:
    struct Write {
        bool wal;
        std::size_t offset;
        std::vector<std::byte> bytes;
    };
    std::vector<std::byte> database, wal;
    std::vector<Write> pending;
    std::vector<IoEvent> events;
    bool crashed = false;
    SyncFailureMode sync_failure_mode = SyncFailureMode::RetainPending;
    std::uint64_t crash_at = 0, fail_read = 0, fail_write = 0, fail_sync = 0, short_read = 0;
    std::uint64_t reads = 0, writes = 0, syncs = 0, event_count = 0;
    CrashMode mode = CrashMode::DropAllUnsynced;
    std::uint64_t seed = 42;
    std::size_t tear_alignment = 512;
    void alive() const {
        if (crashed)
            throw DbError(ErrorCode::IoError, "simulated process crashed");
    }
    static void apply(std::vector<std::byte> &destination, const Write &write, std::size_t count) {
        if (destination.size() < write.offset + count)
            destination.resize(write.offset + count);
        std::copy_n(write.bytes.begin(), count,
                    destination.begin() + static_cast<std::ptrdiff_t>(write.offset));
    }
    std::vector<std::byte> visible(bool log) const {
        alive();
        auto result = log ? wal : database;
        for (const auto &write : pending)
            if (write.wal == log)
                apply(result, write, write.bytes.size());
        return result;
    }
    void event(std::string operation, bool log, std::uint64_t offset = 0) {
        alive();
        events.push_back({std::move(operation), log, offset});
        if (++event_count == crash_at) {
            crashNow(mode, seed);
            throw DbError(ErrorCode::IoError, "injected crash");
        }
    }
    void readEvent() {
        alive();
        ++reads;
        if (reads == fail_read)
            throw DbError(ErrorCode::IoError, "injected read error");
        if (reads == short_read)
            throw DbError(ErrorCode::ShortRead, "injected short read");
    }
    void write(bool log, std::size_t offset, std::span<const std::byte> bytes) {
        event("write", log, offset);
        if (++writes == fail_write)
            throw DbError(ErrorCode::IoError, "injected write error");
        pending.push_back({log, offset, {bytes.begin(), bytes.end()}});
    }
    void sync(bool log) {
        event("sync", log);
        if (++syncs == fail_sync) {
            if (sync_failure_mode == SyncFailureMode::DiscardPending)
                std::erase_if(pending, [&](const Write &w) { return w.wal == log; });
            throw DbError(ErrorCode::IoError, "injected sync error");
        }
        for (const auto &write : pending)
            if (write.wal == log)
                apply(log ? wal : database, write, write.bytes.size());
        std::erase_if(pending, [&](const Write &write) { return write.wal == log; });
    }
    void reset(std::span<const std::byte> header) {
        event("reset", true);
        if (++writes == fail_write)
            throw DbError(ErrorCode::IoError, "injected reset error");
        // Models synced atomic replacement: after the event, either old or new header exists.
        wal.assign(header.begin(), header.end());
        std::erase_if(pending, [](const Write &w) { return w.wal; });
    }
    void crashNow(CrashMode crash_mode, std::uint64_t random_seed) {
        std::mt19937_64 random(random_seed);
        if (crash_mode == CrashMode::KeepRandomSubset) {
            // Database writes may reorder. WAL bytes must remain a prefix.
            bool wal_stopped = false;
            for (const auto &write : pending) {
                if (write.wal) {
                    if (!wal_stopped && random() % 2)
                        apply(wal, write, write.bytes.size());
                    else
                        wal_stopped = true;
                } else if (random() % 2)
                    apply(database, write, write.bytes.size());
            }
        } else if (crash_mode == CrashMode::TearOneWrite && !pending.empty()) {
            const auto index = static_cast<std::size_t>(random() % pending.size());
            const auto &write = pending[index];
            if (write.wal)
                for (std::size_t i = 0; i < index; ++i)
                    if (pending[i].wal)
                        apply(wal, pending[i], pending[i].bytes.size());
            const auto count = std::min(
                write.bytes.size(),
                static_cast<std::size_t>(random() % (write.bytes.size() / tear_alignment + 1)) *
                    tear_alignment);
            apply(write.wal ? wal : database, write, count);
        }
        pending.clear();
        crashed = true;
    }
    void reopen() {
        crashed = false;
        crash_at = fail_read = fail_write = fail_sync = short_read = 0;
    }
    void failNthRead(std::uint64_t n) { fail_read = reads + n; }
    void failNthWrite(std::uint64_t n) { fail_write = writes + n; }
    void failNthSync(std::uint64_t n) { fail_sync = syncs + n; }
    void shortReadOnNth(std::uint64_t n) { short_read = reads + n; }
    void crashAtEvent(std::uint64_t n, CrashMode m, std::uint64_t s) {
        crash_at = event_count + n;
        mode = m;
        seed = s;
    }
    std::uint64_t ioEventCount() const { return event_count; }
    const std::vector<IoEvent> &trace() const { return events; }
    void flipBit(PageId id, std::size_t bit) {
        const auto offset = static_cast<std::size_t>(id) * PAGE_SIZE + bit / 8;
        if (bit >= PAGE_SIZE * 8 || offset >= database.size())
            throw std::out_of_range("invalid corruption location");
        database[offset] ^= static_cast<std::byte>(1U << (bit % 8));
    }
};
class FaultInjectingDiskManager final : public IDiskManager {
  public:
    explicit FaultInjectingDiskManager(std::shared_ptr<SimulatedStorage> state)
        : state_(std::move(state)) {}
    void readPage(PageId id, PageView out) override {
        state_->readEvent();
        const auto bytes = state_->visible(false);
        const auto offset = static_cast<std::size_t>(id) * PAGE_SIZE;
        if (offset >= bytes.size())
            throw DbError(ErrorCode::PageNotOnDisk, "missing simulated page");
        if (bytes.size() - offset < PAGE_SIZE)
            throw DbError(ErrorCode::ShortRead, "short simulated page");
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), PAGE_SIZE, out.begin());
    }
    void writePage(PageId id, ConstPageView bytes) override {
        state_->write(false, static_cast<std::size_t>(id) * PAGE_SIZE, bytes);
    }
    void publishInitial(std::span<const PageBytes> pages) override {
        if (byteSize() != 0)
            throw DbError(ErrorCode::InvalidArgument, "nonempty bootstrap");
        state_->event("publish_initial", false);
        std::vector<std::byte> all;
        for (const auto &page : pages)
            all.insert(all.end(), page.begin(), page.end());
        state_->database = std::move(all);
        state_->event("publish_initial_synced", false);
    }
    void sync() override { state_->sync(false); }
    std::uint32_t pageCountOnDisk() const override {
        return static_cast<std::uint32_t>((byteSize() + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    std::uint64_t byteSize() const override { return state_->visible(false).size(); }

  private:
    std::shared_ptr<SimulatedStorage> state_;
};
class FaultInjectingWalFile final : public IWalFile {
  public:
    explicit FaultInjectingWalFile(std::shared_ptr<SimulatedStorage> state)
        : state_(std::move(state)) {}
    std::vector<std::byte> readAll() override {
        state_->readEvent();
        return state_->visible(true);
    }
    void append(std::span<const std::byte> bytes) override {
        state_->write(true, state_->visible(true).size(), bytes);
    }
    void sync() override { state_->sync(true); }
    void reset(std::span<const std::byte> header) override { state_->reset(header); }

  private:
    std::shared_ptr<SimulatedStorage> state_;
};
} // namespace rohandb::testing
