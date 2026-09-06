#pragma once

// 把頁首頁尾／頁碼／浮水印／Bates 畫到列印畫布上（PRD-PAGE-004 的列印側）。
//
// 與 stamp_layout.h 的分工：那邊算位置與字串，這邊只負責把算好的結果交給
// QPainter。所有可斷言的邏輯都留在 layout 那一側，這裡不做任何決策。
//
// 這條路徑是「印出來時帶戳記」，不會改動文件本身。要把 Bates 永久寫進 PDF
// 是另一條需要內容串流與增量儲存的路徑，見 stamp_content_stream.h。

#include <QPainter>
#include <QRectF>

#include "app/print/stamp_layout.h"

namespace alioth::app::print {

// printableRectPt 以點為單位；painter 的座標系是印表機的裝置像素，
// 兩者以 deviceScale（= dpi / 72）換算。
//
// 字級刻意用 setPixelSize(點數 × deviceScale) 而不是 setPointSizeF：
// 後者的結果取決於畫布回報的邏輯 DPI，同一份設定在不同印表機上會印出
// 不同大小的頁碼，而頁碼大小在對照紙本時是會被注意到的。
void paintStamps(QPainter& painter, const QRectF& printableRectPt, const StampOptions& options,
                 const StampContext& context, double deviceScale);

// 單一戳記。公開出來讓測試可以只驗一個元素，也讓呈現層的預覽可以重用。
void paintStampText(QPainter& painter, const QRectF& contentBoxPt, const StampText& style,
                    const QString& resolvedText, double deviceScale);

void paintWatermark(QPainter& painter, const QRectF& printableRectPt,
                    const WatermarkOptions& options, const StampContext& context,
                    double deviceScale);

}  // namespace alioth::app::print
