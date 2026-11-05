#pragma once
#include <filesystem>
#include <memory>
#include <rohandb/common/types.hpp>
#include <rohandb/page.hpp>
namespace rohandb {
// Pure page I/O. Allocation, format validation, and checksums belong above this interface.
class IDiskManager {
  public:
    virtual ~IDiskManager() = default;
    virtual void readPage(PageId id, PageView out) = 0;
    virtual void writePage(PageId id, ConstPageView data) = 0;
    virtual void sync() = 0;
    // Publish all bootstrap pages atomically; never expose a partially initialized database.
    virtual void publishInitial(std::span<const PageBytes> pages) = 0;
    virtual std::uint32_t pageCountOnDisk() const = 0;
    virtual std::uint64_t byteSize() const = 0;
};
class DiskManager final : public IDiskManager {
  public:
    explicit DiskManager(const std::filesystem::path &path, bool create = true,
                         bool read_only = false);
    static std::unique_ptr<DiskManager> open(const std::filesystem::path &path, bool create);
    ~DiskManager() override;
    DiskManager(const DiskManager &) = delete;
    DiskManager &operator=(const DiskManager &) = delete;
    void readPage(PageId id, PageView out) override;
    void writePage(PageId id, ConstPageView data) override;
    void sync() override;
    void publishInitial(std::span<const PageBytes> pages) override;
    std::uint32_t pageCountOnDisk() const override;
    std::uint64_t byteSize() const override;
    // Compatibility with the original learning API.
    Page read_page(PageId id);
    void write_page(PageId id, const Page &page);

  private:
    int fd_ = -1;
    std::filesystem::path path_;
    bool read_only_ = false;
};
} // namespace rohandb
