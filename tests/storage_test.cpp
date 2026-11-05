#include "test_support.hpp"
#include <fstream>
#include <iostream>
#include <rohandb/common/crc32c.hpp>
#include <rohandb/storage/meta_manager.hpp>
#include <rohandb/storage/slotted_page.hpp>
#include <unistd.h>
using namespace rohandb;
using rohandb::testing::check;
using rohandb::testing::expectError;
int main() {
    const auto path =
        std::filesystem::temp_directory_path() / ("rohandb-storage-" + std::to_string(::getpid()));
    try {
        std::filesystem::remove(path);
        {
            DiskManager disk(path);
            DatabaseMetaManager meta(disk);
            meta.initialiseNew();
            check(meta.allocatePageId() == 2);
            meta.setRootPageId(2);
            PageBytes root{};
            disk.writePage(1, makeIdentityPage(meta.databaseId()));
            disk.writePage(2, root);
            disk.sync();
            meta.writeIfDirty();
            disk.sync();
            expectError(ErrorCode::PageNotOnDisk, [&] { disk.readPage(3, root); });
        }
        {
            DiskManager disk(path, false);
            DatabaseMetaManager meta(disk);
            meta.load();
            check(meta.rootPageId() == 2 && meta.allocatePageId() == 3);
            auto bytes = meta.bytes();
            writeScalar<std::uint64_t>(bytes, layout::MAGIC, 0);
            stampChecksum(bytes);
            disk.writePage(0, bytes);
            expectError(ErrorCode::BadMagic, [&] { meta.load(); });
        }
        {
            DiskManager disk(path);
            DatabaseMetaManager meta(disk);
            meta.initialiseNew();
            (void)meta.allocatePageId();
            meta.setRootPageId(2);
            meta.writeIfDirty();
            const auto original = disk.read_page(0);
            for (const auto [offset, code] :
                 {std::pair{layout::VERSION, ErrorCode::UnsupportedVersion},
                  std::pair{layout::PAGE_BYTES, ErrorCode::PageSizeMismatch},
                  std::pair{layout::ROOT, ErrorCode::CorruptPage}}) {
                auto changed = original;
                PageView view(changed.data(), PAGE_SIZE);
                writeScalar<std::uint32_t>(view, offset, 0);
                stampChecksum(view);
                disk.writePage(0, view);
                expectError(code, [&] { meta.load(); });
            }
        }
        std::filesystem::resize_file(path, 1);
        {
            DiskManager disk(path);
            PageBytes bytes{};
            expectError(ErrorCode::ShortRead, [&] { disk.readPage(0, bytes); });
        }
        PageBytes bytes{};
        SlottedPage page(bytes);
        page.initialise();
        const auto a = encodeRecord(7, std::string(1800, 'a'));
        const auto b = encodeRecord(8, std::string(1800, 'b'));
        check(page.insert(a) == 0);
        check(page.insert(b) == 1);
        check(!page.insert(a));
        page.erase(0);
        check(page.insert(encodeRecord(9, std::string(1900, 'c'))) == 0);
        check(page.read(1) == b);
        page.compact();
        check(page.read(1) == b);
        check(decodeRecord(page.read(0)).first == 9);
        expectError(ErrorCode::CorruptPage, [&] { (void)page.read(2); });
        writeScalar<std::uint16_t>(bytes, layout::FREE_END, 0);
        expectError(ErrorCode::CorruptPage, [&] { page.validate(); });
        page.initialise();
        check(page.insert(encodeRecord(0, std::string(layout::MAX_VALUE_SIZE, 'x'))) == 0);
        expectError(ErrorCode::RecordTooLarge,
                    [] { (void)encodeRecord(0, std::string(layout::MAX_VALUE_SIZE + 1, 'x')); });
        check(decodeRecord(encodeRecord(99, std::string("a\0b", 3))).second ==
              std::string("a\0b", 3));
        check(decodeRecord(encodeRecord(0, "")).second.empty());
        std::filesystem::remove(path);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove(path);
        return 1;
    }
}
