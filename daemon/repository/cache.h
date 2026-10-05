#pragma once
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
namespace aegis::daemon::repository {
template<class T> class ReviewCache final {
public:
    struct Stats { std::size_t entries, bytes, hits, misses; };
    explicit ReviewCache(std::size_t maxBytes = 32 * 1024 * 1024, std::size_t maxEntries = 128) : maxBytes_(maxBytes), maxEntries_(maxEntries) {}
    std::shared_ptr<const T> get(const std::string &key) const {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end()) { ++misses_; return {}; }
        order_.splice(order_.begin(), order_, found->second.position); ++hits_;
        return found->second.value;
    }
    void put(std::string key, T value, std::size_t bytes) const {
        bytes += key.size() + 256;
        if (bytes > maxBytes_) return;
        auto shared = std::make_shared<const T>(std::move(value));
        std::lock_guard lock(mutex_);
        if (const auto found = entries_.find(key); found != entries_.end()) {
            used_ -= found->second.bytes; order_.erase(found->second.position); entries_.erase(found);
        }
        while (!order_.empty() && (used_ + bytes > maxBytes_ || entries_.size() >= maxEntries_)) {
            auto found = entries_.find(order_.back()); used_ -= found->second.bytes;
            entries_.erase(found); order_.pop_back();
        }
        order_.push_front(key); used_ += bytes;
        entries_.emplace(std::move(key), Item{std::move(shared), bytes, order_.begin()});
    }
    Stats stats() const { std::lock_guard lock(mutex_); return {entries_.size(), used_, hits_, misses_}; }
private:
    struct Item { std::shared_ptr<const T> value; std::size_t bytes; std::list<std::string>::iterator position; };
    std::size_t maxBytes_, maxEntries_;
    mutable std::mutex mutex_;
    mutable std::size_t used_ = 0, hits_ = 0, misses_ = 0;
    mutable std::list<std::string> order_;
    mutable std::unordered_map<std::string, Item> entries_;
};
}
