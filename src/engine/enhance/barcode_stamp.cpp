#include "engine/enhance/barcode_stamp.h"

#include <algorithm>

#include "domain/document_source.h"
#include "engine/objects/content_stream_appender.h"
#include "engine/objects/pdf_object.h"

namespace alioth::engine::enhance {

namespace {

[[nodiscard]] BarcodeStampResult failure(std::string reason) {
    BarcodeStampResult result;
    result.diagnostic = std::move(reason);
    return result;
}

}  // namespace

BarcodeStampResult stampBarcode(objects::IncrementalAppender& appender, int pageIndex,
                                const std::string& text, const domain::RectF& rect,
                                const BarcodeStampOptions& options) {
    if (!appender.isOpen()) return failure("附加器尚未開啟");
    if (rect.normalized().isEmpty()) return failure("條碼矩形不可為空");
    if (options.quietZoneModules < 0) return failure("靜區模組數不可為負數");

    const domain::barcode::Code128Result code = domain::barcode::encodeCode128(text);
    if (!code.ok) return failure(code.diagnostic);

    const domain::RectF box = rect.normalized();
    // 人讀文字需要在條碼下方留一行空間，否則兩者會疊在一起。
    const double textHeight =
        options.includeHumanReadableText ? options.humanReadableFontSize * 1.4 : 0.0;
    if (box.height() <= textHeight) return failure("條碼矩形高度不足以容納條碼與人讀文字");

    // 靜區以模組數換算成點：先算出「若靜區不佔任何點」時的每模組寬度上界，
    // 再反推靜區實際寬度。用迭代而非解析解是因為模組寬本身依賴扣掉靜區之後
    // 的可用寬度，兩者互相牽動；固定點數的靜區只需要一次換算就能收斂，
    // 不需要真的迭代——先假設全部寬度都是墨區來估出模組寬，再扣掉靜區重算一次。
    const double totalModules = static_cast<double>(code.totalModules);
    const double quietModules = static_cast<double>(options.quietZoneModules) * 2.0;
    const double approxModuleWidth = box.width() / (totalModules + quietModules);
    const double quietWidth = approxModuleWidth * static_cast<double>(options.quietZoneModules);

    const domain::RectF inkRect{box.left + quietWidth, box.bottom + textHeight,
                                box.right - quietWidth, box.top};
    // 用未正規化的 width()/height() 而不是 normalized().isEmpty()：
    // normalized() 會把左右或上下顛倒的矩形直接交換回正常順序，那正是
    // 「已經放不下」的訊號本身，交換回去只會讓失敗被悄悄吃掉
    // （engine/formbuild/field_appearance.cpp 的條碼欄位踩過同一個坑）。
    if (inkRect.width() <= 0.0 || inkRect.height() <= 0.0) {
        return failure("扣掉靜區與人讀文字列後沒有可畫墨的區域");
    }

    std::string content = domain::barcode::barcodeBarsContentStream(code, inkRect);
    if (content.empty()) return failure("條碼內容串流產生失敗");

    objects::ContentAppendOptions appendOptions;
    appendOptions.wrapInGraphicsState = true;

    if (options.includeHumanReadableText) {
        appendOptions.fonts.push_back(
            objects::ContentFontRequest{"F0", "Helvetica"});

        using domain::create::StandardFont;
        const double fontSize = options.humanReadableFontSize;
        const double textWidth =
            domain::create::measureText(StandardFont::Helvetica, text, fontSize);
        const double textX = box.left + (box.width() - textWidth) / 2.0;
        const double textY = box.bottom + (textHeight - fontSize) / 2.0;

        content += "BT\n/F0 " + std::to_string(fontSize) + " Tf\n" + std::to_string(textX) + " " +
                   std::to_string(textY) + " Td\n(" + objects::escapeLiteralString(text) +
                   ") Tj\nET\n";
    }

    const objects::ContentAppendResult appended =
        objects::appendPageContent(appender, pageIndex, content, appendOptions);
    if (!appended.ok) return failure(appended.diagnostic);

    BarcodeStampResult result;
    result.ok = true;
    result.contentObject = appended.contentObject;
    return result;
}

}  // namespace alioth::engine::enhance
