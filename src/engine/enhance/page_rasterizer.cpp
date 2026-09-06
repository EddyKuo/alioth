#include "engine/enhance/page_rasterizer.h"
#include "engine/pdfium_lock.h"

#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "engine/enhance/image_codec.h"
#include "engine/enhance/image_ops.h"
#include "engine/enhance/pdf_image_writer.h"
#include "engine/objects/page_object_editor.h"
#include "engine/objects/pdf_object.h"
#include "engine/pdfium_library.h"

namespace alioth::engine::enhance {
namespace {

using domain::enhance::RasterizeSettings;
using objects::PdfObject;
using objects::PdfRef;

constexpr const char* kImageResource = "AliothRasterIm";

// 90 度的整數倍旋轉。用專門的函式而不是共用的雙線性 rotate()：
// 直角旋轉是像素的重新排列，不需要取樣，因此完全無損；
// 走取樣路徑會讓每一頁都平白糊掉一層。turns 以順時針計。
[[nodiscard]] PixelBuffer rotateQuarterTurns(const PixelBuffer& source, int turns) {
    const int normalized = ((turns % 4) + 4) % 4;
    if (source.isNull() || normalized == 0) return clonePixels(source);

    const std::int32_t w = source.width();
    const std::int32_t h = source.height();
    const bool swapped = normalized % 2 == 1;
    PixelBuffer out(swapped ? h : w, swapped ? w : h);

    for (std::int32_t y = 0; y < h; ++y) {
        const std::uint8_t* src = source.data() + source.stride() * static_cast<std::size_t>(y);
        for (std::int32_t x = 0; x < w; ++x) {
            std::int32_t dx = 0;
            std::int32_t dy = 0;
            switch (normalized) {
                case 1: dx = h - 1 - y; dy = x; break;
                case 2: dx = w - 1 - x; dy = h - 1 - y; break;
                default: dx = y; dy = w - 1 - x; break;
            }
            std::uint8_t* dst = out.scanline(dy) + static_cast<std::size_t>(dx) * 4;
            std::memcpy(dst, src + static_cast<std::size_t>(x) * 4, 4);
        }
    }
    return out;
}

[[nodiscard]] int pageRotationQuarters(const objects::PdfSourceDocument& source,
                                       const PdfRef& page) {
    const PdfObject value = source.resolve(source.inheritedPageAttribute(page, "Rotate"));
    if (!value.isNumber()) return 0;
    const auto degrees = static_cast<int>(std::lround(value.asNumber()));
    return ((degrees / 90) % 4 + 4) % 4;
}

// FPDF_DOCUMENT 的 RAII。PDFium 的把手漏掉不會當場出錯，
// 但同一個行程裡連續處理幾百頁就會把記憶體吃光。
class MemoryDocument {
public:
    explicit MemoryDocument(const std::string& bytes)
        : document_(FPDF_LoadMemDocument64(bytes.data(),
                                           static_cast<size_t>(bytes.size()), nullptr)) {}
    ~MemoryDocument() {
        if (document_ != nullptr) FPDF_CloseDocument(document_);
    }

    MemoryDocument(const MemoryDocument&) = delete;
    MemoryDocument& operator=(const MemoryDocument&) = delete;

    [[nodiscard]] FPDF_DOCUMENT get() const noexcept { return document_; }
    [[nodiscard]] bool valid() const noexcept { return document_ != nullptr; }

private:
    FPDF_DOCUMENT document_{nullptr};
};

}  // namespace

RasterizedPage renderPage(const std::string& pdfBytes, std::int32_t pageIndex, double dpi,
                          bool includeAnnotations) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    RasterizedPage result;
    if (pdfBytes.empty() || dpi <= 0.0) {
        result.diagnostic = "輸入為空或 dpi 不合法";
        return result;
    }

    PdfiumRuntime runtime;
    MemoryDocument document(pdfBytes);
    if (!document.valid()) {
        result.diagnostic = "PDFium 無法載入位元組";
        return result;
    }
    if (pageIndex < 0 || pageIndex >= FPDF_GetPageCount(document.get())) {
        result.diagnostic = "頁碼超出範圍";
        return result;
    }

