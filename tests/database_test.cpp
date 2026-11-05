#include "test_support.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <rohandb/common/error.hpp>
#include <rohandb/database.hpp>
#include <rohandb/disk_manager.hpp>
#include <rohandb/page.hpp>
#include <stdexcept>
#include <unistd.h>

using rohandb::testing::check;

std::filesystem::path test_database_path() {
    return std::filesystem::temp_directory_path() /
           ("rohandb_disk_manager_test_" + std::to_string(::getpid()) + ".db");
}

void cleanup_test_database() {
    std::error_code error;
    std::filesystem::remove(test_database_path(), error);
}

void test_page_defaults() {
    rohandb::Page page;

    check(page.size() == rohandb::PAGE_SIZE);
    check(page.data() != nullptr);

    for (std::size_t index = 0; index < page.size(); ++index) {
        check(page.data()[index] == std::byte{0});
    }
}

void test_page_is_mutable_through_data_pointer() {
    rohandb::Page page;

    page.data()[0] = std::byte{0x41};
    page.data()[1] = std::byte{0x42};

    check(page.data()[0] == std::byte{0x41});
    check(page.data()[1] == std::byte{0x42});
}

void test_page_const_access_is_read_only() {
    const rohandb::Page page;

    check(page.size() == rohandb::PAGE_SIZE);
    check(page.data() != nullptr);
    check(page.data()[0] == std::byte{0});
}

void test_page_copy_owns_independent_bytes() {
    rohandb::Page original;
    original.data()[0] = std::byte{0x41};

    rohandb::Page copy = original;
    check(copy.data()[0] == std::byte{0x41});

    copy.data()[0] = std::byte{0x42};

    check(copy.data()[0] == std::byte{0x42});
    check(original.data()[0] == std::byte{0x41});
}

void test_database_crud() {
    rohandb::Database db;

    // set/get basic behavior
    db.set("name", "Rohan");
    auto name = db.get("name");
    check(name.has_value());
    check(*name == "Rohan");

    // missing key behavior
    auto missing = db.get("missing");
    check(!missing.has_value());

    // contains checks
    check(db.contains("name"));
    check(!db.contains("missing"));

    // update an existing key
    db.set("name", "RohanDB");
    auto updated = db.get("name");
    check(updated.has_value());
    check(*updated == "RohanDB");

    // remove success and failure
    check(db.remove("name"));
    check(!db.contains("name"));
    check(!db.get("name").has_value());
    check(!db.remove("name"));

    // multiple keys
    db.set("key1", "value1");
    db.set("key2", "value2");
    check(db.contains("key1"));
    check(db.contains("key2"));
    check(db.remove("key1"));
    check(!db.contains("key1"));
}

void test_disk_manager_page_round_trip() {
    cleanup_test_database();

    {
        rohandb::DiskManager disk(test_database_path());

        rohandb::Page page;
        page.data()[0] = std::byte{0x41};
        page.data()[1] = std::byte{0x42};
        page.data()[4095] = std::byte{0x7f};

        disk.write_page(0, page);

        rohandb::Page loaded = disk.read_page(0);
        check(loaded.data()[0] == std::byte{0x41});
        check(loaded.data()[1] == std::byte{0x42});
        check(loaded.data()[4095] == std::byte{0x7f});
    }

    cleanup_test_database();
}

void test_disk_manager_persists_after_reopen() {
    cleanup_test_database();

    {
        rohandb::DiskManager disk(test_database_path());

        rohandb::Page page;
        page.data()[0] = std::byte{0x41};
        page.data()[rohandb::PAGE_SIZE - 1] = std::byte{0x7f};
        disk.write_page(0, page);
    } // Destroying the manager closes the file before we reopen it.

    {
        rohandb::DiskManager disk(test_database_path());

        const rohandb::Page loaded = disk.read_page(0);
        check(loaded.data()[0] == std::byte{0x41});
        check(loaded.data()[rohandb::PAGE_SIZE - 1] == std::byte{0x7f});
    }

    cleanup_test_database();
}

void test_disk_manager_read_missing_page_throws() {
    cleanup_test_database();

    bool threw = false;

    try {
        rohandb::DiskManager disk(test_database_path());
        (void)disk.read_page(0);
    } catch (const rohandb::DbError &error) {
        check(error.code() == rohandb::ErrorCode::PageNotOnDisk);
        threw = true;
    }

    check(threw);

    cleanup_test_database();
}

void test_disk_manager_overwrite_preserves_other_pages() {
    cleanup_test_database();
    {
        rohandb::DiskManager disk(test_database_path());
        rohandb::Page first;
        rohandb::Page second;
        std::fill_n(first.data(), first.size(), std::byte{0x11});
        std::fill_n(second.data(), second.size(), std::byte{0x22});
        disk.write_page(0, first);
        disk.write_page(1, second);

        std::fill_n(first.data(), first.size(), std::byte{0x33});
        disk.write_page(0, first);
        const auto loaded_first = disk.read_page(0);
        const auto loaded_second = disk.read_page(1);
        check(std::equal(first.data(), first.data() + first.size(), loaded_first.data()));
        check(std::equal(second.data(), second.data() + second.size(), loaded_second.data()));
        check(std::filesystem::file_size(test_database_path()) == 2 * rohandb::PAGE_SIZE);
    }
    cleanup_test_database();
}

void test_disk_manager_rejects_overflowing_page_ids() {
    cleanup_test_database();
    {
        rohandb::DiskManager disk(test_database_path());
        rohandb::Page page;
        page.data()[0] = std::byte{0x41};
        disk.write_page(0, page);

        const rohandb::PageId invalid_ids[] = {rohandb::INVALID_PAGE_ID};
        for (const auto id : invalid_ids) {
            bool read_threw = false;
            try {
                (void)disk.read_page(id);
            } catch (const rohandb::DbError &error) {
                check(error.code() == rohandb::ErrorCode::InvalidArgument);
                read_threw = true;
            }
            check(read_threw);

            bool write_threw = false;
            try {
                disk.write_page(id, rohandb::Page{});
            } catch (const rohandb::DbError &error) {
                check(error.code() == rohandb::ErrorCode::InvalidArgument);
                write_threw = true;
            }
            check(write_threw);
        }
        check(std::filesystem::file_size(test_database_path()) == rohandb::PAGE_SIZE);
        check(disk.read_page(0).data()[0] == std::byte{0x41});
    }
    cleanup_test_database();
}

void test_disk_manager_rejects_partial_page() {
    cleanup_test_database();
    {
        std::ofstream file(test_database_path(), std::ios::binary);
        file.put('x');
        check(file.good());
    }
    {
        rohandb::DiskManager disk(test_database_path());
        bool threw = false;
        try {
            (void)disk.read_page(0);
        } catch (const rohandb::DbError &error) {
            check(error.code() == rohandb::ErrorCode::ShortRead);
            threw = true;
        }
        check(threw);
    }
    cleanup_test_database();
}

int main() {
    test_page_defaults();
    test_page_is_mutable_through_data_pointer();
    test_page_const_access_is_read_only();
    test_page_copy_owns_independent_bytes();
    test_database_crud();
    test_disk_manager_page_round_trip();
    test_disk_manager_persists_after_reopen();
    test_disk_manager_read_missing_page_throws();
    test_disk_manager_overwrite_preserves_other_pages();
    test_disk_manager_rejects_overflowing_page_ids();
    test_disk_manager_rejects_partial_page();

    return 0;
}
