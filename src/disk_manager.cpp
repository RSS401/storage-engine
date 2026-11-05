#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <rohandb/common/error.hpp>
#include <rohandb/disk_manager.hpp>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace rohandb {
namespace {
[[noreturn]] void ioError(const char *operation) {
    throw DbError(ErrorCode::IoError, std::string(operation) + ": " + std::strerror(errno));
}
off_t offsetOf(PageId id) {
    if (id == INVALID_PAGE_ID)
        throw DbError(ErrorCode::InvalidArgument, "invalid page ID");
    const auto offset = static_cast<std::uint64_t>(id) * PAGE_SIZE;
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - PAGE_SIZE)
        throw DbError(ErrorCode::InvalidArgument, "page offset out of range");
    return static_cast<off_t>(offset);
}
} // namespace
DiskManager::DiskManager(const std::filesystem::path &path, bool create, bool read_only)
    : path_(path), read_only_(read_only) {
    fd_ = ::open(path.c_str(), (read_only ? O_RDONLY : O_RDWR) | (create ? O_CREAT : 0) | O_CLOEXEC,
                 0600);
    if (fd_ < 0)
        ioError("open database");
}
std::unique_ptr<DiskManager> DiskManager::open(const std::filesystem::path &path, bool create) {
    return std::make_unique<DiskManager>(path, create);
}
DiskManager::~DiskManager() {
    if (fd_ >= 0)
        ::close(fd_);
}
std::uint64_t DiskManager::byteSize() const {
    struct stat info{};
    if (::fstat(fd_, &info) != 0)
        ioError("stat database");
    return static_cast<std::uint64_t>(info.st_size);
}
std::uint32_t DiskManager::pageCountOnDisk() const {
    const auto bytes = byteSize();
    const auto count = bytes / PAGE_SIZE + (bytes % PAGE_SIZE != 0 ? 1U : 0U);
    if (count > INVALID_PAGE_ID)
        throw DbError(ErrorCode::CorruptPage, "database exceeds address space");
    return static_cast<std::uint32_t>(count);
}
void DiskManager::readPage(PageId id, PageView out) {
    const auto offset = offsetOf(id);
    if (static_cast<std::uint64_t>(offset) >= byteSize())
        throw DbError(ErrorCode::PageNotOnDisk, "page is beyond end of file");
    std::size_t done = 0;
    while (done < PAGE_SIZE) {
        const auto n =
            ::pread(fd_, out.data() + done, PAGE_SIZE - done, offset + static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ioError("read page");
        }
        if (n == 0)
            throw DbError(ErrorCode::ShortRead, "incomplete page");
        done += static_cast<std::size_t>(n);
    }
}
void DiskManager::writePage(PageId id, ConstPageView data) {
    const auto offset = offsetOf(id);
    std::size_t done = 0;
    while (done < PAGE_SIZE) {
        const auto n =
            ::pwrite(fd_, data.data() + done, PAGE_SIZE - done, offset + static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ioError("write page");
        }
        if (n == 0)
            throw DbError(ErrorCode::IoError, "write made no progress");
        done += static_cast<std::size_t>(n);
    }
}
void DiskManager::sync() {
#ifdef __APPLE__
    if (::fcntl(fd_, F_FULLFSYNC) < 0)
        ioError("full sync database");
#else
    if (::fsync(fd_) < 0)
        ioError("sync database");
#endif
}
void DiskManager::publishInitial(std::span<const PageBytes> pages) {
    if (read_only_ || byteSize() != 0 || pages.empty())
        throw DbError(ErrorCode::InvalidArgument, "bootstrap requires an empty writable database");
    auto pattern = path_.string() + ".bootstrap.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int temporary = ::mkstemp(name.data());
    if (temporary < 0)
        ioError("create bootstrap file");
    try {
        for (const auto &page : pages) {
            std::size_t done = 0;
            while (done < PAGE_SIZE) {
                const auto count = ::write(temporary, page.data() + done, PAGE_SIZE - done);
                if (count < 0) {
                    if (errno == EINTR)
                        continue;
                    ioError("write bootstrap");
                }
                if (count == 0)
                    throw DbError(ErrorCode::IoError, "bootstrap write made no progress");
                done += static_cast<std::size_t>(count);
            }
        }
#ifdef __APPLE__
        if (::fcntl(temporary, F_FULLFSYNC) < 0)
            ioError("sync bootstrap");
#else
        if (::fsync(temporary) < 0)
            ioError("sync bootstrap");
#endif
        if (::rename(name.data(), path_.c_str()) < 0)
            ioError("publish bootstrap");
        const int old = fd_;
        fd_ = temporary;
        temporary = -1;
        ::close(old);
        auto directory = path_.parent_path();
        if (directory.empty())
            directory = ".";
        const int parent = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0)
            ioError("open bootstrap directory");
        const auto result = ::fsync(parent);
        const auto saved = errno;
        ::close(parent);
        errno = saved;
        if (result < 0)
            ioError("sync bootstrap directory");
    } catch (...) {
        if (temporary >= 0)
            ::close(temporary);
        ::unlink(name.data());
        throw;
    }
}
Page DiskManager::read_page(PageId id) {
    Page page;
    readPage(id, PageView(page.data(), PAGE_SIZE));
    return page;
}
void DiskManager::write_page(PageId id, const Page &page) {
    writePage(id, ConstPageView(page.data(), PAGE_SIZE));
}
} // namespace rohandb
