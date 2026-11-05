#pragma once
#include <list>
#include <optional>
#include <rohandb/common/types.hpp>
#include <unordered_map>
namespace rohandb {
class LruReplacer {
  public:
    void setEvictable(FrameId frame, bool evictable) {
        const auto it = pos_.find(frame);
        if (!evictable) {
            if (it != pos_.end()) {
                lru_.erase(it->second);
                pos_.erase(it);
            }
        } else if (it == pos_.end()) {
            lru_.push_back(frame);
            pos_.emplace(frame, std::prev(lru_.end()));
        }
    }
    void recordAccess(FrameId frame) {
        const auto it = pos_.find(frame);
        if (it != pos_.end())
            lru_.splice(lru_.end(), lru_, it->second);
    }
    std::optional<FrameId> candidate() const {
        return lru_.empty() ? std::nullopt : std::optional(lru_.front());
    }
    std::optional<FrameId> evict() {
        const auto frame = candidate();
        if (frame)
            setEvictable(*frame, false);
        return frame;
    }
    std::size_t size() const { return lru_.size(); }

  private:
    std::list<FrameId> lru_;
    std::unordered_map<FrameId, std::list<FrameId>::iterator> pos_;
};
} // namespace rohandb
