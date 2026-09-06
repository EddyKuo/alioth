#include "app/stamp_service.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

#include "app/print/stamp_content_stream.h"
#include "engine/fonts/cid_font_writer.h"
#include "engine/fonts/cjk_font_library.h"
#include "engine/fonts/text_runs.h"
#include "engine/objects/content_stream_appender.h"
#include "engine/objects/page_object_editor.h"
#include "engine/objects/incremental_appender.h"
#include "engine/objects/pdf_source_document.h"
#include "platform/atomic_file.h"

namespace alioth::app {
namespace {

constexpr const char* kFontResource = "AliothStampF0";
constexpr const char* kBaseFont = "Helvetica";
// CJK 字型的資源名稱（ADR-007）。名字刻意帶前綴：頁面上很可能已經有一個
// 叫 /CJK 的字型，撞名會讓戳記用到別人的字型而畫出完全不同的字。
constexpr const char* kCjkFontResource = "AliothStampCJK";

// 拉丁字以 0.5 em 估算：不查 AFM 度量表，因為這裡只是把戳記放進九宮格，
// 差幾個點不影響法務用途。真正需要精確度量的是欄位外觀，那條路徑另有處理。
double estimateTextWidth(const QString& text, double fontSize) {
    // CJK 走內嵌字型的實際字寬（ADR-007）。全形字約 1 em，用 0.5 em 去算會讓
    // 置中與靠右的戳記整個偏出頁面一半——那看起來像「錨點算錯」而不像字寬問題。
    double units = 0.0;
    for (const char32_t codepoint : engine::fonts::decodeUtf8(text.toStdString())) {
        if (!engine::fonts::needsCjkFont(codepoint)) {
            units += 500.0;
            continue;
        }
        const std::uint16_t advance =
            engine::fonts::CjkFontLibrary::instance().advanceFor(codepoint);
        units += advance != 0 ? advance : 1000.0;
    }
    return units / 1000.0 * fontSize;
}

// 由 MediaBox 取頁面尺寸。原點不一定是 (0,0)，所以用寬高而不是右上角座標。
domain::SizeF mediaBoxSize(const engine::objects::PdfSourceDocument& source, int pageIndex) {
    if (pageIndex < 0 || pageIndex >= static_cast<int>(source.pages().size())) return {};

    const engine::objects::PdfObject box =
        source.inheritedPageAttribute(source.pages()[static_cast<std::size_t>(pageIndex)],
                                      "MediaBox");

    // resolve() 回傳的是值。把它綁到具名變數而不是直接串 .asArray()——
    // 後者取到的指標指進一個在語句結束就消失的暫時物件，
    // 症狀是「讀不到 MediaBox」而不是崩潰，因此極難從錯誤訊息回推。
    const engine::objects::PdfObject resolved = source.resolve(box);
    const engine::objects::PdfArray* values = resolved.asArray();
    if (values == nullptr || values->size() < 4) return {};

    const auto number = [&source](const engine::objects::PdfObject& object) {
        const engine::objects::PdfObject value = source.resolve(object);
        return value.asNumber();
    };
    const double left = number((*values)[0]);
    const double bottom = number((*values)[1]);
    const double right = number((*values)[2]);
    const double top = number((*values)[3]);
    return domain::SizeF{std::abs(right - left), std::abs(top - bottom)};
}

QByteArray boundaryHashOf(const QByteArray& bytes) {
    constexpr int kWindow = 4096;
    const auto from = static_cast<int>(std::max<qsizetype>(0, bytes.size() - kWindow));
    return QCryptographicHash::hash(bytes.mid(from), QCryptographicHash::Sha256);
}

}  // namespace

StampService::StampService(QObject* parent) : QObject(parent) {}

StampResult StampService::applyStamps(const StampRequest& request) {
    StampResult result;

    if (request.textTemplate.isEmpty()) {
        result.message = tr("戳記內容是空的");
        return result;
    }

    QFile source(request.path);
    if (!source.open(QIODevice::ReadOnly)) {
        result.message = tr("無法讀取檔案：%1").arg(source.errorString());
        return result;
    }
    const QByteArray original = source.readAll();
    source.close();

    result.previousSize = static_cast<quint64>(original.size());
    result.boundaryGuard = boundaryHashOf(original);

    engine::objects::IncrementalAppender appender;
    std::string diagnostic;
    if (appender.open(std::string(original.constData(),
                                  static_cast<std::size_t>(original.size())),
                      &diagnostic) != engine::objects::SourceStatus::Ok) {
        result.message = tr("無法解析文件：%1").arg(QString::fromStdString(diagnostic));
        return result;
    }

    const int pageCount = static_cast<int>(appender.source().pages().size());
    std::vector<std::int32_t> pages = request.pages;
    if (pages.empty()) {
        pages.reserve(static_cast<std::size_t>(pageCount));
        for (std::int32_t i = 0; i < pageCount; ++i) pages.push_back(i);
    }

    // Bates 序列先整批算出來再逐頁蓋：跳號與重號要在寫檔之前就被擋下，
    // 而不是蓋到一半才發現——那時檔案已經被改了一半。
    std::vector<QString> bates;
    if (request.useBates && request.bates.enabled) {
        std::vector<int> ordinals;
        ordinals.reserve(pages.size());
        for (const std::int32_t page : pages) ordinals.push_back(static_cast<int>(page));
        bates = print::batesSequence(request.bates, ordinals);
        if (!print::isBatesSequenceUnique(bates)) {
            result.message = tr("Bates 序列有重號，請檢查起始號與遞增量");
            return result;
        }
    }

    engine::objects::ContentAppendOptions options;
    engine::objects::ContentFontRequest font;
    font.resourceName = kFontResource;
    font.baseFont = kBaseFont;
    options.fonts.push_back(font);

    const QFileInfo info(request.path);
    const QDateTime now = QDateTime::currentDateTime();
    QString skipReason;
    std::set<char32_t> cjkCodepoints;
    std::vector<std::int32_t> stampedPageIndexes;

    for (std::size_t i = 0; i < pages.size(); ++i) {
        const std::int32_t pageIndex = pages[i];
        if (pageIndex < 0 || pageIndex >= pageCount) continue;

        print::StampContext context;
        context.pageNumber = pageIndex + 1;
        context.pageCount = pageCount;
        context.printSequence = static_cast<int>(i) + 1;
        context.sheetNumber = context.printSequence;
        context.fileName = info.fileName();
        context.timestamp = now;
        if (i < bates.size()) context.batesText = bates[i];

        const QString text = print::expandStampTokens(request.textTemplate, context);
        if (text.isEmpty()) {
            skipReason = tr("符號展開後是空字串");
            continue;
        }

        // 頁面尺寸從 MediaBox 讀。它可能繼承自父節點，所以要走 inheritedPageAttribute
        // 而不是直接看頁面字典——真實檔案裡「頁面自己沒有 MediaBox」很常見。
        const domain::SizeF pageSize = mediaBoxSize(appender.source(), pageIndex);
        if (pageSize.isEmpty()) {
            skipReason = tr("讀不到第 %1 頁的 MediaBox").arg(pageIndex + 1);
            continue;
        }

        const double width = estimateTextWidth(text, request.fontSize);
        const domain::PointF origin =
            print::stampBaselineOrigin(pageSize.width, pageSize.height, width, request.fontSize,
                                       request.anchor, request.margins);

        print::StampStreamOptions streamOptions;
        streamOptions.fontResourceName = kFontResource;
        streamOptions.cjkFontResourceName = kCjkFontResource;
        streamOptions.fontSize = request.fontSize;

        const print::StampStream stream =
            print::makeTextStampStream(text.toStdString(), origin.x, origin.y, streamOptions);
        if (!stream.valid) {
            // 明確失敗而不是輸出會缺字的位元組。缺字的浮水印看起來像排版問題，
            // 不像功能失敗，使用者不會回報。
            result.message = tr("第 %1 頁的戳記無法產生：%2")
                                 .arg(pageIndex + 1)
                                 .arg(QString::fromStdString(stream.diagnostic));
            return result;
        }

        const auto appended =
            engine::objects::appendPageContent(appender, pageIndex, stream.content, options);
        if (!appended.ok) {
            result.message = tr("第 %1 頁寫入失敗：%2")
                                 .arg(pageIndex + 1)
                                 .arg(QString::fromStdString(appended.diagnostic));
            return result;
        }
        cjkCodepoints.insert(stream.cjkCodepoints.begin(), stream.cjkCodepoints.end());
        stampedPageIndexes.push_back(pageIndex);
        ++result.stampedPages;
    }

    // CJK 子集只嵌一份，再登記到每一頁的 /Resources /Font（ADR-007）。
    // 逐頁各嵌一份會讓一份五百頁的浮水印文件多出五百份相同的字型。
    if (!cjkCodepoints.empty()) {
        auto& library = engine::fonts::CjkFontLibrary::instance();
        const engine::fonts::SubsetResult subset = library.subsetFor(cjkCodepoints);
        if (!subset.ok) {
            result.message = tr("CJK 字型子集化失敗：%1")
                                 .arg(QString::fromStdString(subset.diagnostic));
            return result;
        }
        const engine::fonts::EmbeddedFontResult embedded =
            engine::fonts::embedSubsetFont(appender, subset, library.baseName());
        if (!embedded.ok) {
            result.message =
                tr("內嵌 CJK 字型失敗：%1").arg(QString::fromStdString(embedded.diagnostic));
            return result;
        }
        for (const std::int32_t pageIndex : stampedPageIndexes) {
            engine::objects::PdfRef pageRef{};
            if (!engine::objects::pageRefAt(appender, pageIndex, pageRef)) continue;
            const engine::objects::PageEditStatus status = engine::objects::setPageResource(
                appender, pageRef, "Font", kCjkFontResource,
                engine::objects::makeRef(embedded.fontObject));
            if (!status.ok) {
                // 少登記一頁的結果是那一頁的戳記畫不出來，而檔案本身合法——
                // 使用者只會看到「有幾頁沒蓋到」，查不出原因。所以整批中止。
                result.message = tr("第 %1 頁登記 CJK 字型失敗：%2")
                                     .arg(pageIndex + 1)
                                     .arg(QString::fromStdString(status.diagnostic));
                return result;
            }
        }
    }

    if (result.stampedPages == 0) {
        // 「沒有任何頁面被蓋章」而不說原因，使用者無從判斷是選錯頁面還是文件有問題。
        result.message = skipReason.isEmpty()
                             ? tr("沒有任何頁面被蓋章")
                             : tr("沒有任何頁面被蓋章：%1").arg(skipReason);
        return result;
    }

    const engine::objects::BuildResult built = appender.build();
    if (!built.ok) {
        result.message = tr("增量儲存失敗：%1").arg(QString::fromStdString(built.diagnostic));
        return result;
    }

    // 純附加的前提要驗過才寫檔：這條不成立的話既有簽章會從「有變更」變成「無效」。
    if (built.bytes.size() < static_cast<std::size_t>(original.size()) ||
        std::memcmp(built.bytes.data(), original.constData(),
                    static_cast<std::size_t>(original.size())) != 0) {
        result.message = tr("儲存結果不是增量，已中止以保全簽章");
        return result;
    }

    platform::AtomicFileWriter writer(request.path);
    if (!writer.begin() || !writer.write(built.bytes.data(), built.bytes.size()) ||
        !writer.commit()) {
        result.message = tr("寫檔失敗");
        return result;
    }

    result.ok = true;
    result.batesNumbers = std::move(bates);
    result.message = tr("已在 %1 頁蓋上戳記（增量 %2 位元組）")
                         .arg(result.stampedPages)
                         .arg(built.appendedBytes);
    return result;
}

}  // namespace alioth::app
