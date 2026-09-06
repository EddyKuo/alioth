#pragma once

// 朗讀控制器（Read Out Loud，PRD-A11Y-006）。
//
// 把「文字擷取」（engine::objects::extractReadingText，純函數，見引擎轉接層）
// 與「語音合成」（platform::SpeechSynthesizer，Windows 走 SAPI）接起來，
// 並負責跨頁：一頁念完自動翻到下一頁繼續念，直到文件結尾或使用者停止。
//
// 用輪詢而不是 SAPI 的事件通知（ISpNotifySink）：後者需要另外處理 COM 的
// callback 執行緒與 Qt 事件迴圈的交接，輪詢在使用者可感知的時間尺度上
// （念一句話至少要一兩秒）完全夠用，換來的是簡單很多的狀態機。

#include <QObject>
#include <QString>

#include <cstdint>
#include <vector>

#include "engine/objects/reading_text_extractor.h"
#include "engine/objects/struct_tree_reader.h"
#include "platform/speech_synthesis.h"

class QTimer;

namespace alioth::app {

class ReadAloudController : public QObject {
    Q_OBJECT

public:
    explicit ReadAloudController(QObject* parent = nullptr);
    ~ReadAloudController() override;

    // 這台機器上有沒有可用的朗讀後端。UI 在提供這個功能前應該先問過這個，
    // 不可用時要明確停用或提示，而不是讓使用者按了沒反應。
    [[nodiscard]] bool available() const;

    // 從 fromPageIndex 開始朗讀，一路念到 pageCount - 1 或使用者停止為止。
    // tree 必須是呼叫端已經讀出的結構樹（與 Tags／Order 面板共用同一份，
    // 見 ui/main_window.cpp 的 reloadNavigationPanels）。
    void start(engine::objects::StructTree tree, std::int32_t fromPageIndex,
              std::int32_t pageCount);
    void stop();
    void togglePause();

    [[nodiscard]] bool isActive() const noexcept { return active_; }
    [[nodiscard]] bool isPaused() const noexcept { return paused_; }

signals:
    // 目前正在朗讀哪一頁；UI 用來同步捲動位置。
    void pageChanged(int pageIndex);
    // 念到文件結尾（或找不到更多可朗讀的頁面）。
    void finished();
    // 沒有可用的朗讀後端。reason 已經是可以直接顯示給使用者的文字。
    void unavailable(QString reason);

private:
    void loadPage(std::int32_t pageIndex);
    void advance();

    platform::SpeechSynthesizer speech_;
    engine::objects::StructTree tree_;
    std::int32_t pageCount_{0};
    std::int32_t currentPage_{-1};
    std::vector<engine::objects::ReadingUtterance> pending_;
    std::size_t cursor_{0};
    bool active_{false};
    bool paused_{false};
    QTimer* pollTimer_{nullptr};
};

}  // namespace alioth::app
