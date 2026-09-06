#include "engine/annotations/annotation_document.h"
#include "engine/pdfium_lock.h"

#include <fpdf_annot.h>
#include <fpdf_save.h>
#include <fpdfview.h>

#include <map>
#include <utility>

#include "engine/pdfium_library.h"

namespace alioth::engine::annotations {

namespace {

// FPDF_SaveWithVersion 的接收端。
struct MemoryWriter {
    FPDF_FILEWRITE base{};
    std::vector<unsigned char>* out{nullptr};
};

int writeBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* writer = reinterpret_cast<MemoryWriter*>(self);
    if (writer == nullptr || writer->out == nullptr) return 0;
    const auto* bytes = static_cast<const unsigned char*>(data);
    writer->out->insert(writer->out->end(), bytes, bytes + size);
    return 1;
}

// PDFium 的取字串介面統一是「先問長度、再給緩衝區」，且長度含結尾的兩個零位元組。
// 回傳值 2 代表「鍵存在但是空字串」，與「鍵不存在」（回傳 0）是不同的情況。
template <typename Fetch>
std::optional<std::string> fetchUtf16String(Fetch&& fetch) {
    const unsigned long bytes = fetch(nullptr, 0);
    if (bytes == 0) return std::nullopt;
    std::vector<unsigned short> buffer(bytes / sizeof(unsigned short) + 1, 0);
    const unsigned long written = fetch(buffer.data(), bytes);
    if (written == 0) return std::nullopt;

    std::string out;
    out.reserve(buffer.size());
    for (unsigned short unit : buffer) {
        if (unit == 0) break;
        // 註解內容與外觀串流在本子系統中皆為 ASCII 或基本多語言平面字元。
        // 這裡只做 UTF-16 → UTF-8 的兩段式轉換，代理對留給日後真的需要時再說。
        if (unit < 0x80) {
            out += static_cast<char>(unit);
        } else if (unit < 0x800) {
            out += static_cast<char>(0xC0 | (unit >> 6));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (unit >> 12));
            out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        }
    }
    return out;
}

const char* subtypeToName(FPDF_ANNOTATION_SUBTYPE subtype) noexcept {
    switch (subtype) {
        case FPDF_ANNOT_TEXT:      return "Text";
        case FPDF_ANNOT_LINE:      return "Line";
        case FPDF_ANNOT_SQUARE:    return "Square";
        case FPDF_ANNOT_CIRCLE:    return "Circle";
        case FPDF_ANNOT_HIGHLIGHT: return "Highlight";
        case FPDF_ANNOT_UNDERLINE: return "Underline";
        case FPDF_ANNOT_SQUIGGLY:  return "Squiggly";
        case FPDF_ANNOT_STRIKEOUT: return "StrikeOut";
        case FPDF_ANNOT_INK:       return "Ink";
        default:                   return "Unknown";
    }
}

}  // namespace

struct AnnotationDocument::Impl {
    // 宣告順序即銷毀順序：runtime 必須排在最前面，才會在文件與頁面全部關閉之後
    // 才釋放，否則 FPDF_DestroyLibrary 會先於 FPDF_CloseDocument 發生。
    PdfiumRuntime runtime{};
    FPDF_DOCUMENT document{nullptr};
    std::map<int, FPDF_PAGE> pages{};

    ~Impl() { closeAll(); }

    void closeAll() {
        for (auto& [index, page] : pages) FPDF_ClosePage(page);
        pages.clear();
        if (document != nullptr) {
            FPDF_CloseDocument(document);
            document = nullptr;
        }
    }

    // 頁面保持載入到文件關閉為止：反覆 Load/Close 會重新剖析頁面樹，而註解
    // 修改必須落在同一個頁面物件上，否則後寫的會看不到先寫的。
    FPDF_PAGE page(int index) {
        if (document == nullptr || index < 0) return nullptr;
        if (auto it = pages.find(index); it != pages.end()) return it->second;
        FPDF_PAGE loaded = FPDF_LoadPage(document, index);
        if (loaded == nullptr) return nullptr;
        pages.emplace(index, loaded);
        return loaded;
    }
};

AnnotationDocument::AnnotationDocument() : impl_(std::make_unique<Impl>()) {}
AnnotationDocument::~AnnotationDocument() = default;
AnnotationDocument::AnnotationDocument(AnnotationDocument&&) noexcept = default;
AnnotationDocument& AnnotationDocument::operator=(AnnotationDocument&&) noexcept = default;

