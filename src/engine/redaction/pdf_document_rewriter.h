#pragma once

// 全檔重寫器（WBS 11，PRD-ANN-032 / 034）。
//
// 為什麼 Redaction 不能走增量附加：
//
// 專案其他所有寫入路徑都是「原檔位元組 + 新段」（ADR-002、SDD §1.3），那是為了
// 保住既有數位簽章。但增量附加的定義就是**原檔的每一個位元組都還在檔案裡**——
// 被塗黑的那段文字仍然躺在舊版本的內容串流中，只是不再被最新的 xref 指到。
// 任何一個 `strings` 或 `qpdf --qdf` 都能把它撈出來。PRD-ANN-032 的驗收標準是
// 「原文不可還原」，因此 Redaction 是全案唯一必須放棄增量、整份重寫的操作。
//
// 這件事的代價要說清楚：**重寫必然使既有數位簽章失效**。這不是實作缺陷，
// 是 Redaction 的本質——刪掉簽章覆蓋的位元組後，那份簽章本來就不該再顯示為有效。
//
// 重寫採「從 trailer 可達性標記—清除」而不是「照抄所有物件」：
//
//   - 被移出 /Annots 的註解、被移出資源字典的影像 XObject 會自動不再可達，
//     它們的位元組不會出現在輸出裡。少了這一步，刪掉的便利貼內容還在檔案中
//   - /ObjStm 與 /XRef 串流不可能從 /Root 走到，因此自動被丟棄。這很重要：
//     物件串流裡壓著註解字典的**壓縮複本**，照抄物件會把它一起帶進新檔案
//   - 輸出不會有懸空參照，qpdf --check 才可能零警告
//
// 本層只管物件圖與檔案結構，不認得塗黑語意；內容串流的編輯在 content_redactor。

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "engine/objects/pdf_object.h"
#include "engine/objects/pdf_source_document.h"

namespace alioth::engine::redaction {

using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;
using objects::PdfSourceDocument;
using objects::SourceStatus;

class PdfDocumentRewriter {
public:
    // 加密文件在這裡就被擋下（ADR-002 驗收條件 5）：字串與串流需要加密後才能
    // 寫回去，寫出讀不出來的內容比明確失敗糟糕得多。
    [[nodiscard]] SourceStatus open(std::string bytes, std::string* diagnostic = nullptr);

    [[nodiscard]] const PdfSourceDocument& source() const noexcept { return source_; }
    [[nodiscard]] bool isOpen() const noexcept { return opened_; }

    [[nodiscard]] int pageCount() const noexcept {
        return static_cast<int>(source_.pages().size());
    }
    [[nodiscard]] bool pageRef(int index, PdfRef& out) const;

    // 目前物件圖中的物件。已載入的複本優先，因此連續多次編輯會疊加而不是互相覆蓋。
    [[nodiscard]] const PdfObject* object(int number) const;
    [[nodiscard]] PdfObject* object(int number);
    [[nodiscard]] PdfObject resolve(const PdfObject& value) const;

    // 沿 /Parent 鏈找可繼承的屬性（/Resources、/MediaBox）。
    // 走的是重寫器自己的物件圖，因此看得到本次已經做過的修改。
    [[nodiscard]] PdfObject inheritedPageAttribute(const PdfRef& page, const std::string& key) const;

    void setObject(int number, PdfObject value);
    [[nodiscard]] int addObject(PdfObject value);

    // 物件圖中指向 number 的參照數。內容串流與 Form XObject 只有在恰好被引用
    // 一次時才可以就地改寫：被兩頁共用的串流改一邊等於改兩邊，
    // 而另一頁並沒有要求塗黑。
    [[nodiscard]] int referenceCount(int number) const;

    [[nodiscard]] PdfDictionary& trailer() noexcept { return trailer_; }
    [[nodiscard]] const PdfDictionary& trailer() const noexcept { return trailer_; }

    // 產生完整的新檔位元組。不可達的物件不會被寫出去。
    [[nodiscard]] std::string build() const;

private:
    void loadReachable();
    [[nodiscard]] std::vector<int> reachableObjects() const;

    PdfSourceDocument source_;
    std::map<int, PdfObject> objects_{};
    std::map<int, int> generations_{};
    PdfDictionary trailer_{};
    std::string header_{"%PDF-1.7"};
    int nextNumber_{1};
    bool opened_{false};
};

// 從某個頁面或 Form XObject 的 /Resources /<category> 移除若干名稱。
//
// 路徑上的字典可能被多處共用（/Resources 常常整份掛在 /Pages 上由全部頁面繼承），
// 就地刪除會把別頁的資源一起刪掉。因此沿路採寫入時複製：被共用的字典先複製成
// 新物件再改，未共用的就地改。
//
// 頁面沒有自己的 /Resources 時，先把繼承來的整份複製下來再刪——直接建一個新的
// 空字典會遮蔽繼承鏈，該頁原本用到的字型與影像會全部消失。
[[nodiscard]] bool removeResourceEntries(PdfDocumentRewriter& document, int ownerObject,
                                         const std::string& category,
                                         const std::vector<std::string>& names);

// 在 /Resources /<category> 底下登記一項資源（覆蓋矩形的字型走這條路）。
// 共用路徑的處理與 removeResourceEntries 相同。
[[nodiscard]] bool setResourceEntry(PdfDocumentRewriter& document, int ownerObject,
                                    const std::string& category, const std::string& name,
                                    PdfObject value);

// 取得某個字典鍵底下、可安全就地修改的陣列（/Annots）。
// 鍵不存在時回傳 nullptr；陣列被共用時先複製再回傳複本的指標。
[[nodiscard]] objects::PdfArray* unsharedDictionaryArray(PdfDocumentRewriter& document,
                                                         int ownerObject, const std::string& key);

}  // namespace alioth::engine::redaction
