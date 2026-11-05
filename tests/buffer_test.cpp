#include "../testing/memory_disk.hpp"
#include "test_support.hpp"
#include <iostream>
#include <random>
#include <rohandb/engine/record_manager.hpp>
using namespace rohandb;
using rohandb::testing::check;
using rohandb::testing::expectError;
int main() {
    try {
        LruReplacer lru;
        check(!lru.evict());
        lru.setEvictable(1, true);
        lru.setEvictable(2, true);
        lru.recordAccess(1);
        check(lru.evict() == 2);
        lru.setEvictable(1, false);
        check(lru.size() == 0);
        // Model LRU ordering, idempotent evictability, and repeated accesses.
        LruReplacer modeled;
        std::vector<FrameId> order;
        std::mt19937 random(61);
        for (unsigned i = 0; i < 1000; ++i) {
            const FrameId id = random() % 7;
            const auto op = random() % 4;
            auto position = std::find(order.begin(), order.end(), id);
            if (op == 0) {
                modeled.setEvictable(id, true);
                if (position == order.end())
                    order.push_back(id);
            }
            if (op == 1) {
                modeled.setEvictable(id, false);
                if (position != order.end())
                    order.erase(position);
            }
            if (op == 2) {
                modeled.recordAccess(id);
                if (position != order.end()) {
                    order.erase(position);
                    order.push_back(id);
                }
            }
            if (op == 3) {
                auto victim = modeled.evict();
                check(victim == (order.empty() ? std::nullopt : std::optional(order.front())));
                if (!order.empty())
                    order.erase(order.begin());
            }
            check(modeled.size() == order.size());
        }
        testing::MemoryDisk disk;
        DatabaseMetaManager meta(disk);
        meta.initialiseNew();
        BufferPoolManager pool(1, disk, meta);
        auto a = pool.newPage();
        const auto id = a.pageId();
        SlottedPage(a.mutableData()).initialise();
        check(disk.trace.empty());
        auto moved = std::move(a);
        check(pool.pinCount() == 1);
        expectError(ErrorCode::AllFramesPinned, [&] { (void)pool.newPage(); });
        moved.release();
        check(pool.pinCount() == 0);
        disk.fail_write = true;
        expectError(ErrorCode::IoError, [&] { (void)pool.newPage(); });
        check(pool.resident(id));
        const auto trace_size = disk.trace.size();
        {
            auto read_only = pool.fetchPage(id);
            check(read_only.pageId() == id);
        }
        check(disk.trace.size() == trace_size); // Still mapped: no read was needed.
        disk.fail_write = false;
        auto b = pool.newPage();
        SlottedPage(b.mutableData()).initialise();
        const auto second = b.pageId();
        b.release();
        check(pool.stats().dirty_evictions == 1); // Read-only unpin did not clear dirty state.
        check(disk.pages[id][layout::TYPE] ==
              std::byte{2}); // Failed victim was retried and written.

        disk.fail_read = true;
        expectError(ErrorCode::IoError, [&] { (void)pool.fetchPage(id); });
        check(!pool.resident(id) && !pool.resident(second));
        disk.fail_read = false;
        {
            auto again = pool.fetchPage(id);
            check(again.pageId() == id);
        }
        check(pool.pinCount() == 0);
        RecordManager records(pool);
        std::vector<RecordId> ids;
        for (unsigned i = 0; i < 10; ++i)
            ids.push_back(records.insert(encodeRecord(i, std::string(3000, 'x'))));
        for (unsigned i = 0; i < 10; ++i)
            check(decodeRecord(records.read(ids[i])).first == i);
        check(pool.pinCount() == 0);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
