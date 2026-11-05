#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace rohandb {

enum class ErrorCode {
    IoError,
    ShortRead,
    PageNotOnDisk,
    CorruptPage,
    AllFramesPinned,
    RecordTooLarge,
    InvalidArgument,
    BadMagic,
    UnsupportedVersion,
    PageSizeMismatch,
    WalCorrupt,
    EngineFailed
};

class DbError : public std::runtime_error {
  public:
    DbError(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

  private:
    ErrorCode code_;
};

} // namespace rohandb
