#include "engine/enhance/scan_enhancer.h"

#include <algorithm>
#include <utility>

#include "engine/enhance/image_codec.h"
#include "engine/enhance/image_ops.h"
#include "engine/enhance/page_rasterizer.h"
#include "engine/enhance/pdf_image_writer.h"

namespace alioth::engine::enhance {
namespace {

constexpr const char* kImageResource = "AliothScanIm";

}  // namespace

ScanEnhanceResult enhanceScannedPages(objects::IncrementalAppender& appender,
                                      const std::vector<std::int32_t>& pages,
                                      const ScanEnhanceSettings& settings) {
    ScanEnhanceResult result;
    if (!appender.isOpen()) {
        result.diagnostic = "附加器尚未開啟";
        return result;
    }
    if (settings.dpi < 36.0 || settings.dpi > 1200.0 || !settings.compression.valid()) {
        result.diagnostic = "掃描增強設定不合法";
        return result;
    }

    const std::string& sourceBytes = appender.source().bytes();
    const auto pageCount = static_cast<std::int32_t>(appender.source().pages().size());

    for (std::int32_t index = 0; index < pageCount; ++index) {
        if (!pages.empty() && std::find(pages.begin(), pages.end(), index) == pages.end()) continue;

        const objects::PdfRef page = appender.source().pages()[static_cast<std::size_t>(index)];
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

        PageEnhanceReport report;
        report.pageIndex = index;

        if (settings.deskewEnabled) {
            const domain::enhance::DeskewResult skew = detectSkew(rendered.pixels, settings.deskew);
            report.angleDeg = skew.angleDeg;
            report.deskewNote = skew.note;
            if (skew.detected) {
                rendered.pixels = rotate(rendered.pixels,
                                         domain::enhance::correctionAngleDeg(skew),
                                         settings.deskewBackground);
                report.deskewApplied = true;
            }
        } else {
            report.deskewNote = "去斜未啟用";
        }

        // 增強一律在去斜之後：先二值化再旋轉的話，旋轉的雙線性取樣會在
        // 純黑與純白之間插出灰階，二值化的結果就白做了。
        PixelBuffer processed = applyEnhancement(rendered.pixels, settings.enhancement);
        if (processed.isNull()) {
            result.diagnostic = "增強處理失敗";
            return result;
        }

        const EncodedImage encoded = encodeImage(processed, settings.compression);
        if (!encoded.ok) {
            result.diagnostic = encoded.diagnostic;
            return result;
        }
        report.imageBytes = encoded.byteSize();

        if (!writePageAsImage(appender, page, box, encoded, kImageResource, &result.diagnostic)) {
            return result;
        }
        result.pages.push_back(std::move(report));
    }

    result.ok = true;
    return result;
}

}  // namespace alioth::engine::enhance
