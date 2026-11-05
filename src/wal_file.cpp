#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <rohandb/common/error.hpp>
#include <rohandb/wal/wal_file.hpp>
#include <sys/stat.h>
#include <unistd.h>
namespace rohandb {
namespace {
[[noreturn]] void error(const char *op) {
    throw DbError(ErrorCode::IoError, std::string(op) + ": " + std::strerror(errno));
}
void force(int fd) {
#ifdef __APPLE__
    if (::fcntl(fd, F_FULLFSYNC) < 0)
        error("sync WAL");
#else
    if (::fsync(fd) < 0)
        error("sync WAL");
#endif
}
void writeAll(int fd, std::span<const std::byte> data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const auto n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            error("write WAL");
        }
        if (n == 0)
            throw DbError(ErrorCode::IoError, "WAL write made no progress");
        done += static_cast<std::size_t>(n);
    }
}
void syncDirectory(const std::filesystem::path &path) {
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        error("open WAL directory");
    const auto result = ::fsync(fd);
    const auto saved = errno;
    ::close(fd);
    errno = saved;
    if (result < 0)
        error("sync WAL directory");
}
} // namespace
WalFile::WalFile(std::filesystem::path path) : path_(std::move(path)) {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd_ < 0)
        error("open WAL");
}
WalFile::~WalFile() {
    if (fd_ >= 0)
        ::close(fd_);
}
std::vector<std::byte> WalFile::readAll() {
    struct stat info{};
    if (::fstat(fd_, &info) < 0)
        error("stat WAL");
    std::vector<std::byte> bytes(static_cast<std::size_t>(info.st_size));
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n =
            ::pread(fd_, bytes.data() + done, bytes.size() - done, static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            error("read WAL");
        }
        if (n == 0)
            throw DbError(ErrorCode::IoError, "short WAL read");
        done += static_cast<std::size_t>(n);
    }
    return bytes;
}
void WalFile::append(std::span<const std::byte> data) {
    writeAll(fd_, data);
}
void WalFile::sync() {
    force(fd_);
}
void WalFile::reset(std::span<const std::byte> header) {
    // Atomic replacement avoids a crash window with an empty or torn WAL header.
    auto pattern = path_.string() + ".reset.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int temporary = ::mkstemp(name.data());
    if (temporary < 0)
        error("create replacement WAL");
    try {
        writeAll(temporary, header);
        force(temporary);
        if (::rename(name.data(), path_.c_str()) < 0)
            error("replace WAL");
        const int old = fd_;
        fd_ = temporary;
        temporary = -1;
        ::close(old);
        if (::fcntl(fd_, F_SETFL, O_APPEND) < 0)
            error("set WAL append mode");
        syncDirectory(path_);
    } catch (...) {
        if (temporary >= 0)
            ::close(temporary);
        ::unlink(name.data());
        throw;
    }
}
} // namespace rohandb
