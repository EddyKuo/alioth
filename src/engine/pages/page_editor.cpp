#include "engine/pages/page_editor.h"
#include "engine/pdfium_lock.h"

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include <QFile>
#include <QString>

#include <algorithm>
#include <utility>

#include "engine/pdfium_library.h"

namespace alioth::engine::pages {
namespace dom = domain::pages;

namespace {

FPDF_DOCUMENT toDocument(void* handle) {
    return static_cast<FPDF_DOCUMENT>(handle);
}

// FPDF_LoadPage / FPDF_ClosePage 的配對守衛。頁面把手在結構性操作之後就失效，
// 所以一律用完即關，不做快取——快取省下的時間遠小於一個懸空把手的代價。
class ScopedPage {
public:
    ScopedPage(FPDF_DOCUMENT document, int index)
        : page_(document ? FPDF_LoadPage(document, index) : nullptr) {}
    ~ScopedPage() {
        if (page_) FPDF_ClosePage(page_);
    }

    ScopedPage(const ScopedPage&) = delete;
    ScopedPage& operator=(const ScopedPage&) = delete;

    [[nodiscard]] FPDF_PAGE get() const noexcept { return page_; }
    [[nodiscard]] explicit operator bool() const noexcept { return page_ != nullptr; }

private:
    FPDF_PAGE page_{nullptr};
};

// FPDF_CreateNewDocument 的守衛，用於「借一份暫時的文件搬運頁面」這個手法。
class ScopedNewDocument {
public:
    ScopedNewDocument() : document_(FPDF_CreateNewDocument()) {}
    ~ScopedNewDocument() {
        if (document_) FPDF_CloseDocument(document_);
    }

    ScopedNewDocument(const ScopedNewDocument&) = delete;
    ScopedNewDocument& operator=(const ScopedNewDocument&) = delete;

    [[nodiscard]] FPDF_DOCUMENT get() const noexcept { return document_; }
    [[nodiscard]] explicit operator bool() const noexcept { return document_ != nullptr; }

private:
    FPDF_DOCUMENT document_{nullptr};
};

// 只數位元組不留內容：估算每頁大小時輸出可能有數十 MB，沒有理由真的存下來。
struct SizeSink {
    FPDF_FILEWRITE base{};
    std::uint64_t size{0};
};

int measureBlock(FPDF_FILEWRITE* self, const void* /*data*/, unsigned long size) {
    reinterpret_cast<SizeSink*>(self)->size += size;
    return 1;
}

PageEditResult fail(PageEditStatus status, std::string message,
                    dom::OperationStatus validation = dom::OperationStatus::Ok) {
    return PageEditResult{status, validation, std::move(message)};
}

PageEditResult invalid(dom::OperationStatus validation) {
    return PageEditResult{PageEditStatus::InvalidArgument, validation, dom::describe(validation)};
}

domain::RectF clampToBox(const domain::RectF& box, const domain::RectF& bounds) {
    domain::RectF out{std::max(box.left, bounds.left), std::max(box.bottom, bounds.bottom),
                      std::min(box.right, bounds.right), std::min(box.top, bounds.top)};
    return out;
}

int countWidgetAnnotations(FPDF_DOCUMENT document) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）：遞迴鎖，呼叫端是否已持鎖不重要。
    int total = 0;
    const int pages = FPDF_GetPageCount(document);
    for (int i = 0; i < pages; ++i) {
        ScopedPage page(document, i);
        if (!page) continue;
        const int annots = FPDFPage_GetAnnotCount(page.get());
        for (int a = 0; a < annots; ++a) {
            FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page.get(), a);
            if (!annot) continue;
            if (FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_WIDGET) ++total;
            FPDFPage_CloseAnnot(annot);
        }
    }
    return total;
}

}  // namespace

