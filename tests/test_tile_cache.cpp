// 圖磚快取測試：預算上限、LRU 順序、縮放層級逐出。
//
// 快取行為直接決定 PRD §8.1 的記憶體上限（500 頁工程圖閒置 ≤ 512 MB），
// 逐出邏輯出錯不會當機，只會安靜地吃光記憶體——所以要測。

#include <QtTest>

#include <memory>

#include "engine/tile_cache.h"

using namespace alioth::domain;
using namespace alioth::engine;

namespace {

PixelBufferPtr makeTile() {
    auto buffer = std::make_shared<PixelBuffer>(kTileSize, kTileSize);
    return buffer;
}

TileKey keyAt(std::int32_t col, std::int32_t row, std::int32_t scaleKey = 1000) {
    return TileKey{0, scaleKey, col, row, Rotation::None, false};
}

constexpr std::size_t kTileBytes =
    static_cast<std::size_t>(kTileSize) * kTileSize * 4;  // 1 MB

}  // namespace

class TestTileCache : public QObject {
    Q_OBJECT

private slots:
    void insertAndFind() {
        TileCache cache(kMinCacheBytes);
        const TileKey key = keyAt(0, 0);
        QVERIFY(cache.find(key) == nullptr);
        cache.insert(key, makeTile());
        QVERIFY(cache.find(key) != nullptr);

        const CacheStats stats = cache.stats();
        QCOMPARE(stats.hits, std::size_t{1});
        QCOMPARE(stats.misses, std::size_t{1});
        QCOMPARE(stats.entries, std::size_t{1});
    }

    void capacityIsClampedToSupportedRange() {
        // 128–1024 MB（PRD §4.3）。給極端值時夾住，而不是照單全收。
        TileCache tiny(1024);
        QCOMPARE(tiny.capacityBytes(), kMinCacheBytes);
        TileCache huge(8ull * 1024 * 1024 * 1024);
        QCOMPARE(huge.capacityBytes(), kMaxCacheBytes);
    }

    void evictsLeastRecentlyUsedWhenOverBudget() {
        const std::size_t capacity = kMinCacheBytes;
        const auto capacityInTiles = static_cast<std::int32_t>(capacity / kTileBytes);
        TileCache cache(capacity);

        for (std::int32_t i = 0; i < capacityInTiles; ++i) {
            cache.insert(keyAt(i, 0), makeTile());
        }
        QCOMPARE(cache.stats().evictions, std::size_t{0});

        // 碰一下第 0 塊讓它變成最近使用，再塞爆一塊：該被丟的是第 1 塊而不是第 0 塊。
        QVERIFY(cache.find(keyAt(0, 0)) != nullptr);
        cache.insert(keyAt(capacityInTiles, 0), makeTile());

        QVERIFY(cache.stats().evictions >= 1);
        QVERIFY(cache.find(keyAt(0, 0)) != nullptr);
        QVERIFY(cache.find(keyAt(1, 0)) == nullptr);
        QVERIFY(cache.stats().bytes <= capacity);
    }

    void evictOtherScalesKeepsCurrentLevel() {
        TileCache cache(kMinCacheBytes);
        cache.insert(keyAt(0, 0, 1000), makeTile());
        cache.insert(keyAt(0, 0, 1331), makeTile());
        cache.insert(keyAt(1, 0, 1331), makeTile());

        cache.evictOtherScales(1331);

        QVERIFY(cache.find(keyAt(0, 0, 1000)) == nullptr);
        QVERIFY(cache.find(keyAt(0, 0, 1331)) != nullptr);
        QVERIFY(cache.find(keyAt(1, 0, 1331)) != nullptr);
    }

    void reinsertSameKeyDoesNotDoubleCount() {
        TileCache cache(kMinCacheBytes);
        const TileKey key = keyAt(5, 5);
        cache.insert(key, makeTile());
        const std::size_t after1 = cache.stats().bytes;
        cache.insert(key, makeTile());
        QCOMPARE(cache.stats().bytes, after1);
        QCOMPARE(cache.stats().entries, std::size_t{1});
    }

    void clearReleasesEverything() {
        TileCache cache(kMinCacheBytes);
        cache.insert(keyAt(0, 0), makeTile());
        cache.clear();
        QCOMPARE(cache.stats().bytes, std::size_t{0});
        QCOMPARE(cache.stats().entries, std::size_t{0});
    }
};

QTEST_APPLESS_MAIN(TestTileCache)
#include "test_tile_cache.moc"