bool AnnotationDocument::openFromMemory(const void* data, std::size_t size,
                                        const std::string& password) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    impl_->closeAll();
    impl_->document = FPDF_LoadMemDocument64(data, size, password.empty() ? nullptr : password.c_str());
    return impl_->document != nullptr;
}

bool AnnotationDocument::isOpen() const noexcept { return impl_->document != nullptr; }

int AnnotationDocument::pageCount() const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    return impl_->document != nullptr ? FPDF_GetPageCount(impl_->document) : 0;
}

WriteResult AnnotationDocument::addAnnotation(int pageIndex, const domain::Annotation& annotation,
                                              const AppearanceOptions& options) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    FPDF_PAGE page = impl_->page(pageIndex);
    if (page == nullptr) {
        WriteResult result{};
        result.diagnostic = "無法載入頁面";
        return result;
    }
    return writeAnnotation(page, annotation, options);
}

std::vector<unsigned char> AnnotationDocument::saveIncremental() const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    std::vector<unsigned char> out;
    if (impl_->document == nullptr) return out;

    MemoryWriter writer{};
    writer.base.version = 1;
    writer.base.WriteBlock = &writeBlock;
    writer.out = &out;
    if (FPDF_SaveWithVersion(impl_->document, &writer.base, FPDF_INCREMENTAL, 17) == 0) out.clear();
    return out;
}

int AnnotationDocument::annotationCount(int pageIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    FPDF_PAGE page = impl_->page(pageIndex);
    return page != nullptr ? FPDFPage_GetAnnotCount(page) : 0;
}

namespace {

// 取註解把手並保證關閉。PDFium 的註解把手是引用計數的，漏關會讓文件關不掉。
class ScopedAnnot {
public:
    ScopedAnnot(FPDF_PAGE page, int index)
        : annot_(page != nullptr ? FPDFPage_GetAnnot(page, index) : nullptr) {}
    ~ScopedAnnot() {
        if (annot_ != nullptr) FPDFPage_CloseAnnot(annot_);
    }
    ScopedAnnot(const ScopedAnnot&) = delete;
    ScopedAnnot& operator=(const ScopedAnnot&) = delete;

    [[nodiscard]] FPDF_ANNOTATION get() const noexcept { return annot_; }
    explicit operator bool() const noexcept { return annot_ != nullptr; }

private:
    FPDF_ANNOTATION annot_{nullptr};
};

}  // namespace

std::optional<std::string> AnnotationDocument::appearanceStream(int pageIndex, int annotIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    const ScopedAnnot annot(impl_->page(pageIndex), annotIndex);
    if (!annot) return std::nullopt;
    return fetchUtf16String([&](unsigned short* buffer, unsigned long length) {
        return FPDFAnnot_GetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, buffer, length);
    });
}

std::optional<std::string> AnnotationDocument::stringValue(int pageIndex, int annotIndex,
                                                           const char* key) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    const ScopedAnnot annot(impl_->page(pageIndex), annotIndex);
    if (!annot) return std::nullopt;
    return fetchUtf16String([&](unsigned short* buffer, unsigned long length) {
        return FPDFAnnot_GetStringValue(annot.get(), key, buffer, length);
    });
}

std::optional<domain::RectF> AnnotationDocument::annotationRect(int pageIndex, int annotIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    const ScopedAnnot annot(impl_->page(pageIndex), annotIndex);
    if (!annot) return std::nullopt;
    FS_RECTF rect{};
    if (FPDFAnnot_GetRect(annot.get(), &rect) == 0) return std::nullopt;
    return domain::RectF{rect.left, rect.bottom, rect.right, rect.top}.normalized();
}

std::optional<int> AnnotationDocument::annotationFlags(int pageIndex, int annotIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    const ScopedAnnot annot(impl_->page(pageIndex), annotIndex);
    if (!annot) return std::nullopt;
    return FPDFAnnot_GetFlags(annot.get());
}

std::size_t AnnotationDocument::quadPointCount(int pageIndex, int annotIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    const ScopedAnnot annot(impl_->page(pageIndex), annotIndex);
    if (!annot) return 0;
    return FPDFAnnot_CountAttachmentPoints(annot.get());
}

std::optional<std::string> AnnotationDocument::subtypeName(int pageIndex, int annotIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    const ScopedAnnot annot(impl_->page(pageIndex), annotIndex);
    if (!annot) return std::nullopt;
    return std::string(subtypeToName(FPDFAnnot_GetSubtype(annot.get())));
}

}  // namespace alioth::engine::annotations