const char* describe(PageEditStatus status) noexcept {
    switch (status) {
        case PageEditStatus::Ok:               return "成功";
        case PageEditStatus::NotOpen:          return "尚未開啟文件";
        case PageEditStatus::InvalidArgument:  return "參數不合法";
        case PageEditStatus::PageLoadFailed:   return "頁面載入失敗";
        case PageEditStatus::PdfiumRejected:   return "PDFium 拒絕此操作";
        case PageEditStatus::SourceUnreadable: return "來源檔案無法讀取";
        case PageEditStatus::SaveFailed:       return "存檔失敗";
        case PageEditStatus::Unsupported:      return "目前不支援";
    }
    return "未知狀態";
}

struct PageEditor::Impl {
    // 宣告順序即銷毀順序：runtime 排在最前面才會最後才釋放，
    // 否則 FPDF_DestroyLibrary 會早於 FPDF_CloseDocument。
    std::unique_ptr<PdfiumRuntime> runtime;
    FPDF_DOCUMENT document{nullptr};
    std::vector<unsigned char> bytes;
    std::string sourcePath;
    bool structural{false};

    ~Impl() { closeDocument(); }

    void closeDocument() {
        if (document) {
            FPDF_CloseDocument(document);
            document = nullptr;
        }
        bytes.clear();
        bytes.shrink_to_fit();
        sourcePath.clear();
        structural = false;
        runtime.reset();
    }
};

PageEditor::PageEditor() : impl_(std::make_unique<Impl>()) {}
PageEditor::~PageEditor() = default;
PageEditor::PageEditor(PageEditor&&) noexcept = default;
PageEditor& PageEditor::operator=(PageEditor&&) noexcept = default;

bool PageEditor::open(const std::string& path, const std::string& password) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    close();

    // 用 QFile 而不是 std::fopen：Windows 上窄字元路徑會被當成當地代碼頁解讀，
    // 非 ASCII 檔名會直接開不起來。std::string 一律視為 UTF-8，與 save 子系統一致。
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray content = file.readAll();
    file.close();
    if (content.isEmpty()) return false;

    const auto* begin = reinterpret_cast<const unsigned char*>(content.constData());
    impl_->bytes.assign(begin, begin + content.size());
    impl_->runtime = std::make_unique<PdfiumRuntime>();
    impl_->document = FPDF_LoadMemDocument64(impl_->bytes.data(), impl_->bytes.size(),
                                             password.empty() ? nullptr : password.c_str());
    if (!impl_->document) {
        impl_->closeDocument();
        return false;
    }
    impl_->sourcePath = path;
    return true;
}

bool PageEditor::createEmpty() {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    close();
    impl_->runtime = std::make_unique<PdfiumRuntime>();
    impl_->document = FPDF_CreateNewDocument();
    if (!impl_->document) {
        impl_->closeDocument();
        return false;
    }
    return true;
}

void PageEditor::close() {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    impl_->closeDocument();
}

bool PageEditor::isOpen() const noexcept {
    return impl_->document != nullptr;
}

int PageEditor::pageCount() const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    return impl_->document ? FPDF_GetPageCount(impl_->document) : 0;
}

const std::string& PageEditor::sourcePath() const noexcept {
    return impl_->sourcePath;
}

bool PageEditor::hasStructuralChange() const noexcept {
    return impl_->structural;
}

void* PageEditor::documentHandle() const noexcept {
    return impl_->document;
}

PageEditResult PageEditor::apply(const dom::PageOperation& operation) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (const auto* op = std::get_if<dom::InsertBlankPages>(&operation)) {
        return insertBlankPages(op->atIndex, op->count, op->widthPt, op->heightPt);
    }
    if (const auto* op = std::get_if<dom::DeletePages>(&operation)) {
        return deletePages(op->pages);
    }
    if (const auto* op = std::get_if<dom::RotatePages>(&operation)) {
        return rotatePages(op->pages, op->rotation, op->relative);
    }
    if (const auto* op = std::get_if<dom::MovePages>(&operation)) {
        return movePages(op->pages, op->destinationIndex);
    }
    if (const auto* op = std::get_if<dom::DuplicatePages>(&operation)) {
        return duplicatePages(op->pages, op->destinationIndex);
    }
    if (const auto* op = std::get_if<dom::SwapPages>(&operation)) {
        return swapPages(op->first, op->second);
    }
    if (std::get_if<dom::ReversePages>(&operation) != nullptr) {
        return reversePages();
    }
    return fail(PageEditStatus::Unsupported, "未知的頁面操作");
}

