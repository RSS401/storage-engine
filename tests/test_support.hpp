#pragma once
#include <rohandb/common/error.hpp>
#include <source_location>
#include <stdexcept>
#include <string>
namespace rohandb::testing {
inline void check(bool ok, std::source_location where = std::source_location::current()) {
    if (!ok)
        throw std::runtime_error(std::string(where.file_name()) + ":" +
                                 std::to_string(where.line()) + ": check failed");
}
template <class F>
void expectError(ErrorCode code, F fn,
                 std::source_location where = std::source_location::current()) {
    try {
        fn();
    } catch (const DbError &e) {
        check(e.code() == code, where);
        return;
    }
    check(false, where);
}
} // namespace rohandb::testing
