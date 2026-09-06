#pragma once

// 從零建立一份完整 PDF（WBS 15 的共用底座）。
//
// 為什麼不用 objects::IncrementalAppender：它的合約是「原檔位元組 + 新段」，
// 前提是有一份原檔。這裡沒有原檔，一切都是新的，因此需要的是另一件事——
// 完整的檔案結構：檔頭、物件本體、傳統 xref 表、trailer。硬把附加器套上來
// 只會得到一個以空檔案為基底的特例，而那個特例會反過來污染附加器裡
// 「純附加、原檔不可動」的核心不變式。
//
// 也刻意不連結 PDFium：PDFium 的建檔 API 產不出 /SMask、產不出直接嵌入的
// DCTDecode 影像，而且它會把我們寫的東西重新序列化一次，位元組層級的測試
// 就失去意義。這一層輸入是物件、輸出是位元組，可以逐位元組驗到底。
//
// xref 一律用傳統表而非 xref 串流。新建的檔案沒有相容性包袱，而傳統表
// 是所有解析器（含最舊的那些）都讀得懂的形態；xref 串流的好處是體積，
// 對只有數十個物件的新檔而言等於零。

#include <cstddef>
#include <string>
#include <vector>

#include "engine/objects/pdf_object.h"

namespace alioth::engine::create {

struct DocumentBuildResult {
    bool ok{false};
    std::string diagnostic;
    std::string bytes;
};

// zlib（RFC 1950）壓縮，供 /FlateDecode 用。壓不動時回傳空字串，
// 呼叫端據此改寫成未壓縮串流——寧可檔案大一點，也不要寫出宣稱是
// FlateDecode 卻解不開的資料。
[[nodiscard]] std::string deflateBytes(const std::string& raw);

class PdfDocumentBuilder {
public:
    PdfDocumentBuilder();

    [[nodiscard]] int allocateObject();
    void setObject(int number, objects::PdfObject object);

    // 新增一頁。content 是尚未壓縮的內容串流位元組；本函式負責建立串流物件、
    // 決定要不要壓縮、把頁面掛進頁面樹。回傳頁面物件編號。
    int addPage(double widthPt, double heightPt, const std::string& content,
                objects::PdfDictionary resources);

    void setProducer(std::string producer) { producer_ = std::move(producer); }
    void setTitle(std::string title) { title_ = std::move(title); }

    // 壓縮開關。測試要逐位元組比對嵌入的 JPEG 時需要關掉——不是因為
    // 壓縮有錯，而是因為「原始位元組有沒有被動過」這件事必須直接看得到。
    void setCompressContent(bool enabled) { compressContent_ = enabled; }

    [[nodiscard]] std::size_t pageCount() const noexcept { return pages_.size(); }
    [[nodiscard]] int catalogObject() const noexcept { return catalogNumber_; }
    [[nodiscard]] int pagesRootObject() const noexcept { return pagesNumber_; }

    [[nodiscard]] DocumentBuildResult build() const;

private:
    struct PendingObject {
        int number{0};
        objects::PdfObject object;
    };

    std::vector<PendingObject> objects_;
    std::vector<int> pages_;
    int catalogNumber_{1};
    int pagesNumber_{2};
    int nextNumber_{3};
    bool compressContent_{true};
    std::string producer_{"Alioth"};
    std::string title_;
};

// 標準 14 字型的字型字典。/Encoding 一律寫 WinAnsiEncoding：不寫的話
// 檢視器會退回字型內建的編碼，而 Helvetica 的內建編碼是 StandardEncoding，
// 引號與連字號那一段會與我們寫進去的位元組對不上。
[[nodiscard]] objects::PdfObject makeStandardFontDictionary(const std::string& baseFont);

}  // namespace alioth::engine::create
