#include <fstream>
#include <iostream>
#include <queue>
#include <rohandb/common/crc32c.hpp>
#include <rohandb/index/bplus_tree.hpp>
#include <rohandb/wal/log_manager.hpp>
#include <set>
using namespace rohandb;
int main(int argc, char **argv) {
    try {
        if (argc < 3) {
            std::cerr << "usage: db_inspect FILE meta|page ID|btree|record PAGE SLOT|verify|wal\n";
            return 2;
        }
        const auto wal_path = std::string(argv[1]) + ".wal";
        std::ifstream wal_input(wal_path, std::ios::binary);
        std::vector<char> wal_chars((std::istreambuf_iterator<char>(wal_input)), {});
        const auto wal_scan = scanWal(std::as_bytes(std::span(wal_chars)));
        DiskManager disk(argv[1], false, true);
        const auto database_id = readDatabaseId(disk);
        if (!wal_chars.empty() && wal_scan.database_id != database_id)
            throw DbError(ErrorCode::WalCorrupt, "WAL belongs to a different database");
        std::set<std::uint64_t> committed;
        for (const auto &record : wal_scan.records)
            if (record.image.empty())
                committed.insert(record.op_id);
        if (std::string(argv[2]) == "wal") {
            for (const auto &record : wal_scan.records)
                std::cout << "lsn=" << record.lsn << " op=" << record.op_id << " "
                          << (record.image.empty() ? "COMMIT" : "PAGE_IMAGE")
                          << " page=" << record.page_id
                          << " committed=" << committed.contains(record.op_id) << '\n';
            std::cout << "committed_operations=" << committed.size()
                      << " torn_tail=" << wal_scan.torn_tail << '\n';
            return 0;
        }
        std::set<std::uint64_t> unapplied;
        for (const auto &record : wal_scan.records)
            if (!record.image.empty() && committed.contains(record.op_id)) {
                try {
                    PageBytes bytes;
                    disk.readPage(record.page_id, bytes);
                    verifyChecksum(bytes);
                    if (readScalar<Lsn>(bytes, layout::LSN) < record.lsn)
                        unapplied.insert(record.op_id);
                } catch (const DbError &) {
                    unapplied.insert(record.op_id);
                }
            }
        if (!unapplied.empty()) {
            std::cerr << "WAL contains " << unapplied.size()
                      << " committed operations with unapplied page images; inspector does not "
                         "recover.\n";
            if (std::string(argv[2]) == "verify")
                return 1;
        }
        DatabaseMetaManager meta(disk);
        meta.load();
        BufferPoolManager pool(8, disk, meta);
        RecordManager records(pool);
        BPlusTree tree(pool, meta);
        const std::string command = argv[2];
        const auto pageId = [](const char *text) {
            const auto n = std::stoull(text);
            if (n >= INVALID_PAGE_ID)
                throw std::invalid_argument("invalid page ID");
            return static_cast<PageId>(n);
        };
        if (command == "meta") {
            const auto bytes = meta.bytes();
            std::cout << "magic=" << readScalar<std::uint64_t>(bytes, layout::MAGIC)
                      << " checksum=" << readScalar<std::uint32_t>(bytes, layout::CHECKSUM)
                      << " checksum_status=" << (FORMAT_FEATURES ? "valid" : "disabled")
                      << " page_lsn=" << readScalar<Lsn>(bytes, layout::LSN)
                      << " format=" << readScalar<std::uint32_t>(bytes, layout::VERSION)
                      << " page_size=" << PAGE_SIZE
                      << " flags=" << readScalar<std::uint32_t>(bytes, layout::FEATURES)
                      << " root=" << meta.rootPageId() << " next=" << meta.nextPageId()
                      << " free_head=" << readScalar<PageId>(bytes, layout::FREE_HEAD)
                      << " checkpoint_lsn=" << readScalar<Lsn>(bytes, layout::CHECKPOINT) << '\n';
        } else if (command == "verify") {
            tree.verify(records);
            std::cout << "Structural and record verification passed\n";
        } else if (command == "record" && argc == 5) {
            const auto slot = std::stoul(argv[4]);
            if (slot > std::numeric_limits<SlotId>::max())
                throw std::invalid_argument("invalid slot");
            const auto [key, value] =
                decodeRecord(records.read({pageId(argv[3]), static_cast<SlotId>(slot)}));
            std::cout << "key=" << key << " value_length=" << value.size() << " value=" << value
                      << '\n';
        } else if (command == "btree") {
            std::queue<std::pair<PageId, unsigned>> pending;
            pending.emplace(tree.root(), 0);
            std::set<PageId> seen;
            while (!pending.empty()) {
                const auto [id, level] = pending.front();
                pending.pop();
                if (!seen.insert(id).second)
                    throw DbError(ErrorCode::CorruptPage, "tree cycle");
                const auto node = tree.readNode(id);
                std::cout << "level=" << level << " page=" << id << " "
                          << (node.leaf ? "leaf" : "internal") << " keys=";
                for (auto key : node.keys)
                    std::cout << key << ' ';
                if (node.leaf)
                    std::cout << " next=" << node.next;
                std::cout << '\n';
                for (auto child : node.children)
                    pending.emplace(child, level + 1);
            }
        } else if (command == "page" && argc == 4) {
            PageBytes bytes;
            const auto id = pageId(argv[3]);
            disk.readPage(id, bytes);
            verifyChecksum(bytes);
            const auto type = readScalar<std::uint8_t>(bytes, layout::TYPE);
            std::cout << "page=" << id << " type=" << static_cast<unsigned>(type)
                      << " checksum=" << readScalar<std::uint32_t>(bytes, layout::CHECKSUM)
                      << " lsn=" << readScalar<Lsn>(bytes, layout::LSN) << '\n';
            if (type == 2) {
                SlottedPage page(bytes);
                page.validate();
                for (SlotId slot = 0; slot < page.slotCount(); ++slot) {
                    const auto offset = layout::DATA_HEADER + slot * layout::SLOT_SIZE;
                    std::cout << "slot=" << slot << " offset="
                              << readScalar<std::uint16_t>(bytes, offset + layout::SLOT_OFFSET)
                              << " length="
                              << readScalar<std::uint16_t>(bytes, offset + layout::SLOT_LENGTH)
                              << " flags="
                              << readScalar<std::uint16_t>(bytes, offset + layout::SLOT_FLAGS)
                              << '\n';
                }
            } else if (type == 3 || type == 4) {
                const auto node = tree.readNode(id);
                for (auto key : node.keys)
                    std::cout << "key=" << key << '\n';
            } else if (type == identity_layout::TYPE && id == IDENTITY_PAGE_ID) {
                (void)readDatabaseId(disk);
                std::cout << "immutable database identity (validated), format="
                          << layout::VERSION_VALUE << '\n';
            } else if (type != 1)
                throw DbError(ErrorCode::CorruptPage, "invalid page type");
        } else {
            std::cerr << "invalid inspector arguments\n";
            return 2;
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