    FPDF_PAGE page = FPDF_LoadPage(document.get(), pageIndex);
    if (page == nullptr) {
        result.diagnostic = "無法載入頁面";
        return result;
    }

    // FPDF_GetPageWidthF 給的是**顯示後**的寬度（已套用 /Rotate），
    // 因此直接拿來配置點陣圖是對的；轉回使用者空間是之後的事。
    const double displayWidthPt = FPDF_GetPageWidthF(page);
    const double displayHeightPt = FPDF_GetPageHeightF(page);
    const std::int32_t width = domain::enhance::pixelsForPoints(displayWidthPt, dpi);
    const std::int32_t height = domain::enhance::pixelsForPoints(displayHeightPt, dpi);
    if (width <= 0 || height <= 0) {
        FPDF_ClosePage(page);
        result.diagnostic = "頁面尺寸不合法";
        return result;
    }

    PixelBuffer buffer = makeBuffer(width, height, 255);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, buffer.data(),
                                             static_cast<int>(buffer.stride()));
    if (bitmap == nullptr) {
        FPDF_ClosePage(page);
        result.diagnostic = "無法建立點陣圖";
        return result;
    }
    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
    // 預設不帶 FPDF_ANNOT：註解是獨立物件並且會被保留，烤進圖裡會變成兩份。
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0,
                          includeAnnotations ? FPDF_ANNOT : 0);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);

    result.pixels = std::move(buffer);
    result.box = domain::RectF{0.0, 0.0, displayWidthPt, displayHeightPt};
    result.ok = true;
    return result;
}

SnapshotResult renderSnapshot(const std::string& pdfBytes, std::int32_t pageIndex,
                              const SnapshotArea& area, double dpi, bool includeAnnotations) {
    const PdfiumGuard guard;  // 行程級序列化（ADR-005）
    SnapshotResult result;
    if (pdfBytes.empty() || dpi <= 0.0) {
        result.diagnostic = "輸入為空或 dpi 不合法";
        return result;
    }

    PdfiumRuntime runtime;
    MemoryDocument document(pdfBytes);
    if (!document.valid()) {
        result.diagnostic = "PDFium 無法載入位元組";
        return result;
    }
    if (pageIndex < 0 || pageIndex >= FPDF_GetPageCount(document.get())) {
        result.diagnostic = "頁碼超出範圍";
        return result;
    }

    FPDF_PAGE page = FPDF_LoadPage(document.get(), pageIndex);
    if (page == nullptr) {
        result.diagnostic = "無法載入頁面";
        return result;
    }

    // 與 renderPage 同一個座標系：FPDF_GetPageWidthF/HeightF 給的是顯示後
    // （已套用 /Rotate）的頁面尺寸，因此可以直接把 area 拿來與它相交而不必
    // 自己再算一次旋轉矩陣。
    const double displayWidthPt = FPDF_GetPageWidthF(page);
    const double displayHeightPt = FPDF_GetPageHeightF(page);

    SnapshotArea clipped{
        std::max(0.0, area.left),
        std::max(0.0, area.top),
        std::min(displayWidthPt, area.right),
        std::min(displayHeightPt, area.bottom),
    };
    if (clipped.isEmpty()) {
        FPDF_ClosePage(page);
        result.diagnostic = "框選範圍與頁面不相交";
        return result;
    }

    const std::int32_t fullWidthPx = domain::enhance::pixelsForPoints(displayWidthPt, dpi);
    const std::int32_t fullHeightPx = domain::enhance::pixelsForPoints(displayHeightPt, dpi);
    const std::int32_t left = domain::enhance::pixelsForPoints(clipped.left, dpi);
    const std::int32_t top = domain::enhance::pixelsForPoints(clipped.top, dpi);
    const std::int32_t width = domain::enhance::pixelsForPoints(clipped.width(), dpi);
    const std::int32_t height = domain::enhance::pixelsForPoints(clipped.height(), dpi);
    if (fullWidthPx <= 0 || fullHeightPx <= 0 || width <= 0 || height <= 0) {
        FPDF_ClosePage(page);
        result.diagnostic = "框選範圍換算後的像素尺寸不合法";
        return result;
    }

    PixelBuffer buffer = makeBuffer(width, height, 255);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, buffer.data(),
                                             static_cast<int>(buffer.stride()));
    if (bitmap == nullptr) {
        FPDF_ClosePage(page);
        result.diagnostic = "無法建立點陣圖";
        return result;
    }
    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);

    // 整頁仍以 fullWidthPx × fullHeightPx 渲染（PDFium 的內容串流解析成本無法
    // 只算框選範圍），但用負的 start_x/start_y 把整頁位圖平移，讓只有框選範圍
    // 落在我們配置的 width×height 緩衝區內——緩衝區外的像素 PDFium 根本不會寫，
    // 因此逐像素成本與記憶體用量都只跟框選範圍成正比，不是整頁。
    FPDF_RenderPageBitmap(bitmap, page, -left, -top, fullWidthPx, fullHeightPx, 0,
                          includeAnnotations ? FPDF_ANNOT : 0);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);

    result.pixels = std::move(buffer);
    result.clippedArea = clipped;
    result.ok = true;
    return result;
}

