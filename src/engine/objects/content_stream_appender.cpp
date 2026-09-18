#include "engine/objects/content_stream_appender.h"

#include <algorithm>
#include <array>
#include <variant>

#include "engine/objects/page_object_editor.h"

namespace alioth::engine::objects {

namespace {

// ISO 32000 附錄 D 的標準 14。這些字型不需要內嵌，任何符合規格的檢視器
// 都必須提供，因此 Bates 編號可以在完全不碰字型授權的前提下寫進檔案。
constexpr std::array<const char*, 14> kStandard14 = {
    "Helvetica",   "Helvetica-Bold",   "Helvetica-Oblique",   "Helvetica-BoldOblique",
    "Times-Roman", "Times-Bold",       "Times-Italic",        "Times-BoldItalic",
    "Courier",     "Courier-Bold",     "Courier-Oblique",     "Courier-BoldOblique",
    "Symbol",      "ZapfDingbats",
};

[[nodiscard]] ContentAppendResult failure(std::string reason) {
    ContentAppendResult result{};
    result.diagnostic = std::move(reason);
    return result;
}

}  // namespace

bool isStandard14Font(const std::string& baseFont) {
    return std::any_of(kStandard14.begin(), kStandard14.end(),
                       [&baseFont](const char* name) { return baseFont == name; });
}

ContentAppendResult appendPageContent(IncrementalAppender& appender, int pageIndex,
                                      const std::string& content,
                                      const ContentAppendOptions& options) {
    if (!appender.isOpen()) return failure("附加器尚未開啟原檔");
    if (content.empty()) return failure("內容串流是空的");

    PdfRef pageRef{};
    if (!pageRefAt(appender, pageIndex, pageRef)) {
        return failure("頁碼超出範圍：" + std::to_string(pageIndex));
    }

    // 這個函式只負責「把位元組接到頁面內容後面」，不管字型。含 CJK 的內容
    // 必須由呼叫端先跳脫成 ASCII 安全的形式（Identity-H 走八進位跳脫）並自行
    // 登記 /Resources /Font，因此這裡看到裸的非 ASCII 位元組就是呼叫端有錯。
    const auto nonAscii = std::find_if(content.begin(), content.end(), [](char c) {
        return static_cast<unsigned char>(c) >= 0x80;
    });
    if (nonAscii != content.end()) {
        return failure("內容含未跳脫的非 ASCII 位元組；含 CJK 的內容必須先跳脫並自行登記字型");
    }

    for (const ContentFontRequest& font : options.fonts) {
        if (font.resourceName.empty()) return failure("字型資源名稱是空的");
        if (!isStandard14Font(font.baseFont)) {
            return failure("只支援標準 14 字型，收到：" + font.baseFont);
        }
    }

    ContentAppendResult result{};

    for (const ContentFontRequest& font : options.fonts) {
        PdfDictionary dict;
        dict.set("Type", makeName("Font"));
        dict.set("Subtype", makeName("Type1"));
        dict.set("BaseFont", makeName(font.baseFont));
        // Symbol 與 ZapfDingbats 自帶編碼，硬指定 WinAnsi 會讓字元全部對錯。
        if (font.baseFont != "Symbol" && font.baseFont != "ZapfDingbats") {
            dict.set("Encoding", makeName("WinAnsiEncoding"));
        }
        const int fontNumber = appender.allocateObject();
        appender.setObject(fontNumber, PdfObject{std::move(dict)});
        result.fontObjects.push_back(fontNumber);

        const PageEditStatus status =
            setPageResource(appender, pageRef, "Font", font.resourceName, makeRef(fontNumber));
        if (!status.ok) return failure("登記字型資源失敗：" + status.diagnostic);
    }

    // 前置換行：/Contents 陣列的相鄰串流之間必須有 token 邊界，
    // 少了它，前一份串流最後一個 token 會與我們的第一個 token 黏成一個。
    std::string data = "\n";
    if (options.wrapInGraphicsState) data += "q\n";
    data += content;
    if (!data.empty() && data.back() != '\n') data += '\n';
    if (options.wrapInGraphicsState) data += "Q\n";

    PdfDictionary streamDict;
    if (!options.markerKey.empty()) streamDict.set(options.markerKey, PdfObject{true});

    const int contentNumber = appender.allocateObject();
    appender.setObject(contentNumber,
                       PdfObject{PdfStream{std::move(streamDict), std::move(data)}});

    const PageEditStatus status =
        appendToPageArray(appender, pageRef, "Contents", makeRef(contentNumber));
    if (!status.ok) return failure("掛上 /Contents 失敗：" + status.diagnostic);

    result.ok = true;
    result.contentObject = contentNumber;
    return result;
}

RemoveMarkedResult removeMarkedPageContent(IncrementalAppender& appender,
                                           const std::string& markerKey) {
    RemoveMarkedResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "附加器尚未開啟原檔";
        return result;
    }
    if (markerKey.empty()) {
        result.diagnostic = "標記鍵是空的";
        return result;
    }

    const auto& pages = appender.source().pages();
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const PdfRef& page = pages[i];
        const PdfObject pageObject = appender.currentObject(page.number);
        const PdfDictionary* pageDict = pageObject.asDictionary();
        if (pageDict == nullptr) continue;

        const PdfObject* contents = pageDict->find("Contents");
        if (contents == nullptr) continue;

        // /Contents 可以是單一參照或陣列。單一參照的情形不可能是我們加的
        // （我們一律用 appendToPageArray，它會把單一參照升成陣列），
        // 但還是要處理——別的工具產生的檔案可能長成任何樣子。
        std::vector<int> candidates;
        if (contents->isRef()) {
            candidates.push_back(contents->asRef().number);
        } else if (const PdfArray* array = contents->asArray(); array != nullptr) {
            for (const PdfObject& item : *array) {
                if (item.isRef()) candidates.push_back(item.asRef().number);
            }
        }

        bool touched = false;
        for (const int number : candidates) {
            const PdfObject stream = appender.currentObject(number);
            const PdfStream* asStream = stream.asStream();
            if (asStream == nullptr) continue;
            const PdfObject* marker = asStream->dict.find(markerKey);
            // 只認 true。留 false 的話代表有人刻意停用過這個標記，
            // 而我們不知道他為什麼那樣做——不要替他決定。
            if (marker == nullptr) continue;
            const bool* flag = std::get_if<bool>(&marker->value());
            if (flag == nullptr || !*flag) continue;

            const PageEditStatus status =
                removeFromPageArray(appender, page, "Contents", number);
            if (!status.ok) {
                result.diagnostic = "移除 /Contents 參照失敗：" + status.diagnostic;
                return result;
            }
            ++result.removedReferences;
            touched = true;
        }
        if (touched) ++result.affectedPages;
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::objects
