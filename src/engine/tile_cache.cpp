#include "engine/tile_cache.h"

#include <algorithm>

namespace alioth::engine {

TileCache::TileCache(std::size_t capacityBytes)
    : capacityBytes_(std::clamp(capacityBytes, kMinCacheBytes, kMaxCacheBytes)) {}

PixelBufferPtr TileCache::find(const domain::TileKey& key) {
    std::lock_guard lock(mutex_);
    const auto it = index_.find(key);
    if (it == index_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second);
    ++stats_.hits;
    return it->second->buffer;
}

void TileCache::insert(const domain::TileKey& key, PixelBufferPtr buffer) {
    if (!buffer) return;

    std::lock_guard lock(mutex_);
    if (const auto it = index_.find(key); it != index_.end()) {
        usedBytes_ -= it->second->buffer->sizeBytes();
        it->second->buffer = std::move(buffer);
        usedBytes_ += it->second->buffer->sizeBytes();
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }

    usedBytes_ += buffer->sizeBytes();
    lru_.push_front(Entry{key, std::move(buffer)});
    index_.emplace(key, lru_.begin());
    evictToFit();
}

void TileCache::evictToFit() {
    while (usedBytes_ > capacityBytes_ && lru_.size() > 1) {
        const Entry& victim = lru_.back();
        usedBytes_ -= victim.buffer->sizeBytes();
        index_.erase(victim.key);
        lru_.pop_back();
        ++stats_.evictions;
    }
}

void TileCache::evictOtherScales(std::int32_t keepScaleKey) {
    std::lock_guard lock(mutex_);
    for (auto it = lru_.begin(); it != lru_.end();) {
        if (it->key.scaleKey != keepScaleKey) {
            usedBytes_ -= it->buffer->sizeBytes();
            index_.erase(it->key);
            it = lru_.erase(it);
            ++stats_.evictions;
        } else {
            ++it;
        }
    }
}

void TileCache::clear() {
    std::lock_guard lock(mutex_);
    lru_.clear();
    index_.clear();
    usedBytes_ = 0;
}

void TileCache::setCapacityBytes(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    capacityBytes_ = std::clamp(bytes, kMinCacheBytes, kMaxCacheBytes);
    evictToFit();
}

std::size_t TileCache::capacityBytes() const {
    std::lock_guard lock(mutex_);
    return capacityBytes_;
}

CacheStats TileCache::stats() const {
    std::lock_guard lock(mutex_);
    CacheStats out = stats_;
    out.bytes = usedBytes_;
    out.entries = lru_.size();
    return out;
}

}  // namespace alioth::engine
