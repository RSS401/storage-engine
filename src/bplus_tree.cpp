#include <algorithm>
#include <functional>
#include <rohandb/index/bplus_tree.hpp>
#include <set>
namespace rohandb {
namespace {
std::size_t position(const std::vector<std::uint64_t> &keys, std::uint64_t key) {
    return static_cast<std::size_t>(std::lower_bound(keys.begin(), keys.end(), key) - keys.begin());
}
std::size_t childPosition(const BTreeNode &node, std::uint64_t key) {
    return static_cast<std::size_t>(std::upper_bound(node.keys.begin(), node.keys.end(), key) -
                                    node.keys.begin());
}
[[noreturn]] void badTree(const char *message) {
    throw DbError(ErrorCode::CorruptPage, message);
}
// Reads entries in place from a page that passed validateNode when it was loaded.
class NodeView {
  public:
    explicit NodeView(ConstPageView bytes) : bytes_(bytes) {
        const auto type = readScalar<std::uint8_t>(bytes_, layout::TYPE);
        if (type != 3 && type != 4)
            badTree("page is not a tree node");
        leaf_ = type == 3;
        size_ = readScalar<std::uint16_t>(bytes_, layout::NODE_SIZE);
        if (!leaf_ && size_ < 2)
            badTree("invalid node size");
    }
    bool leaf() const { return leaf_; }
    std::size_t keyCount() const { return leaf_ ? size_ : size_ - 1U; }
    std::uint64_t key(std::size_t i) const {
        return readScalar<std::uint64_t>(bytes_, leaf_ ? layout::ENTRIES + i * layout::LEAF_ENTRY
                                                       : layout::FIRST_SEPARATOR +
                                                             i * layout::INTERNAL_ENTRY);
    }
    PageId child(std::size_t i) const {
        return readScalar<PageId>(bytes_, i == 0 ? layout::FIRST_CHILD
                                                 : layout::FIRST_SEPARATOR +
                                                       (i - 1) * layout::INTERNAL_ENTRY +
                                                       layout::CHILD_IN_ENTRY);
    }
    RecordId record(std::size_t i) const {
        const auto entry = layout::ENTRIES + i * layout::LEAF_ENTRY;
        return {readScalar<PageId>(bytes_, entry + layout::LEAF_RID_PAGE),
                readScalar<SlotId>(bytes_, entry + layout::LEAF_RID_SLOT)};
    }
    // First index whose key is >= key (or > key when upper).
    std::size_t search(std::uint64_t key, bool upper) const {
        std::size_t low = 0, high = keyCount();
        while (low < high) {
            const auto mid = low + (high - low) / 2;
            const auto probe = this->key(mid);
            if (probe < key || (upper && probe == key))
                low = mid + 1;
            else
                high = mid;
        }
        return low;
    }

