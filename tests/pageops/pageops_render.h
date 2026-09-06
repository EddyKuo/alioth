#pragma once

// 以真正的渲染結果驗收（WBS 12）。
//
// 圖形狀態污染與上下層順序這兩件事，看內容串流是驗不出來的：串流的形狀對，
// 不代表檢視器畫出來的結果對，而那個差距正是這兩項的全部風險所在。
// 因此這裡走 PdfiumEngine 的縮圖路徑取回像素——縮圖是 CLAUDE.md 明列的
// 兩個允許整頁光柵化的例外之一，且這是離線、一次性、低解析度的用途。

#include <QString>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#include "domain/geometry.h"
#include "engine/cancellation.h"
#include "engine/pdfium_engine.h"
#include "pageops_fixture.h"

namespace alioth::test::pageops {

inline engine::PixelBufferPtr renderPage(const std::string& bytes, const QString& directory,
                                         int pageIndex, int maxEdgePixels) {
    const QString path = directory + QStringLiteral("/render.pdf");
    if (!writeBytes(path, bytes)) return nullptr;

    engine::PdfiumEngine engine;
    std::atomic<bool> opened{false};
    engine.openDocument(path.toStdString(), "",
                        [&opened](engine::OpenResult result) { opened = result.ok(); });
    engine.waitForIdle();
    if (!opened.load()) return nullptr;

    engine::PixelBufferPtr buffer;
    engine.renderThumbnail(pageIndex, maxEdgePixels, engine::CancellationToken{},
                           [&buffer](engine::RenderResult result) {
                               if (result.ok()) buffer = result.buffer;
                           });
    engine.waitForIdle();
    return buffer;
}

struct Pixel {
    int b{0};
    int g{0};
    int r{0};
};

// 取樣點以頁面座標給定。Y 軸在此翻轉一次——位圖原點在左上、頁面原點在左下。
inline Pixel samplePage(const engine::PixelBuffer& buffer, const domain::SizeF& pageSize,
                        const domain::PointF& pagePoint) {
    const double scaleX = static_cast<double>(buffer.width()) / pageSize.width;
    const double scaleY = static_cast<double>(buffer.height()) / pageSize.height;
    const int x = std::clamp(static_cast<int>(pagePoint.x * scaleX), 0, buffer.width() - 1);
    const int y = std::clamp(static_cast<int>((pageSize.height - pagePoint.y) * scaleY), 0,
                             buffer.height() - 1);
    // BGRA 預乘。stride 一律問 buffer，不可假設是寬×4。
    const std::uint8_t* row = buffer.data() + buffer.stride() * static_cast<std::size_t>(y);
    const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
    return Pixel{pixel[0], pixel[1], pixel[2]};
}

}  // namespace alioth::test::pageops
