#pragma once

// 頁首頁尾／頁碼／浮水印／Bates 的版面計算（PRD-PAGE-004）。
//
// 這一檔只算「文字放哪裡、字串長什麼樣」，不碰 QPainter 也不碰 PDF。
// 分開的理由與 Bates 相同：位置與內容是可以逐格驗證的規格，
// 而繪製是無法在單元測試裡斷言的部分，兩者混在一起會讓前者失去可測性。
//
// 座標系一律是「紙張座標，單位為點（1/72 吋），原點左上、Y 軸向下」。
// 不用 PDF 的左下原點是因為這一層的輸出最終交給 QPainter，
// 而在兩個座標系之間反覆換算正是座標錯誤的來源。

#include <QColor>
#include <QDateTime>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <cstdint>
#include <vector>

#include "app/print/bates_numbering.h"

namespace alioth::app::print {

// 九宮格定位。PDF-XChange 與 Acrobat 的頁首頁尾對話框都是這個模型，
// 使用者的既有預期就是九個格子加四個邊距。
enum class StampAnchor : std::uint8_t {
    TopLeft,
    TopCenter,
    TopRight,
    MiddleLeft,
    Center,
    MiddleRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

struct StampMargins {
    double left{36.0};
    double top{36.0};
    double right{36.0};
    double bottom{36.0};
};

struct StampText {
    QString text;  // 可含符號，見 expandStampTokens
    StampAnchor anchor{StampAnchor::BottomRight};
    double fontPointSize{10.0};
    QString fontFamily;  // 空字串表示交給 Qt 選預設字型
    bool bold{false};
    QColor color{0, 0, 0};
    double opacity{1.0};
    double rotationDegrees{0.0};
};

struct WatermarkOptions {
    bool enabled{false};
    QString text;
    QString fontFamily;
    QColor color{128, 128, 128};
    double opacity{0.25};
    double rotationDegrees{-45.0};
    // 浮水印文字寬度佔可繪製區寬度的比例。給比例而不是字級，
    // 是因為同一組設定要能同時套在 A4 與 A0 上而看起來一致。
    double widthFraction{0.7};
};

struct StampOptions {
    StampMargins marginsPt{};
    std::vector<StampText> headerFooter;  // 頁首、頁尾、頁碼共用同一個機制
    WatermarkOptions watermark{};
    BatesOptions bates{};
    StampText batesStyle{QString{}, StampAnchor::BottomRight, 10.0, QString{}, false,
                         QColor{0, 0, 0}, 1.0, 0.0};
};

// 符號替換的上下文。
struct StampContext {
    int pageNumber{1};      // 1-based 文件頁碼
    int pageCount{1};
    int printSequence{1};   // 1-based，該頁在本次列印中的次序
    int sheetNumber{1};     // 1-based，含海報分割後的紙張序號
    QString batesText;
    QString fileName;
    QDateTime timestamp;
};

// 符號替換。支援 <<Page>>、<<Pages>>、<<Sequence>>、<<Sheet>>、<<Bates>>、
// <<Date>>、<<Time>>、<<FileName>>。
//
// 用雙角括號而不是 % 或 $：頁首頁尾的內容常常是檔案路徑或法務案號，
// 那些字串裡出現 % 與 $ 的機率遠高於出現 <<。
[[nodiscard]] QString expandStampTokens(const QString& templ, const StampContext& context);

// 可繪製區域 = 紙張可列印區扣掉邊距。邊距大到把區域吃光時回傳空矩形，
// 呼叫端據此跳過繪製而不是畫出翻轉的矩形。
[[nodiscard]] QRectF stampContentBox(const QRectF& printableRectPt, const StampMargins& margins);

// 把一個尺寸為 itemSize 的方塊放進 box 的指定格子。
[[nodiscard]] QRectF placeStamp(const QRectF& box, const QSizeF& itemSize, StampAnchor anchor);

}  // namespace alioth::app::print
