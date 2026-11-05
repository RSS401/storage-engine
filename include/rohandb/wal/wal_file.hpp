#pragma once
#include <filesystem>
#include <rohandb/common/types.hpp>
#include <vector>
namespace rohandb {
class IWalFile {
  public:
    virtual ~IWalFile() = default;
    virtual std::vector<std::byte> readAll() = 0;
    virtual void append(std::span<const std::byte> data) = 0;
    virtual void sync() = 0;
    // Replace with a complete header atomically; must preserve old file on failure before rename.
    virtual void reset(std::span<const std::byte> header) = 0;
};
class WalFile final : public IWalFile {
  public:
    explicit WalFile(std::filesystem::path path);
    ~WalFile() override;
    std::vector<std::byte> readAll() override;
    void append(std::span<const std::byte> data) override;
    void sync() override;
    void reset(std::span<const std::byte> header) override;

  private:
    std::filesystem::path path_;
    int fd_ = -1;
};
} // namespace rohandb