PixelBuffer orientToUserSpace(const objects::PdfSourceDocument& source, const PdfRef& page,
                              const PixelBuffer& rendered) {
    // /Rotate 是顯示時才套用的，內容串流畫在使用者空間。轉回去之後，
    // 繪製矩陣就只是單純的縮放平移，而 /Rotate 保持原值——
    // 註解的座標因此完全不受影響。
    const int quarters = pageRotationQuarters(source, page);
    return rotateQuarterTurns(rendered, (4 - quarters) % 4);
}

bool writePageAsImage(objects::IncrementalAppender& appender, const PdfRef& page,
                      const domain::RectF& box, const EncodedImage& image,
                      const std::string& resourceName, std::string* diagnostic) {
    const ImageWriteResult written = writeImageObject(appender, image);
    if (!written.ok) {
        if (diagnostic != nullptr) *diagnostic = written.diagnostic;
        return false;
    }

    objects::PdfStream stream;
    stream.data = drawImageContent(resourceName, box);
    const int contentObject = appender.allocateObject();
    appender.setObject(contentObject, PdfObject{std::move(stream)});

    const objects::PageEditStatus status = objects::setPageResource(
        appender, page, "XObject", resourceName, objects::makeRef(written.imageObject));
    if (!status.ok) {
        if (diagnostic != nullptr) *diagnostic = status.diagnostic;
        return false;
    }

    if (!replacePageContent(appender, page, contentObject)) {
        if (diagnostic != nullptr) *diagnostic = "無法取代頁面內容";
        return false;
    }
    return true;
}

RasterizeResult rasterizePages(objects::IncrementalAppender& appender,
                               const std::vector<std::int32_t>& pages,
                               const RasterizeSettings& settings) {
    RasterizeResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "附加器尚未開啟";
        return result;
    }
    if (!settings.valid()) {
        result.diagnostic = "點陣化設定不合法";
        return result;
    }

    const std::string& sourceBytes = appender.source().bytes();
    const auto pageCount = static_cast<std::int32_t>(appender.source().pages().size());

    for (std::int32_t index = 0; index < pageCount; ++index) {
        if (!pages.empty() && std::find(pages.begin(), pages.end(), index) == pages.end()) continue;

        PdfRef page{};
        if (!objects::pageRefAt(appender, index, page)) continue;

        domain::RectF box{};
        if (!pageBox(appender.source(), page, box)) {
            result.diagnostic = "頁面缺少可用的 /MediaBox";
            return result;
        }

        RasterizedPage rendered = renderPage(sourceBytes, index, settings.dpi);
        if (!rendered.ok) {
            result.diagnostic = rendered.diagnostic;
            return result;
        }
        rendered.pixels = orientToUserSpace(appender.source(), page, rendered.pixels);

        const EncodedImage encoded = encodeImage(rendered.pixels, settings.compression);
        if (!encoded.ok) {
            result.diagnostic = encoded.diagnostic;
            return result;
        }

        if (!writePageAsImage(appender, page, box, encoded, kImageResource, &result.diagnostic)) {
            return result;
        }

        result.imageBytes += encoded.byteSize();
        result.pagesChanged.push_back(index);
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::enhance