PageEditResult PageEditor::insertBlankPages(int atIndex, int count, double widthPt,
                                            double heightPt) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen()) return fail(PageEditStatus::NotOpen, describe(PageEditStatus::NotOpen));

    const dom::InsertBlankPages op{atIndex, count, widthPt, heightPt};
    const dom::OperationStatus validation = dom::validate(op, pageCount());
    if (validation != dom::OperationStatus::Ok) return invalid(validation);

    for (int i = 0; i < count; ++i) {
        FPDF_PAGE page = FPDFPage_New(impl_->document, atIndex + i, widthPt, heightPt);
        if (!page) {
            return fail(PageEditStatus::PdfiumRejected, "FPDFPage_New 回傳空把手");
        }
        FPDF_ClosePage(page);
    }
    impl_->structural = true;
    return {};
}

PageEditResult PageEditor::deletePages(const std::vector<int>& pages) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen()) return fail(PageEditStatus::NotOpen, describe(PageEditStatus::NotOpen));

    const dom::OperationStatus validation = dom::validate(dom::DeletePages{pages}, pageCount());
    if (validation != dom::OperationStatus::Ok) return invalid(validation);

    // 由大到小刪，否則刪掉前面的頁之後，後面的索引全部往前位移一格，
    // 使用者選的第 5 頁會變成刪到第 6 頁。
    std::vector<int> ordered = pages;
    std::sort(ordered.begin(), ordered.end(), std::greater<int>());
    for (const int page : ordered) {
        FPDFPage_Delete(impl_->document, page);
    }
    impl_->structural = true;
    return {};
}

PageEditResult PageEditor::rotatePages(const std::vector<int>& pages, dom::PageRotation rotation,
                                       bool relative) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen()) return fail(PageEditStatus::NotOpen, describe(PageEditStatus::NotOpen));

    const dom::OperationStatus validation =
        dom::validate(dom::RotatePages{pages, rotation, relative}, pageCount());
    if (validation != dom::OperationStatus::Ok) return invalid(validation);

    for (const int index : pages) {
        ScopedPage page(impl_->document, index);
        if (!page) return fail(PageEditStatus::PageLoadFailed, "無法載入第 " +
                                                                  std::to_string(index + 1) + " 頁");
        dom::PageRotation target = rotation;
        if (relative) {
            const int current = FPDFPage_GetRotation(page.get());
            // -1 代表 PDFium 讀不出來；當成未旋轉處理會把使用者原本的角度吃掉，
            // 所以直接回報失敗。
            if (current < 0) {
                return fail(PageEditStatus::PdfiumRejected, "FPDFPage_GetRotation 失敗");
            }
            target = dom::combine(dom::rotationFromQuarterTurns(current), rotation);
        }
        FPDFPage_SetRotation(page.get(), dom::quarterTurns(target));
    }
    // 旋轉只改頁面字典的 /Rotate，不動頁面樹，因此不算結構性變更：
    // 這種文件仍有機會真正增量儲存並保住既有簽章。
    return {};
}

PageEditResult PageEditor::movePages(const std::vector<int>& pages, int destinationIndex) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen()) return fail(PageEditStatus::NotOpen, describe(PageEditStatus::NotOpen));

    const dom::OperationStatus validation =
        dom::validate(dom::MovePages{pages, destinationIndex}, pageCount());
    if (validation != dom::OperationStatus::Ok) return invalid(validation);

    if (FPDF_MovePages(impl_->document, pages.data(),
                       static_cast<unsigned long>(pages.size()), destinationIndex) == 0) {
        return fail(PageEditStatus::PdfiumRejected, "FPDF_MovePages 回報失敗，文件可能已處於不確定狀態");
    }
    impl_->structural = true;
    return {};
}

