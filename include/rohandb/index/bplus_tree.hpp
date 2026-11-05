#pragma once
#include <optional>
#include <rohandb/engine/record_manager.hpp>
#include <vector>
namespace rohandb {
// Decoded transient node, never persisted as an object representation.
struct BTreeNode {
    bool leaf = true;
    std::uint16_t max_size = layout::LEAF_CAPACITY;
    PageId next = INVALID_PAGE_ID;
    std::vector<std::uint64_t> keys;
    std::vector<RecordId> records;
    std::vector<PageId> children;
};
class RangeIterator;
// Full structural check of a leaf or internal page; registered as the load-time validator.
void validateNode(ConstPageView bytes, PageId next_page_id);
class BPlusTree {
  public:
    BPlusTree(BufferPoolManager &pool, DatabaseMetaManager &meta, std::uint16_t leaf_max = 0,
              std::uint16_t internal_max = 0);
    void createEmptyRoot();
    RangeIterator lowerBound(std::uint64_t key);
    void invalidateIterators() { ++generation_; }
    std::optional<RecordId> find(std::uint64_t key);
    void insert(std::uint64_t key, RecordId rid);
    // One descent: replaces an existing key's record ID or inserts it. Returns the old ID.
    std::optional<RecordId> upsert(std::uint64_t key, RecordId rid);
    void replace(std::uint64_t key, RecordId rid);
    bool erase(std::uint64_t key);
    void verify(RecordManager &records);
    BTreeNode readNode(PageId id);
    PageId root() const { return meta_.rootPageId(); }

  private:
    friend class RangeIterator;
    std::uint64_t generation_ = 0;
    using Split = std::pair<std::uint64_t, PageId>;
    std::optional<Split> insertAt(PageId id, std::uint64_t key, RecordId rid,
                                  std::optional<RecordId> *replaced);
    void insertFromRoot(std::uint64_t key, RecordId rid, std::optional<RecordId> *replaced);
    PageId findLeaf(std::uint64_t key);
    PageGuard leafFor(std::uint64_t key);
    PageId createNode(const BTreeNode &node);
    void writeNode(PageId id, const BTreeNode &node);
    void encode(PageView bytes, const BTreeNode &node);
    BufferPoolManager &pool_;
    DatabaseMetaManager &meta_;
    std::uint16_t leaf_max_, internal_max_;
};
} // namespace rohandb
