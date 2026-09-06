#include "app/read_aloud_controller.h"

#include <QTimer>

#include <utility>

namespace alioth::app {

ReadAloudController::ReadAloudController(QObject* parent) : QObject(parent) {
    pollTimer_ = new QTimer(this);
    // 150 毫秒：比使用者能感知的語句間隔短得多，但不會頻繁到浪費 CPU
    // 去問一個「一句話要念一兩秒」的狀態。
    pollTimer_->setInterval(150);
    connect(pollTimer_, &QTimer::timeout, this, &ReadAloudController::advance);
}

ReadAloudController::~ReadAloudController() = default;

bool ReadAloudController::available() const { return speech_.available(); }

void ReadAloudController::loadPage(std::int32_t pageIndex) {
    currentPage_ = pageIndex;
    pending_ = engine::objects::extractReadingText(tree_, pageIndex).utterances;
    cursor_ = 0;
    emit pageChanged(pageIndex);
}

void ReadAloudController::start(engine::objects::StructTree tree, std::int32_t fromPageIndex,
                                std::int32_t pageCount) {
    stop();

    if (!speech_.available()) {
        emit unavailable(tr("這台機器上沒有可用的語音合成引擎"));
        return;
    }
    if (fromPageIndex < 0 || pageCount <= 0) return;

    tree_ = std::move(tree);
    pageCount_ = pageCount;
    active_ = true;
    paused_ = false;
    loadPage(fromPageIndex);
    pollTimer_->start();
    advance();  // 立即念第一句，不必等第一次輪詢間隔。
}

void ReadAloudController::stop() {
    active_ = false;
    paused_ = false;
    pending_.clear();
    cursor_ = 0;
    pollTimer_->stop();
    speech_.stop();
}

void ReadAloudController::togglePause() {
    if (!active_) return;
    paused_ = !paused_;
    if (paused_) {
        speech_.pause();
    } else {
        speech_.resume();
    }
}

void ReadAloudController::advance() {
    if (!active_ || paused_) return;
    if (speech_.isSpeaking()) return;  // 上一句還沒念完，下一輪輪詢再看

    if (cursor_ < pending_.size()) {
        // 回傳值不理會：念失敗（例如語音引擎中途被移除）也要繼續往下一句走，
        // 不能讓朗讀卡死在同一句——使用者按停止鍵仍然要能停。
        [[maybe_unused]] const platform::SpeechStatus status =
            speech_.speak(QString::fromStdString(pending_[cursor_].text));
        ++cursor_;
        return;
    }

    // 這一頁能念的都念完了（也可能一句都沒有），找下一個有內容的頁面。
    for (std::int32_t next = currentPage_ + 1; next < pageCount_; ++next) {
        loadPage(next);
        if (!pending_.empty()) return;  // 下一輪輪詢會念到它
    }

    stop();
    emit finished();
}

}  // namespace alioth::app
