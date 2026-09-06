#pragma once

// 註解子系統的最小文件把手。
//
// 存在理由是分層：PDFium 只能在引擎轉接層被呼叫，但要驗證 /AP 真的寫進了
// 檔案、而且讀得回來，就必須有人負責開檔、掛註解、存檔、再讀回。這個類別
// 把那條路徑收在引擎層內，讓測試與上層完全不必見到 FPDF_* 型別。
//
// 它刻意只做註解相關的最小集合：完整的文件生命週期、快取、優先權佇列在
// PdfiumEngine，增量儲存策略在 WBS 6.x。這裡不重複那些職責。
//
// 執行緒限制與 PdfiumEngine 相同：同一個實體只能由單一執行緒使用。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/annotation.h"
#include "engine/annotations/annotation_writer.h"

namespace alioth::engine::annotations {

class AnnotationDocument {
public:
    AnnotationDocument();
    ~AnnotationDocument();

    AnnotationDocument(const AnnotationDocument&) = delete;
    AnnotationDocument& operator=(const AnnotationDocument&) = delete;
    AnnotationDocument(AnnotationDocument&&) noexcept;
    AnnotationDocument& operator=(AnnotationDocument&&) noexcept;

    // 資料在載入期間必須保持有效；PDFium 不會複製它。
    [[nodiscard]] bool openFromMemory(const void* data, std::size_t size,
                                      const std::string& password = {});
    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] int pageCount() const;

    [[nodiscard]] WriteResult addAnnotation(int pageIndex, const domain::Annotation& annotation,
                                            const AppearanceOptions& options = {});

    // 增量儲存：只追加新物件，原有位元組原封不動，既有簽章才不會變成「無效」。
    [[nodiscard]] std::vector<unsigned char> saveIncremental() const;

    [[nodiscard]] int annotationCount(int pageIndex) const;
    [[nodiscard]] std::optional<std::string> appearanceStream(int pageIndex, int annotIndex) const;
    [[nodiscard]] std::optional<std::string> stringValue(int pageIndex, int annotIndex,
                                                         const char* key) const;
    [[nodiscard]] std::optional<domain::RectF> annotationRect(int pageIndex, int annotIndex) const;
    [[nodiscard]] std::optional<int> annotationFlags(int pageIndex, int annotIndex) const;
    [[nodiscard]] std::size_t quadPointCount(int pageIndex, int annotIndex) const;
    [[nodiscard]] std::optional<std::string> subtypeName(int pageIndex, int annotIndex) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine::annotations
