#pragma once

#include <array>
#include <cstddef>
#include <rohandb/common/types.hpp>

namespace rohandb {

class Page {
  public:
    Page() = default;

    std::byte *data() noexcept;

    const std::byte *data() const noexcept;

    std::size_t size() const noexcept;

  private:
    std::array<std::byte, PAGE_SIZE> data_{};
};

inline std::byte *Page::data() noexcept {
    return data_.data();
}

inline const std::byte *Page::data() const noexcept {
    return data_.data();
}

inline std::size_t Page::size() const noexcept {
    return data_.size();
}

} // namespace rohandb
