#pragma once

// 書籤子系統的門面（WBS 9）。
//
// 把「開檔 → 讀出可寫入的書籤樹 → 套用批次操作 → 寫回」串成一條路。存在的
// 理由是批次操作幾乎都要同時碰三樣東西：書籤樹、命名目標表、頁數。分散在
// 呼叫端各自組裝的話，最容易漏的是「頁數」——沒有它就無法驗證目標，
// 而沒驗證的批次操作會產出一堆指向不存在頁面的書籤（PRD-BM-019 要抓的正是這個）。
//
// 這個類別**不持有執行緒也不碰 PDFium**：書籤的讀寫是純位元組運算，
// 不需要文件把手，因此也不受「PDFium 非執行緒安全」的約束。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/bookmark_ops.h"
#include "engine/bookmarks/destination_codec.h"
#include "engine/bookmarks/outline_writer.h"
#include "engine/bookmarks/toc_page_builder.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::bookmarks {

class BookmarkDocument {
public:
    // 開啟原檔位元組。加密文件會在這裡被擋下（ADR-002 的立場）。
    [[nodiscard]] objects::SourceStatus open(std::string bytes, std::string* diagnostic = nullptr);

    [[nodiscard]] bool isOpen() const noexcept { return open_; }
    [[nodiscard]] std::int32_t pageCount() const noexcept { return pageCount_; }

    // 讀出來的書籤樹。批次操作直接改這棵樹，改完呼叫 commitOutline()。
    [[nodiscard]] domain::bookmarks::BookmarkTree& tree() noexcept { return tree_; }
    [[nodiscard]] const domain::bookmarks::BookmarkTree& tree() const noexcept { return tree_; }

    [[nodiscard]] const std::vector<domain::bookmarks::NamedDestination>& namedDestinations()
        const noexcept {
        return namedDestinations_;
    }

    // 讀取時被截斷的節點數（原檔的書籤鏈有迴圈或過深）。
    [[nodiscard]] std::size_t truncatedOnRead() const noexcept { return truncated_; }

    // PRD-BM-019。頁數與已知的命名目標由本類別自動帶入，呼叫端不需要（也不該）
    // 自行湊那兩個參數——湊錯的後果是驗證通過但檔案仍然是壞的。
    [[nodiscard]] std::vector<domain::bookmarks::ValidationIssue> validate(int maxDepth = 8) const;

    // 把目前的樹寫回 /Outlines。
    [[nodiscard]] OutlineWriteResult commitOutline();

    // PRD-BM-006：把樹裡的直接目標轉成命名目標並寫進 /Names /Dests。
    [[nodiscard]] NamedDestinationWriteResult commitNamedDestinations(
        const std::vector<domain::bookmarks::NamedDestination>& destinations, bool merge = true);

    // PRD-BM-005 / 008。
    [[nodiscard]] TocBuildResult buildTableOfContents(const TocBuildOptions& options = {});

    // PRD-BM-018。回傳的診斷為空代表成功。
    [[nodiscard]] std::string reorderPagesByBookmarks();

    [[nodiscard]] objects::IncrementalAppender& appender() noexcept { return appender_; }
    [[nodiscard]] objects::BuildResult build() const { return appender_.build(); }

private:
    objects::IncrementalAppender appender_;
    domain::bookmarks::BookmarkTree tree_;
    std::vector<domain::bookmarks::NamedDestination> namedDestinations_;
    std::int32_t pageCount_{0};
    std::size_t truncated_{0};
    bool open_{false};
};

}  // namespace alioth::engine::bookmarks
