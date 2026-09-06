#pragma once

// 取消權杖。可視區一變，過期的渲染任務必須在 16 毫秒內丟棄（PRD-VIEW-003）。
//
// 兩種取消時機：任務還在佇列裡（直接移除），或已經在渲染中
// （由 PDFium 的漸進式渲染回呼輪詢本權杖）。

#include <atomic>
#include <memory>

namespace alioth::engine {

class CancellationToken {
public:
    CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() const noexcept {
        if (flag_) flag_->store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isCancelled() const noexcept {
        return flag_ && flag_->load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool valid() const noexcept { return flag_ != nullptr; }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// 一整批任務的取消來源，例如「捲動後所有舊可視區任務」。
class CancellationSource {
public:
    [[nodiscard]] CancellationToken token() const noexcept { return token_; }
    void cancelAll() noexcept { token_.cancel(); }
    void reset() noexcept { token_ = CancellationToken{}; }

private:
    CancellationToken token_{};
};

}  // namespace alioth::engine
