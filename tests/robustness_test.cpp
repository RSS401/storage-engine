#include "../testing/memory_disk.hpp"
#include "test_support.hpp"
#include <iostream>
#include <rohandb/common/crc32c.hpp>
#include <rohandb/engine/storage_engine.hpp>
using namespace rohandb;
using rohandb::testing::check;
using rohandb::testing::expectError;
int main() {
    try {
        const std::string vector = "123456789";
        check(crc32c(std::as_bytes(std::span(vector))) == 0xe3069283U);
        check(crc32cSoftware(std::as_bytes(std::span(vector))) == 0xe3069283U);
        {
            // Hardware and table paths agree at every length and misalignment.
            std::vector<std::byte> random(PAGE_SIZE + 16);
            std::uint32_t state = 7;
            for (auto &b : random)
                b = static_cast<std::byte>((state = state * 1103515245U + 12345U) >> 24);
            for (std::size_t offset = 0; offset < 8; ++offset)
                for (std::size_t length = 0; length <= 64; ++length) {
                    const auto view = std::span(random).subspan(offset, length);
                    check(crc32c(view) == crc32cSoftware(view));
                }
            const auto page = std::span(random).subspan(3, PAGE_SIZE);
            check(crc32c(page) == crc32cSoftware(page));
        }
        auto disk = std::make_unique<testing::MemoryDisk>();
        auto *raw = disk.get();
        StorageEngine engine;
        EngineOptions opts;
        opts.leaf_max_size = 4;
        opts.internal_max_size = 4;
        opts.buffer_pool_frames = 32;
#ifdef ROHANDB_WAL
        engine.open(std::move(disk), std::make_unique<testing::MemoryWal>(), opts);
#else
        engine.open(std::move(disk), opts);
#endif
        for (std::uint64_t i = 0; i < 100; ++i)
            engine.put(i * 2, std::to_string(i));
        for (std::uint64_t i = 10; i < 50; ++i)
            engine.erase(i * 2);
        {
            auto it = engine.scan(19);
            check(it.valid() && it.key() == 100);
            for (std::uint64_t i = 50; i < 100; ++i) {
                check(it.key() == i * 2 && it.value() == std::to_string(i));
                it.next();
            }
            check(!it.valid());
        }
        {
            auto it = engine.scan(199);
            check(!it.valid());
        }
        {
            auto it = engine.scan(0);
            expectError(ErrorCode::RecordTooLarge,
                        [&] { engine.put(1000, std::string(layout::MAX_VALUE_SIZE + 1, 'x')); });
            check(it.valid());
            check(!engine.erase(9999));
            check(it.valid());
            engine.put(301, "x");
            expectError(ErrorCode::InvalidArgument, [&] { (void)it.valid(); });
        }
        engine.verify();
        engine.flush();
        raw->fail_write = true;
        bool failed = false;
        try {
#ifdef ROHANDB_WAL
            engine.checkpoint();
#else
            for (std::uint64_t i = 1000; i < 1100; ++i)
                engine.put(i, std::string(3000, 'x'));
#endif
        } catch (const DbError &e) {
            check(e.code() == ErrorCode::IoError);
            failed = true;
        }
        check(failed);
        expectError(ErrorCode::EngineFailed, [&] { (void)engine.get(0); });
        const auto events = raw->trace.size();
        raw->fail_write = false; // close in Failed state must not write.
        engine.close();
        (void)events;
#ifdef ROHANDB_CHECKSUMS
        testing::MemoryDisk storage;
        DatabaseMetaManager meta(storage);
        meta.initialiseNew();
        BufferPoolManager pool(1, storage, meta);
        auto a = pool.newPage();
        const auto id = a.pageId();
        SlottedPage(a.mutableData()).initialise();
        a.release();
        pool.flushAllPages();
        auto b = pool.newPage();
        SlottedPage(b.mutableData()).initialise();
        b.release();
        storage.pages[id][100] ^= std::byte{1};
        expectError(ErrorCode::CorruptPage, [&] { (void)pool.fetchPage(id); });
        check(!pool.resident(id));
        // A mixed old/new page must be rejected before its body is parsed, including M2 builds.
        auto old = storage.pages[id];
        old[100] ^= std::byte{1}; // undo the previous flipped bit
        auto newer = old;
        newer[1000] ^= std::byte{1};
        stampChecksum(newer);
        std::copy_n(newer.begin(), 512, old.begin());
        storage.pages[id] = old;
        expectError(ErrorCode::CorruptPage, [&] { (void)pool.fetchPage(id); });
        check(!pool.resident(id));

#endif
        {
            // Pages are validated structurally when loaded; a checksum-valid bad page never
            // becomes a trusted cached frame.
            testing::MemoryDisk bad_disk;
            DatabaseMetaManager bad_meta(bad_disk);
            bad_meta.initialiseNew();
            for (unsigned i = 0; i < 3; ++i)
                (void)bad_meta.allocatePageId();
            bad_meta.setRootPageId(2);
            const auto store = [&](PageId page, PageBytes bytes) {
                stampChecksum(bytes);
                bad_disk.writePage(page, bytes);
            };
            PageBytes leaf{};
            writeScalar<std::uint8_t>(leaf, layout::TYPE, 3);
            writeScalar<std::uint16_t>(leaf, layout::NODE_SIZE, 2);
            writeScalar<std::uint16_t>(leaf, layout::NODE_MAX, 4);
            for (std::size_t i = 0; i < 2; ++i) {
                const auto entry = layout::ENTRIES + i * layout::LEAF_ENTRY;
                writeScalar<std::uint64_t>(leaf, entry + layout::LEAF_KEY, 9 - i); // unordered
                writeScalar<PageId>(leaf, entry + layout::LEAF_RID_PAGE, 4);
            }
            store(2, leaf);
            PageBytes internal{};
            writeScalar<std::uint8_t>(internal, layout::TYPE, 4);
            writeScalar<std::uint16_t>(internal, layout::NODE_SIZE, 2);
            writeScalar<std::uint16_t>(internal, layout::NODE_MAX, 4);
            writeScalar<PageId>(internal, layout::FIRST_CHILD, 2);
            writeScalar<std::uint64_t>(internal, layout::FIRST_SEPARATOR, 5);
            writeScalar<PageId>(internal, layout::FIRST_SEPARATOR + layout::CHILD_IN_ENTRY, 99);
            store(3, internal);
            PageBytes data{};
            SlottedPage page(data);
            page.initialise();
            (void)page.insert(encodeRecord(1, "first"));
            (void)page.insert(encodeRecord(2, "second"));
            // Make slot 1 overlap slot 0; reading slot 0 alone would otherwise look valid.
            writeScalar<std::uint16_t>(data, layout::DATA_HEADER + layout::SLOT_SIZE,
                                       readScalar<std::uint16_t>(data, layout::DATA_HEADER) + 1);
            store(4, data);
            BufferPoolManager bad_pool(4, bad_disk, bad_meta);
            BPlusTree tree(bad_pool, bad_meta);
            RecordManager records(bad_pool);
            expectError(ErrorCode::CorruptPage, [&] { (void)tree.find(9); });
            check(!bad_pool.resident(2));
            bad_meta.setRootPageId(3);
            expectError(ErrorCode::CorruptPage, [&] { (void)tree.find(1); });
            check(!bad_pool.resident(3));
            expectError(ErrorCode::CorruptPage, [&] { (void)records.read({4, 0}); });
            check(!bad_pool.resident(4));
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