PageEditResult PageEditor::duplicatePages(const std::vector<int>& pages, int destinationIndex) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen()) return fail(PageEditStatus::NotOpen, describe(PageEditStatus::NotOpen));

    const dom::OperationStatus validation =
        dom::validate(dom::DuplicatePages{pages, destinationIndex}, pageCount());
    if (validation != dom::OperationStatus::Ok) return invalid(validation);

    // PDFium 沒有「複製頁面」這支 API，而把 FPDF_ImportPagesByIndex 的來源與目標
    // 指向同一份文件，等於在走訪頁面樹的同時改動它。改走一份暫存文件當跳板：
    // 先把選取的頁匯出去，再整批匯回來，兩次都是不同文件之間的搬運。
    ScopedNewDocument scratch;
    if (!scratch) return fail(PageEditStatus::PdfiumRejected, "FPDF_CreateNewDocument 失敗");

    if (FPDF_ImportPagesByIndex(scratch.get(), impl_->document, pages.data(),
                                static_cast<unsigned long>(pages.size()), 0) == 0) {
        return fail(PageEditStatus::PdfiumRejected, "FPDF_ImportPagesByIndex（匯出複本）失敗");
    }
    if (FPDF_ImportPagesByIndex(impl_->document, scratch.get(), nullptr, 0, destinationIndex) == 0) {
        return fail(PageEditStatus::PdfiumRejected, "FPDF_ImportPagesByIndex（匯回複本）失敗");
    }
    impl_->structural = true;
    return {};
}

PageEditResult PageEditor::swapPages(int first, int second) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen()) return fail(PageEditStatus::NotOpen, describe(PageEditStatus::NotOpen));

    const dom::OperationStatus validation = dom::validate(dom::SwapPages{first, second}, pageCount());
    if (validation != dom::OperationStatus::Ok) return invalid(validation);
    if (first == second) return {};

    const int low = std::min(first, second);
    const int high = std::max(first, second);

    // 兩次搬移即可交換：先把後面那頁移到前面那頁的位置（原本的前頁因此被推到 low+1），
    // 再把它移到後面那頁原本的位置。
    const int toFront = high;
    if (FPDF_MovePages(impl_->document, &toFront, 1, low) == 0) {
        return fail(PageEditStatus::PdfiumRejected, "FPDF_MovePages 失敗（交換第一步）");
    }
    const int toBack = low + 1;
    if (FPDF_MovePages(impl_->document, &toBack, 1, high) == 0) {
        return fail(PageEditStatus::PdfiumRejected, "FPDF_MovePages 失敗（交換第二步）");
    }
    impl_->structural = true;
    return {};
}

PageEditResult PageEditor::reversePages() {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen()) return fail(PageEditStatus::NotOpen, describe(PageEditStatus::NotOpen));

    const int count = pageCount();
    if (count <= 1) return {};

    // 一次 FPDF_MovePages 傳入整份反序清單看似更快，但它失敗時「文件可能處於不確定
    // 狀態」（PDFium 標頭原文），而反轉整份文件正是最不能容忍半成品的操作。
    // 逐對交換每一步都是可還原的最小異動。
    for (int i = 0; i < count / 2; ++i) {
        const PageEditResult step = swapPages(i, count - 1 - i);
        if (!step.ok()) return step;
    }
    impl_->structural = true;
    return {};
}

std::optional<dom::PageRotation> PageEditor::rotation(int pageIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen() || pageIndex < 0 || pageIndex >= pageCount()) return std::nullopt;
    ScopedPage page(impl_->document, pageIndex);
    if (!page) return std::nullopt;
    const int value = FPDFPage_GetRotation(page.get());
    if (value < 0) return std::nullopt;
    return dom::rotationFromQuarterTurns(value);
}

std::optional<domain::SizeF> PageEditor::pageSize(int pageIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen() || pageIndex < 0 || pageIndex >= pageCount()) return std::nullopt;
    ScopedPage page(impl_->document, pageIndex);
    if (!page) return std::nullopt;
    return domain::SizeF{static_cast<double>(FPDF_GetPageWidthF(page.get())),
                         static_cast<double>(FPDF_GetPageHeightF(page.get()))};
}

