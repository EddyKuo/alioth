#pragma once

// 圖磚 LRU 快取。預設上限 256 MB，可設 128–1024 MB（PRD §4.3）。
//
// 加密文件的圖磚永遠不落地（PRD §8.2），因此本快取只存在記憶體中，
// 未來加入磁碟層時必須以 allowDiskSpill 明確區分。

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "domain/tile.h"
#include "engine/pixel_buffer.h"

namespace alioth::engine {

inline constexpr std::size_t kDefaultCacheBytes = 256ull * 1024 * 1024;
inline constexpr std::size_t kMinCacheBytes = 128ull * 1024 * 1024;
inline constexpr std::size_t kMaxCacheBytes = 1024ull * 1024 * 1024;

struct CacheStats {
    std::size_t hits{0};
    std::size_t misses{0};
    std::size_t evictions{0};
    std::size_t bytes{0};
    std::size_t entries{0};

    [[nodiscard]] double hitRate() const noexcept {
        const std::size_t total = hits + misses;
        return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }
};

class TileCache {
public:
    explicit TileCache(std::size_t capacityBytes = kDefaultCacheBytes);

    [[nodiscard]] PixelBufferPtr find(const domain::TileKey& key);
    void insert(const domain::TileKey& key, PixelBufferPtr buffer);

    // 丟棄其他縮放倍率的圖磚。縮放停止後呼叫，避免過場用的粗略倍率長期佔住預算。
    void evictOtherScales(std::int32_t keepScaleKey);
    void clear();

    void setCapacityBytes(std::size_t bytes);
    [[nodiscard]] std::size_t capacityBytes() const;
    [[nodiscard]] CacheStats stats() const;

private:
    struct Entry {
        domain::TileKey key;
        PixelBufferPtr buffer;
    };

    using EntryList = std::list<Entry>;

    void evictToFit();

    mutable std::mutex mutex_;
    std::size_t capacityBytes_;
    std::size_t usedBytes_{0};
    EntryList lru_;  // 前端為最近使用
    std::unordered_map<domain::TileKey, EntryList::iterator> index_;
    CacheStats stats_{};
};

}  // namespace alioth::engine
