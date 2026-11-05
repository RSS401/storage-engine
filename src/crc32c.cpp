#include <cstring>
#include <rohandb/common/crc32c.hpp>
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#endif
namespace rohandb {
namespace {
constexpr auto makeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        auto value = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0x82f63b78U : 0U);
        table[i] = value;
    }
    return table;
}
constexpr auto table = makeTable();
} // namespace
std::uint32_t crc32cSoftware(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (auto byte : bytes)
        crc = table[(crc ^ std::to_integer<std::uint32_t>(byte)) & 0xffU] ^ (crc >> 8);
    return ~crc;
}
// Same polynomial and reflection as the table; the instructions consume little-endian words.
std::uint32_t crc32c(std::span<const std::byte> bytes) {
#if defined(__ARM_FEATURE_CRC32) || (defined(__SSE4_2__) && defined(__x86_64__))
    std::uint32_t crc = 0xffffffffU;
    const auto *data = bytes.data();
    auto size = bytes.size();
    for (; size >= 8; data += 8, size -= 8) {
        std::uint64_t word;
        std::memcpy(&word, data, 8);
#if defined(__ARM_FEATURE_CRC32)
        crc = __crc32cd(crc, word);
#else
        crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, word));
#endif
    }
    for (; size > 0; ++data, --size) {
#if defined(__ARM_FEATURE_CRC32)
        crc = __crc32cb(crc, std::to_integer<std::uint8_t>(*data));
#else
        crc = _mm_crc32_u8(crc, std::to_integer<std::uint8_t>(*data));
#endif
    }
    return ~crc;
#else
    return crc32cSoftware(bytes);
#endif
}
void stampChecksum(PageView page) {
#ifdef ROHANDB_CHECKSUMS
    writeScalar<std::uint32_t>(page, layout::CHECKSUM, crc32c(page.subspan(4)));
#else
    writeScalar<std::uint32_t>(page, layout::CHECKSUM, 0);
#endif
}
void verifyChecksum(ConstPageView page) {
#ifdef ROHANDB_CHECKSUMS
    if (readScalar<std::uint32_t>(page, layout::CHECKSUM) != crc32c(page.subspan(4)))
        throw DbError(ErrorCode::CorruptPage, "page checksum mismatch");
#else
    (void)page;
#endif
}
} // namespace rohandb