std::optional<domain::RectF> PageEditor::mediaBox(int pageIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen() || pageIndex < 0 || pageIndex >= pageCount()) return std::nullopt;
    ScopedPage page(impl_->document, pageIndex);
    if (!page) return std::nullopt;
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    if (FPDFPage_GetMediaBox(page.get(), &left, &bottom, &right, &top) == 0) return std::nullopt;
    return domain::RectF{left, bottom, right, top};
}

std::optional<domain::RectF> PageEditor::cropBox(int pageIndex) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen() || pageIndex < 0 || pageIndex >= pageCount()) return std::nullopt;
    ScopedPage page(impl_->document, pageIndex);
    if (!page) return std::nullopt;
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    // 沒有 /CropBox 時 PDFium 回傳 false（而不是替我們填 MediaBox）。
    // 那是「未設定」而不是錯誤，呼叫端要看得出差別。
    if (FPDFPage_GetCropBox(page.get(), &left, &bottom, &right, &top) == 0) return std::nullopt;
    return domain::RectF{left, bottom, right, top};
}

PageEditResult PageEditor::setCropBox(int pageIndex, const domain::RectF& box) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen()) return fail(PageEditStatus::NotOpen, describe(PageEditStatus::NotOpen));
    if (pageIndex < 0 || pageIndex >= pageCount()) {
        return invalid(dom::OperationStatus::IndexOutOfRange);
    }
    if (box.isEmpty()) return invalid(dom::OperationStatus::InvalidGeometry);

    ScopedPage page(impl_->document, pageIndex);
    if (!page) return fail(PageEditStatus::PageLoadFailed, "無法載入頁面");

    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    domain::RectF bounds{0.0, 0.0, FPDF_GetPageWidthF(page.get()), FPDF_GetPageHeightF(page.get())};
    if (FPDFPage_GetMediaBox(page.get(), &left, &bottom, &right, &top) != 0) {
        bounds = domain::RectF{std::min(left, right), std::min(bottom, top), std::max(left, right),
                               std::max(bottom, top)};
    }

    const domain::RectF clamped = clampToBox(box, bounds);
    if (clamped.isEmpty()) return invalid(dom::OperationStatus::InvalidGeometry);

    FPDFPage_SetCropBox(page.get(), static_cast<float>(clamped.left),
                        static_cast<float>(clamped.bottom), static_cast<float>(clamped.right),
                        static_cast<float>(clamped.top));
    return {};
}

