#include "app/page_operations_service.h"

#include "engine/create/text_to_pdf.h"
#include "engine/pages/page_editor.h"

#include <QFile>
#include <QFileInfo>

#include "engine/pageops/page_boxes.h"
#include "engine/pageops/page_merge.h"
#include "engine/pageops/page_overlay.h"
#include "platform/atomic_file.h"

namespace alioth::app {
namespace {

// 讀整份檔案。頁面操作本來就是全檔重寫，串流讀取在這裡沒有意義。
bool readAll(const QString& path, QByteArray* out, QString* message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (message) *message = QObject::tr("無法讀取檔案：%1").arg(file.errorString());
        return false;
    }
    *out = file.readAll();
    return true;
}

std::string toStd(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

}  // namespace

PageOperationsService::PageOperationsService(QObject* parent) : QObject(parent) {}

PageOperationResult PageOperationsService::writeBack(const QString& path,
                                                     const QByteArray& previous,
                                                     const std::string& bytes, int pageCount,
                                                     const QString& successMessage) {
    PageOperationResult result;

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() || !writer.write(bytes.data(), bytes.size()) || !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.pageCount = pageCount;
    result.previousBytes = previous;
    result.message = successMessage;
    return result;
}

PageOperationResult PageOperationsService::mergePages(const QString& path,
                                                      const std::vector<int>& pages,
                                                      const domain::compose::MergeLayout& layout,
                                                      RewriteConsent) {
    PageOperationResult result;
    QByteArray source;
    if (!readAll(path, &source, &result.message)) return result;

    engine::pageops::MergePagesRequest request;
    request.pages = pages;
    request.layout = layout;

    const auto merged = engine::pageops::mergePages(toStd(source), request);
    if (!merged.ok()) {
        result.message = tr("合併頁面失敗：%1").arg(QString::fromStdString(merged.diagnostic));
        return result;
    }

    return writeBack(path, source, merged.bytes, merged.pageCount,
                     tr("已將 %1 頁合併為一頁").arg(pages.size()));
}

PageOperationResult PageOperationsService::overlay(const QString& path, const QString& overlayPath,
                                                   const domain::compose::OverlayOptions& options,
                                                   RewriteConsent) {
    PageOperationResult result;
    QByteArray base;
    QByteArray over;
    if (!readAll(path, &base, &result.message)) return result;
    if (!readAll(overlayPath, &over, &result.message)) return result;

    engine::pageops::OverlayRequest request;
    request.options = options;
    // 兩份文件頁數相同時逐頁對應，是「把審閱意見疊回原稿」這個主要用法的預期行為。
    request.overlayPageIndex = -1;

    const auto composed = engine::pageops::overlayDocument(toStd(base), toStd(over), request);
    if (!composed.ok()) {
        result.message = tr("疊加失敗：%1").arg(QString::fromStdString(composed.diagnostic));
        return result;
    }

    return writeBack(path, base, composed.bytes, composed.pageCount, tr("已完成疊加"));
}

PageOperationResult PageOperationsService::replacePages(const QString& path,
                                                        const QString& replacementPath,
                                                        int firstPage, int lastPage,
                                                        RewriteConsent) {
    PageOperationResult result;
    if (lastPage < firstPage) {
        result.message = tr("頁面範圍不合法");
        return result;
    }

    QByteArray base;
    QByteArray replacement;
    if (!readAll(path, &base, &result.message)) return result;
    if (!readAll(replacementPath, &replacement, &result.message)) return result;

    engine::pageops::ReplaceRequest request;
    request.firstPage = firstPage;
    request.pageCount = lastPage - firstPage + 1;

    const auto replaced =
        engine::pageops::replacePages(toStd(base), toStd(replacement), request);
    if (!replaced.ok()) {
        result.message = tr("取代頁面失敗：%1").arg(QString::fromStdString(replaced.diagnostic));
        return result;
    }

    return writeBack(path, base, replaced.bytes, replaced.pageCount,
                     tr("已取代 %1 頁，插入 %2 頁")
                         .arg(replaced.removedPages)
                         .arg(replaced.insertedPages));
}

PageOperationResult PageOperationsService::movePages(const QString& path,
                                                    const std::vector<int>& pages,
                                                    int destinationIndex, RewriteConsent) {
    PageOperationResult result;
    if (pages.empty()) {
        result.message = tr("沒有要移動的頁面");
        return result;
    }

    QByteArray source;
    if (!readAll(path, &source, &result.message)) return result;

    engine::pages::PageEditor editor;
    if (!editor.open(path.toStdString(), "")) {
        result.message = tr("無法解析文件結構");
        return result;
    }

    const engine::pages::PageEditResult moved = editor.movePages(pages, destinationIndex);
    if (!moved.ok()) {
        result.message = tr("移動頁面失敗：%1").arg(QString::fromStdString(moved.message));
        return result;
    }

    // 頁面重排必然改動頁面樹，沒辦法以純附加表達，因此走全檔重寫的路徑
    // （與其他 pageops 一致），復原靠把原始位元組寫回去。
    const QString temporary = path + QStringLiteral(".reorder");
    const engine::pages::PageSaveResult saved =
        editor.saveAsCopy(temporary.toStdString(), engine::save::SaveOptions{});
    if (!saved.ok()) {
        QFile::remove(temporary);
        result.message = tr("寫檔失敗");
        return result;
    }

    QByteArray reordered;
    if (!readAll(temporary, &reordered, &result.message)) {
        QFile::remove(temporary);
        return result;
    }
    QFile::remove(temporary);

    return writeBack(path, source, toStd(reordered), editor.pageCount(),
                     tr("已移動 %1 頁").arg(pages.size()));
}

PageOperationResult PageOperationsService::insertPagesFrom(const QString& path,
                                                           const QString& sourcePath, int atIndex,
                                                           const std::vector<int>& sourcePages,
                                                           RewriteConsent) {
    PageOperationResult result;
    QByteArray base;
    QByteArray source;
    if (!readAll(path, &base, &result.message)) return result;
    if (!readAll(sourcePath, &source, &result.message)) return result;

    const auto inserted =
        engine::pageops::insertPagesFrom(toStd(base), toStd(source), atIndex, sourcePages);
    if (!inserted.ok()) {
        result.message = tr("插入頁面失敗：%1").arg(QString::fromStdString(inserted.diagnostic));
        return result;
    }

    return writeBack(path, base, inserted.bytes, inserted.pageCount,
                     tr("已插入 %1 頁").arg(inserted.insertedPages));
}

PageOperationResult PageOperationsService::insertPagesFromText(const QString& path,
                                                               const QString& text, int atIndex,
                                                               RewriteConsent) {
    PageOperationResult result;
    if (text.trimmed().isEmpty()) {
        result.message = tr("沒有要插入的文字");
        return result;
    }

    const engine::create::TextImportResult composed =
        engine::create::createPdfFromPlainText(text.toStdString());
    if (!composed.ok) {
        // 非 ASCII 與版面算不出來是兩種不同的問題，訊息要分開——前者使用者
        // 要知道是字型限制，後者要知道是選項問題。
        result.message = composed.nonAscii
                             ? tr("文字含目前無法排版的字元：%1")
                                   .arg(QString::fromStdString(composed.diagnostic))
                             : tr("排版失敗：%1").arg(QString::fromStdString(composed.diagnostic));
        return result;
    }

    QByteArray base;
    if (!readAll(path, &base, &result.message)) return result;

    const auto inserted =
        engine::pageops::insertPagesFrom(toStd(base), composed.bytes, atIndex, {});
    if (!inserted.ok()) {
        result.message = tr("插入頁面失敗：%1").arg(QString::fromStdString(inserted.diagnostic));
        return result;
    }

    return writeBack(path, base, inserted.bytes, inserted.pageCount,
                     tr("已插入 %1 頁文字").arg(inserted.insertedPages));
}

PageOperationResult PageOperationsService::deletePages(const QString& path,
                                                      const std::vector<int>& pages,
                                                      RewriteConsent) {
    PageOperationResult result;
    if (pages.empty()) {
        result.message = tr("沒有要刪除的頁面");
        return result;
    }

    QByteArray source;
    if (!readAll(path, &source, &result.message)) return result;

    engine::pages::PageEditor editor;
    if (!editor.open(path.toStdString(), "")) {
        result.message = tr("無法解析文件結構");
        return result;
    }

    // 刪光所有頁面會產生一份沒有頁面的 PDF——多數檢視器打不開，而使用者
    // 得到的是一個壞掉的檔案而不是一個空文件。
    if (static_cast<int>(pages.size()) >= editor.pageCount()) {
        result.message = tr("不能刪除全部頁面：文件至少要保留一頁");
        return result;
    }

    const engine::pages::PageEditResult deleted = editor.deletePages(pages);
    if (!deleted.ok()) {
        result.message = tr("刪除頁面失敗：%1").arg(QString::fromStdString(deleted.message));
        return result;
    }

    const QString temporary = path + QStringLiteral(".delete");
    const engine::pages::PageSaveResult saved =
        editor.saveAsCopy(temporary.toStdString(), engine::save::SaveOptions{});
    if (!saved.ok()) {
        QFile::remove(temporary);
        result.message = tr("寫檔失敗");
        return result;
    }

    QByteArray rewritten;
    if (!readAll(temporary, &rewritten, &result.message)) {
        QFile::remove(temporary);
        return result;
    }
    QFile::remove(temporary);

    return writeBack(path, source, toStd(rewritten), editor.pageCount(),
                     tr("已刪除 %1 頁").arg(pages.size()));
}

PageOperationResult PageOperationsService::extractPages(const QString& path,
                                                        const std::vector<int>& pages,
                                                        const QString& targetPath) {
    PageOperationResult result;
    if (pages.empty()) {
        result.message = tr("沒有要擷取的頁面");
        return result;
    }
    if (QFileInfo(targetPath) == QFileInfo(path)) {
        // 擷取的輸出蓋掉來源，等於一次不可復原的刪頁，而使用者以為自己
        // 只是「另存一份」。
        result.message = tr("請選擇與原文件不同的檔名——擷取會另存新檔，不動原檔。");
        return result;
    }

    const engine::pages::AssemblyResult extracted = engine::pages::extractPages(
        path.toStdString(), pages, targetPath.toStdString());
    if (!extracted.ok()) {
        result.message = tr("擷取頁面失敗：%1").arg(QString::fromStdString(extracted.message));
        return result;
    }

    result.ok = true;
    result.pageCount = extracted.pageCount;
    // previousBytes 刻意留空：原檔沒有被動過，沒有東西要復原。
    result.message = tr("已擷取 %1 頁到 %2").arg(extracted.pageCount).arg(targetPath);
    return result;
}

PageOperationResult PageOperationsService::mergeDocuments(const QStringList& sourcePaths,
                                                          const QString& targetPath) {
    PageOperationResult result;
    if (sourcePaths.size() < 2) {
        result.message = tr("至少要選兩份文件才需要合併");
        return result;
    }
    std::vector<std::string> sources;
    sources.reserve(static_cast<std::size_t>(sourcePaths.size()));
    for (const QString& path : sourcePaths) {
        if (QFileInfo(path) == QFileInfo(targetPath)) {
            // 輸出蓋掉其中一份來源，等於在讀取途中把它換掉——結果不可預期。
            result.message = tr("輸出檔名不能與任何一份來源相同");
            return result;
        }
        sources.push_back(path.toStdString());
    }

    const engine::pages::AssemblyResult merged =
        engine::pages::mergeDocuments(sources, targetPath.toStdString());
    if (!merged.ok()) {
        result.message = tr("合併文件失敗：%1").arg(QString::fromStdString(merged.message));
        return result;
    }

    result.ok = true;
    result.pageCount = merged.pageCount;
    // previousBytes 留空：沒有任何來源被改過，沒有東西要復原。
    result.message = tr("已合併 %1 份文件（共 %2 頁）到 %3")
                         .arg(sourcePaths.size())
                         .arg(merged.pageCount)
                         .arg(targetPath);
    return result;
}

PageOperationResult PageOperationsService::splitDocument(const QString& path, int pagesPerChunk,
                                                         const QString& targetPattern) {
    PageOperationResult result;
    if (pagesPerChunk < 1) {
        result.message = tr("每份至少要有一頁");
        return result;
    }

    domain::pages::SplitRule rule;
    rule.mode = domain::pages::SplitMode::EveryNPages;
    rule.pagesPerChunk = pagesPerChunk;

    const engine::pages::SplitResult split = engine::pages::splitDocument(
        path.toStdString(), rule, targetPattern.toStdString());
    if (!split.ok()) {
        result.message = tr("分割文件失敗：%1").arg(QString::fromStdString(split.message));
        return result;
    }

    result.ok = true;
    result.pageCount = static_cast<int>(split.outputs.size());
    result.message = tr("已分割成 %1 個檔案").arg(split.outputs.size());
    return result;
}

PageOperationResult PageOperationsService::cropToContent(const QString& path,
                                                         const std::vector<int>& pages,
                                                         RewriteConsent) {
    PageOperationResult result;
    QByteArray source;
    if (!readAll(path, &source, &result.message)) return result;

    engine::pages::PageEditor editor;
    if (!editor.open(path.toStdString(), "")) {
        result.message = tr("無法解析文件結構");
        return result;
    }

    std::vector<int> targets = pages;
    if (targets.empty()) {
        targets.reserve(static_cast<std::size_t>(editor.pageCount()));
        for (int i = 0; i < editor.pageCount(); ++i) targets.push_back(i);
    }

    int cropped = 0;
    const engine::pages::PageEditResult result2 =
        editor.cropToContent(targets, engine::pages::ContentBoundsOptions{}, &cropped);
    if (!result2.ok()) {
        result.message = tr("裁切失敗：%1").arg(QString::fromStdString(result2.message));
        return result;
    }
    if (cropped == 0) {
        // 一頁都沒裁到代表整份文件都是空白（或偵測不到內容）。回報成功卻
        // 什麼都沒變，使用者會以為裁切壞掉了。
        result.message = tr("沒有偵測到可裁切的內容邊界，未做變更");
        return result;
    }

    const QString temporary = path + QStringLiteral(".crop");
    const engine::pages::PageSaveResult saved =
        editor.saveAsCopy(temporary.toStdString(), engine::save::SaveOptions{});
    if (!saved.ok()) {
        QFile::remove(temporary);
        result.message = tr("寫檔失敗");
        return result;
    }
    QByteArray rewritten;
    if (!readAll(temporary, &rewritten, &result.message)) {
        QFile::remove(temporary);
        return result;
    }
    QFile::remove(temporary);

    return writeBack(path, source, toStd(rewritten), editor.pageCount(),
                     tr("已裁切 %1 頁至內容邊界").arg(cropped));
}

PageOperationResult PageOperationsService::rotatePages(const QString& path,
                                                      const std::vector<int>& pages,
                                                      domain::pages::PageRotation rotation,
                                                      RewriteConsent) {
    PageOperationResult result;
    if (pages.empty()) {
        result.message = tr("沒有要旋轉的頁面");
        return result;
    }
    if (rotation == domain::pages::PageRotation::None) {
        // 轉 0 度不是「不做事」而是呼叫端算錯了。靜靜成功會讓使用者以為
        // 轉過了，而畫面沒有任何變化。
        result.message = tr("旋轉角度是 0，未做變更");
        return result;
    }

    QByteArray source;
    if (!readAll(path, &source, &result.message)) return result;

    engine::pages::PageEditor editor;
    if (!editor.open(path.toStdString(), "")) {
        result.message = tr("無法解析文件結構");
        return result;
    }

    // relative = true：在頁面現有的 /Rotate 上累加，而不是設成絕對角度。
    // 使用者按的是「再轉 90 度」，不是「轉成 90 度」——已經轉過的頁面
    // 用絕對角度會整批對齊到同一個方向，看起來像有幾頁沒轉。
    const engine::pages::PageEditResult rotated = editor.rotatePages(pages, rotation, true);
    if (!rotated.ok()) {
        result.message = tr("旋轉頁面失敗：%1").arg(QString::fromStdString(rotated.message));
        return result;
    }

    const QString temporary = path + QStringLiteral(".rotate");
    const engine::pages::PageSaveResult saved =
        editor.saveAsCopy(temporary.toStdString(), engine::save::SaveOptions{});
    if (!saved.ok()) {
        QFile::remove(temporary);
        result.message = tr("寫檔失敗");
        return result;
    }

    QByteArray rewritten;
    if (!readAll(temporary, &rewritten, &result.message)) {
        QFile::remove(temporary);
        return result;
    }
    QFile::remove(temporary);

    return writeBack(path, source, toStd(rewritten), editor.pageCount(),
                     tr("已旋轉 %1 頁").arg(pages.size()));
}

PageOperationResult PageOperationsService::insertSummaryPages(
    const QString& path, const std::vector<std::pair<int, QString>>& summaries, RewriteConsent) {
    PageOperationResult result;
    if (summaries.empty()) {
        result.message = tr("這份文件沒有註解可以摘要");
        return result;
    }

    // 全部摘要排版成**一份**中繼 PDF，每一段摘要各自成頁；再一次插進主文件。
    //
    // 逐頁各排一份、逐次插入也做得到，但那是每頁一次全檔重寫：100 頁有註解
    // 就是 100 次重寫一份可能 100 MB 的檔案。
    QString combined;
    std::vector<int> pageBreaks;  // 每一段摘要在中繼文件裡的起始頁
    std::vector<int> targets;     // 對應要插在主文件的哪一頁之後
    engine::create::TextImportResult composed;
    {
        // 先逐段排版量出頁數，才知道每一段從中繼文件的第幾頁開始。
        // 一段摘要可能長到跨頁，假設「一段一頁」會讓長摘要之後的插入點全錯。
        int cursor = 0;
        for (const auto& entry : summaries) {
            if (entry.second.trimmed().isEmpty()) continue;
            const engine::create::TextImportResult piece =
                engine::create::createPdfFromPlainText(entry.second.toStdString());
            if (!piece.ok) {
                result.message = piece.nonAscii
                                     ? tr("摘要含目前無法排版的字元：%1")
                                           .arg(QString::fromStdString(piece.diagnostic))
                                     : tr("摘要排版失敗：%1")
                                           .arg(QString::fromStdString(piece.diagnostic));
                return result;
            }
            pageBreaks.push_back(cursor);
            targets.push_back(entry.first);
            cursor += static_cast<int>(piece.pageCount);
            combined += entry.second;
            // 換頁符：讓下一段摘要從新的一頁開始，與上面量到的頁數一致。
            combined += QLatin1Char('\f');
        }
        if (pageBreaks.empty()) {
            result.message = tr("這份文件沒有註解可以摘要");
            return result;
        }
        composed = engine::create::createPdfFromPlainText(combined.toStdString());
        if (!composed.ok) {
            result.message = tr("摘要排版失敗：%1")
                                 .arg(QString::fromStdString(composed.diagnostic));
            return result;
        }
    }

    std::vector<engine::pageops::PagePlacement> placements;
    placements.reserve(pageBreaks.size());
    for (std::size_t i = 0; i < pageBreaks.size(); ++i) {
        const int lastPage = (i + 1 < pageBreaks.size())
                                 ? pageBreaks[i + 1] - 1
                                 : static_cast<int>(composed.pageCount) - 1;
        for (int page = pageBreaks[i]; page <= lastPage; ++page) {
            placements.push_back(engine::pageops::PagePlacement{page, targets[i]});
        }
    }

    QByteArray base;
    if (!readAll(path, &base, &result.message)) return result;

    const auto merged =
        engine::pageops::interleavePagesFrom(toStd(base), composed.bytes, placements);
    if (!merged.ok()) {
        result.message = tr("插入摘要頁失敗：%1").arg(QString::fromStdString(merged.diagnostic));
        return result;
    }

    return writeBack(path, base, merged.bytes, merged.pageCount,
                     tr("已插入 %1 頁摘要").arg(merged.insertedPages));
}

PageOperationResult PageOperationsService::normalize(const QString& path, RewriteConsent) {
    PageOperationResult result;
    QByteArray source;
    if (!readAll(path, &source, &result.message)) return result;

    const auto normalized =
        engine::pageops::normalizePages(toStd(source), engine::pageops::NormalizeRequest{});
    if (!normalized.ok()) {
        result.message = tr("正規化失敗：%1").arg(QString::fromStdString(normalized.diagnostic));
        return result;
    }

    if (normalized.normalizedPages == 0) {
        // 已經正規化過的文件不該被重寫一次：那會白白讓簽章失效。
        result.ok = true;
        result.pageCount = normalized.pageCount;
        result.message = tr("所有頁面的原點都已是 (0,0)，未做變更");
        return result;
    }

    return writeBack(path, source, normalized.bytes, normalized.pageCount,
                     tr("已正規化 %1 頁，平移 %2 則註解")
                         .arg(normalized.normalizedPages)
                         .arg(normalized.movedAnnotations));
}

bool PageOperationsService::restore(const QString& path, const QByteArray& previousBytes,
                                    QString* message) {
    if (previousBytes.isEmpty()) {
        if (message) *message = tr("沒有可復原的內容");
        return false;
    }

    platform::AtomicFileWriter writer(path);
    if (!writer.begin() ||
        !writer.write(previousBytes.constData(), static_cast<std::size_t>(previousBytes.size())) ||
        !writer.commit()) {
        if (message) *message = tr("寫檔失敗");
        return false;
    }
    return true;
}

}  // namespace alioth::app
