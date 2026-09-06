#include "engine/iosec/text_export.h"

#include <QFile>
#include <QIODevice>
#include <QString>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace alioth::engine::iosec {
namespace {

struct State {
    text::TextExtractor* extractor{nullptr};
    std::string outputPath;
    std::string pageBreak;
    std::function<void(TextExportResult)> callback;
    std::vector<std::string> pageTexts;
    std::int32_t pageCount{0};
    std::int32_t cursor{0};
};

void finish(const std::shared_ptr<State>& state) {
    if (state->pageCount == 0) {
        TextExportResult result;
        result.status = TextExportStatus::NoPages;
        result.message = "文件沒有頁面可匯出";
        if (state->callback) state->callback(std::move(result));
        return;
    }

    std::string joined;
    for (std::int32_t i = 0; i < state->pageCount; ++i) {
        joined += state->pageTexts[static_cast<std::size_t>(i)];
        if (i + 1 < state->pageCount) joined += state->pageBreak;
    }

    QFile file(QString::fromStdString(state->outputPath));
    TextExportResult result;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.status = TextExportStatus::WriteFailed;
        result.message = "無法開啟輸出檔：" + file.errorString().toStdString();
        if (state->callback) state->callback(std::move(result));
        return;
    }
    const qint64 written = file.write(joined.data(), static_cast<qint64>(joined.size()));
    file.close();
    if (written != static_cast<qint64>(joined.size())) {
        result.status = TextExportStatus::WriteFailed;
        result.message = "寫入不完整：" + file.errorString().toStdString();
        if (state->callback) state->callback(std::move(result));
        return;
    }

    result.status = TextExportStatus::Ok;
    result.pagesExported = state->pageCount;
    result.message = "已匯出 " + std::to_string(state->pageCount) + " 頁純文字";
    if (state->callback) state->callback(std::move(result));
}

void scheduleNext(const std::shared_ptr<State>& state) {
    if (state->cursor >= state->pageCount) {
        finish(state);
        return;
    }
    const std::int32_t pageIndex = state->cursor++;
    state->extractor->withTextPage(pageIndex, [state, pageIndex](const text::TextPage* page) {
        std::string text;
        if (page != nullptr && page->valid()) {
            text = text::textForRange(*page, text::pageRange(*page));
        }
        state->pageTexts[static_cast<std::size_t>(pageIndex)] = std::move(text);
        scheduleNext(state);
    });
}

}  // namespace

const char* describe(TextExportStatus status) noexcept {
    switch (status) {
        case TextExportStatus::Ok:          return "成功";
        case TextExportStatus::NoPages:     return "沒有頁面可匯出";
        case TextExportStatus::WriteFailed: return "寫檔失敗";
    }
    return "未知狀態";
}

void exportDocumentText(text::TextExtractor& extractor, const std::string& outputPath,
                        std::function<void(TextExportResult)> callback, std::string pageBreak) {
    auto state = std::make_shared<State>();
    state->extractor = &extractor;
    state->outputPath = outputPath;
    state->pageBreak = std::move(pageBreak);
    state->callback = std::move(callback);
    state->pageCount = extractor.pageCount();
    state->pageTexts.resize(static_cast<std::size_t>(std::max(state->pageCount, 0)));
    state->cursor = 0;

    scheduleNext(state);
}

}  // namespace alioth::engine::iosec