std::optional<domain::RectF> PageEditor::detectContentBounds(
    int pageIndex, const ContentBoundsOptions& options) const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen() || pageIndex < 0 || pageIndex >= pageCount()) return std::nullopt;
    if (options.maxEdgePixels <= 0) return std::nullopt;

    ScopedPage page(impl_->document, pageIndex);
    if (!page) return std::nullopt;

    const double widthPt = FPDF_GetPageWidthF(page.get());
    const double heightPt = FPDF_GetPageHeightF(page.get());
    if (widthPt <= 0.0 || heightPt <= 0.0) return std::nullopt;

    const double scale = static_cast<double>(options.maxEdgePixels) / std::max(widthPt, heightPt);
    const int pixelWidth = std::max(1, static_cast<int>(widthPt * scale));
    const int pixelHeight = std::max(1, static_cast<int>(heightPt * scale));

    FPDF_BITMAP bitmap = FPDFBitmap_Create(pixelWidth, pixelHeight, 0);
    if (!bitmap) return std::nullopt;
    // 白底是「白邊」這個概念的前提：透明背景在掃描時無從判斷，先填白再說。
    FPDFBitmap_FillRect(bitmap, 0, 0, pixelWidth, pixelHeight, 0xFFFFFFFF);
    const int flags = options.includeAnnotations ? FPDF_ANNOT : 0;
    FPDF_RenderPageBitmap(bitmap, page.get(), 0, 0, pixelWidth, pixelHeight, 0, flags);

    const auto* buffer = static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
    const int stride = FPDFBitmap_GetStride(bitmap);
    const int threshold = std::clamp(options.whiteThreshold, 0, 255);

    int minX = pixelWidth;
    int minY = pixelHeight;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < pixelHeight; ++y) {
        const unsigned char* row = buffer + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < pixelWidth; ++x) {
            const unsigned char* px = row + static_cast<std::size_t>(x) * 4;
            if (px[0] >= threshold && px[1] >= threshold && px[2] >= threshold) continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    FPDFBitmap_Destroy(bitmap);

    if (maxX < 0) return std::nullopt;  // 整頁皆白：沒有內容可以框

    // 像素是有面積的：非白像素涵蓋 [x, x+1)，所以右下角要取到 maxX + 1。
    // 座標換算交給 FPDF_DeviceToPage，因為它會一併處理 /Rotate 與 CropBox 位移，
    // 自己手算等於把 PDFium 的頁面矩陣重寫一遍。
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    const bool okTopLeft = FPDF_DeviceToPage(page.get(), 0, 0, pixelWidth, pixelHeight, 0, minX,
                                             minY, &x0, &y0) != 0;
    const bool okBottomRight = FPDF_DeviceToPage(page.get(), 0, 0, pixelWidth, pixelHeight, 0,
                                                 maxX + 1, maxY + 1, &x1, &y1) != 0;
    if (!okTopLeft || !okBottomRight) return std::nullopt;

    domain::RectF bounds{std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
    if (options.marginPt > 0.0) {
        bounds.left -= options.marginPt;
        bounds.bottom -= options.marginPt;
        bounds.right += options.marginPt;
        bounds.top += options.marginPt;
    }
    return bounds;
}

PageEditResult PageEditor::cropToContent(const std::vector<int>& pages,
                                         const ContentBoundsOptions& options, int* croppedPages) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    if (!isOpen()) return fail(PageEditStatus::NotOpen, describe(PageEditStatus::NotOpen));

    if (pages.empty()) return invalid(dom::OperationStatus::EmptySelection);
    for (const int index : pages) {
        if (index < 0 || index >= pageCount()) return invalid(dom::OperationStatus::IndexOutOfRange);
    }

    int cropped = 0;
    for (const int index : pages) {
        const std::optional<domain::RectF> bounds = detectContentBounds(index, options);
        if (!bounds) continue;  // 空白頁跳過；把它裁成 0 大小只會產生壞檔案
        const PageEditResult result = setCropBox(index, *bounds);
        if (!result.ok()) return result;
        ++cropped;
    }
    if (croppedPages) *croppedPages = cropped;
    return {};
}

PageSaveResult PageEditor::save(const std::string& targetPath, const save::SaveOptions& options) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    PageSaveResult result;
    result.structural = impl_->structural;
    if (!isOpen()) {
        result.save.status = save::SaveStatus::InvalidDocument;
        result.save.message = "尚未開啟文件";
        return result;
    }
    if (impl_->sourcePath.empty()) {
        // 新建文件沒有原始位元組可以保留，增量儲存無從談起。改走 saveAsCopy 是
        // 呼叫端的決定，不由這裡替它決定。
        result.save.status = save::SaveStatus::SourceUnreadable;
        result.save.message = "此文件非由檔案開啟，無法增量儲存，請改用 saveAsCopy";
        return result;
    }
    result.save = save::IncrementalSaver::saveIncremental(impl_->document, impl_->sourcePath,
                                                          targetPath, options);
    return result;
}

PageSaveResult PageEditor::saveAsCopy(const std::string& targetPath,
                                      const save::SaveOptions& options) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    PageSaveResult result;
    result.structural = impl_->structural;
    if (!isOpen()) {
        result.save.status = save::SaveStatus::InvalidDocument;
        result.save.message = "尚未開啟文件";
        return result;
    }
    result.save = save::IncrementalSaver::saveAsCopy(impl_->document, targetPath, options);
    // 整份重寫是這條路徑的定義，不是退回；但欄位語意要一致，讓上層只看這一個旗標。
    result.save.fullRewriteFallback = true;
    return result;
}

