#include <algorithm>
#include <rohandb/index/range_iterator.hpp>
namespace rohandb {
RangeIterator BPlusTree::lowerBound(std::uint64_t key) {
    return RangeIterator(*this, key);
}
RangeIterator::RangeIterator(BPlusTree &tree, std::uint64_t key)
    : tree_(&tree), generation_(tree.generation_), end_(false) {
    const auto id = tree.findLeaf(key);
    node_ = tree.readNode(id);
    leaf_ = tree.pool_.fetchPage(id);
    index_ = static_cast<std::size_t>(std::lower_bound(node_.keys.begin(), node_.keys.end(), key) -
                                      node_.keys.begin());
    advanceEmpty();
}
void RangeIterator::checkGeneration() const {
    if (tree_)
        tree_->pool_.requireHealthy();
    if (tree_ && tree_->generation_ != generation_)
        throw DbError(ErrorCode::InvalidArgument, "iterator invalidated by mutation");
}
bool RangeIterator::valid() const {
    checkGeneration();
    return !end_;
}
std::uint64_t RangeIterator::key() const {
    if (!valid())
        throw DbError(ErrorCode::InvalidArgument, "iterator at end");
    return node_.keys[index_];
}
RecordId RangeIterator::rid() const {
    (void)key();
    return node_.records[index_];
}
std::string RangeIterator::value() const {
    const auto expected = key();
    if (!records_)
        throw DbError(ErrorCode::InvalidArgument, "index iterator has no value accessor");
    auto [stored, value] = decodeRecord(records_->read(rid()));
    if (stored != expected)
        throw DbError(ErrorCode::CorruptPage, "iterator record key mismatch");
    return value;
}
void RangeIterator::advanceEmpty() {
    std::size_t hops = 0;
    while (index_ == node_.keys.size()) {
        if (node_.next == INVALID_PAGE_ID) {
            end_ = true;
            leaf_.release();
            return;
        }
        if (++hops > tree_->meta_.nextPageId())
            throw DbError(ErrorCode::CorruptPage, "cycle in leaf chain");
        auto next = tree_->pool_.fetchPage(node_.next);
        auto decoded = tree_->readNode(node_.next);
        if (!decoded.leaf)
            throw DbError(ErrorCode::CorruptPage, "leaf link points to internal node");
        leaf_ = std::move(next);
        node_ = std::move(decoded);
        index_ = 0;
    }
}
void RangeIterator::next() {
    if (!valid())
        return;
    ++index_;
    advanceEmpty();
}
} // namespace rohandb
