#include "engine/bookmarks/bookmark_document.h"

#include <algorithm>

#include "engine/bookmarks/outline_reader.h"
#include "engine/bookmarks/page_order_writer.h"

namespace alioth::engine::bookmarks {

objects::SourceStatus BookmarkDocument::open(std::string bytes, std::string* diagnostic) {
    open_ = false;
    tree_.clear();
    namedDestinations_.clear();
    truncated_ = 0;
    pageCount_ = 0;

    const objects::SourceStatus status = appender_.open(std::move(bytes), diagnostic);
    if (status != objects::SourceStatus::Ok) return status;

    const PageIndexMap pages(appender_.source());
    pageCount_ = static_cast<std::int32_t>(pages.pageCount());
    namedDestinations_ = readNamedDestinations(appender_.source(), pages);

    const OutlineReadResult read = readOutline(appender_.source());
    tree_ = std::move(read.tree);
    truncated_ = read.truncated;

    open_ = true;
    return objects::SourceStatus::Ok;
}

std::vector<domain::bookmarks::ValidationIssue> BookmarkDocument::validate(int maxDepth) const {
    domain::bookmarks::ValidationOptions options;
    options.pageCount = pageCount_;
    options.maxDepth = maxDepth;
    options.knownDestinationNames.reserve(namedDestinations_.size());
    for (const domain::bookmarks::NamedDestination& entry : namedDestinations_) {
        options.knownDestinationNames.push_back(entry.name);
    }
    return domain::bookmarks::validate(tree_, options);
}

OutlineWriteResult BookmarkDocument::commitOutline() { return writeOutline(appender_, tree_); }

NamedDestinationWriteResult BookmarkDocument::commitNamedDestinations(
    const std::vector<domain::bookmarks::NamedDestination>& destinations, bool merge) {
    const PageIndexMap pages(appender_.source());
    NamedDestinationWriteResult result =
        writeNamedDestinations(appender_, pages, destinations, merge);
    if (!result.ok) return result;

    // 寫完就把本地的清單同步過去，否則接下來的 validate() 會把剛加進去的
    // 命名目標判成「不存在」。
    for (const domain::bookmarks::NamedDestination& entry : destinations) {
        const auto it = std::find_if(namedDestinations_.begin(), namedDestinations_.end(),
                                     [&entry](const domain::bookmarks::NamedDestination& existing) {
                                         return existing.name == entry.name;
                                     });
        if (it == namedDestinations_.end()) {
            namedDestinations_.push_back(entry);
        } else {
            it->destination = entry.destination;
        }
    }
    return result;
}

TocBuildResult BookmarkDocument::buildTableOfContents(const TocBuildOptions& options) {
    return alioth::engine::bookmarks::buildTableOfContents(appender_, tree_, options);
}

std::string BookmarkDocument::reorderPagesByBookmarks() {
    const domain::bookmarks::PageOrderResult order =
        domain::bookmarks::pageOrderFromBookmarks(tree_, pageCount_);
    if (order.order.empty()) return "沒有可用的頁序（文件沒有頁面）";
    // 順序沒變時不寫入：附加一段什麼都沒改的更新只會讓檔案變大，
    // 而且會讓「檔案有沒有被改過」的判斷失準。
    if (order.identity) return {};
    const PageReorderResult result = reorderPages(appender_, order.order);
    return result.ok ? std::string{} : result.diagnostic;
}

}  // namespace alioth::engine::bookmarks