std::vector<std::uint64_t> PageEditor::estimatePageBytes() const {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    std::vector<std::uint64_t> sizes;
    if (!isOpen()) return sizes;

    const int count = pageCount();
    sizes.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        ScopedNewDocument single;
        if (!single) {
            sizes.push_back(0);
            continue;
        }
        const int index = i;
        if (FPDF_ImportPagesByIndex(single.get(), impl_->document, &index, 1, 0) == 0) {
            sizes.push_back(0);
            continue;
        }
        SizeSink sink;
        sink.base.version = 1;
        sink.base.WriteBlock = &measureBlock;
        FPDF_SaveWithVersion(single.get(), &sink.base, FPDF_NO_INCREMENTAL, 17);
        sizes.push_back(sink.size);
    }
    return sizes;
}

// ---------------------------------------------------------------------------
// 擷取／合併／分割
// ---------------------------------------------------------------------------

std::string formatSplitPath(const std::string& pattern, int oneBasedIndex) {
    const std::string number = std::to_string(oneBasedIndex);
    const std::size_t placeholder = pattern.find("{n}");
    if (placeholder != std::string::npos) {
        std::string out = pattern;
        out.replace(placeholder, 3, number);
        return out;
    }
    // 沒有佔位符時把序號插在副檔名之前，而不是接在最後面——"a.pdf3" 不是 PDF。
    const std::size_t slash = pattern.find_last_of("/\\");
    const std::size_t dot = pattern.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return pattern.substr(0, dot) + "-" + number + pattern.substr(dot);
    }
    return pattern + "-" + number;
}

AssemblyResult extractPages(const std::string& sourcePath, const std::vector<int>& pages,
                            const std::string& targetPath, const std::string& password) {
    AssemblyResult result;
    if (pages.empty()) {
        result.status = PageEditStatus::InvalidArgument;
        result.message = dom::describe(dom::OperationStatus::EmptySelection);
        return result;
    }

    PageEditor source;
    if (!source.open(sourcePath, password)) {
        result.status = PageEditStatus::SourceUnreadable;
        result.message = "無法開啟 " + sourcePath;
        return result;
    }
    const int total = source.pageCount();
    for (const int page : pages) {
        if (page < 0 || page >= total) {
            result.status = PageEditStatus::InvalidArgument;
            result.message = dom::describe(dom::OperationStatus::IndexOutOfRange);
            return result;
        }
    }

    PageEditor target;
    if (!target.createEmpty()) {
        result.status = PageEditStatus::PdfiumRejected;
        result.message = "FPDF_CreateNewDocument 失敗";
        return result;
    }

    FPDF_DOCUMENT dest = toDocument(target.documentHandle());
    FPDF_DOCUMENT src = toDocument(source.documentHandle());
    {
        const PdfiumGuard guard;  // 行程級序列化（ADR-005）：涵蓋這一件「搬頁面」工作。
        if (FPDF_ImportPagesByIndex(dest, src, pages.data(), static_cast<unsigned long>(pages.size()),
                                    0) == 0) {
            result.status = PageEditStatus::PdfiumRejected;
            result.message = "FPDF_ImportPagesByIndex 失敗";
            return result;
        }
        FPDF_CopyViewerPreferences(dest, src);
    }

    result.forms.widgetAnnotations = countWidgetAnnotations(dest);
    result.pageCount = target.pageCount();
    result.save = target.saveAsCopy(targetPath);
    if (!result.save.ok()) {
        result.status = PageEditStatus::SaveFailed;
        result.message = result.save.save.message;
    }
    return result;
}

