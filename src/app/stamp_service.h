#pragma once

// 把 Bates 編號、頁首頁尾、浮水印**寫進文件**（PRD-PAGE-004）。
//
// 列印路徑（app/print）已經能把這些戳記畫在紙上，但 PRD-PAGE-004 的完整語意包含
// 「永久套用到 PDF」——法務流程需要的是拿到檔案的人也看得到編號，而不是只有
// 印出來那一份有。
//
// 這一層把兩個既有元件接起來：print 的戳記內容串流產生器負責「畫什麼」，
// objects 的內容附加器負責「怎麼寫進檔案」。兩者的介面本來就對齊
// （fontResourceName ↔ ContentFontRequest::resourceName），這裡不做轉換只做編排。
//
// 寫入走純附加，所以既有簽章仍然只會顯示「簽署後有變更」而不是「無效」。

#include <QObject>
#include <QString>

#include <cstdint>
#include <vector>

#include "app/print/bates_numbering.h"
#include "app/print/stamp_layout.h"

namespace alioth::app {

struct StampRequest {
    QString path;

    // 要蓋章的頁面（0 起算）。留空代表全部頁面。
    std::vector<std::int32_t> pages;

    // 每頁的文字。支援 print::stamp_layout 的符號（<<Page>>、<<Bates>> 等）。
    QString textTemplate;

    print::StampAnchor anchor{print::StampAnchor::BottomRight};
    print::StampMargins margins{};
    double fontSize{10.0};

    // 非空時每頁遞增，並讓 <<Bates>> 可用。
    print::BatesOptions bates{};
    bool useBates{false};
};

struct StampResult {
    bool ok{false};
    QString message;
    int stampedPages{0};

    // 復原用：純附加寫入，截回原長度即可。
    quint64 previousSize{0};
    QByteArray boundaryGuard;

    // 實際寫進去的 Bates 序列，供法務流程存證。
    std::vector<QString> batesNumbers;
};

class StampService : public QObject {
    Q_OBJECT

public:
    explicit StampService(QObject* parent = nullptr);

    [[nodiscard]] StampResult applyStamps(const StampRequest& request);

    // 移除本程式加過的所有頁面標記（PDF-XChange 的 Watermarks / Header and
    // Footer / Bates Numbering → Remove All）。
    //
    // 只移除我們自己加的——判定依據是寫入時放進串流字典的私有標記鍵。
    // 別的工具加的浮水印移不掉，那是正確的行為：移除它需要剖析內容串流
    // 並猜測哪一段是浮水印，而猜錯會刪掉使用者原本的內容。
    //
    // 與寫入一樣是純附加（只把 /Contents 裡的參照摘掉，不刪物件），
    // 所以復原＝截回原長度，既有簽章維持「有效，簽署後有變更」。
    [[nodiscard]] StampResult removeStamps(const QString& path);
};

}  // namespace alioth::app
