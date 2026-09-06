#pragma once

// 表單欄位的物件層寫入通道（PRD-FORM-010 ~ 020，ADR-002）。
//
// PDFium 沒有「建立表單欄位」的公開 API：FPDFAnnot_* 那組能建立註解，
// 但表單欄位除了 widget 註解之外還需要 /AcroForm 登記、/DR 字型資源、
// 欄位樹（/Kids、/Parent）與 /AP 的狀態字典，這些都沒有對應的 setter。
// 逐項去找替代路徑只會拼湊出一堆特例，因此走 ADR-002 的物件層通道，
// 自己把字典寫成位元組附加在檔尾。
//
// 「附加新物件」的界線在這裡同樣成立：欄位物件、外觀串流、字型都是新物件；
// 被改寫的只有頁面的 /Annots 與 catalog 的 /AcroForm 這兩個陣列的容器物件，
// 而改寫本身也是以「寫出新版本」的方式完成，原檔位元組一個都不動。
//
// 用法是「開一個 writer → 加若干欄位 → finish()」。/AcroForm 的登記刻意
// 累積到 finish() 才寫出：每加一個欄位就重寫一次 catalog，會讓增量段裡
// 出現 N 份幾乎相同的 catalog，PRD-IO-001 的「增量 ≤ 20 KB」很快就守不住。

#include <string>
#include <vector>

#include "engine/formbuild/field_definition.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::formbuild {

struct FieldWriteResult {
    bool ok{false};
    std::string diagnostic;

    int fieldObject{0};                  // 欄位字典（單 widget 欄位時同時也是 widget）
    std::vector<int> widgetObjects;      // widget 註解；單 widget 時只有一個且等於 fieldObject
    std::vector<int> appearanceObjects;  // /AP /N 的 Form XObject

    // 注意：欄位值含畫不出來的字元（不是可列印 ASCII，內嵌的 CJK 子集裡也
    // 沒有字形）時，外觀串流會明確失敗，整個欄位不會被建立——ok 會是 false、
    // diagnostic 帶原因。
    // 這裡不再有「降級但仍建立成功」這條路徑（見 field_appearance.h）。
};

class FormFieldWriter {
public:
    explicit FormFieldWriter(objects::IncrementalAppender& appender);

    FormFieldWriter(const FormFieldWriter&) = delete;
    FormFieldWriter& operator=(const FormFieldWriter&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }

    // 文件中已存在的欄位名（開啟時掃描而得）。用於重複名稱檢查，
    // 也讓呼叫端能在 UI 上提示「這個名字已經被用了」。
    [[nodiscard]] const std::vector<std::string>& existingFieldNames() const noexcept {
        return fieldNames_;
    }

    [[nodiscard]] FieldWriteResult addField(const FieldDefinition& definition);

    // 寫出 /AcroForm 與 catalog。回傳空字串代表成功。
    // 必須呼叫，否則欄位物件雖然存在，Acrobat 與 PDFium 都不會認得它們。
    [[nodiscard]] std::string finish();

    [[nodiscard]] bool finished() const noexcept { return finished_; }

    // 已成功寫入的欄位規格，供 Fields 面板建樹（PRD-FORM-021）。
    [[nodiscard]] const std::vector<FieldDefinition>& writtenFields() const noexcept {
        return written_;
    }

private:
    [[nodiscard]] bool loadCatalog();
    void collectExistingNames();
    [[nodiscard]] int ensureDefaultFont();

    [[nodiscard]] objects::PdfObject buildMk(const FieldDefinition& definition) const;
    [[nodiscard]] objects::PdfObject buildBorderStyle(const FieldDefinition& definition) const;
    [[nodiscard]] std::string buildDefaultAppearance(const FieldDefinition& definition) const;

    objects::IncrementalAppender& appender_;

    objects::PdfRef catalogRef_{};
    objects::PdfDictionary acroForm_{};
    int acroFormObject_{0};  // /AcroForm 為間接參照時的物件編號，0 代表直接寫在 catalog 內
    objects::PdfArray fields_{};

    std::vector<std::string> fieldNames_;
    std::vector<FieldDefinition> written_;

    int fontObject_{0};
    bool needsSigFlags_{false};
    bool ready_{false};
    bool finished_{false};
    std::string diagnostic_;
};

}  // namespace alioth::engine::formbuild