  private:
    ConstPageView bytes_;
    bool leaf_ = true;
    std::uint16_t size_ = 0;
};
} // namespace
void validateNode(ConstPageView bytes, PageId next_page_id) {
    const auto type = readScalar<std::uint8_t>(bytes, layout::TYPE);
    if (type != 3 && type != 4)
        badTree("page is not a tree node");
    const bool leaf = type == 3;
    const auto size = readScalar<std::uint16_t>(bytes, layout::NODE_SIZE);
    const auto max_size = readScalar<std::uint16_t>(bytes, layout::NODE_MAX);
    if (max_size < 4 || max_size > (leaf ? layout::LEAF_CAPACITY : layout::INTERNAL_CAPACITY) ||
        size > max_size || (!leaf && size < 2))
        badTree("invalid node size");
    const auto validId = [&](PageId page) { return page >= 2 && page < next_page_id; };
    const NodeView node(bytes);
    if (leaf) {
        const auto next = readScalar<PageId>(bytes, layout::NEXT_LEAF);
        if (next != INVALID_PAGE_ID && !validId(next))
            badTree("invalid leaf link");
        for (std::size_t i = 0; i < size; ++i)
            if (!validId(node.record(i).page_id))
                badTree("invalid record page");
    } else {
        for (std::size_t i = 0; i < size; ++i)
            if (!validId(node.child(i)))
                badTree("invalid child page");
    }
    for (std::size_t i = 1; i < node.keyCount(); ++i)
        if (node.key(i - 1) >= node.key(i))
            badTree("unordered node keys");
}
BPlusTree::BPlusTree(BufferPoolManager &pool, DatabaseMetaManager &meta, std::uint16_t leaf_max,
                     std::uint16_t internal_max)
    : pool_(pool), meta_(meta), leaf_max_(leaf_max == 0 ? layout::LEAF_CAPACITY : leaf_max),
      internal_max_(internal_max == 0 ? layout::INTERNAL_CAPACITY : internal_max) {
    pool_.setValidator(layout::PageType::Leaf, validateNode);
    pool_.setValidator(layout::PageType::Internal, validateNode);
    if (leaf_max_ < 4 || leaf_max_ > layout::LEAF_CAPACITY || internal_max_ < 4 ||
        internal_max_ > layout::INTERNAL_CAPACITY)
        throw DbError(ErrorCode::InvalidArgument, "invalid B+ tree capacity");
}
void BPlusTree::encode(PageView bytes, const BTreeNode &node) {
    // Preserve common fields (including future LSN) when replacing an existing node.
    std::fill(bytes.begin() + layout::COMMON_SIZE, bytes.end(), std::byte{0});
    writeScalar<std::uint8_t>(
        bytes, layout::TYPE,
        static_cast<std::uint8_t>(node.leaf ? layout::PageType::Leaf : layout::PageType::Internal));
    writeScalar<std::uint16_t>(
        bytes, layout::NODE_SIZE,
        static_cast<std::uint16_t>(node.leaf ? node.keys.size() : node.children.size()));
    writeScalar<std::uint16_t>(bytes, layout::NODE_MAX, node.max_size);
    if (node.leaf) {
        writeScalar<PageId>(bytes, layout::NEXT_LEAF, node.next);
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            const auto entry = layout::ENTRIES + i * layout::LEAF_ENTRY;
            writeScalar<std::uint64_t>(bytes, entry + layout::LEAF_KEY, node.keys[i]);
            writeScalar<PageId>(bytes, entry + layout::LEAF_RID_PAGE, node.records[i].page_id);
            writeScalar<SlotId>(bytes, entry + layout::LEAF_RID_SLOT, node.records[i].slot_id);
        }
    } else {
        writeScalar<PageId>(bytes, layout::FIRST_CHILD, node.children[0]);
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            const auto entry = layout::FIRST_SEPARATOR + i * layout::INTERNAL_ENTRY;
            writeScalar<std::uint64_t>(bytes, entry, node.keys[i]);
            writeScalar<PageId>(bytes, entry + layout::CHILD_IN_ENTRY, node.children[i + 1]);
        }
    }
}
BTreeNode BPlusTree::readNode(PageId id) {
    auto guard = pool_.fetchPage(id);
    const auto bytes = guard.data();
    // Decoding for mutation and verification re-checks the whole node.
    validateNode(bytes, meta_.nextPageId());
    const NodeView view(bytes);
    BTreeNode node;
    node.leaf = view.leaf();
    node.max_size = readScalar<std::uint16_t>(bytes, layout::NODE_MAX);
    node.keys.reserve(view.keyCount());
    for (std::size_t i = 0; i < view.keyCount(); ++i)
        node.keys.push_back(view.key(i));
    if (node.leaf) {
        node.next = readScalar<PageId>(bytes, layout::NEXT_LEAF);
        node.records.reserve(view.keyCount());
        for (std::size_t i = 0; i < view.keyCount(); ++i)
            node.records.push_back(view.record(i));
    } else {
        node.children.reserve(view.keyCount() + 1);
        for (std::size_t i = 0; i <= view.keyCount(); ++i)
            node.children.push_back(view.child(i));
    }
    return node;
}
PageId BPlusTree::createNode(const BTreeNode &node) {
    auto guard = pool_.newPage();
    encode(guard.mutableData(), node);
    return guard.pageId();
}
void BPlusTree::writeNode(PageId id, const BTreeNode &node) {
    auto guard = pool_.fetchPage(id);
    encode(guard.mutableData(), node);
}
void BPlusTree::createEmptyRoot() {
    BTreeNode node;
    node.max_size = leaf_max_;
    meta_.setRootPageId(createNode(node));
}
PageGuard BPlusTree::leafFor(std::uint64_t key) {
    auto id = root();
    // A valid descent visits each page at most once, so more hops than pages is a cycle.
    for (PageId hops = 0;; ++hops) {
        if (hops > meta_.nextPageId())
            badTree("cycle in tree descent");
        auto guard = pool_.fetchPage(id);
        const NodeView node(guard.data());
        if (node.leaf())
            return guard;
        id = node.child(node.search(key, true));
    }
}
PageId BPlusTree::findLeaf(std::uint64_t key) {
    return leafFor(key).pageId();
}
std::optional<RecordId> BPlusTree::find(std::uint64_t key) {
    const auto guard = leafFor(key);
    const NodeView node(guard.data());
    const auto i = node.search(key, false);
    if (i == node.keyCount() || node.key(i) != key)
        return std::nullopt;
    return node.record(i);
}
void BPlusTree::replace(std::uint64_t key, RecordId rid) {
    const auto id = findLeaf(key);
    auto node = readNode(id);
    const auto i = position(node.keys, key);
    if (i == node.keys.size() || node.keys[i] != key)
        badTree("replace missing key");
    node.records[i] = rid;
    writeNode(id, node);
}
bool BPlusTree::erase(std::uint64_t key) {
    const auto id = findLeaf(key);
    auto node = readNode(id);
    const auto i = position(node.keys, key);
    if (i == node.keys.size() || node.keys[i] != key)
        return false;
    node.keys.erase(node.keys.begin() + static_cast<std::ptrdiff_t>(i));
    node.records.erase(node.records.begin() + static_cast<std::ptrdiff_t>(i));
    writeNode(id, node);
    return true;
}
std::optional<BPlusTree::Split> BPlusTree::insertAt(PageId id, std::uint64_t key, RecordId rid,
                                                    std::optional<RecordId> *replaced) {
    auto node = readNode(id);
    if (node.leaf) {
        // Existing key: rejected (insert) or replaced in place (upsert), before any write.
        if (const auto i = position(node.keys, key); i < node.keys.size() && node.keys[i] == key) {
            if (!replaced)
                throw DbError(ErrorCode::InvalidArgument, "duplicate index key");
            *replaced = node.records[i];
            node.records[i] = rid;
            writeNode(id, node);
            return std::nullopt;
        }
        const auto put = [&](BTreeNode &into) {
            const auto i = position(into.keys, key);
            into.keys.insert(into.keys.begin() + static_cast<std::ptrdiff_t>(i), key);
            into.records.insert(into.records.begin() + static_cast<std::ptrdiff_t>(i), rid);
        };
        if (node.keys.size() < node.max_size) {
            put(node);
            writeNode(id, node);
            return std::nullopt;
        }
        BTreeNode right;
        right.max_size = node.max_size;
        right.next = node.next;
        const auto mid = static_cast<std::ptrdiff_t>(node.keys.size() / 2);
        right.keys.assign(node.keys.begin() + mid, node.keys.end());
        right.records.assign(node.records.begin() + mid, node.records.end());
        node.keys.resize(static_cast<std::size_t>(mid));
        node.records.resize(static_cast<std::size_t>(mid));
        const auto separator = right.keys.front();
        put(key < separator ? node : right);
        node.next = createNode(right);
        writeNode(id, node);
        return Split{separator, node.next};
    }
    const auto child = childPosition(node, key);
    const auto split = insertAt(node.children[child], key, rid, replaced);
    if (!split)
        return std::nullopt;
    const auto put = [&](BTreeNode &into) {
        const auto i = position(into.keys, split->first);
        into.keys.insert(into.keys.begin() + static_cast<std::ptrdiff_t>(i), split->first);
        into.children.insert(into.children.begin() + static_cast<std::ptrdiff_t>(i + 1),
                             split->second);
    };
    if (node.children.size() < node.max_size) {
        put(node);
        writeNode(id, node);
        return std::nullopt;
    }
    const auto mid = node.children.size() / 2;
    const auto separator = node.keys[mid - 1];
    BTreeNode right;
    right.leaf = false;
    right.max_size = node.max_size;
    right.children.assign(node.children.begin() + static_cast<std::ptrdiff_t>(mid),
                          node.children.end());
    right.keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(mid), node.keys.end());
    node.children.resize(mid);
    node.keys.resize(mid - 1);
    put(split->first < separator ? node : right);
    const auto right_id = createNode(right);
    writeNode(id, node);
    return Split{separator, right_id};
}
void BPlusTree::insert(std::uint64_t key, RecordId rid) {
    insertFromRoot(key, rid, nullptr);
}
std::optional<RecordId> BPlusTree::upsert(std::uint64_t key, RecordId rid) {
    std::optional<RecordId> replaced;
    insertFromRoot(key, rid, &replaced);
    return replaced;
}
void BPlusTree::insertFromRoot(std::uint64_t key, RecordId rid,
                               std::optional<RecordId> *replaced) {
    const auto old_root = root();
    if (const auto split = insertAt(old_root, key, rid, replaced)) {
        BTreeNode node;
        node.leaf = false;
        node.max_size = internal_max_;
        node.keys = {split->first};
        node.children = {old_root, split->second};
        meta_.setRootPageId(createNode(node));
    }
}
void BPlusTree::verify(RecordManager &records) {
    std::set<PageId> visited;
    std::vector<std::pair<PageId, PageId>> leaves;
    std::optional<std::size_t> leaf_depth;
    std::function<void(PageId, std::optional<std::uint64_t>, std::optional<std::uint64_t>,
                       std::size_t)>
        walk;
    walk = [&](PageId id, auto low, auto high, std::size_t depth) {
        if (!visited.insert(id).second)
            badTree("tree cycle or shared child");
        const auto node = readNode(id);
        for (auto key : node.keys)
            if ((low && key < *low) || (high && key >= *high))
                badTree("separator bound violation");
        if (node.leaf) {
            if (leaf_depth && *leaf_depth != depth)
                badTree("nonuniform leaf depth");
            leaf_depth = depth;
            leaves.emplace_back(id, node.next);
            for (std::size_t i = 0; i < node.keys.size(); ++i)
                if (decodeRecord(records.read(node.records[i])).first != node.keys[i])
                    badTree("record key mismatch");
        } else {
            for (std::size_t i = 0; i < node.children.size(); ++i)
                walk(node.children[i], i == 0 ? low : std::optional(node.keys[i - 1]),
                     i == node.keys.size() ? high : std::optional(node.keys[i]), depth + 1);
        }
    };
    walk(root(), std::nullopt, std::nullopt, 0);
    for (std::size_t i = 0; i < leaves.size(); ++i)
        if (leaves[i].second != (i + 1 < leaves.size() ? leaves[i + 1].first : INVALID_PAGE_ID))
            badTree("broken leaf chain");
}
} // namespace rohandb
