#pragma once
#include <rohandb/index/bplus_tree.hpp>
namespace rohandb {
// Borrowed iterator: engine must outlive it. Any put/erase invalidates it.
class RangeIterator {
  public:
    RangeIterator() = default;
    RangeIterator(RangeIterator &&) noexcept = default;
    RangeIterator &operator=(RangeIterator &&) noexcept = default;
    RangeIterator(const RangeIterator &) = delete;
    RangeIterator &operator=(const RangeIterator &) = delete;
    bool valid() const;
    std::uint64_t key() const;
    RecordId rid() const;
    std::string value() const;
    void next();

  private:
    friend class BPlusTree;
    friend class StorageEngine;
    RangeIterator(BPlusTree &tree, std::uint64_t key);
    void advanceEmpty();
    void checkGeneration() const;
    BPlusTree *tree_ = nullptr;
    RecordManager *records_ = nullptr;
    PageGuard leaf_;
    BTreeNode node_;
    std::size_t index_ = 0;
    std::uint64_t generation_ = 0;
    bool end_ = true;
};
} // namespace rohandb
