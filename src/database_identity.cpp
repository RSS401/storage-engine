#include <algorithm>
#include <random>
#include <rohandb/storage/database_identity.hpp>
namespace rohandb {
DatabaseId newDatabaseId() {
    std::random_device random;
    DatabaseId id{};
    do {
        for (std::size_t i = 0; i < id.size(); i += 4)
            writeScalar<std::uint32_t>(id, i, random());
    } while (id == DatabaseId{});
    return id;
}
DatabaseId readId(std::span<const std::byte> bytes, std::size_t offset) {
    detail::checkScalarBounds(bytes.size(), offset, 16);
    DatabaseId id;
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), id.size(), id.begin());
    return id;
}
void writeId(std::span<std::byte> bytes, std::size_t offset, const DatabaseId &id) {
    detail::checkScalarBounds(bytes.size(), offset, id.size());
    std::copy(id.begin(), id.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}
PageBytes makeIdentityPage(const DatabaseId &id) {
    PageBytes bytes{};
    writeScalar<std::uint8_t>(bytes, layout::TYPE, identity_layout::TYPE);
    writeScalar<std::uint64_t>(bytes, identity_layout::MAGIC, identity_layout::MAGIC_VALUE);
    writeScalar<std::uint32_t>(bytes, identity_layout::VERSION, layout::VERSION_VALUE);
    writeScalar<std::uint32_t>(bytes, identity_layout::FEATURES, FORMAT_FEATURES);
    writeId(bytes, identity_layout::ID, id);
    // Identity protection is mandatory, including core-only builds.
    writeScalar<std::uint32_t>(bytes, layout::CHECKSUM, crc32c(std::span(bytes).subspan(4)));
    return bytes;
}
DatabaseId readDatabaseId(IDiskManager &disk) {
    PageBytes bytes;
    disk.readPage(IDENTITY_PAGE_ID, bytes);
    if (readScalar<std::uint8_t>(bytes, layout::TYPE) != identity_layout::TYPE ||
        readScalar<std::uint64_t>(bytes, identity_layout::MAGIC) != identity_layout::MAGIC_VALUE ||
        readScalar<std::uint32_t>(bytes, identity_layout::VERSION) != layout::VERSION_VALUE)
        throw DbError(ErrorCode::UnsupportedVersion,
                      "version 4 immutable identity page required; no automatic migration");
    if (readScalar<std::uint32_t>(bytes, layout::CHECKSUM) != crc32c(std::span(bytes).subspan(4)))
        throw DbError(ErrorCode::CorruptPage, "immutable identity checksum mismatch");
    if (readScalar<std::uint32_t>(bytes, identity_layout::FEATURES) != FORMAT_FEATURES)
        throw DbError(ErrorCode::UnsupportedVersion,
                      "identity feature flags do not match this build");
    const auto id = readId(bytes, identity_layout::ID);
    if (id == DatabaseId{})
        throw DbError(ErrorCode::CorruptPage, "empty database identity");
    return id;
}
} // namespace rohandb