AssemblyResult mergeDocuments(const std::vector<std::string>& sourcePaths,
                              const std::string& targetPath) {
    AssemblyResult result;
    if (sourcePaths.empty()) {
        result.status = PageEditStatus::InvalidArgument;
        result.message = "沒有任何來源檔案";
        return result;
    }

    PageEditor target;
    if (!target.createEmpty()) {
        result.status = PageEditStatus::PdfiumRejected;
        result.message = "FPDF_CreateNewDocument 失敗";
        return result;
    }
    FPDF_DOCUMENT dest = toDocument(target.documentHandle());

    bool copiedPreferences = false;
    for (const std::string& path : sourcePaths) {
        PageEditor source;
        if (!source.open(path)) {
            result.status = PageEditStatus::SourceUnreadable;
            result.message = "無法開啟 " + path;
            return result;
        }
        FPDF_DOCUMENT src = toDocument(source.documentHandle());
        {
            const PdfiumGuard guard;  // 行程級序列化（ADR-005）：涵蓋這一件「搬頁面」工作。
            // 一律附加在尾端：合併的順序就是使用者給的順序，不重新排列。
            const int insertAt = FPDF_GetPageCount(dest);
            if (FPDF_ImportPagesByIndex(dest, src, nullptr, 0, insertAt) == 0) {
                result.status = PageEditStatus::PdfiumRejected;
                result.message = "FPDF_ImportPagesByIndex 失敗：" + path;
                return result;
            }
            if (!copiedPreferences) {
                // 檢視器偏好只能有一份，取第一份文件的；後面的會被無聲蓋掉才是意外。
                FPDF_CopyViewerPreferences(dest, src);
                copiedPreferences = true;
            }
        }
    }

    result.forms.widgetAnnotations = countWidgetAnnotations(dest);
    result.pageCount = target.pageCount();
    result.save = target.saveAsCopy(targetPath);
    if (!result.save.ok()) {
        result.status = PageEditStatus::SaveFailed;
        result.message = result.save.save.message;
    }
    return result;
}

SplitResult splitDocument(const std::string& sourcePath, const dom::SplitRule& rule,
                          const std::string& targetPattern, const std::string& password) {
    SplitResult result;

    PageEditor source;
    if (!source.open(sourcePath, password)) {
        result.status = PageEditStatus::SourceUnreadable;
        result.message = "無法開啟 " + sourcePath;
        return result;
    }

    std::vector<std::uint64_t> pageBytes;
    if (rule.mode == dom::SplitMode::MaxBytes) pageBytes = source.estimatePageBytes();

    result.plan = dom::planSplit(rule, source.pageCount(), pageBytes);
    if (!result.plan.ok()) {
        result.status = PageEditStatus::InvalidArgument;
        result.message = "分割規則不合法";
        return result;
    }

    FPDF_DOCUMENT src = toDocument(source.documentHandle());
    int fileIndex = 0;
    for (const dom::SplitChunk& chunk : result.plan.chunks) {
        ++fileIndex;
        std::vector<int> indices;
        indices.reserve(static_cast<std::size_t>(chunk.count()));
        for (int page = chunk.first; page <= chunk.last; ++page) indices.push_back(page);

        PageEditor target;
        if (!target.createEmpty()) {
            result.status = PageEditStatus::PdfiumRejected;
            result.message = "FPDF_CreateNewDocument 失敗";
            return result;
        }
        FPDF_DOCUMENT dest = toDocument(target.documentHandle());
        {
            const PdfiumGuard guard;  // 行程級序列化（ADR-005）：涵蓋這一件「搬頁面」工作。
            if (FPDF_ImportPagesByIndex(dest, src, indices.data(),
                                        static_cast<unsigned long>(indices.size()), 0) == 0) {
                result.status = PageEditStatus::PdfiumRejected;
                result.message = "FPDF_ImportPagesByIndex 失敗（第 " + std::to_string(fileIndex) + " 份）";
                return result;
            }
            FPDF_CopyViewerPreferences(dest, src);
        }

        const std::string path = formatSplitPath(targetPattern, fileIndex);
        const PageSaveResult saved = target.saveAsCopy(path);
        if (!saved.ok()) {
            result.status = PageEditStatus::SaveFailed;
            result.message = saved.save.message;
            return result;
        }
        result.outputs.push_back(SplitOutput{path, chunk, target.pageCount()});
    }
    return result;
}

}  // namespace alioth::engine::pages
