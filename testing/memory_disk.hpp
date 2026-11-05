#pragma once
#include <rohandb/common/error.hpp>
#include <rohandb/disk_manager.hpp>
#include <rohandb/wal/wal_file.hpp>
#include <vector>
namespace rohandb::testing {
class MemoryDisk : public IDiskManager {
  public:
    std::vector<PageBytes> pages;
    std::vector<std::string> trace;
    bool fail_read = false, fail_write = false;
    void readPage(PageId id, PageView out) override {
        trace.push_back("read:" + std::to_string(id));
        if (fail_read)
            throw DbError(ErrorCode::IoError, "injected read failure");
        if (id >= pages.size())
            throw DbError(ErrorCode::PageNotOnDisk, "missing page");
        std::copy(pages[id].begin(), pages[id].end(), out.begin());
    }
    void writePage(PageId id, ConstPageView data) override {
        trace.push_back("write:" + std::to_string(id));
        if (fail_write)
            throw DbError(ErrorCode::IoError, "injected write failure");
        if (id >= pages.size())
            pages.resize(static_cast<std::size_t>(id) + 1);
        std::copy(data.begin(), data.end(), pages[id].begin());
    }
    void publishInitial(std::span<const PageBytes> initial) override {
        if (!pages.empty())
            throw DbError(ErrorCode::InvalidArgument, "nonempty bootstrap");
        pages.assign(initial.begin(), initial.end());
    }
    void sync() override { trace.push_back("sync"); }
    std::uint32_t pageCountOnDisk() const override {
        return static_cast<std::uint32_t>(pages.size());
    }
    std::uint64_t byteSize() const override { return pages.size() * PAGE_SIZE; }
};
class MemoryWal : public IWalFile {
  public:
    std::vector<std::byte> bytes;
    std::vector<std::byte> readAll() override { return bytes; }
    void append(std::span<const std::byte> data) override {
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void sync() override {}
    void reset(std::span<const std::byte> header) override {
        bytes.assign(header.begin(), header.end());
    }
};
} // namespace rohandb::testing
