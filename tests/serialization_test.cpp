#include "test_support.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

#include <rohandb/common/serialization.hpp>

namespace {
using rohandb::testing::check;

template <typename F> void expectCorruptPage(F operation) {
    try {
        operation();
    } catch (const rohandb::DbError &error) {
        check(error.code() == rohandb::ErrorCode::CorruptPage);
        return;
    }
    throw std::runtime_error("expected CorruptPage");
}

template <rohandb::DiskScalar T> void checkWidth() {
    std::array<std::byte, sizeof(T) + 2> bytes{};
    bytes.front() = std::byte{0xaa};
    bytes.back() = std::byte{0xbb};
    const std::array<T, 3> values{T{0}, T{1}, std::numeric_limits<T>::max()};
    for (const auto value : values) {
        rohandb::writeScalar<T>(bytes, 1, value);
        check(rohandb::readScalar<T>(bytes, 1) == value);
        check(bytes.front() == std::byte{0xaa});
        check(bytes.back() == std::byte{0xbb});
    }
}

void testExactBytes() {
    const std::array<std::byte, 8> expected{std::byte{0xef}, std::byte{0xcd}, std::byte{0xab},
                                            std::byte{0x89}, std::byte{0x67}, std::byte{0x45},
                                            std::byte{0x23}, std::byte{0x01}};
    check(rohandb::readScalar<std::uint64_t>(expected, 0) == 0x0123456789abcdefULL);
    check(rohandb::readScalar<std::uint32_t>(expected, 0) == 0x89abcdefU);
    check(rohandb::readScalar<std::uint16_t>(expected, 0) == 0xcdefU);
    check(rohandb::readScalar<std::uint8_t>(expected, 0) == 0xefU);
    std::array<std::byte, 8> actual{};
    rohandb::writeScalar<std::uint64_t>(actual, 0, 0x0123456789abcdefULL);
    check(actual == expected);
}

void testBounds() {
    std::array<std::byte, 4> bytes{};
    rohandb::writeScalar<std::uint16_t>(bytes, 2, 0x1234);
    check(bytes[2] == std::byte{0x34} && bytes[3] == std::byte{0x12});
    check(rohandb::readScalar<std::uint16_t>(bytes, 2) == 0x1234);
    const auto original = bytes;
    for (const auto offset :
         {std::size_t{3}, std::size_t{4}, std::numeric_limits<std::size_t>::max()}) {
        expectCorruptPage([&] { (void)rohandb::readScalar<std::uint16_t>(bytes, offset); });
        expectCorruptPage([&] { rohandb::writeScalar<std::uint16_t>(bytes, offset, 1); });
        check(bytes == original);
    }
    expectCorruptPage([] { (void)rohandb::readScalar<std::uint8_t>({}, 0); });
    expectCorruptPage([] { rohandb::writeScalar<std::uint8_t>({}, 0, 1); });
}
} // namespace

int main() {
    static_assert(!rohandb::DiskScalar<bool>);
    static_assert(!rohandb::DiskScalar<std::int32_t>);
    try {
        checkWidth<std::uint8_t>();
        checkWidth<std::uint16_t>();
        checkWidth<std::uint32_t>();
        checkWidth<std::uint64_t>();
        testExactBytes();
        testBounds();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
